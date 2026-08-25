#include "Game/MatterFluxPlayableWorldActor.h"

#include "Game/MatterFluxGroundStateChunkActor.h"

#include "IMatterFluxScriptRuntime.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxFragmentSourceProxyComponent.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxTerrainMesh.h"
#include "Game/MatterFluxTwoStoreyHouseActor.h"
#include "Game/MatterFluxWorldStreamingPlan.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "MatterFluxLog.h"
#include "Material/MatterFluxLiquidBuoyancy.h"
#include "Material/MatterFluxMaterialContactGeometry.h"
#include "Material/MatterFluxMaterialReactionEngine.h"
#include "Material/MatterFluxReaction.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProceduralMeshComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rendering/MatterFluxInstanceVisuals.h"
#include "Rendering/MatterFluxItemOcclusion.h"
#include "Rendering/MatterFluxLiquidSurfaceProjection.h"
#include "Stats/Stats.h"
#include "Tasks/Task.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/Package.h"

#include <atomic>

struct FMatterFluxPreparedTerrainChunk
{
	FIntPoint Coordinate = FIntPoint::ZeroValue;
	MatterFlux::TerrainMesh::FChunk Chunk;
	bool bSuccess = false;
};

struct FMatterFluxAsyncTerrainBuildState
{
	std::atomic<bool> bCancelled{false};
	TQueue<FMatterFluxPreparedTerrainChunk, EQueueMode::Mpsc> CompletedChunks;
};

struct FMatterFluxPreparedPopulationChunk
{
	FIntPoint Coordinate = FIntPoint::ZeroValue;
	MatterFlux::PlayableLevel::FStreamingChunkPopulation Population;
	bool bSuccess = false;
};

struct FMatterFluxAsyncPopulationBuildState
{
	std::atomic<bool> bCancelled{false};
	TQueue<FMatterFluxPreparedPopulationChunk, EQueueMode::Mpsc>
		CompletedChunks;
};

DECLARE_STATS_GROUP(
	TEXT("MatterFlux"),
	STATGROUP_MatterFlux,
	STATCAT_Advanced);
DECLARE_CYCLE_STAT(
	TEXT("Playable World Tick"),
	STAT_MatterFluxWorldTick,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("World Rebuild"),
	STAT_MatterFluxWorldRebuild,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Terrain Streaming"),
	STAT_MatterFluxTerrainStreaming,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Decoration Streaming"),
	STAT_MatterFluxDecorationStreaming,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Reaction"),
	STAT_MatterFluxReaction,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Ground Reaction Simulation"),
	STAT_MatterFluxGroundReactionSimulation,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Ground Reaction Replication"),
	STAT_MatterFluxGroundReactionReplication,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Source Reaction Replication"),
	STAT_MatterFluxSourceReactionReplication,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Ground Reaction Visuals"),
	STAT_MatterFluxGroundReactionVisuals,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Reaction Propagation"),
	STAT_MatterFluxReactionPropagation,
	STATGROUP_MatterFlux);

namespace
{
	// Body displacement is a local pressure boundary, not a request to rescan
	// the whole visible lake for every occupied cell. Sixteen material cells
	// provide substantially more free-column capacity than the largest current
	// character/creature footprint while keeping work inside a compact dirty
	// neighborhood. The conserved wake pass reconnects that local surplus later.
	constexpr int32 BodyLiquidDisplacementSearchRadiusCells = 16;

	bool SweepFixedFragmentSourceCandidates(
		const TArray<AFragment2DSourceActor*>& Sources,
		const FVector& Start,
		const FVector& End,
		const float Radius,
		FVector& OutImpactLocation,
		FVector& OutImpactNormal,
		AFragment2DSourceActor*& OutSource)
	{
		OutImpactLocation = End;
		OutImpactNormal = FVector::ZeroVector;
		OutSource = nullptr;
		float BestTime = TNumericLimits<float>::Max();
		for (AFragment2DSourceActor* Source : Sources)
		{
			if (!IsValid(Source) || Source->bBroken)
			{
				continue;
			}
			const FBox ActiveBounds = Source->GetActiveWorldBounds();
			const FBox SourceBounds = ActiveBounds.IsValid
				? ActiveBounds
				: Source->GetCanonicalWorldBounds();
			FVector BoundsHitLocation;
			FVector BoundsHitNormal;
			float BoundsHitTime = 0.0f;
			if (!SourceBounds.IsValid
				|| !FMath::LineExtentBoxIntersection(
					SourceBounds,
					Start,
					End,
					FVector(FMath::Max(Radius, 0.0f)),
					BoundsHitLocation,
					BoundsHitNormal,
					BoundsHitTime))
			{
				continue;
			}
			FVector HitLocation;
			FVector HitNormal;
			if (!Source->SweepRuntimeMask(
				Start,
				End,
				FMath::Max(Radius, 0.0f),
				HitLocation,
				HitNormal))
			{
				continue;
			}
			const FVector Sweep = End - Start;
			const float HitTime = Sweep.SizeSquared() > UE_SMALL_NUMBER
				? FMath::Clamp(
					FVector::DotProduct(HitLocation - Start, Sweep)
						/ Sweep.SizeSquared(),
					0.0f,
					1.0f)
				: 0.0f;
			if (HitTime >= BestTime)
			{
				continue;
			}
			BestTime = HitTime;
			OutImpactLocation = HitLocation;
			OutImpactNormal = HitNormal;
			OutSource = Source;
		}
		return IsValid(OutSource);
	}

	FIntPoint ToMaterialRenderChunk(
		const FIntPoint Cell,
		const int32 ChunkSize)
	{
		return FIntPoint(
			FMath::FloorToInt(static_cast<double>(Cell.X) / ChunkSize),
			FMath::FloorToInt(static_cast<double>(Cell.Y) / ChunkSize));
	}

	FName MakeLiquidProjectionComponentKey(
		const FName MaterialId,
		const FIntPoint Chunk)
	{
		return FName(*FString::Printf(
			TEXT("Liquid_%s_Chunk_%d_%d"),
			*MaterialId.ToString(),
			Chunk.X,
			Chunk.Y));
	}
}

bool FMatterFluxReplicatedFragmentSourceStateList::RebuildAuthorityCache()
{
	AuthorityIndexBySourceId.Reset();
	AuthorityIndexBySourceId.Reserve(Items.Num());
	int64 PayloadByteCount = 0;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const FMatterFluxReplicatedFragmentSourceState& Item = Items[Index];
		if (!Item.SourceId.IsValid()
			|| Item.Revision < 0
			|| AuthorityIndexBySourceId.Contains(Item.SourceId))
		{
			AuthorityIndexBySourceId.Reset();
			AuthorityPayloadByteCount = 0;
			AuthorityCachedItemCount = -1;
			return false;
		}
		AuthorityIndexBySourceId.Add(Item.SourceId, Index);
		PayloadByteCount += Item.PackedRuntimeMask.Num()
			+ Item.PackedOutputMask.Num()
			+ Item.PackedActiveMask.Num();
		if (PayloadByteCount > MAX_int32)
		{
			AuthorityIndexBySourceId.Reset();
			AuthorityPayloadByteCount = 0;
			AuthorityCachedItemCount = -1;
			return false;
		}
	}
	AuthorityPayloadByteCount = static_cast<int32>(PayloadByteCount);
	AuthorityCachedItemCount = Items.Num();
	return true;
}

EMatterFluxFragmentSourceStateUpsertResult
FMatterFluxReplicatedFragmentSourceStateList::UpsertAuthorityBatch(
	const TConstArrayView<FMatterFluxFragmentSourceStateBatchUpdate> Updates,
	const int32 MaximumItemCount,
	const int32 MaximumPayloadBytes)
{
	if (MaximumItemCount <= 0
		|| MaximumPayloadBytes < 0
		|| (AuthorityCachedItemCount != Items.Num()
			&& !RebuildAuthorityCache()))
	{
		return EMatterFluxFragmentSourceStateUpsertResult::InvalidState;
	}
	if (Updates.IsEmpty())
	{
		return EMatterFluxFragmentSourceStateUpsertResult::Committed;
	}

	TArray<int32, TInlineAllocator<64>> StableOrder;
	StableOrder.Reserve(Updates.Num());
	for (int32 Index = 0; Index < Updates.Num(); ++Index)
	{
		StableOrder.Add(Index);
	}
	StableOrder.Sort(
		[&Updates](const int32 LeftIndex, const int32 RightIndex)
		{
			const FGuid& Left = Updates[LeftIndex].SourceId;
			const FGuid& Right = Updates[RightIndex].SourceId;
			return Left.A != Right.A ? Left.A < Right.A
				: Left.B != Right.B ? Left.B < Right.B
				: Left.C != Right.C ? Left.C < Right.C
				: Left.D < Right.D;
		});

	auto IsBinaryMask = [](const TArray<uint8>& Mask)
	{
		return !Mask.IsEmpty()
			&& !Mask.ContainsByPredicate(
				[](const uint8 Value)
				{
					return Value > 1;
				});
	};
	auto PayloadByteCount = [](const FFragment2DSourceStreamingState& State)
	{
		const int64 PackedMaskBytes =
			FMath::DivideAndRoundUp(State.GetRuntimeMask().Num(), 8);
		return State.bHasReactionState
			? PackedMaskBytes * 3
			: PackedMaskBytes;
	};
	auto StoredPayloadByteCount = [](
		const FMatterFluxReplicatedFragmentSourceState& Item)
	{
		return static_cast<int64>(Item.PackedRuntimeMask.Num())
			+ Item.PackedOutputMask.Num()
			+ Item.PackedActiveMask.Num();
	};

	const FGuid* PreviousSourceId = nullptr;
	for (const int32 UpdateIndex : StableOrder)
	{
		const FMatterFluxFragmentSourceStateBatchUpdate& Update =
			Updates[UpdateIndex];
		const FFragment2DSourceStreamingState* State = Update.State;
		if (!Update.SourceId.IsValid()
			|| !State
			|| State->Revision < 0
			|| (PreviousSourceId && *PreviousSourceId == Update.SourceId)
			|| !IsBinaryMask(State->GetRuntimeMask())
			|| (State->bHasReactionState
				&& (!IsBinaryMask(State->ReactionState.OutputMask)
					|| State->ReactionState.OutputMask.Num()
						!= State->GetRuntimeMask().Num()
					|| State->ReactionState.ActiveMask.Num()
						!= State->GetRuntimeMask().Num()
					|| !FMath::IsFinite(State->ReactionAccumulator)
					|| State->ReactionAccumulator < 0.0f
					|| State->TotalMaterialEmissionCount < 0)))
		{
			return EMatterFluxFragmentSourceStateUpsertResult::InvalidState;
		}
		PreviousSourceId = &Update.SourceId;
	}

	int32 AddedItemCount = 0;
	int64 ProspectivePayloadByteCount = AuthorityPayloadByteCount;
	for (const int32 UpdateIndex : StableOrder)
	{
		const FMatterFluxFragmentSourceStateBatchUpdate& Update =
			Updates[UpdateIndex];
		const FFragment2DSourceStreamingState& State = *Update.State;
		const int32* ExistingIndex =
			AuthorityIndexBySourceId.Find(Update.SourceId);
		if (ExistingIndex)
		{
			ProspectivePayloadByteCount -=
				StoredPayloadByteCount(Items[*ExistingIndex]);
		}
		else
		{
			++AddedItemCount;
			if (Items.Num() + AddedItemCount > MaximumItemCount)
			{
				return EMatterFluxFragmentSourceStateUpsertResult::
					ItemBudgetExceeded;
			}
		}
		ProspectivePayloadByteCount += PayloadByteCount(State);
		if (ProspectivePayloadByteCount > MaximumPayloadBytes
			|| ProspectivePayloadByteCount > MAX_int32)
		{
			return EMatterFluxFragmentSourceStateUpsertResult::
				ByteBudgetExceeded;
		}
	}
	auto PackPresenceMask = [](
		const TArray<uint8>& Mask,
		TArray<uint8>& OutPacked)
	{
		OutPacked.Reset();
		OutPacked.Init(0, FMath::DivideAndRoundUp(Mask.Num(), 8));
		for (int32 Index = 0; Index < Mask.Num(); ++Index)
		{
			OutPacked[Index >> 3] |=
				static_cast<uint8>(Mask[Index] != 0) << (Index & 7);
		}
	};
	for (const int32 UpdateIndex : StableOrder)
	{
		const FMatterFluxFragmentSourceStateBatchUpdate& Update =
			Updates[UpdateIndex];
		const FFragment2DSourceStreamingState& State = *Update.State;
		int32* ExistingIndex = AuthorityIndexBySourceId.Find(Update.SourceId);
		int32 ItemIndex = INDEX_NONE;
		if (ExistingIndex)
		{
			ItemIndex = *ExistingIndex;
		}
		else
		{
			ItemIndex = Items.AddDefaulted();
			AuthorityIndexBySourceId.Add(Update.SourceId, ItemIndex);
		}
		FMatterFluxReplicatedFragmentSourceState& Item = Items[ItemIndex];
		Item.SourceId = Update.SourceId;
		Item.Revision = State.Revision;
		PackPresenceMask(State.GetRuntimeMask(), Item.PackedRuntimeMask);
		Item.bHasReactionState = State.bHasReactionState;
		Item.ReactionRuleId = State.bHasReactionState
			? State.ReactionState.RuleId
			: NAME_None;
		Item.ReactionSeed = State.bHasReactionState
			? State.ReactionState.Seed
			: 0;
		Item.ReactionTick = State.bHasReactionState
			? State.ReactionState.Tick
			: 0;
		if (State.bHasReactionState)
		{
			PackPresenceMask(
				State.ReactionState.OutputMask,
				Item.PackedOutputMask);
			PackPresenceMask(
				State.ReactionState.ActiveMask,
				Item.PackedActiveMask);
			Item.ReactionAccumulator = State.ReactionAccumulator;
			Item.TotalMaterialEmissionCount = State.TotalMaterialEmissionCount;
		}
		else
		{
			Item.PackedOutputMask.Reset();
			Item.PackedActiveMask.Reset();
			Item.ReactionAccumulator = 0.0f;
			Item.TotalMaterialEmissionCount = 0;
		}
		MarkItemDirty(Item);
	}
	AuthorityPayloadByteCount =
		static_cast<int32>(ProspectivePayloadByteCount);
	AuthorityCachedItemCount = Items.Num();
	return EMatterFluxFragmentSourceStateUpsertResult::Committed;
}

int32 FMatterFluxReplicatedFragmentSourceStateList::
	GetAuthorityPayloadByteCount()
{
	return AuthorityCachedItemCount == Items.Num() || RebuildAuthorityCache()
		? AuthorityPayloadByteCount
		: INDEX_NONE;
}

void FMatterFluxReplicatedFragmentSourceStateList::ResetAuthorityItems()
{
	Items.Reset();
	AuthorityIndexBySourceId.Reset();
	AuthorityPayloadByteCount = 0;
	AuthorityCachedItemCount = 0;
	MarkArrayDirty();
}

void FMatterFluxReplicatedFragmentSourceStateList::
	RequestClientFullRebuild()
{
	bClientFullRebuildRequired = true;
	PendingClientUpsertReplicationIds.Reset();
	PendingClientRemovedSourceIds.Reset();
}

void FMatterFluxReplicatedFragmentSourceStateList::
	ConsumeClientApplyPlan(
		FMatterFluxFragmentSourceClientApplyPlan& OutPlan)
{
	OutPlan = FMatterFluxFragmentSourceClientApplyPlan();
	if (bClientFullRebuildRequired)
	{
		OutPlan.bFullRebuild = true;
		bClientFullRebuildRequired = false;
		PendingClientUpsertReplicationIds.Reset();
		PendingClientRemovedSourceIds.Reset();
		return;
	}

	bool bResolutionFailed = false;
	TArray<int32> ResolvedIndices;
	ResolvedIndices.Reserve(PendingClientUpsertReplicationIds.Num());
	for (const int32 ReplicationId : PendingClientUpsertReplicationIds)
	{
		const int32* ItemIndex = ItemMap.Find(ReplicationId);
		if (!ItemIndex
			|| !Items.IsValidIndex(*ItemIndex)
			|| Items[*ItemIndex].ReplicationID != ReplicationId
			|| !Items[*ItemIndex].SourceId.IsValid())
		{
			bResolutionFailed = true;
			break;
		}
		ResolvedIndices.Add(*ItemIndex);
	}

	auto SourceIdLess = [this](const int32 LeftIndex, const int32 RightIndex)
	{
		const FGuid& Left = Items[LeftIndex].SourceId;
		const FGuid& Right = Items[RightIndex].SourceId;
		return Left.A != Right.A ? Left.A < Right.A
			: Left.B != Right.B ? Left.B < Right.B
			: Left.C != Right.C ? Left.C < Right.C
			: Left.D < Right.D;
	};
	ResolvedIndices.StableSort(SourceIdLess);
	FGuid PreviousSourceId;
	bool bHasPreviousSourceId = false;
	for (const int32 ItemIndex : ResolvedIndices)
	{
		const FGuid& SourceId = Items[ItemIndex].SourceId;
		if (!bHasPreviousSourceId || SourceId != PreviousSourceId)
		{
			OutPlan.UpsertItemIndices.Add(ItemIndex);
			PreviousSourceId = SourceId;
			bHasPreviousSourceId = true;
		}
	}

	for (const FGuid& SourceId : PendingClientRemovedSourceIds)
	{
		if (!SourceId.IsValid())
		{
			bResolutionFailed = true;
			break;
		}
		OutPlan.RemovedSourceIds.Add(SourceId);
	}
	auto GuidLess = [](const FGuid& Left, const FGuid& Right)
	{
		return Left.A != Right.A ? Left.A < Right.A
			: Left.B != Right.B ? Left.B < Right.B
			: Left.C != Right.C ? Left.C < Right.C
			: Left.D < Right.D;
	};
	OutPlan.RemovedSourceIds.StableSort(GuidLess);
	for (int32 Index = OutPlan.RemovedSourceIds.Num() - 1; Index > 0; --Index)
	{
		if (OutPlan.RemovedSourceIds[Index]
			== OutPlan.RemovedSourceIds[Index - 1])
		{
			OutPlan.RemovedSourceIds.RemoveAt(Index, 1, EAllowShrinking::No);
		}
	}
	if (!OutPlan.UpsertItemIndices.IsEmpty()
		&& !OutPlan.RemovedSourceIds.IsEmpty())
	{
		TSet<FGuid> UpsertSourceIds;
		UpsertSourceIds.Reserve(OutPlan.UpsertItemIndices.Num());
		for (const int32 ItemIndex : OutPlan.UpsertItemIndices)
		{
			UpsertSourceIds.Add(Items[ItemIndex].SourceId);
		}
		OutPlan.RemovedSourceIds.RemoveAll(
			[&UpsertSourceIds](const FGuid& SourceId)
			{
				return UpsertSourceIds.Contains(SourceId);
			});
	}

	PendingClientUpsertReplicationIds.Reset();
	PendingClientRemovedSourceIds.Reset();
	if (bResolutionFailed)
	{
		OutPlan = FMatterFluxFragmentSourceClientApplyPlan();
		OutPlan.bFullRebuild = true;
	}
}

void FMatterFluxReplicatedFragmentSourceStateList::PreReplicatedRemove(
	const TArrayView<int32>& RemovedIndices,
	const int32 FinalSize)
{
	for (const int32 ItemIndex : RemovedIndices)
	{
		if (!Items.IsValidIndex(ItemIndex)
			|| !Items[ItemIndex].SourceId.IsValid())
		{
			bClientFullRebuildRequired = true;
			continue;
		}
		PendingClientRemovedSourceIds.Add(Items[ItemIndex].SourceId);
	}
}

void FMatterFluxReplicatedFragmentSourceStateList::PostReplicatedAdd(
	const TArrayView<int32>& AddedIndices,
	const int32 FinalSize)
{
	for (const int32 ItemIndex : AddedIndices)
	{
		if (!Items.IsValidIndex(ItemIndex)
			|| Items[ItemIndex].ReplicationID == INDEX_NONE)
		{
			bClientFullRebuildRequired = true;
			continue;
		}
		PendingClientUpsertReplicationIds.Add(
			Items[ItemIndex].ReplicationID);
	}
}

void FMatterFluxReplicatedFragmentSourceStateList::PostReplicatedChange(
	const TArrayView<int32>& ChangedIndices,
	const int32 FinalSize)
{
	for (const int32 ItemIndex : ChangedIndices)
	{
		if (!Items.IsValidIndex(ItemIndex)
			|| Items[ItemIndex].ReplicationID == INDEX_NONE)
		{
			bClientFullRebuildRequired = true;
			continue;
		}
		PendingClientUpsertReplicationIds.Add(
			Items[ItemIndex].ReplicationID);
	}
}

void FMatterFluxReplicatedFragmentSourceStateList::PostReplicatedReceive(
	const FFastArraySerializer::FPostReplicatedReceiveParameters& Parameters)
{
	if (Owner.IsValid())
	{
		Owner->MarkReplicatedFragmentSourceStatesDirty();
	}
}

namespace
{
	constexpr int32 MaxMaterialSimulationStepsPerFrame = 4;
	constexpr int32 InitialMaterialWarmupStepsPerBatch =
		MaxMaterialSimulationStepsPerFrame;
	constexpr int32 InitialMaterialWarmupMinimumSteps = 30;
	constexpr int32 InitialMaterialWarmupQuietSteps = 8;
	constexpr int32 InitialMaterialWarmupMaximumSteps = 120;
	constexpr int32 MaxAlwaysLoadedRenderOnlyLayerInstances = 4096;
	constexpr int32 MaxStreamingWindowChunks = 65536;
	constexpr int32 MaxReplicatedFragmentSourceStates = 4096;

	int32 GetPowderMaximumStableSlopeAmount(const float CellSize)
	{
		return FMath::Clamp(
			FMath::RoundToInt(
				255.0f * FMath::Tan(FMath::DegreesToRadians(34.0f))
					* CellSize
					/ MatterFlux::Material::SurfacePowderFullColumnHeight),
			1,
			255);
	}
	constexpr int32 MaxReplicatedFragmentSourceStateBytes = 1024 * 1024;
	void ApplyLiquidOptics(
		UMaterialInstanceDynamic& DynamicMaterial,
		const FMatterFluxMaterialDefinition& Material)
	{
		// The material simulation supplies volume; this adapter draws only the
		// outside free surface. Depth-fading against the stepped terrain below
		// exposes every basin voxel as a false ring/crack in that surface, so the
		// Scene-depth fading reads stepped terrain as concentric water bands. The
		// projection supplies canonical column depth through vertex alpha instead,
		// so these remain the configured endpoints of a particle-derived range.
		const float ShallowOpacity = FMath::Clamp(
			Material.ShallowOpacity, 0.0f, 0.98f);
		const float DeepOpacity = FMath::Clamp(
			Material.DeepOpacity, ShallowOpacity, 0.98f);
		DynamicMaterial.SetVectorParameterValue(TEXT("Color"), Material.Color);
		DynamicMaterial.SetScalarParameterValue(
			TEXT("ShallowOpacity"),
			ShallowOpacity);
		DynamicMaterial.SetScalarParameterValue(
			TEXT("DeepOpacity"),
			DeepOpacity);
	}

	bool UnpackFragmentSourceMask(
		const TArray<uint8>& Packed,
		const int32 CellCount,
		TArray<uint8>& OutMask)
	{
		OutMask.Reset();
		if (CellCount <= 0
			|| Packed.Num() != FMath::DivideAndRoundUp(CellCount, 8))
		{
			return false;
		}
		const int32 UsedBits = CellCount & 7;
		if (UsedBits != 0 && (Packed.Last() >> UsedBits) != 0)
		{
			return false;
		}
		OutMask.SetNumUninitialized(CellCount);
		for (int32 Index = 0; Index < CellCount; ++Index)
		{
			OutMask[Index] =
				(Packed[Index >> 3] >> (Index & 7)) & 1u;
		}
		return true;
	}

	constexpr int32 GroundReactionChunkSize = 64;

	MatterFlux::Reaction::FGroundRuntimeSettings
	MakeGroundReactionRuntimeSettings()
	{
		MatterFlux::Reaction::FGroundRuntimeSettings Settings;
		Settings.Width = MatterFlux::PlayableLevel::TerrainCellsX;
		Settings.Height = MatterFlux::PlayableLevel::TerrainCellsY;
		Settings.ChunkSize = GroundReactionChunkSize;
		Settings.StepSeconds = 0.1f;
		Settings.MaxStepsPerAdvance = 3;
		return Settings;
	}

	struct FMaterialVisualKey
	{
		FName MaterialId = NAME_None;
		FIntPoint Chunk = FIntPoint::ZeroValue;

		bool operator==(const FMaterialVisualKey& Other) const
		{
			return MaterialId == Other.MaterialId && Chunk == Other.Chunk;
		}

		friend uint32 GetTypeHash(const FMaterialVisualKey& Key)
		{
			return HashCombineFast(
				GetTypeHash(Key.MaterialId),
				GetTypeHash(Key.Chunk));
		}
	};

	FName MakeMaterialVisualComponentKey(
		const FMaterialVisualKey& Key)
	{
		return FName(*FString::Printf(
			TEXT("%s|%d|%d"),
			*Key.MaterialId.ToString(),
			Key.Chunk.X,
			Key.Chunk.Y));
	}

	void MixMaterialVisualHash(uint64& Hash, const uint32 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ull;
	}

	uint64 BuildMaterialVisualHash(
		const TConstArrayView<MatterFlux::Material::FCellSnapshot> Cells,
		const EMatterFluxMaterialPhase Phase)
	{
		uint64 Hash = 1469598103934665603ull;
		MixMaterialVisualHash(Hash, static_cast<uint32>(Phase));
		MixMaterialVisualHash(Hash, static_cast<uint32>(Cells.Num()));
		for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
		{
			MixMaterialVisualHash(Hash, static_cast<uint32>(Cell.WorldCell.X));
			MixMaterialVisualHash(Hash, static_cast<uint32>(Cell.WorldCell.Y));
			MixMaterialVisualHash(Hash, static_cast<uint32>(Cell.SupportHeight));
			MixMaterialVisualHash(Hash, static_cast<uint32>(Cell.Amount));
		}
		return Hash;
	}

	FMatterFluxSavedReactionState ToSavedReactionState(
		const MatterFlux::Reaction::FStateSnapshot& State)
	{
		FMatterFluxSavedReactionState Saved;
		Saved.RuleId = State.RuleId;
		Saved.Width = State.Width;
		Saved.Height = State.Height;
		Saved.Seed = State.Seed;
		Saved.Tick = State.Tick;
		Saved.InputMask = State.InputMask;
		Saved.OutputMask = State.OutputMask;
		Saved.ActiveMask = State.ActiveMask;
		return Saved;
	}

	MatterFlux::Reaction::FStateSnapshot ToRuntimeReactionState(
		const FMatterFluxSavedReactionState& State)
	{
		MatterFlux::Reaction::FStateSnapshot Runtime;
		Runtime.RuleId = State.RuleId;
		Runtime.Width = State.Width;
		Runtime.Height = State.Height;
		Runtime.Seed = State.Seed;
		Runtime.Tick = State.Tick;
		Runtime.InputMask = State.InputMask;
		Runtime.OutputMask = State.OutputMask;
		Runtime.ActiveMask = State.ActiveMask;
		return Runtime;
	}

	FMatterFluxSavedFragmentSourceState ToSavedFragmentState(
		const FGuid SourceId,
		const FFragment2DSourceStreamingState& State)
	{
		FMatterFluxSavedFragmentSourceState Saved;
		Saved.SourceId = SourceId;
		Saved.Revision = State.Revision;
		Saved.RuntimeMask = State.GetRuntimeMask();
		Saved.bHasReactionState = State.bHasReactionState;
		if (State.bHasReactionState)
		{
			Saved.ReactionState = ToSavedReactionState(
				State.ReactionState);
		}
		Saved.ReactionAccumulator = State.ReactionAccumulator;
		Saved.TotalMaterialEmissionCount = State.TotalMaterialEmissionCount;
		return Saved;
	}

	FFragment2DSourceStreamingState ToRuntimeFragmentState(
		const FMatterFluxSavedFragmentSourceState& State)
	{
		FFragment2DSourceStreamingState Runtime;
		Runtime.Revision = State.Revision;
		Runtime.bHasReactionState = State.bHasReactionState;
		if (State.bHasReactionState)
		{
			Runtime.ReactionState = ToRuntimeReactionState(
				State.ReactionState);
		}
		Runtime.ReactionAccumulator = State.ReactionAccumulator;
		Runtime.TotalMaterialEmissionCount = State.TotalMaterialEmissionCount;
		Runtime.SetRuntimeMask(State.RuntimeMask);
		return Runtime;
	}

	void RetireTerrainChunkComponent(
		UProceduralMeshComponent* Component)
	{
		if (!IsValid(Component))
		{
			return;
		}
		Component->DestroyComponent();
		// DestroyComponent unregisters the component, but the UObject remains
		// under its actor until garbage collection. Move it away immediately so
		// a rebuild can reuse the exact deterministic chunk name on every net
		// world instead of acquiring a client-specific _1/_2 suffix.
		const FName RetiredName = MakeUniqueObjectName(
			GetTransientPackage(),
			UProceduralMeshComponent::StaticClass(),
			TEXT("RetiredMatterFluxTerrainChunk"));
		Component->Rename(
			*RetiredName.ToString(),
			GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional);
	}

	bool TryWorldLocationToCell(
		const FTransform& WorldTransform,
		const FVector& WorldLocation,
		const float CellSize,
		FIntPoint& OutCell)
	{
		if (!WorldTransform.IsValid()
			|| WorldLocation.ContainsNaN()
			|| !FMath::IsFinite(CellSize)
			|| CellSize <= 0.0f)
		{
			return false;
		}
		const FVector Local =
			WorldTransform.InverseTransformPosition(WorldLocation);
		// 世界格使用左下闭、右上开的 floor 约定。地形中心允许位于
		// 半格坐标；这里必须与初始化边界及 SeedSurface 完全一致。
		const double CellX =
			FMath::FloorToDouble(Local.X / CellSize);
		const double CellY =
			FMath::FloorToDouble(Local.Y / CellSize);
		if (!FMath::IsFinite(CellX)
			|| !FMath::IsFinite(CellY)
			|| CellX < MIN_int32
			|| CellX > MAX_int32
			|| CellY < MIN_int32
			|| CellY > MAX_int32)
		{
			return false;
		}
		OutCell = FIntPoint(
			static_cast<int32>(CellX),
			static_cast<int32>(CellY));
		return true;
	}

	FBox BuildFragmentSourceLocalBounds(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
	{
		if (!Source.Mask.IsValid() || !Source.Transform.IsValid())
		{
			return FBox(ForceInit);
		}
		const double RadialHalfExtent =
			static_cast<double>(Source.Mask.Width)
				* Source.Mask.CellSize * 0.5;
		const FVector HalfExtent(
			static_cast<double>(Source.Mask.Width)
				* Source.Mask.CellSize * 0.5,
			Source.Mask.GeometryStyle
				== EFragmentSourceGeometryStyle::RadialColumn
					? RadialHalfExtent
					: Source.Mask.CellSize * 0.5,
			static_cast<double>(Source.Mask.Height)
				* Source.Mask.CellSize * 0.5);
		return FBox(-HalfExtent, HalfExtent).TransformBy(
			Source.Transform.ToMatrixWithScale());
	}

	FBox BuildConeWorldBounds(
		const FVector& Start,
		const FVector& Direction,
		const float Range,
		const float StartRadius,
		const float EndRadius)
	{
		const FVector End = Start + Direction * Range;
		FBox Bounds(ForceInit);
		Bounds += Start - FVector(StartRadius);
		Bounds += Start + FVector(StartRadius);
		Bounds += End - FVector(EndRadius);
		Bounds += End + FVector(EndRadius);
		return Bounds;
	}

}


AMatterFluxPlayableWorldActor::AMatterFluxPlayableWorldActor()
{
	ReplicatedFragmentSourceStates.Owner = this;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10.0f);
	SetMinNetUpdateFrequency(2.0f);
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.0f;
	verify(SmokeVisualPool.Configure(
		MatterFlux::Rendering::FSmokeVisualSettings()));

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	// The playable-world actor itself never moves. A static root is required
	// for streamed static terrain chunks to attach successfully; movable
	// visualization and light children remain valid beneath a static parent.
	SceneRoot->SetMobility(EComponentMobility::Static);
	SetRootComponent(SceneRoot);
	FragmentSourceProxy =
		CreateDefaultSubobject<UMatterFluxFragmentSourceProxyComponent>(
			TEXT("FragmentSourceProxy"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneFinder(
		TEXT("/Engine/BasicShapes/Plane.Plane"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(
		TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ColorMaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial_Inst.BasicShapeMaterial_Inst"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ColorMaterialFallbackFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> VoxelColorMaterialFinder(
		TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> VoxelLeafMaterialFinder(
		TEXT("/Game/MatterFlux/Materials/M_VoxelLeaf.M_VoxelLeaf"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> VoxelWoodMaterialFinder(
		TEXT("/Game/MatterFlux/Materials/M_VoxelWood.M_VoxelWood"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> VoxelGasMaterialFinder(
		TEXT("/Game/MatterFlux/Materials/M_VoxelGas.M_VoxelGas"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> VoxelLiquidMaterialFinder(
		TEXT("/Game/MatterFlux/Materials/M_VoxelLiquid.M_VoxelLiquid"));
	CubeMesh = CubeFinder.Object;
	LiquidSurfaceMesh = PlaneFinder.Succeeded()
		? PlaneFinder.Object.Get()
		: CubeMesh.Get();
	SphereMesh = SphereFinder.Object;
	CylinderMesh = CylinderFinder.Object;
	ConeMesh = ConeFinder.Object;
	ColorMaterialTemplate = VoxelColorMaterialFinder.Succeeded()
		? VoxelColorMaterialFinder.Object
		: (ColorMaterialFinder.Succeeded()
			? ColorMaterialFinder.Object
			: ColorMaterialFallbackFinder.Object);
	VoxelColorMaterialTemplate = VoxelColorMaterialFinder.Succeeded()
		? VoxelColorMaterialFinder.Object
		: ColorMaterialTemplate;
	VoxelLeafMaterialTemplate = VoxelLeafMaterialFinder.Succeeded()
		? VoxelLeafMaterialFinder.Object
		: VoxelColorMaterialTemplate;
	VoxelWoodMaterialTemplate = VoxelWoodMaterialFinder.Succeeded()
		? VoxelWoodMaterialFinder.Object
		: VoxelColorMaterialTemplate;
	VoxelGasMaterialTemplate = VoxelGasMaterialFinder.Succeeded()
		? VoxelGasMaterialFinder.Object
		: VoxelColorMaterialTemplate;
	VoxelLiquidMaterialTemplate = VoxelLiquidMaterialFinder.Succeeded()
		? VoxelLiquidMaterialFinder.Object
		: VoxelColorMaterialTemplate;
	FragmentSourceProxy->Configure(
		SceneRoot,
		VoxelColorMaterialTemplate,
		VoxelLeafMaterialTemplate,
		VoxelWoodMaterialTemplate);

	SunLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("SunLight"));
	SunLight->SetupAttachment(SceneRoot);
	SunLight->SetMobility(EComponentMobility::Movable);
	SunLight->SetRelativeRotation(FRotator(-40.0f, -35.0f, 0.0f));
	SunLight->Intensity = 2.25f;
	SunLight->LightColor = FColor(255, 235, 210);
	// A slightly tighter sun keeps the voxel silhouettes crisp while the sky
	// fill below prevents the unlit faces from collapsing to black.
	SunLight->LightSourceAngle = 0.85f;
	SunLight->bAtmosphereSunLight = true;

	SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
	SkyLight->SetupAttachment(SceneRoot);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->Intensity = 0.85f;
	SkyLight->SourceType = SLS_CapturedScene;
	// The sky and sun are static during play. Continuous cubemap capture adds
	// a render pass every frame and can introduce small exposure/lighting
	// changes along thin voxel silhouettes. Capture once after generation.
	SkyLight->bRealTimeCapture = false;
	SkyLight->bLowerHemisphereIsBlack = false;
	SkyLight->LowerHemisphereColor =
		FLinearColor(0.018f, 0.030f, 0.014f);

	SkyAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkyAtmosphere"));
	SkyAtmosphere->SetupAttachment(SceneRoot);
}

void AMatterFluxPlayableWorldActor::BeginPlay()
{
	Super::BeginPlay();
	ProceduralMaterialSimulationCellSize = MaterialSimulationCellSize;
	ReplicatedFragmentSourceStates.Owner = this;
	if (UFragmentSimulationSubsystem* FragmentSubsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr)
	{
		FragmentSourcePresenceHandle = FragmentSubsystem
			->OnSourcePresenceChanged()
			.AddUObject(
				this,
				&AMatterFluxPlayableWorldActor::
					HandleFragmentSourcePresenceChanged);
	}

	IMatterFluxScriptRuntime& ScriptRuntime = IMatterFluxScriptRuntime::Get();
	ContentReloadHandle = ScriptRuntime.OnContentReloaded().AddWeakLambda(
		this,
		[this](FMatterFluxContentRegistryPtr)
		{
			if (IsGenerationInProgress())
			{
				++GenerationRequestSerial;
				PendingGeneratedLayout.Reset();
				PendingGenerationRegistry.Reset();
				CompleteAsyncGeneration(
					false,
					TEXT("Lua 内容热更中断了地图生成"));
			}
			if (MapSeed != 0)
			{
				if (HasAuthority())
				{
					ReplicatedMaterialSimulationStep = 0;
				}
				if (ActiveCustomMapId.IsNone())
				{
					RebuildLevel();
				}
				else
				{
					FString Error;
					if (!RebuildActiveCustomMap(Error))
					{
						UE_LOG(LogMatterFlux, Error,
							TEXT("Custom map reload failed: %s"), *Error);
					}
				}
				ForceNetUpdate();
			}
		});

	if (HasAuthority() && MapSeed == 0)
	{
		MapSeed = FMath::RandRange(1, MAX_int32);
		ForceNetUpdate();
	}

	const bool bDeferredHostedSave = HasAuthority()
		&& GetWorld()
		&& GetWorld()->URL.HasOption(TEXT("MatterFluxHostSlot="));
	if (MapSeed != 0 && !bDeferredHostedSave)
	{
		if (ActiveCustomMapId.IsNone())
		{
			RebuildLevel();
		}
		else
		{
			FString Error;
			if (!RebuildActiveCustomMap(Error))
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Custom map startup failed: %s"), *Error);
			}
		}
	}
}

bool AMatterFluxPlayableWorldActor::SetInitialMapSeed(
	const int32 InitialSeed)
{
	if (HasActorBegunPlay()
		|| !HasAuthority()
		|| MapSeed != 0
		|| InitialSeed <= 0)
	{
		return false;
	}
	MapSeed = InitialSeed;
	return true;
}

void AMatterFluxPlayableWorldActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	++GenerationRequestSerial;
	if (AsyncTerrainBuildState.IsValid())
	{
		AsyncTerrainBuildState->bCancelled.store(
			true, std::memory_order_release);
	}
	if (AsyncPopulationBuildState.IsValid())
	{
		AsyncPopulationBuildState->bCancelled.store(
			true, std::memory_order_release);
	}
	PendingGeneratedLayout.Reset();
	PendingGenerationRegistry.Reset();
	if (IMatterFluxScriptRuntime::IsAvailable())
	{
		IMatterFluxScriptRuntime::Get()
			.OnContentReloaded()
			.Remove(ContentReloadHandle);
	}
	ContentReloadHandle.Reset();
	if (UFragmentSimulationSubsystem* FragmentSubsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr)
	{
		FragmentSubsystem->OnSourcePresenceChanged().Remove(
			FragmentSourcePresenceHandle);
	}
	FragmentSourcePresenceHandle.Reset();
	DestroyMaterialVisualization();
	DestroyCustomMapSceneBoxes();
	DestroyTerrainChunkMeshes();
	DestroyGeneratedHouse();
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->ResetSources();
	}
	PendingMaterialDisplacementCells.Reset();
	PreviousMaterialDisplacementCells.Reset();
	RecentMaterialChunkWakes.Reset();
	ExternalMaterialSupportCells.Reset();
	MaterialSimulation.Reset();
	if (HasAuthority())
	{
		DestroyGroundStateChunks();
		DestroyGeneratedFragmentSources();
	}
	Super::EndPlay(EndPlayReason);
}

void AMatterFluxPlayableWorldActor::
	HandleFragmentSourcePresenceChanged(
		const FGuid& SourceId,
		const bool bMaterialized)
{
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->SetSourceMaterialized(
			SourceId,
			bMaterialized
				|| RemovedFragmentSourceIds.Contains(SourceId));
	}
}

void AMatterFluxPlayableWorldActor::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMatterFluxPlayableWorldActor, MapSeed);
	DOREPLIFETIME(AMatterFluxPlayableWorldActor, ActiveCustomMapId);
	DOREPLIFETIME(
		AMatterFluxPlayableWorldActor,
		ReplicatedMaterialSimulationStep);
	DOREPLIFETIME(
		AMatterFluxPlayableWorldActor,
		ReplicatedMaterialSimulationFocus);
	DOREPLIFETIME(
		AMatterFluxPlayableWorldActor,
		ReplicatedMaterialState);
	DOREPLIFETIME(
		AMatterFluxPlayableWorldActor,
		ReplicatedFragmentSourceStates);
	DOREPLIFETIME(
		AMatterFluxPlayableWorldActor,
		ReplicatedTerrainHeightOverrides);
}

void AMatterFluxPlayableWorldActor::Tick(const float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxWorldTick);
	Super::Tick(DeltaSeconds);
	FMatterFluxContentRegistryPtr MaterialRegistry;
	// A projectile deposit records the Source face it geometrically hit. Resolve
	// that authored contact before any streaming/proxy flush can replace or
	// unregister the current Source projection, and before the liquid solver can
	// move the payload away from the wall. Continuing contacts are still handled
	// after fixed material steps below.
	if (HasAuthority()
		&& MaterialSimulation
		&& !PendingMaterialStimuli.IsEmpty()
		&& IMatterFluxScriptRuntime::IsAvailable())
	{
		MaterialRegistry =
			IMatterFluxScriptRuntime::Get().GetActiveRegistry();
		if (MaterialRegistry.IsValid())
		{
			ResolveMaterialInteractions(*MaterialRegistry);
		}
	}
	if (bReplicatedFragmentSourceStatesDirty)
	{
		bReplicatedFragmentSourceStatesDirty = false;
		ApplyReplicatedFragmentSourceStates();
	}
	if (FragmentSourceProxy)
	{
		ReactionProxyFlushAccumulator +=
			FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
		if (ReactionProxyFlushAccumulator >= 0.5f)
		{
			ReactionProxyFlushAccumulator = FMath::Fmod(
				ReactionProxyFlushAccumulator,
				0.5f);
			FragmentSourceProxy->FlushDeferredReactionChanges();
		}
		FragmentSourceProxy->FlushPendingChanges();
	}
	AdvanceAsyncGeneration();
	// Terrain/population commits go first. Once those immutable prerequisites are
	// ready, advance one exact, bounded batch per loading frame and commit its
	// disposable projection before the next batch. Unlike normal wall-clock
	// catch-up this cannot feed a slow frame back into the accumulator. Grouping
	// four logical steps also avoids rebuilding the same settling river projection
	// after every intermediate state.
	const bool bInitialWorldStreamingReady =
		IsInitialWorldStreamingReady();
	const bool bInitialWorldEntryPending = HasAuthority()
		&& MaterialSimulation
		&& !IsInitialWorldEntryReady();
	const bool bRunInitialMaterialWarmupStep =
		bInitialWorldEntryPending
		&& bInitialWorldStreamingReady
		&& !bInitialMaterialWarmupStepsComplete
		&& !bMaterialVisualizationDirty
		&& !bMaterialVisualizationDeferredForStreaming;
	const bool bReserveFrameForMaterialStep = HasAuthority()
		&& MaterialSimulation
		&& (bRunInitialMaterialWarmupStep
			|| (!bInitialWorldEntryPending
				&& MaterialSimulation->WillAdvanceStep(DeltaSeconds)));
	// Continuous movement can keep at least one terrain/population transaction
	// alive every frame. Waiting for a completely idle frame then starves liquid
	// projections indefinitely. Once the accumulated visual age reaches this
	// bound, reserve one non-simulation frame for presentation; async builders
	// still run, but their UObject commits wait until the following frame.
	constexpr float MaximumStreamingVisualizationDeferralSeconds = 0.25f;
	const bool bReserveFrameForMaterialVisualization =
		!bReserveFrameForMaterialStep
		&& bMaterialVisualizationDeferredForStreaming
		&& bMaterialVisualizationDirty
		&& MaterialVisualizationAccumulator
			>= MaximumStreamingVisualizationDeferralSeconds;
	const bool bCreatedTerrainChunk =
		ProcessPendingTerrainChunkPrefetches(
			!bReserveFrameForMaterialStep
				&& !bReserveFrameForMaterialVisualization);
	// Async task submission continues on reserved frames so predictive loading
	// does not lose throughput. Only UObject/physics commits wait for the next
	// non-simulation frame. Terrain mesh creation and surface seeding also keep
	// separate budgets to avoid stacking both full transactions.
	const bool bHadDeferredMaterialVisualization =
		bMaterialVisualizationDeferredForStreaming;
	bool bCommittedProceduralPopulationChunk = false;
	const bool bProcessedProceduralSurfaceSeed =
		ProcessPendingProceduralPopulationUpdates(
			!bReserveFrameForMaterialStep
				&& !bReserveFrameForMaterialVisualization,
			!bReserveFrameForMaterialStep
				&& !bReserveFrameForMaterialVisualization,
			!bCreatedTerrainChunk
				&& !bReserveFrameForMaterialVisualization,
			&bCommittedProceduralPopulationChunk);
	const bool bDidStreamingWork = bCreatedTerrainChunk
		|| bCommittedProceduralPopulationChunk
		|| bProcessedProceduralSurfaceSeed;
	if (bDidStreamingWork)
	{
		bMaterialVisualizationDeferredForStreaming = true;
	}
	AdvanceLogicalSourceReaction(DeltaSeconds);
	AdvanceUnifiedSmokeVisualization(DeltaSeconds);
	if (!MaterialSimulation)
	{
		RefreshVisibleLevelLayers(false);
		RefreshVisibleFragmentSources(false);
		ProcessPendingFragmentSourceSpawns();
		return;
	}

	if (!HasAuthority())
	{
		const bool bFinishDeferredVisualization =
			bHadDeferredMaterialVisualization && !bDidStreamingWork;
		RefreshVisibleLevelLayers(false);
		RefreshVisibleFragmentSources(false);
		ApplyReplicatedMaterialSimulationState();
		if (bFinishDeferredVisualization)
		{
			bMaterialVisualizationDeferredForStreaming = false;
			UpdateMaterialVisualization(
				FMath::Max(DeltaSeconds, MaterialVisualizationInterval));
		}
		else if (!bMaterialVisualizationDeferredForStreaming)
		{
			UpdateMaterialVisualization(DeltaSeconds);
		}
		AdvanceGroundReaction(DeltaSeconds);
		return;
	}

	const bool bFinishDeferredVisualization =
		bHadDeferredMaterialVisualization && !bDidStreamingWork;
	if (!MaterialRegistry.IsValid()
		&& IMatterFluxScriptRuntime::IsAvailable())
	{
		MaterialRegistry =
			IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	}
	AdvanceAirborneMaterialParticles(DeltaSeconds);
	TArray<FIntPoint> NextMaterialFocusCells;
	WakeRecentVisibleMaterialChunks();
	PruneExternalMaterialSupports();
	GatherMaterialSimulationFocusCells(NextMaterialFocusCells);
	int32 DisplacedMaterialCells =
		0;
	TArray<MatterFlux::Material::FLiquidDisplacementConstraint>
		DisplacementConstraints;
	DisplacementConstraints.Reserve(
		PendingMaterialDisplacementCells.Num());
	for (const TPair<
		FIntPoint,
		FMatterFluxMaterialDisplacementState>& Pair
		: PendingMaterialDisplacementCells)
	{
		DisplacementConstraints.Add({
			Pair.Key,
			Pair.Value.MaximumRemainingAmount });
	}
	DisplacedMaterialCells += MaterialSimulation->DisplaceLiquids(
		DisplacementConstraints,
		BodyLiquidDisplacementSearchRadiusCells);
	DisplacedMaterialCells += MaterialSimulation->DisplacePowders(
		DisplacementConstraints,
		BodyLiquidDisplacementSearchRadiusCells);
	MatterFlux::Material::FRuntimeAdvanceResult MaterialAdvance =
		MaterialSimulation->AdvanceAuthority(
			bRunInitialMaterialWarmupStep
				? MaterialSimulationStepSeconds
					* InitialMaterialWarmupStepsPerBatch
				: bInitialWorldEntryPending
					? 0.0f
					: DeltaSeconds,
			NextMaterialFocusCells,
			// Loading may consume a bounded batch before play begins. During play,
			// never stack several costly falling-material solves onto the same
			// streaming frame; stale excess debt is deliberately discarded.
			bRunInitialMaterialWarmupStep
				? InitialMaterialWarmupStepsPerBatch
				: 1);
	if (bRunInitialMaterialWarmupStep && MaterialAdvance.Steps > 0)
	{
		InitialMaterialWarmupStepCount += MaterialAdvance.Steps;
		InitialMaterialWarmupQuietStepCount = MaterialAdvance.bStateChanged
			? 0
			: InitialMaterialWarmupQuietStepCount + MaterialAdvance.Steps;
		bInitialMaterialWarmupStepsComplete =
			(InitialMaterialWarmupStepCount
					>= InitialMaterialWarmupMinimumSteps
				&& InitialMaterialWarmupQuietStepCount
					>= InitialMaterialWarmupQuietSteps)
			|| InitialMaterialWarmupStepCount
				>= InitialMaterialWarmupMaximumSteps;
	}
	// Only a material step can flow liquid back into a body. Frames that merely
	// project the already constrained state do not need a second identical
	// solve. This keeps body occupancy deterministic while avoiding a redundant
	// full displacement search on most render frames.
	if (MaterialAdvance.Steps > 0 || bProcessedProceduralSurfaceSeed)
	{
		DisplacedMaterialCells += MaterialSimulation->DisplaceLiquids(
			DisplacementConstraints,
			BodyLiquidDisplacementSearchRadiusCells);
		DisplacedMaterialCells += MaterialSimulation->DisplacePowders(
			DisplacementConstraints,
			BodyLiquidDisplacementSearchRadiusCells);
	}
	PreviousMaterialDisplacementCells = PendingMaterialDisplacementCells;
	PendingMaterialDisplacementCells.Reset();
	MaterialAdvance.bStateChanged |= DisplacedMaterialCells > 0;
	if (MaterialAdvance.bFocusChanged)
	{
		ReplicatedMaterialSimulationFocus =
			MaterialSimulation->GetFocuses()[0];
		bMaterialVisualizationDirty = true;
		bMaterialVisualizationDeferredForStreaming = true;
		ForceNetUpdate();
	}
	// Terrain, decorations, and the material simulation all follow every
	// possessed authority player. The material world's round-robin allocator
	// keeps this union deterministic and within its hard chunk budget.
	TArray<FIntPoint> StreamingFocusChunks;
	GatherStreamingFocusChunks(StreamingFocusChunks);
	const bool bStreamingFocusChanged =
		StreamingFocusChunks != VisibleLayerFocusChunks
		|| StreamingFocusChunks != VisibleFragmentFocusChunks;
	RefreshVisibleLevelLayers(false);
	RefreshVisibleFragmentSources(false);
	ProcessPendingFragmentSourceSpawns();

	ReplicatedMaterialSimulationStep = MaterialAdvance.LogicalStep;
	if (MaterialAdvance.bStateChanged)
	{
		bMaterialVisualizationDirty = true;
	}
	if (MaterialAdvance.Steps > 0)
	{
		if (MaterialRegistry.IsValid())
		{
			ResolveMaterialInteractions(*MaterialRegistry);
		}
	}
	// Focus changes already archive/restore material chunks and refresh both
	// terrain streaming systems. Networked worlds publish the now-dirty atomic
	// snapshot on a stable streaming frame. Standalone saves export the live
	// runtime directly and need no recurring compressed replication snapshot.
	if (!MaterialAdvance.bFocusChanged
		&& !bStreamingFocusChanged
		&& GetNetMode() != NM_Standalone
		&& MaterialSimulation->NeedsReplicationPublish())
	{
		PublishMaterialSimulationState();
		ForceNetUpdate();
	}
	if (MaterialAdvance.Steps > 0 || bDidStreamingWork)
	{
		// A liquid step and a projection rebuild both scan the active material
		// window. Accumulate the visual interval now, then rebuild on the next
		// non-simulation frame so the two O(N) passes never form one hitch.
		UpdateMaterialVisualization(DeltaSeconds, false);
	}
	else if (bFinishDeferredVisualization
		&& !MaterialAdvance.bFocusChanged)
	{
		bMaterialVisualizationDeferredForStreaming = false;
		UpdateMaterialVisualization(
			FMath::Max(DeltaSeconds, MaterialVisualizationInterval));
	}
	else if (!bMaterialVisualizationDeferredForStreaming)
	{
		UpdateMaterialVisualization(DeltaSeconds);
	}
	EmitActiveReactionParticles(DeltaSeconds);
	AdvanceGroundReaction(DeltaSeconds);
	if (!bInitialMaterialWarmupComplete
		&& bInitialMaterialWarmupStepsComplete
		&& !bMaterialVisualizationDirty
		&& !bMaterialVisualizationDeferredForStreaming
		&& IsInitialWorldStreamingReady())
	{
		bInitialMaterialWarmupComplete = true;
	}
}

void AMatterFluxPlayableWorldActor::Regenerate(const int32 NewSeed)
{
	if (!HasAuthority())
	{
		return;
	}
	PrepareForProceduralWorld();
	++GenerationRequestSerial;
	PendingGeneratedLayout.Reset();
	PendingGenerationRegistry.Reset();
	if (IsGenerationInProgress())
	{
		CompleteAsyncGeneration(false, TEXT("地图生成被同步重建替换"));
	}

	int32 CandidateSeed = FMath::RandRange(1, MAX_int32);
	if (NewSeed != 0)
	{
		CandidateSeed = NewSeed == MIN_int32 ? MAX_int32 : FMath::Abs(NewSeed);
	}
	if (CandidateSeed == MapSeed)
	{
		CandidateSeed = CandidateSeed == MAX_int32 ? 1 : CandidateSeed + 1;
	}

	MapSeed = FMath::Max(CandidateSeed, 1);
	ReplicatedTerrainHeightOverrides.Reset();
	ReplicatedMaterialSimulationStep = 0;
	ReplicatedMaterialSimulationFocus = FIntPoint::ZeroValue;
	RebuildLevel();
	ForceNetUpdate();
}

bool AMatterFluxPlayableWorldActor::RequestRegenerateAsync(
	const int32 NewSeed,
	const bool bForceExactSeed)
{
	if (!HasAuthority() || IsGenerationInProgress())
	{
		return false;
	}
	SanitizeGenerationSettings();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		CompleteAsyncGeneration(false, TEXT("Lua 内容注册表尚未就绪"));
		return false;
	}

	int32 CandidateSeed = FMath::RandRange(1, MAX_int32);
	if (NewSeed != 0)
	{
		CandidateSeed = NewSeed == MIN_int32
			? MAX_int32
			: FMath::Abs(NewSeed);
	}
	CandidateSeed = FMath::Max(CandidateSeed, 1);
	if (!bForceExactSeed && CandidateSeed == MapSeed)
	{
		CandidateSeed = CandidateSeed == MAX_int32
			? 1
			: CandidateSeed + 1;
	}

	GenerationPreviousSeed = MapSeed;
	MapSeed = CandidateSeed;
	ReplicatedMaterialSimulationStep = 0;
	ReplicatedMaterialSimulationFocus = FIntPoint::ZeroValue;
	GenerationPhase = EMatterFluxWorldGenerationPhase::BuildingLayout;
	GenerationProgress = 0.05f;
	GenerationStatusText = TEXT("正在计算地形、河流与生态分布…");
	InitialPendingGenerationObjects = 0;
	const uint64 RequestSerial = ++GenerationRequestSerial;
	ForceNetUpdate();

	TWeakObjectPtr<AMatterFluxPlayableWorldActor> WeakThis(this);
	Async(
		EAsyncExecution::ThreadPool,
		[WeakThis, Registry, CandidateSeed, RequestSerial]()
		{
			auto GeneratedLayout = MakeShared<
				MatterFlux::PlayableLevel::FLevelLayout,
				ESPMode::ThreadSafe>();
			const bool bBuilt = MatterFlux::PlayableLevel::BuildLevelLayout(
				CandidateSeed,
				*GeneratedLayout,
				Registry.Get());
			AsyncTask(
				ENamedThreads::GameThread,
				[WeakThis,
				 Registry,
				 RequestSerial,
				 bBuilt,
				 GeneratedLayout]() mutable
				{
					AMatterFluxPlayableWorldActor* WorldActor = WeakThis.Get();
					if (!WorldActor
						|| WorldActor->GenerationRequestSerial != RequestSerial
						|| WorldActor->GenerationPhase
							!= EMatterFluxWorldGenerationPhase::BuildingLayout)
					{
						return;
					}
					if (!bBuilt)
					{
						WorldActor->MapSeed = WorldActor->GenerationPreviousSeed;
						WorldActor->ForceNetUpdate();
						WorldActor->CompleteAsyncGeneration(
							false,
							TEXT("地图布局计算失败，已保留原世界"));
						return;
					}
					WorldActor->PendingGeneratedLayout = MakeUnique<
						MatterFlux::PlayableLevel::FLevelLayout>(
							MoveTemp(*GeneratedLayout));
					WorldActor->PendingGenerationRegistry = Registry;
					WorldActor->GenerationPhase =
						EMatterFluxWorldGenerationPhase::InitializingSimulation;
					WorldActor->GenerationProgress = 0.55f;
					WorldActor->GenerationStatusText =
						TEXT("正在初始化液体、气体与燃烧模拟…");
				});
		});
	return true;
}

bool AMatterFluxPlayableWorldActor::LoadCustomMap(
	const FName MapId,
	const int32 Seed,
	FString& OutError)
{
	OutError.Reset();
	if (!HasAuthority())
	{
		OutError = TEXT("custom map loading requires authority");
		return false;
	}
	if (MapId.IsNone() || IsGenerationInProgress())
	{
		OutError = TEXT("custom map id is empty or another generation is active");
		return false;
	}
	const FName PreviousMapId = ActiveCustomMapId;
	const int32 PreviousSeed = MapSeed;
	ActiveCustomMapId = MapId;
	MapSeed = MapId == TEXT("story.paper_magic")
		? PaperMagicStorySeed
		: FMath::Max(Seed, 1);
	if (!RebuildActiveCustomMap(OutError))
	{
		ActiveCustomMapId = PreviousMapId;
		MapSeed = PreviousSeed;
		return false;
	}
	GenerationPhase = EMatterFluxWorldGenerationPhase::Complete;
	GenerationProgress = 1.0f;
	GenerationStatusText = TEXT("故事地图加载完成");
	CustomMapLoadSerial = CustomMapLoadSerial == MAX_int32
		? 1 : CustomMapLoadSerial + 1;
	ForceNetUpdate();
	return true;
}

bool AMatterFluxPlayableWorldActor::TryGetCustomMapMarker(
	const FName MarkerId,
	FVector& OutWorldLocation) const
{
	const FVector* LocalLocation =
		ActiveCustomMapScene.MarkerLocations.Find(MarkerId);
	if (!LocalLocation)
	{
		return false;
	}
	OutWorldLocation = GetActorTransform().TransformPosition(*LocalLocation);
	return true;
}

bool AMatterFluxPlayableWorldActor::RebuildActiveCustomMap(
	FString& OutError)
{
	OutError.Reset();
	if (ActiveCustomMapId.IsNone())
	{
		OutError = TEXT("no custom map is selected");
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("Lua content registry is unavailable");
		return false;
	}
	if (ActiveCustomMapId == TEXT("story.paper_magic"))
	{
		return RebuildProceduralStoryMap(*Registry, OutError);
	}

	auto NextRuntime =
		MakeUnique<MatterFlux::Material::FSimulationRuntime>();
	MatterFlux::Material::FCustomMapScene NextScene;
	if (!MatterFlux::Material::BuildPlayableCustomMap(
		ActiveCustomMapId,
		*Registry,
		MapSeed,
		MaterialSimulationStepSeconds,
		MaxMaterialSimulationStepsPerFrame,
		*NextRuntime,
		NextScene,
		OutError))
	{
		return false;
	}

	++GenerationRequestSerial;
	PendingGeneratedLayout.Reset();
	PendingGenerationRegistry.Reset();
	DestroyMaterialVisualization();
	DestroyCustomMapSceneBoxes();
	DestroyTerrainChunkMeshes();
	DestroyGeneratedHouse();
	if (HasAuthority())
	{
		DestroyGroundStateChunks();
	}
	// Both authority and clients own disposable proxy meshes. Clearing only on
	// the server leaves the client's free-mode forest rendered around the
	// authored story bounds after ActiveCustomMapId replicates.
	DestroyGeneratedFragmentSources();
	GroundReaction.Reset();
	LayerStreamingCaches.Reset();
	LiquidLayerDefinitions.Reset();
	VisibleLayerFocusChunks.Reset();
	for (const TPair<
		FName,
		TObjectPtr<UHierarchicalInstancedStaticMeshComponent>>& Pair
		: GeneratedLayerInstances)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->DestroyComponent();
		}
	}
	GeneratedLayerInstances.Reset();

	MaterialSimulation = MoveTemp(NextRuntime);
	ActiveCustomMapScene = MoveTemp(NextScene);
	MaterialSimulationCellSize = ActiveCustomMapScene.CellSizeCentimeters;
	MaterialVisualizationAccumulator = 0.0f;
	bMaterialVisualizationDirty = true;
	ReplicatedMaterialSimulationStep = 0;
	const TArray<FIntPoint>& Focuses = MaterialSimulation->GetFocuses();
	ReplicatedMaterialSimulationFocus = Focuses.IsEmpty()
		? FIntPoint::ZeroValue
		: Focuses[0];
	BuildCustomMapSceneBoxes(*Registry);
	BuildCustomMapStructures(*Registry);
	RebuildMaterialVisualization(*Registry);
	if (HasAuthority())
	{
		PublishMaterialSimulationState();
	}
	else
	{
		ApplyReplicatedMaterialSimulationState();
	}
	RecaptureStaticSky();
	return true;
}

bool AMatterFluxPlayableWorldActor::RebuildProceduralStoryMap(
	const FMatterFluxContentRegistry& Registry,
	FString& OutError)
{
	MatterFlux::Material::FChunkedMaterialWorld AuthoredSurface;
	MatterFlux::Material::FCustomMapScene NextScene;
	if (!MatterFlux::Material::BuildCustomMap(
		ActiveCustomMapId,
		Registry,
		MapSeed,
		AuthoredSurface,
		NextScene,
		OutError))
	{
		return false;
	}

	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!MatterFlux::PlayableLevel::BuildLevelLayout(
		MapSeed,
		Layout,
		&Registry))
	{
		OutError = TEXT("fixed-seed story environment generation failed");
		return false;
	}

	const FVector2D Minimum(
		NextScene.MinimumCell.X * NextScene.CellSizeCentimeters,
		NextScene.MinimumCell.Y * NextScene.CellSizeCentimeters);
	const FVector2D MaximumExclusive(
		NextScene.MaximumCellExclusive.X * NextScene.CellSizeCentimeters,
		NextScene.MaximumCellExclusive.Y * NextScene.CellSizeCentimeters);
	const auto IsInsideStoryBounds = [Minimum, MaximumExclusive](
		const FVector& Location)
	{
		return Location.X >= Minimum.X
			&& Location.X < MaximumExclusive.X
			&& Location.Y >= Minimum.Y
			&& Location.Y < MaximumExclusive.Y;
	};
	const FVector* StoryHouseLocation = NextScene.MarkerLocations.Find(
		TEXT("structure.house.two_storey.0"));
	constexpr float StoryHouseTreeReserveHalfExtent = 950.0f;
	const auto IsInsideStoryHouseTreeReserve = [StoryHouseLocation](
		const FVector& Location)
	{
		return StoryHouseLocation
			&& FMath::Abs(Location.X - StoryHouseLocation->X)
				< StoryHouseTreeReserveHalfExtent
			&& FMath::Abs(Location.Y - StoryHouseLocation->Y)
				< StoryHouseTreeReserveHalfExtent;
	};

	// Keep complete tree aggregates together when the authored story boundary
	// cuts through the seed forest. A partial aggregate would leave detached
	// canopies or trunks at the edge. The camp house is authored at a fixed
	// marker, so reject the entire random tree aggregate when its root enters
	// the reserved house clearing.
	TSet<FGuid> IncludedAggregates;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.AggregateId.IsValid()
			&& Source.bAggregateRoot
			&& IsInsideStoryBounds(Source.Transform.GetLocation())
			&& (Source.Name != TEXT("TreeTrunk")
				|| !IsInsideStoryHouseTreeReserve(
					Source.Transform.GetLocation())))
		{
			IncludedAggregates.Add(Source.AggregateId);
		}
	}
	Layout.FragmentSources.RemoveAll(
		[&IncludedAggregates, &IsInsideStoryBounds](
			const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
		{
			return Source.AggregateId.IsValid()
				? !IncludedAggregates.Contains(Source.AggregateId)
				: !IsInsideStoryBounds(Source.Transform.GetLocation());
		});
	for (MatterFlux::PlayableLevel::FLevelLayer& Layer : Layout.Layers)
	{
		if (Layer.RenderMode
			!= MatterFlux::PlayableLevel::ELevelLayerRenderMode::Liquid)
		{
			continue;
		}
		Layer.Instances.RemoveAll(
			[&IsInsideStoryBounds](const FTransform& Transform)
			{
				return !IsInsideStoryBounds(Transform.GetLocation());
			});
	}

	++GenerationRequestSerial;
	PendingGeneratedLayout.Reset();
	PendingGenerationRegistry.Reset();
	DestroyMaterialVisualization();
	DestroyCustomMapSceneBoxes();
	DestroyTerrainChunkMeshes();
	DestroyGeneratedHouse();
	if (HasAuthority())
	{
		DestroyGroundStateChunks();
	}
	DestroyGeneratedFragmentSources();
	GroundReaction.Reset();
	LayerStreamingCaches.Reset();
	LiquidLayerDefinitions.Reset();
	VisibleLayerFocusChunks.Reset();
	for (const TPair<
		FName,
		TObjectPtr<UHierarchicalInstancedStaticMeshComponent>>& Pair
		: GeneratedLayerInstances)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->DestroyComponent();
		}
	}
	GeneratedLayerInstances.Reset();

	ActiveCustomMapScene = MoveTemp(NextScene);
	MaterialSimulationCellSize = ProceduralMaterialSimulationCellSize;
	InitializeMaterialSimulation(Registry, Layout);
	InitializeGroundReaction(Registry, Layout);
	BuildLayerStreamingCache(Layout);
	RefreshVisibleLevelLayers(true);
	RebuildFragmentSources(Layout.FragmentSources);
	BuildCustomMapSceneBoxes(Registry);
	BuildCustomMapStructures(Registry);
	RecaptureStaticSky();
	if (!MaterialSimulation || !TerrainHeightField.IsValid())
	{
		OutError = TEXT("fixed-seed story environment initialization failed");
		return false;
	}
	return true;
}

void AMatterFluxPlayableWorldActor::PrepareForProceduralWorld()
{
	DestroyCustomMapSceneBoxes();
	ActiveCustomMapScene = MatterFlux::Material::FCustomMapScene();
	ActiveCustomMapId = NAME_None;
	MaterialSimulationCellSize = ProceduralMaterialSimulationCellSize;
}

void AMatterFluxPlayableWorldActor::BuildCustomMapSceneBoxes(
	const FMatterFluxContentRegistry& Registry)
{
	DestroyCustomMapSceneBoxes();
	if (!CubeMesh)
	{
		return;
	}
	for (const MatterFlux::Material::FCustomMapSceneBox& Box
		: ActiveCustomMapScene.Boxes)
	{
		const FName ComponentName = MakeUniqueObjectName(
			this,
			UStaticMeshComponent::StaticClass(),
			*FString::Printf(TEXT("CustomMap_%s"), *Box.Id.ToString()));
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
			this,
			ComponentName);
		Component->SetupAttachment(SceneRoot);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(CubeMesh);
		FVector BoxCenter = Box.Center;
		if (Box.bCollision && TerrainHeightField.IsValid())
		{
			const FVector Probe = GetActorTransform().TransformPosition(
				FVector(Box.Center.X, Box.Center.Y, 0.0f));
			float SurfaceWorldZ = 0.0f;
			if (TrySampleTerrainHeightAtWorldLocation(Probe, SurfaceWorldZ))
			{
				const FVector LocalSurface = GetActorTransform()
					.InverseTransformPosition(FVector(
						Probe.X,
						Probe.Y,
						SurfaceWorldZ));
				BoxCenter.Z = LocalSurface.Z + FMath::Abs(Box.Size.Z) * 0.5f;
			}
		}
		Component->SetRelativeLocation(BoxCenter);
		Component->SetRelativeScale3D(Box.Size / 100.0f);
		Component->SetCollisionEnabled(Box.bCollision
			? ECollisionEnabled::QueryAndPhysics
			: ECollisionEnabled::NoCollision);
		Component->SetCollisionResponseToAllChannels(
			Box.bCollision ? ECR_Block : ECR_Ignore);
		Component->SetCanEverAffectNavigation(Box.bCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCastShadow(true);
		Component->ComponentTags.AddUnique(TEXT("MatterFluxCustomMapScene"));
		if (const FMatterFluxMaterialDefinition* Material =
			Registry.Materials.Find(Box.MaterialId))
		{
			if (VoxelColorMaterialTemplate)
			{
				UMaterialInstanceDynamic* DynamicMaterial =
					UMaterialInstanceDynamic::Create(
						VoxelColorMaterialTemplate,
						this);
				DynamicMaterial->SetVectorParameterValue(
					TEXT("Color"), Material->Color);
				DynamicMaterial->SetScalarParameterValue(
					TEXT("FaceContrast"), 0.82f);
				DynamicMaterial->SetScalarParameterValue(
					TEXT("ColorVariation"), 0.05f);
				DynamicMaterial->SetScalarParameterValue(
					TEXT("PixelSize"), 12.0f);
				DynamicMaterial->SetScalarParameterValue(
					TEXT("Roughness"), 0.90f);
				Component->SetMaterial(0, DynamicMaterial);
			}
		}
		AddInstanceComponent(Component);
		Component->RegisterComponent();
		GeneratedCustomMapSceneBoxes.Add(Component);
	}
}

void AMatterFluxPlayableWorldActor::DestroyCustomMapSceneBoxes()
{
	for (UStaticMeshComponent* Component : GeneratedCustomMapSceneBoxes)
	{
		if (IsValid(Component))
		{
			Component->DestroyComponent();
		}
	}
	GeneratedCustomMapSceneBoxes.Reset();
}

void AMatterFluxPlayableWorldActor::BuildCustomMapStructures(
	const FMatterFluxContentRegistry& Registry)
{
	DestroyGeneratedHouse();
	if (!HasAuthority() || !GetWorld())
	{
		return;
	}
	const FName StructureId = TEXT("structure.house.two_storey");
	const FMatterFluxStructureDefinition* Definition =
		Registry.Structures.Find(StructureId);
	FVector HouseLocation;
	if (!Definition
		|| Definition->GeneratorId != TEXT("two_storey_house")
		|| !TryGetCustomMapMarker(
			TEXT("structure.house.two_storey.0"), HouseLocation))
	{
		return;
	}
	float FoundationTop = HouseLocation.Z;
	const FVector2D SampleOffsets[] = {
		FVector2D::ZeroVector,
		FVector2D(-500.0f, -360.0f),
		FVector2D(-500.0f, 360.0f),
		FVector2D(500.0f, -360.0f),
		FVector2D(500.0f, 360.0f)
	};
	for (const FVector2D Offset : SampleOffsets)
	{
		FVector Sample = HouseLocation;
		Sample.X += Offset.X;
		Sample.Y += Offset.Y;
		float Height = 0.0f;
		if (TrySampleTerrainHeightAtWorldLocation(Sample, Height))
		{
			FoundationTop = FMath::Max(FoundationTop, Height);
		}
	}
	HouseLocation.Z = FoundationTop + 4.0f;
	const FTransform HouseTransform(
		FRotator::ZeroRotator, HouseLocation);
	GeneratedHouse = GetWorld()->SpawnActorDeferred<
		AMatterFluxTwoStoreyHouseActor>(
			AMatterFluxTwoStoreyHouseActor::StaticClass(),
			HouseTransform,
			this,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (GeneratedHouse)
	{
		GeneratedHouse->InitializeStructureDefinition(StructureId);
		GeneratedHouse->FinishSpawning(HouseTransform);
		GeneratedHouse->Tags.AddUnique(TEXT("MatterFluxGeneratedHouse"));
		GeneratedHouse->Tags.AddUnique(TEXT("MatterFluxStoryHouse"));
	}
}

bool AMatterFluxPlayableWorldActor::IsGenerationInProgress() const
{
	return GenerationPhase == EMatterFluxWorldGenerationPhase::BuildingLayout
		|| GenerationPhase
			== EMatterFluxWorldGenerationPhase::InitializingSimulation
		|| GenerationPhase
			== EMatterFluxWorldGenerationPhase::BuildingStreaming
		|| GenerationPhase
			== EMatterFluxWorldGenerationPhase::SpawningWorldObjects;
}

void AMatterFluxPlayableWorldActor::AdvanceAsyncGeneration()
{
	if (!PendingGeneratedLayout || !PendingGenerationRegistry.IsValid())
	{
		return;
	}
	switch (GenerationPhase)
	{
	case EMatterFluxWorldGenerationPhase::InitializingSimulation:
		// Keep the authored story world intact while the worker builds the next
		// layout. Only commit the mode switch after that build has succeeded.
		PrepareForProceduralWorld();
		InitializeMaterialSimulation(
			*PendingGenerationRegistry,
			*PendingGeneratedLayout);
		InitializeGroundReaction(
			*PendingGenerationRegistry,
			*PendingGeneratedLayout);
		GenerationPhase = EMatterFluxWorldGenerationPhase::BuildingStreaming;
		GenerationProgress = 0.68f;
		GenerationStatusText = TEXT("正在构建可见地形分块…");
		break;

	case EMatterFluxWorldGenerationPhase::BuildingStreaming:
		BuildLayerStreamingCache(*PendingGeneratedLayout);
		RefreshVisibleLevelLayers(true);
		RebuildGeneratedHouse(*PendingGeneratedLayout);
		GenerationPhase =
			EMatterFluxWorldGenerationPhase::SpawningWorldObjects;
		GenerationProgress = 0.82f;
		GenerationStatusText = TEXT("正在生成树木、花草与可破坏物体…");
		if (HasAuthority())
		{
			RebuildFragmentSources(
				PendingGeneratedLayout->FragmentSources,
				false);
		}
		InitialPendingGenerationObjects = FMath::Max(
			PendingFragmentSourceSpawns.Num()
				+ PendingFragmentSourceDespawns.Num(),
			1);
		break;

	case EMatterFluxWorldGenerationPhase::SpawningWorldObjects:
	{
		const int32 Remaining = PendingFragmentSourceSpawns.Num()
			+ PendingFragmentSourceDespawns.Num();
		if (Remaining <= 0)
		{
			PendingGeneratedLayout.Reset();
			PendingGenerationRegistry.Reset();
			CompleteAsyncGeneration(true, TEXT("地图生成完成"));
		}
		else
		{
			const float CompletedRatio = 1.0f
				- static_cast<float>(Remaining)
					/ static_cast<float>(InitialPendingGenerationObjects);
			GenerationProgress = FMath::Clamp(
				0.82f + CompletedRatio * 0.17f,
				0.82f,
				0.99f);
		}
		break;
	}
	default:
		break;
	}
}

void AMatterFluxPlayableWorldActor::CompleteAsyncGeneration(
	const bool bSuccess,
	const FString& Message)
{
	GenerationPhase = bSuccess
		? EMatterFluxWorldGenerationPhase::Complete
		: EMatterFluxWorldGenerationPhase::Failed;
	GenerationProgress = bSuccess ? 1.0f : 0.0f;
	GenerationStatusText = Message;
	if (!bSuccess)
	{
		PendingGeneratedLayout.Reset();
		PendingGenerationRegistry.Reset();
	}
	else
	{
		RecaptureStaticSky();
	}
	GenerationFinished.Broadcast(bSuccess, Message);
}

void AMatterFluxPlayableWorldActor::OnRep_MapSeed()
{
	++GenerationRequestSerial;
	PendingGeneratedLayout.Reset();
	PendingGenerationRegistry.Reset();
	if (ActiveCustomMapId.IsNone())
	{
		RebuildLevel();
	}
	else
	{
		FString Error;
		if (!RebuildActiveCustomMap(Error))
		{
			UE_LOG(LogMatterFlux, Error,
				TEXT("Replicated custom map rebuild failed: %s"), *Error);
		}
	}
}

void AMatterFluxPlayableWorldActor::OnRep_ActiveCustomMapId()
{
	if (ActiveCustomMapId.IsNone())
	{
		RebuildLevel();
		return;
	}
	FString Error;
	if (!RebuildActiveCustomMap(Error))
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Replicated custom map selection failed: %s"), *Error);
	}
}

void AMatterFluxPlayableWorldActor::OnRep_MaterialSimulationState()
{
	ApplyReplicatedMaterialSimulationState();
	RefreshVisibleLevelLayers(false);
}

void AMatterFluxPlayableWorldActor::OnRep_FragmentSourceStates()
{
	bReplicatedFragmentSourceStatesDirty = false;
	ApplyReplicatedFragmentSourceStates();
}

void AMatterFluxPlayableWorldActor::OnRep_TerrainHeightOverrides()
{
	ApplyTerrainHeightOverrides(
		ReplicatedTerrainHeightOverrides,
		true);
}

void AMatterFluxPlayableWorldActor::
	MarkReplicatedFragmentSourceStatesDirty()
{
	bReplicatedFragmentSourceStatesDirty = true;
}

void AMatterFluxPlayableWorldActor::RebuildLevel()
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxWorldRebuild);
	PrepareForProceduralWorld();
	SanitizeGenerationSettings();

	MatterFlux::PlayableLevel::FLevelLayout Layout;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!MatterFlux::PlayableLevel::BuildLevelLayout(
		MapSeed,
		Layout,
		Registry.Get()))
	{
		return;
	}
	if (Registry.IsValid())
	{
		ApplyGeneratedLayoutSynchronously(*Registry, Layout);
	}
	else
	{
		BuildLayerStreamingCache(Layout);
		RefreshVisibleLevelLayers(true);
		RebuildGeneratedHouse(Layout);
		RebuildFragmentSources(Layout.FragmentSources);
	}
	RecaptureStaticSky();
}

void AMatterFluxPlayableWorldActor::SanitizeGenerationSettings()
{
	MaterialSimulationStepSeconds =
		FMath::IsFinite(MaterialSimulationStepSeconds)
			? FMath::Clamp(
				MaterialSimulationStepSeconds,
				0.01f,
				0.25f)
			: 1.0f / 30.0f;
	MaterialBodyWakeRefillDelaySeconds =
		FMath::IsFinite(MaterialBodyWakeRefillDelaySeconds)
			? FMath::Clamp(MaterialBodyWakeRefillDelaySeconds, 0.0f, 2.0f)
			: 0.25f;
	MaterialBodyWakeRefillDurationSeconds =
		FMath::IsFinite(MaterialBodyWakeRefillDurationSeconds)
			? FMath::Clamp(MaterialBodyWakeRefillDurationSeconds, 0.05f, 4.0f)
			: 0.55f;
	MaterialSimulationChunkSize =
		FMath::Clamp(MaterialSimulationChunkSize, 4, 256);
	MaterialSimulationActiveChunkRadius =
		FMath::Clamp(MaterialSimulationActiveChunkRadius, 0, 8);
	MaterialSimulationCellSize =
		FMath::IsFinite(MaterialSimulationCellSize)
			? FMath::Clamp(
				MaterialSimulationCellSize,
				4.0f,
				100.0f)
			: MatterFlux::PlayableLevel::TerrainCellSize;
	MaterialLiquidColumnHeight =
		FMath::IsFinite(MaterialLiquidColumnHeight)
			? FMath::Clamp(MaterialLiquidColumnHeight, 8.0f, 300.0f)
			: 128.0f;
	MaterialLiquidVisualThickness =
		FMath::IsFinite(MaterialLiquidVisualThickness)
			? FMath::Clamp(MaterialLiquidVisualThickness, 1.0f, 32.0f)
			: 8.0f;
	MaterialVisualizationInterval =
		FMath::IsFinite(MaterialVisualizationInterval)
			? FMath::Clamp(
				MaterialVisualizationInterval,
				1.0f / 60.0f,
				1.0f)
			: 1.0f / 30.0f;
	MaxLiquidProjectionChunksPerVisualization = FMath::Clamp(
		MaxLiquidProjectionChunksPerVisualization, 1, 8);
	TerrainStreamingChunkSize =
		FMath::Clamp(TerrainStreamingChunkSize, 8, 128);
	TerrainStreamingChunkRadius =
		FMath::Clamp(TerrainStreamingChunkRadius, 0, 5);
	const int32 StreamingDiameter =
		(TerrainStreamingChunkRadius + 1) * 2 + 1;
	const int32 CameraWindowOverlapSide =
		FMath::Max(StreamingDiameter - 2, 0);
	const int32 MinimumTerrainCacheSize = FMath::Min(
		StreamingDiameter * StreamingDiameter * 2
			- CameraWindowOverlapSide * CameraWindowOverlapSide,
		256);
	TerrainChunkCacheLimit =
		FMath::Clamp(
			TerrainChunkCacheLimit,
			FMath::Max(MinimumTerrainCacheSize, 9),
			256);
	MaxTerrainChunkPrefetchesPerFrame =
		FMath::Clamp(MaxTerrainChunkPrefetchesPerFrame, 1, 8);
	TerrainChunkPrefetchBudgetMilliseconds = FMath::Clamp(
		TerrainChunkPrefetchBudgetMilliseconds,
		0.5f,
		16.0f);
	FragmentSourceProxyCacheLimit =
		FMath::Clamp(FragmentSourceProxyCacheLimit, 9, 256);
	MaxDecorationSpawnsPerFrame =
		FMath::Clamp(MaxDecorationSpawnsPerFrame, 1, 64);
	DecorationSpawnBudgetMilliseconds =
		FMath::IsFinite(DecorationSpawnBudgetMilliseconds)
			? FMath::Clamp(
				DecorationSpawnBudgetMilliseconds,
				0.25f,
				16.0f)
			: 4.0f;
	MaxProceduralPopulationUpdatesPerFrame = FMath::Clamp(
		MaxProceduralPopulationUpdatesPerFrame,
		1,
		8);
	ProceduralPopulationBudgetMilliseconds =
		FMath::IsFinite(ProceduralPopulationBudgetMilliseconds)
			? FMath::Clamp(
				ProceduralPopulationBudgetMilliseconds,
				0.25f,
				16.0f)
			: 4.0f;
	ProceduralSurfaceSeedCellsPerFrame = FMath::Clamp(
		ProceduralSurfaceSeedCellsPerFrame,
		64,
		2048);
	MaxAsyncStreamingBuildTasks =
		FMath::Clamp(MaxAsyncStreamingBuildTasks, 1, 16);
}

void AMatterFluxPlayableWorldActor::ApplyGeneratedLayoutSynchronously(
	const FMatterFluxContentRegistry& Registry,
	const MatterFlux::PlayableLevel::FLevelLayout& Layout)
{
	InitializeMaterialSimulation(Registry, Layout);
	InitializeGroundReaction(Registry, Layout);
	BuildLayerStreamingCache(Layout);
	RefreshVisibleLevelLayers(true);
	RebuildGeneratedHouse(Layout);
	RebuildFragmentSources(Layout.FragmentSources);
}

void AMatterFluxPlayableWorldActor::RecaptureStaticSky()
{
	if (SkyLight
		&& GetNetMode() != NM_DedicatedServer
		&& !SkyLight->bRealTimeCapture)
	{
		SkyLight->RecaptureSky();
	}
}

void AMatterFluxPlayableWorldActor::RebuildGeneratedHouse(
	const MatterFlux::PlayableLevel::FLevelLayout& Layout)
{
	// 房屋是带权威碰撞的复制 Actor。客户端只接收服务器生成的实例，
	// 不能在 OnRep_MapSeed 的本地地图重建中再生成一份重叠房屋。
	if (!HasAuthority() || !GetWorld())
	{
		return;
	}
	DestroyGeneratedHouse();

	const FVector2D SampleOffsets[] = {
		FVector2D::ZeroVector,
		FVector2D(-500.0f, -360.0f),
		FVector2D(-500.0f, 360.0f),
		FVector2D(500.0f, -360.0f),
		FVector2D(500.0f, 360.0f),
		FVector2D(-500.0f, 0.0f),
		FVector2D(500.0f, 0.0f)
	};
	float FoundationTop = -TNumericLimits<float>::Max();
	for (const FVector2D Offset : SampleOffsets)
	{
		FVector Sample = Layout.HouseLocation;
		Sample.X += Offset.X;
		Sample.Y += Offset.Y;
		float Height = 0.0f;
		if (TrySampleTerrainHeightAtWorldLocation(Sample, Height))
		{
			FoundationTop = FMath::Max(FoundationTop, Height);
		}
	}
	if (!FMath::IsFinite(FoundationTop))
	{
		FoundationTop = 150.0f;
	}

	const FName StructureDefinitionId = TEXT("structure.house.two_storey");
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	const FMatterFluxStructureDefinition* StructureDefinition =
		Registry.IsValid()
			? Registry->Structures.Find(StructureDefinitionId) : nullptr;
	if (!StructureDefinition
		|| StructureDefinition->GeneratorId != TEXT("two_storey_house"))
	{
		UE_LOG(LogMatterFlux, Warning,
			TEXT("Skipping generated house because Lua structure '%s' is unavailable or selects an unsupported generator."),
			*StructureDefinitionId.ToString());
		return;
	}

	const FTransform HouseTransform(
		// 固定 2.5D 镜头的 yaw 为 -45°。房屋旋转 45° 后，山墙
		// 面向镜头、屋脊横跨屏幕，避免沿屋脊观察产生放射透视。
		FRotator(0.0f, 45.0f, 0.0f),
		FVector(
			Layout.HouseLocation.X,
			Layout.HouseLocation.Y,
			FoundationTop + 4.0f));
	GeneratedHouse = GetWorld()->SpawnActorDeferred<AMatterFluxTwoStoreyHouseActor>(
		AMatterFluxTwoStoreyHouseActor::StaticClass(),
		HouseTransform,
		this,
		nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (GeneratedHouse)
	{
		GeneratedHouse->InitializeStructureDefinition(StructureDefinitionId);
		GeneratedHouse->FinishSpawning(HouseTransform);
		GeneratedHouse->Tags.AddUnique(TEXT("MatterFluxGeneratedHouse"));
		PrewarmStreamedHousePool(HouseTransform);
	}
}

void AMatterFluxPlayableWorldActor::PrewarmStreamedHousePool(
	const FTransform& TemplateTransform)
{
	if (!HasAuthority() || !GetWorld())
	{
		return;
	}
	const FName StructureDefinitionId = TEXT("structure.house.two_storey");
	while (StreamedHousePool.Num() < PrewarmedStreamedHouseCount)
	{
		// House source IDs are deterministic from the owning house transform.
		// Spawning pool entries at the live house transform gives every pooled
		// wall the same IDs as the visible house; the last prewarm then replaces
		// the live walls in the world-cut registry. Give each prewarm a unique,
		// underground staging transform until it is activated at its real site.
		FTransform PrewarmTransform = TemplateTransform;
		FVector PrewarmLocation = TemplateTransform.GetLocation();
		PrewarmLocation.Z = -10000000.0f
			- static_cast<float>(StreamedHousePool.Num()) * 2000.0f;
		PrewarmTransform.SetLocation(PrewarmLocation);
		AMatterFluxTwoStoreyHouseActor* House =
			GetWorld()->SpawnActorDeferred<AMatterFluxTwoStoreyHouseActor>(
				AMatterFluxTwoStoreyHouseActor::StaticClass(),
				PrewarmTransform,
				this,
				nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!House)
		{
			break;
		}
		House->InitializeStructureDefinition(StructureDefinitionId);
		House->FinishSpawning(PrewarmTransform);
		House->Tags.AddUnique(TEXT("MatterFluxGeneratedHouse"));
		House->Tags.AddUnique(TEXT("MatterFluxStreamedPopulation"));
		House->DeactivateForStreamingPool();
		StreamedHousePool.Add(House);
	}
}

AMatterFluxTwoStoreyHouseActor*
	AMatterFluxPlayableWorldActor::AcquireStreamedHouse(
		const FTransform& HouseTransform,
		const FName StructureDefinitionId)
{
	while (!StreamedHousePool.IsEmpty())
	{
		AMatterFluxTwoStoreyHouseActor* House = StreamedHousePool.Pop();
		if (!IsValid(House))
		{
			continue;
		}
		House->ReactivateFromStreamingPool(
			HouseTransform,
			StructureDefinitionId);
		return House;
	}
	if (!GetWorld())
	{
		return nullptr;
	}
	AMatterFluxTwoStoreyHouseActor* House =
		GetWorld()->SpawnActorDeferred<AMatterFluxTwoStoreyHouseActor>(
			AMatterFluxTwoStoreyHouseActor::StaticClass(),
			HouseTransform,
			this,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (House)
	{
		House->InitializeStructureDefinition(StructureDefinitionId);
		House->FinishSpawning(HouseTransform);
		House->Tags.AddUnique(TEXT("MatterFluxGeneratedHouse"));
		House->Tags.AddUnique(TEXT("MatterFluxStreamedPopulation"));
	}
	return House;
}

void AMatterFluxPlayableWorldActor::ReleaseStreamedHouse(
	AMatterFluxTwoStoreyHouseActor* House)
{
	if (!IsValid(House))
	{
		return;
	}
	House->DeactivateForStreamingPool();
	StreamedHousePool.AddUnique(House);
}

void AMatterFluxPlayableWorldActor::DestroyGeneratedHouse()
{
	if (IsValid(GeneratedHouse))
	{
		GeneratedHouse->Destroy();
	}
	GeneratedHouse = nullptr;
	DestroyGeneratedStreamedHouses();
}

void AMatterFluxPlayableWorldActor::DestroyGeneratedStreamedHouses()
{
	for (const TPair<
		FIntPoint,
		TObjectPtr<AMatterFluxTwoStoreyHouseActor>>& Pair
		: GeneratedStreamedHouses)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	GeneratedStreamedHouses.Reset();
	for (AMatterFluxTwoStoreyHouseActor* House : StreamedHousePool)
	{
		if (IsValid(House))
		{
			House->Destroy();
		}
	}
	StreamedHousePool.Reset();
}

bool AMatterFluxPlayableWorldActor::CaptureSaveState(
	FMatterFluxWorldSaveState& OutState,
	FString& OutError) const
{
	OutState = FMatterFluxWorldSaveState();
	OutError.Reset();
	if (!HasAuthority() || !MaterialSimulation)
	{
		OutError = TEXT("world save capture requires an initialized authority world");
		return false;
	}
	if (!MaterialSimulation->ExportActiveState(
		OutState.MaterialActiveState,
		OutError))
	{
		return false;
	}
	OutState.TerrainHeightOverrides =
		ReplicatedTerrainHeightOverrides;

	TMap<FGuid, FMatterFluxSavedFragmentSourceState> SavedById;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		if (Pair.Value.HasPersistentChanges())
		{
			SavedById.Add(
				Pair.Key,
				ToSavedFragmentState(Pair.Key, Pair.Value));
		}
	}
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		const AFragment2DSourceActor* SourceActor = Pair.Value;
		if (!IsValid(SourceActor))
		{
			continue;
		}
		FFragment2DSourceStreamingState RuntimeState;
		FString CaptureError;
		if (!SourceActor->CaptureStreamingState(RuntimeState, CaptureError))
		{
			OutError = FString::Printf(
				TEXT("fragment source %s could not be saved: %s"),
				*Pair.Key.ToString(),
				*CaptureError);
			return false;
		}
		if (RuntimeState.HasPersistentChanges()
			|| SourceActor->bDetachedFromTerrain)
		{
			FMatterFluxSavedFragmentSourceState Saved =
				ToSavedFragmentState(Pair.Key, RuntimeState);
			Saved.ActorTransform = SourceActor->GetActorTransform();
			Saved.bDetachedFromTerrain =
				SourceActor->bDetachedFromTerrain;
			SavedById.Add(Pair.Key, MoveTemp(Saved));
		}
	}
	SavedById.GenerateValueArray(OutState.FragmentSources);
	OutState.FragmentSources.Sort(
		[](const FMatterFluxSavedFragmentSourceState& A,
			const FMatterFluxSavedFragmentSourceState& B)
		{
			return A.SourceId.ToString(EGuidFormats::Digits)
				< B.SourceId.ToString(EGuidFormats::Digits);
		});
	OutState.RemovedFragmentSourceIds.Reserve(
		RemovedFragmentSourceIds.Num());
	for (const FGuid SourceId : RemovedFragmentSourceIds)
	{
		OutState.RemovedFragmentSourceIds.Add(SourceId);
	}
	OutState.RemovedFragmentSourceIds.Sort(
		[](const FGuid& A, const FGuid& B)
		{
			return A.ToString(EGuidFormats::Digits)
				< B.ToString(EGuidFormats::Digits);
		});

	if (GroundReaction)
	{
		MatterFlux::Reaction::FGroundRuntimeSnapshot Snapshot;
		if (!GroundReaction->CaptureState(Snapshot))
		{
			OutError = TEXT("ground reaction state could not be captured");
			return false;
		}
		OutState.bHasGroundReactionState = true;
		OutState.GroundReactionState =
			ToSavedReactionState(Snapshot.ReactionState);
		OutState.GroundReactionAccumulator =
			Snapshot.StepAccumulator;
		OutState.GroundReactionRevision =
			Snapshot.Revision;
	}
	OutState.SourcesThatActivatedGround.Reserve(
		SourcesThatActivatedGround.Num());
	for (const FGuid SourceId : SourcesThatActivatedGround)
	{
		OutState.SourcesThatActivatedGround.Add(SourceId);
	}
	return true;
}

bool AMatterFluxPlayableWorldActor::RestoreSaveState(
	const FMatterFluxWorldSaveState& State,
	FString& OutError)
{
	OutError.Reset();
	if (!HasAuthority() || !MaterialSimulation || IsGenerationInProgress())
	{
		OutError = TEXT("world save restore requires an idle initialized authority world");
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		OutError = TEXT("world save restore requires the Lua content registry");
		return false;
	}
	TSet<FIntPoint> SavedTerrainCells;
	for (const FMatterFluxTerrainHeightOverride& Override
		: State.TerrainHeightOverrides)
	{
		if (!FMath::IsFinite(Override.Height)
			|| Override.Height < TerrainHeightField.BottomZ
			|| SavedTerrainCells.Contains(Override.WorldCell))
		{
			OutError = TEXT(
				"saved terrain overrides are invalid or duplicated");
			return false;
		}
		SavedTerrainCells.Add(Override.WorldCell);
	}

	if (!State.MaterialActiveState.IsEmpty())
	{
		int32 ImportedStep = INDEX_NONE;
		FIntPoint ImportedFocus = FIntPoint::ZeroValue;
		if (!MaterialSimulation->ImportActiveState(
			State.MaterialActiveState,
			ImportedStep,
			ImportedFocus,
			OutError))
		{
			return false;
		}
		ReplicatedMaterialSimulationStep = ImportedStep;
		ReplicatedMaterialSimulationFocus = ImportedFocus;
		// A save already owns an explicitly simulated canonical state. Advancing it
		// merely to satisfy new-world warm-up would mutate player history during
		// loading, so only rebuild its disposable presentation before entry.
		InitialMaterialWarmupStepCount = ImportedStep;
		InitialMaterialWarmupQuietStepCount = InitialMaterialWarmupQuietSteps;
		bInitialMaterialWarmupStepsComplete = true;
		bInitialMaterialWarmupComplete = false;
		// The save contains its own focus set. Force the next authority tick to
		// reconcile it with the currently possessed players.
		MaterialSimulation->RequireFocusReconciliation();
		bMaterialVisualizationDirty = true;
		PublishMaterialSimulationState();
	}
	ReplicatedTerrainHeightOverrides = State.TerrainHeightOverrides;
	ApplyTerrainHeightOverrides(
		ReplicatedTerrainHeightOverrides,
		true);
	ForceNetUpdate();

	TMap<FGuid, FFragment2DSourceStreamingState> NextStreamedStates;
	TMap<FGuid, const FMatterFluxSavedFragmentSourceState*> SavedById;
	for (const FMatterFluxSavedFragmentSourceState& Saved
		: State.FragmentSources)
	{
		if (!Saved.SourceId.IsValid()
			|| SavedById.Contains(Saved.SourceId))
		{
			OutError = TEXT("saved fragment source IDs are invalid or duplicated");
			return false;
		}
		SavedById.Add(Saved.SourceId, &Saved);
		NextStreamedStates.Add(
			Saved.SourceId,
			ToRuntimeFragmentState(Saved));
	}
	TSet<FGuid> NextRemoved;
	for (const FGuid SourceId : State.RemovedFragmentSourceIds)
	{
		if (!SourceId.IsValid())
		{
			OutError = TEXT("saved removed-source list contains an invalid ID");
			return false;
		}
		NextRemoved.Add(SourceId);
	}

	if (State.bHasGroundReactionState)
	{
		const FMatterFluxReactionDefinition* Rule = Registry->Reactions.Find(
			State.GroundReactionState.RuleId);
		if (!Rule)
		{
			OutError = TEXT("saved ground reaction rule no longer exists");
			return false;
		}
		auto RestoredGround = MakeUnique<
			MatterFlux::Reaction::FGroundReactionRuntime>();
		MatterFlux::Reaction::FGroundRuntimeSnapshot Snapshot;
		Snapshot.ReactionState =
			ToRuntimeReactionState(State.GroundReactionState);
		Snapshot.StepAccumulator = State.GroundReactionAccumulator;
		Snapshot.Revision = State.GroundReactionRevision;
		if (!RestoredGround->RestoreState(
			MakeGroundReactionRuntimeSettings(),
			Snapshot,
			*Rule,
			OutError))
		{
			return false;
		}
		GroundReaction = MoveTemp(RestoredGround);
		bGroundReactionVisualDirty = true;
		bGroundReactionVisualNeedsFullRebuild = true;
		PendingGroundReactionVisualCellIndices.Reset();
		DestroyGroundStateChunks();
		InitializeGroundStateChunks();
	}
	SourcesThatActivatedGround.Reset();
	for (const FGuid SourceId : State.SourcesThatActivatedGround)
	{
		if (SourceId.IsValid())
		{
			SourcesThatActivatedGround.Add(SourceId);
		}
	}

	TArray<FGuid> ActiveSourcesToRemove;
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		if (NextRemoved.Contains(Pair.Key))
		{
			if (IsValid(Pair.Value))
			{
				Pair.Value->Destroy();
			}
			ActiveSourcesToRemove.Add(Pair.Key);
		}
	}
	for (const FGuid SourceId : ActiveSourcesToRemove)
	{
		GeneratedFragmentSources.Remove(SourceId);
	}
	StreamedFragmentSourceStates = MoveTemp(NextStreamedStates);
	RemovedFragmentSourceIds = MoveTemp(NextRemoved);
	ActiveSourceReactions.Reset();
	LogicalSourceReactionIndex.Reset();
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		LogicalSourceReactionIndex.ApplySnapshot(
			Pair.Key,
			Pair.Value.bHasReactionState,
			Pair.Value.ReactionState.ActiveMask);
	}
	if (FragmentSourceProxy)
	{
		TArray<FGuid> RestoredSourceIds;
		RestoredSourceIds.Reserve(StreamedFragmentSourceStates.Num());
		for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
			: StreamedFragmentSourceStates)
		{
			const FMatterFluxReactionDefinition* Rule = nullptr;
			FName OutputMaterial = NAME_None;
			FLinearColor OutputColor = FLinearColor::Transparent;
			TArray<uint8> OutputMask;
			if (Pair.Value.bHasReactionState)
			{
				Rule = Registry->Reactions.Find(
					Pair.Value.ReactionState.RuleId);
				const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
					FindFragmentSourceDefinition(Pair.Key);
				if (!Rule || !Source)
				{
					OutError = TEXT("saved logical source reaction rule or source no longer exists");
					return false;
				}
				OutputMask = Pair.Value.ReactionState.OutputMask;
				OutputMaterial = Rule->OutputA;
				OutputColor = FLinearColor(0.08f, 0.07f, 0.06f);
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(Rule->OutputA))
				{
					OutputColor = Material->Color;
				}
			}
			else
			{
				OutputMask.Init(0, Pair.Value.GetRuntimeMask().Num());
			}
			const bool bReactionActive = Rule
				&& Pair.Value.ReactionState.ActiveMask.ContainsByPredicate(
					[](const uint8 Value)
					{
						return Value != 0;
					});
			if (FragmentSourceProxy->ApplySourceState(
				Pair.Key,
				Pair.Value.GetRuntimeMask(),
				OutputMask,
				OutputMaterial,
				OutputColor,
				bReactionActive)
				== EMatterFluxFragmentSourceProxyApplyResult::Invalid)
			{
				OutError = TEXT("saved logical source masks do not match the generated source");
				return false;
			}
			RestoredSourceIds.Add(Pair.Key);
			if (bReactionActive)
			{
				auto Runtime = MakeUnique<
					MatterFlux::Reaction::FSourceReactionRuntime>();
				if (!Runtime->RestoreState(
					MatterFlux::Reaction::FSourceRuntimeSettings(),
					Pair.Value,
					*Rule,
					OutError))
				{
					return false;
				}
				ActiveSourceReactions.Add(Pair.Key, MoveTemp(Runtime));
			}
		}
		if (!PublishFragmentSourceStateBatch(RestoredSourceIds))
		{
			OutError = TEXT("saved fragment source states exceed the replication budget");
			return false;
		}
	}
	bSourceReactionVisualDirty = true;
	VisibleFragmentFocusChunks.Reset();
	RefreshVisibleFragmentSources(true);

	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		AFragment2DSourceActor* SourceActor = Pair.Value;
		const FMatterFluxSavedFragmentSourceState* const* SavedPtr =
			SavedById.Find(Pair.Key);
		if (!IsValid(SourceActor) || !SavedPtr || !*SavedPtr)
		{
			continue;
		}
		const FMatterFluxSavedFragmentSourceState& Saved = **SavedPtr;
		FString RestoreError;
		if (!SourceActor->RestoreStreamingState(
			ToRuntimeFragmentState(Saved),
			RestoreError))
		{
			OutError = FString::Printf(
				TEXT("fragment source %s could not be restored: %s"),
				*Pair.Key.ToString(),
				*RestoreError);
			return false;
		}
		SourceActor->bDetachedFromTerrain = Saved.bDetachedFromTerrain;
		if (Saved.bDetachedFromTerrain)
		{
			SourceActor->SetActorTransform(
				Saved.ActorTransform,
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
		}
		SourceActor->ForceNetUpdate();
	}
	ForceNetUpdate();
	return true;
}

int32 AMatterFluxPlayableWorldActor::GetSimulatedMaterialCount(
	const FName MaterialId) const
{
	return MaterialSimulation
		? MaterialSimulation->CountMaterial(MaterialId)
		: 0;
}

int64 AMatterFluxPlayableWorldActor::GetSimulatedMaterialAmount(
	const FName MaterialId) const
{
	return MaterialSimulation
		? MaterialSimulation->SumMaterialAmount(MaterialId)
		: 0;
}

FGuid AMatterFluxPlayableWorldActor::SpawnAirborneSimulatedMaterial(
	const FName MaterialId,
	const int32 CellCount,
	const TConstArrayView<FVector> WorldPositions,
	const TConstArrayView<FVector> InitialVelocities,
	const float ParticleRadius,
	const float GravityScale,
	const float Lifetime,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !MaterialSimulation
		|| IsGenerationInProgress()
		|| MaterialId.IsNone()
		|| CellCount <= 0)
	{
		return FGuid();
	}
	bool bLiquid = false;
	if (const FMovementMediumDefinition* Medium =
		MaterialMovementMedia.Find(MaterialId))
	{
		bLiquid = Medium->Phase == EMatterFluxMaterialPhase::Liquid;
	}
	const int32 ConservedAmountPerCell = bLiquid
		? FMath::Clamp(
			FMath::RoundToInt(
				255.0f * MaterialSimulationCellSize
					/ FMath::Max(MaterialLiquidColumnHeight, 1.0f)),
			1,
			255)
		: 255;
	return MaterialSimulation->SpawnAirborneParticles(
		MaterialId,
		WorldPositions,
		InitialVelocities,
		FMath::Clamp(CellCount, 1, 4096),
		ConservedAmountPerCell,
		FMath::Clamp(ParticleRadius, 1.0f, 12.0f),
		GravityScale,
		Lifetime,
		EventSeed);
}

void AMatterFluxPlayableWorldActor::GetAirborneSimulatedMaterialParticles(
	const FGuid& BatchId,
	TArray<MatterFlux::Material::FAirborneParticle>& OutParticles) const
{
	OutParticles.Reset();
	if (MaterialSimulation)
	{
		MaterialSimulation->GetAirborneParticlesForBatch(BatchId, OutParticles);
	}
}

bool AMatterFluxPlayableWorldActor::HasAirborneSimulatedMaterialBatch(
	const FGuid& BatchId) const
{
	return MaterialSimulation
		&& MaterialSimulation->HasAirborneParticleBatch(BatchId);
}

int32 AMatterFluxPlayableWorldActor::GetAirborneSimulatedMaterialParticleCount(
	const FName MaterialId) const
{
	return MaterialSimulation
		? MaterialSimulation->CountAirborneParticles(MaterialId)
		: 0;
}

int64 AMatterFluxPlayableWorldActor::GetAirborneSimulatedMaterialAmount(
	const FName MaterialId) const
{
	return MaterialSimulation
		? MaterialSimulation->SumAirborneMaterialAmount(MaterialId)
		: 0;
}

void AMatterFluxPlayableWorldActor::AdvanceAirborneMaterialParticles(
	const float DeltaSeconds)
{
	if (!HasAuthority()
		|| !MaterialSimulation
		|| IsGenerationInProgress()
		|| MaterialSimulation->CountAirborneParticles() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FMatterFluxContentRegistryPtr ActiveRegistry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	const float GravityZ = World->GetGravityZ();
	FBox AirborneBounds(ForceInit);
	float MaximumParticleRadius = 0.0f;
	float MaximumParticleSpeed = 0.0f;
	float MaximumGravityScale = 0.0f;
	const bool bHasCompactAirborneBounds =
		MaterialSimulation->GetAirborneParticleBounds(
			AirborneBounds,
			MaximumParticleRadius,
			MaximumParticleSpeed,
			MaximumGravityScale)
		&& AirborneBounds.GetExtent().GetMax() <= 10000.0f;
	TArray<AFragment2DSourceActor*> FixedSourceCandidates;
	bool bMayHitDynamicPhysics = true;
	if (bHasCompactAirborneBounds)
	{
		const float BroadphaseExpansion = MaximumParticleRadius
			+ MaximumParticleSpeed * FMath::Clamp(DeltaSeconds, 0.0f, 0.10f)
			+ FMath::Abs(GravityZ) * MaximumGravityScale
				* FMath::Square(FMath::Clamp(DeltaSeconds, 0.0f, 0.10f));
		const FBox SweepBounds = AirborneBounds.ExpandBy(BroadphaseExpansion);
		GatherFragmentSourcesInBounds(SweepBounds, FixedSourceCandidates);

		FCollisionObjectQueryParams BroadphaseObjectQuery;
		BroadphaseObjectQuery.AddObjectTypesToQuery(ECC_PhysicsBody);
		FCollisionQueryParams BroadphaseQueryParams(
			SCENE_QUERY_STAT(MatterFluxAirborneMaterialBroadphase),
			false,
			this);
		bMayHitDynamicPhysics = World->OverlapAnyTestByObjectType(
			SweepBounds.GetCenter(),
			FQuat::Identity,
			BroadphaseObjectQuery,
			FCollisionShape::MakeBox(SweepBounds.GetExtent()),
			BroadphaseQueryParams);
	}
	// Dense powder entering a liquid is exchanged into that column in bounded
	// amounts. Aggregate the writes so a 2048-grain spell performs one canonical
	// layer transaction per touched cell rather than thousands of wake/sort calls.
	TMap<FName, TMap<FIntPoint, int32>> PendingSubmergedPowderExchanges;
	// A large powder body can contact the surface almost simultaneously. Its
	// grains are still independent facts, so amortize their canonical handoff
	// across a short settling window instead of performing thousands of stable-
	// surface searches and dirty-neighborhood wakes in one game-thread frame.
	constexpr int32 MaximumPowderSurfaceTransfersPerFrame = 128;
	int32 RemainingPowderSurfaceTransfers =
		MaximumPowderSurfaceTransfersPerFrame;
	constexpr int32 MaximumComplexPowderSurfaceTransfersPerFrame = 8;
	int32 RemainingComplexPowderSurfaceTransfers =
		MaximumComplexPowderSurfaceTransfersPerFrame;
	TSet<FIntPoint> AirbornePowderDestinationCells;
	const int32 Transferred = MaterialSimulation->AdvanceAirborneParticles(
		DeltaSeconds,
		[this,
			World,
			GravityZ,
			ActiveRegistry,
			bHasCompactAirborneBounds,
			bMayHitDynamicPhysics,
			&FixedSourceCandidates,
			&PendingSubmergedPowderExchanges,
			&RemainingPowderSurfaceTransfers,
			&RemainingComplexPowderSurfaceTransfers,
			&AirbornePowderDestinationCells](
			MatterFlux::Material::FAirborneParticle& Particle,
			const float StepSeconds)
		{
			const FVector Start = Particle.WorldPosition;
			Particle.RemainingLifetime -= StepSeconds;
			Particle.Velocity.Z +=
				GravityZ * Particle.GravityScale * StepSeconds;
			const FVector End = Start + Particle.Velocity * StepSeconds;

			bool bHasImpact = false;
			float BestDistanceSquared = MAX_flt;
			FVector ImpactLocation = End;
			AActor* ImpactActor = nullptr;
			FName ImpactContactMaterial = NAME_None;
			const auto ConsiderImpact = [
				&bHasImpact,
				&BestDistanceSquared,
				&ImpactLocation,
				&ImpactActor,
				&ImpactContactMaterial,
				&Start](
					const FVector& Candidate,
					AActor* CandidateActor,
					const FName ContactMaterial = NAME_None)
			{
				const float DistanceSquared = FVector::DistSquared(Start, Candidate);
				if (!bHasImpact || DistanceSquared < BestDistanceSquared)
				{
					bHasImpact = true;
					BestDistanceSquared = DistanceSquared;
					ImpactLocation = Candidate;
					ImpactActor = CandidateActor;
					ImpactContactMaterial = ContactMaterial;
				}
			};

			FVector FixedImpactLocation;
			FVector FixedImpactNormal;
			AFragment2DSourceActor* FixedSource = nullptr;
			const bool bHitFixedSource = bHasCompactAirborneBounds
				? SweepFixedFragmentSourceCandidates(
					FixedSourceCandidates,
					Start,
					End,
					Particle.Radius,
					FixedImpactLocation,
					FixedImpactNormal,
					FixedSource)
				: SweepFixedFragmentSource(
					Start,
					End,
					Particle.Radius,
					FixedImpactLocation,
					FixedImpactNormal,
					FixedSource);
			if (bHitFixedSource)
			{
				ConsiderImpact(FixedImpactLocation, FixedSource);
			}

			FVector MaterialImpactLocation;
			FName ContactMaterial;
			if (SweepSimulatedMaterial(
				Start,
				End,
				Particle.Radius,
				MaterialImpactLocation,
				ContactMaterial))
			{
				const FMovementMediumDefinition* IncomingMedium =
					MaterialMovementMedia.Find(Particle.MaterialId);
				const FMovementMediumDefinition* ContactMedium =
					MaterialMovementMedia.Find(ContactMaterial);
				const bool bDensePowderEntersLiquid = IncomingMedium
					&& IncomingMedium->Phase == EMatterFluxMaterialPhase::Powder
					&& ContactMedium
					&& ContactMedium->Phase == EMatterFluxMaterialPhase::Liquid
					&& IncomingMedium->Density > ContactMedium->Density
					&& (!bHasImpact
						|| FVector::DistSquared(Start, MaterialImpactLocation)
							<= BestDistanceSquared);
				if (bDensePowderEntersLiquid)
				{
					FIntPoint ExchangeCell = FIntPoint::ZeroValue;
					if (TryWorldLocationToCell(
						GetActorTransform(),
						MaterialImpactLocation,
						MaterialSimulationCellSize,
						ExchangeCell))
					{
						TMap<FIntPoint, int32>& ExchangesForMaterial =
							PendingSubmergedPowderExchanges.FindOrAdd(
								Particle.MaterialId);
						int32& PendingAmount =
							ExchangesForMaterial.FindOrAdd(ExchangeCell);
						const int32 ExistingAmount =
							MaterialSimulation->GetMaterialAmountAt(
								ExchangeCell,
								Particle.MaterialId);
						const int32 AvailableAmount = FMath::Max(
							static_cast<int32>(MAX_uint16)
								- ExistingAmount - PendingAmount,
							0);
						// One full grain takes roughly 0.55 seconds to exchange
						// through a liquid column. Thousands of grains therefore
						// settle as a continuous stream instead of one shoreline
						// burst or an instantaneous bottom disk.
						const int32 MaximumExchangeThisStep = FMath::Max(
							FMath::CeilToInt(255.0f * StepSeconds / 0.55f),
							1);
						const int32 ExchangeAmount = FMath::Min3(
							Particle.ConservedMaterialAmount,
							MaximumExchangeThisStep,
							AvailableAmount);
						if (ExchangeAmount > 0)
						{
							const int32 PreviousAmount =
								Particle.ConservedMaterialAmount;
							PendingAmount += ExchangeAmount;
							Particle.ConservedMaterialAmount -= ExchangeAmount;
							if (Particle.ConservedMaterialAmount > 0)
							{
								Particle.Radius *= FMath::Pow(
									static_cast<float>(Particle.ConservedMaterialAmount)
										/ PreviousAmount,
									1.0f / 3.0f);
							}
						}
					}

					const float Resistance = FMath::Max(
						ContactMedium->MovementResistance,
						0.05f);
					const float Damping = FMath::Exp(
						-Resistance * 4.0f * StepSeconds);
					Particle.Velocity.X *= Damping;
					Particle.Velocity.Y *= Damping;
					const float RelativeDensity =
						(IncomingMedium->Density - ContactMedium->Density)
							/ FMath::Max(ContactMedium->Density, 0.01f);
					const float TerminalSinkSpeed = FMath::Clamp(
						120.0f + RelativeDensity * 140.0f,
						120.0f,
						320.0f);
					Particle.Velocity.Z = FMath::Lerp(
						Particle.Velocity.Z,
						-TerminalSinkSpeed,
						1.0f - Damping);
				}
				else
				{
					ConsiderImpact(
						MaterialImpactLocation,
						nullptr,
						ContactMaterial);
				}
			}

			FCollisionObjectQueryParams ObjectQuery;
			ObjectQuery.AddObjectTypesToQuery(ECC_PhysicsBody);
			FCollisionQueryParams QueryParams(
				SCENE_QUERY_STAT(MatterFluxAirborneMaterialParticle),
				false,
				this);
			FHitResult DynamicHit;
			if (bMayHitDynamicPhysics
				&& World->SweepSingleByObjectType(
				DynamicHit,
				Start,
				End,
				FQuat::Identity,
				ObjectQuery,
				FCollisionShape::MakeSphere(Particle.Radius),
				QueryParams))
			{
				ConsiderImpact(DynamicHit.ImpactPoint, DynamicHit.GetActor());
			}

			float TerrainZ = -MAX_flt;
			if (TrySampleTerrainHeightAtWorldLocation(End, TerrainZ)
				&& End.Z <= TerrainZ + Particle.Radius)
			{
				ConsiderImpact(
					FVector(End.X, End.Y, TerrainZ),
					nullptr);
			}
			if (Particle.ConservedMaterialAmount <= 0)
			{
				return true;
			}

			// A spell lifetime limits how long a particle is propelled; it never
			// deletes material. Transfer the conserved particle at its current cell.
			if (!bHasImpact && Particle.RemainingLifetime <= 0.0f)
			{
				bHasImpact = true;
				ImpactLocation = End;
			}
			if (!bHasImpact)
			{
				Particle.WorldPosition = End;
				return false;
			}

			// An airborne material fact meeting a settled material fact is a
			// canonical chemistry event. Handle reactions that leave the settled
			// side unchanged here, before looking for a neighboring deposit cell;
			// otherwise fire touching water could survive simply because the
			// deposition adapter found an empty column beside the contact.
			if (!ImpactContactMaterial.IsNone()
				&& ImpactContactMaterial != Particle.MaterialId
				&& ActiveRegistry.IsValid())
			{
				FIntPoint ContactCell = FIntPoint::ZeroValue;
				TryWorldLocationToCell(
					GetActorTransform(),
					ImpactLocation,
					MaterialSimulationCellSize,
					ContactCell);
				for (const TPair<FName, FMatterFluxReactionDefinition>& Pair :
					ActiveRegistry->Reactions)
				{
					const FMatterFluxReactionDefinition& Rule = Pair.Value;
					if (Rule.Kind
						!= FMatterFluxReactionDefinition::EKind::Contact)
					{
						continue;
					}
					MatterFlux::Reaction::FDeterministicContext Context;
					Context.Seed = Particle.EventSeed;
					Context.Tick = MaterialSimulation->GetLogicalStep();
					Context.FirstCell = ContactCell;
					Context.SecondCell = ContactCell + FIntPoint(1, 0);
					MatterFlux::Reaction::FContactResult ContactResult;
					if (!MatterFlux::Reaction::FMaterialReactionEngine::EvaluateContact(
							Rule,
							ImpactContactMaterial,
							Particle.MaterialId,
							Context,
							ContactResult)
						|| !ContactResult.bReacted
						|| ContactResult.FirstMaterial != ImpactContactMaterial)
					{
						continue;
					}
					if (ContactResult.SecondMaterial.IsNone()
						|| ContactResult.SecondMaterial == TEXT("empty"))
					{
						return true;
					}
					Particle.MaterialId = ContactResult.SecondMaterial;
					break;
				}
			}

			const FMovementMediumDefinition* ImpactMedium =
				MaterialMovementMedia.Find(Particle.MaterialId);
			const bool bIsPowderImpact = ImpactMedium
				&& ImpactMedium->Phase == EMatterFluxMaterialPhase::Powder;
			const bool bHasSurfaceTransferBudget = !bIsPowderImpact
				|| RemainingPowderSurfaceTransfers > 0;
			bool bUsedDirectTerrainPowderTransfer = false;
			int32 Deposited = 0;
			// Fixed-source collision has already identified trees, walls, roofs,
			// and detached supports. A powder grain whose nearest impact is plain
			// terrain or its own settled material must not repeat the expensive
			// authored-source discovery performed by the general impact adapter.
			if (bHasSurfaceTransferBudget
				&& bIsPowderImpact
				&& !IsValid(ImpactActor)
				&& (ImpactContactMaterial.IsNone()
					|| ImpactContactMaterial == Particle.MaterialId))
			{
				FIntPoint ImpactCell = FIntPoint::ZeroValue;
				FIntPoint DestinationCell = FIntPoint::ZeroValue;
				if (TryWorldLocationToCell(
						GetActorTransform(),
						ImpactLocation,
						MaterialSimulationCellSize,
						ImpactCell))
				{
					DestinationCell = ImpactCell;
					const int32 AcceptedAmount =
						MaterialSimulation->AddCellAmount(
							ImpactCell,
							Particle.MaterialId,
							static_cast<uint16>(FMath::Clamp(
								Particle.ConservedMaterialAmount,
								1,
								static_cast<int32>(MAX_uint16))));
					if (AcceptedAmount > 0)
					{
						bUsedDirectTerrainPowderTransfer = true;
						Particle.ConservedMaterialAmount -= AcceptedAmount;
						Particle.CellCount = FMath::Max(
							1,
							FMath::DivideAndRoundUp(
								Particle.ConservedMaterialAmount,
								255));
						AirbornePowderDestinationCells.Add(DestinationCell);
						if (Particle.ConservedMaterialAmount <= 0)
						{
							Deposited = 1;
						}
					}
				}
			}
			if (bHasSurfaceTransferBudget
				&& Deposited <= 0
				&& (!bIsPowderImpact
					|| RemainingComplexPowderSurfaceTransfers > 0))
			{
				int32 AcceptedImpactAmount = 0;
				const int32 RemainingBeforeImpact =
					Particle.ConservedMaterialAmount;
				Deposited = DepositSimulatedMaterialFromImpact(
					ImpactLocation,
					Particle.MaterialId,
					Particle.CellCount,
					ImpactActor,
					Particle.Radius,
					Particle.Radius,
					1,
					RemainingBeforeImpact,
					&AcceptedImpactAmount);
				if (bIsPowderImpact
					&& AcceptedImpactAmount > 0
					&& AcceptedImpactAmount < RemainingBeforeImpact)
				{
					Particle.ConservedMaterialAmount =
						RemainingBeforeImpact - AcceptedImpactAmount;
					Particle.CellCount = FMath::Max(
						1,
						FMath::DivideAndRoundUp(
							Particle.ConservedMaterialAmount,
							255));
					// The accepted fraction is now canonical; keep only the remainder
					// airborne rather than deleting or duplicating the whole particle.
					Deposited = 0;
				}
				if (Deposited > 0 && bIsPowderImpact)
				{
					--RemainingComplexPowderSurfaceTransfers;
				}
			}
			if (Deposited > 0)
			{
				if (bIsPowderImpact)
				{
					--RemainingPowderSurfaceTransfers;
				}
				if (!bUsedDirectTerrainPowderTransfer)
				{
					AFragment2DSourceActor* DirectSource =
						Cast<AFragment2DSourceActor>(ImpactActor);
					const bool bDirectSourceActivated = DirectSource
						&& DirectSource->ApplyMaterialStimulusAtWorldLocation(
								ImpactLocation,
								Particle.MaterialId,
								Particle.EventSeed);
					if (!bDirectSourceActivated)
					{
						ApplyMaterialStimulusAtWorldLocation(
							ImpactLocation,
							Particle.MaterialId,
							Particle.EventSeed,
							Particle.Radius + MaterialSimulationCellSize);
					}
				}
				return true;
			}
			Particle.WorldPosition = ImpactLocation
				+ FVector::UpVector * Particle.Radius * 2.0f;
			Particle.Velocity = FVector::ZeroVector;
			Particle.RemainingLifetime = FMath::Max(
				Particle.RemainingLifetime,
				0.05f);
			return false;
		});
	if (!AirbornePowderDestinationCells.IsEmpty())
	{
		TArray<FIntPoint> DestinationCells;
		DestinationCells.Reserve(AirbornePowderDestinationCells.Num());
		for (const FIntPoint DestinationCell : AirbornePowderDestinationCells)
		{
			DestinationCells.Add(DestinationCell);
		}
		RegisterRecentMaterialWakeCells(DestinationCells);
		WakeRecentVisibleMaterialChunks();
	}
	int32 ExchangedAmount = 0;
	for (const TPair<FName, TMap<FIntPoint, int32>>& MaterialPair
		: PendingSubmergedPowderExchanges)
	{
		for (const TPair<FIntPoint, int32>& CellPair : MaterialPair.Value)
		{
			int32 RemainingAmount = CellPair.Value;
			while (RemainingAmount > 0)
			{
				const uint16 TransactionAmount = static_cast<uint16>(
					FMath::Min(RemainingAmount, static_cast<int32>(MAX_uint16)));
				const int32 Accepted = MaterialSimulation->AddCellAmount(
					CellPair.Key,
					MaterialPair.Key,
					TransactionAmount);
				ExchangedAmount += Accepted;
				RemainingAmount -= Accepted;
				if (Accepted < TransactionAmount)
				{
					ensureMsgf(false,
						TEXT("Submerged powder exchange capacity changed during the airborne transaction"));
					break;
				}
			}
		}
	}
	if (Transferred > 0 || ExchangedAmount > 0)
	{
		bMaterialVisualizationDirty = true;
	}
}

void AMatterFluxPlayableWorldActor::GetSimulatedMaterialCells(
	const FName MaterialId,
	TArray<MatterFlux::Material::FCellSnapshot>& OutCells) const
{
	OutCells.Reset();
	if (!MaterialSimulation || MaterialId.IsNone())
	{
		return;
	}
	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	MaterialSimulation->GetAllCells(Cells);
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		if (Cell.MaterialId == MaterialId)
		{
			OutCells.Add(Cell);
		}
	}
}

bool AMatterFluxPlayableWorldActor::SetSimulatedMaterialAtWorldLocation(
	const FVector& WorldLocation,
	const FName MaterialId)
{
	if (!HasAuthority() || !MaterialSimulation || IsGenerationInProgress())
	{
		return false;
	}
	FIntPoint WorldCell;
	if (!TryWorldLocationToCell(
		GetActorTransform(),
		WorldLocation,
		MaterialSimulationCellSize,
		WorldCell)
		|| !MaterialSimulation->SetCell(WorldCell, MaterialId))
	{
		return false;
	}
	if (!MaterialId.IsNone())
	{
		FPendingMaterialStimulus& Stimulus =
			PendingMaterialStimuli.AddDefaulted_GetRef();
		Stimulus.WorldLocation = WorldLocation;
		Stimulus.WorldCell = WorldCell;
		Stimulus.MaterialId = MaterialId;
		Stimulus.EventSeed = MapSeed
			^ MaterialSimulation->GetLogicalStep()
			^ static_cast<int32>(GetTypeHash(WorldLocation));
	}
	bMaterialVisualizationDirty = true;
	return true;
}

int64 AMatterFluxPlayableWorldActor::RemoveSimulatedMaterialInOrientedBox(
	const FTransform& BoxTransform,
	const FVector& BoxHalfExtent)
{
	if (!HasAuthority()
		|| !MaterialSimulation
		|| IsGenerationInProgress()
		|| !BoxTransform.IsValid()
		|| BoxHalfExtent.ContainsNaN()
		|| BoxHalfExtent.GetMin() <= 0.0f)
	{
		return 0;
	}

	FTransform NormalizedBoxTransform = BoxTransform;
	NormalizedBoxTransform.SetScale3D(FVector::OneVector);
	const FVector BoxAxisX = NormalizedBoxTransform.GetUnitAxis(EAxis::X);
	const FVector BoxAxisY = NormalizedBoxTransform.GetUnitAxis(EAxis::Y);
	const FVector BoxAxisZ = NormalizedBoxTransform.GetUnitAxis(EAxis::Z);
	const auto IntersectsOrientedBox = [
		&NormalizedBoxTransform,
		&BoxHalfExtent,
		&BoxAxisX,
		&BoxAxisY,
		&BoxAxisZ](
			const FVector& WorldCenter,
			const FVector& WorldExtentX,
			const FVector& WorldExtentY,
			const FVector& WorldExtentZ)
	{
		const FVector LocalCenter =
			NormalizedBoxTransform.InverseTransformPosition(WorldCenter);
		const FVector ProjectedExtent(
			FMath::Abs(FVector::DotProduct(WorldExtentX, BoxAxisX))
				+ FMath::Abs(FVector::DotProduct(WorldExtentY, BoxAxisX))
				+ FMath::Abs(FVector::DotProduct(WorldExtentZ, BoxAxisX)),
			FMath::Abs(FVector::DotProduct(WorldExtentX, BoxAxisY))
				+ FMath::Abs(FVector::DotProduct(WorldExtentY, BoxAxisY))
				+ FMath::Abs(FVector::DotProduct(WorldExtentZ, BoxAxisY)),
			FMath::Abs(FVector::DotProduct(WorldExtentX, BoxAxisZ))
				+ FMath::Abs(FVector::DotProduct(WorldExtentY, BoxAxisZ))
				+ FMath::Abs(FVector::DotProduct(WorldExtentZ, BoxAxisZ)));
		return FMath::Abs(LocalCenter.X) <= BoxHalfExtent.X + ProjectedExtent.X
			&& FMath::Abs(LocalCenter.Y) <= BoxHalfExtent.Y + ProjectedExtent.Y
			&& FMath::Abs(LocalCenter.Z) <= BoxHalfExtent.Z + ProjectedExtent.Z;
	};

	const FTransform MaterialWorldTransform = GetActorTransform();
	const FMatterFluxContentRegistryPtr ActiveRegistry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	TArray<MatterFlux::Material::FCellSnapshot> ResidentCells;
	MaterialSimulation->GetResidentCells(ResidentCells);
	TSet<FIntPoint> RemovedCells;
	int64 RemovedAmount = 0;
	for (const MatterFlux::Material::FCellSnapshot& Cell : ResidentCells)
	{
		if (Cell.MaterialId.IsNone() || Cell.Amount == 0)
		{
			continue;
		}
		EMatterFluxMaterialPhase Phase = EMatterFluxMaterialPhase::StaticSolid;
		if (const FMovementMediumDefinition* Medium =
			MaterialMovementMedia.Find(Cell.MaterialId))
		{
			Phase = Medium->Phase;
		}
		else if (ActiveRegistry.IsValid())
		{
			if (const FMatterFluxMaterialDefinition* Definition =
				ActiveRegistry->Materials.Find(Cell.MaterialId))
			{
				Phase = Definition->Phase;
			}
		}
		const MatterFlux::Material::FMaterialContactGeometry Geometry =
			MatterFlux::Material::BuildMaterialContactGeometry(
				Phase,
				MaterialSimulationCellSize,
				MaterialLiquidColumnHeight);
		if (!Geometry.IsValid())
		{
			continue;
		}
		const float AmountScale =
			static_cast<float>(Cell.Amount) / static_cast<float>(MAX_uint8);
		const float ColumnHeight = FMath::Max(
			UE_SMALL_NUMBER,
			Geometry.ColumnHeight * AmountScale);
		const FVector LocalCenter(
			(static_cast<double>(Cell.WorldCell.X) + 0.5)
				* MaterialSimulationCellSize,
			(static_cast<double>(Cell.WorldCell.Y) + 0.5)
				* MaterialSimulationCellSize,
			static_cast<double>(Cell.SupportHeight) + ColumnHeight * 0.5);
		const FVector WorldCenter =
			MaterialWorldTransform.TransformPosition(LocalCenter);
		const FVector WorldExtentX = MaterialWorldTransform.TransformVector(
			FVector(MaterialSimulationCellSize * 0.5f, 0.0f, 0.0f));
		const FVector WorldExtentY = MaterialWorldTransform.TransformVector(
			FVector(0.0f, MaterialSimulationCellSize * 0.5f, 0.0f));
		const FVector WorldExtentZ = MaterialWorldTransform.TransformVector(
			FVector(0.0f, 0.0f, ColumnHeight * 0.5f));
		if (!IntersectsOrientedBox(
			WorldCenter,
			WorldExtentX,
			WorldExtentY,
			WorldExtentZ))
		{
			continue;
		}
		RemovedCells.Add(Cell.WorldCell);
		RemovedAmount += Cell.Amount;
	}

	for (const FIntPoint& WorldCell : RemovedCells)
	{
		MaterialSimulation->SetCell(WorldCell, NAME_None);
	}
	RemovedAmount += MaterialSimulation->RemoveAirborneParticles(
		[&NormalizedBoxTransform, &BoxHalfExtent](
			const MatterFlux::Material::FAirborneParticle& Particle)
		{
			const FVector LocalPosition =
				NormalizedBoxTransform.InverseTransformPosition(
					Particle.WorldPosition);
			const float Radius = FMath::Max(0.0f, Particle.Radius);
			return FMath::Abs(LocalPosition.X) <= BoxHalfExtent.X + Radius
				&& FMath::Abs(LocalPosition.Y) <= BoxHalfExtent.Y + Radius
				&& FMath::Abs(LocalPosition.Z) <= BoxHalfExtent.Z + Radius;
		});
	if (RemovedAmount <= 0)
	{
		return 0;
	}
	PendingMaterialStimuli.RemoveAll(
		[&RemovedCells](const FPendingMaterialStimulus& Stimulus)
		{
			return RemovedCells.Contains(Stimulus.WorldCell);
		});
	bMaterialVisualizationDirty = true;
	return RemovedAmount;
}

bool AMatterFluxPlayableWorldActor::SweepSimulatedMaterial(
	const FVector& Start,
	const FVector& End,
	const float Radius,
	FVector& OutImpactLocation,
	FName& OutMaterialId) const
{
	OutImpactLocation = FVector::ZeroVector;
	OutMaterialId = NAME_None;
	if (!MaterialSimulation
		|| Start.ContainsNaN()
		|| End.ContainsNaN()
		|| !FMath::IsFinite(Radius)
		|| Radius < 0.0f
		|| MaterialSimulationCellSize <= UE_SMALL_NUMBER)
	{
		return false;
	}

	const FTransform WorldTransform = GetActorTransform();
	const FVector AbsoluteScale = WorldTransform.GetScale3D().GetAbs();
	const float MinimumScale = AbsoluteScale.GetMin();
	if (!WorldTransform.IsValid() || MinimumScale <= UE_SMALL_NUMBER)
	{
		return false;
	}
	const FVector LocalStart = WorldTransform.InverseTransformPosition(Start);
	const FVector LocalEnd = WorldTransform.InverseTransformPosition(End);
	const FVector LocalDirection = LocalEnd - LocalStart;
	const float LocalRadius = Radius / MinimumScale;
	const FVector LocalMinimum = LocalStart.ComponentMin(LocalEnd)
		- FVector(LocalRadius);
	const FVector LocalMaximum = LocalStart.ComponentMax(LocalEnd)
		+ FVector(LocalRadius);
	const int32 MinimumCellX = FMath::FloorToInt(
		LocalMinimum.X / MaterialSimulationCellSize);
	const int32 MaximumCellX = FMath::FloorToInt(
		LocalMaximum.X / MaterialSimulationCellSize);
	const int32 MinimumCellY = FMath::FloorToInt(
		LocalMinimum.Y / MaterialSimulationCellSize);
	const int32 MaximumCellY = FMath::FloorToInt(
		LocalMaximum.Y / MaterialSimulationCellSize);

	const auto FindSegmentEntry = [
		&LocalStart,
		&LocalDirection](const FBox& Box, double& OutEntry)
	{
		double Entry = 0.0;
		double Exit = 1.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Origin = LocalStart[Axis];
			const double Direction = LocalDirection[Axis];
			if (FMath::Abs(Direction) <= UE_SMALL_NUMBER)
			{
				if (Origin < Box.Min[Axis] || Origin > Box.Max[Axis])
				{
					return false;
				}
				continue;
			}
			double First = (Box.Min[Axis] - Origin) / Direction;
			double Last = (Box.Max[Axis] - Origin) / Direction;
			if (First > Last)
			{
				Swap(First, Last);
			}
			Entry = FMath::Max(Entry, First);
			Exit = FMath::Min(Exit, Last);
			if (Entry > Exit)
			{
				return false;
			}
		}
		OutEntry = Entry;
		return Exit >= 0.0 && Entry <= 1.0;
	};

	double BestEntry = TNumericLimits<double>::Max();
	FIntPoint BestCell(MAX_int32, MAX_int32);
	for (int32 CellY = MinimumCellY; CellY <= MaximumCellY; ++CellY)
	{
		for (int32 CellX = MinimumCellX; CellX <= MaximumCellX; ++CellX)
		{
			MatterFlux::Material::FCellSnapshot Snapshot;
			if (!MaterialSimulation->TryGetCellSnapshot(
				FIntPoint(CellX, CellY), Snapshot)
				|| Snapshot.MaterialId.IsNone()
				|| Snapshot.Amount == 0)
			{
				continue;
			}

			EMatterFluxMaterialPhase Phase =
				EMatterFluxMaterialPhase::StaticSolid;
			if (const FMovementMediumDefinition* Medium =
				MaterialMovementMedia.Find(Snapshot.MaterialId))
			{
				Phase = Medium->Phase;
			}
			else if (IMatterFluxScriptRuntime::IsAvailable())
			{
				const FMatterFluxContentRegistryPtr Registry =
					IMatterFluxScriptRuntime::Get().GetActiveRegistry();
				if (Registry.IsValid())
				{
					if (const FMatterFluxMaterialDefinition* Definition =
						Registry->Materials.Find(Snapshot.MaterialId))
					{
						Phase = Definition->Phase;
					}
				}
			}
			// Gas cells can react through the material simulation, but they do
			// not own a blocking volume. Treating their visualization column as
			// solid made projectiles resolve inside smoke before reaching the
			// physical tree, object, powder, or liquid behind it.
			if (Phase == EMatterFluxMaterialPhase::Gas)
			{
				continue;
			}
			float ColumnHeight = MaterialSimulationCellSize;
			if (Phase == EMatterFluxMaterialPhase::Liquid)
			{
				ColumnHeight = MaterialLiquidColumnHeight
					* (static_cast<float>(Snapshot.Amount) / 255.0f);
			}
			else if (Phase == EMatterFluxMaterialPhase::Powder)
			{
				ColumnHeight =
					MatterFlux::Material::SurfacePowderFullColumnHeight
					* (static_cast<float>(Snapshot.Amount) / 255.0f);
			}
			ColumnHeight = FMath::Max(ColumnHeight, UE_SMALL_NUMBER);
			const FVector CellMinimum(
				static_cast<double>(CellX) * MaterialSimulationCellSize,
				static_cast<double>(CellY) * MaterialSimulationCellSize,
				static_cast<double>(Snapshot.SupportHeight));
			const FBox ExpandedColumn(
				CellMinimum - FVector(LocalRadius),
				CellMinimum + FVector(
					MaterialSimulationCellSize + LocalRadius,
					MaterialSimulationCellSize + LocalRadius,
					ColumnHeight + LocalRadius));
			double Entry = 0.0;
			const FIntPoint Cell(CellX, CellY);
			if (!FindSegmentEntry(ExpandedColumn, Entry)
				|| Entry > BestEntry
				|| (FMath::IsNearlyEqual(Entry, BestEntry)
					&& (Cell.Y > BestCell.Y
						|| (Cell.Y == BestCell.Y && Cell.X >= BestCell.X))))
			{
				continue;
			}
			BestEntry = Entry;
			BestCell = Cell;
			OutMaterialId = Snapshot.MaterialId;
		}
	}
	if (OutMaterialId.IsNone())
	{
		return false;
	}
	OutImpactLocation = WorldTransform.TransformPosition(
		LocalStart + LocalDirection * BestEntry);
	return true;
}

int32 AMatterFluxPlayableWorldActor::DepositSimulatedMaterialAtWorldLocation(
	const FVector& WorldLocation,
	const FName MaterialId,
	const int32 CellCount)
{
	return DepositSimulatedMaterialFromImpact(
		WorldLocation,
		MaterialId,
		CellCount,
		nullptr);
}

int32 AMatterFluxPlayableWorldActor::DepositSimulatedMaterialFromImpact(
	const FVector& WorldLocation,
	const FName MaterialId,
	const int32 CellCount,
	AActor* ImpactActor,
	const float ImpactRadius,
	const float SupportSearchRadius,
	const int32 PreferredPowderColumnLayers,
	const int32 ExplicitMaterialAmount,
	int32* OutAcceptedMaterialAmount)
{
	if (OutAcceptedMaterialAmount)
	{
		*OutAcceptedMaterialAmount = 0;
	}
	if (!HasAuthority()
		|| !MaterialSimulation
		|| IsGenerationInProgress()
		|| MaterialId.IsNone()
		|| CellCount <= 0)
	{
		return 0;
	}

	FIntPoint CenterCell;
	if (!TryWorldLocationToCell(
		GetActorTransform(),
		WorldLocation,
		MaterialSimulationCellSize,
		CenterCell))
	{
		return 0;
	}

	// Preserve each liquid voxel's volume, but inject repeated impacts into a
	// compact set of columns. The hydraulic solver, rather than the deposition
	// adapter, owns downhill spreading and shoreline shape. Pre-flattening one
	// voxel per horizontal cell authored a rigid disk whose discontinuous terrain
	// supports rendered as upright panels beside walls and cliffs.
	// One liquid voxel is one simulation-cell layer (8 cm here), not one full
	// 128 cm liquid column; treating it as a full column inflated spell volume
	// sixteen-fold and produced the tall block seen in capture.
	// Powders likewise enter one impact column and let the granular solver build
	// an angle-of-repose pile from conserved volume.
	static constexpr int32 MaximumSearchRadius = 32;
	static const TArray<FIntPoint> CandidateOffsets = []
	{
		TArray<FIntPoint> Result;
		Result.Reserve(FMath::Square(MaximumSearchRadius * 2 + 1));
		for (int32 Y = -MaximumSearchRadius; Y <= MaximumSearchRadius; ++Y)
		{
			for (int32 X = -MaximumSearchRadius; X <= MaximumSearchRadius; ++X)
			{
				Result.Add(FIntPoint(X, Y));
			}
		}
		Result.Sort([](const FIntPoint Left, const FIntPoint Right)
		{
			const int32 LeftDistance = Left.X * Left.X + Left.Y * Left.Y;
			const int32 RightDistance = Right.X * Right.X + Right.Y * Right.Y;
			return LeftDistance != RightDistance
				? LeftDistance < RightDistance
				: Left.Y != Right.Y
					? Left.Y < Right.Y
					: Left.X < Right.X;
		});
		return Result;
	}();

	bool bLiquid = false;
	bool bPowder = false;
	// The simulation already cached these physical facts when the world was
	// initialized. Projectile impacts can arrive before the script-runtime
	// registry is available to a new gameplay world, so use the simulation's
	// canonical cache first instead of misclassifying early sand/water as solid.
	if (const FMovementMediumDefinition* Medium =
		MaterialMovementMedia.Find(MaterialId))
	{
		bLiquid = Medium->Phase == EMatterFluxMaterialPhase::Liquid;
		bPowder = Medium->Phase == EMatterFluxMaterialPhase::Powder;
	}
	else if (IMatterFluxScriptRuntime::IsAvailable())
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::Get().GetActiveRegistry();
		if (Registry.IsValid())
		{
			if (const FMatterFluxMaterialDefinition* Material =
				Registry->Materials.Find(MaterialId))
			{
				bLiquid =
					Material->Phase == EMatterFluxMaterialPhase::Liquid;
				bPowder =
					Material->Phase == EMatterFluxMaterialPhase::Powder;
			}
		}
	}
	const bool bFlowMaterial = bPowder || bLiquid;
	constexpr int32 FullCellAmount = 255;
	const int32 LiquidVoxelAmount = FMath::Clamp(
		FMath::RoundToInt(
			FullCellAmount * MaterialSimulationCellSize
				/ FMath::Max(MaterialLiquidColumnHeight, 1.0f)),
		1,
		FullCellAmount);
	const int32 RequestedMaterialAmount = ExplicitMaterialAmount > 0
		? FMath::Clamp(ExplicitMaterialAmount, 1, MAX_uint16)
		: FMath::Clamp(CellCount, 1, 4096)
			* (bLiquid ? LiquidVoxelAmount : FullCellAmount);
	const int32 PowderColumnLayers = PreferredPowderColumnLayers > 0
		? FMath::Clamp(PreferredPowderColumnLayers, 1, 64)
		: ImpactRadius > UE_SMALL_NUMBER
			? FMath::Clamp(FMath::CeilToInt(
				ImpactRadius * 2.0f
					/ MatterFlux::Material::SurfacePowderFullColumnHeight), 1, 64)
			: MAX_uint16 / FullCellAmount;
	const int32 TargetAmountPerCell = bLiquid
		? FullCellAmount
		: bPowder
			? PowderColumnLayers * FullCellAmount
			: FullCellAmount;
	int32 RemainingMaterialAmount = RequestedMaterialAmount;
	int32 DepositedCellCount = 0;
	TArray<AFragment2DSourceActor*> AuthoredImpactSources;
	if (IsValid(ImpactActor))
	{
		if (AFragment2DSourceActor* DirectSource =
			Cast<AFragment2DSourceActor>(ImpactActor))
		{
			AuthoredImpactSources.Add(DirectSource);
		}
		else
		{
			TArray<AFragment2DSourceActor*> ContactCandidates;
			GatherFragmentSourcesInBounds(
				FBox::BuildAABB(
					WorldLocation,
					FVector(MaterialSimulationCellSize * 4.0f)),
				ContactCandidates);
			for (AFragment2DSourceActor* Candidate : ContactCandidates)
			{
				if (IsValid(Candidate)
					&& (Candidate == ImpactActor
						|| Candidate->GetOwner() == ImpactActor))
				{
					AuthoredImpactSources.AddUnique(Candidate);
				}
			}
		}
	}
	if (bFlowMaterial)
	{
		TArray<AFragment2DSourceActor*> ContactCandidates;
		const float SupportQueryRadius = FMath::Clamp(
			FMath::Max(ImpactRadius, SupportSearchRadius) > UE_SMALL_NUMBER
				? FMath::Max(ImpactRadius, SupportSearchRadius)
					+ MaterialSimulationCellSize * 2.0f
				: MaterialSimulationCellSize * 4.0f,
			MaterialSimulationCellSize * 2.0f,
			300.0f);
		// The projectile body can pass through an authored proxy before its flow
		// payload is committed at the terrain impact. Search the incoming body's
		// vertical footprint as well as its horizontal impact footprint so a large
		// sand/water body settles on a tree instead of teleporting beneath it.
		const float SupportQueryHeight = FMath::Clamp(
			FMath::Max(
				SupportQueryRadius,
				FMath::Max(ImpactRadius, SupportSearchRadius) * 4.0f),
			MaterialSimulationCellSize * 4.0f,
			600.0f);
		GatherFragmentSourcesInBounds(
			FBox::BuildAABB(
				WorldLocation,
				FVector(
					SupportQueryRadius,
					SupportQueryRadius,
					SupportQueryHeight)),
			ContactCandidates);
		for (AFragment2DSourceActor* Candidate : ContactCandidates)
		{
			if (IsValid(Candidate) && !Candidate->bBroken)
			{
				AuthoredImpactSources.AddUnique(Candidate);
			}
		}

		const int32 SupportRadiusCells = FMath::Clamp(
			FMath::CeilToInt(SupportQueryRadius / MaterialSimulationCellSize),
			1,
			48);
		const float TraceTopZ = WorldLocation.Z + SupportQueryHeight;
		const float TraceBottomZ = WorldLocation.Z - SupportQueryHeight;
		for (int32 Y = -SupportRadiusCells; Y <= SupportRadiusCells; ++Y)
		{
			for (int32 X = -SupportRadiusCells; X <= SupportRadiusCells; ++X)
			{
				if (X * X + Y * Y
					> SupportRadiusCells * SupportRadiusCells)
				{
					continue;
				}
				const FIntPoint SupportCell = CenterCell + FIntPoint(X, Y);
				const FVector LocalCellCenter(
					(static_cast<double>(SupportCell.X) + 0.5)
						* MaterialSimulationCellSize,
					(static_cast<double>(SupportCell.Y) + 0.5)
						* MaterialSimulationCellSize,
					0.0);
				const FVector CellWorld = GetActorTransform().TransformPosition(
					LocalCellCenter);
				const FVector TraceStart(CellWorld.X, CellWorld.Y, TraceTopZ);
				const FVector TraceEnd(CellWorld.X, CellWorld.Y, TraceBottomZ);
				float HighestSupportZ = -MAX_flt;
				AFragment2DSourceActor* HighestSupportSource = nullptr;
				for (AFragment2DSourceActor* Source : AuthoredImpactSources)
				{
					if (!IsValid(Source) || Source->bBroken)
					{
						continue;
					}
					const FBox Bounds = Source->GetActiveWorldBounds().IsValid
						? Source->GetActiveWorldBounds()
						: Source->GetCanonicalWorldBounds();
					if (!Bounds.IsValid
						|| CellWorld.X < Bounds.Min.X - MaterialSimulationCellSize * 0.5f
						|| CellWorld.X > Bounds.Max.X + MaterialSimulationCellSize * 0.5f
						|| CellWorld.Y < Bounds.Min.Y - MaterialSimulationCellSize * 0.5f
						|| CellWorld.Y > Bounds.Max.Y + MaterialSimulationCellSize * 0.5f
						|| Bounds.Max.Z < TraceBottomZ
						|| Bounds.Min.Z > TraceTopZ)
					{
						continue;
					}
					FVector SupportLocation;
					FVector SupportNormal;
					if (Source->SweepRuntimeMask(
						TraceStart,
						TraceEnd,
						0.0f,
						SupportLocation,
						SupportNormal))
					{
						if (SupportLocation.Z > HighestSupportZ)
						{
							HighestSupportZ = SupportLocation.Z;
							HighestSupportSource = Source;
						}
					}
				}
				float TerrainZ = -MAX_flt;
				TrySampleTerrainHeightAtWorldLocation(CellWorld, TerrainZ);
				if (!IsValid(HighestSupportSource)
					|| HighestSupportZ
						<= TerrainZ + MaterialSimulationCellSize * 0.5f)
				{
					continue;
				}
				const FVector LocalSupport = GetActorTransform()
					.InverseTransformPosition(FVector(
						CellWorld.X, CellWorld.Y, HighestSupportZ + 1.0f));
				if (MaterialSimulation->SetExternalSupportHeight(
						SupportCell, FMath::RoundToInt(LocalSupport.Z)))
				{
					FExternalMaterialSupportState& SupportState =
						ExternalMaterialSupportCells.FindOrAdd(SupportCell);
					SupportState.MaterialId = MaterialId;
					SupportState.FixedSource = HighestSupportSource;
				}
			}
		}
	}
	if (bPowder)
	{
		// Collision only hands conserved powder to the cell it actually reached.
		// The ordinary fixed-step granular solver owns every later diagonal slide
		// and repose-angle transfer. Searching for a final stable destination here
		// teleports grains across the surface and flattens large payloads into a pad.
		while (RemainingMaterialAmount > 0)
		{
			const int32 RequestedPacketAmount = FMath::Min(
				RemainingMaterialAmount,
				FullCellAmount);
			FIntPoint DestinationCell = CenterCell;
			const int32 AcceptedAmount =
				MaterialSimulation->AddCellAmount(
					CenterCell,
					MaterialId,
					static_cast<uint16>(RequestedPacketAmount));
			if (AcceptedAmount <= 0)
			{
				break;
			}

			RegisterRecentMaterialWakeCells(
				MakeArrayView(&DestinationCell, 1));
			WakeRecentVisibleMaterialChunks();
			const FVector DestinationLocal(
				(static_cast<double>(DestinationCell.X) + 0.5)
					* MaterialSimulationCellSize,
				(static_cast<double>(DestinationCell.Y) + 0.5)
					* MaterialSimulationCellSize,
				GetActorTransform().InverseTransformPosition(WorldLocation).Z);
			FPendingMaterialStimulus& Stimulus =
				PendingMaterialStimuli.AddDefaulted_GetRef();
			Stimulus.WorldLocation =
				GetActorTransform().TransformPosition(DestinationLocal);
			Stimulus.WorldCell = DestinationCell;
			for (AFragment2DSourceActor* AuthoredSource : AuthoredImpactSources)
			{
				Stimulus.AuthoredSources.Add(AuthoredSource);
			}
			Stimulus.MaterialId = MaterialId;
			Stimulus.EventSeed = MapSeed
				^ MaterialSimulation->GetLogicalStep()
				^ static_cast<int32>(GetTypeHash(DestinationCell));
			++DepositedCellCount;
			RemainingMaterialAmount -= AcceptedAmount;
		}
		if (OutAcceptedMaterialAmount)
		{
			*OutAcceptedMaterialAmount =
				RequestedMaterialAmount - RemainingMaterialAmount;
		}
		if (DepositedCellCount > 0)
		{
			bMaterialVisualizationDirty = true;
			return DepositedCellCount;
		}
		// Powder has exactly one canonical handoff: a stable falling-sand landing.
		// Never fall through to the legacy raw column writer when no stable cell was
		// found; the airborne particle must remain in motion instead.
		return 0;
	}
	TArray<FIntPoint> SupportedCandidateOffsets;
	const TArray<FIntPoint>* OrderedCandidateOffsets = &CandidateOffsets;
	// Explicit powder-column capacity is used by independently swept falling
	// grains.  Their impact cell is already authoritative, so do not teleport a
	// grain to some other supported column merely because a tree exists within
	// the broader support-query radius.  Bulk/legacy deposits still prefer
	// discovered supports as before.
	if (bFlowMaterial
		&& PreferredPowderColumnLayers <= 0
		&& !ExternalMaterialSupportCells.IsEmpty())
	{
		SupportedCandidateOffsets = CandidateOffsets;
		SupportedCandidateOffsets.Sort([this, CenterCell](
			const FIntPoint Left,
			const FIntPoint Right)
		{
			const bool bLeftSupported =
				ExternalMaterialSupportCells.Contains(CenterCell + Left);
			const bool bRightSupported =
				ExternalMaterialSupportCells.Contains(CenterCell + Right);
			if (bLeftSupported != bRightSupported)
			{
				return bLeftSupported;
			}
			const int32 LeftDistance = Left.X * Left.X + Left.Y * Left.Y;
			const int32 RightDistance = Right.X * Right.X + Right.Y * Right.Y;
			return LeftDistance != RightDistance
				? LeftDistance < RightDistance
				: Left.Y != Right.Y
					? Left.Y < Right.Y
					: Left.X < Right.X;
		});
		OrderedCandidateOffsets = &SupportedCandidateOffsets;
	}
	for (const FIntPoint Offset : *OrderedCandidateOffsets)
	{
		const FIntPoint CandidateCell = CenterCell + Offset;
		const FName ExistingMaterial =
			MaterialSimulation->GetMaterialAt(CandidateCell);
		if (!ExistingMaterial.IsNone()
			&& ExistingMaterial != MaterialId
			&& !bFlowMaterial)
		{
			continue;
		}
		const int32 ExistingAmount =
			MaterialSimulation->GetMaterialAmountAt(
				CandidateCell, MaterialId);
		// External fixed-object supports (leaves, roofs, detached-item tops)
		// may initially accept one powder layer, but never a payload-height
		// column.  The canonical powder solver retains its configured uneven
		// thin coating and routes the rest toward lower neighboring supports.
		// This is a phase/support rule shared by every powder material; the
		// deposit adapter does not know which object or spell produced it.
		const int32 CandidateTargetAmount = bPowder
			&& ExternalMaterialSupportCells.Contains(CandidateCell)
			? FMath::Min(TargetAmountPerCell, FullCellAmount)
			: TargetAmountPerCell;
		const int32 TransferAmount = FMath::Min(
			RemainingMaterialAmount,
			FMath::Max(CandidateTargetAmount - ExistingAmount, 0));
		if (TransferAmount <= 0)
		{
			continue;
		}

		const FVector CandidateLocation = WorldLocation
			+ GetActorTransform().TransformVectorNoScale(FVector(
				static_cast<float>(Offset.X) * MaterialSimulationCellSize,
				static_cast<float>(Offset.Y) * MaterialSimulationCellSize,
				0.0f));
		RegisterRecentMaterialWakeCells(
			MakeArrayView(&CandidateCell, 1));
		WakeRecentVisibleMaterialChunks();
		const int32 AcceptedAmount = bFlowMaterial
			? MaterialSimulation->AddCellAmount(
				CandidateCell,
				MaterialId,
				static_cast<uint16>(TransferAmount))
			: MaterialSimulation->SetCellAmount(
				CandidateCell,
				MaterialId,
				static_cast<uint16>(ExistingAmount + TransferAmount))
				? TransferAmount
				: 0;
		if (AcceptedAmount > 0)
		{
			FPendingMaterialStimulus& Stimulus =
				PendingMaterialStimuli.AddDefaulted_GetRef();
			Stimulus.WorldLocation = CandidateLocation;
			Stimulus.WorldCell = CandidateCell;
			for (AFragment2DSourceActor* AuthoredSource : AuthoredImpactSources)
			{
				Stimulus.AuthoredSources.Add(AuthoredSource);
			}
			Stimulus.MaterialId = MaterialId;
			Stimulus.EventSeed = MapSeed
				^ MaterialSimulation->GetLogicalStep()
				^ static_cast<int32>(GetTypeHash(CandidateLocation));
			++DepositedCellCount;
			RemainingMaterialAmount -= AcceptedAmount;
			if (RemainingMaterialAmount <= 0)
			{
				break;
			}
		}
	}
	bMaterialVisualizationDirty |= DepositedCellCount > 0;
	return DepositedCellCount;
}

void AMatterFluxPlayableWorldActor::PruneExternalMaterialSupports()
{
	if (!MaterialSimulation || ExternalMaterialSupportCells.IsEmpty())
	{
		return;
	}
	TArray<FIntPoint> ReleasedCells;
	for (const TPair<FIntPoint, FExternalMaterialSupportState>& Pair :
		ExternalMaterialSupportCells)
	{
		const AFragment2DSourceActor* FixedSource = Pair.Value.FixedSource.Get();
		if (!IsValid(FixedSource)
			|| FixedSource->bBroken
			|| MaterialSimulation->GetMaterialAmountAt(
				Pair.Key,
				Pair.Value.MaterialId) == 0)
		{
			ReleasedCells.Add(Pair.Key);
		}
	}
	for (const FIntPoint Cell : ReleasedCells)
	{
		MaterialSimulation->ClearExternalSupportHeight(Cell);
		ExternalMaterialSupportCells.Remove(Cell);
	}
	bMaterialVisualizationDirty |= !ReleasedCells.IsEmpty();
}

int64 AMatterFluxPlayableWorldActor::GetExternalMaterialSupportedAmount(
	const FName MaterialId) const
{
	if (!MaterialSimulation || MaterialId.IsNone())
	{
		return 0;
	}
	int64 Amount = 0;
	for (const TPair<FIntPoint, FExternalMaterialSupportState>& Pair :
		ExternalMaterialSupportCells)
	{
		if (Pair.Value.MaterialId != MaterialId)
		{
			continue;
		}
		Amount += MaterialSimulation->GetMaterialAmountAt(
			Pair.Key,
			MaterialId);
	}
	return Amount;
}

bool AMatterFluxPlayableWorldActor::TrySampleTerrainHeightAtWorldLocation(
	const FVector& WorldLocation,
	float& OutWorldHeight) const
{
	if (!TerrainHeightField.IsValid())
	{
		return false;
	}
	const FVector Local = GetActorTransform().InverseTransformPosition(
		WorldLocation);
	const int64 FirstWorldCellX = FMath::FloorToInt64(
		TerrainHeightField.FirstCellCenter.X
			/ TerrainHeightField.CellSize);
	const int64 FirstWorldCellY = FMath::FloorToInt64(
		TerrainHeightField.FirstCellCenter.Y
			/ TerrainHeightField.CellSize);
	const int64 CellX = FirstWorldCellX + FMath::RoundToInt64(
		(Local.X - TerrainHeightField.FirstCellCenter.X)
			/ TerrainHeightField.CellSize);
	const int64 CellY = FirstWorldCellY + FMath::RoundToInt64(
		(Local.Y - TerrainHeightField.FirstCellCenter.Y)
			/ TerrainHeightField.CellSize);
	float LocalHeight = 0.0f;
	uint8 ColorBand = 0;
	if (!TerrainHeightField.TrySampleWorldCell(
		CellX, CellY, LocalHeight, ColorBand))
	{
		return false;
	}
	OutWorldHeight = GetActorTransform().TransformPosition(
		FVector(Local.X, Local.Y, LocalHeight)).Z;
	return true;
}

bool AMatterFluxPlayableWorldActor::TryResolveTerrainSpawnLocation(
	const FVector& RequestedWorldLocation,
	const float HorizontalRadius,
	const float CapsuleHalfHeight,
	const float Clearance,
	FVector& OutWorldLocation) const
{
	if (!FMath::IsFinite(RequestedWorldLocation.X)
		|| !FMath::IsFinite(RequestedWorldLocation.Y)
		|| !FMath::IsFinite(RequestedWorldLocation.Z)
		|| !FMath::IsFinite(HorizontalRadius)
		|| !FMath::IsFinite(CapsuleHalfHeight)
		|| !FMath::IsFinite(Clearance)
		|| HorizontalRadius < 0.0f
		|| CapsuleHalfHeight <= 0.0f
		|| Clearance < 0.0f)
	{
		return false;
	}

	const FTransform WorldTransform = GetActorTransform();
	const FVector LocalRequest =
		WorldTransform.InverseTransformPosition(RequestedWorldLocation);
	const FVector WorldScale = WorldTransform.GetScale3D().GetAbs();
	const float MinimumHorizontalScale = FMath::Min(WorldScale.X, WorldScale.Y);
	if (!FMath::IsFinite(MinimumHorizontalScale)
		|| MinimumHorizontalScale <= UE_SMALL_NUMBER)
	{
		return false;
	}

	if (IsCustomMapActive() && !TerrainHeightField.IsValid())
	{
		const double LocalRadius = HorizontalRadius / MinimumHorizontalScale;
		float MaximumWorldHeight = -TNumericLimits<float>::Max();
		bool bFoundCollisionFloor = false;
		for (const MatterFlux::Material::FCustomMapSceneBox& Box
			: ActiveCustomMapScene.Boxes)
		{
			if (!Box.bCollision)
			{
				continue;
			}
			const FVector Extent = Box.Size.GetAbs() * 0.5f;
			if (FMath::Abs(LocalRequest.X - Box.Center.X)
					> Extent.X + LocalRadius
				|| FMath::Abs(LocalRequest.Y - Box.Center.Y)
					> Extent.Y + LocalRadius)
			{
				continue;
			}
			const float WorldHeight = WorldTransform.TransformPosition(
				FVector(LocalRequest.X, LocalRequest.Y,
					Box.Center.Z + Extent.Z)).Z;
			if (FMath::IsFinite(WorldHeight))
			{
				MaximumWorldHeight = FMath::Max(
					MaximumWorldHeight, WorldHeight);
				bFoundCollisionFloor = true;
			}
		}
		if (!bFoundCollisionFloor)
		{
			return false;
		}
		OutWorldLocation = RequestedWorldLocation;
		OutWorldLocation.Z =
			MaximumWorldHeight + CapsuleHalfHeight + Clearance;
		return FMath::IsFinite(OutWorldLocation.X)
			&& FMath::IsFinite(OutWorldLocation.Y)
			&& FMath::IsFinite(OutWorldLocation.Z);
	}

	if (!TerrainHeightField.IsValid())
	{
		return false;
	}

	// Terrain collision vertices average a two-cell-stride neighborhood. Scan
	// two extra cells around the capsule so no averaged triangle can rise above
	// the height used to place its bottom, including at streamed chunk seams.
	const double LocalRadius = HorizontalRadius / MinimumHorizontalScale
		+ TerrainHeightField.CellSize * 2.0;
	const int64 FirstWorldCellX = FMath::FloorToInt64(
		TerrainHeightField.FirstCellCenter.X / TerrainHeightField.CellSize);
	const int64 FirstWorldCellY = FMath::FloorToInt64(
		TerrainHeightField.FirstCellCenter.Y / TerrainHeightField.CellSize);
	const int64 MinimumCellX = FirstWorldCellX + FMath::FloorToInt64(
		(LocalRequest.X - LocalRadius
			- TerrainHeightField.FirstCellCenter.X)
		/ TerrainHeightField.CellSize);
	const int64 MaximumCellX = FirstWorldCellX + FMath::CeilToInt64(
		(LocalRequest.X + LocalRadius
			- TerrainHeightField.FirstCellCenter.X)
		/ TerrainHeightField.CellSize);
	const int64 MinimumCellY = FirstWorldCellY + FMath::FloorToInt64(
		(LocalRequest.Y - LocalRadius
			- TerrainHeightField.FirstCellCenter.Y)
		/ TerrainHeightField.CellSize);
	const int64 MaximumCellY = FirstWorldCellY + FMath::CeilToInt64(
		(LocalRequest.Y + LocalRadius
			- TerrainHeightField.FirstCellCenter.Y)
		/ TerrainHeightField.CellSize);

	float MaximumWorldHeight = -TNumericLimits<float>::Max();
	bool bSampledTerrain = false;
	for (int64 CellY = MinimumCellY; CellY <= MaximumCellY; ++CellY)
	{
		for (int64 CellX = MinimumCellX; CellX <= MaximumCellX; ++CellX)
		{
			float LocalHeight = 0.0f;
			uint8 ColorBand = 0;
			if (!TerrainHeightField.TrySampleWorldCell(
				CellX, CellY, LocalHeight, ColorBand))
			{
				continue;
			}
			const FVector LocalSurface(
				TerrainHeightField.FirstCellCenter.X
					+ static_cast<double>(CellX - FirstWorldCellX)
						* TerrainHeightField.CellSize,
				TerrainHeightField.FirstCellCenter.Y
					+ static_cast<double>(CellY - FirstWorldCellY)
						* TerrainHeightField.CellSize,
				LocalHeight);
			const float WorldHeight =
				WorldTransform.TransformPosition(LocalSurface).Z;
			if (FMath::IsFinite(WorldHeight))
			{
				MaximumWorldHeight = FMath::Max(
					MaximumWorldHeight, WorldHeight);
				bSampledTerrain = true;
			}
		}
	}
	if (!bSampledTerrain)
	{
		return false;
	}

	OutWorldLocation = RequestedWorldLocation;
	OutWorldLocation.Z =
		MaximumWorldHeight + CapsuleHalfHeight + Clearance;
	return FMath::IsFinite(OutWorldLocation.X)
		&& FMath::IsFinite(OutWorldLocation.Y)
		&& FMath::IsFinite(OutWorldLocation.Z);
}

bool AMatterFluxPlayableWorldActor::RequestPlayerSpawnRegion(
	const FVector& WorldLocation,
	FIntPoint& OutTerrainChunk)
{
	OutTerrainChunk = FIntPoint::ZeroValue;
	if (!HasAuthority() || IsGenerationInProgress() || !MaterialSimulation)
	{
		return false;
	}
	// Authored story maps are committed atomically as scene boxes rather than
	// procedural terrain chunks. Reaching this state is their equivalent entry
	// barrier.
	if (IsCustomMapActive() && !TerrainHeightField.IsValid())
	{
		return true;
	}
	if (!TerrainHeightField.IsValid()
		|| !FMath::IsFinite(WorldLocation.X)
		|| !FMath::IsFinite(WorldLocation.Y))
	{
		return false;
	}

	const FVector LocalLocation =
		GetActorTransform().InverseTransformPosition(WorldLocation);
	const int64 FirstWorldCellX = FMath::FloorToInt64(
		TerrainHeightField.FirstCellCenter.X / TerrainHeightField.CellSize);
	const int64 FirstWorldCellY = FMath::FloorToInt64(
		TerrainHeightField.FirstCellCenter.Y / TerrainHeightField.CellSize);
	const int64 CellX = FirstWorldCellX + FMath::RoundToInt64(
		(LocalLocation.X - TerrainHeightField.FirstCellCenter.X)
			/ TerrainHeightField.CellSize);
	const int64 CellY = FirstWorldCellY + FMath::RoundToInt64(
		(LocalLocation.Y - TerrainHeightField.FirstCellCenter.Y)
			/ TerrainHeightField.CellSize);
	OutTerrainChunk = FIntPoint(
		FMath::FloorToInt(
			static_cast<double>(CellX) / TerrainStreamingChunkSize),
		FMath::FloorToInt(
			static_cast<double>(CellY) / TerrainStreamingChunkSize));
	PlayerSpawnRegionFocusCounts.FindOrAdd(OutTerrainChunk)++;

	// This runs behind the entry gate. Commit the player's own floor now and
	// retarget the surrounding render/population queues before any creature or
	// pawn can BeginPlay at this location.
	RefreshVisibleLevelLayers(true);
	RefreshVisibleFragmentSources(true);
	return true;
}

void AMatterFluxPlayableWorldActor::ReleasePlayerSpawnRegion(
	const FIntPoint& TerrainChunk)
{
	int32* Count = PlayerSpawnRegionFocusCounts.Find(TerrainChunk);
	if (!Count)
	{
		return;
	}
	if (--(*Count) <= 0)
	{
		PlayerSpawnRegionFocusCounts.Remove(TerrainChunk);
	}
}

bool AMatterFluxPlayableWorldActor::IsPlayerSpawnRegionTerrainReady(
	const FIntPoint& TerrainChunk) const
{
	if (IsGenerationInProgress() || !MaterialSimulation)
	{
		return false;
	}
	if (IsCustomMapActive() && !TerrainHeightField.IsValid())
	{
		return true;
	}
	UProceduralMeshComponent* TerrainComponent =
		GeneratedTerrainChunks.FindRef(TerrainChunk);
	const UBodySetup* TerrainBodySetup = IsValid(TerrainComponent)
		? TerrainComponent->GetBodySetup()
		: nullptr;
	return TerrainHeightField.IsValid()
		&& IsValid(TerrainComponent)
		&& ActiveTerrainChunks.Contains(TerrainChunk)
		&& TerrainComponent->GetCollisionEnabled()
			!= ECollisionEnabled::NoCollision
		// A procedural component enables collision before its asynchronous Chaos
		// cook has finished. Treating that flag as readiness lets gravity move the
		// new pawn through a chunk which has no physics mesh yet.
		&& TerrainBodySetup
		&& TerrainBodySetup->bCreatedPhysicsMeshes
		&& !TerrainBodySetup->bFailedToCreatePhysicsMeshes
		&& TerrainComponent->IsPhysicsStateCreated();
}

bool AMatterFluxPlayableWorldActor::IsInitialWorldStreamingReady() const
{
	if (IsGenerationInProgress() || !MaterialSimulation)
	{
		return false;
	}
	if (IsCustomMapActive() && !TerrainHeightField.IsValid())
	{
		return true;
	}
	if (!TerrainHeightField.IsValid()
		|| GetPendingTerrainChunkPrefetchCount() > 0
		|| GetPendingProceduralPopulationUpdateCount() > 0)
	{
		return false;
	}

	// A mesh transaction can leave its asynchronous Chaos cook running after
	// the terrain build queue reaches zero. Do not expose any part of the initial
	// visible window until all of its collision is usable as well.
	for (const FIntPoint Chunk : DesiredTerrainChunks)
	{
		UProceduralMeshComponent* TerrainComponent =
			GeneratedTerrainChunks.FindRef(Chunk);
		const UBodySetup* TerrainBodySetup = IsValid(TerrainComponent)
			? TerrainComponent->GetBodySetup()
			: nullptr;
		if (!IsValid(TerrainComponent)
			|| !ActiveTerrainChunks.Contains(Chunk)
			|| TerrainComponent->GetCollisionEnabled()
				== ECollisionEnabled::NoCollision
			|| !TerrainBodySetup
			|| !TerrainBodySetup->bCreatedPhysicsMeshes
			|| TerrainBodySetup->bFailedToCreatePhysicsMeshes
			|| !TerrainComponent->IsPhysicsStateCreated())
		{
			return false;
		}
	}
	return !DesiredTerrainChunks.IsEmpty();
}

bool AMatterFluxPlayableWorldActor::IsInitialWorldEntryReady() const
{
	return bInitialMaterialWarmupComplete
		&& IsInitialWorldStreamingReady();
}

int32 AMatterFluxPlayableWorldActor::ApplyTerrainDamage(
	const FFragmentDamageShape& DamageShape,
	const float DamagePower)
{
	if (!HasAuthority()
		|| !TerrainHeightField.IsValid()
		|| !DamageShape.WorldTransform.IsValid()
		|| !FMath::IsFinite(DamagePower)
		|| DamagePower < 0.0f)
	{
		return 0;
	}

	float SearchRadius = 0.0f;
	switch (DamageShape.Type)
	{
	case EFragmentDamageShapeType::Circle:
		SearchRadius = DamageShape.Radius;
		break;
	case EFragmentDamageShapeType::Box:
		SearchRadius = DamageShape.Extents.Size();
		break;
	case EFragmentDamageShapeType::Line:
		SearchRadius = FVector2D(
			DamageShape.Extents.X * 0.5f,
			DamageShape.Thickness * 0.5f).Size();
		break;
	default:
		return 0;
	}
	if (!FMath::IsFinite(SearchRadius) || SearchRadius <= 0.0f)
	{
		return 0;
	}

	const float CellSize = TerrainHeightField.CellSize;
	const FVector LocalCenter = GetActorTransform().InverseTransformPosition(
		DamageShape.WorldTransform.GetLocation());
	const FIntPoint FirstWorldCell(
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.X / CellSize),
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.Y / CellSize));
	const int32 MinimumCellX = FirstWorldCell.X + FMath::FloorToInt(
		(LocalCenter.X - SearchRadius
			- TerrainHeightField.FirstCellCenter.X) / CellSize);
	const int32 MaximumCellX = FirstWorldCell.X + FMath::CeilToInt(
		(LocalCenter.X + SearchRadius
			- TerrainHeightField.FirstCellCenter.X) / CellSize);
	const int32 MinimumCellY = FirstWorldCell.Y + FMath::FloorToInt(
		(LocalCenter.Y - SearchRadius
			- TerrainHeightField.FirstCellCenter.Y) / CellSize);
	const int32 MaximumCellY = FirstWorldCell.Y + FMath::CeilToInt(
		(LocalCenter.Y + SearchRadius
			- TerrainHeightField.FirstCellCenter.Y) / CellSize);

	TArray<FIntPoint> ChangedCells;
	for (int32 CellY = MinimumCellY; CellY <= MaximumCellY; ++CellY)
	{
		for (int32 CellX = MinimumCellX; CellX <= MaximumCellX; ++CellX)
		{
			float CurrentHeight = 0.0f;
			uint8 ColorBand = 0;
			if (!TerrainHeightField.TrySampleWorldCell(
				CellX,
				CellY,
				CurrentHeight,
				ColorBand))
			{
				continue;
			}
			const FVector LocalSurface(
				TerrainHeightField.FirstCellCenter.X
					+ static_cast<double>(CellX - FirstWorldCell.X)
						* CellSize,
				TerrainHeightField.FirstCellCenter.Y
					+ static_cast<double>(CellY - FirstWorldCell.Y)
						* CellSize,
				CurrentHeight);
			const FVector ShapeLocal = DamageShape.WorldTransform
				.InverseTransformPosition(
					GetActorTransform().TransformPosition(LocalSurface));

			float VerticalReach = 0.0f;
			bool bInsideFootprint = false;
			switch (DamageShape.Type)
			{
			case EFragmentDamageShapeType::Circle:
			{
				const double HorizontalSquared =
					ShapeLocal.X * ShapeLocal.X
					+ ShapeLocal.Y * ShapeLocal.Y;
				bInsideFootprint = HorizontalSquared
					<= FMath::Square(DamageShape.Radius);
				if (bInsideFootprint)
				{
					VerticalReach = FMath::Sqrt(FMath::Max(
						0.0,
						FMath::Square(
							static_cast<double>(DamageShape.Radius))
							- HorizontalSquared));
				}
				break;
			}
			case EFragmentDamageShapeType::Box:
				bInsideFootprint =
					FMath::Abs(ShapeLocal.X) <= DamageShape.Extents.X
					&& FMath::Abs(ShapeLocal.Y) <= DamageShape.Extents.Y;
				VerticalReach = FMath::Max(
					CellSize,
					FMath::Min(
						DamageShape.Extents.X,
						DamageShape.Extents.Y));
				break;
			case EFragmentDamageShapeType::Line:
				bInsideFootprint =
					FMath::Abs(ShapeLocal.X)
						<= DamageShape.Extents.X * 0.5f
					&& FMath::Abs(ShapeLocal.Y)
						<= DamageShape.Thickness * 0.5f;
				VerticalReach = FMath::Max(
					CellSize,
					DamageShape.Thickness * 0.5f);
				break;
			default:
				break;
			}
			if (!bInsideFootprint
				|| VerticalReach <= 0.0f
				|| FMath::Abs(ShapeLocal.Z) > VerticalReach)
			{
				continue;
			}

			const FVector CutFloorWorld = DamageShape.WorldTransform
				.TransformPosition(FVector(
					ShapeLocal.X,
					ShapeLocal.Y,
					-VerticalReach));
			const float CutFloorLocal = GetActorTransform()
				.InverseTransformPosition(CutFloorWorld).Z;
			const float NextHeight = FMath::Max(
				TerrainHeightField.BottomZ,
				FMath::Min(CurrentHeight, CutFloorLocal));
			if (NextHeight >= CurrentHeight - KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const FIntPoint WorldCell(CellX, CellY);
			TerrainHeightField.RuntimeHeightOverrides.Add(
				WorldCell,
				NextHeight);
			ChangedCells.Add(WorldCell);

			const int32 LocalX = CellX - FirstWorldCell.X;
			const int32 LocalY = CellY - FirstWorldCell.Y;
			if (LocalX >= 0 && LocalX < TerrainHeightField.Width
				&& LocalY >= 0 && LocalY < TerrainHeightField.Height)
			{
				const int32 GroundIndex = TerrainHeightField.ToIndex(
					LocalX,
					LocalY);
				if (GroundSurfacePositions.IsValidIndex(GroundIndex))
				{
					GroundSurfacePositions[GroundIndex].Z = NextHeight;
				}
			}
		}
	}
	if (ChangedCells.IsEmpty())
	{
		return 0;
	}

	PublishTerrainHeightOverrides();
	RefreshTerrainBackdropForCells(ChangedCells);
	RebuildResidentTerrainChunksForCells(ChangedCells);
	bGroundReactionVisualDirty = true;
	bGroundReactionVisualNeedsFullRebuild = true;
	PendingGroundReactionVisualCellIndices.Reset();
	bMaterialVisualizationDirty = true;
	return ChangedCells.Num();
}

bool AMatterFluxPlayableWorldActor::TrySampleLiquidColumnAtWorldLocation(
	const FVector& WorldLocation,
	MatterFlux::Liquid::FLiquidColumn& OutColumn) const
{
	OutColumn = {};
	if (!MaterialSimulation)
	{
		return false;
	}

	FIntPoint WorldCell;
	if (!TryWorldLocationToCell(
			GetActorTransform(),
			WorldLocation,
			MaterialSimulationCellSize,
			WorldCell))
	{
		return false;
	}

	MatterFlux::Material::FCellSnapshot Snapshot;
	FName MaterialId = NAME_None;
	float LocalBottomZ = 0.0f;
	float LocalSurfaceZ = 0.0f;
	if (MaterialSimulation->TryGetCellSnapshot(WorldCell, Snapshot)
		&& MaterialLiquidDensities.Contains(Snapshot.MaterialId))
	{
		MaterialId = Snapshot.MaterialId;
		LocalBottomZ = static_cast<float>(Snapshot.SupportHeight);
		LocalSurfaceZ = LocalBottomZ
			+ MaterialLiquidColumnHeight
				* (static_cast<float>(Snapshot.Amount) / 255.0f);
	}
	const float* LiquidDensity = MaterialLiquidDensities.Find(MaterialId);
	if (!LiquidDensity
		|| !FMath::IsFinite(*LiquidDensity)
		|| *LiquidDensity <= 0.0f)
	{
		return false;
	}

	const FVector LocalCenter(
		(static_cast<double>(WorldCell.X) + 0.5)
			* MaterialSimulationCellSize,
		(static_cast<double>(WorldCell.Y) + 0.5)
			* MaterialSimulationCellSize,
		LocalBottomZ);
	const FVector WorldBottom =
		GetActorTransform().TransformPosition(LocalCenter);
	const FVector WorldSurface = GetActorTransform().TransformPosition(
		FVector(LocalCenter.X, LocalCenter.Y, LocalSurfaceZ));
	OutColumn.MaterialId = MaterialId;
	OutColumn.Density = *LiquidDensity;
	OutColumn.BottomZ = FMath::Min(WorldBottom.Z, WorldSurface.Z);
	OutColumn.SurfaceZ = FMath::Max(WorldBottom.Z, WorldSurface.Z);
	OutColumn.FlowVelocity = FVector::ZeroVector;
	return OutColumn.IsValid();
}

bool AMatterFluxPlayableWorldActor::
	TrySampleAmbientLiquidColumnAtWorldLocation(
		const FVector& WorldLocation,
		MatterFlux::Liquid::FLiquidColumn& OutColumn) const
{
	FIntPoint WorldCell;
	if (!TryWorldLocationToCell(
		GetActorTransform(),
		WorldLocation,
		MaterialSimulationCellSize,
		WorldCell))
	{
		return false;
	}
	MatterFlux::Liquid::FLiquidColumn LocalColumn;
	const bool bHasLocalColumn = TrySampleLiquidColumnAtWorldLocation(
		WorldLocation, LocalColumn);
	if (!PreviousMaterialDisplacementCells.Contains(WorldCell))
	{
		OutColumn = LocalColumn;
		return bHasLocalColumn;
	}

	const FVector Local =
		GetActorTransform().InverseTransformPosition(WorldLocation);
	for (int32 Radius = 1; Radius <= 6; ++Radius)
	{
		TArray<FIntPoint, TInlineAllocator<48>> Ring;
		for (int32 X = -Radius; X <= Radius; ++X)
		{
			Ring.Add(FIntPoint(X, -Radius));
		}
		for (int32 Y = -Radius + 1; Y <= Radius; ++Y)
		{
			Ring.Add(FIntPoint(Radius, Y));
		}
		for (int32 X = Radius - 1; X >= -Radius; --X)
		{
			Ring.Add(FIntPoint(X, Radius));
		}
		for (int32 Y = Radius - 1; Y > -Radius; --Y)
		{
			Ring.Add(FIntPoint(-Radius, Y));
		}
		for (const FIntPoint Offset : Ring)
		{
			const FIntPoint NeighborCell = WorldCell + Offset;
			if (PreviousMaterialDisplacementCells.Contains(NeighborCell))
			{
				continue;
			}
			const FVector NeighborWorld =
				GetActorTransform().TransformPosition(FVector(
					(static_cast<double>(NeighborCell.X) + 0.5)
						* MaterialSimulationCellSize,
					(static_cast<double>(NeighborCell.Y) + 0.5)
						* MaterialSimulationCellSize,
					Local.Z));
			MatterFlux::Liquid::FLiquidColumn Neighbor;
			if (!TrySampleLiquidColumnAtWorldLocation(
					NeighborWorld, Neighbor))
			{
				continue;
			}
			OutColumn = Neighbor;
			float LocalTerrainHeight = 0.0f;
			if (TrySampleTerrainHeightAtWorldLocation(
					WorldLocation, LocalTerrainHeight)
				&& LocalTerrainHeight < OutColumn.SurfaceZ)
			{
				OutColumn.BottomZ = LocalTerrainHeight;
			}
			return OutColumn.IsValid();
		}
	}
	OutColumn = LocalColumn;
	return bHasLocalColumn;
}

bool AMatterFluxPlayableWorldActor::
	TrySampleMovementMediumColumnAtWorldLocation(
		const FVector& WorldLocation,
		FMatterFluxMovementMediumColumn& OutColumn) const
{
	OutColumn = {};
	if (!MaterialSimulation)
	{
		return false;
	}
	FIntPoint WorldCell;
	if (!TryWorldLocationToCell(
			GetActorTransform(),
			WorldLocation,
			MaterialSimulationCellSize,
			WorldCell))
	{
		return false;
	}

	FName MaterialId = NAME_None;
	int32 SupportHeight = 0;
	uint16 Amount = 0;
	MatterFlux::Material::FCellSnapshot Snapshot;
	if (MaterialSimulation->TryGetCellSnapshot(WorldCell, Snapshot)
		&& MaterialMovementMedia.Contains(Snapshot.MaterialId))
	{
		MaterialId = Snapshot.MaterialId;
		SupportHeight = Snapshot.SupportHeight;
		Amount = Snapshot.Amount;
	}
	else if (const FMatterFluxMaterialDisplacementState* Previous =
		PreviousMaterialDisplacementCells.Find(WorldCell);
		Previous
			&& !Previous->MaterialId.IsNone()
			&& MaterialMovementMedia.Contains(Previous->MaterialId))
	{
		// The body's own occupancy intentionally made the current cell empty.
		// Retain the pre-displacement column only as a force/drag sample; it is
		// never written back into simulation state here.
		MaterialId = Previous->MaterialId;
		SupportHeight = Previous->SupportHeight;
		Amount = Previous->ReferenceAmount;
	}
	const FMovementMediumDefinition* Definition =
		MaterialMovementMedia.Find(MaterialId);
	if (!Definition || Amount == 0)
	{
		return false;
	}

	const FVector LocalCenter(
		(static_cast<double>(WorldCell.X) + 0.5)
			* MaterialSimulationCellSize,
		(static_cast<double>(WorldCell.Y) + 0.5)
			* MaterialSimulationCellSize,
		static_cast<double>(SupportHeight));
	const FVector WorldBottom =
		GetActorTransform().TransformPosition(LocalCenter);
	const FVector WorldSurface = GetActorTransform().TransformPosition(FVector(
		LocalCenter.X,
		LocalCenter.Y,
		static_cast<double>(SupportHeight)
			+ Definition->FullColumnHeight
				* (static_cast<float>(Amount) / 255.0f)));
	OutColumn.MaterialId = MaterialId;
	OutColumn.Phase = Definition->Phase;
	OutColumn.MovementResistance = Definition->MovementResistance;
	OutColumn.BottomZ = FMath::Min(WorldBottom.Z, WorldSurface.Z);
	OutColumn.SurfaceZ = FMath::Max(WorldBottom.Z, WorldSurface.Z);
	return OutColumn.IsValid();
}

bool AMatterFluxPlayableWorldActor::DisplaceLiquidInWorldBounds(
	const FVector& Center,
	const FVector& HorizontalExtent,
	const float BottomZ,
	const float TopZ,
	const bool bCapsuleShape,
	const bool bDeferMaterialSolve)
{
	return DisplaceMaterialInWorldBounds(
		Center,
		HorizontalExtent,
		BottomZ,
		TopZ,
		bCapsuleShape,
		bDeferMaterialSolve,
		false);
}

bool AMatterFluxPlayableWorldActor::DisplaceMaterialInWorldBounds(
	const FVector& Center,
	const FVector& HorizontalExtent,
	const float BottomZ,
	const float TopZ,
	const bool bCapsuleShape,
	const bool bDeferMaterialSolve,
	const bool bDisplacePowders)
{
	if (!HasAuthority()
		|| !MaterialSimulation
		|| Center.ContainsNaN()
		|| HorizontalExtent.ContainsNaN()
		|| !FMath::IsFinite(BottomZ)
		|| !FMath::IsFinite(TopZ)
		|| HorizontalExtent.X <= UE_SMALL_NUMBER
		|| HorizontalExtent.Y <= UE_SMALL_NUMBER
		|| TopZ <= BottomZ)
	{
		return false;
	}

	FIntPoint MinCell;
	FIntPoint MaxCell;
	if (!TryWorldLocationToCell(
			GetActorTransform(),
			Center - FVector(
				HorizontalExtent.X, HorizontalExtent.Y, 0.0f),
			MaterialSimulationCellSize,
			MinCell)
		|| !TryWorldLocationToCell(
			GetActorTransform(),
			Center + FVector(
				HorizontalExtent.X, HorizontalExtent.Y, 0.0f),
			MaterialSimulationCellSize,
			MaxCell))
	{
		return false;
	}

	const FVector LocalCenter =
		GetActorTransform().InverseTransformPosition(Center);
	const float BodyHalfHeight = (TopZ - BottomZ) * 0.5f;
	const float CapsuleRadius = FMath::Min(
		HorizontalExtent.X, HorizontalExtent.Y);
	const float CapsuleCylinderHalfHeight = FMath::Max(
		BodyHalfHeight - CapsuleRadius, 0.0f);
	TArray<
		MatterFlux::Material::FLiquidDisplacementConstraint,
		TInlineAllocator<64>> Constraints;
	for (int64 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		for (int64 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			const FIntPoint Cell(
				static_cast<int32>(CellX),
				static_cast<int32>(CellY));
			const FVector LocalCellCenter(
				(static_cast<double>(Cell.X) + 0.5)
					* MaterialSimulationCellSize,
				(static_cast<double>(Cell.Y) + 0.5)
					* MaterialSimulationCellSize,
				LocalCenter.Z);
			const double ClosestDx = FMath::Max(
				FMath::Abs(LocalCellCenter.X - LocalCenter.X)
					- MaterialSimulationCellSize * 0.5,
				0.0);
			const double ClosestDy = FMath::Max(
				FMath::Abs(LocalCellCenter.Y - LocalCenter.Y)
					- MaterialSimulationCellSize * 0.5,
				0.0);
			const double NormalizedDistanceSquared = bCapsuleShape
				? FMath::Square(ClosestDx / HorizontalExtent.X)
					+ FMath::Square(ClosestDy / HorizontalExtent.Y)
				: 0.0;
			if (bCapsuleShape && NormalizedDistanceSquared > 1.0)
			{
				continue;
			}
			if (!bCapsuleShape
				&& (ClosestDx > UE_SMALL_NUMBER
					|| ClosestDy > UE_SMALL_NUMBER))
			{
				continue;
			}

			const FVector WorldCellCenter =
				GetActorTransform().TransformPosition(LocalCellCenter);
			FMatterFluxMaterialDisplacementState State;
			if (const FMatterFluxMaterialDisplacementState* Existing =
				PendingMaterialDisplacementCells.Find(Cell))
			{
				State = *Existing;
			}
			else if (const FMatterFluxMaterialDisplacementState* Previous =
				PreviousMaterialDisplacementCells.Find(Cell))
			{
				State = *Previous;
			}
			else
			{
				MatterFlux::Material::FCellSnapshot Snapshot;
				if (!MaterialSimulation->TryGetCellSnapshot(Cell, Snapshot)
					|| !MaterialMovementMedia.Contains(
						Snapshot.MaterialId))
				{
					// Keep empty cells inside the footprint unavailable as
					// displacement destinations, matching the physical obstacle.
					State.MaximumRemainingAmount = 0;
					PendingMaterialDisplacementCells.Add(Cell, State);
					Constraints.Add({ Cell, 0 });
					continue;
				}
				State.MaterialId = Snapshot.MaterialId;
				State.Phase = MaterialMovementMedia.FindChecked(
					Snapshot.MaterialId).Phase;
				State.SupportHeight = Snapshot.SupportHeight;
				State.ReferenceAmount = Snapshot.Amount;
				State.MaximumRemainingAmount = Snapshot.Amount;
			}

			if (State.ReferenceAmount == 0
				|| State.MaterialId.IsNone())
			{
				PendingMaterialDisplacementCells.Add(Cell, State);
				Constraints.Add({ Cell, 0 });
				continue;
			}

			const FVector WorldReferenceBottom =
				GetActorTransform().TransformPosition(FVector(
					LocalCellCenter.X,
					LocalCellCenter.Y,
					static_cast<double>(State.SupportHeight)));
			const FMovementMediumDefinition* Medium =
				MaterialMovementMedia.Find(State.MaterialId);
			if (!Medium)
			{
				continue;
			}
			// Characters stand on the canonical powder surface. Their capsule must
			// not excavate its full volume every frame merely because the disposable
			// collision projection has not refreshed yet. Footsteps and landings use
			// the separate bounded disturbance transaction below.
			if (State.Phase == EMatterFluxMaterialPhase::Powder
				&& !bDisplacePowders)
			{
				continue;
			}
			const FVector WorldReferenceSurface =
				GetActorTransform().TransformPosition(FVector(
					LocalCellCenter.X,
					LocalCellCenter.Y,
					static_cast<double>(State.SupportHeight)
						+ Medium->FullColumnHeight
							* (static_cast<float>(
								State.ReferenceAmount) / 255.0f)));
			const float ReferenceBottomZ = FMath::Min(
				WorldReferenceBottom.Z, WorldReferenceSurface.Z);
			const float ReferenceSurfaceZ = FMath::Max(
				WorldReferenceBottom.Z, WorldReferenceSurface.Z);
			if (TopZ <= ReferenceBottomZ || BottomZ >= ReferenceSurfaceZ)
			{
				continue;
			}

			float AverageOverlapHeight = 0.0f;
			if (!bCapsuleShape)
			{
				const float CellMinX = LocalCellCenter.X
					- MaterialSimulationCellSize * 0.5f;
				const float CellMaxX = LocalCellCenter.X
					+ MaterialSimulationCellSize * 0.5f;
				const float CellMinY = LocalCellCenter.Y
					- MaterialSimulationCellSize * 0.5f;
				const float CellMaxY = LocalCellCenter.Y
					+ MaterialSimulationCellSize * 0.5f;
				const float OverlapX = FMath::Max(
					FMath::Min(CellMaxX, LocalCenter.X + HorizontalExtent.X)
						- FMath::Max(CellMinX, LocalCenter.X - HorizontalExtent.X),
					0.0f);
				const float OverlapY = FMath::Max(
					FMath::Min(CellMaxY, LocalCenter.Y + HorizontalExtent.Y)
						- FMath::Max(CellMinY, LocalCenter.Y - HorizontalExtent.Y),
					0.0f);
				const float HorizontalCoverage = FMath::Clamp(
					(OverlapX * OverlapY)
						/ FMath::Square(MaterialSimulationCellSize),
					0.0f,
					1.0f);
				const float VerticalOverlap = FMath::Max(
					FMath::Min(TopZ, ReferenceSurfaceZ)
						- FMath::Max(BottomZ, ReferenceBottomZ),
					0.0f);
				AverageOverlapHeight = VerticalOverlap
					* HorizontalCoverage;
			}
			else
			{
				constexpr int32 SamplesPerAxis = 4;
				float AccumulatedOverlap = 0.0f;
				for (int32 SampleY = 0;
					SampleY < SamplesPerAxis;
					++SampleY)
				{
					for (int32 SampleX = 0;
						SampleX < SamplesPerAxis;
						++SampleX)
					{
						const float LocalSampleX = LocalCellCenter.X
							+ ((SampleX + 0.5f) / SamplesPerAxis - 0.5f)
								* MaterialSimulationCellSize;
						const float LocalSampleY = LocalCellCenter.Y
							+ ((SampleY + 0.5f) / SamplesPerAxis - 0.5f)
								* MaterialSimulationCellSize;
						const float NormalizedRadiusSquared =
							FMath::Square(
								(LocalSampleX - LocalCenter.X)
									/ HorizontalExtent.X)
							+ FMath::Square(
								(LocalSampleY - LocalCenter.Y)
									/ HorizontalExtent.Y);
						if (NormalizedRadiusSquared >= 1.0f)
						{
							continue;
						}
						const float CapHalfHeight = CapsuleRadius
							* FMath::Sqrt(1.0f - NormalizedRadiusSquared);
						const float SampleHalfHeight =
							CapsuleCylinderHalfHeight + CapHalfHeight;
						const float SampleBottom = Center.Z
							- SampleHalfHeight;
						const float SampleTop = Center.Z
							+ SampleHalfHeight;
						AccumulatedOverlap += FMath::Max(
							FMath::Min(SampleTop, ReferenceSurfaceZ)
								- FMath::Max(SampleBottom, ReferenceBottomZ),
							0.0f);
					}
				}
				AverageOverlapHeight = AccumulatedOverlap
					/ static_cast<float>(SamplesPerAxis * SamplesPerAxis);
			}

			const int32 OccupiedAmount = FMath::Clamp(
				FMath::CeilToInt(
					255.0f * AverageOverlapHeight
						/ Medium->FullColumnHeight),
				0,
				static_cast<int32>(State.ReferenceAmount));
			if (OccupiedAmount <= 0)
			{
				continue;
			}
			State.MaximumRemainingAmount = FMath::Min(
				State.MaximumRemainingAmount,
				static_cast<uint16>(
					State.ReferenceAmount - OccupiedAmount));
			PendingMaterialDisplacementCells.Add(Cell, State);
			Constraints.Add({ Cell, State.MaximumRemainingAmount });
		}
	}
	// Buoyancy components tick before the playable world and submit into the
	// same sparse footprint map. Defer their solve so every character, creature,
	// and rigid body is handled by one merged material transaction. Direct
	// callers keep the immediate behavior used by tools and focused tests.
	if (!bDeferMaterialSolve)
	{
		const int32 Moved = MaterialSimulation->DisplaceLiquids(
				Constraints,
				BodyLiquidDisplacementSearchRadiusCells)
			+ MaterialSimulation->DisplacePowders(
				Constraints,
				BodyLiquidDisplacementSearchRadiusCells);
		bMaterialVisualizationDirty |= Moved > 0;
	}
	return !Constraints.IsEmpty();
}

int32 AMatterFluxPlayableWorldActor::DisturbPowderAtWorldLocation(
	const FVector& WorldLocation,
	const int32 MaximumAmountToMove,
	const int32 SearchRadiusCells)
{
	if (!HasAuthority()
		|| !MaterialSimulation
		|| WorldLocation.ContainsNaN()
		|| MaximumAmountToMove <= 0
		|| SearchRadiusCells <= 0)
	{
		return 0;
	}

	FIntPoint CenterCell;
	if (!TryWorldLocationToCell(
		GetActorTransform(),
		WorldLocation,
		MaterialSimulationCellSize,
		CenterCell))
	{
		return 0;
	}

	struct FPowderCandidate
	{
		FIntPoint Cell = FIntPoint::ZeroValue;
		uint16 Amount = 0;
		int32 DistanceSquared = 0;
	};
	TArray<FPowderCandidate, TInlineAllocator<9>> Candidates;
	for (int32 Y = -1; Y <= 1; ++Y)
	{
		for (int32 X = -1; X <= 1; ++X)
		{
			const FIntPoint Cell = CenterCell + FIntPoint(X, Y);
			MatterFlux::Material::FCellSnapshot Snapshot;
			if (!MaterialSimulation->TryGetCellSnapshot(Cell, Snapshot))
			{
				continue;
			}
			const FMovementMediumDefinition* Medium =
				MaterialMovementMedia.Find(Snapshot.MaterialId);
			if (!Medium
				|| Medium->Phase != EMatterFluxMaterialPhase::Powder
				|| Snapshot.Amount == 0)
			{
				continue;
			}
			Candidates.Add({ Cell, Snapshot.Amount, X * X + Y * Y });
		}
	}
	Candidates.Sort([](const FPowderCandidate& A, const FPowderCandidate& B)
	{
		if (A.DistanceSquared != B.DistanceSquared)
		{
			return A.DistanceSquared < B.DistanceSquared;
		}
		return A.Cell.Y != B.Cell.Y
			? A.Cell.Y < B.Cell.Y
			: A.Cell.X < B.Cell.X;
	});

	int32 RemainingBudget = FMath::Clamp(MaximumAmountToMove, 1, 255);
	TArray<MatterFlux::Material::FLiquidDisplacementConstraint,
		TInlineAllocator<9>> Constraints;
	for (const FPowderCandidate& Candidate : Candidates)
	{
		if (RemainingBudget <= 0)
		{
			break;
		}
		const int32 AmountToMove = FMath::Min<int32>(
			Candidate.Amount,
			RemainingBudget);
		Constraints.Add({
			Candidate.Cell,
			static_cast<uint16>(Candidate.Amount - AmountToMove) });
		RemainingBudget -= AmountToMove;
	}
	if (Constraints.IsEmpty())
	{
		return 0;
	}

	const int32 Moved = MaterialSimulation->DisplacePowders(
		Constraints,
		FMath::Clamp(SearchRadiusCells, 1, 16));
	if (Moved > 0)
	{
		bMaterialVisualizationDirty = true;
	}
	return Moved;
}

bool AMatterFluxPlayableWorldActor::TryGetLiquidProjectionHeightAudit(
	const FName MaterialId,
	FMatterFluxLiquidProjectionHeightAudit& OutAudit) const
{
	if (const FMatterFluxLiquidProjectionHeightAudit* Audit =
		LiquidProjectionHeightAudits.Find(MaterialId))
	{
		OutAudit = *Audit;
		return true;
	}
	OutAudit = {};
	return false;
}

bool AMatterFluxPlayableWorldActor::HasLiquidProjectionAtWorldLocation(
	const FName MaterialId,
	const FVector& WorldLocation) const
{
	FIntPoint MaterialCell;
	if (MaterialId.IsNone()
		|| !TryWorldLocationToCell(
			GetActorTransform(),
			WorldLocation,
			MaterialSimulationCellSize,
			MaterialCell))
	{
		return false;
	}
	const FIntPoint RenderChunk = ToMaterialRenderChunk(
		MaterialCell, MaterialSimulationChunkSize);
	for (const TPair<FName, FIntPoint>& Pair : LiquidProjectionChunks)
	{
		if (Pair.Value == RenderChunk
			&& LiquidProjectionMaterials.FindRef(Pair.Key) == MaterialId
			&& IsValid(GeneratedLiquidLayerMeshes.FindRef(Pair.Key)))
		{
			return true;
		}
	}
	return false;
}

void AMatterFluxPlayableWorldActor::SetWorldStreamingFocus(
	const FVector& WorldLocation)
{
	if (!HasAuthority() || !MaterialSimulation)
	{
		return;
	}
	FIntPoint NewFocus;
	if (!TryWorldLocationToCell(
		GetActorTransform(),
		WorldLocation,
		MaterialSimulationCellSize,
		NewFocus))
	{
		return;
	}
	const auto ToChunk =
		[](const FIntPoint Cell, const int32 ChunkSize)
		{
			return FIntPoint(
				FMath::FloorToInt(
					static_cast<double>(Cell.X)
						/ ChunkSize),
				FMath::FloorToInt(
					static_cast<double>(Cell.Y)
						/ ChunkSize));
		};
	const FIntPoint NewMaterialChunk =
		ToChunk(NewFocus, MaterialSimulationChunkSize);
	const FIntPoint NewMaterialFocus(
		NewMaterialChunk.X * MaterialSimulationChunkSize,
		NewMaterialChunk.Y * MaterialSimulationChunkSize);
	const TArray<FIntPoint> NewMaterialFocusCells = {
		NewMaterialFocus
	};
	const bool bMaterialChunkChanged =
		MaterialSimulation->GetFocuses() != NewMaterialFocusCells;
	const bool bTerrainChunkChanged =
		ToChunk(
			ReplicatedMaterialSimulationFocus,
			TerrainStreamingChunkSize)
		!= ToChunk(NewFocus, TerrainStreamingChunkSize);
	if (!bMaterialChunkChanged && !bTerrainChunkChanged)
	{
		return;
	}
	ReplicatedMaterialSimulationFocus = NewMaterialFocus;
	if (bMaterialChunkChanged)
	{
		MaterialSimulation->SetFocuses(NewMaterialFocusCells);
		bMaterialVisualizationDirty = true;
		PublishMaterialSimulationState();
	}
	RefreshVisibleLevelLayers(false);
	RefreshVisibleFragmentSources(false);
	ForceNetUpdate();
}

int32 AMatterFluxPlayableWorldActor::GetResidentMaterialChunkCount() const
{
	return MaterialSimulation
		? MaterialSimulation->GetResidentChunkCount()
		: 0;
}

int32 AMatterFluxPlayableWorldActor::GetArchivedMaterialChunkCount() const
{
	return MaterialSimulation
		? MaterialSimulation->GetArchivedChunkCount()
		: 0;
}

int32 AMatterFluxPlayableWorldActor::GetVisibleTerrainInstanceCount() const
{
	return GetVisibleTerrainTriangleCount();
}

int32 AMatterFluxPlayableWorldActor::GetVisibleTerrainTriangleCount() const
{
	int32 TriangleCount = 0;
	for (const FIntPoint Coordinate : ActiveTerrainChunks)
	{
		UProceduralMeshComponent* Component =
			GeneratedTerrainChunks.FindRef(Coordinate);
		if (!IsValid(Component))
		{
			continue;
		}
		for (int32 SectionIndex = 0;
			SectionIndex < Component->GetNumSections();
			++SectionIndex)
		{
			if (!Component->IsMeshSectionVisible(SectionIndex))
			{
				continue;
			}
			if (const FProcMeshSection* Section =
				Component->GetProcMeshSection(SectionIndex))
			{
				TriangleCount += Section->ProcIndexBuffer.Num() / 3;
			}
		}
	}
	return TriangleCount;
}

int32 AMatterFluxPlayableWorldActor::GetVisibleTerrainChunkCount() const
{
	return ActiveTerrainChunks.Num();
}

int32 AMatterFluxPlayableWorldActor::GetCachedFragmentSourceCount() const
{
	int32 Count = 0;
	for (const TPair<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
		: FragmentSourceChunks)
	{
		Count += Pair.Value.Num();
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::
	GetVisibleFragmentSourceProxyCount() const
{
	return FragmentSourceProxy
		? FragmentSourceProxy->GetVisibleSourceCount()
		: 0;
}

bool AMatterFluxPlayableWorldActor::UpdateLocalFragmentItemOcclusion(
	const FVector& CameraLocation,
	const FBox& ViewerBounds)
{
	if (!FragmentSourceProxy)
	{
		return false;
	}
	if (CameraLocation.ContainsNaN() || !ViewerBounds.IsValid)
	{
		FragmentSourceProxy->SetGhostedSources(TSet<FGuid>());
		return FragmentSourceProxy->HasActiveGhosting();
	}

	FBox QueryBounds(ForceInit);
	QueryBounds += CameraLocation;
	QueryBounds += ViewerBounds.Min;
	QueryBounds += ViewerBounds.Max;
	QueryBounds = QueryBounds.ExpandBy(260.0f);
	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*> Candidates;
	GatherLogicalFragmentSourceCandidates(QueryBounds, Candidates);
	TArray<MatterFlux::ItemOcclusion::FItem, TInlineAllocator<64>> Items;
	Items.Reserve(Candidates.Num());
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
		: Candidates)
	{
		if (!Source
			|| RemovedFragmentSourceIds.Contains(Source->SourceId)
			|| GeneratedFragmentSources.Contains(Source->SourceId)
			|| DynamicAggregateCarriers.Contains(Source->SourceId))
		{
			continue;
		}
		const FBox WorldBounds = BuildFragmentSourceLocalBounds(*Source)
			.TransformBy(GetActorTransform().ToMatrixWithScale());
		if (!WorldBounds.IsValid)
		{
			continue;
		}
		Items.Add({
			Source->SourceId,
			Source->AggregateId,
			WorldBounds,
			Source->Mask.CellSize});
	}

	MatterFlux::ItemOcclusion::FResult Result;
	MatterFlux::ItemOcclusion::Resolve(
		CameraLocation, ViewerBounds, Items, Result);
	FragmentSourceProxy->SetGhostedSources(Result.GhostItemIds);
	return FragmentSourceProxy->HasActiveGhosting();
}

void AMatterFluxPlayableWorldActor::SetFragmentSourceDebugIsolatedAggregate(
	const FGuid& AggregateId)
{
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->SetDebugIsolatedAggregate(AggregateId);
	}
}

bool AMatterFluxPlayableWorldActor::
	FindNearestTreeAggregateForVisualInspection(
		const FVector& Focus,
		FGuid& OutAggregateId,
		FGuid& OutRootSourceId,
		FBox& OutWorldBounds,
		FTransform& OutRootWorldTransform) const
{
	OutAggregateId.Invalidate();
	OutRootSourceId.Invalidate();
	OutWorldBounds = FBox(ForceInit);
	OutRootWorldTransform = FTransform::Identity;
	double BestDistanceSquared = TNumericLimits<double>::Max();
	for (const TPair<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
		: FragmentSourceChunks)
	{
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Pair.Value)
		{
			if (!Source.bAggregateRoot
				|| Source.Name != TEXT("TreeTrunk")
				|| !Source.AggregateId.IsValid()
				|| RemovedFragmentSourceIds.Contains(Source.SourceId))
			{
				continue;
			}
			const FTransform WorldTransform =
				Source.Transform * GetActorTransform();
			const double DistanceSquared = FVector::DistSquared(
				WorldTransform.GetLocation(),
				Focus);
			if (OutAggregateId.IsValid()
				&& DistanceSquared >= BestDistanceSquared)
			{
				continue;
			}
			OutAggregateId = Source.AggregateId;
			OutRootSourceId = Source.SourceId;
			OutRootWorldTransform = WorldTransform;
			BestDistanceSquared = DistanceSquared;
		}
	}
	if (!OutAggregateId.IsValid())
	{
		return false;
	}
	for (const TPair<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
		: FragmentSourceChunks)
	{
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Pair.Value)
		{
			if (Source.AggregateId == OutAggregateId
				&& !RemovedFragmentSourceIds.Contains(Source.SourceId))
			{
				OutWorldBounds += BuildFragmentSourceLocalBounds(Source)
					.TransformBy(GetActorTransform().ToMatrixWithScale());
			}
		}
	}
	return OutWorldBounds.IsValid != 0;
}

bool AMatterFluxPlayableWorldActor::ApplyMaterialStimulusToLogicalFragmentAggregate(
	const FGuid& AggregateId,
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority() || !AggregateId.IsValid())
	{
		return false;
	}
	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*> Candidates;
	for (const TPair<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
		: FragmentSourceChunks)
	{
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Pair.Value)
		{
			if (Source.AggregateId == AggregateId
				&& !RemovedFragmentSourceIds.Contains(Source.SourceId)
				&& !GeneratedFragmentSources.Contains(Source.SourceId))
			{
				Candidates.Add(&Source);
			}
		}
	}
	Candidates.Sort([this, &WorldLocation](
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Left,
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Right)
	{
		const double LeftDistance = BuildFragmentSourceLocalBounds(Left)
			.TransformBy(GetActorTransform().ToMatrixWithScale())
			.ComputeSquaredDistanceToPoint(WorldLocation);
		const double RightDistance = BuildFragmentSourceLocalBounds(Right)
			.TransformBy(GetActorTransform().ToMatrixWithScale())
			.ComputeSquaredDistanceToPoint(WorldLocation);
		if (!FMath::IsNearlyEqual(LeftDistance, RightDistance))
		{
			return LeftDistance < RightDistance;
		}
		return Left.SourceId.ToString(EGuidFormats::Digits)
			< Right.SourceId.ToString(EGuidFormats::Digits);
	});
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
		: Candidates)
	{
		FName CandidateStimulus = StimulusMaterial;
		if (Source && CandidateStimulus.IsNone()
			&& IMatterFluxScriptRuntime::IsAvailable())
		{
			const FMatterFluxContentRegistryPtr Registry =
				IMatterFluxScriptRuntime::Get().GetActiveRegistry();
			if (Registry.IsValid())
			{
				if (const FMatterFluxReactionDefinition* Rule =
					MatterFlux::Reaction::FMaterialReactionEngine::
						FindPropagatingRule(*Registry, Source->MaterialId))
				{
					CandidateStimulus = Rule->InputB;
				}
			}
		}
		if (Source && ApplyMaterialStimulusToLogicalFragmentSource(
			*Source,
			WorldLocation,
			CandidateStimulus,
			EventSeed ^ static_cast<int32>(GetTypeHash(Source->SourceId))))
		{
			return true;
		}
	}
	return false;
}

void AMatterFluxPlayableWorldActor::GatherFragmentSourcesInBounds(
	const FBox& Bounds,
	TArray<AFragment2DSourceActor*>& OutSources)
{
	OutSources.Reset();
	if (!HasAuthority() || !Bounds.IsValid)
	{
		return;
	}

	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>
		Candidates;
	GatherLogicalFragmentSourceCandidates(Bounds, Candidates);
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
		: Candidates)
	{
		if (!Source
			|| RemovedFragmentSourceIds.Contains(Source->SourceId)
			|| GeneratedFragmentSources.Contains(Source->SourceId)
			|| !Source->Mask.IsValid())
		{
			continue;
		}
		const FBox SourceWorldBounds =
			BuildFragmentSourceLocalBounds(*Source).TransformBy(
				GetActorTransform().ToMatrixWithScale());
		if (!SourceWorldBounds.IsValid
			|| !SourceWorldBounds.Intersect(Bounds))
		{
			continue;
		}
		SpawnFragmentSource(*Source);
	}

	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		AFragment2DSourceActor* Source = Pair.Value;
		if (IsValid(Source)
			&& !Source->IsActorBeingDestroyed()
			&& Source->GetCanonicalWorldBounds().Intersect(Bounds))
		{
			OutSources.Add(Source);
		}
	}
	// Houses and other independently spawned cuttable actors register with the
	// shared fragment subsystem rather than this world's streamed-source map.
	// Contact chemistry must see both ownership models; AddUnique also removes
	// streamed sources returned by both stores.
	if (UFragmentSimulationSubsystem* FragmentSubsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr)
	{
		TArray<AFragment2DSourceActor*> RegisteredSources;
		FragmentSubsystem->GatherSourcesInBounds(Bounds, RegisteredSources);
		for (AFragment2DSourceActor* Source : RegisteredSources)
		{
			if (IsValid(Source)
				&& !Source->IsActorBeingDestroyed()
				&& !Source->bBroken
				&& Source->GetCanonicalWorldBounds().Intersect(Bounds))
			{
				OutSources.AddUnique(Source);
			}
		}
	}
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->FlushPendingChanges();
	}
}

bool AMatterFluxPlayableWorldActor::SweepFixedFragmentSource(
	const FVector& Start,
	const FVector& End,
	const float Radius,
	FVector& OutImpactLocation,
	FVector& OutImpactNormal,
	AFragment2DSourceActor*& OutSource)
{
	OutImpactLocation = End;
	OutImpactNormal = FVector::ZeroVector;
	OutSource = nullptr;
	if (!HasAuthority() || Start.Equals(End, UE_SMALL_NUMBER))
	{
		return false;
	}
	FBox QueryBounds(ForceInit);
	QueryBounds += Start;
	QueryBounds += End;
	QueryBounds = QueryBounds.ExpandBy(FMath::Max(Radius, 0.0f));
	TArray<AFragment2DSourceActor*> Sources;
	GatherFragmentSourcesInBounds(QueryBounds, Sources);
	return SweepFixedFragmentSourceCandidates(
		Sources,
		Start,
		End,
		Radius,
		OutImpactLocation,
		OutImpactNormal,
		OutSource);
}

int32 AMatterFluxPlayableWorldActor::
	MaterializeFragmentSourcesForDamage(
		const FFragmentDamageShape& DamageShape)
{
	if (!HasAuthority() || !DamageShape.WorldTransform.IsValid())
	{
		return 0;
	}
	double ShapeRadius = 0.0;
	switch (DamageShape.Type)
	{
	case EFragmentDamageShapeType::Circle:
		ShapeRadius = DamageShape.Radius;
		break;
	case EFragmentDamageShapeType::Box:
		ShapeRadius = DamageShape.Extents.Size();
		break;
	case EFragmentDamageShapeType::Line:
		ShapeRadius = FVector2D(
			DamageShape.Extents.X * 0.5,
			DamageShape.Thickness * 0.5).Size();
		break;
	default:
		return 0;
	}
	if (!FMath::IsFinite(ShapeRadius) || ShapeRadius <= 0.0)
	{
		return 0;
	}

	const FVector DamageCenter =
		DamageShape.WorldTransform.GetLocation();
	const FBox DamageBounds = FBox::BuildAABB(
		DamageCenter,
		FVector(ShapeRadius));
	const FTransform OwnerTransform = GetActorTransform();

	int32 MaterializedCount = 0;
	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>
		Candidates;
	GatherLogicalFragmentSourceCandidates(DamageBounds, Candidates);
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
		: Candidates)
	{
		if (!Source
			|| RemovedFragmentSourceIds.Contains(Source->SourceId)
			|| GeneratedFragmentSources.Contains(Source->SourceId)
			|| !Source->Mask.IsValid())
		{
			continue;
		}
		const FTransform SourceWorldTransform =
			Source->Transform * OwnerTransform;
		FFragmentDamageShape LocalShape = DamageShape;
		LocalShape.WorldTransform =
			DamageShape.WorldTransform.GetRelativeTransform(
				SourceWorldTransform);
		TArray<uint8> CandidateMask = Source->Mask.SolidMask;
		if (MatterFlux::FragmentGeometry::ApplyDamageShape(
			CandidateMask,
			Source->Mask.Width,
			Source->Mask.Height,
			Source->Mask.CellSize,
			LocalShape))
		{
			SpawnFragmentSource(*Source);
			MaterializedCount +=
				GeneratedFragmentSources.Contains(Source->SourceId)
					? 1
					: 0;
		}
	}
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->FlushPendingChanges();
	}
	return MaterializedCount;
}

void AMatterFluxPlayableWorldActor::MaterializeFragmentAggregate(
	const FGuid& AggregateId)
{
	if (!HasAuthority() || !AggregateId.IsValid())
	{
		return;
	}
	for (const TPair<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
		: FragmentSourceChunks)
	{
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Pair.Value)
		{
			if (Source.AggregateId == AggregateId
				&& !RemovedFragmentSourceIds.Contains(Source.SourceId)
				&& !GeneratedFragmentSources.Contains(Source.SourceId))
			{
				SpawnFragmentSource(Source);
			}
		}
	}
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->FlushPendingChanges();
	}
}

void AMatterFluxPlayableWorldActor::SetFragmentSourceStreamingPinned(
	const FGuid& SourceId,
	const bool bPinned)
{
	if (!HasAuthority() || !SourceId.IsValid())
	{
		return;
	}
	if (bPinned)
	{
		if (!GeneratedFragmentSources.Contains(SourceId))
		{
			return;
		}
		StreamingPinnedFragmentSourceIds.Add(SourceId);
		PendingFragmentSourceDespawns.Remove(SourceId);
	}
	else
	{
		StreamingPinnedFragmentSourceIds.Remove(SourceId);
	}
}

bool AMatterFluxPlayableWorldActor::DematerializeFragmentSource(
	const FGuid& SourceId)
{
	if (!HasAuthority()
		|| !SourceId.IsValid()
		|| StreamingPinnedFragmentSourceIds.Contains(SourceId))
	{
		return false;
	}
	AFragment2DSourceActor* SourceActor =
		GeneratedFragmentSources.FindRef(SourceId);
	if (!IsValid(SourceActor)
		|| SourceActor->bDetachedFromTerrain)
	{
		return false;
	}

	FFragment2DSourceStreamingState State;
	FString Error;
	if (!SourceActor->CaptureStreamingState(State, Error))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Cannot return fragment source %s to its chunk batch: %s"),
			*SourceId.ToString(),
			*Error);
		return false;
	}
	if (!ArchiveFragmentSourceState(SourceId, State))
	{
		return false;
	}
	SourceActor->Destroy();
	GeneratedFragmentSources.Remove(SourceId);
	PendingFragmentSourceDespawns.Remove(SourceId);
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->FlushPendingChanges();
	}
	return true;
}

bool AMatterFluxPlayableWorldActor::RetireFragmentSourceIntoDynamicAggregate(
	AFragment2DSourceActor& SourceActor,
	AFragment2DActor& CarrierActor)
{
	const FGuid SourceId = SourceActor.SourceId;
	if (!HasAuthority()
		|| SourceActor.GetOwner() != this
		|| CarrierActor.GetWorld() != GetWorld()
		|| !SourceId.IsValid()
		|| SourceActor.bDetachedFromTerrain
		|| SourceActor.Revision < 0
		|| SourceActor.Revision == MAX_int32
		|| GeneratedFragmentSources.FindRef(SourceId) != &SourceActor)
	{
		return false;
	}

	FFragment2DSourceStreamingState SourceState;
	FString CaptureError;
	if (!SourceActor.CaptureStreamingState(SourceState, CaptureError))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Cannot move source %s into dynamic aggregate: %s"),
			*SourceId.ToString(),
			*CaptureError);
		return false;
	}
	if (SourceState.bHasReactionState)
	{
		if (!ArchiveFragmentSourceState(SourceId, SourceState))
		{
			return false;
		}
	}
	else
	{
		FFragment2DSourceStreamingState Tombstone;
		Tombstone.Revision = FMath::Max(SourceActor.Revision + 1, 1);
		TArray<uint8> EmptyRuntimeMask;
		EmptyRuntimeMask.Init(0, SourceActor.GetRuntimeMask().Num());
		Tombstone.SetRuntimeMask(MoveTemp(EmptyRuntimeMask));
		if (!PublishFragmentSourceState(SourceId, Tombstone))
		{
			return false;
		}
		StreamedFragmentSourceStates.Add(SourceId, Tombstone);
		ActiveSourceReactions.Remove(SourceId);
		LogicalSourceReactionIndex.Remove(SourceId);
	}

	GeneratedFragmentSources.Remove(SourceId);
	PendingFragmentSourceDespawns.Remove(SourceId);
	DynamicAggregateCarriers.Add(SourceId, &CarrierActor);
	NotifyDynamicAggregateOwnsSource(SourceId, &CarrierActor);
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->SetSourceMaterialized(SourceId, true);
		FragmentSourceProxy->FlushPendingChanges();
	}
	ForceNetUpdate();
	return true;
}

void AMatterFluxPlayableWorldActor::NotifyDynamicAggregateOwnsSource(
	const FGuid& SourceId,
	AFragment2DActor* CarrierActor)
{
	if (!SourceId.IsValid())
	{
		return;
	}
	RemovedFragmentSourceIds.Add(SourceId);
	StreamingPinnedFragmentSourceIds.Remove(SourceId);
	if (IsValid(CarrierActor)
		&& CarrierActor->GetWorld() == GetWorld())
	{
		DynamicAggregateCarriers.Add(SourceId, CarrierActor);
		if (const FFragment2DSourceStreamingState* Existing =
			StreamedFragmentSourceStates.Find(SourceId);
			Existing && Existing->bHasReactionState)
		{
			FName OutputMaterial = NAME_None;
			FLinearColor OutputColor(0.08f, 0.07f, 0.06f);
			const FMatterFluxContentRegistryPtr Registry =
				IMatterFluxScriptRuntime::IsAvailable()
					? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
					: nullptr;
			if (Registry.IsValid())
			{
				if (const FMatterFluxReactionDefinition* Rule =
					Registry->Reactions.Find(
						Existing->ReactionState.RuleId))
				{
					OutputMaterial = Rule->OutputA;
					if (const FMatterFluxMaterialDefinition* Material =
						Registry->Materials.Find(OutputMaterial))
					{
						OutputColor = Material->Color;
					}
				}
			}
			CarrierActor->ApplyAggregateSourceStreamingState(
				SourceId,
				*Existing,
				OutputMaterial,
				OutputColor);
		}
	}
	PendingFragmentSourceSpawns.RemoveAll(
		[&SourceId](
			const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
		{
			return Source.SourceId == SourceId;
		});
	PendingFragmentSourceDespawns.Remove(SourceId);
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->SetSourceMaterialized(SourceId, true);
	}
}

void AMatterFluxPlayableWorldActor::ReleaseDynamicAggregateCarrier(
	const AFragment2DActor& CarrierActor)
{
	for (auto It = DynamicAggregateCarriers.CreateIterator(); It; ++It)
	{
		if (!It.Value().IsValid()
			|| It.Value().Get() == &CarrierActor)
		{
			It.RemoveCurrent();
		}
	}
}

bool AMatterFluxPlayableWorldActor::GetFragmentSourceRuntimeState(
	const FGuid& SourceId,
	int32& OutRevision,
	TArray<uint8>& OutRuntimeMask) const
{
	OutRevision = INDEX_NONE;
	OutRuntimeMask.Reset();
	if (!SourceId.IsValid()
		|| RemovedFragmentSourceIds.Contains(SourceId))
	{
		return false;
	}
	if (const AFragment2DSourceActor* Active =
		GeneratedFragmentSources.FindRef(SourceId))
	{
		if (!IsValid(Active))
		{
			return false;
		}
		OutRevision = Active->Revision;
		OutRuntimeMask = Active->GetRuntimeMask();
		return true;
	}
	if (const FFragment2DSourceStreamingState* State =
		StreamedFragmentSourceStates.Find(SourceId))
	{
		OutRevision = State->Revision;
		OutRuntimeMask = State->GetRuntimeMask();
		return true;
	}
	if (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
		FindFragmentSourceDefinition(SourceId))
	{
		OutRevision = 0;
		OutRuntimeMask = Source->Mask.SolidMask;
		return true;
	}
	return false;
}

bool AMatterFluxPlayableWorldActor::ArchiveFragmentSourceState(
	const FGuid& SourceId,
	const FFragment2DSourceStreamingState& State)
{
	if (!HasAuthority() || !SourceId.IsValid() || State.Revision < 0)
	{
		return false;
	}
	TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime>
		PreparedRuntime;
	const FMatterFluxReactionDefinition* Rule = nullptr;
	FLinearColor OutputColor(0.08f, 0.07f, 0.06f);
	if (State.bHasReactionState)
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		Rule = Registry.IsValid()
			? Registry->Reactions.Find(State.ReactionState.RuleId)
			: nullptr;
		if (!Rule)
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Cannot archive source %s without reaction rule %s"),
				*SourceId.ToString(),
				*State.ReactionState.RuleId.ToString());
			return false;
		}
		if (const FMatterFluxMaterialDefinition* Material =
			Registry->Materials.Find(Rule->OutputA))
		{
			OutputColor = Material->Color;
		}
		if (State.ReactionState.ActiveMask.ContainsByPredicate(
			[](const uint8 Value)
			{
				return Value != 0;
			}))
		{
			PreparedRuntime = MakeUnique<
				MatterFlux::Reaction::FSourceReactionRuntime>();
			FString Error;
			if (!PreparedRuntime->RestoreState(
				MatterFlux::Reaction::FSourceRuntimeSettings(),
				State,
				*Rule,
				Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Cannot resume archived source %s reaction: %s"),
					*SourceId.ToString(),
					*Error);
				return false;
			}
		}
	}
	if (State.HasPersistentChanges()
		&& !PublishFragmentSourceState(SourceId, State))
	{
		return false;
	}
	if (State.HasPersistentChanges())
	{
		StreamedFragmentSourceStates.Add(SourceId, State);
	}
	else
	{
		StreamedFragmentSourceStates.Remove(SourceId);
	}
	ActiveSourceReactions.Remove(SourceId);
	if (PreparedRuntime)
	{
		ActiveSourceReactions.Add(SourceId, MoveTemp(PreparedRuntime));
	}
	LogicalSourceReactionIndex.ApplySnapshot(
		SourceId,
		State.bHasReactionState,
		State.ReactionState.ActiveMask);
	if (FragmentSourceProxy)
	{
		TArray<uint8> OutputMask;
		OutputMask.Init(0, State.GetRuntimeMask().Num());
		FName OutputMaterial = NAME_None;
		if (State.bHasReactionState && Rule)
		{
			OutputMask = State.ReactionState.OutputMask;
			OutputMaterial = Rule->OutputA;
		}
		if (FragmentSourceProxy->ApplySourceState(
			SourceId,
			State.GetRuntimeMask(),
			OutputMask,
			OutputMaterial,
			OutputColor,
			ActiveSourceReactions.Contains(SourceId))
			== EMatterFluxFragmentSourceProxyApplyResult::Invalid)
		{
			return false;
		}
		FragmentSourceProxy->SetSourceMaterialized(SourceId, false);
	}
	bSourceReactionVisualDirty |= State.bHasReactionState;
	return true;
}

const MatterFlux::PlayableLevel::FLevelFragmentSource*
	AMatterFluxPlayableWorldActor::FindFragmentSourceDefinition(
		const FGuid& SourceId) const
{
	const FIntPoint* Chunk = FragmentSourceChunkById.Find(SourceId);
	const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
		Chunk ? FragmentSourceChunks.Find(*Chunk) : nullptr;
	return Sources
		? Sources->FindByPredicate(
			[&SourceId](
				const MatterFlux::PlayableLevel::FLevelFragmentSource& Candidate)
			{
				return Candidate.SourceId == SourceId;
			})
		: nullptr;
}

void AMatterFluxPlayableWorldActor::
	GatherLogicalFragmentSourceCandidates(
		const FBox& WorldBounds,
		TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>&
			OutCandidates) const
{
	OutCandidates.Reset();
	const FTransform OwnerTransform = GetActorTransform();
	if (!WorldBounds.IsValid
		|| WorldBounds.Min.ContainsNaN()
		|| WorldBounds.Max.ContainsNaN()
		|| !OwnerTransform.IsValid()
		|| OwnerTransform.GetScale3D().GetAbsMin() <= UE_SMALL_NUMBER)
	{
		return;
	}

	const FBox LocalBounds = WorldBounds.TransformBy(
		OwnerTransform.ToInverseMatrixWithScale());
	TArray<FGuid> CandidateIds;
	FragmentSourceDefinitionIndex.Query(LocalBounds, CandidateIds);
	OutCandidates.Reserve(CandidateIds.Num());
	for (const FGuid& SourceId : CandidateIds)
	{
		if (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
			FindFragmentSourceDefinition(SourceId))
		{
			OutCandidates.Add(Source);
		}
	}
}

bool AMatterFluxPlayableWorldActor::ApplyMaterialStimulusToLogicalFragmentSource(
	const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !Source.SourceId.IsValid()
		|| !Source.Mask.IsValid()
		|| RemovedFragmentSourceIds.Contains(Source.SourceId)
		|| GeneratedFragmentSources.Contains(Source.SourceId)
		|| WorldLocation.ContainsNaN()
		|| StimulusMaterial.IsNone())
	{
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (!Registry.IsValid())
	{
		return false;
	}
	const FMatterFluxReactionDefinition* Rule =
		MatterFlux::Reaction::FMaterialReactionEngine::FindPropagatingRule(
			*Registry, Source.MaterialId, StimulusMaterial);
	if (!Rule)
	{
		return false;
	}

	TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime> Candidate;
	TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime>*
		ExistingRuntime = ActiveSourceReactions.Find(Source.SourceId);
	MatterFlux::Reaction::FSourceReactionRuntime* Runtime =
		ExistingRuntime ? ExistingRuntime->Get() : nullptr;
	if (Runtime)
	{
		const FMatterFluxReactionDefinition* ActiveRule = Runtime->GetRule();
		if (!ActiveRule || ActiveRule->Id != Rule->Id)
		{
			// A source can now have several reactions (for example fire and
			// acid corrosion). Do not reinterpret an in-flight simulation with
			// another rule; its masks and timers belong to the original rule.
			if (Runtime->IsActive())
			{
				return false;
			}
			ActiveSourceReactions.Remove(Source.SourceId);
			Runtime = nullptr;
		}
	}
	if (!Runtime)
	{
		Candidate = MakeUnique<
			MatterFlux::Reaction::FSourceReactionRuntime>();
		FString Error;
		const FFragment2DSourceStreamingState* Existing =
			StreamedFragmentSourceStates.Find(Source.SourceId);
		const bool bExistingReactionActive =
			Existing
			&& Existing->bHasReactionState
			&& Existing->ReactionState.ActiveMask.ContainsByPredicate(
				[](const uint8 Value)
				{
					return Value != 0;
				});
		const bool bCanRestoreExisting =
			Existing
			&& Existing->bHasReactionState
			&& Existing->ReactionState.RuleId == Rule->Id;
		if (bExistingReactionActive && !bCanRestoreExisting)
		{
			return false;
		}
		if (bCanRestoreExisting)
		{
			if (!Candidate->RestoreState(
				MatterFlux::Reaction::FSourceRuntimeSettings(),
				*Existing,
				*Rule,
				Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Logical source %s reaction restore failed: %s"),
					*Source.SourceId.ToString(),
					*Error);
				return false;
			}
		}
		else
		{
			FFragmentSourceMask RuntimeMask = Source.Mask;
			if (Existing)
			{
				RuntimeMask.SolidMask = Existing->GetRuntimeMask();
			}
			if (!Candidate->Initialize(
				MatterFlux::Reaction::FSourceRuntimeSettings(),
				RuntimeMask,
				*Rule,
				EventSeed,
				Error))
			{
				return false;
			}
		}
		Runtime = Candidate.Get();
	}

	const FTransform SourceWorldTransform =
		Source.Transform * GetActorTransform();
	if (!SourceWorldTransform.IsValid())
	{
		return false;
	}
	const FVector Local =
		SourceWorldTransform.InverseTransformPosition(WorldLocation);
	const FIntPoint RequestedCell(
		FMath::FloorToInt(
			Local.X / Source.Mask.CellSize
				+ static_cast<double>(Source.Mask.Width) * 0.5),
		FMath::FloorToInt(
			Local.Z / Source.Mask.CellSize
				+ static_cast<double>(Source.Mask.Height) * 0.5));
	if (!Runtime->ActivateNearest(RequestedCell, StimulusMaterial))
	{
		return false;
	}
	if (Candidate)
	{
		ActiveSourceReactions.Add(
			Source.SourceId,
			MoveTemp(Candidate));
		Runtime = ActiveSourceReactions.FindChecked(Source.SourceId).Get();
	}
	SynchronizeLogicalSourceReactionState(
		Source.SourceId,
		Source,
		*Runtime,
		true);
	bSourceReactionVisualDirty = true;
	return true;
}

bool AMatterFluxPlayableWorldActor::ApplyMaterialStimulusToDynamicAggregateSource(
	AFragment2DActor& CarrierActor,
	const FGuid& SourceId,
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	const TWeakObjectPtr<AFragment2DActor>* RegisteredCarrier =
		DynamicAggregateCarriers.Find(SourceId);
	if (!HasAuthority()
		|| CarrierActor.GetWorld() != GetWorld()
		|| !RegisteredCarrier
		|| RegisteredCarrier->Get() != &CarrierActor
		|| WorldLocation.ContainsNaN()
		|| StimulusMaterial.IsNone())
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
		FindFragmentSourceDefinition(SourceId);
	FFragmentAggregateSourceState CarrierState;
	if (!Source
		|| !CarrierActor.GetAggregateSourceState(SourceId, CarrierState)
		|| CarrierState.MaterialId != Source->MaterialId)
	{
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (!Registry.IsValid())
	{
		return false;
	}
	const FMatterFluxReactionDefinition* Rule = nullptr;
	TArray<FName> RuleIds;
	Registry->Reactions.GetKeys(RuleIds);
	RuleIds.Sort(FNameLexicalLess());
	for (const FName RuleId : RuleIds)
	{
		const FMatterFluxReactionDefinition* CandidateRule =
			Registry->Reactions.Find(RuleId);
		if (CandidateRule
			&& CandidateRule->Kind
				== FMatterFluxReactionDefinition::EKind::Propagating
			&& CandidateRule->InputA == CarrierState.MaterialId
			&& CandidateRule->InputB == StimulusMaterial)
		{
			Rule = CandidateRule;
			break;
		}
	}
	if (!Rule)
	{
		return false;
	}

	TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime> Candidate;
	TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime>* RuntimePtr =
		ActiveSourceReactions.Find(SourceId);
	MatterFlux::Reaction::FSourceReactionRuntime* Runtime =
		RuntimePtr ? RuntimePtr->Get() : nullptr;
	if (!Runtime)
	{
		Candidate = MakeUnique<
			MatterFlux::Reaction::FSourceReactionRuntime>();
		FString Error;
		if (CarrierState.bHasReactionState)
		{
			MatterFlux::Reaction::FSourceRuntimeSnapshot Snapshot;
			Snapshot.ReactionState.RuleId = CarrierState.ReactionRuleId;
			Snapshot.ReactionState.Width = CarrierState.SourceMask.Width;
			Snapshot.ReactionState.Height = CarrierState.SourceMask.Height;
			Snapshot.ReactionState.Seed = CarrierState.ReactionSeed;
			Snapshot.ReactionState.Tick = CarrierState.ReactionTick;
			Snapshot.ReactionState.InputMask =
				CarrierState.SourceMask.SolidMask;
			Snapshot.ReactionState.OutputMask =
				CarrierState.OutputMask.SolidMask;
			Snapshot.ReactionState.ActiveMask =
				CarrierState.ActiveMask.SolidMask;
			Snapshot.ReactionAccumulator =
				CarrierState.ReactionAccumulator;
			Snapshot.TotalMaterialEmissionCount =
				CarrierState.TotalMaterialEmissionCount;
			if (!Candidate->RestoreState(
				MatterFlux::Reaction::FSourceRuntimeSettings(),
				Snapshot,
				*Rule,
				Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Dynamic aggregate source %s reaction restore failed: %s"),
					*SourceId.ToString(),
					*Error);
				return false;
			}
		}
		else if (!Candidate->Initialize(
			MatterFlux::Reaction::FSourceRuntimeSettings(),
			CarrierState.SourceMask,
			*Rule,
			EventSeed,
			Error))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Dynamic aggregate source %s reaction initialization failed: %s"),
				*SourceId.ToString(),
				*Error);
			return false;
		}
		Runtime = Candidate.Get();
	}

	FTransform SourceWorldTransform;
	if (!CarrierActor.GetAggregateSourceWorldTransform(
		SourceId,
		SourceWorldTransform))
	{
		return false;
	}
	const FVector Local = SourceWorldTransform.InverseTransformPosition(
		WorldLocation);
	const FIntPoint RequestedCell(
		FMath::FloorToInt(
			Local.X / CarrierState.SourceMask.CellSize
				+ static_cast<double>(CarrierState.SourceMask.Width) * 0.5),
		FMath::FloorToInt(
			Local.Z / CarrierState.SourceMask.CellSize
				+ static_cast<double>(CarrierState.SourceMask.Height) * 0.5));
	if (!Runtime->ActivateNearest(RequestedCell, StimulusMaterial))
	{
		return false;
	}
	if (Candidate)
	{
		ActiveSourceReactions.Add(SourceId, MoveTemp(Candidate));
		Runtime = ActiveSourceReactions.FindChecked(SourceId).Get();
	}
	if (!SynchronizeLogicalSourceReactionState(
		SourceId,
		*Source,
		*Runtime,
		true))
	{
		return false;
	}
	bSourceReactionVisualDirty = true;
	return true;
}

bool AMatterFluxPlayableWorldActor::
	SynchronizeLogicalSourceReactionState(
		const FGuid& SourceId,
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const MatterFlux::Reaction::FSourceReactionRuntime& Runtime,
		const bool bPublish)
{
	FFragment2DSourceStreamingState& State =
		StreamedFragmentSourceStates.FindOrAdd(SourceId);
	if (!State.CaptureReactionState(Runtime))
	{
		return false;
	}
	State.Revision = FMath::Max(State.Revision, 0);
	LogicalSourceReactionIndex.ApplySnapshot(
		SourceId,
		State.bHasReactionState,
		State.ReactionState.ActiveMask);
	const FName OutputMaterial = Runtime.GetRule()
		? Runtime.GetRule()->OutputA
		: NAME_None;
	FLinearColor OutputColor(0.08f, 0.07f, 0.06f);
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (Registry.IsValid())
	{
		if (const FMatterFluxMaterialDefinition* Material =
			Registry->Materials.Find(OutputMaterial))
		{
			OutputColor = Material->Color;
		}
	}
	if (FragmentSourceProxy)
	{
		if (FragmentSourceProxy->ApplySourceState(
			SourceId,
			State.GetRuntimeMask(),
			State.ReactionState.OutputMask,
			OutputMaterial,
			OutputColor,
			Runtime.IsActive())
			== EMatterFluxFragmentSourceProxyApplyResult::Invalid)
		{
			return false;
		}
	}
	if (TWeakObjectPtr<AFragment2DActor>* CarrierPtr =
		DynamicAggregateCarriers.Find(SourceId))
	{
		if (AFragment2DActor* Carrier = CarrierPtr->Get())
		{
			if (!Carrier->ApplyAggregateSourceStreamingState(
				SourceId,
				State,
				OutputMaterial,
				OutputColor))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Dynamic aggregate rejected reaction update for source %s"),
					*SourceId.ToString());
			}
		}
		else
		{
			DynamicAggregateCarriers.Remove(SourceId);
		}
	}
	if (bPublish && !PublishFragmentSourceState(SourceId, State))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Logical source %s reaction state could not be published"),
			*SourceId.ToString());
		return false;
	}
	return true;
}

int32 AMatterFluxPlayableWorldActor::ApplyMaterialStimulusToLogicalFragmentSourcesInCone(
	const FVector& Start,
	const FVector& Direction,
	const float Range,
	const float StartRadius,
	const float EndRadius,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| Start.ContainsNaN()
		|| Direction.ContainsNaN()
		|| !FMath::IsFinite(Range)
		|| !FMath::IsFinite(StartRadius)
		|| !FMath::IsFinite(EndRadius)
		|| Range <= 0.0f
		|| StartRadius <= 0.0f
		|| EndRadius < StartRadius)
	{
		return 0;
	}
	const FVector Forward = Direction.GetSafeNormal();
	if (Forward.IsNearlyZero())
	{
		return 0;
	}
	int32 Activated = 0;
	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>
		Candidates;
	GatherLogicalFragmentSourceCandidates(
		BuildConeWorldBounds(
			Start,
			Forward,
			Range,
			StartRadius,
			EndRadius),
		Candidates);
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
		: Candidates)
	{
		if (!Source
			|| GeneratedFragmentSources.Contains(Source->SourceId)
			|| ActiveSourceReactions.Contains(Source->SourceId)
			|| RemovedFragmentSourceIds.Contains(Source->SourceId))
		{
			continue;
		}
		const FBox SourceBounds = BuildFragmentSourceLocalBounds(*Source)
			.TransformBy(GetActorTransform().ToMatrixWithScale());
		const float Along = FVector::DotProduct(
			SourceBounds.GetCenter() - Start,
			Forward);
		if (Along < 0.0f || Along > Range)
		{
			continue;
		}
		const FVector Centerline = Start + Forward * Along;
		const float Radius = FMath::Lerp(
			StartRadius,
			EndRadius,
			Along / Range);
		if (SourceBounds.ComputeSquaredDistanceToPoint(Centerline)
			<= FMath::Square(Radius)
			&& ApplyMaterialStimulusToLogicalFragmentSource(
				*Source,
				SourceBounds.GetClosestPointTo(Centerline),
				StimulusMaterial,
				EventSeed ^ static_cast<int32>(GetTypeHash(Source->SourceId))))
		{
			++Activated;
		}
	}
	return Activated;
}

int32 AMatterFluxPlayableWorldActor::ApplyMaterialStimulusToLogicalFragmentSourcesInBounds(
	const FBox& Bounds,
	const FVector& StimulusPoint,
	const FName StimulusMaterial,
	const int32 EventSeed,
	const int32 MaxActivations)
{
	if (!HasAuthority()
		|| !Bounds.IsValid
		|| StimulusPoint.ContainsNaN()
		|| MaxActivations <= 0)
	{
		return 0;
	}
	int32 Activated = 0;
	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>
		Candidates;
	GatherLogicalFragmentSourceCandidates(Bounds, Candidates);
	Candidates.Sort([](
		const MatterFlux::PlayableLevel::FLevelFragmentSource& A,
		const MatterFlux::PlayableLevel::FLevelFragmentSource& B)
	{
		return A.SourceId.ToString(EGuidFormats::Digits)
			< B.SourceId.ToString(EGuidFormats::Digits);
	});
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
		: Candidates)
	{
		if (Activated >= MaxActivations)
		{
			break;
		}
		if (!Source
			|| GeneratedFragmentSources.Contains(Source->SourceId)
			|| ActiveSourceReactions.Contains(Source->SourceId)
			|| RemovedFragmentSourceIds.Contains(Source->SourceId))
		{
			continue;
		}
		const FBox SourceBounds = BuildFragmentSourceLocalBounds(*Source)
			.TransformBy(GetActorTransform().ToMatrixWithScale());
		if (SourceBounds.Intersect(Bounds)
			&& ApplyMaterialStimulusToLogicalFragmentSource(
			*Source,
			SourceBounds.GetClosestPointTo(StimulusPoint),
			StimulusMaterial,
			EventSeed ^ static_cast<int32>(GetTypeHash(Source->SourceId))))
		{
			++Activated;
		}
	}
	return Activated;
}

bool AMatterFluxPlayableWorldActor::ApplyMaterialStimulusToFirstGeneratedTree(
	const int32 EventSeed)
{
	if (!HasAuthority())
	{
		return false;
	}
	FVector Focus = FVector::ZeroVector;
	if (const APlayerController* PlayerController =
		GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (const APawn* Pawn = PlayerController->GetPawn())
		{
			Focus = Pawn->GetActorLocation();
		}
	}

	const MatterFlux::PlayableLevel::FLevelFragmentSource* BestLogicalTree =
		nullptr;
	double BestDistanceSquared = TNumericLimits<double>::Max();
	FString BestId;
	for (const TPair<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
		: FragmentSourceChunks)
	{
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Pair.Value)
		{
			if (Source.MaterialId != TEXT("wood")
				|| RemovedFragmentSourceIds.Contains(Source.SourceId)
				|| GeneratedFragmentSources.Contains(Source.SourceId)
				|| !Source.Mask.IsValid())
			{
				continue;
			}
			const FVector WorldLocation =
				(Source.Transform * GetActorTransform()).GetLocation();
			const double DistanceSquared = FVector::DistSquared(
				WorldLocation,
				Focus);
			const FString CandidateId =
				Source.SourceId.ToString(EGuidFormats::Digits);
			if (!BestLogicalTree
				|| DistanceSquared < BestDistanceSquared
				|| (DistanceSquared == BestDistanceSquared
					&& CandidateId < BestId))
			{
				BestLogicalTree = &Source;
				BestDistanceSquared = DistanceSquared;
				BestId = CandidateId;
			}
		}
	}
	if (!BestLogicalTree)
	{
		return false;
	}
	FName StimulusMaterial = NAME_None;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (Registry.IsValid())
	{
		if (const FMatterFluxReactionDefinition* Rule =
			MatterFlux::Reaction::FMaterialReactionEngine::
				FindPropagatingRule(
					*Registry, BestLogicalTree->MaterialId))
		{
			StimulusMaterial = Rule->InputB;
		}
	}
	if (StimulusMaterial.IsNone())
	{
		return false;
	}
	const FVector LocalStimulusPoint(
		0.0f,
		0.0f,
		(-static_cast<float>(BestLogicalTree->Mask.Height) * 0.5f
			+ 0.5f)
			* BestLogicalTree->Mask.CellSize);
	const FVector WorldStimulusPoint =
		(BestLogicalTree->Transform * GetActorTransform()).TransformPosition(
			LocalStimulusPoint);
	const bool bActivated = ApplyMaterialStimulusToLogicalFragmentSource(
		*BestLogicalTree,
		WorldStimulusPoint,
		StimulusMaterial,
		EventSeed);
	if (bActivated)
	{
		ApplyMaterialStimulusToGroundAtWorldLocation(
			WorldStimulusPoint,
			StimulusMaterial,
			EventSeed ^ 0x47524f55);
	}
	return bActivated;
}

int32 AMatterFluxPlayableWorldActor::GetReactingSourceCount() const
{
	int32 Count = LogicalSourceReactionIndex.Num();
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		Count += IsValid(Pair.Value)
			&& Pair.Value->IsReacting()
				? 1
				: 0;
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			Count += It->IsRootReacting() ? 1 : 0;
		}
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::GetReactionOutputCellCount() const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		if (!GeneratedFragmentSources.Contains(Pair.Key)
			&& Pair.Value.bHasReactionState)
		{
			for (const uint8 Value : Pair.Value.ReactionState.OutputMask)
			{
				Count += Value != 0 ? 1 : 0;
			}
		}
	}
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		if (IsValid(Pair.Value))
		{
			Count += Pair.Value->GetOutputCellCount();
		}
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			Count += It->GetRootReactionOutputCellCount();
		}
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::
	GetLogicalReactionOutputCellCount(const FName MaterialId) const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
			FindFragmentSourceDefinition(Pair.Key);
		if (!Source
			|| Source->MaterialId != MaterialId
			|| !Pair.Value.bHasReactionState)
		{
			continue;
		}
		for (const uint8 Value : Pair.Value.ReactionState.OutputMask)
		{
			Count += Value != 0 ? 1 : 0;
		}
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::
	GetLogicalReactionInputCellCount(const FName MaterialId) const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
			FindFragmentSourceDefinition(Pair.Key);
		if (!Source
			|| Source->MaterialId != MaterialId
			|| !Pair.Value.bHasReactionState)
		{
			continue;
		}
		for (const uint8 Value : Pair.Value.GetRuntimeMask())
		{
			Count += Value != 0 ? 1 : 0;
		}
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::
	GetLogicalReactionActiveCellCount(const FName MaterialId) const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
			FindFragmentSourceDefinition(Pair.Key);
		if (!Source
			|| Source->MaterialId != MaterialId
			|| !Pair.Value.bHasReactionState)
		{
			continue;
		}
		for (const uint8 Value : Pair.Value.ReactionState.ActiveMask)
		{
			Count += Value != 0 ? 1 : 0;
		}
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::
	GetLogicalReactionMaterialEmissionCount() const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		Count += Pair.Value.bHasReactionState
			? Pair.Value.TotalMaterialEmissionCount
			: 0;
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::
	GetLogicalReactionProjectionOverlapCellCount() const
{
	return FragmentSourceProxy
		? FragmentSourceProxy->GetInputOutputOverlapCellCount()
		: 0;
}

int32 AMatterFluxPlayableWorldActor::
	GetStandaloneTreeReactionOutputProjectionCount() const
{
	return FragmentSourceProxy
		? FragmentSourceProxy->GetStandaloneTreeOutputProjectionCount()
		: 0;
}

int32 AMatterFluxPlayableWorldActor::
	GetLogicalReactionFlameInstanceCount() const
{
	return SourceFlameInstances
		? SourceFlameInstances->GetInstanceCount()
		: 0;
}

int32 AMatterFluxPlayableWorldActor::GetReactedGroundCellCount() const
{
	return GroundReaction
		? GroundReaction->CountOutputCells()
		: 0;
}

int32 AMatterFluxPlayableWorldActor::GetActiveGroundReactionCellCount() const
{
	if (!GroundReaction)
	{
		return 0;
	}
	TArray<int32> ActiveCells;
	GroundReaction->GatherActiveCellIndices(ActiveCells);
	return ActiveCells.Num();
}

void AMatterFluxPlayableWorldActor::InitializeGroundReaction(
	const FMatterFluxContentRegistry& Registry,
	const MatterFlux::PlayableLevel::FLevelLayout& Layout)
{
	const MatterFlux::PlayableLevel::FLevelTerrain& Terrain =
		Layout.Terrain;
	if (!Terrain.IsValid()
		|| Terrain.Width != MatterFlux::PlayableLevel::TerrainCellsX
		|| Terrain.Height != MatterFlux::PlayableLevel::TerrainCellsY)
	{
		return;
	}
	const FMatterFluxReactionDefinition* GrassRule =
		MatterFlux::Reaction::FMaterialReactionEngine::FindPropagatingRule(
			Registry, TEXT("grassland"));
	if (!GrassRule)
	{
		return;
	}

	FFragmentSourceMask GroundMask;
	GroundMask.Width = MatterFlux::PlayableLevel::TerrainCellsX;
	GroundMask.Height = MatterFlux::PlayableLevel::TerrainCellsY;
	GroundMask.CellSize =
		MatterFlux::PlayableLevel::TerrainCellSize;
	GroundMask.MinFragmentAreaPixels = 1;
	GroundMask.MaxFragmentsPerBreak = 16;
	GroundMask.SolidMask.Init(
		1,
		GroundMask.Width * GroundMask.Height);

	TArray<FVector> NextGroundSurfacePositions;
	NextGroundSurfacePositions.Reserve(Terrain.Heights.Num());
	for (int32 Y = 0; Y < Terrain.Height; ++Y)
	{
		for (int32 X = 0; X < Terrain.Width; ++X)
		{
			NextGroundSurfacePositions.Add(FVector(
				Terrain.FirstCellCenter.X + X * Terrain.CellSize,
				Terrain.FirstCellCenter.Y + Y * Terrain.CellSize,
				Terrain.HeightAt(X, Y)));
		}
	}
	if (const MatterFlux::PlayableLevel::FLevelLayer* Stream =
		Layout.FindLayer(TEXT("Stream")))
	{
		const FVector Origin = NextGroundSurfacePositions[0];
		for (const FTransform& Transform : Stream->Instances)
		{
			const int32 X = FMath::RoundToInt(
				(Transform.GetLocation().X - Origin.X)
					/ MatterFlux::PlayableLevel::TerrainCellSize);
			const int32 Y = FMath::RoundToInt(
				(Transform.GetLocation().Y - Origin.Y)
					/ MatterFlux::PlayableLevel::TerrainCellSize);
			if (X >= 0 && X < GroundMask.Width
				&& Y >= 0 && Y < GroundMask.Height)
			{
				GroundMask.SolidMask[
					Y * GroundMask.Width + X] = 0;
			}
		}
	}

	auto NextGroundReaction = MakeUnique<
		MatterFlux::Reaction::FGroundReactionRuntime>();
	FString RuntimeError;
	if (!NextGroundReaction->Initialize(
		MakeGroundReactionRuntimeSettings(),
		GroundMask,
		*GrassRule,
		MapSeed ^ 0x47524153,
		RuntimeError))
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Ground reaction runtime initialization failed: %s"),
			*RuntimeError);
		return;
	}
	if (HasAuthority())
	{
		DestroyGroundStateChunks();
	}
	GroundReaction = MoveTemp(NextGroundReaction);
	GroundSurfacePositions = MoveTemp(NextGroundSurfacePositions);
	SourcesThatActivatedGround.Reset();
	GroundReactionVisualAccumulator = 0.0f;
	bGroundReactionVisualDirty = true;
	bGroundReactionVisualNeedsFullRebuild = true;
	PendingGroundReactionVisualCellIndices.Reset();
	if (HasAuthority())
	{
		InitializeGroundStateChunks();
	}
	EnsureGroundReactionVisuals(Registry);
}

int32 AMatterFluxPlayableWorldActor::ApplyMaterialStimulusAtWorldLocation(
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed,
	const float ContactRadius)
{
	if (!HasAuthority()
		|| StimulusMaterial.IsNone()
		|| WorldLocation.ContainsNaN())
	{
		return 0;
	}

	const float Radius = FMath::Clamp(
		ContactRadius > 0.0f
			? ContactRadius
			: MaterialSimulationCellSize * 0.52f,
		2.0f,
		300.0f);
	const FBox ContactBounds = FBox::BuildAABB(
		WorldLocation,
		FVector(Radius));
	int32 Activated = ApplyMaterialStimulusToLogicalFragmentSourcesInBounds(
		ContactBounds,
		WorldLocation,
		StimulusMaterial,
		EventSeed,
		MaxMaterialSourceReactionsPerFrame);

	if (UFragmentSimulationSubsystem* FragmentSubsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr)
	{
		TArray<AFragment2DSourceActor*> Sources;
		FragmentSubsystem->GatherSourcesInBounds(ContactBounds, Sources);
		for (AFragment2DSourceActor* Source : Sources)
		{
			if (Activated >= MaxMaterialSourceReactionsPerFrame)
			{
				break;
			}
			if (!IsValid(Source)
				|| Source->IsActorBeingDestroyed()
				|| Source->bBroken
				|| !Source->HasReactionRule())
			{
				continue;
			}
			const FBox SourceBounds = Source->GetCanonicalWorldBounds();
			if (SourceBounds.IsValid
				&& SourceBounds.Intersect(ContactBounds)
				&& Source->ApplyMaterialStimulusAtWorldLocation(
					SourceBounds.GetClosestPointTo(WorldLocation),
					StimulusMaterial,
					EventSeed ^ static_cast<int32>(
						GetTypeHash(Source->SourceId))))
			{
				++Activated;
			}
		}
	}

	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			if (Activated >= MaxMaterialSourceReactionsPerFrame)
			{
				break;
			}
			AFragment2DActor* Carrier = *It;
			if (!IsValid(Carrier)
				|| Carrier->IsActorBeingDestroyed()
				|| !Carrier->GetReactiveWorldBounds().Intersect(ContactBounds))
			{
				continue;
			}
			if (Carrier->ApplyMaterialStimulusAtWorldLocation(
				WorldLocation,
				StimulusMaterial,
				EventSeed ^ static_cast<int32>(GetTypeHash(
					Carrier->SpawnPayload.FragmentId))))
			{
				++Activated;
			}
		}
	}

	if (ApplyMaterialStimulusToGroundAtWorldLocation(
			WorldLocation,
			StimulusMaterial,
			EventSeed))
	{
		++Activated;
	}
	return Activated;
}

bool AMatterFluxPlayableWorldActor::ApplyMaterialStimulusToGroundAtWorldLocation(
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !GroundReaction
		|| GroundSurfacePositions.IsEmpty()
		|| WorldLocation.ContainsNaN()
		|| !GetActorTransform().IsValid())
	{
		return false;
	}
	const FVector Local =
		GetActorTransform().InverseTransformPosition(
			WorldLocation);
	const FVector Origin = GroundSurfacePositions[0];
	const double RoundedX = FMath::RoundToDouble(
		(Local.X - Origin.X)
			/ MatterFlux::PlayableLevel::TerrainCellSize);
	const double RoundedY = FMath::RoundToDouble(
		(Local.Y - Origin.Y)
			/ MatterFlux::PlayableLevel::TerrainCellSize);
	if (!FMath::IsFinite(RoundedX)
		|| !FMath::IsFinite(RoundedY)
		|| RoundedX < MIN_int32
		|| RoundedX > MAX_int32
		|| RoundedY < MIN_int32
		|| RoundedY > MAX_int32)
	{
		return false;
	}
	const FIntPoint Requested(
		static_cast<int32>(RoundedX),
		static_cast<int32>(RoundedY));
	const int32 RequestedIndex =
		Requested.Y * MatterFlux::PlayableLevel::TerrainCellsX
			+ Requested.X;
	if (!GroundSurfacePositions.IsValidIndex(RequestedIndex))
	{
		return false;
	}
	const FVector GroundWorld = GetActorTransform().TransformPosition(
		GroundSurfacePositions[RequestedIndex]);
	EMatterFluxMaterialPhase StimulusPhase =
		EMatterFluxMaterialPhase::Gas;
	if (const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry())
	{
		if (const FMatterFluxMaterialDefinition* Material =
			Registry->Materials.Find(StimulusMaterial))
		{
			StimulusPhase = Material->Phase;
		}
	}
	const MatterFlux::Material::FMaterialContactGeometry ContactGeometry =
		MatterFlux::Material::BuildMaterialContactGeometry(
			StimulusPhase,
			MaterialSimulationCellSize,
			MaterialLiquidColumnHeight);
	if (!ContactGeometry.IsValid())
	{
		return false;
	}
	const float VerticalTolerance =
		ContactGeometry.GroundVerticalTolerance;
	if (FMath::Abs(WorldLocation.Z - GroundWorld.Z) > VerticalTolerance)
	{
		return false;
	}
	const FMatterFluxReactionDefinition* Rule =
		GroundReaction->GetRule();
	if (!Rule)
	{
		return false;
	}
	if (Rule->InputB != StimulusMaterial)
	{
		return false;
	}
	FIntPoint ActivatedCell = Requested;
	const bool bActivated = GroundReaction->ActivateNearestInput(
		Requested,
		StimulusMaterial,
		5,
		ActivatedCell);
	if (bActivated)
	{
		PendingGroundReactionVisualCellIndices.Add(
			ActivatedCell.Y * MatterFlux::PlayableLevel::TerrainCellsX
				+ ActivatedCell.X);
		bGroundReactionVisualDirty = true;
		if (!bBatchingGroundReactions)
		{
			PublishGroundReactionState();
		}
	}
	(void)EventSeed;
	return bActivated;
}

void AMatterFluxPlayableWorldActor::AdvanceGroundReaction(
	const float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxReaction);
	const float ClampedDelta =
		FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (HasAuthority() && GroundReaction)
	{
		SCOPE_CYCLE_COUNTER(STAT_MatterFluxGroundReactionSimulation);
		const MatterFlux::Reaction::FGroundAdvanceResult Result =
			GroundReaction->AdvanceAuthority(ClampedDelta);
		if (Result.bStateChanged)
		{
			for (const int32 CellIndex : Result.ChangedCellIndices)
			{
				PendingGroundReactionVisualCellIndices.Add(CellIndex);
			}
			bGroundReactionVisualDirty = true;
			PublishGroundReactionState();
		}
	}
	GroundReactionVisualAccumulator += ClampedDelta;
	if (bGroundReactionVisualDirty
		&& GroundReactionVisualAccumulator >= 0.2f)
	{
		GroundReactionVisualAccumulator = 0.0f;
		if (bGroundReactionVisualNeedsFullRebuild)
		{
			RebuildGroundReactionVisualization();
		}
		else
		{
			TArray<int32> ChangedCellIndices;
			ChangedCellIndices.Reserve(
				PendingGroundReactionVisualCellIndices.Num());
			for (const int32 CellIndex
				: PendingGroundReactionVisualCellIndices)
			{
				ChangedCellIndices.Add(CellIndex);
			}
			ChangedCellIndices.Sort();
			ApplyGroundReactionVisualChanges(ChangedCellIndices);
		}
	}
}

void AMatterFluxPlayableWorldActor::EnsureGroundReactionVisuals(
	const FMatterFluxContentRegistry& Registry)
{
	const auto CreateLayer =
		[this](
			TObjectPtr<UInstancedStaticMeshComponent>& Component,
			const FName Name)
		{
			if (Component)
			{
				return;
			}
			Component = NewObject<UInstancedStaticMeshComponent>(
				this,
				Name);
			Component->SetupAttachment(SceneRoot);
			Component->SetStaticMesh(CubeMesh);
			Component->SetCollisionEnabled(
				ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
			Component->SetRemoveSwap();
			AddInstanceComponent(Component);
			Component->RegisterComponent();
		};
	CreateLayer(
		GroundOutputInstances,
		TEXT("GroundReactionOutput"));
	CreateLayer(
		GroundFlameInstances,
		TEXT("GroundReactionFlames"));
	CreateLayer(
		GroundSmokeInstances,
		TEXT("GroundReactionSmoke"));
	// Smoke is rendered by the shared world pool. Keep this legacy component
	// empty so old serialized worlds cannot display one static grey cube per cell.
	GroundSmokeInstances->SetVisibility(false);
	GroundSmokeInstances->ClearInstances();

	const FMatterFluxReactionDefinition* Rule =
		GroundReaction
			? GroundReaction->GetRule()
			: nullptr;
	if (!Rule || !VoxelColorMaterialTemplate)
	{
		return;
	}
	const auto ApplyMaterial =
		[this, &Registry](
			UInstancedStaticMeshComponent* Component,
			TObjectPtr<UMaterialInstanceDynamic>& Instance,
			const FName MaterialId,
			const FLinearColor Fallback)
		{
			if (!Instance)
			{
				Instance = UMaterialInstanceDynamic::Create(
					VoxelColorMaterialTemplate,
					this);
				Component->SetMaterial(0, Instance);
			}
			const FMatterFluxMaterialDefinition* Material =
				Registry.Materials.Find(MaterialId);
			Instance->SetVectorParameterValue(
				TEXT("Color"),
				Material ? Material->Color : Fallback);
		};
	ApplyMaterial(
		GroundOutputInstances,
		GroundOutputMaterial,
		Rule->OutputA,
		FLinearColor(0.24f, 0.23f, 0.21f));
	ApplyMaterial(
		GroundFlameInstances,
		GroundStimulusMaterial,
		Rule->InputB,
		FLinearColor(1.0f, 0.22f, 0.01f));
	ApplyMaterial(
		GroundSmokeInstances,
		GroundSmokeMaterial,
		Rule->EmissionMaterial,
		FLinearColor(0.15f, 0.16f, 0.18f));
	if (GroundSmokeMaterial)
	{
		const FMatterFluxMaterialDefinition* SmokeDefinition =
			Registry.Materials.Find(Rule->EmissionMaterial);
		const FLinearColor SmokeColor = SmokeDefinition
			? SmokeDefinition->Color
			: FLinearColor(0.15f, 0.16f, 0.18f);
		GroundSmokeMaterial->SetVectorParameterValue(
			TEXT("Color"),
			FLinearColor::LerpUsingHSV(
				SmokeColor,
				FLinearColor(0.48f, 0.51f, 0.56f, 1.0f),
				0.65f));
	}
}

void AMatterFluxPlayableWorldActor::
	RebuildGroundReactionVisualization()
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxGroundReactionVisuals);
	if (GetNetMode() == NM_DedicatedServer)
	{
		bGroundReactionVisualDirty = false;
		bGroundReactionVisualNeedsFullRebuild = false;
		PendingGroundReactionVisualCellIndices.Reset();
		return;
	}
	if (!GroundOutputInstances
		|| !GroundFlameInstances
		|| !GroundSmokeInstances
		|| !GroundReaction
		|| GroundSurfacePositions.Num()
			!= GroundReaction->GetOutputMask().Num()
		|| GroundSurfacePositions.Num()
			!= GroundReaction->GetActiveMask().Num())
	{
		return;
	}
	TArray<FTransform> OutputTransforms;
	TArray<FTransform> FlameTransforms;
	TArray<int32> OutputCellIndices;
	TArray<int32> ActiveCellIndices;
	TArray<int32> OutputVisualCells;
	TArray<int32> FlameVisualCells;
	TArray<int32> SmokeVisualCells;
	const FMatterFluxReactionDefinition* Rule = GroundReaction->GetRule();
	const bool bRenderFlames = Rule
		&& MatterFlux::Reaction::UsesFlamePresentation(*Rule);
	constexpr int32 ChunkSize = 64;
	const FIntPoint FirstWorldCell(
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.X
				/ TerrainHeightField.CellSize),
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.Y
				/ TerrainHeightField.CellSize));
	const FIntPoint FirstWorldChunk(
		FMath::FloorToInt(
			static_cast<double>(FirstWorldCell.X) / ChunkSize),
		FMath::FloorToInt(
			static_cast<double>(FirstWorldCell.Y) / ChunkSize));
	const FIntPoint RuntimeChunkCount(
		FMath::DivideAndRoundUp(
			MatterFlux::PlayableLevel::TerrainCellsX,
			ChunkSize),
		FMath::DivideAndRoundUp(
			MatterFlux::PlayableLevel::TerrainCellsY,
			ChunkSize));
	TArray<FIntPoint> VisibleRuntimeChunks;
	VisibleRuntimeChunks.Reserve(ActiveTerrainChunks.Num());
	for (const FIntPoint WorldChunk : ActiveTerrainChunks)
	{
		const FIntPoint RuntimeChunk = WorldChunk - FirstWorldChunk;
		if (RuntimeChunk.X >= 0
			&& RuntimeChunk.Y >= 0
			&& RuntimeChunk.X < RuntimeChunkCount.X
			&& RuntimeChunk.Y < RuntimeChunkCount.Y)
		{
			VisibleRuntimeChunks.Add(RuntimeChunk);
		}
	}
	GroundReaction->GatherVisibleCellIndicesForChunks(
		VisibleRuntimeChunks,
		OutputCellIndices,
		ActiveCellIndices);
	OutputTransforms.Reserve(OutputCellIndices.Num());
	FlameTransforms.Reserve(FMath::Min(ActiveCellIndices.Num(), 256));
	for (const int32 CellIndex : OutputCellIndices)
	{
		if (!GroundSurfacePositions.IsValidIndex(CellIndex))
		{
			continue;
		}
		OutputTransforms.Emplace(
			FRotator::ZeroRotator,
			GroundSurfacePositions[CellIndex] + FVector(0.0f, 0.0f, 3.0f),
			FVector(0.61f, 0.61f, 0.06f));
		OutputVisualCells.Add(CellIndex);
	}
	for (const int32 CellIndex : ActiveCellIndices)
	{
		if (!GroundSurfacePositions.IsValidIndex(CellIndex))
		{
			continue;
		}
		const FVector Surface = GroundSurfacePositions[CellIndex];
		const float Jitter =
			static_cast<float>((CellIndex * 37) % 11) - 5.0f;
		if (SmokeVisualCells.Num() < 256)
		{
			SmokeVisualCells.Add(CellIndex);
		}
		if (bRenderFlames && FlameTransforms.Num() < 256)
		{
			FlameTransforms.Emplace(
				FRotator::ZeroRotator,
				Surface + FVector(Jitter, -Jitter, 20.0f),
				FVector(0.24f, 0.24f, 0.34f));
			FlameVisualCells.Add(CellIndex);
		}
	}
	const auto ReplaceInstances =
		[](UInstancedStaticMeshComponent* Component,
			const TArray<FTransform>& Transforms)
		{
			MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
				*Component,
				Transforms);
		};
	ReplaceInstances(
		GroundOutputInstances,
		OutputTransforms);
	ReplaceInstances(
		GroundFlameInstances,
		FlameTransforms);
	ReplaceInstances(
		GroundSmokeInstances,
		TArray<FTransform>());
	GroundOutputCellByInstance = MoveTemp(OutputVisualCells);
	GroundFlameCellByInstance = MoveTemp(FlameVisualCells);
	GroundSmokeCellByInstance = MoveTemp(SmokeVisualCells);
	GroundOutputInstanceByCell.Reset();
	GroundFlameInstanceByCell.Reset();
	GroundSmokeInstanceByCell.Reset();
	for (int32 InstanceIndex = 0;
		InstanceIndex < GroundOutputCellByInstance.Num();
		++InstanceIndex)
	{
		GroundOutputInstanceByCell.Add(
			GroundOutputCellByInstance[InstanceIndex],
			InstanceIndex);
	}
	for (int32 InstanceIndex = 0;
		InstanceIndex < GroundFlameCellByInstance.Num();
		++InstanceIndex)
	{
		GroundFlameInstanceByCell.Add(
			GroundFlameCellByInstance[InstanceIndex],
			InstanceIndex);
	}
	GroundSmokeInstanceByCell.Reset();
	bGroundReactionVisualDirty = false;
	bGroundReactionVisualNeedsFullRebuild = false;
	PendingGroundReactionVisualCellIndices.Reset();
	RebuildGroundSmokeAnchors();
}

bool AMatterFluxPlayableWorldActor::IsGroundReactionCellVisible(
	const int32 CellIndex) const
{
	if (!GroundSurfacePositions.IsValidIndex(CellIndex)
		|| TerrainHeightField.CellSize <= 0.0f)
	{
		return false;
	}
	const FIntPoint FirstWorldCell(
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.X
				/ TerrainHeightField.CellSize),
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.Y
				/ TerrainHeightField.CellSize));
	const FIntPoint RuntimeCell(
		CellIndex % MatterFlux::PlayableLevel::TerrainCellsX,
		CellIndex / MatterFlux::PlayableLevel::TerrainCellsX);
	const FIntPoint WorldCell = FirstWorldCell + RuntimeCell;
	const FIntPoint WorldChunk(
		FMath::FloorToInt(
			static_cast<double>(WorldCell.X)
				/ TerrainStreamingChunkSize),
		FMath::FloorToInt(
			static_cast<double>(WorldCell.Y)
				/ TerrainStreamingChunkSize));
	return ActiveTerrainChunks.Contains(WorldChunk);
}

void AMatterFluxPlayableWorldActor::ApplyGroundReactionVisualChanges(
	const TConstArrayView<int32> ChangedCellIndices)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxGroundReactionVisuals);
	if (!GroundOutputInstances
		|| !GroundFlameInstances
		|| !GroundSmokeInstances
		|| !GroundReaction)
	{
		bGroundReactionVisualNeedsFullRebuild = true;
		return;
	}
	const TArray<uint8>& OutputMask = GroundReaction->GetOutputMask();
	const TArray<uint8>& ActiveMask = GroundReaction->GetActiveMask();
	const FMatterFluxReactionDefinition* Rule = GroundReaction->GetRule();
	const bool bRenderFlames = Rule
		&& MatterFlux::Reaction::UsesFlamePresentation(*Rule);
	const auto RemoveCellInstance =
		[this](
			UInstancedStaticMeshComponent& Component,
			TMap<int32, int32>& InstanceByCell,
			TArray<int32>& CellByInstance,
			const int32 CellIndex)
		{
			const int32* ExistingIndex = InstanceByCell.Find(CellIndex);
			if (!ExistingIndex)
			{
				return;
			}
			const int32 InstanceIndex = *ExistingIndex;
			const int32 LastInstanceIndex = CellByInstance.Num() - 1;
			if (!CellByInstance.IsValidIndex(InstanceIndex)
				|| !Component.RemoveInstance(InstanceIndex))
			{
				bGroundReactionVisualNeedsFullRebuild = true;
				return;
			}
			InstanceByCell.Remove(CellIndex);
			if (InstanceIndex != LastInstanceIndex)
			{
				const int32 MovedCell = CellByInstance[LastInstanceIndex];
				CellByInstance[InstanceIndex] = MovedCell;
				InstanceByCell.FindOrAdd(MovedCell) = InstanceIndex;
			}
			CellByInstance.Pop(EAllowShrinking::No);
		};
	const auto AddCellInstance =
		[this](
			UInstancedStaticMeshComponent& Component,
			TMap<int32, int32>& InstanceByCell,
			TArray<int32>& CellByInstance,
			const int32 CellIndex,
			const FTransform& Transform)
		{
			if (InstanceByCell.Contains(CellIndex))
			{
				return;
			}
			const int32 InstanceIndex = Component.AddInstance(Transform, false);
			if (InstanceIndex != CellByInstance.Num())
			{
				bGroundReactionVisualNeedsFullRebuild = true;
				return;
			}
			InstanceByCell.Add(CellIndex, InstanceIndex);
			CellByInstance.Add(CellIndex);
		};

	// Removals run before additions so expired particles immediately free the
	// fixed flame/smoke visual budget for newly active cells.
	for (const int32 CellIndex : ChangedCellIndices)
	{
		const bool bVisible = IsGroundReactionCellVisible(CellIndex);
		const bool bHasOutput = bVisible
			&& OutputMask.IsValidIndex(CellIndex)
			&& OutputMask[CellIndex] != 0;
		const bool bIsActive = bVisible
			&& ActiveMask.IsValidIndex(CellIndex)
			&& ActiveMask[CellIndex] != 0;
		if (!bHasOutput)
		{
			RemoveCellInstance(
				*GroundOutputInstances,
				GroundOutputInstanceByCell,
				GroundOutputCellByInstance,
				CellIndex);
		}
		if (!bIsActive || !bRenderFlames)
		{
			RemoveCellInstance(
				*GroundFlameInstances,
				GroundFlameInstanceByCell,
				GroundFlameCellByInstance,
				CellIndex);
		}
	}
	for (const int32 CellIndex : ChangedCellIndices)
	{
		if (!IsGroundReactionCellVisible(CellIndex)
			|| !GroundSurfacePositions.IsValidIndex(CellIndex)
			|| !OutputMask.IsValidIndex(CellIndex)
			|| OutputMask[CellIndex] == 0)
		{
			continue;
		}
		AddCellInstance(
			*GroundOutputInstances,
			GroundOutputInstanceByCell,
			GroundOutputCellByInstance,
			CellIndex,
			FTransform(
				FRotator::ZeroRotator,
				GroundSurfacePositions[CellIndex]
					+ FVector(0.0f, 0.0f, 3.0f),
				FVector(0.61f, 0.61f, 0.06f)));
	}

	TArray<FIntPoint> VisibleRuntimeChunks;
	constexpr int32 ChunkSize = 64;
	const FIntPoint FirstWorldCell(
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.X
				/ TerrainHeightField.CellSize),
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.Y
				/ TerrainHeightField.CellSize));
	const FIntPoint FirstWorldChunk(
		FMath::FloorToInt(
			static_cast<double>(FirstWorldCell.X) / ChunkSize),
		FMath::FloorToInt(
			static_cast<double>(FirstWorldCell.Y) / ChunkSize));
	for (const FIntPoint WorldChunk : ActiveTerrainChunks)
	{
		VisibleRuntimeChunks.Add(WorldChunk - FirstWorldChunk);
	}
	TArray<int32> VisibleOutputCells;
	TArray<int32> VisibleActiveCells;
	GroundReaction->GatherVisibleCellIndicesForChunks(
		VisibleRuntimeChunks,
		VisibleOutputCells,
		VisibleActiveCells);
	GroundSmokeCellByInstance.Reset();
	GroundSmokeCellByInstance.Reserve(
		FMath::Min(VisibleActiveCells.Num(), 256));
	for (const int32 CellIndex : VisibleActiveCells)
	{
		if (!GroundSurfacePositions.IsValidIndex(CellIndex))
		{
			continue;
		}
		if (GroundSmokeCellByInstance.Num() < 256)
		{
			GroundSmokeCellByInstance.Add(CellIndex);
		}
		if (!bRenderFlames)
		{
			if (GroundSmokeCellByInstance.Num() >= 256)
			{
				break;
			}
			continue;
		}
		const FVector Surface = GroundSurfacePositions[CellIndex];
		const float Jitter =
			static_cast<float>((CellIndex * 37) % 11) - 5.0f;
		if (GroundFlameCellByInstance.Num() < 256)
		{
			AddCellInstance(
				*GroundFlameInstances,
				GroundFlameInstanceByCell,
				GroundFlameCellByInstance,
				CellIndex,
				FTransform(
					FRotator::ZeroRotator,
					Surface + FVector(Jitter, -Jitter, 20.0f),
					FVector(0.24f, 0.24f, 0.34f)));
		}
		if (GroundFlameCellByInstance.Num() >= 256)
		{
			break;
		}
	}
	PendingGroundReactionVisualCellIndices.Reset();
	bGroundReactionVisualDirty =
		bGroundReactionVisualNeedsFullRebuild;
	RebuildGroundSmokeAnchors();
}

int32 AMatterFluxPlayableWorldActor::
	GetReplicatedGroundReactionByteCount() const
{
	int32 Total = 0;
	for (const AMatterFluxGroundStateChunkActor* ChunkActor
		: GroundStateChunkActors)
	{
		if (IsValid(ChunkActor))
		{
			Total += ChunkActor->GetPayloadByteCount();
		}
	}
	return Total;
}

bool AMatterFluxPlayableWorldActor::ApplyReplicatedGroundStateChunk(
	const FMatterFluxGroundStateChunk& State)
{
	if (HasAuthority() || !GroundReaction)
	{
		return false;
	}
	FString Error;
	const MatterFlux::Reaction::EGroundChunkApplyResult Result =
		GroundReaction->ApplyReplicatedChunk(State, Error);
	if (Result == MatterFlux::Reaction::
		EGroundChunkApplyResult::Rejected)
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Rejected replicated ground chunk (%d,%d): %s"),
			State.ChunkCoordinate.X,
			State.ChunkCoordinate.Y,
			*Error);
		return false;
	}
	if (Result == MatterFlux::Reaction::
		EGroundChunkApplyResult::NoChange)
	{
		return true;
	}
	const FIntPoint FirstWorldCell(
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.X
				/ TerrainHeightField.CellSize),
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.Y
				/ TerrainHeightField.CellSize));
	const FIntPoint FirstWorldChunk(
		FMath::FloorToInt(
			static_cast<double>(FirstWorldCell.X) / 64),
		FMath::FloorToInt(
			static_cast<double>(FirstWorldCell.Y) / 64));
	if (ActiveTerrainChunks.Contains(
		FirstWorldChunk + State.ChunkCoordinate))
	{
		bGroundReactionVisualDirty = true;
		bGroundReactionVisualNeedsFullRebuild = true;
		PendingGroundReactionVisualCellIndices.Reset();
	}
	return true;
}

void AMatterFluxPlayableWorldActor::InitializeGroundStateChunks()
{
	if (!HasAuthority() || !GroundReaction || !GetWorld())
	{
		return;
	}
	TArray<FMatterFluxGroundStateChunk> InitialStates;
	FString Error;
	if (!GroundReaction->BuildInitialReplication(InitialStates, Error))
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Could not build initial ground state chunks: %s"),
			*Error);
		return;
	}
	GroundStateChunkActors.Reserve(InitialStates.Num());
	for (const FMatterFluxGroundStateChunk& InitialState : InitialStates)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Owner = this;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AMatterFluxGroundStateChunkActor* ChunkActor =
			GetWorld()->SpawnActor<AMatterFluxGroundStateChunkActor>(
				GetActorLocation(),
				FRotator::ZeroRotator,
				SpawnParameters);
		if (!ChunkActor)
		{
			UE_LOG(LogMatterFlux, Error,
				TEXT("Could not spawn ground state chunk (%d,%d)"),
				InitialState.ChunkCoordinate.X,
				InitialState.ChunkCoordinate.Y);
			continue;
		}
		if (!ChunkActor->InitializeState(*this, InitialState))
		{
			UE_LOG(LogMatterFlux, Error,
				TEXT("Could not initialize ground state chunk (%d,%d)"),
				InitialState.ChunkCoordinate.X,
				InitialState.ChunkCoordinate.Y);
			ChunkActor->Destroy();
			continue;
		}
		GroundStateChunkActors.Add(ChunkActor);
	}
}

void AMatterFluxPlayableWorldActor::DestroyGroundStateChunks()
{
	for (AMatterFluxGroundStateChunkActor* ChunkActor
		: GroundStateChunkActors)
	{
		if (IsValid(ChunkActor))
		{
			ChunkActor->Destroy();
		}
	}
	GroundStateChunkActors.Reset();
}

void AMatterFluxPlayableWorldActor::PublishGroundReactionState()
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxGroundReactionReplication);
	if (!HasAuthority() || !GroundReaction
		|| !GroundReaction->HasPendingReplication())
	{
		return;
	}
	TArray<FMatterFluxGroundStateChunk> Batch;
	FString Error;
	if (!GroundReaction->BuildPendingReplication(Batch, Error))
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Ground chunk replication batch was rolled back: %s"),
			*Error);
		return;
	}
	for (const FMatterFluxGroundStateChunk& State : Batch)
	{
		const TObjectPtr<AMatterFluxGroundStateChunkActor>* Found =
			GroundStateChunkActors.FindByPredicate(
				[&State](const AMatterFluxGroundStateChunkActor* Candidate)
				{
					return IsValid(Candidate)
						&& Candidate->GetChunkCoordinate()
							== State.ChunkCoordinate;
				});
		if (!Found || !(*Found)->InitializeState(*this, State))
		{
			UE_LOG(LogMatterFlux, Error,
				TEXT("Ground chunk replication adapter is missing for (%d,%d)"),
				State.ChunkCoordinate.X,
				State.ChunkCoordinate.Y);
		}
	}
}

void AMatterFluxPlayableWorldActor::AdvanceLogicalSourceReaction(
	const float DeltaSeconds)
{
	const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	bool bSourceFinishedThisFrame = false;
	if (HasAuthority())
	{
		SourceReactionActiveIdsScratch.Reset();
		SourceReactionFinishedIdsScratch.Reset();
		SourceReactionPublishIdsScratch.Reset();
		LogicalSourceReactionIndex.GatherStableIds(
			SourceReactionActiveIdsScratch);
		SourceReactionFinishedIdsScratch.Reserve(
			SourceReactionActiveIdsScratch.Num());
		SourceReactionPublishIdsScratch.Reserve(
			SourceReactionActiveIdsScratch.Num());
		for (const FGuid& SourceId : SourceReactionActiveIdsScratch)
		{
			TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime>*
				RuntimePtr = ActiveSourceReactions.Find(SourceId);
			const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
				FindFragmentSourceDefinition(SourceId);
			if (!RuntimePtr || !RuntimePtr->IsValid() || !Source)
			{
				SourceReactionFinishedIdsScratch.Add(SourceId);
				continue;
			}
			MatterFlux::Reaction::FSourceReactionRuntime& Runtime =
				*RuntimePtr->Get();
			const MatterFlux::Reaction::FSourceAdvanceResult Result =
				Runtime.AdvanceAuthority(ClampedDelta);
			if (Result.Steps > 0)
			{
				if (SynchronizeLogicalSourceReactionState(
					SourceId,
					*Source,
					Runtime,
					false))
				{
					SourceReactionPublishIdsScratch.Add(SourceId);
				}
				bSourceReactionVisualDirty = true;
			}
			if (!Runtime.IsActive())
			{
				SourceReactionFinishedIdsScratch.Add(SourceId);
			}
		}
		if (!SourceReactionPublishIdsScratch.IsEmpty()
			&& !PublishFragmentSourceStateBatch(
				SourceReactionPublishIdsScratch))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Logical Source reaction batch of %d states could not be published"),
				SourceReactionPublishIdsScratch.Num());
		}
		for (const FGuid& SourceId : SourceReactionFinishedIdsScratch)
		{
			ActiveSourceReactions.Remove(SourceId);
			LogicalSourceReactionIndex.Remove(SourceId);
		}
		bSourceFinishedThisFrame = !SourceReactionFinishedIdsScratch.IsEmpty();
	}

	SourceReactionVisualAccumulator += ClampedDelta;
	if (bSourceReactionVisualDirty
		&& (SourceReactionVisualAccumulator >= 0.1f
			|| bSourceFinishedThisFrame))
	{
		SourceReactionVisualAccumulator = 0.0f;
		RebuildLogicalSourceReactionVisualization();
	}
}

void AMatterFluxPlayableWorldActor::
	RebuildLogicalSourceReactionVisualization()
{
	if (GetNetMode() == NM_DedicatedServer || !SceneRoot || !CubeMesh)
	{
		bSourceReactionVisualDirty = false;
		return;
	}
	const auto EnsureInstances =
		[this](
			TObjectPtr<UInstancedStaticMeshComponent>& Component,
			const FName Name)
		{
			if (Component)
			{
				return;
			}
			Component = NewObject<UInstancedStaticMeshComponent>(this, Name);
			Component->SetupAttachment(SceneRoot);
			Component->SetStaticMesh(CubeMesh);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
			AddInstanceComponent(Component);
			Component->RegisterComponent();
		};
	EnsureInstances(SourceFlameInstances, TEXT("LogicalSourceFlames"));
	EnsureInstances(SourceSmokeInstances, TEXT("LogicalSourceSmoke"));

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	FLinearColor FlameColor(1.0f, 0.22f, 0.01f);
	FLinearColor SmokeColor(0.15f, 0.16f, 0.18f, 0.66f);
	bool bResolvedFlameColor = false;
	bool bResolvedSmokeColor = false;
	TArray<FGuid> ActiveVisualIds;
	LogicalSourceReactionIndex.GatherStableIds(ActiveVisualIds);
	for (const FGuid& SourceId : ActiveVisualIds)
	{
		const FFragment2DSourceStreamingState* State =
			StreamedFragmentSourceStates.Find(SourceId);
		if (!State || !Registry.IsValid())
		{
			continue;
		}
		if (const FMatterFluxReactionDefinition* Rule =
			Registry->Reactions.Find(State->ReactionState.RuleId))
		{
			if (!bResolvedFlameColor
				&& MatterFlux::Reaction::UsesFlamePresentation(*Rule))
			{
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(Rule->InputB))
				{
					FlameColor = Material->Color;
				}
				bResolvedFlameColor = true;
			}
			if (!bResolvedSmokeColor)
			{
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(Rule->EmissionMaterial))
				{
					SmokeColor = Material->Color;
				}
				bResolvedSmokeColor = true;
			}
			if (bResolvedFlameColor && bResolvedSmokeColor)
			{
				break;
			}
		}
	}
	const auto ConfigureMaterial =
		[this](
			TObjectPtr<UMaterialInstanceDynamic>& Material,
			UInstancedStaticMeshComponent* Component,
			const FLinearColor& Color,
			UMaterialInterface* Template)
		{
			if (!Material && Template)
			{
				Material = UMaterialInstanceDynamic::Create(
					Template,
					this);
				Material->SetVectorParameterValue(TEXT("Color"), Color);
				Material->SetScalarParameterValue(TEXT("FaceContrast"), 0.35f);
				Material->SetScalarParameterValue(TEXT("Roughness"), 0.5f);
				Component->SetMaterial(0, Material);
			}
		};
	ConfigureMaterial(
		SourceStimulusMaterial,
		SourceFlameInstances,
		FlameColor,
		VoxelColorMaterialTemplate);
	SmokeColor = FLinearColor::LerpUsingHSV(
		SmokeColor,
		FLinearColor(0.48f, 0.51f, 0.56f, 1.0f),
		0.65f);
	SmokeColor.A = 0.52f;
	ConfigureMaterial(
		SourceSmokeMaterial,
		SourceSmokeInstances,
		SmokeColor,
		VoxelGasMaterialTemplate);

	TArray<FTransform> FlameTransforms;
	SourceSmokeAnchors.Reset();
	constexpr int32 MaxVisualInstances = 8192;
	for (const FGuid& SourceId : ActiveVisualIds)
	{
		const FFragment2DSourceStreamingState* State =
			StreamedFragmentSourceStates.Find(SourceId);
		if (!State || GeneratedFragmentSources.Contains(SourceId))
		{
			continue;
		}
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
			FindFragmentSourceDefinition(SourceId);
		if (!Source)
		{
			continue;
		}
		FTransform VisualTransform =
			Source->Transform * GetActorTransform();
		if (const TWeakObjectPtr<AFragment2DActor>* CarrierPtr =
			DynamicAggregateCarriers.Find(SourceId))
		{
			if (const AFragment2DActor* Carrier = CarrierPtr->Get())
			{
				Carrier->GetAggregateSourceWorldTransform(
					SourceId,
					VisualTransform);
			}
		}
		const TArray<uint8>& Active = State->ReactionState.ActiveMask;
		float SmokeProbability = 0.7f;
		bool bRenderFlames = false;
		if (Registry.IsValid())
		{
			if (const FMatterFluxReactionDefinition* Rule =
				Registry->Reactions.Find(State->ReactionState.RuleId))
			{
				bRenderFlames =
					MatterFlux::Reaction::UsesFlamePresentation(*Rule);
				SmokeProbability = FMath::Clamp(
					static_cast<float>(Rule->EmissionChancePermille) / 1000.0f,
					0.0f,
					1.0f);
			}
		}
		TArray<int32, TInlineAllocator<64>> VisibleActiveCells;
		for (int32 Index = 0; Index < Active.Num(); ++Index)
		{
			if (Active[Index] != 0)
			{
				VisibleActiveCells.Add(Index);
			}
		}
		TSet<int32> SmokeSourceCells;
		if (Source->Mask.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks)
		{
			TArray<uint8, TInlineAllocator<256>> Occupied;
			Occupied.Append(State->GetRuntimeMask());
			if (State->ReactionState.OutputMask.Num() == Occupied.Num())
			{
				for (int32 Index = 0; Index < Occupied.Num(); ++Index)
				{
					Occupied[Index] = Occupied[Index] != 0
						|| State->ReactionState.OutputMask[Index] != 0;
				}
			}
			TArray<uint8> OccupiedArray(Occupied);
			TArray<int32> SurfaceCells;
			MatterFlux::FragmentGeometry::GatherTopExposedActiveMaskCells(
				OccupiedArray,
				Active,
				Source->Mask.Width,
				Source->Mask.Height,
				SurfaceCells);
			for (const int32 SurfaceCell : SurfaceCells)
			{
				SmokeSourceCells.Add(SurfaceCell);
			}
		}
		else
		{
			for (const int32 VisibleActiveCell : VisibleActiveCells)
			{
				SmokeSourceCells.Add(VisibleActiveCell);
			}
		}
		for (const int32 Index : VisibleActiveCells)
		{
			if (FlameTransforms.Num() >= MaxVisualInstances
				&& SourceSmokeAnchors.Num() >= MaxVisualInstances)
			{
				break;
			}
			const int32 X = Index % Source->Mask.Width;
			const int32 Y = Index / Source->Mask.Width;
			const FVector LocalPosition(
				(static_cast<float>(X) + 0.5f
					- static_cast<float>(Source->Mask.Width) * 0.5f)
					* Source->Mask.CellSize,
				0.0f,
				(static_cast<float>(Y) + 0.62f
					- static_cast<float>(Source->Mask.Height) * 0.5f)
					* Source->Mask.CellSize);
			const FVector Position =
				VisualTransform.TransformPosition(LocalPosition);
			const float BaseScale = Source->Mask.CellSize / 100.0f;
			const FVector FlameScale(
				BaseScale * 1.06f,
				BaseScale * 1.06f,
				BaseScale * 1.18f);
			if (bRenderFlames
				&& FlameTransforms.Num() < MaxVisualInstances)
			{
				FlameTransforms.Emplace(
					VisualTransform.Rotator(),
					Position,
					FlameScale);
			}
			if (SmokeSourceCells.Contains(Index)
				&& SourceSmokeAnchors.Num() < MaxVisualInstances)
			{
				MatterFlux::Rendering::FMaterialEmissionAnchor& Anchor =
					SourceSmokeAnchors.AddDefaulted_GetRef();
				Anchor.WorldPosition = Position + FVector(
					0.0f,
					0.0f,
					Source->Mask.CellSize * 1.05f);
				Anchor.CellSize = Source->Mask.CellSize;
				Anchor.EmissionProbability = SmokeProbability;
				Anchor.Seed = GetTypeHash(SourceId)
					^ static_cast<uint32>(Index) * 0x9e3779b9u;
			}
		}
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			It->GatherRootReactionVisualTransforms(
				FlameTransforms,
				SourceSmokeAnchors,
				MaxVisualInstances);
		}
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			It->GatherReactionSmokeAnchors(
				SourceSmokeAnchors,
				MaxVisualInstances);
		}
	}
	MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
		*SourceFlameInstances,
		FlameTransforms);
	RefreshUnifiedSmokeAnchors();
	bSourceReactionVisualDirty = false;
}

void AMatterFluxPlayableWorldActor::RefreshUnifiedSmokeAnchors()
{
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor> CombinedAnchors;
	CombinedAnchors.Reserve(
		GroundSmokeAnchors.Num() + SourceSmokeAnchors.Num());
	CombinedAnchors.Append(GroundSmokeAnchors);
	CombinedAnchors.Append(SourceSmokeAnchors);
	SmokeVisualPool.SetEmissionAnchors(CombinedAnchors);
}

void AMatterFluxPlayableWorldActor::AdvanceUnifiedSmokeVisualization(
	const float DeltaSeconds)
{
	if (GetNetMode() == NM_DedicatedServer || !SourceSmokeInstances)
	{
		return;
	}
	SmokeVisualPool.Advance(DeltaSeconds);
	TArray<FTransform> SmokeClusterTransforms;
	SmokeVisualPool.BuildInstanceTransforms(SmokeClusterTransforms);
	MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
		*SourceSmokeInstances,
		SmokeClusterTransforms);
}

void AMatterFluxPlayableWorldActor::RebuildGroundSmokeAnchors()
{
	GroundSmokeAnchors.Reset();
	if (!GroundReaction)
	{
		RefreshUnifiedSmokeAnchors();
		return;
	}
	const FMatterFluxReactionDefinition* Rule = GroundReaction->GetRule();
	const float SmokeProbability = Rule
		? FMath::Clamp(
			static_cast<float>(Rule->EmissionChancePermille) / 1000.0f,
			0.0f,
			1.0f)
		: 0.42f;
	const float VisualCellSize = FMath::Max(
		TerrainHeightField.CellSize,
		12.0f);
	GroundSmokeAnchors.Reserve(GroundSmokeCellByInstance.Num());
	for (const int32 CellIndex : GroundSmokeCellByInstance)
	{
		if (!GroundSurfacePositions.IsValidIndex(CellIndex))
		{
			continue;
		}
		MatterFlux::Rendering::FMaterialEmissionAnchor& Anchor =
			GroundSmokeAnchors.AddDefaulted_GetRef();
		Anchor.WorldPosition = GroundSurfacePositions[CellIndex]
			+ FVector(0.0f, 0.0f, VisualCellSize * 1.1f);
		Anchor.CellSize = VisualCellSize;
		Anchor.EmissionProbability = SmokeProbability;
		Anchor.Seed = static_cast<uint32>(CellIndex) * 0x9e3779b9u
			^ static_cast<uint32>(MapSeed);
	}
	RefreshUnifiedSmokeAnchors();
}

void AMatterFluxPlayableWorldActor::EmitActiveReactionParticles(
	const float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxReaction);
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxReactionPropagation);
	if (!HasAuthority())
	{
		return;
	}
	ReactionPropagationAccumulator +=
		FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (ReactionPropagationAccumulator < 0.2f)
	{
		return;
	}
	ReactionPropagationAccumulator =
		FMath::Fmod(
			ReactionPropagationAccumulator,
			0.2f);

	TArray<FGuid> LogicalActiveIds;
	LogicalSourceReactionIndex.GatherStableIds(LogicalActiveIds);
	bBatchingGroundReactions = true;
	for (const FGuid& ActiveId : LogicalActiveIds)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* ActiveSource =
			FindFragmentSourceDefinition(ActiveId);
		const FFragment2DSourceStreamingState* ActiveState =
			StreamedFragmentSourceStates.Find(ActiveId);
		if (!ActiveSource
			|| !ActiveState
			|| !ActiveState->bHasReactionState)
		{
			continue;
		}
		FBox ActiveBounds(ForceInit);
		TArray<FVector, TInlineAllocator<64>> ActiveCellCenters;
		TArray<FVector, TInlineAllocator<64>> AggregateContactCellCenters;
		const TArray<uint8>& ActiveMask =
			ActiveState->ReactionState.ActiveMask;
		// Cross-source propagation follows the aggregate's authored adjacency.
		// A partially transferred dynamic carrier may move one reacting slice
		// before the remaining logical slices are handed over; using that transient
		// mixed pose here would sever an otherwise connected material object.
		const FTransform AggregateContactTransform =
			ActiveSource->Transform * GetActorTransform();
		FTransform SourceWorldTransform = AggregateContactTransform;
		AFragment2DActor* DynamicCarrier = nullptr;
		if (const TWeakObjectPtr<AFragment2DActor>* CarrierPtr =
			DynamicAggregateCarriers.Find(ActiveId))
		{
			DynamicCarrier = CarrierPtr->Get();
			if (DynamicCarrier)
			{
				DynamicCarrier->GetAggregateSourceWorldTransform(
					ActiveId,
					SourceWorldTransform);
			}
		}
		for (int32 Index = 0; Index < ActiveMask.Num(); ++Index)
		{
			if (ActiveMask[Index] == 0)
			{
				continue;
			}
			const int32 X = Index % ActiveSource->Mask.Width;
			const int32 Y = Index / ActiveSource->Mask.Width;
			const FVector Center = SourceWorldTransform.TransformPosition(FVector(
				(static_cast<float>(X) + 0.5f
					- ActiveSource->Mask.Width * 0.5f)
					* ActiveSource->Mask.CellSize,
				0.0f,
				(static_cast<float>(Y) + 0.5f
					- ActiveSource->Mask.Height * 0.5f)
					* ActiveSource->Mask.CellSize));
			ActiveCellCenters.Add(Center);
			AggregateContactCellCenters.Add(
				AggregateContactTransform.TransformPosition(FVector(
					(static_cast<float>(X) + 0.5f
						- ActiveSource->Mask.Width * 0.5f)
						* ActiveSource->Mask.CellSize,
					0.0f,
					(static_cast<float>(Y) + 0.5f
						- ActiveSource->Mask.Height * 0.5f)
						* ActiveSource->Mask.CellSize)));
			ActiveBounds += FBox::BuildAABB(
				Center,
				FVector(ActiveSource->Mask.CellSize));
		}
		if (!ActiveBounds.IsValid)
		{
			continue;
		}
		FName StimulusMaterial = NAME_None;
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (Registry.IsValid())
		{
			if (const FMatterFluxReactionDefinition* Rule =
				Registry->Reactions.Find(
					ActiveState->ReactionState.RuleId))
			{
				StimulusMaterial = Rule->InputB;
			}
		}
		float GroundHeight = 0.0f;
		const bool bCanReachGround =
			TrySampleTerrainHeightAtWorldLocation(
				ActiveBounds.GetCenter(),
				GroundHeight)
			&& ActiveBounds.Min.Z
				<= GroundHeight + ActiveSource->Mask.CellSize * 1.5f;
		if (bCanReachGround
			&& !SourcesThatActivatedGround.Contains(ActiveId)
			&& SetSimulatedMaterialAtWorldLocation(
				ActiveBounds.GetCenter(),
				StimulusMaterial))
		{
			SourcesThatActivatedGround.Add(ActiveId);
		}
		const FBox StimulusBounds = ActiveBounds.ExpandBy(
			ActiveSource->Mask.CellSize * 1.5f);
		struct FAggregateContact
		{
			const MatterFlux::PlayableLevel::FLevelFragmentSource* Source = nullptr;
			FVector WorldLocation = FVector::ZeroVector;
			double DistanceSquared = TNumericLimits<double>::Max();
			int32 CellIndex = INDEX_NONE;
		};
		TArray<FAggregateContact, TInlineAllocator<16>> AggregateContacts;
		if (ActiveSource->AggregateId.IsValid())
		{
			for (const TPair<
				FIntPoint,
				TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
				: FragmentSourceChunks)
			{
				for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Candidate
					: Pair.Value)
				{
					if (Candidate.SourceId == ActiveId
						|| Candidate.AggregateId != ActiveSource->AggregateId
						|| RemovedFragmentSourceIds.Contains(Candidate.SourceId)
						|| GeneratedFragmentSources.Contains(Candidate.SourceId)
						|| ActiveSourceReactions.Contains(Candidate.SourceId))
					{
						continue;
					}
					const FFragment2DSourceStreamingState* CandidateState =
						StreamedFragmentSourceStates.Find(Candidate.SourceId);
					const TArray<uint8>& CandidateInput = CandidateState
						? CandidateState->GetRuntimeMask()
						: Candidate.Mask.SolidMask;
					if (CandidateInput.Num()
						!= Candidate.Mask.Width * Candidate.Mask.Height)
					{
						continue;
					}
					FTransform CandidateWorldTransform =
						Candidate.Transform * GetActorTransform();
					const double ContactDistance = FMath::Max(
						ActiveSource->Mask.CellSize,
						Candidate.Mask.CellSize) * 1.05;
					const double MaximumDistanceSquared =
						FMath::Square(ContactDistance);
					for (int32 CellIndex = 0;
						CellIndex < CandidateInput.Num();
						++CellIndex)
					{
						if (CandidateInput[CellIndex] == 0)
						{
							continue;
						}
						const int32 X = CellIndex % Candidate.Mask.Width;
						const int32 Y = CellIndex / Candidate.Mask.Width;
						const FVector CandidateCenter =
							CandidateWorldTransform.TransformPosition(FVector(
								(static_cast<float>(X) + 0.5f
									- Candidate.Mask.Width * 0.5f)
									* Candidate.Mask.CellSize,
								0.0f,
								(static_cast<float>(Y) + 0.5f
									- Candidate.Mask.Height * 0.5f)
									* Candidate.Mask.CellSize));
						for (const FVector& ActiveCenter
							: AggregateContactCellCenters)
						{
							const double DistanceSquared = FVector::DistSquared(
								ActiveCenter,
								CandidateCenter);
							if (DistanceSquared > MaximumDistanceSquared)
							{
								continue;
							}
							FAggregateContact* Contact = AggregateContacts.FindByPredicate(
								[&Candidate](const FAggregateContact& Existing)
								{
									return Existing.Source
										&& Existing.Source->SourceId == Candidate.SourceId;
								});
							if (!Contact)
							{
								Contact = &AggregateContacts.AddDefaulted_GetRef();
								Contact->Source = &Candidate;
							}
							if (DistanceSquared < Contact->DistanceSquared
								|| (FMath::IsNearlyEqual(
										DistanceSquared,
										Contact->DistanceSquared)
									&& CellIndex < Contact->CellIndex))
							{
								Contact->WorldLocation = CandidateCenter;
								Contact->DistanceSquared = DistanceSquared;
								Contact->CellIndex = CellIndex;
							}
						}
					}
				}
			}
		}
		AggregateContacts.Sort([](
			const FAggregateContact& Left,
			const FAggregateContact& Right)
		{
			if (!FMath::IsNearlyEqual(
				Left.DistanceSquared,
				Right.DistanceSquared))
			{
				return Left.DistanceSquared < Right.DistanceSquared;
			}
			return Left.Source->SourceId.ToString(EGuidFormats::Digits)
				< Right.Source->SourceId.ToString(EGuidFormats::Digits);
		});
		bool bActivatedAggregateNeighbor = false;
		for (const FAggregateContact& Contact : AggregateContacts)
		{
			if (!Contact.Source)
			{
				continue;
			}
			// A contact between two authored sources is already an exact material
			// interaction. Routing it through the horizontal world-material grid
			// loses the event when that grid cell already contains the stimulus and
			// also creates unrelated residue below elevated source geometry.
			bActivatedAggregateNeighbor |=
				ApplyMaterialStimulusToLogicalFragmentSource(
					*Contact.Source,
					Contact.WorldLocation,
					StimulusMaterial,
					ActiveState->ReactionState.Seed
						^ ActiveState->ReactionState.Tick
						^ static_cast<int32>(GetTypeHash(
							Contact.Source->SourceId)));
		}
		if (!bActivatedAggregateNeighbor
			&& !ActiveSource->AggregateId.IsValid())
		{
			SetSimulatedMaterialAtWorldLocation(
				StimulusBounds.GetCenter(),
				StimulusMaterial);
		}
	}
	bBatchingGroundReactions = false;
	if (GroundReaction && GroundReaction->HasPendingReplication())
	{
		PublishGroundReactionState();
	}

	UFragmentSimulationSubsystem* FragmentSubsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr;
	TArray<AFragment2DSourceActor*> ActiveSources;
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		AFragment2DSourceActor* Source = Pair.Value;
		if (!IsValid(Source))
		{
			continue;
		}
		if (Source->IsReacting())
		{
			ActiveSources.Add(Source);
		}
	}
	bBatchingGroundReactions = true;
	for (AFragment2DSourceActor* ActiveSource : ActiveSources)
	{
		const FBox ActiveBounds =
			ActiveSource->GetActiveWorldBounds();
		if (!ActiveBounds.IsValid)
		{
			continue;
		}
		float GroundHeight = 0.0f;
		const bool bCanReachGround =
			TrySampleTerrainHeightAtWorldLocation(
				ActiveBounds.GetCenter(),
				GroundHeight)
			&& ActiveBounds.Min.Z
				<= GroundHeight + ActiveSource->GetCellSize() * 1.5f;
		if (bCanReachGround
			&& !SourcesThatActivatedGround.Contains(ActiveSource->SourceId)
			&& SetSimulatedMaterialAtWorldLocation(
				ActiveBounds.GetCenter(),
				ActiveSource->GetReactionStimulusMaterial()))
		{
			SourcesThatActivatedGround.Add(ActiveSource->SourceId);
		}
		const FBox StimulusBounds =
			ActiveBounds.ExpandBy(
				ActiveSource->GetCellSize() * 1.5f);
		SetSimulatedMaterialAtWorldLocation(
			StimulusBounds.GetCenter(),
			ActiveSource->GetReactionStimulusMaterial());
	}
	bBatchingGroundReactions = false;
	if (GroundReaction
		&& GroundReaction->HasPendingReplication())
	{
		PublishGroundReactionState();
	}

	if (!GroundReaction
		|| GroundSurfacePositions.IsEmpty()
		|| !GroundReaction->IsActive())
	{
		return;
	}
	const TArray<uint8>& GroundActive =
		GroundReaction->GetActiveMask();
	const FVector GroundOrigin = GroundSurfacePositions[0];
	TArray<FIntPoint> ActiveGroundChunks;
	GroundReaction->GatherActiveChunkCoordinates(
		ActiveGroundChunks);
	TArray<FBox> LogicalQueryBounds;
	LogicalQueryBounds.Reserve(ActiveGroundChunks.Num());
	const double GroundCellHalfExtent =
		MatterFlux::PlayableLevel::TerrainCellSize * 0.5
		+ UE_KINDA_SMALL_NUMBER;
	for (const FIntPoint ActiveChunk : ActiveGroundChunks)
	{
		const int32 StartX = ActiveChunk.X * GroundReactionChunkSize;
		const int32 StartY = ActiveChunk.Y * GroundReactionChunkSize;
		const int32 EndX = FMath::Min(
			StartX + GroundReactionChunkSize,
			MatterFlux::PlayableLevel::TerrainCellsX);
		const int32 EndY = FMath::Min(
			StartY + GroundReactionChunkSize,
			MatterFlux::PlayableLevel::TerrainCellsY);
		const int32 FirstIndex =
			StartY * MatterFlux::PlayableLevel::TerrainCellsX + StartX;
		const int32 LastIndex =
			(EndY - 1) * MatterFlux::PlayableLevel::TerrainCellsX
				+ EndX - 1;
		if (StartX < 0
			|| StartY < 0
			|| EndX <= StartX
			|| EndY <= StartY
			|| !GroundSurfacePositions.IsValidIndex(FirstIndex)
			|| !GroundSurfacePositions.IsValidIndex(LastIndex))
		{
			continue;
		}
		const FVector& FirstSurface = GroundSurfacePositions[FirstIndex];
		const FVector& LastSurface = GroundSurfacePositions[LastIndex];
		const FBox LocalGroundCellBounds(
			FVector(
				FMath::Min(FirstSurface.X, LastSurface.X)
					- GroundCellHalfExtent,
				FMath::Min(FirstSurface.Y, LastSurface.Y)
					- GroundCellHalfExtent,
				-WORLD_MAX),
			FVector(
				FMath::Max(FirstSurface.X, LastSurface.X)
					+ GroundCellHalfExtent,
				FMath::Max(FirstSurface.Y, LastSurface.Y)
					+ GroundCellHalfExtent,
				WORLD_MAX));
		LogicalQueryBounds.Add(LocalGroundCellBounds);
	}
	TArray<FGuid> LogicalCandidateIds;
	FragmentSourceDefinitionIndex.QueryMany(
		LogicalQueryBounds,
		LogicalCandidateIds);
	TArray<AFragment2DSourceActor*> MaterializedGroundCandidates;
	if (FragmentSubsystem && !LogicalQueryBounds.IsEmpty())
	{
		TArray<FBox> WorldQueryBounds;
		WorldQueryBounds.Reserve(LogicalQueryBounds.Num());
		const FMatrix OwnerMatrix = GetActorTransform().ToMatrixWithScale();
		for (const FBox& LocalBounds : LogicalQueryBounds)
		{
			WorldQueryBounds.Add(LocalBounds.TransformBy(OwnerMatrix));
		}
		FragmentSubsystem->GatherSourcesInBoundsMany(
			WorldQueryBounds,
			MaterializedGroundCandidates);
	}
	TMap<FName, FName> StimulusMaterialByInput;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (Registry.IsValid())
	{
		TArray<FName> RuleIds;
		Registry->Reactions.GetKeys(RuleIds);
		RuleIds.Sort(
			[](const FName A, const FName B)
			{
				return A.LexicalLess(B);
			});
		for (const FName RuleId : RuleIds)
		{
			const FMatterFluxReactionDefinition* Rule =
				Registry->Reactions.Find(RuleId);
			if (Rule
				&& Rule->Kind == FMatterFluxReactionDefinition::EKind::Propagating
				&& !StimulusMaterialByInput.Contains(Rule->InputA))
			{
				StimulusMaterialByInput.Add(
					Rule->InputA,
					Rule->InputB);
			}
		}
	}
	const auto FindActiveGroundContact = [
		this,
		&GroundActive,
		&GroundOrigin](
			const FBox& CandidateBounds,
			FVector& OutContact)
	{
		if (!CandidateBounds.IsValid || !GetActorTransform().IsValid())
		{
			return false;
		}
		const FBox LocalBounds = CandidateBounds.TransformBy(
			GetActorTransform().ToInverseMatrixWithScale());
		if (!LocalBounds.IsValid)
		{
			return false;
		}
		const double CellSize =
			MatterFlux::PlayableLevel::TerrainCellSize;
		const int32 UnclampedMinimumX = FMath::CeilToInt(
			(LocalBounds.Min.X - GroundOrigin.X) / CellSize - 0.5);
		const int32 UnclampedMaximumX = FMath::FloorToInt(
			(LocalBounds.Max.X - GroundOrigin.X) / CellSize + 0.5);
		const int32 UnclampedMinimumY = FMath::CeilToInt(
			(LocalBounds.Min.Y - GroundOrigin.Y) / CellSize - 0.5);
		const int32 UnclampedMaximumY = FMath::FloorToInt(
			(LocalBounds.Max.Y - GroundOrigin.Y) / CellSize + 0.5);
		if (UnclampedMaximumX < 0
			|| UnclampedMinimumX >= MatterFlux::PlayableLevel::TerrainCellsX
			|| UnclampedMaximumY < 0
			|| UnclampedMinimumY >= MatterFlux::PlayableLevel::TerrainCellsY)
		{
			return false;
		}
		const int32 MinimumX = FMath::Clamp(
			UnclampedMinimumX,
			0,
			MatterFlux::PlayableLevel::TerrainCellsX - 1);
		const int32 MaximumX = FMath::Clamp(
			UnclampedMaximumX,
			0,
			MatterFlux::PlayableLevel::TerrainCellsX - 1);
		const int32 MinimumY = FMath::Clamp(
			UnclampedMinimumY,
			0,
			MatterFlux::PlayableLevel::TerrainCellsY - 1);
		const int32 MaximumY = FMath::Clamp(
			UnclampedMaximumY,
			0,
			MatterFlux::PlayableLevel::TerrainCellsY - 1);
		if (MaximumX < MinimumX || MaximumY < MinimumY)
		{
			return false;
		}
		const double VerticalTolerance = CellSize * 0.75;
		for (int32 Y = MinimumY; Y <= MaximumY; ++Y)
		{
			for (int32 X = MinimumX; X <= MaximumX; ++X)
			{
				const int32 Index =
					Y * MatterFlux::PlayableLevel::TerrainCellsX + X;
				if (!GroundActive.IsValidIndex(Index)
					|| GroundActive[Index] == 0
					|| !GroundSurfacePositions.IsValidIndex(Index))
				{
					continue;
				}
				const FVector GroundWorld = GetActorTransform().TransformPosition(
					GroundSurfacePositions[Index]);
				if (CandidateBounds.Min.Z > GroundWorld.Z + VerticalTolerance
					|| CandidateBounds.Max.Z < GroundWorld.Z - VerticalTolerance)
				{
					continue;
				}
				OutContact = CandidateBounds.GetClosestPointTo(GroundWorld);
				return true;
			}
		}
		return false;
	};
	for (const FGuid& CandidateId : LogicalCandidateIds)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Candidate =
			FindFragmentSourceDefinition(CandidateId);
		if (!Candidate
			|| GeneratedFragmentSources.Contains(CandidateId)
			|| ActiveSourceReactions.Contains(CandidateId)
			|| RemovedFragmentSourceIds.Contains(CandidateId))
		{
			continue;
		}
		const FTransform CandidateWorldTransform =
			Candidate->Transform * GetActorTransform();
		const FVector CandidateLocation =
			CandidateWorldTransform.GetLocation();
		const FVector Local =
			GetActorTransform().InverseTransformPosition(CandidateLocation);
		const int32 X = FMath::RoundToInt(
			(Local.X - GroundOrigin.X)
				/ MatterFlux::PlayableLevel::TerrainCellSize);
		const int32 Y = FMath::RoundToInt(
			(Local.Y - GroundOrigin.Y)
				/ MatterFlux::PlayableLevel::TerrainCellSize);
		if (X < 0
			|| X >= MatterFlux::PlayableLevel::TerrainCellsX
			|| Y < 0
			|| Y >= MatterFlux::PlayableLevel::TerrainCellsY)
		{
			continue;
		}
		const int32 Index =
			Y * MatterFlux::PlayableLevel::TerrainCellsX + X;
		if (!GroundActive.IsValidIndex(Index)
			|| GroundActive[Index] == 0)
		{
			continue;
		}
		const FVector HalfExtent(
			Candidate->Mask.Width * Candidate->Mask.CellSize * 0.5f,
			Candidate->Mask.CellSize * 0.5f,
			Candidate->Mask.Height * Candidate->Mask.CellSize * 0.5f);
		const FBox CandidateBounds = FBox(-HalfExtent, HalfExtent).TransformBy(
			CandidateWorldTransform.ToMatrixWithScale());
		const FVector GroundWorld = GetActorTransform().TransformPosition(
			GroundSurfacePositions[Index]);
		if (CandidateBounds.Min.Z
			> GroundWorld.Z
				+ MatterFlux::PlayableLevel::TerrainCellSize * 0.75f)
		{
			continue;
		}
		FName StimulusMaterial = NAME_None;
		if (const FName* ConfiguredStimulusMaterial =
			StimulusMaterialByInput.Find(Candidate->MaterialId))
		{
			if (!ConfiguredStimulusMaterial->IsNone())
			{
				StimulusMaterial = *ConfiguredStimulusMaterial;
			}
		}
		if (StimulusMaterial.IsNone())
		{
			continue;
		}
		SetSimulatedMaterialAtWorldLocation(
			FVector(
				CandidateBounds.GetCenter().X,
				CandidateBounds.GetCenter().Y,
				CandidateBounds.Min.Z
					+ Candidate->Mask.CellSize * 0.5f),
			StimulusMaterial);
	}
	for (AFragment2DSourceActor* Candidate : MaterializedGroundCandidates)
	{
		if (!IsValid(Candidate)
			|| Candidate->IsReacting()
			|| Candidate->GetRemainingInputCellCount() == 0
			|| !Candidate->HasReactionRule()
			|| Candidate->GetActorLocation().ContainsNaN())
		{
			continue;
		}
		const FVector Local =
			GetActorTransform().InverseTransformPosition(
				Candidate->GetActorLocation());
		const double RoundedX = FMath::RoundToDouble(
			(Local.X - GroundOrigin.X)
				/ MatterFlux::PlayableLevel::TerrainCellSize);
		const double RoundedY = FMath::RoundToDouble(
			(Local.Y - GroundOrigin.Y)
				/ MatterFlux::PlayableLevel::TerrainCellSize);
		if (!FMath::IsFinite(RoundedX)
			|| !FMath::IsFinite(RoundedY)
			|| RoundedX < MIN_int32
			|| RoundedX > MAX_int32
			|| RoundedY < MIN_int32
			|| RoundedY > MAX_int32)
		{
			continue;
		}
		const int32 X = static_cast<int32>(RoundedX);
		const int32 Y = static_cast<int32>(RoundedY);
		if (X < 0
			|| X >= MatterFlux::PlayableLevel::TerrainCellsX
			|| Y < 0
			|| Y >= MatterFlux::PlayableLevel::TerrainCellsY)
		{
			continue;
		}
		const int32 Index =
			Y * MatterFlux::PlayableLevel::TerrainCellsX + X;
		if (!GroundActive.IsValidIndex(Index)
			|| GroundActive[Index] == 0)
		{
			continue;
		}
		const FBox CandidateBounds =
			Candidate->GetComponentsBoundingBox(true);
		if (!CandidateBounds.IsValid)
		{
			continue;
		}
		const FVector GroundWorld =
			GetActorTransform().TransformPosition(
				GroundSurfacePositions[Index]);
		if (CandidateBounds.Min.Z
			> GroundWorld.Z
				+ MatterFlux::PlayableLevel::TerrainCellSize * 0.75f)
		{
			continue;
		}
		SetSimulatedMaterialAtWorldLocation(
			FVector(
				CandidateBounds.GetCenter().X,
				CandidateBounds.GetCenter().Y,
				CandidateBounds.Min.Z
					+ Candidate->GetCellSize() * 0.5f),
			Candidate->GetReactionStimulusMaterial());
	}
	// Detached pieces are movable projections and therefore are absent from the
	// logical-source and materialized-source spatial indexes above. Query their
	// current reactive bounds so a felled trunk can catch fire wherever
	// physics moved it, including when only one end overlaps a active cell.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			AFragment2DActor* Candidate = *It;
			if (!IsValid(Candidate)
				|| Candidate->IsActorBeingDestroyed())
			{
				continue;
			}
			const FBox CandidateBounds =
				Candidate->GetReactiveWorldBounds();
			FVector Contact = FVector::ZeroVector;
			if (!FindActiveGroundContact(CandidateBounds, Contact))
			{
				continue;
			}
			FName StimulusMaterial = NAME_None;
			if (const FName* ConfiguredStimulusMaterial =
				StimulusMaterialByInput.Find(
					Candidate->RootReactionState.MaterialId);
				ConfiguredStimulusMaterial
					&& !ConfiguredStimulusMaterial->IsNone())
			{
				StimulusMaterial = *ConfiguredStimulusMaterial;
			}
			if (StimulusMaterial.IsNone())
			{
				continue;
			}
			SetSimulatedMaterialAtWorldLocation(
				Contact,
				StimulusMaterial);
		}
	}
}

void AMatterFluxPlayableWorldActor::InitializeMaterialSimulation(
	const FMatterFluxContentRegistry& Registry,
	const MatterFlux::PlayableLevel::FLevelLayout& Layout)
{
	InitialMaterialWarmupStepCount = 0;
	InitialMaterialWarmupQuietStepCount = 0;
	bInitialMaterialWarmupStepsComplete = false;
	bInitialMaterialWarmupComplete = false;
	ExternalMaterialSupportCells.Reset();
	// The title menu preserves this actor. Once a replacement layout commits,
	// discard the previous world's projection components and bookkeeping so they
	// cannot become phantom initialization requirements for the new runtime.
	DestroyMaterialVisualization();
	PendingInitialLiquidProjectionChunks.Reset();
	bCaptureInitialLiquidProjectionRequirements = true;
	PendingMaterialDisplacementCells.Reset();
	PreviousMaterialDisplacementCells.Reset();
	RecentMaterialChunkWakes.Reset();
	MaterialLiquidDensities.Reset();
	MaterialMovementMedia.Reset();
	for (const TPair<FName, FMatterFluxMaterialDefinition>& Pair
		: Registry.Materials)
	{
		if ((Pair.Value.Phase == EMatterFluxMaterialPhase::Liquid
				|| Pair.Value.Phase == EMatterFluxMaterialPhase::Powder)
			&& FMath::IsFinite(Pair.Value.MovementResistance)
			&& Pair.Value.MovementResistance >= 0.0f)
		{
			MaterialMovementMedia.Add(Pair.Key, {
				Pair.Value.Phase,
				Pair.Value.Density,
				Pair.Value.MovementResistance,
				Pair.Value.Phase == EMatterFluxMaterialPhase::Liquid
					? MaterialLiquidColumnHeight
					: static_cast<float>(
						MatterFlux::Material::SurfacePowderFullColumnHeight) });
		}
		if (Pair.Value.Phase == EMatterFluxMaterialPhase::Liquid
			&& FMath::IsFinite(Pair.Value.Density)
			&& Pair.Value.Density > 0.0f)
		{
			MaterialLiquidDensities.Add(Pair.Key, Pair.Value.Density);
		}
	}

	MatterFlux::Material::FRuntimeSettings RuntimeSettings;
	MatterFlux::Material::FWorldSettings& Settings =
		RuntimeSettings.World;
	Settings.ChunkSize = MaterialSimulationChunkSize;
	Settings.ActiveChunkRadius = MaterialSimulationActiveChunkRadius;
	const int32 ActiveDiameter =
		MaterialSimulationActiveChunkRadius * 2 + 1;
	Settings.MaxActiveChunks = ActiveDiameter * ActiveDiameter;
	Settings.MinWorldHeightCells = MaterialSimulationMinHeightCells;
	Settings.MaxWorldHeightCells = MaterialSimulationMaxHeightCells;
	Settings.bCullOutsideVerticalBounds = true;
	Settings.bUseSurfaceTopology = true;
	// The initial height cache is finite, but FLevelTerrain is not. Resident
	// procedural chunks seed their support topology on demand as focus moves.
	Settings.bCullOutsideSurfaceBounds = !Layout.Terrain.bInfinite;
	Settings.LiquidFullColumnHeight = FMath::RoundToInt(
		MaterialLiquidColumnHeight);
	// Convert dry sand's physical repose angle into the conserved height-field
	// delta used equally on terrain, trees, roofs, and dynamic supports.
	Settings.PowderMaximumStableSlopeAmount =
		GetPowderMaximumStableSlopeAmount(MaterialSimulationCellSize);
	Settings.BodyWakeRefillDelaySteps = FMath::Clamp(
		FMath::CeilToInt(
			MaterialBodyWakeRefillDelaySeconds
				/ MaterialSimulationStepSeconds),
		0,
		256);
	Settings.BodyWakeRefillDurationSteps = FMath::Clamp(
		FMath::CeilToInt(
			MaterialBodyWakeRefillDurationSeconds
				/ MaterialSimulationStepSeconds),
		1,
		256);
	RuntimeSettings.StepSeconds = MaterialSimulationStepSeconds;
	RuntimeSettings.MaxStepsPerAdvance =
		MaxMaterialSimulationStepsPerFrame;

	FIntPoint MinimumCell(MAX_int32, MAX_int32);
	FIntPoint MaximumCell(MIN_int32, MIN_int32);
	const MatterFlux::PlayableLevel::FLevelTerrain& Terrain =
		Layout.Terrain;
	if (Terrain.IsValid())
	{
		MinimumCell = FIntPoint(
			FMath::FloorToInt(
				Terrain.FirstCellCenter.X / MaterialSimulationCellSize),
			FMath::FloorToInt(
				Terrain.FirstCellCenter.Y / MaterialSimulationCellSize));
		MaximumCell =
			MinimumCell + FIntPoint(Terrain.Width - 1, Terrain.Height - 1);
	}
	if (MinimumCell.X > MaximumCell.X
		|| MinimumCell.Y > MaximumCell.Y)
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Material simulation has no terrain heightfield."));
		MaterialSimulation.Reset();
		DestroyMaterialVisualization();
		return;
	}
	Settings.MinSurfaceCell = MinimumCell;
	Settings.MaxSurfaceCellExclusive =
		MaximumCell + FIntPoint(1, 1);

	MaterialSimulation =
		MakeUnique<MatterFlux::Material::FSimulationRuntime>();
	FString Error;
	const TArray<FIntPoint> SeedFocuses = {
		FIntPoint::ZeroValue
	};
	if (!MaterialSimulation->Initialize(
		RuntimeSettings,
		Registry,
		MapSeed,
		SeedFocuses,
		Error))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Material simulation failed to initialize: %s"),
			*Error);
		MaterialSimulation.Reset();
		DestroyMaterialVisualization();
		return;
	}

	MaterialVisualizationAccumulator = 0.0f;
	bMaterialVisualizationDirty = false;
	SeedMaterialSimulation(Layout);
	TArray<FIntPoint> InitialFocuses;
	if (HasAuthority())
	{
		GatherMaterialSimulationFocusCells(
			InitialFocuses);
	}
	else
	{
		InitialFocuses = {
			ReplicatedMaterialSimulationFocus
		};
	}
	ReplicatedMaterialSimulationFocus =
		InitialFocuses[0];
	MaterialSimulation->SetFocuses(InitialFocuses);
	if (HasAuthority())
	{
		PublishMaterialSimulationState();
	}
	else
	{
		ApplyReplicatedMaterialSimulationState();
	}
	RebuildMaterialVisualization(Registry);
}

void AMatterFluxPlayableWorldActor::SeedMaterialSimulation(
	const MatterFlux::PlayableLevel::FLevelLayout& Layout)
{
	if (!MaterialSimulation)
	{
		return;
	}
	const auto ToCell =
		[this](const FVector& Location)
		{
			return FIntPoint(
				FMath::FloorToInt(
					Location.X / MaterialSimulationCellSize),
				FMath::FloorToInt(
					Location.Y / MaterialSimulationCellSize));
		};

	const MatterFlux::PlayableLevel::FLevelTerrain& Terrain =
		Layout.Terrain;
	const MatterFlux::PlayableLevel::FLevelLayer* Stream =
		Layout.FindLayer(TEXT("Stream"));
	const MatterFlux::PlayableLevel::FLevelLayer* Lake =
		Layout.FindLayer(TEXT("Lake"));
	if (!Terrain.IsValid() || !Stream)
	{
		return;
	}

	TArray<MatterFlux::Material::FSeedCell> SeedCells;
	TMap<FIntPoint, int32> SeedIndices;
	SeedCells.Reserve(Terrain.Heights.Num());
	SeedIndices.Reserve(Terrain.Heights.Num());
	for (int32 Y = 0; Y < Terrain.Height; ++Y)
	{
		for (int32 X = 0; X < Terrain.Width; ++X)
		{
			const FVector SurfacePosition(
				Terrain.FirstCellCenter.X + X * Terrain.CellSize,
				Terrain.FirstCellCenter.Y + Y * Terrain.CellSize,
				Terrain.HeightAt(X, Y));
			const FIntPoint Cell = ToCell(SurfacePosition);
			const int32 SurfaceHeight =
				FMath::RoundToInt(SurfacePosition.Z);
			const int32 SeedIndex = SeedCells.Add({
				Cell,
				NAME_None,
				SurfaceHeight });
			SeedIndices.Add(Cell, SeedIndex);
		}
	}

	for (int32 StreamIndex = 0;
		StreamIndex < Stream->Instances.Num();
		++StreamIndex)
	{
		const FTransform& StreamTransform = Stream->Instances[StreamIndex];
		if (const int32* SeedIndex = SeedIndices.Find(
			ToCell(StreamTransform.GetLocation())))
		{
			MatterFlux::Material::FSeedCell& SeedCell = SeedCells[*SeedIndex];
			const float AuthoredSurfaceZ = StreamTransform.GetLocation().Z
				+ StreamTransform.GetScale3D().Z * 50.0f;
			const uint8 AuthoredAmount = static_cast<uint8>(FMath::Clamp(
				FMath::RoundToInt(
					(AuthoredSurfaceZ - static_cast<float>(SeedCell.SupportHeight))
						/ MaterialLiquidColumnHeight * 255.0f),
				1,
				255));
			const bool bAlreadyStream = SeedCell.MaterialId == TEXT("water");
			SeedCell.MaterialId = TEXT("water");
			SeedCell.Amount = bAlreadyStream
				? FMath::Max(SeedCell.Amount, static_cast<uint16>(AuthoredAmount))
				: AuthoredAmount;
		}
	}
	if (Lake)
	{
		for (const FTransform& Transform : Lake->Instances)
		{
			const FVector Location = Transform.GetLocation();
			const FIntPoint Cell = ToCell(Location);
			const int32 TerrainX = FMath::Clamp(
				FMath::RoundToInt(
					(Location.X - Terrain.FirstCellCenter.X)
						/ Terrain.CellSize),
				0,
				Terrain.Width - 1);
			const int32 TerrainY = FMath::Clamp(
				FMath::RoundToInt(
					(Location.Y - Terrain.FirstCellCenter.Y)
						/ Terrain.CellSize),
				0,
				Terrain.Height - 1);
			const float BottomZ = Terrain.HeightAt(TerrainX, TerrainY);
			const float SurfaceZ = Location.Z
				+ Transform.GetScale3D().Z * 50.0f;
			const int32* SeedIndex = SeedIndices.Find(Cell);
			if (!SeedIndex || SurfaceZ <= BottomZ)
			{
				continue;
			}
			MatterFlux::Material::FSeedCell& SeedCell =
				SeedCells[*SeedIndex];
			const FName LakeMaterial = Lake->MaterialId.IsNone()
				? FName(TEXT("water"))
				: Lake->MaterialId;
			const bool bAlreadyLake =
				SeedCell.MaterialId == LakeMaterial;
			SeedCell.MaterialId = LakeMaterial;
			const uint8 LakeAmount = static_cast<uint8>(FMath::Clamp(
				FMath::RoundToInt(
					(SurfaceZ - BottomZ)
						/ MaterialLiquidColumnHeight * 255.0f),
				1,
				255));
			SeedCell.Amount = bAlreadyLake
				? FMath::Max(SeedCell.Amount, static_cast<uint16>(LakeAmount))
				: LakeAmount;
		}
	}

	RegisterRecentMaterialWakeSeedCells(SeedCells);
	WakeRecentVisibleMaterialChunks();
	if (!MaterialSimulation->SeedSurface(SeedCells))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Material simulation rejected the generated surface seed."));
	}
}

void AMatterFluxPlayableWorldActor::
	ApplyReplicatedMaterialSimulationState()
{
	if (!MaterialSimulation
		|| HasAuthority())
	{
		return;
	}

	FString Error;
	const MatterFlux::Material::EReplicatedStateApplyResult Result =
		MaterialSimulation->ApplyReplicatedState(
			MapSeed,
			ReplicatedMaterialState,
			Error);
	if (Result == MatterFlux::Material::
		EReplicatedStateApplyResult::Rejected)
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Client rejected material state revision %d: %s"),
			ReplicatedMaterialState.Revision,
			*Error);
		return;
	}
	if (Result != MatterFlux::Material::
		EReplicatedStateApplyResult::Applied)
	{
		return;
	}
	ReplicatedMaterialSimulationStep =
		MaterialSimulation->GetLogicalStep();
	ReplicatedMaterialSimulationFocus =
		MaterialSimulation->GetFocuses()[0];
	bMaterialVisualizationDirty = true;
	bMaterialVisualizationDeferredForStreaming = true;
}

void AMatterFluxPlayableWorldActor::
	PublishMaterialSimulationState()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_Material_PublishReplicatedState);
	if (!HasAuthority() || !MaterialSimulation)
	{
		return;
	}
	if (GetNetMode() == NM_Standalone
		&& GetWorld()
		&& GetWorld()->IsGameWorld())
	{
		return;
	}

	FString Error;
	FMatterFluxReplicatedMaterialState Candidate;
	if (!MaterialSimulation->BuildReplicatedState(
		MapSeed,
		ReplicatedMaterialState.Revision,
		Candidate,
		Error))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Authority could not encode material state: %s"),
			*Error);
		return;
	}
	ReplicatedMaterialState = MoveTemp(Candidate);
	ForceNetUpdate();
}

bool AMatterFluxPlayableWorldActor::PublishFragmentSourceState(
	const FGuid& SourceId,
	const FFragment2DSourceStreamingState& State)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxSourceReactionReplication);
	if (!HasAuthority()
		|| !SourceId.IsValid()
		|| State.Revision < 0
		|| ReplicatedFragmentSourceStates.Items.Num()
			> MaxReplicatedFragmentSourceStates)
	{
		return false;
	}
	const FMatterFluxFragmentSourceStateBatchUpdate Update{SourceId, &State};
	const EMatterFluxFragmentSourceStateUpsertResult Result =
		ReplicatedFragmentSourceStates.UpsertAuthorityBatch(
			MakeArrayView(&Update, 1),
			MaxReplicatedFragmentSourceStates,
			MaxReplicatedFragmentSourceStateBytes);
	if (Result != EMatterFluxFragmentSourceStateUpsertResult::Committed)
	{
		const TCHAR* Reason = TEXT("invalid state");
		if (Result
			== EMatterFluxFragmentSourceStateUpsertResult::ItemBudgetExceeded)
		{
			Reason = TEXT("item budget exhausted");
		}
		else if (Result
			== EMatterFluxFragmentSourceStateUpsertResult::ByteBudgetExceeded)
		{
			Reason = TEXT("byte budget exhausted");
		}
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Cannot replicate logical source %s: %s"),
			*SourceId.ToString(),
			Reason);
		return false;
	}
	ForceNetUpdate();
	return true;
}

bool AMatterFluxPlayableWorldActor::PublishFragmentSourceStateBatch(
	const TConstArrayView<FGuid> SourceIds)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxSourceReactionReplication);
	if (!HasAuthority())
	{
		return false;
	}
	if (SourceIds.IsEmpty())
	{
		return true;
	}
	FragmentSourceReplicationUpdatesScratch.Reset();
	FragmentSourceReplicationUpdatesScratch.Reserve(SourceIds.Num());
	for (const FGuid& SourceId : SourceIds)
	{
		const FFragment2DSourceStreamingState* State =
			StreamedFragmentSourceStates.Find(SourceId);
		if (!SourceId.IsValid() || !State)
		{
			return false;
		}
		FragmentSourceReplicationUpdatesScratch.Add({SourceId, State});
	}
	const EMatterFluxFragmentSourceStateUpsertResult Result =
		ReplicatedFragmentSourceStates.UpsertAuthorityBatch(
			FragmentSourceReplicationUpdatesScratch,
			MaxReplicatedFragmentSourceStates,
			MaxReplicatedFragmentSourceStateBytes);
	if (Result != EMatterFluxFragmentSourceStateUpsertResult::Committed)
	{
		return false;
	}
	ForceNetUpdate();
	return true;
}

void AMatterFluxPlayableWorldActor::ApplyReplicatedFragmentSourceStates()
{
	if (HasAuthority())
	{
		return;
	}
	if (!FragmentSourceProxy)
	{
		ReplicatedFragmentSourceStates.RequestClientFullRebuild();
		return;
	}

	FMatterFluxFragmentSourceClientApplyPlan Plan;
	ReplicatedFragmentSourceStates.ConsumeClientApplyPlan(Plan);
	bool bUseFullRebuild = Plan.bFullRebuild;
	if (!bUseFullRebuild)
	{
		for (const int32 ItemIndex : Plan.UpsertItemIndices)
		{
			if (!ReplicatedFragmentSourceStates.Items.IsValidIndex(ItemIndex))
			{
				bUseFullRebuild = true;
				break;
			}
		}
	}

	const bool bHadWork = bUseFullRebuild
		|| !Plan.UpsertItemIndices.IsEmpty()
		|| !Plan.RemovedSourceIds.IsEmpty();
	if (bUseFullRebuild)
	{
		TSet<FGuid> NextAppliedIds;
		for (const FMatterFluxReplicatedFragmentSourceState& Replicated
			: ReplicatedFragmentSourceStates.Items)
		{
			if (NextAppliedIds.Contains(Replicated.SourceId))
			{
				continue;
			}
			if (ApplyReplicatedFragmentSourceState(Replicated))
			{
				NextAppliedIds.Add(Replicated.SourceId);
			}
		}
		TArray<FGuid> RemovedIds;
		RemovedIds.Reserve(AppliedReplicatedFragmentSourceIds.Num());
		for (const FGuid& PreviousId : AppliedReplicatedFragmentSourceIds)
		{
			if (!NextAppliedIds.Contains(PreviousId))
			{
				RemovedIds.Add(PreviousId);
		}
		}
		for (const FGuid& RemovedId : RemovedIds)
		{
			RemoveReplicatedFragmentSourceState(RemovedId);
		}
		AppliedReplicatedFragmentSourceIds = MoveTemp(NextAppliedIds);
	}
	else
	{
		for (const FGuid& RemovedId : Plan.RemovedSourceIds)
		{
			RemoveReplicatedFragmentSourceState(RemovedId);
		}
		for (const int32 ItemIndex : Plan.UpsertItemIndices)
		{
			const FMatterFluxReplicatedFragmentSourceState& Replicated =
				ReplicatedFragmentSourceStates.Items[ItemIndex];
			if (ApplyReplicatedFragmentSourceState(Replicated))
			{
				AppliedReplicatedFragmentSourceIds.Add(Replicated.SourceId);
			}
		}
 	}
	if (bHadWork)
	{
		bSourceReactionVisualDirty = true;
		FragmentSourceProxy->FlushPendingChanges();
	}
}

bool AMatterFluxPlayableWorldActor::ApplyPersistentFragmentSourceStateToProxy(
	const FGuid& SourceId,
	const FFragment2DSourceStreamingState& State)
{
	if (!FragmentSourceProxy || !SourceId.IsValid())
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Definition =
		FindFragmentSourceDefinition(SourceId);
	if (!Definition)
	{
		return false;
	}

	TArray<uint8> OutputMask;
	OutputMask.Init(0, Definition->Mask.Width * Definition->Mask.Height);
	FName OutputMaterial = NAME_None;
	FLinearColor OutputColor(0.08f, 0.07f, 0.06f);
	if (State.bHasReactionState)
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		const FMatterFluxReactionDefinition* Rule = Registry.IsValid()
			? Registry->Reactions.Find(State.ReactionState.RuleId)
			: nullptr;
		if (!Rule)
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Cannot restore proxy state %s without reaction rule %s"),
				*SourceId.ToString(),
				*State.ReactionState.RuleId.ToString());
			return false;
		}
		OutputMask = State.ReactionState.OutputMask;
		OutputMaterial = Rule->OutputA;
		if (const FMatterFluxMaterialDefinition* Material =
			Registry->Materials.Find(OutputMaterial))
		{
			OutputColor = Material->Color;
		}
	}
	return FragmentSourceProxy->ApplySourceState(
		SourceId,
		State.GetRuntimeMask(),
		OutputMask,
		OutputMaterial,
		OutputColor,
		State.bHasReactionState
			&& State.ReactionState.ActiveMask.Contains(1))
		!= EMatterFluxFragmentSourceProxyApplyResult::Invalid;
}

bool AMatterFluxPlayableWorldActor::ApplyReplicatedFragmentSourceState(
	const FMatterFluxReplicatedFragmentSourceState& Replicated)
{
	if (!Replicated.SourceId.IsValid() || Replicated.Revision < 0)
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Definition =
		FindFragmentSourceDefinition(Replicated.SourceId);
	if (!Definition)
	{
		return false;
	}
	TArray<uint8> RuntimeMask;
	TArray<uint8> OutputMask;
	TArray<uint8> ActiveMask;
	if (!UnpackFragmentSourceMask(
		Replicated.PackedRuntimeMask,
		Definition->Mask.Width * Definition->Mask.Height,
		RuntimeMask))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Client rejected logical source state %s revision %d"),
			*Replicated.SourceId.ToString(),
			Replicated.Revision);
		return false;
	}
	if (Replicated.bHasReactionState
		&& (!UnpackFragmentSourceMask(
				Replicated.PackedOutputMask,
				Definition->Mask.Width * Definition->Mask.Height,
				OutputMask)
			|| !UnpackFragmentSourceMask(
				Replicated.PackedActiveMask,
				Definition->Mask.Width * Definition->Mask.Height,
				ActiveMask)
			|| Replicated.ReactionRuleId.IsNone()
			|| !FMath::IsFinite(Replicated.ReactionAccumulator)
			|| Replicated.ReactionAccumulator < 0.0f
			|| Replicated.TotalMaterialEmissionCount < 0))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Client rejected logical source reaction state %s revision %d"),
			*Replicated.SourceId.ToString(),
			Replicated.Revision);
		return false;
	}
	const FFragment2DSourceStreamingState* ExistingState =
		StreamedFragmentSourceStates.Find(Replicated.SourceId);
	if (ExistingState && ExistingState->Revision > Replicated.Revision)
	{
		LogicalSourceReactionIndex.ApplySnapshot(
			Replicated.SourceId,
			ExistingState->bHasReactionState,
			ExistingState->ReactionState.ActiveMask);
		return true;
	}

	FFragment2DSourceStreamingState CandidateState;
	CandidateState.Revision = Replicated.Revision;
	CandidateState.bHasReactionState = Replicated.bHasReactionState;
	CandidateState.ReactionAccumulator = Replicated.ReactionAccumulator;
	CandidateState.TotalMaterialEmissionCount =
		Replicated.TotalMaterialEmissionCount;
	if (Replicated.bHasReactionState)
	{
		CandidateState.ReactionState.RuleId = Replicated.ReactionRuleId;
		CandidateState.ReactionState.Width = Definition->Mask.Width;
		CandidateState.ReactionState.Height = Definition->Mask.Height;
		CandidateState.ReactionState.Seed = Replicated.ReactionSeed;
		CandidateState.ReactionState.Tick = Replicated.ReactionTick;
		CandidateState.ReactionState.OutputMask = OutputMask;
		CandidateState.ReactionState.ActiveMask = ActiveMask;
	}
	else
	{
		OutputMask.Init(0, Definition->Mask.Width * Definition->Mask.Height);
	}
	CandidateState.SetRuntimeMask(MoveTemp(RuntimeMask));
	FName OutputMaterial = NAME_None;
	FLinearColor OutputColor(0.08f, 0.07f, 0.06f);
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (Registry.IsValid() && Replicated.bHasReactionState)
	{
		if (const FMatterFluxReactionDefinition* Rule =
			Registry->Reactions.Find(Replicated.ReactionRuleId))
		{
			OutputMaterial = Rule->OutputA;
			if (const FMatterFluxMaterialDefinition* Material =
				Registry->Materials.Find(OutputMaterial))
			{
				OutputColor = Material->Color;
			}
		}
	}
	if (FragmentSourceProxy->ApplySourceState(
		Replicated.SourceId,
		CandidateState.GetRuntimeMask(),
		OutputMask,
		OutputMaterial,
		OutputColor,
		CandidateState.bHasReactionState
			&& CandidateState.ReactionState.ActiveMask.Contains(1))
		== EMatterFluxFragmentSourceProxyApplyResult::Invalid)
	{
		return false;
	}
	FFragment2DSourceStreamingState& RuntimeState =
		StreamedFragmentSourceStates.Add(
			Replicated.SourceId,
			MoveTemp(CandidateState));
	if (TWeakObjectPtr<AFragment2DActor>* CarrierPtr =
		DynamicAggregateCarriers.Find(Replicated.SourceId))
	{
		if (AFragment2DActor* Carrier = CarrierPtr->Get())
		{
			Carrier->ApplyAggregateSourceStreamingState(
				Replicated.SourceId,
				RuntimeState,
				OutputMaterial,
				OutputColor);
		}
		else
		{
			DynamicAggregateCarriers.Remove(Replicated.SourceId);
		}
	}
	LogicalSourceReactionIndex.ApplySnapshot(
		Replicated.SourceId,
		RuntimeState.bHasReactionState,
		RuntimeState.ReactionState.ActiveMask);
	return true;
}

void AMatterFluxPlayableWorldActor::RemoveReplicatedFragmentSourceState(
	const FGuid& SourceId)
{
	StreamedFragmentSourceStates.Remove(SourceId);
	LogicalSourceReactionIndex.Remove(SourceId);
	AppliedReplicatedFragmentSourceIds.Remove(SourceId);
	if (const MatterFlux::PlayableLevel::FLevelFragmentSource* Definition =
		FindFragmentSourceDefinition(SourceId))
	{
		TArray<uint8> EmptyOutput;
		EmptyOutput.Init(
			0,
			Definition->Mask.Width * Definition->Mask.Height);
		FragmentSourceProxy->ApplySourceState(
			SourceId,
			Definition->Mask.SolidMask,
			EmptyOutput,
			NAME_None,
			FLinearColor::Transparent,
			false);
	}
}

void AMatterFluxPlayableWorldActor::UpdateMaterialVisualization(
	const float DeltaSeconds,
	const bool bAllowRebuild)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_Material_UpdateVisualization);
	if (!bMaterialVisualizationDirty
		|| GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	MaterialVisualizationAccumulator +=
		FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (!bAllowRebuild)
	{
		return;
	}
	if (MaterialVisualizationAccumulator
		< FMath::Max(MaterialVisualizationInterval, 1.0f / 60.0f))
	{
		return;
	}

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (Registry.IsValid())
	{
		RebuildMaterialVisualization(*Registry);
	}
}

void AMatterFluxPlayableWorldActor::ResolveMaterialInteractions(
	const FMatterFluxContentRegistry& Registry)
{
	if (!HasAuthority()
		|| !MaterialSimulation
		|| MaxMaterialSourceReactionsPerFrame <= 0)
	{
		return;
	}

	// Lua 注册表是无序映射；先稳定排序，保证 listen host、服务器和回放
	// 在多个规则都可能匹配时仍选择同一条规则。
	TArray<FName> ContactRuleIds;
	TSet<FName> SourceReactiveLiquidMaterials;
	TSet<FName> PropagatingStimulusMaterials;
	for (const TPair<FName, FMatterFluxReactionDefinition>& Pair
		: Registry.Reactions)
	{
		if (Pair.Value.Kind
			== FMatterFluxReactionDefinition::EKind::Propagating)
		{
			PropagatingStimulusMaterials.Add(Pair.Value.InputB);
			continue;
		}
		if (Pair.Value.Kind
			== FMatterFluxReactionDefinition::EKind::Contact)
		{
			ContactRuleIds.Add(Pair.Key);
			const FMatterFluxMaterialDefinition* InputA =
				Registry.Materials.Find(Pair.Value.InputA);
			const FMatterFluxMaterialDefinition* InputB =
				Registry.Materials.Find(Pair.Value.InputB);
			if (InputA && InputB
				&& InputA->Phase == EMatterFluxMaterialPhase::Liquid
				&& InputB->Phase
					== EMatterFluxMaterialPhase::StaticSolid)
			{
				SourceReactiveLiquidMaterials.Add(Pair.Value.InputA);
			}
			if (InputA && InputB
				&& InputB->Phase == EMatterFluxMaterialPhase::Liquid
				&& InputA->Phase
					== EMatterFluxMaterialPhase::StaticSolid)
			{
				SourceReactiveLiquidMaterials.Add(Pair.Value.InputB);
			}
		}
	}
	ContactRuleIds.Sort(FNameLexicalLess());
	if (ContactRuleIds.IsEmpty()
		&& PropagatingStimulusMaterials.IsEmpty())
	{
		return;
	}

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	MaterialSimulation->GetActiveCells(Cells);
	int32 AcceptedReactions = 0;
	TArray<FPendingMaterialStimulus> PendingStimuli =
		MoveTemp(PendingMaterialStimuli);
	PendingMaterialStimuli.Reset();
	TMap<FIntPoint, FVector> AuthoredContactPointByCell;
	TMap<FIntPoint, TArray<TWeakObjectPtr<AFragment2DSourceActor>>>
		AuthoredSourcesByCell;
	AuthoredContactPointByCell.Reserve(PendingStimuli.Num());
	AuthoredSourcesByCell.Reserve(PendingStimuli.Num());
	for (const FPendingMaterialStimulus& Stimulus : PendingStimuli)
	{
		if (MaterialSimulation->GetMaterialAt(Stimulus.WorldCell)
				== Stimulus.MaterialId)
		{
			AuthoredContactPointByCell.FindOrAdd(Stimulus.WorldCell) =
				Stimulus.WorldLocation;
			TArray<TWeakObjectPtr<AFragment2DSourceActor>>& CellSources =
				AuthoredSourcesByCell.FindOrAdd(Stimulus.WorldCell);
			for (const TWeakObjectPtr<AFragment2DSourceActor>& Source
				: Stimulus.AuthoredSources)
			{
				if (Source.IsValid())
				{
					CellSources.AddUnique(Source);
				}
			}
		}
		if (AcceptedReactions >= MaxMaterialSourceReactionsPerFrame)
		{
			PendingMaterialStimuli.Add(Stimulus);
			continue;
		}
		AcceptedReactions += ApplyMaterialStimulusAtWorldLocation(
			Stimulus.WorldLocation,
			Stimulus.MaterialId,
			Stimulus.EventSeed);
	}
	TSet<FGuid> ReactedAggregateIds;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		if (AcceptedReactions >= MaxMaterialSourceReactionsPerFrame)
		{
			break;
		}
		const FMatterFluxMaterialDefinition* CellMaterial =
			Registry.Materials.Find(Cell.MaterialId);
		if (!CellMaterial)
		{
			continue;
		}
		const bool bPropagatingStimulus =
			PropagatingStimulusMaterials.Contains(Cell.MaterialId);
		const bool bContactStimulus =
			CellMaterial->Phase == EMatterFluxMaterialPhase::Liquid
			&& SourceReactiveLiquidMaterials.Contains(Cell.MaterialId);
		if (!bPropagatingStimulus && !bContactStimulus)
		{
			continue;
		}

		const FVector LocalColumnBottom(
			(static_cast<double>(Cell.WorldCell.X) + 0.5)
				* MaterialSimulationCellSize,
			(static_cast<double>(Cell.WorldCell.Y) + 0.5)
				* MaterialSimulationCellSize,
			static_cast<double>(Cell.SupportHeight));
		const MatterFlux::Material::FMaterialContactGeometry ContactGeometry =
			MatterFlux::Material::BuildMaterialContactGeometry(
				CellMaterial->Phase,
				MaterialSimulationCellSize,
				MaterialLiquidColumnHeight);
		if (!ContactGeometry.IsValid())
		{
			continue;
		}
		const FVector LocalColumnCenter = LocalColumnBottom
			+ FVector(0.0f, 0.0f, ContactGeometry.CenterOffsetZ);
		const FVector* AuthoredContactPoint =
			AuthoredContactPointByCell.Find(Cell.WorldCell);
		const FVector WorldColumnCenter = AuthoredContactPoint
			? *AuthoredContactPoint
			: GetActorTransform().TransformPosition(LocalColumnCenter);
		const FVector WorldContactOrigin = AuthoredContactPoint
			? *AuthoredContactPoint
			: GetActorTransform().TransformPosition(
				LocalColumnBottom + FVector(
					0.0f,
					0.0f,
					FMath::Min(
						MaterialSimulationCellSize,
						ContactGeometry.ColumnHeight) * 0.5f));
		FVector ContactHalfExtent = ContactGeometry.HalfExtent;
		if (AuthoredContactPoint)
		{
			// A projectile impact is authored on the visible face of an
			// extruded 2D Source. A half-cell overlap volume can sit entirely
			// outside that face after collision skin/floating-point adjustment,
			// especially for independently spawned house wall slices. Give only
			// the initial impact query enough horizontal tolerance to cross the
			// source extrusion. The canonical damage shape below remains a local
			// one-cell cut, so this does not increase corrosion range.
			const float ImpactContactTolerance =
				MaterialSimulationCellSize * 4.0f;
			ContactHalfExtent.X = FMath::Max(
				ContactHalfExtent.X,
				ImpactContactTolerance);
			ContactHalfExtent.Y = FMath::Max(
				ContactHalfExtent.Y,
				ImpactContactTolerance);
		}
		const FBox ContactBounds = FBox::BuildAABB(
			WorldColumnCenter,
			ContactHalfExtent);
		if (bPropagatingStimulus && !bContactStimulus)
		{
			AcceptedReactions += ApplyMaterialStimulusAtWorldLocation(
				WorldColumnCenter,
				Cell.MaterialId,
				MapSeed
					^ MaterialSimulation->GetLogicalStep()
					^ static_cast<int32>(GetTypeHash(Cell.WorldCell)),
				ContactGeometry.RadialContactRadius);
		}
		if (!bContactStimulus
			|| AcceptedReactions >= MaxMaterialSourceReactionsPerFrame)
		{
			continue;
		}
		// Contact chemistry must reach pristine sources that are still owned by
		// the logical batch proxy. Materialize only this liquid-contact volume;
		// propagating gas uses the logical runtime and must not spawn nearby
		// source Actors merely for overlap inspection.
		TArray<AFragment2DSourceActor*> Sources;
		GatherFragmentSourcesInBounds(ContactBounds, Sources);
		if (const TArray<TWeakObjectPtr<AFragment2DSourceActor>>*
			AuthoredSources = AuthoredSourcesByCell.Find(Cell.WorldCell))
		{
			for (const TWeakObjectPtr<AFragment2DSourceActor>& Source
				: *AuthoredSources)
			{
				if (AFragment2DSourceActor* ResolvedSource = Source.Get())
				{
					Sources.AddUnique(ResolvedSource);
				}
			}
		}
		Sources.Sort([](
			const AFragment2DSourceActor& Left,
			const AFragment2DSourceActor& Right)
		{
			// 一个整体物体必须由根 Source 发起破坏事务。若先按 GUID
			// 命中枝干/叶片，它会先生成独立物理碎片，随后根事务又把
			// 整棵树转成 carrier，两套实体便会互相穿过。
			if (Left.bAggregateRoot != Right.bAggregateRoot)
			{
				return Left.bAggregateRoot;
			}
			return Left.SourceId.ToString(EGuidFormats::Digits)
				< Right.SourceId.ToString(EGuidFormats::Digits);
		});
		for (AFragment2DSourceActor* Source : Sources)
		{
			if (AcceptedReactions >= MaxMaterialSourceReactionsPerFrame)
			{
				break;
			}
			if (!IsValid(Source)
				|| Source->IsActorBeingDestroyed()
				|| Source->bBroken
				|| Source->SourceMaterialId.IsNone())
			{
				continue;
			}
			// 液体的接触包围盒可能只覆盖方柱背面的深度切片。整体物体的
			// 化学事务必须始终从 aggregate root 发起，否则该切片会比
			// 正面多提交一次 revision，树桩最终就会变成 L 形。
			if (Source->AggregateId.IsValid() && !Source->bAggregateRoot)
			{
				for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
					: GeneratedFragmentSources)
				{
					AFragment2DSourceActor* CandidateRoot = Pair.Value;
					if (IsValid(CandidateRoot)
						&& !CandidateRoot->bBroken
						&& CandidateRoot->bAggregateRoot
						&& CandidateRoot->AggregateId == Source->AggregateId)
					{
						Source = CandidateRoot;
						break;
					}
				}
			}
			if (Source->AggregateId.IsValid()
				&& ReactedAggregateIds.Contains(Source->AggregateId))
			{
				continue;
			}

			const FMatterFluxReactionDefinition* MatchingRule = nullptr;
			MatterFlux::Reaction::FContactResult ContactResult;
			for (const FName RuleId : ContactRuleIds)
			{
				const FMatterFluxReactionDefinition& Rule =
					Registry.Reactions.FindChecked(RuleId);
				MatterFlux::Reaction::FDeterministicContext Context;
				Context.Seed = MapSeed;
				Context.Tick = static_cast<uint32>(
					MaterialSimulation->GetLogicalStep());
				Context.FirstCell = Cell.WorldCell;
				const uint32 SourceHash = GetTypeHash(Source->SourceId);
				Context.SecondCell = FIntPoint(
					static_cast<int32>(SourceHash & 0xffffu),
					static_cast<int32>(SourceHash >> 16u));
				if (MatterFlux::Reaction::FMaterialReactionEngine::EvaluateContact(
						Rule,
						Cell.MaterialId,
						Source->SourceMaterialId,
						Context,
						ContactResult)
					&& ContactResult.bReacted)
				{
					MatchingRule = &Rule;
					break;
				}
			}
			if (!MatchingRule)
			{
				continue;
			}

			const FGuid ReactionSourceId = Source->SourceId;
			const FGuid ReactionAggregateId = Source->AggregateId;
			bool bNeedsAggregateCarrier = false;
			bool bHasAggregateCarrier = false;
			if (Source->bAggregateRoot && ReactionAggregateId.IsValid())
			{
				for (const TPair<FGuid, TWeakObjectPtr<AFragment2DActor>>& Pair
					: DynamicAggregateCarriers)
				{
					const MatterFlux::PlayableLevel::FLevelFragmentSource* Definition =
						FindFragmentSourceDefinition(Pair.Key);
					if (Pair.Value.IsValid()
						&& Definition
						&& Definition->AggregateId == ReactionAggregateId)
					{
						bHasAggregateCarrier = true;
						break;
					}
				}
				// 第一次腐蚀切断树根时，整棵树仍需要成为一个物理载体。
				// 先物化同 aggregate 成员，既让根事务能吸收它们，也用来
				// 区分“完整树”与之后只剩在地形中的木桩。
				MaterializeFragmentAggregate(ReactionAggregateId);
				bNeedsAggregateCarrier = !bHasAggregateCarrier;
			}
			const FBox SourceBounds = Source->GetCanonicalWorldBounds();
			const FVector ContactPoint = SourceBounds.IsValid
				? SourceBounds.GetClosestPointTo(WorldContactOrigin)
				: Source->GetActorLocation();
			FFragmentDamageEvent Damage;
			Damage.SourceId = ReactionSourceId;
			Damage.BaseRevision = Source->Revision;
			// Acid should eat a horizontal layer through the contacted source.
			// A circular cut wide enough to span a two-cell trunk also reaches
			// into the supporting rows below it and can erase the whole stump.
			Damage.DamageShape.Type = EFragmentDamageShapeType::Box;
			// FragmentGeometry evaluates the damage shape in Source-local X/Z.
			// Preserve the hit Source's orientation so rotated house walls do not
			// receive an identity/world-horizontal box that misses their mask.
			Damage.DamageShape.WorldTransform = Source->GetActorTransform();
			Damage.DamageShape.WorldTransform.SetLocation(ContactPoint);
			Damage.DamageShape.Extents = FVector2D(
				FMath::Max(
					Source->GetCellSize() * 1.5f,
					MaterialSimulationCellSize * 0.2f),
				Source->GetCellSize() * 0.45f);
			Damage.DamagePower = bNeedsAggregateCarrier ? 1000.0f : 0.0f;
			Damage.bDissolveDetachedFragments = !bNeedsAggregateCarrier;
			Damage.EventSeed = MapSeed
				^ MaterialSimulation->GetLogicalStep()
				^ static_cast<int32>(GetTypeHash(Cell.WorldCell))
				^ static_cast<int32>(GetTypeHash(ReactionSourceId));
			bool bDamageAccepted =
				UFragmentSimulationSubsystem::ExecuteFragmentDamage(
					Source,
					Damage);
			// 载体已经存在后，酸继续溶解的是地面上留下的整根树桩。
			// 对所有平行树干切片复用根切片的局部 X/Z 切割平面，避免
			// 前层逐步变矮而后层保留成一块高木板，最终形成 L 形。
			if (bHasAggregateCarrier && ReactionAggregateId.IsValid())
			{
				TArray<AFragment2DSourceActor*> ParallelStumpSlices;
				for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
					: GeneratedFragmentSources)
				{
					AFragment2DSourceActor* Member = Pair.Value;
					FFragmentDamageEvent MemberDamage;
					if (IsValid(Member)
						&& Member != Source
						&& !Member->bBroken
						&& Member->BuildSynchronizedDamageEventFrom(
							*Source,
							Damage,
							MemberDamage))
					{
						ParallelStumpSlices.Add(Member);
					}
				}
				ParallelStumpSlices.Sort([](
					const AFragment2DSourceActor& Left,
					const AFragment2DSourceActor& Right)
				{
					return Left.SourceId.ToString(EGuidFormats::Digits)
						< Right.SourceId.ToString(EGuidFormats::Digits);
				});
				for (AFragment2DSourceActor* Member : ParallelStumpSlices)
				{
					FFragmentDamageEvent MemberDamage;
					if (Member->BuildSynchronizedDamageEventFrom(
						*Source,
						Damage,
						MemberDamage))
					{
						MemberDamage.DamagePower = 0.0f;
						MemberDamage.bDissolveDetachedFragments = true;
						bDamageAccepted |=
							UFragmentSimulationSubsystem::ExecuteFragmentDamage(
								Member,
								MemberDamage);
					}
				}
			}
			if (!bDamageAccepted)
			{
				continue;
			}

			++AcceptedReactions;
			if (ReactionAggregateId.IsValid())
			{
				ReactedAggregateIds.Add(ReactionAggregateId);
			}
			if (ContactResult.FirstMaterial != Cell.MaterialId)
			{
				MaterialSimulation->SetCell(
					Cell.WorldCell,
					ContactResult.FirstMaterial == TEXT("empty")
						? NAME_None
						: ContactResult.FirstMaterial);
			}

			const FMatterFluxMaterialDefinition* SourceOutput =
				Registry.Materials.Find(ContactResult.SecondMaterial);
			if (SourceOutput
				&& SourceOutput->Phase == EMatterFluxMaterialPhase::Gas)
			{
				static const FIntPoint GasOffsets[] = {
					FIntPoint(1, 0), FIntPoint(0, 1),
					FIntPoint(-1, 0), FIntPoint(0, -1),
					FIntPoint(1, 1), FIntPoint(-1, 1),
					FIntPoint(-1, -1), FIntPoint(1, -1)
				};
				const uint32 FirstOffset = GetTypeHash(ReactionSourceId)
					% UE_ARRAY_COUNT(GasOffsets);
				for (int32 Offset = 0;
					Offset < UE_ARRAY_COUNT(GasOffsets);
					++Offset)
				{
					const FIntPoint GasCell = Cell.WorldCell
						+ GasOffsets[(FirstOffset + Offset)
							% UE_ARRAY_COUNT(GasOffsets)];
					if (MaterialSimulation->GetMaterialAt(GasCell).IsNone()
						&& MaterialSimulation->SetCell(
							GasCell,
							ContactResult.SecondMaterial))
					{
						break;
					}
				}
			}
			// Contact evaluation uses a stable pre-pass snapshot. Once the
			// particle has transformed, it cannot react with another overlapping
			// Source from that stale snapshot in the same simulation step.
			if (ContactResult.FirstMaterial != Cell.MaterialId)
			{
				break;
			}
		}
		if (bPropagatingStimulus
			&& bContactStimulus
			&& AcceptedReactions < MaxMaterialSourceReactionsPerFrame)
		{
			AcceptedReactions += ApplyMaterialStimulusAtWorldLocation(
				WorldColumnCenter,
				Cell.MaterialId,
				MapSeed
					^ MaterialSimulation->GetLogicalStep()
					^ static_cast<int32>(GetTypeHash(Cell.WorldCell)),
				ContactGeometry.RadialContactRadius);
		}
	}
	if (AcceptedReactions > 0)
	{
		bMaterialVisualizationDirty = true;
	}
}

void AMatterFluxPlayableWorldActor::RebuildMaterialVisualization(
	const FMatterFluxContentRegistry& Registry)
{
	if (GetNetMode() == NM_DedicatedServer
		|| !MaterialSimulation
		|| !CubeMesh)
	{
		DestroyMaterialVisualization();
		return;
	}
	bMaterialVisualizationDirty = false;
	MaterialVisualizationAccumulator = 0.0f;

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	if (DesiredTerrainChunks.IsEmpty())
	{
		// Initial custom-map projection can run before terrain streaming has built
		// its first desired window. The simulation's small resident set is the
		// bounded, immediately available fallback for that one rebuild.
		MaterialSimulation->GetResidentCells(Cells);
	}
	else
	{
		TSet<FIntPoint> VisibleMaterialChunkSet;
		for (const FIntPoint TerrainChunk : DesiredTerrainChunks)
		{
			const FIntPoint MinimumCell =
				TerrainChunk * TerrainStreamingChunkSize;
			const FIntPoint MaximumCellInclusive = MinimumCell
				+ FIntPoint(TerrainStreamingChunkSize - 1);
			const FIntPoint MinimumMaterialChunk =
				ToMaterialRenderChunk(
					MinimumCell, MaterialSimulationChunkSize);
			const FIntPoint MaximumMaterialChunk =
				ToMaterialRenderChunk(
					MaximumCellInclusive, MaterialSimulationChunkSize);
			for (int32 ChunkY = MinimumMaterialChunk.Y;
				ChunkY <= MaximumMaterialChunk.Y;
				++ChunkY)
			{
				for (int32 ChunkX = MinimumMaterialChunk.X;
					ChunkX <= MaximumMaterialChunk.X;
					++ChunkX)
				{
					VisibleMaterialChunkSet.Add(FIntPoint(ChunkX, ChunkY));
				}
			}
		}
		TArray<FIntPoint> VisibleMaterialChunks =
			VisibleMaterialChunkSet.Array();
		MaterialSimulation->GetCellsInChunks(
			VisibleMaterialChunks, Cells);
	}
	TMap<
		FMaterialVisualKey,
		TArray<MatterFlux::Material::FCellSnapshot>> MaterialCells;
	TMap<FName, TArray<MatterFlux::Material::FCellSnapshot>>
		LiquidMaterialCells;
	TMap<FIntPoint, TSet<FName>> CurrentMaterialsByChunk;
	TMap<FName, TMap<FIntPoint, TArray<int32>>>
		LiquidCellIndicesByMaterialAndChunk;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		const FMatterFluxMaterialDefinition* Material =
			Registry.Materials.Find(Cell.MaterialId);
		if (!Material)
		{
			continue;
		}
		if (Material->Phase == EMatterFluxMaterialPhase::Liquid)
		{
			TArray<MatterFlux::Material::FCellSnapshot>& MaterialLiquidCells =
				LiquidMaterialCells.FindOrAdd(Cell.MaterialId);
			const int32 CellIndex = MaterialLiquidCells.Add(Cell);
			const FIntPoint RenderChunk = ToMaterialRenderChunk(
				Cell.WorldCell, MaterialSimulationChunkSize);
			CurrentMaterialsByChunk.FindOrAdd(RenderChunk).Add(Cell.MaterialId);
			LiquidCellIndicesByMaterialAndChunk.FindOrAdd(Cell.MaterialId)
				.FindOrAdd(RenderChunk)
				.Add(CellIndex);
			continue;
		}
		const FIntPoint Chunk(
			FMath::FloorToInt(
				static_cast<double>(Cell.WorldCell.X)
					/ MaterialSimulationChunkSize),
			FMath::FloorToInt(
				static_cast<double>(Cell.WorldCell.Y)
					/ MaterialSimulationChunkSize));
		MaterialCells.FindOrAdd({ Cell.MaterialId, Chunk }).Add(Cell);
	}

	TSet<FName> DesiredComponents;
	for (const TPair<
		FMaterialVisualKey,
		TArray<MatterFlux::Material::FCellSnapshot>>& Pair
		: MaterialCells)
	{
		DesiredComponents.Add(MakeMaterialVisualComponentKey(Pair.Key));
	}

	TSet<FName> ActiveComponents;
	for (TPair<
		FMaterialVisualKey,
		TArray<MatterFlux::Material::FCellSnapshot>>& Pair
		: MaterialCells)
	{
		const FMatterFluxMaterialDefinition* Material =
			Registry.Materials.Find(Pair.Key.MaterialId);
		if (!Material)
		{
			continue;
		}
		Pair.Value.Sort(
			[](const MatterFlux::Material::FCellSnapshot& Left,
				const MatterFlux::Material::FCellSnapshot& Right)
			{
				return Left.WorldCell.Y != Right.WorldCell.Y
					? Left.WorldCell.Y < Right.WorldCell.Y
					: Left.WorldCell.X < Right.WorldCell.X;
			});

		TArray<FTransform> Transforms;
		Transforms.Reserve(Pair.Value.Num());
		for (const MatterFlux::Material::FCellSnapshot& Cell : Pair.Value)
		{
			float Thickness = 8.0f;
			float HeightOffset = Thickness * 0.5f;
			float VerticalScale = Thickness / 100.0f;
			if (Material->Phase == EMatterFluxMaterialPhase::Liquid)
			{
				// 动态液体使用没有侧壁的平面；相邻格边缘刚好拼接成
				// 连续液面。液量用于流动、浮力和外轮廓收边；同一承托
				// 高度上的顶面保持共面，避免逐格液量差形成百叶窗条纹。
				// 边缘也保留完整像素格：随机轮廓来自占用格集合，不能靠
				// 缩放单格制造会把液面切碎的视觉缝隙。
				Thickness = MaterialLiquidVisualThickness;
				HeightOffset = Thickness;
				VerticalScale = 1.0f;
			}
			else if (Material->Phase == EMatterFluxMaterialPhase::Powder)
			{
				// Amount is a conserved powder-column volume. Rendering the column
				// at its actual height makes the solver's angle of repose visible;
				// a fixed 14 cm slab made every accumulated sand cell look flat.
				constexpr float FullPowderColumnHeight = 14.0f;
				Thickness = FMath::Max(
					FullPowderColumnHeight
						* static_cast<float>(Cell.Amount) / 255.0f,
					1.0f);
				HeightOffset = Thickness * 0.5f;
				VerticalScale = Thickness / 100.0f;
			}
			else if (Material->Phase == EMatterFluxMaterialPhase::Gas)
			{
				Thickness = 10.0f;
				HeightOffset = 30.0f;
			}
			else if (Material->Phase
				== EMatterFluxMaterialPhase::StaticSolid)
			{
				Thickness = 18.0f;
				HeightOffset = Thickness * 0.5f;
			}
			const float HorizontalScale =
				Material->Phase == EMatterFluxMaterialPhase::Gas
					? MaterialSimulationCellSize * 0.26f / 100.0f
					: MaterialSimulationCellSize / 100.0f;
			Transforms.Emplace(
				FRotator::ZeroRotator,
				FVector(
					(static_cast<float>(Cell.WorldCell.X) + 0.5f)
						* MaterialSimulationCellSize,
					(static_cast<float>(Cell.WorldCell.Y) + 0.5f)
						* MaterialSimulationCellSize,
					static_cast<float>(Cell.SupportHeight)
						+ HeightOffset),
				FVector(
					HorizontalScale,
					HorizontalScale,
					VerticalScale));
		}

		const FName ComponentKey =
			MakeMaterialVisualComponentKey(Pair.Key);
		if (!GeneratedMaterialInstances.Contains(ComponentKey))
		{
			FName RecycledKey = NAME_None;
			for (const TPair<
				FName,
				TObjectPtr<UInstancedStaticMeshComponent>>& Candidate
				: GeneratedMaterialInstances)
			{
				if (!DesiredComponents.Contains(Candidate.Key)
					&& MaterialVisualizationMaterials.FindRef(
						Candidate.Key) == Pair.Key.MaterialId)
				{
					RecycledKey = Candidate.Key;
					break;
				}
			}
			if (!RecycledKey.IsNone())
			{
				TObjectPtr<UInstancedStaticMeshComponent> Recycled;
				GeneratedMaterialInstances.RemoveAndCopyValue(
					RecycledKey,
					Recycled);
				GeneratedMaterialInstances.Add(ComponentKey, Recycled);
				MaterialVisualizationHashes.Remove(RecycledKey);
				MaterialVisualizationMaterials.Remove(RecycledKey);
				MaterialVisualizationMaterials.Add(
					ComponentKey,
					Pair.Key.MaterialId);
			}
		}
		const bool bWasPresent =
			GeneratedMaterialInstances.Contains(ComponentKey)
			&& MaterialVisualizationHashes.Contains(ComponentKey);
		UInstancedStaticMeshComponent* Instances =
			FindOrCreateMaterialComponent(
				ComponentKey,
				Pair.Key.MaterialId,
				*Material);
		if (!Instances)
		{
			continue;
		}
		ActiveComponents.Add(ComponentKey);
		const uint64 VisualHash = BuildMaterialVisualHash(
			Pair.Value,
			Material->Phase);
		if (!bWasPresent
			|| MaterialVisualizationHashes.FindRef(ComponentKey)
				!= VisualHash)
		{
			MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
				*Instances,
				Transforms);
			MaterialVisualizationHashes.Add(ComponentKey, VisualHash);
		}
	}

	for (const TPair<
		FName,
		TObjectPtr<UInstancedStaticMeshComponent>>& Pair
		: GeneratedMaterialInstances)
	{
		if (IsValid(Pair.Value)
			&& !ActiveComponents.Contains(Pair.Key))
		{
			// Keep one bounded pool per material. Reusing an off-screen chunk's
			// ISM avoids UObject registration and render-proxy creation on every
			// streaming boundary, while preserving the last complete frame until
			// the replacement transforms are ready.
			Pair.Value->SetVisibility(false, true);
			Pair.Value->SetHiddenInGame(true, true);
		}
	}
	TArray<FIntPoint> DirtyLiquidChunks;
	MaterialSimulation->ConsumeProjectionDirtyChunks(DirtyLiquidChunks);
	auto EnqueueLiquidProjectionChunks = [this](
		const TConstArrayView<FIntPoint> Chunks)
	{
		for (const FIntPoint Chunk : Chunks)
		{
			if (!PendingLiquidProjectionDirtyChunks.Contains(Chunk))
			{
				LiquidProjectionDirtyEnqueueOrders.Add(
					Chunk, NextLiquidProjectionDirtyEnqueueOrder++);
			}
			PendingLiquidProjectionDirtyChunks.Add(Chunk);
		}
	};
	EnqueueLiquidProjectionChunks(DirtyLiquidChunks);
	if (GeneratedLiquidLayerMeshes.IsEmpty())
	{
		CurrentMaterialsByChunk.GetKeys(DirtyLiquidChunks);
		EnqueueLiquidProjectionChunks(DirtyLiquidChunks);
	}
	TSet<FIntPoint> RenderableLiquidChunks;
	CurrentMaterialsByChunk.GetKeys(RenderableLiquidChunks);
	for (const TPair<FName, FIntPoint>& Existing : LiquidProjectionChunks)
	{
		RenderableLiquidChunks.Add(Existing.Value);
	}
	if (bCaptureInitialLiquidProjectionRequirements)
	{
		// SeedMaterialSimulation runs before the first visual rebuild. Capture the
		// visible liquid chunks discovered by that rebuild once and explicitly
		// enqueue the same set. Terrain-only seed dirtiness used to enqueue these by
		// accident; after sparse support seeding, recording a requirement without a
		// matching transaction would leave the entry barrier waiting forever.
		TArray<FIntPoint> InitialLiquidChunks =
			RenderableLiquidChunks.Array();
		EnqueueLiquidProjectionChunks(InitialLiquidChunks);
		PendingInitialLiquidProjectionChunks.Append(RenderableLiquidChunks);
		bCaptureInitialLiquidProjectionRequirements = false;
	}
	for (auto Iterator = PendingLiquidProjectionDirtyChunks.CreateIterator();
		Iterator;
		++Iterator)
	{
		if (!RenderableLiquidChunks.Contains(*Iterator))
		{
			LiquidProjectionDirtyEnqueueOrders.Remove(*Iterator);
			PendingInitialLiquidProjectionChunks.Remove(*Iterator);
			Iterator.RemoveCurrent();
		}
	}
	for (auto Iterator = PendingInitialLiquidProjectionChunks.CreateIterator();
		Iterator;
		++Iterator)
	{
		if (!RenderableLiquidChunks.Contains(*Iterator))
		{
			Iterator.RemoveCurrent();
		}
	}
	DirtyLiquidChunks = PendingLiquidProjectionDirtyChunks.Array();
	const FIntPoint ProjectionFocusChunk = ToMaterialRenderChunk(
		ReplicatedMaterialSimulationFocus,
		MaterialSimulationChunkSize);
	LastLiquidProjectionDirtyChunkCount = DirtyLiquidChunks.Num();
	// Projection mesh generation blocks on its worker batch. Keep the first
	// visible result responsive by spreading a large initial/streaming dirty set
	// across quiet frames instead of waiting for the entire liquid window once.
	MatterFlux::Rendering::SelectLiquidProjectionChunksForRebuild(
		DirtyLiquidChunks,
		ProjectionFocusChunk,
		DirtyLiquidChunks,
		&LiquidProjectionDirtyEnqueueOrders,
		MaxLiquidProjectionChunksPerVisualization);
	for (const FIntPoint DirtyChunk : DirtyLiquidChunks)
	{
		PendingLiquidProjectionDirtyChunks.Remove(DirtyChunk);
		LiquidProjectionDirtyEnqueueOrders.Remove(DirtyChunk);
		PendingInitialLiquidProjectionChunks.Remove(DirtyChunk);
	}
	LastLiquidProjectionRebuiltChunkCount = 0;
	LastLiquidProjectionCheckerboardPassCount = 0;

	struct FLiquidChunkBuildJob
	{
		FName ComponentKey = NAME_None;
		FName MaterialId = NAME_None;
		FIntPoint Chunk = FIntPoint::ZeroValue;
		TArray<MatterFlux::Material::FCellSnapshot> CellsWithHalo;
		MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	};
	TArray<FLiquidChunkBuildJob> LiquidBuildJobs;
	for (const FIntPoint DirtyChunk : DirtyLiquidChunks)
	{
		TSet<FName> MaterialsToBuild =
			CurrentMaterialsByChunk.FindRef(DirtyChunk);
		for (const TPair<FName, FIntPoint>& Existing : LiquidProjectionChunks)
		{
			if (Existing.Value == DirtyChunk)
			{
				MaterialsToBuild.Add(
					LiquidProjectionMaterials.FindRef(Existing.Key));
			}
		}
		TArray<FName> SortedMaterials = MaterialsToBuild.Array();
		SortedMaterials.Sort(FNameLexicalLess());
		const FIntPoint CoreMinimum =
			DirtyChunk * MaterialSimulationChunkSize;
		const FIntPoint HaloMinimum = CoreMinimum - FIntPoint(1, 1);
		const FIntPoint HaloMaximum = CoreMinimum
			+ FIntPoint(MaterialSimulationChunkSize + 1);
		for (const FName MaterialId : SortedMaterials)
		{
			if (MaterialId.IsNone())
			{
				continue;
			}
			FLiquidChunkBuildJob& Job = LiquidBuildJobs.AddDefaulted_GetRef();
			Job.ComponentKey = MakeLiquidProjectionComponentKey(
				MaterialId, DirtyChunk);
			Job.MaterialId = MaterialId;
			Job.Chunk = DirtyChunk;
			if (const TMap<FIntPoint, TArray<int32>>* MaterialChunks =
				LiquidCellIndicesByMaterialAndChunk.Find(MaterialId))
			{
				const TArray<MatterFlux::Material::FCellSnapshot>&
					MaterialLiquidCells = LiquidMaterialCells.FindChecked(MaterialId);
				// The projection needs one cell of halo, so only the core chunk
				// and its eight neighbours can contribute. Looking up those nine
				// buckets avoids rescanning every visible liquid cell once per
				// dirty chunk during a streaming boundary.
				for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
				{
					for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
					{
						const TArray<int32>* ChunkCellIndices = MaterialChunks->Find(
								DirtyChunk + FIntPoint(OffsetX, OffsetY));
						if (!ChunkCellIndices)
						{
							continue;
						}
						for (const int32 CellIndex : *ChunkCellIndices)
						{
							const MatterFlux::Material::FCellSnapshot& Cell =
								MaterialLiquidCells[CellIndex];
							if (Cell.WorldCell.X >= HaloMinimum.X
								&& Cell.WorldCell.Y >= HaloMinimum.Y
								&& Cell.WorldCell.X < HaloMaximum.X
								&& Cell.WorldCell.Y < HaloMaximum.Y)
							{
								Job.CellsWithHalo.Add(Cell);
							}
						}
					}
				}
			}
		}
	}

	TArray<FIntPoint> EvenChunks;
	TArray<FIntPoint> OddChunks;
	MatterFlux::Rendering::PartitionLiquidProjectionChunksCheckerboard(
		DirtyLiquidChunks, EvenChunks, OddChunks);
	TSet<FIntPoint> EvenChunkSet;
	EvenChunkSet.Append(EvenChunks);
	const float ProjectionCellSize = MaterialSimulationCellSize;
	const float ProjectionColumnHeight = MaterialLiquidColumnHeight;
	const int32 ProjectionChunkSize = MaterialSimulationChunkSize;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		TArray<int32> PassJobs;
		for (int32 JobIndex = 0; JobIndex < LiquidBuildJobs.Num(); ++JobIndex)
		{
			const bool bEven = EvenChunkSet.Contains(LiquidBuildJobs[JobIndex].Chunk);
			if (bEven == (Pass == 0))
			{
				PassJobs.Add(JobIndex);
			}
		}
		if (PassJobs.IsEmpty())
		{
			continue;
		}
		++LastLiquidProjectionCheckerboardPassCount;
		ParallelFor(PassJobs.Num(), [&](const int32 PassJobIndex)
		{
			FLiquidChunkBuildJob& Job = LiquidBuildJobs[PassJobs[PassJobIndex]];
			MatterFlux::Rendering::BuildLiquidSurfaceChunkProjection(
				Job.CellsWithHalo,
				ProjectionCellSize,
				ProjectionColumnHeight,
				Job.Chunk,
				ProjectionChunkSize,
				Job.Projection);
		});
	}
	for (FLiquidChunkBuildJob& Job : LiquidBuildJobs)
	{
		const FMatterFluxMaterialDefinition* Material =
			Registry.Materials.Find(Job.MaterialId);
		if (!Material)
		{
			continue;
		}
		ApplyLiquidMaterialChunkMesh(
			Job.ComponentKey, Job.MaterialId, Job.Chunk, *Material, Job.Projection);
		++LastLiquidProjectionRebuiltChunkCount;
	}

	TSet<FName> ActiveLiquidMaterials;
	for (const TPair<FName, TArray<MatterFlux::Material::FCellSnapshot>>& Pair
		: LiquidMaterialCells)
	{
		ActiveLiquidMaterials.Add(Pair.Key);
		FMatterFluxLiquidProjectionHeightAudit& Audit =
			LiquidProjectionHeightAudits.FindOrAdd(Pair.Key);
		TArray<float> CanonicalSurfaces;
		CanonicalSurfaces.Reserve(Pair.Value.Num());
		for (const MatterFlux::Material::FCellSnapshot& Cell : Pair.Value)
		{
			CanonicalSurfaces.Add(static_cast<float>(Cell.SupportHeight)
				+ MaterialLiquidColumnHeight
					* (static_cast<float>(Cell.Amount) / 255.0f));
		}
		CanonicalSurfaces.Sort();
		const int32 Middle = CanonicalSurfaces.Num() / 2;
		Audit.CanonicalMedianSurfaceZ = CanonicalSurfaces.IsEmpty()
			? 0.0f
			: (CanonicalSurfaces.Num() & 1) != 0
				? CanonicalSurfaces[Middle]
				: (CanonicalSurfaces[Middle - 1] + CanonicalSurfaces[Middle]) * 0.5f;
		constexpr float LiquidSurfaceDepthBias = 2.0f;
		Audit.RenderedMedianSurfaceZ = Audit.CanonicalMedianSurfaceZ
			+ LiquidSurfaceDepthBias;
		Audit.MedianOffset = LiquidSurfaceDepthBias;
		Audit.MaximumAbsoluteLocalOffset = LiquidSurfaceDepthBias;
		Audit.CanonicalCellCount = Pair.Value.Num();
		Audit.ProjectedCellCount = 0;
		Audit.MaximumTriangleHeightSpan = 0.0f;
		Audit.SurfacePatchCount = 0;
		for (const TPair<FName, FName>& Component : LiquidProjectionMaterials)
		{
			if (Component.Value != Pair.Key)
			{
				continue;
			}
			if (const FMatterFluxLiquidProjectionHeightAudit* ChunkAudit =
				LiquidChunkProjectionHeightAudits.Find(Component.Key))
			{
				Audit.ProjectedCellCount += ChunkAudit->ProjectedCellCount;
				Audit.MaximumTriangleHeightSpan = FMath::Max(
					Audit.MaximumTriangleHeightSpan,
					ChunkAudit->MaximumTriangleHeightSpan);
				Audit.SurfacePatchCount += ChunkAudit->SurfacePatchCount;
			}
		}
	}
	TArray<FName> StaleLiquidAudits;
	for (const TPair<FName, FMatterFluxLiquidProjectionHeightAudit>& Pair
		: LiquidProjectionHeightAudits)
	{
		if (!ActiveLiquidMaterials.Contains(Pair.Key))
		{
			StaleLiquidAudits.Add(Pair.Key);
		}
	}
	for (const FName MaterialId : StaleLiquidAudits)
	{
		LiquidProjectionHeightAudits.Remove(MaterialId);
	}
	if (!PendingLiquidProjectionDirtyChunks.IsEmpty())
	{
		bMaterialVisualizationDirty = true;
		MaterialVisualizationAccumulator = FMath::Max(
			MaterialVisualizationAccumulator,
			MaterialVisualizationInterval);
	}
}

void AMatterFluxPlayableWorldActor::DestroyMaterialVisualization()
{
	for (const TPair<
		FName,
		TObjectPtr<UInstancedStaticMeshComponent>>& Pair
		: GeneratedMaterialInstances)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->DestroyComponent();
		}
	}
	GeneratedMaterialInstances.Reset();
	for (const TPair<FName, TObjectPtr<UProceduralMeshComponent>>& Pair
		: GeneratedLiquidLayerMeshes)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->DestroyComponent();
		}
	}
	GeneratedLiquidLayerMeshes.Reset();
	LiquidProjectionMaterials.Reset();
	LiquidProjectionChunks.Reset();
	PendingLiquidProjectionDirtyChunks.Reset();
	LiquidProjectionDirtyEnqueueOrders.Reset();
	NextLiquidProjectionDirtyEnqueueOrder = 1;
	PendingInitialLiquidProjectionChunks.Reset();
	bCaptureInitialLiquidProjectionRequirements = false;
	LiquidProjectionHeightAudits.Reset();
	LiquidChunkProjectionHeightAudits.Reset();
	LastLiquidProjectionDirtyChunkCount = 0;
	LastLiquidProjectionRebuiltChunkCount = 0;
	LastLiquidProjectionCheckerboardPassCount = 0;
	MaterialVisualizationHashes.Reset();
	MaterialVisualizationMaterials.Reset();
	bMaterialVisualizationDirty = false;
	bMaterialVisualizationDeferredForStreaming = false;
	MaterialVisualizationAccumulator = 0.0f;
}

UInstancedStaticMeshComponent*
AMatterFluxPlayableWorldActor::FindOrCreateMaterialComponent(
	const FName ComponentKey,
	const FName MaterialId,
	const FMatterFluxMaterialDefinition& Material)
{
	UInstancedStaticMeshComponent* Instances =
		GeneratedMaterialInstances.FindRef(ComponentKey);
	if (!Instances)
	{
		const FName ComponentName = MakeUniqueObjectName(
			this,
			UInstancedStaticMeshComponent::StaticClass(),
			*FString::Printf(
				TEXT("MaterialCells_%s"),
				*MaterialId.ToString()));
		Instances = NewObject<UInstancedStaticMeshComponent>(
			this,
			ComponentName);
		Instances->SetupAttachment(SceneRoot);
		Instances->SetMobility(EComponentMobility::Movable);
		AddInstanceComponent(Instances);
		Instances->RegisterComponent();
		GeneratedMaterialInstances.Add(ComponentKey, Instances);
	}
	MaterialVisualizationMaterials.Add(ComponentKey, MaterialId);
	Instances->SetVisibility(true, true);
	Instances->SetHiddenInGame(false, true);

	const bool bLiquid =
		Material.Phase == EMatterFluxMaterialPhase::Liquid;
	Instances->SetStaticMesh(
		bLiquid && LiquidSurfaceMesh
			? LiquidSurfaceMesh.Get()
			: CubeMesh.Get());
	if (bLiquid)
	{
		Instances->ComponentTags.AddUnique(
			TEXT("MatterFluxSimulatedLiquidSurface"));
	}
	else
	{
		Instances->ComponentTags.Remove(
			TEXT("MatterFluxSimulatedLiquidSurface"));
	}
	Instances->SetCastShadow(
		Material.Phase
		== EMatterFluxMaterialPhase::StaticSolid);
	const bool bEnableSolidCollision =
		bEnableMaterialSimulationCollision
		&& Material.Phase
			== EMatterFluxMaterialPhase::StaticSolid;
	const bool bEnablePowderSupport =
		Material.Phase == EMatterFluxMaterialPhase::Powder;
	Instances->SetCollisionEnabled(
		bEnableSolidCollision
			? ECollisionEnabled::QueryAndPhysics
			: bEnablePowderSupport
				? ECollisionEnabled::QueryAndPhysics
				: ECollisionEnabled::NoCollision);
	Instances->SetCollisionResponseToAllChannels(
		bEnableSolidCollision ? ECR_Block : ECR_Ignore);
	if (bEnablePowderSupport)
	{
		// The disposable shape supports characters and simulated bodies. It is
		// rebuilt only from canonical powder facts and never feeds collision
		// transforms or impulses back into the falling-sand solver.
		Instances->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		Instances->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	}
	Instances->SetCanEverAffectNavigation(bEnableSolidCollision);

	UMaterialInterface* VisualizationTemplate =
		Material.Phase == EMatterFluxMaterialPhase::Gas
			? VoxelGasMaterialTemplate.Get()
			: Material.Phase == EMatterFluxMaterialPhase::Liquid
				? VoxelLiquidMaterialTemplate.Get()
				: VoxelColorMaterialTemplate.Get();
	if (VisualizationTemplate)
	{
		UMaterialInstanceDynamic* DynamicMaterial =
			Cast<UMaterialInstanceDynamic>(Instances->GetMaterial(0));
		if (!DynamicMaterial)
		{
			DynamicMaterial = UMaterialInstanceDynamic::Create(
				VisualizationTemplate,
				this);
			Instances->SetMaterial(0, DynamicMaterial);
		}
		DynamicMaterial->SetVectorParameterValue(
			TEXT("Color"),
			Material.Color);
		const bool bGas =
			Material.Phase == EMatterFluxMaterialPhase::Gas;
		if (bGas)
		{
			DynamicMaterial->SetScalarParameterValue(
				TEXT("Opacity"),
				FMath::Clamp(Material.Color.A * 0.42f, 0.12f, 0.30f));
		}
		else if (bLiquid)
		{
			ApplyLiquidOptics(*DynamicMaterial, Material);
		}
		DynamicMaterial->SetScalarParameterValue(
			TEXT("FaceContrast"),
			bGas ? 0.28f : (bLiquid ? 0.48f : 0.82f));
		DynamicMaterial->SetScalarParameterValue(
			TEXT("ColorVariation"),
			bGas ? 0.08f : (bLiquid ? 0.025f : 0.05f));
		DynamicMaterial->SetScalarParameterValue(
			TEXT("PixelSize"),
			12.0f);
		DynamicMaterial->SetScalarParameterValue(
			TEXT("Roughness"),
			bLiquid ? 0.32f : (bGas ? 0.72f : 0.90f));
		DynamicMaterial->SetScalarParameterValue(
			TEXT("ShadowLift"),
			bGas ? 0.30f : (bLiquid ? 0.22f : 0.14f));
	}
	return Instances;
}
void AMatterFluxPlayableWorldActor::RebuildFragmentSources(
	const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>& Sources,
	const bool bImmediate)
{
	DestroyGeneratedFragmentSources();
	FragmentSourceChunks.Reset();
	ProceduralPopulationChunks.Reset();
	DesiredProceduralPopulationChunks.Reset();
	PendingProceduralPopulationChunks.Reset();
	PendingProceduralPopulationRemovals.Reset();
	ProceduralPopulationFocusChunks.Reset();
	bPreferProceduralSurfaceSeed = false;
	ProceduralRiverCellsByChunk.Reset();
	PrefetchedProceduralRiverSurfaceChunks.Reset();
	SeededProceduralSurfaceChunks.Reset();
	FragmentSourceDefinitionIndex.Reset();
	FragmentSourceChunkById.Reset();
	RemovedFragmentSourceIds.Reset();
	StreamingPinnedFragmentSourceIds.Reset();
	AppliedReplicatedFragmentSourceIds.Reset();
	if (HasAuthority())
	{
		ReplicatedFragmentSourceStates.ResetAuthorityItems();
		ForceNetUpdate();
	}
	else
	{
		ReplicatedFragmentSourceStates.RequestClientFullRebuild();
	}
	// 同一 aggregate 必须驻留在同一个渲染 chunk。树冠和树干若按各自
	// 世界位置分桶，跨区块树会退化为两份网格，内部面无法统一剔除。
	TMap<FGuid, FIntPoint> AggregateRootChunks;
	const auto ChunkForLocation = [this](const FVector& Location)
	{
		const FIntPoint Cell(
			FMath::RoundToInt(
				Location.X / MatterFlux::PlayableLevel::TerrainCellSize),
			FMath::RoundToInt(
				Location.Y / MatterFlux::PlayableLevel::TerrainCellSize));
		return FIntPoint(
			FMath::FloorToInt(
				static_cast<double>(Cell.X) / TerrainStreamingChunkSize),
			FMath::FloorToInt(
				static_cast<double>(Cell.Y) / TerrainStreamingChunkSize));
	};
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source : Sources)
	{
		if (Source.AggregateId.IsValid() && Source.bAggregateRoot)
		{
			AggregateRootChunks.Add(
				Source.AggregateId,
				ChunkForLocation(Source.Transform.GetLocation()));
		}
	}
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source : Sources)
	{
		const FVector Location = Source.Transform.GetLocation();
		FIntPoint Chunk = ChunkForLocation(Location);
		if (Source.AggregateId.IsValid())
		{
			if (const FIntPoint* RootChunk =
				AggregateRootChunks.Find(Source.AggregateId))
			{
				Chunk = *RootChunk;
			}
			else
			{
				// 损坏数据没有 root 时仍保持确定性：输入稳定顺序中的首个
				// member 决定 aggregate chunk。
				Chunk = AggregateRootChunks.FindOrAdd(
					Source.AggregateId,
					Chunk);
			}
		}
		FragmentSourceChunks.FindOrAdd(Chunk).Add(Source);
		if (Source.SourceId.IsValid())
		{
			FragmentSourceChunkById.Add(Source.SourceId, Chunk);
			const FBox LocalBounds =
				BuildFragmentSourceLocalBounds(Source);
			if (LocalBounds.IsValid)
			{
				FragmentSourceDefinitionIndex.Upsert(
					Source.SourceId,
					LocalBounds);
			}
		}
	}
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->Configure(
			SceneRoot,
			VoxelColorMaterialTemplate,
			VoxelLeafMaterialTemplate,
			VoxelWoodMaterialTemplate);
		FragmentSourceProxy->SetSourceChunks(FragmentSourceChunks);
		// Proxy chunk geometry is disposable. Rebuilding it must immediately
		// re-project the canonical streamed mask/reaction facts; otherwise a
		// burned tree briefly or permanently returns as its pristine source.
		for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
			: StreamedFragmentSourceStates)
		{
			if (FragmentSourceProxy->IsProxySource(Pair.Key))
			{
				ApplyPersistentFragmentSourceStateToProxy(Pair.Key, Pair.Value);
			}
		}
		if (!HasAuthority())
		{
			ApplyReplicatedFragmentSourceStates();
		}
		// The playable forest fits inside this independently bounded proxy
		// cache. Pay triangulation during loading so ordinary movement only
		// toggles completed chunk meshes. Larger worlds retain on-demand
		// construction instead of growing memory without a bound.
		FragmentSourceProxy->PrepareSourceChunks(
			FragmentSourceProxyCacheLimit);
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
			{
				FragmentSourceProxy->SetSourceMaterialized(
					It->SourceId,
					true);
			}
		}
	}
	RefreshVisibleFragmentSources(bImmediate);
}

void AMatterFluxPlayableWorldActor::RefreshProceduralPopulation(
	const TConstArrayView<FIntPoint> OrderedDesiredChunks,
	const TConstArrayView<FIntPoint> FocusChunks)
{
	if (!TerrainHeightField.IsValid() || MapSeed == 0)
	{
		return;
	}
	const int64 FirstCellX = FMath::FloorToInt64(
		static_cast<double>(TerrainHeightField.FirstCellCenter.X)
			/ TerrainHeightField.CellSize);
	const int64 FirstCellY = FMath::FloorToInt64(
		static_cast<double>(TerrainHeightField.FirstCellCenter.Y)
			/ TerrainHeightField.CellSize);
	const int64 LastCellX = FirstCellX + TerrainHeightField.Width - 1;
	const int64 LastCellY = FirstCellY + TerrainHeightField.Height - 1;
	const auto OverlapsSeedArea = [this, FirstCellX, FirstCellY,
		LastCellX, LastCellY](const FIntPoint Chunk)
	{
		const int64 MinimumX =
			static_cast<int64>(Chunk.X) * TerrainStreamingChunkSize;
		const int64 MinimumY =
			static_cast<int64>(Chunk.Y) * TerrainStreamingChunkSize;
		const int64 MaximumX = MinimumX + TerrainStreamingChunkSize - 1;
		const int64 MaximumY = MinimumY + TerrainStreamingChunkSize - 1;
		return MinimumX <= LastCellX && MaximumX >= FirstCellX
			&& MinimumY <= LastCellY && MaximumY >= FirstCellY;
	};
	const auto OverlapsActiveCustomMap = [this, FirstCellX, FirstCellY](
		const FIntPoint Chunk)
	{
		if (!IsCustomMapActive())
		{
			return true;
		}
		const int64 MinimumCellX =
			static_cast<int64>(Chunk.X) * TerrainStreamingChunkSize;
		const int64 MinimumCellY =
			static_cast<int64>(Chunk.Y) * TerrainStreamingChunkSize;
		const int64 MaximumCellX = MinimumCellX
			+ TerrainStreamingChunkSize - 1;
		const int64 MaximumCellY = MinimumCellY
			+ TerrainStreamingChunkSize - 1;
		const float ChunkMinimumX = TerrainHeightField.FirstCellCenter.X
			+ static_cast<double>(MinimumCellX - FirstCellX)
				* TerrainHeightField.CellSize;
		const float ChunkMinimumY = TerrainHeightField.FirstCellCenter.Y
			+ static_cast<double>(MinimumCellY - FirstCellY)
				* TerrainHeightField.CellSize;
		const float ChunkMaximumX = TerrainHeightField.FirstCellCenter.X
			+ static_cast<double>(MaximumCellX - FirstCellX)
				* TerrainHeightField.CellSize;
		const float ChunkMaximumY = TerrainHeightField.FirstCellCenter.Y
			+ static_cast<double>(MaximumCellY - FirstCellY)
				* TerrainHeightField.CellSize;
		const float MapMinimumX = ActiveCustomMapScene.MinimumCell.X
			* ActiveCustomMapScene.CellSizeCentimeters;
		const float MapMinimumY = ActiveCustomMapScene.MinimumCell.Y
			* ActiveCustomMapScene.CellSizeCentimeters;
		const float MapMaximumX = ActiveCustomMapScene.MaximumCellExclusive.X
			* ActiveCustomMapScene.CellSizeCentimeters;
		const float MapMaximumY = ActiveCustomMapScene.MaximumCellExclusive.Y
			* ActiveCustomMapScene.CellSizeCentimeters;
		return ChunkMinimumX < MapMaximumX
			&& ChunkMaximumX >= MapMinimumX
			&& ChunkMinimumY < MapMaximumY
			&& ChunkMaximumY >= MapMinimumY;
	};
	DesiredProceduralPopulationChunks.Reset();
	PendingProceduralPopulationChunks.Reset();
	PendingProceduralPopulationRemovals.Reset();
	ProceduralPopulationFocusChunks = FocusChunks;
	ProceduralSurfaceSeedPriorityChunks.Reset();
	for (const FIntPoint Chunk : OrderedDesiredChunks)
	{
		if (!OverlapsSeedArea(Chunk) && OverlapsActiveCustomMap(Chunk))
		{
			DesiredProceduralPopulationChunks.Add(Chunk);
			ProceduralSurfaceSeedPriorityChunks.Add(Chunk);
			if (!ProceduralPopulationChunks.Contains(Chunk)
				&& !ProceduralPopulationChunksBuilding.Contains(Chunk))
			{
				PendingProceduralPopulationChunks.Add(Chunk);
			}
		}
	}
	ProceduralSurfaceSeedPriorityChunks.Sort(
		[this, FocusChunks](const FIntPoint A, const FIntPoint B)
		{
			const bool bAVisible = DesiredTerrainChunks.Contains(A);
			const bool bBVisible = DesiredTerrainChunks.Contains(B);
			if (bAVisible != bBVisible)
			{
				return bAVisible;
			}
			const auto MinimumDistanceSquared = [FocusChunks](
				const FIntPoint Chunk)
			{
				int64 Result = MAX_int64;
				for (const FIntPoint Focus : FocusChunks)
				{
					const int64 DeltaX =
						static_cast<int64>(Chunk.X) - Focus.X;
					const int64 DeltaY =
						static_cast<int64>(Chunk.Y) - Focus.Y;
					Result = FMath::Min(
						Result, DeltaX * DeltaX + DeltaY * DeltaY);
				}
				return Result;
			};
			const int64 ADistance = MinimumDistanceSquared(A);
			const int64 BDistance = MinimumDistanceSquared(B);
			return ADistance != BDistance
				? ADistance < BDistance
				: A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});
	for (const FIntPoint Chunk : ProceduralPopulationChunks)
	{
		if (!DesiredProceduralPopulationChunks.Contains(Chunk))
		{
			PendingProceduralPopulationRemovals.Add(Chunk);
		}
	}
	PendingProceduralPopulationRemovals.Sort(
		[](const FIntPoint A, const FIntPoint B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});
}

int32 AMatterFluxPlayableWorldActor::GetProceduralTreeAggregateCount() const
{
	TSet<FGuid> TreeAggregates;
	for (const FIntPoint Chunk : ProceduralPopulationChunks)
	{
		const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
			FragmentSourceChunks.Find(Chunk);
		if (!Sources)
		{
			continue;
		}
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: *Sources)
		{
			if (Source.AggregateId.IsValid())
			{
				TreeAggregates.Add(Source.AggregateId);
			}
		}
	}
	return TreeAggregates.Num();
}

bool AMatterFluxPlayableWorldActor::
	ProcessPendingProceduralPopulationUpdates(
		const bool bAllowSurfaceSeed,
		const bool bAllowChunkCommit,
		const bool bAllowSurfaceFinalization,
		bool* const bOutDidChunkCommit)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_ProceduralPopulation_ProcessQueue);
	if (bOutDidChunkCommit)
	{
		*bOutDidChunkCommit = false;
	}
	if (!TerrainHeightField.IsValid() || MapSeed == 0)
	{
		return false;
	}
	const double DeadlineSeconds = FPlatformTime::Seconds()
		+ static_cast<double>(ProceduralPopulationBudgetMilliseconds)
			/ 1000.0;
	int32 RemainingOperations =
		FMath::Max(MaxProceduralPopulationUpdatesPerFrame, 1);
	TArray<FIntPoint> ChunksToRemove;
	TArray<FIntPoint> ChunksToAdd;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (!AsyncPopulationBuildState.IsValid())
	{
		AsyncPopulationBuildState = MakeShared<
			FMatterFluxAsyncPopulationBuildState,
			ESPMode::ThreadSafe>();
	}
	if (!AsyncTerrainHeightField.IsValid())
	{
		AsyncTerrainHeightField = MakeShared<
			const MatterFlux::PlayableLevel::FLevelTerrain,
			ESPMode::ThreadSafe>(TerrainHeightField);
	}
	while (!PendingProceduralPopulationChunks.IsEmpty()
		&& ProceduralPopulationChunksBuilding.Num()
			< FMath::Max(MaxAsyncStreamingBuildTasks, 1))
	{
		const FIntPoint Chunk = PendingProceduralPopulationChunks[0];
		PendingProceduralPopulationChunks.RemoveAt(
			0,
			1,
			EAllowShrinking::No);
		if (!DesiredProceduralPopulationChunks.Contains(Chunk)
			|| ProceduralPopulationChunks.Contains(Chunk)
			|| ProceduralPopulationChunksBuilding.Contains(Chunk))
		{
			continue;
		}
		ProceduralPopulationChunksBuilding.Add(Chunk);
		const TSharedPtr<FMatterFluxAsyncPopulationBuildState,
			ESPMode::ThreadSafe> BuildState = AsyncPopulationBuildState;
		const TSharedPtr<const MatterFlux::PlayableLevel::FLevelTerrain,
			ESPMode::ThreadSafe> TerrainSnapshot = AsyncTerrainHeightField;
		const int32 Seed = MapSeed;
		const int32 ChunkSize = TerrainStreamingChunkSize;
		UE::Tasks::Launch(
			UE_SOURCE_LOCATION,
			[BuildState, TerrainSnapshot, Registry, Seed, Chunk, ChunkSize]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(
					MatterFlux_ProceduralPopulation_BuildChunkAsync);
				if (!BuildState.IsValid()
					|| BuildState->bCancelled.load(
						std::memory_order_acquire))
				{
					return;
				}
				FMatterFluxPreparedPopulationChunk Result;
				Result.Coordinate = Chunk;
				Result.bSuccess = TerrainSnapshot.IsValid()
					&& MatterFlux::PlayableLevel::
						BuildStreamingChunkPopulation(
							Seed,
							*TerrainSnapshot,
							Chunk,
							ChunkSize,
							Result.Population,
							Registry.Get());
				if (!BuildState->bCancelled.load(
					std::memory_order_acquire))
				{
					BuildState->CompletedChunks.Enqueue(MoveTemp(Result));
				}
			},
			// This is predictive player-facing streaming work. Background workers
			// can be occupied indefinitely by editor/render maintenance during a
			// sustained traversal, leaving the four in-flight slots wedged while
			// the desired window moves on. Normal still executes off the game
			// thread, but gives the result a frame deadline instead of best-effort
			// background semantics.
			UE::Tasks::ETaskPriority::Normal);
	}
	if (!bAllowChunkCommit)
	{
		return false;
	}
	const auto ResetSurfaceSeedProgress = [this]()
	{
		ProceduralSurfaceSeedChunkInProgress.Reset();
		bProceduralSurfaceSeedIsRiverPrefetch = false;
		ProceduralSurfaceSeedNextTerrainCell = 0;
		ProceduralSurfaceSeedMaterialCells.Reset();
		ProceduralSurfaceSeedFirstCell.Reset();
	};
	if (ProceduralSurfaceSeedChunkInProgress.IsSet()
		&& !DesiredProceduralPopulationChunks.Contains(
			ProceduralSurfaceSeedChunkInProgress.GetValue()))
	{
		ResetSurfaceSeedProgress();
	}
	FIntPoint ChunkToSeed = FIntPoint::ZeroValue;
	bool bFoundChunkToSeed = false;
	bool bRiverPrefetchOnly = false;
	if (ProceduralSurfaceSeedChunkInProgress.IsSet())
	{
		ChunkToSeed = ProceduralSurfaceSeedChunkInProgress.GetValue();
		bFoundChunkToSeed = true;
		bRiverPrefetchOnly = bProceduralSurfaceSeedIsRiverPrefetch;
	}
	else if (HasAuthority() && MaterialSimulation)
	{
		for (const FIntPoint Focus : ProceduralPopulationFocusChunks)
		{
			for (int32 OffsetY = -1; OffsetY <= 1 && !bFoundChunkToSeed;
				++OffsetY)
			{
				for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					const FIntPoint Candidate =
						Focus + FIntPoint(OffsetX, OffsetY);
					if (DesiredProceduralPopulationChunks.Contains(Candidate)
						&& !SeededProceduralSurfaceChunks.Contains(Candidate))
					{
						ChunkToSeed = Candidate;
						bFoundChunkToSeed = true;
						break;
					}
				}
			}
		}
		// Terrain is visible much farther than the active material simulation.
		// Preseed only distant chunks which the population worker proved contain
		// a river. Canonical water inside the render window receives a bounded
		// settling wake; off-screen prefetches remain archived until they are seen.
		if (!bFoundChunkToSeed)
		{
			for (const FIntPoint Candidate
				: ProceduralSurfaceSeedPriorityChunks)
			{
				if (DesiredProceduralPopulationChunks.Contains(Candidate)
					&& ProceduralRiverChunks.Contains(Candidate)
					&& !PrefetchedProceduralRiverSurfaceChunks.Contains(Candidate))
				{
					ChunkToSeed = Candidate;
					bFoundChunkToSeed = true;
					bRiverPrefetchOnly = true;
					break;
				}
			}
		}
	}
	const TArray<MatterFlux::PlayableLevel::FStreamingRiverCell>*
		RiverCellsToPrefetch = bRiverPrefetchOnly
			? ProceduralRiverCellsByChunk.Find(ChunkToSeed)
			: nullptr;
	const int32 TotalSurfaceTerrainCells = bRiverPrefetchOnly
		? (RiverCellsToPrefetch ? RiverCellsToPrefetch->Num() : 0)
		: TerrainStreamingChunkSize * TerrainStreamingChunkSize;
	const bool bSurfaceFinalizationPending =
		ProceduralSurfaceSeedChunkInProgress.IsSet()
		&& ProceduralSurfaceSeedNextTerrainCell
			+ ProceduralSurfaceSeedCellsPerFrame
			>= TotalSurfaceTerrainCells;
	const bool bSeedOnlyThisFrame =
		bAllowSurfaceSeed
		&& bFoundChunkToSeed
		&& bAllowSurfaceFinalization
		&& (bPreferProceduralSurfaceSeed
			|| bSurfaceFinalizationPending);
	int32 AdditionsThisFrame = 0;
	FMatterFluxPreparedPopulationChunk PreparedPopulation;
	while (RemainingOperations > 0
		&& AdditionsThisFrame < 1
		&& !bSeedOnlyThisFrame
		&& AsyncPopulationBuildState->CompletedChunks.Dequeue(
			PreparedPopulation))
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(
			MatterFlux_ProceduralPopulation_CommitChunk);
		const FIntPoint Chunk = PreparedPopulation.Coordinate;
		ProceduralPopulationChunksBuilding.Remove(Chunk);
		if (!PreparedPopulation.bSuccess
			|| !DesiredProceduralPopulationChunks.Contains(Chunk)
			|| ProceduralPopulationChunks.Contains(Chunk))
		{
			continue;
		}
		MatterFlux::PlayableLevel::FStreamingChunkPopulation& Population =
			PreparedPopulation.Population;
		if (Population.RiverCells.IsEmpty())
		{
			ProceduralRiverChunks.Remove(Chunk);
			ProceduralRiverCellsByChunk.Remove(Chunk);
		}
		else
		{
			ProceduralRiverChunks.Add(Chunk);
			ProceduralRiverCellsByChunk.Add(
				Chunk, MoveTemp(Population.RiverCells));
		}
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>& ChunkSources =
			FragmentSourceChunks.FindOrAdd(Chunk);
		for (MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Population.FragmentSources)
		{
			if (Source.SourceId.IsValid())
			{
				FragmentSourceChunkById.Add(Source.SourceId, Chunk);
				const FBox Bounds = BuildFragmentSourceLocalBounds(Source);
				if (Bounds.IsValid)
				{
					FragmentSourceDefinitionIndex.Upsert(
						Source.SourceId,
						Bounds);
				}
			}
			ChunkSources.Add(MoveTemp(Source));
		}
		ProceduralPopulationChunks.Add(Chunk);
		ChunksToAdd.Add(Chunk);
		++AdditionsThisFrame;
		// The next commit-capable frame belongs to one bounded surface batch.
		// Without this handoff, a sustained stream of completed population jobs
		// consumes the shared deadline before river topology can ever advance.
		bPreferProceduralSurfaceSeed = true;

		if (HasAuthority() && GetWorld() && ActiveCustomMapId.IsNone()
			&& Population.bHasHouse
			&& !GeneratedStreamedHouses.Contains(Chunk)
			&& Registry.IsValid())
		{
			const FName StructureDefinitionId =
				TEXT("structure.house.two_storey");
			const FMatterFluxStructureDefinition* StructureDefinition =
				Registry->Structures.Find(StructureDefinitionId);
			if (StructureDefinition
				&& StructureDefinition->GeneratorId
					== TEXT("two_storey_house"))
			{
				float FoundationTop = Population.HouseLocation.Z;
				const FVector2D SampleOffsets[] = {
					FVector2D::ZeroVector,
					FVector2D(-500.0f, -360.0f),
					FVector2D(-500.0f, 360.0f),
					FVector2D(500.0f, -360.0f),
					FVector2D(500.0f, 360.0f)
				};
				for (const FVector2D Offset : SampleOffsets)
				{
					FVector Sample = Population.HouseLocation;
					Sample.X += Offset.X;
					Sample.Y += Offset.Y;
					float Height = 0.0f;
					if (TrySampleTerrainHeightAtWorldLocation(Sample, Height))
					{
						FoundationTop = FMath::Max(
							FoundationTop,
							Height);
					}
				}
				const FTransform HouseTransform(
					FRotator(0.0f, 45.0f, 0.0f),
					FVector(
						Population.HouseLocation.X,
						Population.HouseLocation.Y,
						FoundationTop + 4.0f));
				AMatterFluxTwoStoreyHouseActor* House = AcquireStreamedHouse(
					HouseTransform,
					StructureDefinitionId);
				if (House)
				{
					GeneratedStreamedHouses.Add(Chunk, House);
				}
			}
		}
		--RemainingOperations;
		if (FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			break;
		}
	}

	while (RemainingOperations > 0
		&& !bSeedOnlyThisFrame
		&& (!bFoundChunkToSeed || RemainingOperations > 1)
		&& !PendingProceduralPopulationRemovals.IsEmpty())
	{
		const FIntPoint Chunk = PendingProceduralPopulationRemovals[0];
		PendingProceduralPopulationRemovals.RemoveAt(
			0,
			1,
			EAllowShrinking::No);
		if (DesiredProceduralPopulationChunks.Contains(Chunk)
			|| !ProceduralPopulationChunks.Contains(Chunk))
		{
			continue;
		}
		if (const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
			FragmentSourceChunks.Find(Chunk))
		{
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
				: *Sources)
			{
				FragmentSourceDefinitionIndex.Remove(Source.SourceId);
				FragmentSourceChunkById.Remove(Source.SourceId);
			}
		}
		FragmentSourceChunks.Remove(Chunk);
		ProceduralPopulationChunks.Remove(Chunk);
		ProceduralRiverChunks.Remove(Chunk);
		ProceduralRiverCellsByChunk.Remove(Chunk);
		if (AMatterFluxTwoStoreyHouseActor* House =
			GeneratedStreamedHouses.FindRef(Chunk))
		{
			if (IsValid(House))
			{
				ReleaseStreamedHouse(House);
			}
			GeneratedStreamedHouses.Remove(Chunk);
		}
		ChunksToRemove.Add(Chunk);
		--RemainingOperations;
		if (FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			break;
		}
	}

	bool bProcessedSurfaceSeed = false;
	// One terrain chunk contains thousands of material columns. Treating it as
	// one budgeted operation made the nominal 4 ms queue spend 25-31 ms before
	// it could observe its deadline. Continue one logical chunk across bounded
	// batches instead. Baseline encoding is finalized only on the last batch,
	// so intermediate frames do not repeatedly serialize the same chunk.
	if (RemainingOperations > 0
		&& bFoundChunkToSeed
		&& bAllowSurfaceSeed
		&& HasAuthority()
		&& MaterialSimulation
		&& FPlatformTime::Seconds() < DeadlineSeconds)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(
			MatterFlux_ProceduralPopulation_SeedSurfaceChunk);
		if (bFoundChunkToSeed)
		{
			if (!ProceduralSurfaceSeedChunkInProgress.IsSet()
				|| ProceduralSurfaceSeedChunkInProgress.GetValue()
					!= ChunkToSeed)
			{
				ResetSurfaceSeedProgress();
				ProceduralSurfaceSeedChunkInProgress = ChunkToSeed;
				bProceduralSurfaceSeedIsRiverPrefetch = bRiverPrefetchOnly;
			}
			TArray<MatterFlux::Material::FSeedCell> SeedCells;
			SeedCells.Reserve(ProceduralSurfaceSeedCellsPerFrame);
			const int64 FirstCellX = FMath::FloorToInt64(
				static_cast<double>(TerrainHeightField.FirstCellCenter.X)
					/ TerrainHeightField.CellSize);
			const int64 FirstCellY = FMath::FloorToInt64(
				static_cast<double>(TerrainHeightField.FirstCellCenter.Y)
					/ TerrainHeightField.CellSize);
			const int64 MinimumCellX =
				static_cast<int64>(ChunkToSeed.X) * TerrainStreamingChunkSize;
			const int64 MinimumCellY =
				static_cast<int64>(ChunkToSeed.Y) * TerrainStreamingChunkSize;
			const int32 TotalTerrainCells = TotalSurfaceTerrainCells;
			const int32 LastCellThisFrame = bAllowSurfaceFinalization
				? TotalTerrainCells
				: FMath::Max(TotalTerrainCells - 1, 0);
			int32 SamplesThisFrame = 0;
			while (ProceduralSurfaceSeedNextTerrainCell < LastCellThisFrame
				&& SamplesThisFrame < ProceduralSurfaceSeedCellsPerFrame
				&& FPlatformTime::Seconds() < DeadlineSeconds)
			{
				const int32 LinearCell =
					ProceduralSurfaceSeedNextTerrainCell++;
				++SamplesThisFrame;
				const MatterFlux::PlayableLevel::FStreamingRiverCell*
					RiverCell = bRiverPrefetchOnly
						&& RiverCellsToPrefetch
						&& RiverCellsToPrefetch->IsValidIndex(LinearCell)
							? &(*RiverCellsToPrefetch)[LinearCell]
							: nullptr;
				const int64 CellX = RiverCell
					? RiverCell->WorldCell.X
					: MinimumCellX
						+ LinearCell % TerrainStreamingChunkSize;
				const int64 CellY = RiverCell
					? RiverCell->WorldCell.Y
					: MinimumCellY
						+ LinearCell / TerrainStreamingChunkSize;
				float Height = 0.0f;
				uint8 ColorBand = 0;
				if (!TerrainHeightField.TrySampleWorldCell(
					CellX, CellY, Height, ColorBand))
				{
					continue;
				}
				const FVector LocalLocation(
					TerrainHeightField.FirstCellCenter.X
						+ static_cast<double>(CellX - FirstCellX)
							* TerrainHeightField.CellSize,
					TerrainHeightField.FirstCellCenter.Y
						+ static_cast<double>(CellY - FirstCellY)
							* TerrainHeightField.CellSize,
					Height);
				const FIntPoint MaterialCell(
					FMath::FloorToInt(
						LocalLocation.X / MaterialSimulationCellSize),
					FMath::FloorToInt(
						LocalLocation.Y / MaterialSimulationCellSize));
				if (ProceduralSurfaceSeedMaterialCells.Contains(MaterialCell))
				{
					continue;
				}
				ProceduralSurfaceSeedMaterialCells.Add(MaterialCell);
				MatterFlux::Material::FSeedCell& SeedCell =
					SeedCells.AddDefaulted_GetRef();
				SeedCell.WorldCell = MaterialCell;
				SeedCell.SupportHeight = FMath::RoundToInt(Height);
				float RiverHeight = Height;
				float WaterSurface = RiverCell
					? RiverCell->WaterSurfaceZ
					: 0.0f;
				bool bContainsWater = RiverCell != nullptr;
				if (bContainsWater
					|| (TerrainHeightField.TrySampleInfiniteRiverCell(
						CellX,
						CellY,
						RiverHeight,
						WaterSurface,
						bContainsWater)
						&& bContainsWater))
				{
					SeedCell.MaterialId = TEXT("water");
					SeedCell.Amount = static_cast<uint8>(FMath::Clamp(
						FMath::RoundToInt(
							(WaterSurface - Height)
								/ MaterialLiquidColumnHeight * 255.0f),
						1,
						255));
				}
				if (!ProceduralSurfaceSeedFirstCell.IsSet())
				{
					ProceduralSurfaceSeedFirstCell = SeedCell;
				}
			}
			const bool bFinalBatch =
				ProceduralSurfaceSeedNextTerrainCell >= TotalTerrainCells;
			// A non-default cell size can collapse the final terrain samples onto
			// already-seeded material cells. Re-submit one deterministic cell so
			// the material world still knows which chunk to finalize.
			if (bFinalBatch && SeedCells.IsEmpty()
				&& ProceduralSurfaceSeedFirstCell.IsSet())
			{
				SeedCells.Add(ProceduralSurfaceSeedFirstCell.GetValue());
			}
			if (!SeedCells.IsEmpty())
			{
				RegisterRecentMaterialWakeSeedCells(SeedCells);
				WakeRecentVisibleMaterialChunks();
			}
			if (!SeedCells.IsEmpty()
				&& MaterialSimulation->SeedSurface(SeedCells, bFinalBatch))
			{
				if (!bRiverPrefetchOnly)
				{
					// Near-focus river cells must reach the projection mesh once before
					// the entry gate opens. Distant river prefetches stay archived and
					// intentionally do not require an off-screen render transaction.
					for (const MatterFlux::Material::FSeedCell& SeedCell : SeedCells)
					{
						if (SeedCell.MaterialId == TEXT("water")
							&& SeedCell.Amount > 0)
						{
							PendingInitialLiquidProjectionChunks.Add(
								ToMaterialRenderChunk(
									SeedCell.WorldCell,
									MaterialSimulationChunkSize));
						}
					}
				}
				bProcessedSurfaceSeed = true;
				bPreferProceduralSurfaceSeed = false;
				if (bFinalBatch)
				{
					PrefetchedProceduralRiverSurfaceChunks.Add(ChunkToSeed);
					if (!bRiverPrefetchOnly)
					{
						SeededProceduralSurfaceChunks.Add(ChunkToSeed);
					}
					bMaterialVisualizationDirty = true;
					ResetSurfaceSeedProgress();
				}
			}
			else if (!SeedCells.IsEmpty())
			{
				ResetSurfaceSeedProgress();
			}
		}
	}
	if (FragmentSourceProxy)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(
			MatterFlux_ProceduralPopulation_ApplyProxyDelta);
		TMap<
			FIntPoint,
			TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>
			UpdatedProxyChunks;
		for (const FIntPoint Chunk : ChunksToAdd)
		{
			if (const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>*
				Sources = FragmentSourceChunks.Find(Chunk))
			{
				UpdatedProxyChunks.Add(Chunk, *Sources);
			}
		}
		FragmentSourceProxy->ApplySourceChunkDelta(
			ChunksToRemove,
			UpdatedProxyChunks);
		// Streaming a deterministic chunk back in recreates pristine source
		// definitions. Re-apply durable masks before the chunk can become visible.
		for (const TPair<
			FIntPoint,
			TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
			: UpdatedProxyChunks)
		{
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
				: Pair.Value)
			{
				if (const FFragment2DSourceStreamingState* State =
					StreamedFragmentSourceStates.Find(Source.SourceId))
				{
					ApplyPersistentFragmentSourceStateToProxy(
						Source.SourceId,
						*State);
				}
			}
		}
	}
	if (bOutDidChunkCommit)
	{
		*bOutDidChunkCommit = !ChunksToAdd.IsEmpty()
			|| !ChunksToRemove.IsEmpty();
	}
	return bProcessedSurfaceSeed;
}

void AMatterFluxPlayableWorldActor::RefreshVisibleFragmentSources(
	const bool bImmediate)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxDecorationStreaming);
	TArray<FIntPoint> FocusChunks;
	GatherStreamingFocusChunks(FocusChunks);
	if (!bImmediate && FocusChunks == VisibleFragmentFocusChunks)
	{
		return;
	}
	MatterFlux::WorldStreaming::FChunkWindowRequest WindowRequest;
	WindowRequest.FocusChunks = FocusChunks;
	WindowRequest.WindowOffsets = {
		FIntPoint::ZeroValue,
		FIntPoint(2, -2)
	};
	WindowRequest.Radius = TerrainStreamingChunkRadius;
	WindowRequest.MaximumChunkCount = MaxStreamingWindowChunks;
	TArray<FIntPoint> OrderedDesiredChunks;
	FString WindowError;
	if (!MatterFlux::WorldStreaming::BuildChunkWindow(
		WindowRequest,
		OrderedDesiredChunks,
		WindowError))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Cannot plan fragment-source streaming window: %s"),
			*WindowError);
		return;
	}
	// Population is generated one ring outside the render window. Queue the
	// visible window first, then the off-screen ring, so normal movement reveals
	// completed trees/cover instead of starting their build at the camera edge.
	MatterFlux::WorldStreaming::FChunkWindowRequest PopulationRequest =
		WindowRequest;
	PopulationRequest.Radius = TerrainStreamingChunkRadius + 1;
	TArray<FIntPoint> OrderedPopulationWindow;
	FString PopulationWindowError;
	if (!MatterFlux::WorldStreaming::BuildChunkWindow(
		PopulationRequest,
		OrderedPopulationWindow,
		PopulationWindowError))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Cannot plan procedural-population prefetch window: %s"),
			*PopulationWindowError);
		return;
	}
	TArray<FIntPoint> OrderedPopulationChunks;
	OrderedPopulationChunks.Reserve(OrderedPopulationWindow.Num());
	TSet<FIntPoint> PopulationChunksAdded;
	PopulationChunksAdded.Reserve(OrderedPopulationWindow.Num());
	const auto AppendPopulationChunk = [
		&OrderedPopulationChunks,
		&PopulationChunksAdded](const FIntPoint Chunk)
	{
		if (!PopulationChunksAdded.Contains(Chunk))
		{
			PopulationChunksAdded.Add(Chunk);
			OrderedPopulationChunks.Add(Chunk);
		}
	};
	for (const FIntPoint Chunk : OrderedDesiredChunks)
	{
		AppendPopulationChunk(Chunk);
	}
	for (const FIntPoint Chunk : OrderedPopulationWindow)
	{
		AppendPopulationChunk(Chunk);
	}
	RefreshProceduralPopulation(OrderedPopulationChunks, FocusChunks);
	VisibleFragmentFocusChunks = FocusChunks;
	PendingFragmentSourceSpawns.Reset();
	TSet<FIntPoint> DesiredChunks;
	DesiredChunks.Reserve(OrderedDesiredChunks.Num());
	for (const FIntPoint Chunk : OrderedDesiredChunks)
	{
		DesiredChunks.Add(Chunk);
	}
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->SetVisibleChunks(DesiredChunks);
	}
	if (!HasAuthority())
	{
		return;
	}
	const auto IsSourceDesired =
		[this, &DesiredChunks](const FGuid& SourceId)
		{
			const FIntPoint* SourceChunk =
				FragmentSourceChunkById.Find(SourceId);
			return SourceChunk && DesiredChunks.Contains(*SourceChunk);
		};

	TArray<FGuid> SourcesToRemove;
	for (const TPair<
		FGuid,
		TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		AFragment2DSourceActor* SourceActor = Pair.Value;
		if (!IsValid(SourceActor))
		{
			PendingFragmentSourceDespawns.Remove(Pair.Key);
			StreamingPinnedFragmentSourceIds.Remove(Pair.Key);
			RemovedFragmentSourceIds.Add(Pair.Key);
			SourcesToRemove.Add(Pair.Key);
			continue;
		}
		if (!IsSourceDesired(Pair.Key)
			&& !StreamingPinnedFragmentSourceIds.Contains(Pair.Key)
			&& !SourceActor->bDetachedFromTerrain)
		{
			FFragment2DSourceStreamingState State;
			FString Error;
			if (!SourceActor->CaptureStreamingState(
				State,
				Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Cannot archive streamed fragment source %s: %s"),
					*Pair.Key.ToString(),
					*Error);
				continue;
			}
			if (State.HasPersistentChanges())
			{
				if (!ArchiveFragmentSourceState(Pair.Key, State))
				{
					continue;
				}
				SourceActor->Destroy();
				SourcesToRemove.Add(Pair.Key);
			}
			else
			{
				// Spawning was already budgeted, but destroying every pristine
				// actor leaving the window in one boundary frame caused the
				// corresponding teardown work to hitch. Queue disposable
				// sources and share the per-frame streaming budget below.
				PendingFragmentSourceDespawns.Add(Pair.Key);
			}
		}
	}
	for (const FGuid& SourceId : SourcesToRemove)
	{
		PendingFragmentSourceDespawns.Remove(SourceId);
		GeneratedFragmentSources.Remove(SourceId);
	}
	for (auto It = PendingFragmentSourceDespawns.CreateIterator(); It; ++It)
	{
		if (IsSourceDesired(*It)
			|| StreamingPinnedFragmentSourceIds.Contains(*It)
			|| !GeneratedFragmentSources.Contains(*It))
		{
			It.RemoveCurrent();
		}
	}

	// Mutable logical sources remain in the world store regardless of render
	// residency. The chunk proxy already reads RuntimeMask and output overlays;
	// only a cut or another interaction that requires independent physics may
	// materialize an Actor.
}

void AMatterFluxPlayableWorldActor::
	ProcessPendingFragmentSourceSpawns()
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxDecorationStreaming);
	if (!HasAuthority()
		|| (PendingFragmentSourceSpawns.IsEmpty()
			&& PendingFragmentSourceDespawns.IsEmpty()))
	{
		return;
	}
	int32 RemainingOperations =
		FMath::Max(MaxDecorationSpawnsPerFrame, 1);
	const double SpawnDeadlineSeconds =
		FPlatformTime::Seconds()
		+ FMath::Max(
			static_cast<double>(
				DecorationSpawnBudgetMilliseconds),
			0.25)
			/ 1000.0;
	for (auto It = PendingFragmentSourceDespawns.CreateIterator();
		It && RemainingOperations > 0;
		++It)
	{
		const FGuid SourceId = *It;
		bool bCompleted = false;
		AFragment2DSourceActor* SourceActor =
			GeneratedFragmentSources.FindRef(SourceId);
		if (!IsValid(SourceActor))
		{
			StreamingPinnedFragmentSourceIds.Remove(SourceId);
			RemovedFragmentSourceIds.Add(SourceId);
			GeneratedFragmentSources.Remove(SourceId);
			bCompleted = true;
		}
		else if (SourceActor->bDetachedFromTerrain
			|| StreamingPinnedFragmentSourceIds.Contains(SourceId))
		{
			bCompleted = true;
		}
		else
		{
			FFragment2DSourceStreamingState State;
			FString Error;
			if (!SourceActor->CaptureStreamingState(
				State,
				Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Cannot archive streamed fragment source %s: %s"),
					*SourceId.ToString(),
					*Error);
			}
			else if (ArchiveFragmentSourceState(SourceId, State))
			{
				SourceActor->Destroy();
				GeneratedFragmentSources.Remove(SourceId);
				bCompleted = true;
			}
		}
		if (bCompleted)
		{
			It.RemoveCurrent();
		}
		--RemainingOperations;
		if (FPlatformTime::Seconds() >= SpawnDeadlineSeconds)
		{
			return;
		}
	}

	while (RemainingOperations > 0
		&& !PendingFragmentSourceSpawns.IsEmpty())
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource Source =
			PendingFragmentSourceSpawns.Pop(
				EAllowShrinking::No);
		if (!RemovedFragmentSourceIds.Contains(Source.SourceId)
			&& !GeneratedFragmentSources.Contains(Source.SourceId))
		{
			SpawnFragmentSource(Source);
		}
		--RemainingOperations;
		if (FPlatformTime::Seconds() >= SpawnDeadlineSeconds)
		{
			break;
		}
	}
}

void AMatterFluxPlayableWorldActor::SpawnFragmentSource(
	const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return;
	}
	const FFragment2DSourceStreamingState* ArchivedState =
		StreamedFragmentSourceStates.Find(Source.SourceId);
	if (ArchivedState
		&& !ArchivedState->GetRuntimeMask().Contains(1)
		&& (!ArchivedState->bHasReactionState
			|| !ArchivedState->ReactionState.OutputMask.Contains(1)))
	{
		// A fully consumed logical source is a durable tombstone, not an
		// invisible Actor. In particular, aggregate materialization must not
		// resurrect an already-cut canopy merely to hand it to a carrier.
		return;
	}
	const FTransform WorldTransform =
		Source.Transform * GetActorTransform();
	AFragment2DSourceActor* SourceActor =
		World->SpawnActorDeferred<AFragment2DSourceActor>(
			AFragment2DSourceActor::StaticClass(),
			WorldTransform,
			this,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!SourceActor)
	{
		return;
	}

	SourceActor->FragmentMaterial = Source.MaterialId == TEXT("leaf")
		? VoxelLeafMaterialTemplate.Get()
		: (Source.MaterialId == TEXT("wood")
			? VoxelWoodMaterialTemplate.Get()
			: VoxelColorMaterialTemplate.Get());
	// Lazy render-only sources keep a hidden replicated actor after a complete
	// break. That actor is the durable tombstone preventing a client-side
	// pristine proxy from reappearing when the source is destroyed.
	SourceActor->bDestroySourceOnFirstBreak = Source.bEnableCollision;
	SourceActor->ConfigureAggregate(
		Source.AggregateId,
		Source.bAggregateRoot);
	SourceActor->SetSourceCollisionEnabled(Source.bEnableCollision);
	SourceActor->MeshComponent->SetCastShadow(
		Source.MaterialId == TEXT("wood")
		|| Source.MaterialId == TEXT("stone"));
	if (!SourceActor->InitializeFromProceduralMask(
		Source.Mask,
		Source.SourceId,
		Source.Color,
		Source.MaterialId))
	{
		SourceActor->Destroy();
		return;
	}
	SourceActor->Tags.AddUnique(TEXT("MatterFluxGeneratedDecoration"));
#if WITH_EDITOR
	SourceActor->SetActorLabel(FString::Printf(
		TEXT("%s_%s"),
		*Source.Name.ToString(),
		*Source.SourceId.ToString(EGuidFormats::Short)));
#endif
	SourceActor->FinishSpawning(WorldTransform);
	if (ArchivedState)
	{
		FString Error;
		if (!SourceActor->RestoreStreamingState(
			*ArchivedState,
			Error))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Cannot restore streamed fragment source %s: %s"),
				*Source.SourceId.ToString(),
				*Error);
			SourceActor->Destroy();
			return;
		}
	}
	GeneratedFragmentSources.Add(Source.SourceId, SourceActor);
	ActiveSourceReactions.Remove(Source.SourceId);
	LogicalSourceReactionIndex.Remove(Source.SourceId);
	bSourceReactionVisualDirty = true;
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->SetSourceMaterialized(Source.SourceId, true);
	}
}

void AMatterFluxPlayableWorldActor::DestroyGeneratedFragmentSources()
{
	if (AsyncPopulationBuildState.IsValid())
	{
		AsyncPopulationBuildState->bCancelled.store(
			true, std::memory_order_release);
	}
	AsyncPopulationBuildState.Reset();
	ProceduralPopulationChunksBuilding.Reset();
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	GeneratedFragmentSources.Reset();
	if (FragmentSourceProxy)
	{
		// The proxy owns disposable merged meshes independently of the logical
		// source maps. Reset it explicitly when switching to an authored map so
		// free-mode trees and flowers cannot survive outside story bounds.
		FragmentSourceProxy->ResetSources();
	}
	ActiveSourceReactions.Reset();
	LogicalSourceReactionIndex.Reset();
	FragmentSourceChunks.Reset();
	ProceduralPopulationChunks.Reset();
	ProceduralSurfaceSeedPriorityChunks.Reset();
	ProceduralRiverChunks.Reset();
	ProceduralRiverCellsByChunk.Reset();
	PrefetchedProceduralRiverSurfaceChunks.Reset();
	SeededProceduralSurfaceChunks.Reset();
	ProceduralSurfaceSeedChunkInProgress.Reset();
	bProceduralSurfaceSeedIsRiverPrefetch = false;
	ProceduralSurfaceSeedNextTerrainCell = 0;
	ProceduralSurfaceSeedMaterialCells.Reset();
	ProceduralSurfaceSeedFirstCell.Reset();
	FragmentSourceDefinitionIndex.Reset();
	FragmentSourceChunkById.Reset();
	StreamedFragmentSourceStates.Reset();
	VisibleFragmentFocusChunks.Reset();
	RemovedFragmentSourceIds.Reset();
	PendingFragmentSourceSpawns.Reset();
	PendingFragmentSourceDespawns.Reset();
	StreamingPinnedFragmentSourceIds.Reset();
	DynamicAggregateCarriers.Reset();
	if (SourceFlameInstances)
	{
		SourceFlameInstances->ClearInstances();
	}
	if (SourceSmokeInstances)
	{
		SourceSmokeInstances->ClearInstances();
	}
	SourceSmokeAnchors.Reset();
	GroundSmokeAnchors.Reset();
	SmokeVisualPool.Reset();
	bSourceReactionVisualDirty = false;
	SourceReactionVisualAccumulator = 0.0f;
}

void AMatterFluxPlayableWorldActor::BuildLayerStreamingCache(
	const MatterFlux::PlayableLevel::FLevelLayout& Layout)
{
	LayerStreamingCaches.Reset();
	LiquidLayerDefinitions.Reset();
	VisibleLayerFocusChunks.Reset();
	BuildTerrainStreamingCache(Layout.Terrain);
	TSet<FName> ActiveLayerNames;
	TSet<FName> ActiveLiquidMaterialIds;
	const auto ToChunk =
		[this](const FVector& LocalLocation)
		{
			const FIntPoint Cell(
				FMath::RoundToInt(
					LocalLocation.X
						/ MatterFlux::PlayableLevel::TerrainCellSize),
				FMath::RoundToInt(
					LocalLocation.Y
						/ MatterFlux::PlayableLevel::TerrainCellSize));
			return FIntPoint(
				FMath::FloorToInt(
					static_cast<double>(Cell.X)
						/ TerrainStreamingChunkSize),
				FMath::FloorToInt(
					static_cast<double>(Cell.Y)
						/ TerrainStreamingChunkSize));
		};

	for (const MatterFlux::PlayableLevel::FLevelLayer& Layer
		: Layout.Layers)
	{
		if (Layer.Name == TEXT("Land")
			|| Layer.Name == TEXT("Lowlands")
			|| Layer.Name == TEXT("Midlands")
			|| Layer.Name == TEXT("Highlands"))
		{
			continue;
		}
		if (Layer.RenderMode
			== MatterFlux::PlayableLevel::ELevelLayerRenderMode::Liquid)
		{
			ActiveLiquidMaterialIds.Add(Layer.MaterialId);
			LiquidLayerDefinitions.Add(Layer.Name, Layer);
			continue;
		}
		ActiveLayerNames.Add(Layer.Name);
		FLayerStreamingCache& Cache =
			LayerStreamingCaches.FindOrAdd(Layer.Name);
		Cache.Layer.Name = Layer.Name;
		Cache.Layer.Primitive = Layer.Primitive;
		Cache.Layer.RenderMode = Layer.RenderMode;
		Cache.Layer.Color = Layer.Color;
		Cache.Layer.MaterialId = Layer.MaterialId;
		Cache.Layer.bEnableCollision = Layer.bEnableCollision;
		const bool bSmallRenderOnlyLayer =
			!Layer.bEnableCollision
			&& Layer.Instances.Num()
				<= MaxAlwaysLoadedRenderOnlyLayerInstances;
		if (Layer.Instances.Num() <= 4 || bSmallRenderOnlyLayer)
		{
			// HISM already culls render-only instances through its spatial
			// hierarchy. Keeping modest layers resident avoids clearing and
			// rebuilding the full instance tree at every chunk boundary.
			Cache.AlwaysLoadedInstances = Layer.Instances;
		}
		else
		{
			for (const FTransform& Transform : Layer.Instances)
			{
				Cache.ChunkInstances
					.FindOrAdd(ToChunk(Transform.GetLocation()))
					.Add(Transform);
			}
		}
		FindOrCreateLayerComponent(Cache.Layer);
	}

	TArray<FName> StaleLayerNames;
	for (const TPair<
		FName,
		TObjectPtr<UHierarchicalInstancedStaticMeshComponent>>& Pair
		: GeneratedLayerInstances)
	{
		if (!ActiveLayerNames.Contains(Pair.Key))
		{
			StaleLayerNames.Add(Pair.Key);
		}
	}
	for (const FName StaleLayerName : StaleLayerNames)
	{
		if (UHierarchicalInstancedStaticMeshComponent* Component =
			GeneratedLayerInstances.FindRef(StaleLayerName))
		{
			Component->DestroyComponent();
		}
		GeneratedLayerInstances.Remove(StaleLayerName);
	}

	TArray<FName> StaleLiquidLayerNames;
	for (const TPair<FName, TObjectPtr<UProceduralMeshComponent>>& Pair
		: GeneratedLiquidLayerMeshes)
	{
		if (!ActiveLiquidMaterialIds.Contains(
			LiquidProjectionMaterials.FindRef(Pair.Key)))
		{
			StaleLiquidLayerNames.Add(Pair.Key);
		}
	}
	for (const FName StaleLayerName : StaleLiquidLayerNames)
	{
		if (UProceduralMeshComponent* Component =
			GeneratedLiquidLayerMeshes.FindRef(StaleLayerName))
		{
			Component->DestroyComponent();
		}
		GeneratedLiquidLayerMeshes.Remove(StaleLayerName);
		LiquidProjectionMaterials.Remove(StaleLayerName);
		LiquidProjectionChunks.Remove(StaleLayerName);
		LiquidChunkProjectionHeightAudits.Remove(StaleLayerName);
	}
	if (!TerrainHeightField.RuntimeHeightOverrides.IsEmpty())
	{
		TArray<FIntPoint> EditedCells;
		TerrainHeightField.RuntimeHeightOverrides.GenerateKeyArray(EditedCells);
		RefreshTerrainBackdropForCells(EditedCells);
	}
}

void AMatterFluxPlayableWorldActor::GatherMaterialSimulationFocusCells(
	TArray<FIntPoint>& OutFocusCells) const
{
	OutFocusCells.Reset();
	TSet<FIntPoint> UniqueChunkOrigins;
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<APlayerController> It(World); It; ++It)
		{
			const APlayerController* Controller = *It;
			if (!Controller
				|| (!HasAuthority()
					&& !Controller->IsLocalController()))
			{
				continue;
			}
			const APawn* Pawn = Controller->GetPawn();
			if (!IsValid(Pawn))
			{
				continue;
			}
			FIntPoint Cell;
			if (!TryWorldLocationToCell(
				GetActorTransform(),
				Pawn->GetActorLocation(),
				MaterialSimulationCellSize,
				Cell))
			{
				continue;
			}
			const FIntPoint Chunk(
				FMath::FloorToInt(
					static_cast<double>(Cell.X)
						/ MaterialSimulationChunkSize),
				FMath::FloorToInt(
					static_cast<double>(Cell.Y)
						/ MaterialSimulationChunkSize));
			UniqueChunkOrigins.Add(FIntPoint(
				Chunk.X * MaterialSimulationChunkSize,
				Chunk.Y * MaterialSimulationChunkSize));
		}
	}
	for (const TPair<FIntPoint, int32>& Pair : PlayerSpawnRegionFocusCounts)
	{
		if (Pair.Value <= 0)
		{
			continue;
		}
		const FIntPoint TerrainCellOrigin =
			Pair.Key * TerrainStreamingChunkSize;
		const FIntPoint MaterialChunk(
			FMath::FloorToInt(
				static_cast<double>(TerrainCellOrigin.X)
					/ MaterialSimulationChunkSize),
			FMath::FloorToInt(
				static_cast<double>(TerrainCellOrigin.Y)
					/ MaterialSimulationChunkSize));
		UniqueChunkOrigins.Add(
			MaterialChunk * MaterialSimulationChunkSize);
	}
	OutFocusCells = UniqueChunkOrigins.Array();
	OutFocusCells.Sort([](const FIntPoint A, const FIntPoint B)
	{
		return A.X != B.X ? A.X < B.X : A.Y < B.Y;
	});

	const int32 ActiveDiameter =
		MaterialSimulationActiveChunkRadius * 2 + 1;
	const int32 FocusBudget = ActiveDiameter * ActiveDiameter;
	if (OutFocusCells.Num() > FocusBudget)
	{
		OutFocusCells.SetNum(FocusBudget);
	}
	if (OutFocusCells.IsEmpty())
	{
		const FIntPoint FallbackChunk(
			FMath::FloorToInt(
				static_cast<double>(
					ReplicatedMaterialSimulationFocus.X)
					/ MaterialSimulationChunkSize),
			FMath::FloorToInt(
				static_cast<double>(
					ReplicatedMaterialSimulationFocus.Y)
					/ MaterialSimulationChunkSize));
		OutFocusCells.Add(FIntPoint(
			FallbackChunk.X * MaterialSimulationChunkSize,
			FallbackChunk.Y * MaterialSimulationChunkSize));
	}
}

void AMatterFluxPlayableWorldActor::RegisterRecentMaterialWakeCells(
	const TConstArrayView<FIntPoint> WorldCells)
{
	const UWorld* World = GetWorld();
	if (!HasAuthority()
		|| !MaterialSimulation
		|| !World
		|| MaterialSimulationChunkSize <= 0
		|| MaterialRecentViewWakeSeconds <= 0.0f)
	{
		return;
	}

	const double Expiration = World->GetTimeSeconds()
		+ static_cast<double>(MaterialRecentViewWakeSeconds);
	for (const FIntPoint WorldCell : WorldCells)
	{
		const FIntPoint MaterialChunk(
			FMath::FloorToInt(
				static_cast<double>(WorldCell.X)
					/ MaterialSimulationChunkSize),
			FMath::FloorToInt(
				static_cast<double>(WorldCell.Y)
					/ MaterialSimulationChunkSize));
		FRecentMaterialWake& Wake =
			RecentMaterialChunkWakes.FindOrAdd(MaterialChunk);
		Wake.SampleCell = WorldCell;
		Wake.ExpiresAtWorldSeconds = FMath::Max(
			Wake.ExpiresAtWorldSeconds,
			Expiration);
	}
}

void AMatterFluxPlayableWorldActor::RegisterRecentMaterialWakeSeedCells(
	const TConstArrayView<MatterFlux::Material::FSeedCell> SeedCells)
{
	TArray<FIntPoint> MaterialCells;
	MaterialCells.Reserve(SeedCells.Num());
	for (const MatterFlux::Material::FSeedCell& SeedCell : SeedCells)
	{
		if (!SeedCell.MaterialId.IsNone() && SeedCell.Amount > 0)
		{
			MaterialCells.Add(SeedCell.WorldCell);
		}
	}
	RegisterRecentMaterialWakeCells(MaterialCells);
}

bool AMatterFluxPlayableWorldActor::IsMaterialCellInsideTerrainView(
	const FIntPoint& WorldCell) const
{
	if (TerrainStreamingChunkSize <= 0 || DesiredTerrainChunks.IsEmpty())
	{
		return false;
	}
	// Material and terrain cells share the playable world's horizontal lattice.
	// Keep this conversion identical to GatherStreamingFocusChunks so negative
	// coordinates select the same deterministic streaming chunk.
	const FIntPoint TerrainChunk(
		FMath::FloorToInt(
			static_cast<double>(WorldCell.X)
				/ TerrainStreamingChunkSize),
		FMath::FloorToInt(
			static_cast<double>(WorldCell.Y)
				/ TerrainStreamingChunkSize));
	return DesiredTerrainChunks.Contains(TerrainChunk);
}

void AMatterFluxPlayableWorldActor::WakeRecentVisibleMaterialChunks()
{
	const UWorld* World = GetWorld();
	if (!HasAuthority()
		|| !MaterialSimulation
		|| !World
		|| RecentMaterialChunkWakes.IsEmpty())
	{
		return;
	}

	TArray<FIntPoint> OrderedChunks;
	RecentMaterialChunkWakes.GenerateKeyArray(OrderedChunks);
	OrderedChunks.Sort([](const FIntPoint A, const FIntPoint B)
	{
		return A.X != B.X ? A.X < B.X : A.Y < B.Y;
	});
	const double Now = World->GetTimeSeconds();
	TArray<FIntPoint> CellsToWake;
	CellsToWake.Reserve(OrderedChunks.Num());
	for (const FIntPoint Chunk : OrderedChunks)
	{
		const FRecentMaterialWake* Wake =
			RecentMaterialChunkWakes.Find(Chunk);
		if (!Wake)
		{
			continue;
		}
		if (Wake->ExpiresAtWorldSeconds <= Now)
		{
			RecentMaterialChunkWakes.Remove(Chunk);
			continue;
		}
		if (!IsMaterialCellInsideTerrainView(Wake->SampleCell))
		{
			continue;
		}
		CellsToWake.Add(Wake->SampleCell);
		RecentMaterialChunkWakes.Remove(Chunk);
	}
	MaterialSimulation->WakeSurfaceCells(CellsToWake);
}

void AMatterFluxPlayableWorldActor::GatherStreamingFocusChunks(
	TArray<FIntPoint>& OutFocusChunks) const
{
	OutFocusChunks.Reset();
	TSet<FIntPoint> UniqueChunks;
	for (const TPair<FIntPoint, int32>& Pair : PlayerSpawnRegionFocusCounts)
	{
		if (Pair.Value > 0)
		{
			UniqueChunks.Add(Pair.Key);
		}
	}
	const UWorld* World = GetWorld();
	if (World)
	{
		for (TActorIterator<APlayerController> It(World); It; ++It)
		{
			const APlayerController* Controller = *It;
			if (!Controller
				|| (!HasAuthority() && !Controller->IsLocalController()))
			{
				continue;
			}
			const APawn* Pawn = Controller->GetPawn();
			if (!IsValid(Pawn))
			{
				continue;
			}
			FIntPoint Cell;
			if (!TryWorldLocationToCell(
				GetActorTransform(),
				Pawn->GetActorLocation(),
				MaterialSimulationCellSize,
				Cell))
			{
				continue;
			}
			UniqueChunks.Add(FIntPoint(
				FMath::FloorToInt(
					static_cast<double>(Cell.X)
						/ TerrainStreamingChunkSize),
				FMath::FloorToInt(
					static_cast<double>(Cell.Y)
						/ TerrainStreamingChunkSize)));
		}
	}
	OutFocusChunks = UniqueChunks.Array();

	if (OutFocusChunks.IsEmpty())
	{
		OutFocusChunks.Add(FIntPoint(
			FMath::FloorToInt(
				static_cast<double>(
					ReplicatedMaterialSimulationFocus.X)
					/ TerrainStreamingChunkSize),
			FMath::FloorToInt(
				static_cast<double>(
					ReplicatedMaterialSimulationFocus.Y)
					/ TerrainStreamingChunkSize)));
	}
	OutFocusChunks.Sort(
		[](const FIntPoint A, const FIntPoint B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});
}

void AMatterFluxPlayableWorldActor::RefreshVisibleLevelLayers(
	const bool bForce)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxTerrainStreaming);
	TArray<FIntPoint> FocusChunks;
	GatherStreamingFocusChunks(FocusChunks);
	if (!bForce && FocusChunks == VisibleLayerFocusChunks)
	{
		return;
	}
	if (!RefreshVisibleTerrainChunks(bForce, FocusChunks))
	{
		return;
	}
	VisibleLayerFocusChunks = FocusChunks;

	for (TPair<FName, FLayerStreamingCache>& Pair
		: LayerStreamingCaches)
	{
		FLayerStreamingCache& Cache = Pair.Value;
		UHierarchicalInstancedStaticMeshComponent* Instances =
			GeneratedLayerInstances.FindRef(Pair.Key);
		if (!Instances)
		{
			continue;
		}
		if (!bForce && Cache.ChunkInstances.IsEmpty())
		{
			// Always-loaded layers were populated during the forced rebuild
			// and do not depend on the streaming focus. Rebuilding their HISM
			// tree here defeats the reason they were kept resident.
			continue;
		}
		if (GetNetMode() == NM_DedicatedServer
			&& !Cache.Layer.bEnableCollision)
		{
			Instances->ClearInstances();
			continue;
		}

		TArray<FTransform> VisibleTransforms =
			Cache.AlwaysLoadedInstances;
		const int32 Radius =
			Cache.Layer.RenderMode
				== MatterFlux::PlayableLevel::
					ELevelLayerRenderMode::CollisionOnly
				? 0
				: TerrainStreamingChunkRadius;
		MatterFlux::WorldStreaming::FChunkWindowRequest WindowRequest;
		WindowRequest.FocusChunks = FocusChunks;
		if (Radius > 0)
		{
			WindowRequest.WindowOffsets = {
				FIntPoint::ZeroValue,
				FIntPoint(2, -2)
			};
		}
		WindowRequest.Radius = Radius;
		WindowRequest.MaximumChunkCount = MaxStreamingWindowChunks;
		TArray<FIntPoint> OrderedLayerChunks;
		FString WindowError;
		if (!MatterFlux::WorldStreaming::BuildChunkWindow(
			WindowRequest,
			OrderedLayerChunks,
			WindowError))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Cannot plan level-layer streaming window: %s"),
				*WindowError);
			continue;
		}
		for (const FIntPoint Chunk : OrderedLayerChunks)
		{
			if (const TArray<FTransform>* ChunkTransforms =
				Cache.ChunkInstances.Find(Chunk))
			{
				VisibleTransforms.Append(*ChunkTransforms);
			}
		}

		MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
			*Instances,
			VisibleTransforms);
	}
}

void AMatterFluxPlayableWorldActor::BuildTerrainStreamingCache(
	const MatterFlux::PlayableLevel::FLevelTerrain& Terrain)
{
	DestroyTerrainChunkMeshes();
	TerrainHeightField = Terrain;
	ApplyTerrainHeightOverrides(
		ReplicatedTerrainHeightOverrides,
		false);
}

void AMatterFluxPlayableWorldActor::ApplyTerrainHeightOverrides(
	const TConstArrayView<FMatterFluxTerrainHeightOverride> Overrides,
	const bool bRebuildResidentChunks)
{
	if (!TerrainHeightField.IsValid())
	{
		return;
	}
	TArray<FIntPoint> ChangedCells;
	TerrainHeightField.RuntimeHeightOverrides.GenerateKeyArray(ChangedCells);
	TerrainHeightField.RuntimeHeightOverrides.Reset();
	for (const FMatterFluxTerrainHeightOverride& Override : Overrides)
	{
		if (!FMath::IsFinite(Override.Height)
			|| Override.Height < TerrainHeightField.BottomZ)
		{
			continue;
		}
		TerrainHeightField.RuntimeHeightOverrides.Add(
			Override.WorldCell,
			Override.Height);
		ChangedCells.AddUnique(Override.WorldCell);
	}

	const FIntPoint FirstWorldCell(
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.X
				/ TerrainHeightField.CellSize),
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.Y
				/ TerrainHeightField.CellSize));
	for (const FIntPoint WorldCell : ChangedCells)
	{
		const int32 LocalX = WorldCell.X - FirstWorldCell.X;
		const int32 LocalY = WorldCell.Y - FirstWorldCell.Y;
		if (LocalX < 0 || LocalY < 0
			|| LocalX >= TerrainHeightField.Width
			|| LocalY >= TerrainHeightField.Height)
		{
			continue;
		}
		const int32 Index = TerrainHeightField.ToIndex(LocalX, LocalY);
		if (!GroundSurfacePositions.IsValidIndex(Index))
		{
			continue;
		}
		float Height = 0.0f;
		uint8 ColorBand = 0;
		if (TerrainHeightField.TrySampleWorldCell(
			WorldCell.X,
			WorldCell.Y,
			Height,
			ColorBand))
		{
			GroundSurfacePositions[Index].Z = Height;
		}
	}
	if (bRebuildResidentChunks && !ChangedCells.IsEmpty())
	{
		RefreshTerrainBackdropForCells(ChangedCells);
		RebuildResidentTerrainChunksForCells(ChangedCells);
		bGroundReactionVisualDirty = true;
		bGroundReactionVisualNeedsFullRebuild = true;
		PendingGroundReactionVisualCellIndices.Reset();
		bMaterialVisualizationDirty = true;
	}
	if (AsyncTerrainBuildState.IsValid())
	{
		AsyncTerrainBuildState->bCancelled.store(
			true, std::memory_order_release);
	}
	TerrainChunksBuilding.Reset();
	AsyncTerrainHeightField = MakeShared<
		const MatterFlux::PlayableLevel::FLevelTerrain,
		ESPMode::ThreadSafe>(TerrainHeightField);
	AsyncTerrainBuildState = MakeShared<
		FMatterFluxAsyncTerrainBuildState,
		ESPMode::ThreadSafe>();
}

void AMatterFluxPlayableWorldActor::PublishTerrainHeightOverrides()
{
	if (!HasAuthority())
	{
		return;
	}
	ReplicatedTerrainHeightOverrides.Reset(
		TerrainHeightField.RuntimeHeightOverrides.Num());
	for (const TPair<FIntPoint, float>& Pair
		: TerrainHeightField.RuntimeHeightOverrides)
	{
		FMatterFluxTerrainHeightOverride& Override =
			ReplicatedTerrainHeightOverrides.AddDefaulted_GetRef();
		Override.WorldCell = Pair.Key;
		Override.Height = Pair.Value;
	}
	ReplicatedTerrainHeightOverrides.Sort(
		[](const FMatterFluxTerrainHeightOverride& Left,
			const FMatterFluxTerrainHeightOverride& Right)
		{
			return Left.WorldCell.X != Right.WorldCell.X
				? Left.WorldCell.X < Right.WorldCell.X
				: Left.WorldCell.Y < Right.WorldCell.Y;
		});
	ForceNetUpdate();
}

void AMatterFluxPlayableWorldActor::RefreshTerrainBackdropForCells(
	const TConstArrayView<FIntPoint> WorldCells)
{
	if (WorldCells.IsEmpty() || !TerrainHeightField.IsValid())
	{
		return;
	}
	FLayerStreamingCache* Cache = LayerStreamingCaches.Find(TEXT("Backdrop"));
	if (!Cache)
	{
		return;
	}
	const FIntPoint FirstWorldCell(
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.X
				/ TerrainHeightField.CellSize),
		FMath::FloorToInt(
			TerrainHeightField.FirstCellCenter.Y
				/ TerrainHeightField.CellSize));
	TArray<FVector> EditedSurfaces;
	EditedSurfaces.Reserve(WorldCells.Num());
	FBox2D EditedBounds(EForceInit::ForceInit);
	for (const FIntPoint WorldCell : WorldCells)
	{
		float Height = 0.0f;
		uint8 ColorBand = 0;
		if (!TerrainHeightField.TrySampleWorldCell(
			WorldCell.X, WorldCell.Y, Height, ColorBand))
		{
			continue;
		}
		const FVector EditedSurface(
			TerrainHeightField.FirstCellCenter.X
				+ static_cast<double>(WorldCell.X - FirstWorldCell.X)
					* TerrainHeightField.CellSize,
			TerrainHeightField.FirstCellCenter.Y
				+ static_cast<double>(WorldCell.Y - FirstWorldCell.Y)
					* TerrainHeightField.CellSize,
			Height - TerrainHeightField.CellSize);
		EditedSurfaces.Add(EditedSurface);
		EditedBounds += FVector2D(EditedSurface);
	}
	if (EditedSurfaces.IsEmpty())
	{
		return;
	}
	const auto LowerProjection =
		[&EditedSurfaces, &EditedBounds](FTransform& Transform)
		{
			FVector Location = Transform.GetLocation();
			const FVector Scale = Transform.GetScale3D().GetAbs();
			const double HalfX = Scale.X * 50.0;
			const double HalfY = Scale.Y * 50.0;
			const double HalfZ = Scale.Z * 50.0;
			if (Location.X + HalfX < EditedBounds.Min.X
				|| Location.X - HalfX > EditedBounds.Max.X
				|| Location.Y + HalfY < EditedBounds.Min.Y
				|| Location.Y - HalfY > EditedBounds.Max.Y)
			{
				return false;
			}
			float DesiredTop = static_cast<float>(Location.Z + HalfZ);
			for (const FVector& EditedSurface : EditedSurfaces)
			{
				if (FMath::Abs(EditedSurface.X - Location.X) > HalfX
					|| FMath::Abs(EditedSurface.Y - Location.Y) > HalfY)
				{
					continue;
				}
				DesiredTop = FMath::Min(DesiredTop, EditedSurface.Z);
			}
			const float CurrentTop = static_cast<float>(Location.Z + HalfZ);
			if (DesiredTop >= CurrentTop - KINDA_SMALL_NUMBER)
			{
				return false;
			}
			Location.Z = DesiredTop - HalfZ;
			Transform.SetLocation(Location);
			return true;
		};

	for (FTransform& Transform : Cache->AlwaysLoadedInstances)
	{
		LowerProjection(Transform);
	}
	for (TPair<FIntPoint, TArray<FTransform>>& Pair : Cache->ChunkInstances)
	{
		for (FTransform& Transform : Pair.Value)
		{
			LowerProjection(Transform);
		}
	}

	UHierarchicalInstancedStaticMeshComponent* Backdrop =
		GeneratedLayerInstances.FindRef(TEXT("Backdrop"));
	if (!IsValid(Backdrop))
	{
		return;
	}
	bool bProjectionChanged = false;
	for (int32 Index = 0; Index < Backdrop->GetInstanceCount(); ++Index)
	{
		FTransform Transform;
		if (Backdrop->GetInstanceTransform(Index, Transform, false)
			&& LowerProjection(Transform))
		{
			Backdrop->UpdateInstanceTransform(
				Index, Transform, false, false, true);
			bProjectionChanged = true;
		}
	}
	if (bProjectionChanged)
	{
		Backdrop->MarkRenderStateDirty();
	}
}

void AMatterFluxPlayableWorldActor::RebuildResidentTerrainChunksForCells(
	const TConstArrayView<FIntPoint> WorldCells)
{
	if (TerrainStreamingChunkSize <= 0 || WorldCells.IsEmpty())
	{
		return;
	}
	TSet<FIntPoint> ChunksToRebuild;
	for (const FIntPoint WorldCell : WorldCells)
	{
		const FIntPoint CenterChunk(
			FMath::FloorToInt(
				static_cast<double>(WorldCell.X)
					/ TerrainStreamingChunkSize),
			FMath::FloorToInt(
				static_cast<double>(WorldCell.Y)
					/ TerrainStreamingChunkSize));
		// Terrain mesh normals and shared collision corners sample one neighbor
		// cell beyond the chunk. Rebuild the compact adjacent ring so both sides
		// of a streamed seam project the same canonical edit immediately.
		for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
		{
			for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
			{
				ChunksToRebuild.Add(
					CenterChunk + FIntPoint(OffsetX, OffsetY));
			}
		}
	}
	for (const FIntPoint ChunkCoordinate : ChunksToRebuild)
	{
		UProceduralMeshComponent* Existing =
			GeneratedTerrainChunks.FindRef(ChunkCoordinate);
		if (!IsValid(Existing))
		{
			continue;
		}
		const bool bActive = ActiveTerrainChunks.Contains(ChunkCoordinate);
		const uint64 LastUsed = TerrainChunkLastUsed.FindRef(ChunkCoordinate);
		RetireTerrainChunkComponent(Existing);
		GeneratedTerrainChunks.Remove(ChunkCoordinate);
		UProceduralMeshComponent* Rebuilt =
			CreateTerrainChunkComponent(ChunkCoordinate);
		if (!Rebuilt)
		{
			continue;
		}
		Rebuilt->SetVisibility(
			bActive && GetNetMode() != NM_DedicatedServer,
			true);
		Rebuilt->SetHiddenInGame(
			!bActive || GetNetMode() == NM_DedicatedServer,
			true);
		Rebuilt->SetCollisionEnabled(
			bActive || bTerrainCacheCoversWholeMap
				? ECollisionEnabled::QueryAndPhysics
				: ECollisionEnabled::NoCollision);
		TerrainChunkLastUsed.FindOrAdd(ChunkCoordinate) = LastUsed;
	}
}

bool AMatterFluxPlayableWorldActor::RefreshVisibleTerrainChunks(
	const bool bForce,
	const TArray<FIntPoint>& FocusChunks)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_Terrain_RefreshVisibleChunks);
	if (!TerrainHeightField.IsValid())
	{
		DestroyTerrainChunkMeshes();
		return true;
	}

	MatterFlux::WorldStreaming::FChunkWindowRequest WindowRequest;
	WindowRequest.FocusChunks = FocusChunks;
	WindowRequest.WindowOffsets = {
		FIntPoint::ZeroValue,
		FIntPoint(2, -2)
	};
	WindowRequest.Radius = TerrainStreamingChunkRadius;
	WindowRequest.MaximumChunkCount = MaxStreamingWindowChunks;
	TArray<FIntPoint> OrderedDesiredChunks;
	FString WindowError;
	if (!MatterFlux::WorldStreaming::BuildChunkWindow(
		WindowRequest,
		OrderedDesiredChunks,
		WindowError))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Cannot plan terrain streaming window: %s"),
			*WindowError);
		return false;
	}
	// The fixed isometric camera sees two chunks farther toward +X/-Y at
	// maximum zoom. The expanded radius keeps the visible frustum inside an
	// already-resident ring instead of exposing chunk creation at its edge. The
	// planner unions that render window with every player collision window and
	// returns a stable order for component creation.
	TSet<FIntPoint> DesiredChunks;
	DesiredChunks.Reserve(OrderedDesiredChunks.Num());
	for (const FIntPoint Chunk : OrderedDesiredChunks)
	{
		DesiredChunks.Add(Chunk);
	}
	DesiredTerrainChunks = DesiredChunks;

	// Legacy finite heightfields may fit entirely inside the hot cache. Seeded
	// infinite terrain deliberately skips that path and keeps only the bounded
	// streaming window plus LRU history resident.
	if (bForce && !TerrainHeightField.bInfinite)
	{
		bTerrainCacheCoversWholeMap = false;
		const FIntPoint FirstWorldCell(
			FMath::FloorToInt(
				TerrainHeightField.FirstCellCenter.X
					/ TerrainHeightField.CellSize),
			FMath::FloorToInt(
				TerrainHeightField.FirstCellCenter.Y
					/ TerrainHeightField.CellSize));
		const FIntPoint LastWorldCell(
			FirstWorldCell.X + TerrainHeightField.Width - 1,
			FirstWorldCell.Y + TerrainHeightField.Height - 1);
		const FIntPoint FirstChunk(
			FMath::FloorToInt(
				static_cast<double>(FirstWorldCell.X)
					/ TerrainStreamingChunkSize),
			FMath::FloorToInt(
				static_cast<double>(FirstWorldCell.Y)
					/ TerrainStreamingChunkSize));
		const FIntPoint LastChunk(
			FMath::FloorToInt(
				static_cast<double>(LastWorldCell.X)
					/ TerrainStreamingChunkSize),
			FMath::FloorToInt(
				static_cast<double>(LastWorldCell.Y)
					/ TerrainStreamingChunkSize));
		const int64 TotalTerrainChunkCount =
			static_cast<int64>(LastChunk.X - FirstChunk.X + 1)
			* static_cast<int64>(LastChunk.Y - FirstChunk.Y + 1);
		if (TotalTerrainChunkCount > 0
			&& TotalTerrainChunkCount <= TerrainChunkCacheLimit)
		{
			bTerrainCacheCoversWholeMap = true;
			for (int32 ChunkY = FirstChunk.Y;
				ChunkY <= LastChunk.Y;
				++ChunkY)
			{
				for (int32 ChunkX = FirstChunk.X;
					ChunkX <= LastChunk.X;
					++ChunkX)
				{
					const FIntPoint Coordinate(ChunkX, ChunkY);
					if (!GeneratedTerrainChunks.Contains(Coordinate))
					{
						CreateTerrainChunkComponent(Coordinate);
					}
				}
			}
		}
	}

	const TSet<FIntPoint> PreviouslyActiveChunks = ActiveTerrainChunks;
	ActiveTerrainChunks.Reset();
	const uint64 CurrentUseGeneration = ++TerrainChunkUseCounter;
	for (const TPair<FIntPoint, TObjectPtr<UProceduralMeshComponent>>& Pair
		: GeneratedTerrainChunks)
	{
		const bool bActive = DesiredChunks.Contains(Pair.Key);
		const bool bWasActive =
			PreviouslyActiveChunks.Contains(Pair.Key);
		if (IsValid(Pair.Value)
			&& (bForce || bActive != bWasActive))
		{
			// Visibility changes are cheap enough to follow the render window.
			// When the whole map fits in the hot cache, collision was already
			// cooked during loading and stays enabled to avoid recreating
			// physics state at every chunk boundary.
			Pair.Value->SetVisibility(
				bActive && GetNetMode() != NM_DedicatedServer,
				true);
			Pair.Value->SetHiddenInGame(
				!bActive || GetNetMode() == NM_DedicatedServer,
				true);
			if (!bTerrainCacheCoversWholeMap)
			{
				Pair.Value->SetCollisionEnabled(
					bActive
						? ECollisionEnabled::QueryAndPhysics
						: ECollisionEnabled::NoCollision);
			}
		}
		if (bActive)
		{
			ActiveTerrainChunks.Add(Pair.Key);
			TerrainChunkLastUsed.Add(Pair.Key, CurrentUseGeneration);
		}
	}

	for (const FIntPoint Coordinate : OrderedDesiredChunks)
	{
		// A forced rebuild fills the whole visible window. During ordinary
		// streaming, only a genuinely missing player-floor chunk is critical;
		// the camera ring is safe to complete through the bounded prefetch queue.
		if (bForce && FocusChunks.Contains(Coordinate)
			&& !GeneratedTerrainChunks.Contains(Coordinate))
		{
			if (CreateTerrainChunkComponent(Coordinate))
			{
				ActiveTerrainChunks.Add(Coordinate);
				TerrainChunkLastUsed.Add(Coordinate, CurrentUseGeneration);
			}
		}
	}

	while (GeneratedTerrainChunks.Num() > TerrainChunkCacheLimit)
	{
		TArray<FIntPoint> ResidentChunks;
		GeneratedTerrainChunks.GenerateKeyArray(ResidentChunks);
		FIntPoint EvictionCandidate;
		if (!MatterFlux::WorldStreaming::SelectEvictionCandidate(
			ResidentChunks,
			ActiveTerrainChunks,
			TerrainChunkLastUsed,
			EvictionCandidate))
		{
			break;
		}
		if (UProceduralMeshComponent* Component =
			GeneratedTerrainChunks.FindRef(EvictionCandidate))
		{
			RetireTerrainChunkComponent(Component);
		}
		GeneratedTerrainChunks.Remove(EvictionCandidate);
		TerrainChunkLastUsed.Remove(EvictionCandidate);
	}

	// The next ring is generated incrementally on subsequent frames. Missing
	// active cells are queued first; under normal walking they were already in
	// the previous ring. A large teleport receives its immediate floor above,
	// while the remaining camera window finishes through the loading flow.
	MatterFlux::WorldStreaming::FChunkWindowRequest PrefetchRequest;
	PrefetchRequest.FocusChunks = FocusChunks;
	PrefetchRequest.WindowOffsets = WindowRequest.WindowOffsets;
	PrefetchRequest.Radius = TerrainStreamingChunkRadius + 1;
	PrefetchRequest.MaximumChunkCount = MaxStreamingWindowChunks;
	TArray<FIntPoint> OrderedPrefetchChunks;
	FString PrefetchError;
	if (!MatterFlux::WorldStreaming::BuildChunkWindow(
		PrefetchRequest,
		OrderedPrefetchChunks,
		PrefetchError))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Cannot plan terrain prefetch window: %s"),
			*PrefetchError);
		return false;
	}
	PendingTerrainChunkPrefetches.Reset();
	PendingTerrainChunkPrefetches.Reserve(OrderedPrefetchChunks.Num());
	TSet<FIntPoint> QueuedChunks;
	QueuedChunks.Reserve(OrderedPrefetchChunks.Num());
	const auto QueueMissingChunk = [this, &QueuedChunks](
		const FIntPoint Coordinate)
		{
			if (!GeneratedTerrainChunks.Contains(Coordinate)
				&& !TerrainChunksBuilding.Contains(Coordinate)
				&& !QueuedChunks.Contains(Coordinate))
			{
				QueuedChunks.Add(Coordinate);
				PendingTerrainChunkPrefetches.Add(Coordinate);
			}
		};
	for (const FIntPoint Coordinate : OrderedDesiredChunks)
	{
		QueueMissingChunk(Coordinate);
	}
	for (const FIntPoint Coordinate : OrderedPrefetchChunks)
	{
		QueueMissingChunk(Coordinate);
	}
	bool bActiveChunkSetChanged =
		PreviouslyActiveChunks.Num() != ActiveTerrainChunks.Num();
	if (!bActiveChunkSetChanged)
	{
		for (const FIntPoint Coordinate : PreviouslyActiveChunks)
		{
			if (!ActiveTerrainChunks.Contains(Coordinate))
			{
				bActiveChunkSetChanged = true;
				break;
			}
		}
	}
	if (bActiveChunkSetChanged)
	{
		bGroundReactionVisualDirty = true;
		bGroundReactionVisualNeedsFullRebuild = true;
		PendingGroundReactionVisualCellIndices.Reset();
	}
	return true;
}

bool AMatterFluxPlayableWorldActor::ProcessPendingTerrainChunkPrefetches(
	const bool bAllowChunkCommit)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_Terrain_ProcessPrefetchQueue);
	if (!TerrainHeightField.IsValid())
	{
		return false;
	}
	if (!AsyncTerrainHeightField.IsValid())
	{
		AsyncTerrainHeightField = MakeShared<
			const MatterFlux::PlayableLevel::FLevelTerrain,
			ESPMode::ThreadSafe>(TerrainHeightField);
	}
	if (!AsyncTerrainBuildState.IsValid())
	{
		AsyncTerrainBuildState = MakeShared<
			FMatterFluxAsyncTerrainBuildState,
			ESPMode::ThreadSafe>();
	}

	while (!PendingTerrainChunkPrefetches.IsEmpty()
		&& TerrainChunksBuilding.Num()
			< FMath::Max(MaxAsyncStreamingBuildTasks, 1))
	{
		const FIntPoint Coordinate = PendingTerrainChunkPrefetches[0];
		PendingTerrainChunkPrefetches.RemoveAt(
			0,
			1,
			EAllowShrinking::No);
		if (GeneratedTerrainChunks.Contains(Coordinate)
			|| TerrainChunksBuilding.Contains(Coordinate))
		{
			continue;
		}
		TerrainChunksBuilding.Add(Coordinate);
		const TSharedPtr<FMatterFluxAsyncTerrainBuildState,
			ESPMode::ThreadSafe> BuildState = AsyncTerrainBuildState;
		const TSharedPtr<const MatterFlux::PlayableLevel::FLevelTerrain,
			ESPMode::ThreadSafe> TerrainSnapshot = AsyncTerrainHeightField;
		const int32 ChunkSize = TerrainStreamingChunkSize;
		UE::Tasks::Launch(
			UE_SOURCE_LOCATION,
			[BuildState, TerrainSnapshot, Coordinate, ChunkSize]()
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(
					MatterFlux_Terrain_BuildChunkAsync);
				if (!BuildState.IsValid()
					|| BuildState->bCancelled.load(
						std::memory_order_acquire))
				{
					return;
				}
				FMatterFluxPreparedTerrainChunk Result;
				Result.Coordinate = Coordinate;
				Result.bSuccess = TerrainSnapshot.IsValid()
					&& MatterFlux::TerrainMesh::BuildChunk(
						*TerrainSnapshot,
						Coordinate,
						ChunkSize,
						Result.Chunk);
				if (!BuildState->bCancelled.load(
						std::memory_order_acquire))
				{
					BuildState->CompletedChunks.Enqueue(MoveTemp(Result));
				}
			},
			// Terrain immediately outside the camera is latency-sensitive async
			// work. Use regular task workers so render/editor background jobs cannot
			// starve all in-flight builds while the player keeps walking.
			UE::Tasks::ETaskPriority::Normal);
	}
	if (!bAllowChunkCommit)
	{
		return false;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	int32 CreatedChunkCount = 0;
	FMatterFluxPreparedTerrainChunk PreparedChunk;
	while (CreatedChunkCount < MaxTerrainChunkPrefetchesPerFrame
		&& AsyncTerrainBuildState->CompletedChunks.Dequeue(PreparedChunk))
	{
		TerrainChunksBuilding.Remove(PreparedChunk.Coordinate);
		if (!PreparedChunk.bSuccess
			|| GeneratedTerrainChunks.Contains(PreparedChunk.Coordinate))
		{
			continue;
		}

		UProceduralMeshComponent* Component =
			CreateTerrainChunkComponentFromData(
				PreparedChunk.Coordinate,
				PreparedChunk.Chunk);
		if (!Component)
		{
			continue;
		}
		++CreatedChunkCount;
		const bool bShouldBeActive =
			DesiredTerrainChunks.Contains(PreparedChunk.Coordinate);
		Component->SetVisibility(
			bShouldBeActive && GetNetMode() != NM_DedicatedServer,
			true);
		Component->SetHiddenInGame(
			!bShouldBeActive || GetNetMode() == NM_DedicatedServer,
			true);
		Component->SetCollisionEnabled(
			bShouldBeActive
				? ECollisionEnabled::QueryAndPhysics
				: ECollisionEnabled::NoCollision);
		TerrainChunkLastUsed.Add(
			PreparedChunk.Coordinate,
			++TerrainChunkUseCounter);
		if (bShouldBeActive)
		{
			ActiveTerrainChunks.Add(PreparedChunk.Coordinate);
			bGroundReactionVisualDirty = true;
			bGroundReactionVisualNeedsFullRebuild = true;
			PendingGroundReactionVisualCellIndices.Reset();
		}
		if ((FPlatformTime::Seconds() - StartSeconds) * 1000.0
			>= TerrainChunkPrefetchBudgetMilliseconds)
		{
			break;
		}
	}

	while (GeneratedTerrainChunks.Num() > TerrainChunkCacheLimit)
	{
		TArray<FIntPoint> ResidentChunks;
		GeneratedTerrainChunks.GenerateKeyArray(ResidentChunks);
		FIntPoint EvictionCandidate;
		if (!MatterFlux::WorldStreaming::SelectEvictionCandidate(
			ResidentChunks,
			ActiveTerrainChunks,
			TerrainChunkLastUsed,
			EvictionCandidate))
		{
			break;
		}
		if (UProceduralMeshComponent* Component =
			GeneratedTerrainChunks.FindRef(EvictionCandidate))
		{
			RetireTerrainChunkComponent(Component);
		}
		GeneratedTerrainChunks.Remove(EvictionCandidate);
		TerrainChunkLastUsed.Remove(EvictionCandidate);
	}
	return CreatedChunkCount > 0;
}

UProceduralMeshComponent*
AMatterFluxPlayableWorldActor::CreateTerrainChunkComponent(
	const FIntPoint ChunkCoordinate)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_Terrain_BuildChunkSynchronous);
	MatterFlux::TerrainMesh::FChunk Chunk;
	if (!MatterFlux::TerrainMesh::BuildChunk(
		TerrainHeightField,
		ChunkCoordinate,
		TerrainStreamingChunkSize,
		Chunk))
	{
		return nullptr;
	}
	return CreateTerrainChunkComponentFromData(ChunkCoordinate, Chunk);
}

UProceduralMeshComponent*
AMatterFluxPlayableWorldActor::CreateTerrainChunkComponentFromData(
	const FIntPoint ChunkCoordinate,
	const MatterFlux::TerrainMesh::FChunk& Chunk)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_Terrain_CommitChunkComponent);
	const FName ComponentName = MakeUniqueObjectName(
		this,
		UProceduralMeshComponent::StaticClass(),
		*FString::Printf(
			TEXT("TerrainChunk_%d_%d"),
			ChunkCoordinate.X,
			ChunkCoordinate.Y));
	UProceduralMeshComponent* Component =
		NewObject<UProceduralMeshComponent>(this, ComponentName);
	Component->SetupAttachment(SceneRoot);
	// Terrain chunks stream in and out, but their transforms never move. Marking
	// them movable makes CharacterMovement treat the floor as a dynamic
	// movement base and serialize the runtime component through every client
	// move. Client rebuilds can legitimately give those transient components a
	// different UObject suffix, so they cannot be resolved through NetGUIDs.
	// Static mobility keeps character moves in world space and avoids both the
	// unresolved-base corrections and the per-frame network warning storm.
	Component->SetMobility(EComponentMobility::Static);
	// Vertex/index generation is completed on a background task. UObject and
	// render-state submission stay here on the game thread, while Chaos cooking
	// remains asynchronous and starts well outside the visible boundary.
	Component->bUseAsyncCooking = true;
	// Character based-movement replication may still include the component
	// pointer while transitioning on or off the floor. Deterministic names plus
	// this flag make those occasional references resolvable without replicating
	// the procedural mesh itself.
	Component->SetNetAddressable();
	Component->bUseComplexAsSimpleCollision = true;
	Component->SetCollisionObjectType(ECC_WorldStatic);
	Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Component->SetCollisionResponseToAllChannels(ECR_Block);
	// The streamed terrain currently has no AI-navigation contract. Registering
	// movable procedural chunks with the navigation octree makes every
	// visibility transition enqueue expensive updates, and registration occurs
	// before mesh sections exist (which also produces empty-bounds warnings).
	// If navigation is added later, use stable navigation proxies instead.
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(true);
	Component->SetVisibility(GetNetMode() != NM_DedicatedServer, true);
	Component->SetHiddenInGame(GetNetMode() == NM_DedicatedServer, true);
	AddInstanceComponent(Component);
	Component->RegisterComponent();

	const TArray<FLinearColor> EmptyColors;
	const TArray<FProcMeshTangent> EmptyTangents;
	for (int32 SectionIndex = 0;
		SectionIndex < Chunk.Sections.Num();
		++SectionIndex)
	{
		const MatterFlux::TerrainMesh::FSection& Section =
			Chunk.Sections[SectionIndex];
		if (!Section.IsValid())
		{
			continue;
		}
		Component->CreateMeshSection_LinearColor(
			SectionIndex,
			Section.Vertices,
			Section.Triangles,
			Section.Normals,
			Section.UVs,
			EmptyColors,
			EmptyTangents,
			false);
		if (ColorMaterialTemplate
			&& TerrainHeightField.BandColors.IsValidIndex(SectionIndex))
		{
			UMaterialInstanceDynamic* Material =
				UMaterialInstanceDynamic::Create(
					ColorMaterialTemplate,
					Component);
			Material->SetVectorParameterValue(
				TEXT("Color"),
				TerrainHeightField.BandColors[SectionIndex]);
			Material->SetScalarParameterValue(TEXT("FaceContrast"), 0.86f);
			Material->SetScalarParameterValue(TEXT("ColorVariation"), 0.030f);
			Material->SetScalarParameterValue(
				TEXT("PixelSize"),
				TerrainHeightField.CellSize);
			Material->SetScalarParameterValue(TEXT("Roughness"), 0.92f);
			Material->SetScalarParameterValue(TEXT("ShadowLift"), 0.27f);
			Component->SetMaterial(SectionIndex, Material);
		}
	}
	const int32 CollisionSectionIndex = Chunk.Sections.Num();
	Component->CreateMeshSection_LinearColor(
		CollisionSectionIndex,
		Chunk.CollisionSurface.Vertices,
		Chunk.CollisionSurface.Triangles,
		Chunk.CollisionSurface.Normals,
		Chunk.CollisionSurface.UVs,
		EmptyColors,
		EmptyTangents,
		true);
	Component->SetMeshSectionVisible(CollisionSectionIndex, false);
	GeneratedTerrainChunks.Add(ChunkCoordinate, Component);
	return Component;
}

void AMatterFluxPlayableWorldActor::DestroyTerrainChunkMeshes()
{
	if (AsyncTerrainBuildState.IsValid())
	{
		AsyncTerrainBuildState->bCancelled.store(
			true, std::memory_order_release);
	}
	AsyncTerrainBuildState.Reset();
	AsyncTerrainHeightField.Reset();
	TerrainChunksBuilding.Reset();
	for (const TPair<FIntPoint, TObjectPtr<UProceduralMeshComponent>>& Pair
		: GeneratedTerrainChunks)
	{
		if (IsValid(Pair.Value))
		{
			RetireTerrainChunkComponent(Pair.Value);
		}
	}
	GeneratedTerrainChunks.Reset();
	DesiredTerrainChunks.Reset();
	ActiveTerrainChunks.Reset();
	PendingTerrainChunkPrefetches.Reset();
	TerrainChunkLastUsed.Reset();
	TerrainChunkUseCounter = 0;
	bTerrainCacheCoversWholeMap = false;
	TerrainHeightField = MatterFlux::PlayableLevel::FLevelTerrain();
}

UHierarchicalInstancedStaticMeshComponent*
AMatterFluxPlayableWorldActor::FindOrCreateLayerComponent(
	const MatterFlux::PlayableLevel::FLevelLayer& Layer)
{
	UStaticMesh* Mesh = CubeMesh;
	switch (Layer.Primitive)
	{
	case MatterFlux::PlayableLevel::ELayerPrimitive::Sphere:
		Mesh = SphereMesh;
		break;
	case MatterFlux::PlayableLevel::ELayerPrimitive::Cylinder:
		Mesh = CylinderMesh;
		break;
	case MatterFlux::PlayableLevel::ELayerPrimitive::Cone:
		Mesh = ConeMesh;
		break;
	default:
		break;
	}
	if (!Mesh)
	{
		return nullptr;
	}

	UHierarchicalInstancedStaticMeshComponent* Instances =
		GeneratedLayerInstances.FindRef(Layer.Name);
	if (!Instances)
	{
		const FName ComponentName = MakeUniqueObjectName(
			this,
			UHierarchicalInstancedStaticMeshComponent::StaticClass(),
			*FString::Printf(TEXT("Generated_%s"), *Layer.Name.ToString()));
		Instances = NewObject<UHierarchicalInstancedStaticMeshComponent>(
			this,
			ComponentName);
		Instances->SetupAttachment(SceneRoot);
		Instances->SetMobility(EComponentMobility::Movable);
		AddInstanceComponent(Instances);
		Instances->RegisterComponent();
		GeneratedLayerInstances.Add(Layer.Name, Instances);
	}

	Instances->SetStaticMesh(Mesh);
	const bool bVisible =
		Layer.RenderMode
		!= MatterFlux::PlayableLevel::ELevelLayerRenderMode::CollisionOnly;
	Instances->SetVisibility(bVisible, true);
	Instances->SetHiddenInGame(!bVisible, true);
	const bool bTerrainSurfaceBand =
		Layer.Name == TEXT("Lowlands")
		|| Layer.Name == TEXT("Midlands")
		|| Layer.Name == TEXT("Highlands")
		|| Layer.Name == TEXT("Stream");
	Instances->SetCastShadow(
		!bTerrainSurfaceBand
		&& (Layer.RenderMode
				== MatterFlux::PlayableLevel::ELevelLayerRenderMode::Lit
			|| Layer.RenderMode
				== MatterFlux::PlayableLevel::ELevelLayerRenderMode::VoxelLit));
	Instances->SetCanEverAffectNavigation(Layer.bEnableCollision);
	Instances->SetCollisionEnabled(
		Layer.bEnableCollision
			? ECollisionEnabled::QueryAndPhysics
			: ECollisionEnabled::NoCollision);
	Instances->SetCollisionResponseToAllChannels(
		Layer.bEnableCollision ? ECR_Block : ECR_Ignore);

	const bool bLiquidLayer = Layer.RenderMode
		== MatterFlux::PlayableLevel::ELevelLayerRenderMode::Liquid;
	UMaterialInterface* LayerMaterialTemplate = bLiquidLayer
		? VoxelLiquidMaterialTemplate.Get()
		: Layer.RenderMode
			== MatterFlux::PlayableLevel::ELevelLayerRenderMode::VoxelUnlit
			? VoxelColorMaterialTemplate.Get()
			: ColorMaterialTemplate.Get();
	if (bVisible && LayerMaterialTemplate)
	{
		UMaterialInstanceDynamic* Material =
			Cast<UMaterialInstanceDynamic>(Instances->GetMaterial(0));
		if (!Material)
		{
			Material =
				UMaterialInstanceDynamic::Create(LayerMaterialTemplate, this);
			Instances->SetMaterial(0, Material);
		}
		Material->SetVectorParameterValue(TEXT("Color"), Layer.Color);
		if (bLiquidLayer
			&& IMatterFluxScriptRuntime::IsAvailable())
		{
			const FMatterFluxContentRegistryPtr Registry =
				IMatterFluxScriptRuntime::Get().GetActiveRegistry();
			if (Registry.IsValid())
			{
				if (const FMatterFluxMaterialDefinition* Definition =
					Registry->Materials.Find(Layer.MaterialId))
				{
					ApplyLiquidOptics(*Material, *Definition);
				}
			}
		}
		const bool bStreamLayer = Layer.Name == TEXT("Stream")
			|| Layer.Name == TEXT("Lake");
		Material->SetScalarParameterValue(
			TEXT("FaceContrast"),
			bStreamLayer ? 0.42f : 0.72f);
		Material->SetScalarParameterValue(TEXT("ColorVariation"), 0.030f);
		Material->SetScalarParameterValue(
			TEXT("PixelSize"),
			MatterFlux::PlayableLevel::TerrainCellSize);
		Material->SetScalarParameterValue(
			TEXT("Roughness"),
			bStreamLayer ? 0.42f : 0.92f);
		Material->SetScalarParameterValue(
			TEXT("ShadowLift"),
			0.20f);
	}
	return Instances;
}

void AMatterFluxPlayableWorldActor::ApplyLiquidMaterialChunkMesh(
	const FName ComponentKey,
	const FName MaterialId,
	const FIntPoint ChunkCoordinate,
	const FMatterFluxMaterialDefinition& MaterialDefinition,
	MatterFlux::Rendering::FLiquidSurfaceProjection& Projection)
{
	UProceduralMeshComponent* Mesh =
		GeneratedLiquidLayerMeshes.FindRef(ComponentKey);
	if (GetNetMode() == NM_DedicatedServer
		|| Projection.Triangles.IsEmpty())
	{
		if (Mesh)
		{
			Mesh->DestroyComponent();
		}
		GeneratedLiquidLayerMeshes.Remove(ComponentKey);
		LiquidProjectionMaterials.Remove(ComponentKey);
		LiquidProjectionChunks.Remove(ComponentKey);
		LiquidChunkProjectionHeightAudits.Remove(ComponentKey);
		return;
	}
	if (!Mesh)
	{
		const FName ComponentName = MakeUniqueObjectName(
			this,
			UProceduralMeshComponent::StaticClass(),
			*ComponentKey.ToString());
		Mesh = NewObject<UProceduralMeshComponent>(this, ComponentName);
		Mesh->SetupAttachment(SceneRoot);
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(false);
		Mesh->ComponentTags.AddUnique(TEXT("MatterFluxLiquidSurface"));
		Mesh->ComponentTags.AddUnique(
			TEXT("MatterFluxCanonicalLiquidSurface"));
		AddInstanceComponent(Mesh);
		Mesh->RegisterComponent();
		GeneratedLiquidLayerMeshes.Add(ComponentKey, Mesh);
	}
	LiquidProjectionMaterials.Add(ComponentKey, MaterialId);
	LiquidProjectionChunks.Add(ComponentKey, ChunkCoordinate);
	// Keep the disposable free-surface projection just above coplanar voxel
	// support faces. This is a render-space depth bias only: sampling, buoyancy,
	// displacement, replication, and the projection history retain canonical Z.
	constexpr float LiquidSurfaceDepthBias = 2.0f;
	for (FVector& Vertex : Projection.Vertices)
	{
		Vertex.Z += LiquidSurfaceDepthBias;
	}
	FMatterFluxLiquidProjectionHeightAudit& Audit =
		LiquidChunkProjectionHeightAudits.FindOrAdd(ComponentKey);
	Audit.CanonicalMedianSurfaceZ =
		Projection.CanonicalMedianSurfaceHeight;
	Audit.RenderedMedianSurfaceZ =
		Projection.ProjectedCanonicalMedianSurfaceHeight
			+ LiquidSurfaceDepthBias;
	Audit.MedianOffset = Projection.MedianCanonicalHeightOffset
		+ LiquidSurfaceDepthBias;
	Audit.MaximumAbsoluteLocalOffset =
		Projection.MaximumAbsoluteCanonicalHeightOffset
			+ LiquidSurfaceDepthBias;
	Audit.CanonicalCellCount = Projection.ProjectedCellCount;
	Audit.ProjectedCellCount = Projection.ProjectedCellCount;
	float MaximumTriangleHeightSpan = 0.0f;
	for (int32 TriangleIndex = 0;
		TriangleIndex + 2 < Projection.TopTriangleIndexCount;
		TriangleIndex += 3)
	{
		const float Height0 = Projection.Vertices[
			Projection.Triangles[TriangleIndex]].Z;
		const float Height1 = Projection.Vertices[
			Projection.Triangles[TriangleIndex + 1]].Z;
		const float Height2 = Projection.Vertices[
			Projection.Triangles[TriangleIndex + 2]].Z;
		MaximumTriangleHeightSpan = FMath::Max(
			MaximumTriangleHeightSpan,
			FMath::Max3(Height0, Height1, Height2)
				- FMath::Min3(Height0, Height1, Height2));
	}
	Audit.MaximumTriangleHeightSpan = MaximumTriangleHeightSpan;
	Audit.SurfacePatchCount = Projection.SurfacePatchCount;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	Colors.Reserve(Projection.Vertices.Num());
	const float SafeOpacityDepth = FMath::Max(
		MaterialDefinition.OpacityDepth,
		1.0f);
	for (int32 VertexIndex = 0;
		VertexIndex < Projection.Vertices.Num();
		++VertexIndex)
	{
		const float ColumnDepth = Projection.ColumnDepths.IsValidIndex(VertexIndex)
			? Projection.ColumnDepths[VertexIndex]
			: SafeOpacityDepth;
		Colors.Add(FLinearColor(
			1.0f,
			1.0f,
			1.0f,
			FMath::Clamp(ColumnDepth / SafeOpacityDepth, 0.0f, 1.0f)));
	}
	Tangents.Init(
		FProcMeshTangent(FVector::ForwardVector, false),
		Projection.Vertices.Num());
	Mesh->CreateMeshSection_LinearColor(
		0,
		Projection.Vertices,
		Projection.Triangles,
		Projection.Normals,
		Projection.UVs,
		Colors,
		Tangents,
		false);
	Mesh->SetVisibility(true, true);
	Mesh->SetHiddenInGame(false, true);
	if (VoxelLiquidMaterialTemplate)
	{
		UMaterialInstanceDynamic* Material =
			Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0));
		if (!Material)
		{
			Material = UMaterialInstanceDynamic::Create(
				VoxelLiquidMaterialTemplate, this);
			Mesh->SetMaterial(0, Material);
		}
		ApplyLiquidOptics(*Material, MaterialDefinition);
		// Runtime liquid projections use the same voxel-face response as authored
		// lakes. Leaving these at the material defaults makes newly exposed
		// particle-column side walls render almost black, so a normal refill wake
		// looks like detached debris instead of a stepped liquid silhouette.
		Material->SetScalarParameterValue(TEXT("FaceContrast"), 0.42f);
		Material->SetScalarParameterValue(TEXT("ColorVariation"), 0.030f);
		Material->SetScalarParameterValue(
			TEXT("PixelSize"), MaterialSimulationCellSize);
		Material->SetScalarParameterValue(TEXT("Roughness"), 0.42f);
		Material->SetScalarParameterValue(TEXT("ShadowLift"), 0.20f);
	}
}
