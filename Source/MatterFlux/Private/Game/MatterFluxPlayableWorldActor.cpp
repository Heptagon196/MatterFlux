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
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "MatterFluxLog.h"
#include "Material/MatterFluxLiquidBuoyancy.h"
#include "Material/MatterFluxMaterialReactionEngine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/MatterFluxInstanceVisuals.h"
#include "Rendering/MatterFluxLiquidSurfaceProjection.h"
#include "Stats/Stats.h"
#include "UObject/ConstructorHelpers.h"

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
	TEXT("Combustion"),
	STAT_MatterFluxCombustion,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Ground Combustion Simulation"),
	STAT_MatterFluxGroundCombustionSimulation,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Ground Combustion Replication"),
	STAT_MatterFluxGroundCombustionReplication,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Source Combustion Replication"),
	STAT_MatterFluxSourceCombustionReplication,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Ground Combustion Visuals"),
	STAT_MatterFluxGroundCombustionVisuals,
	STATGROUP_MatterFlux);
DECLARE_CYCLE_STAT(
	TEXT("Combustion Propagation"),
	STAT_MatterFluxCombustionPropagation,
	STATGROUP_MatterFlux);

namespace
{
	// Body displacement is a local pressure boundary, not a request to rescan
	// the whole visible lake for every occupied cell. Sixteen material cells
	// provide substantially more free-column capacity than the largest current
	// character/creature footprint while keeping work inside a compact dirty
	// neighborhood. The conserved wake pass reconnects that local surplus later.
	constexpr int32 BodyLiquidDisplacementSearchRadiusCells = 16;

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
			+ Item.PackedResidueMask.Num()
			+ Item.PackedBurningMask.Num();
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
		return State.bHasCombustionState
			? PackedMaskBytes * 3
			: PackedMaskBytes;
	};
	auto StoredPayloadByteCount = [](
		const FMatterFluxReplicatedFragmentSourceState& Item)
	{
		return static_cast<int64>(Item.PackedRuntimeMask.Num())
			+ Item.PackedResidueMask.Num()
			+ Item.PackedBurningMask.Num();
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
			|| (State->bHasCombustionState
				&& (!IsBinaryMask(State->CombustionState.ResidueMask)
					|| State->CombustionState.ResidueMask.Num()
						!= State->GetRuntimeMask().Num()
					|| State->CombustionState.BurningMask.Num()
						!= State->GetRuntimeMask().Num()
					|| !FMath::IsFinite(State->CombustionAccumulator)
					|| State->CombustionAccumulator < 0.0f
					|| State->TotalSmokeEmissionCount < 0)))
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
		Item.bHasCombustionState = State.bHasCombustionState;
		Item.CombustionRuleId = State.bHasCombustionState
			? State.CombustionState.RuleId
			: NAME_None;
		Item.CombustionSeed = State.bHasCombustionState
			? State.CombustionState.Seed
			: 0;
		Item.CombustionTick = State.bHasCombustionState
			? State.CombustionState.Tick
			: 0;
		if (State.bHasCombustionState)
		{
			PackPresenceMask(
				State.CombustionState.ResidueMask,
				Item.PackedResidueMask);
			PackPresenceMask(
				State.CombustionState.BurningMask,
				Item.PackedBurningMask);
			Item.CombustionAccumulator = State.CombustionAccumulator;
			Item.TotalSmokeEmissionCount = State.TotalSmokeEmissionCount;
		}
		else
		{
			Item.PackedResidueMask.Reset();
			Item.PackedBurningMask.Reset();
			Item.CombustionAccumulator = 0.0f;
			Item.TotalSmokeEmissionCount = 0;
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
	constexpr int32 MaxAlwaysLoadedRenderOnlyLayerInstances = 4096;
	constexpr int32 MaxStreamingWindowChunks = 65536;
	constexpr int32 MaxReplicatedFragmentSourceStates = 4096;
	constexpr int32 MaxReplicatedFragmentSourceStateBytes = 1024 * 1024;
	constexpr float NeutralVoxelLiquidRefractionIndex = 1.0f;

	void ApplyLiquidOptics(
		UMaterialInstanceDynamic& DynamicMaterial,
		const FMatterFluxMaterialDefinition& Material)
	{
		// The material simulation supplies volume; this adapter draws only the
		// outside free surface. Depth-fading against the stepped terrain below
		// exposes every basin voxel as a false ring/crack in that surface, so the
		// complete projected shape uses one opacity. Refraction remains neutral to
		// avoid a second, screen-space copy of the voxel shoreline.
		// Refraction is neutral and the simulation already provides the complete
		// outside silhouette. Making that shell opaque prevents stepped terrain
		// below from masquerading as holes or detached liquid fragments.
		const float SurfaceOpacity = 1.0f;
		DynamicMaterial.SetVectorParameterValue(TEXT("Color"), Material.Color);
		DynamicMaterial.SetScalarParameterValue(
			TEXT("ShallowOpacity"),
			SurfaceOpacity);
		DynamicMaterial.SetScalarParameterValue(
			TEXT("DeepOpacity"),
			SurfaceOpacity);
		DynamicMaterial.SetScalarParameterValue(
			TEXT("OpacityDepth"),
			Material.OpacityDepth);
		DynamicMaterial.SetScalarParameterValue(
			TEXT("RefractionIndex"),
			NeutralVoxelLiquidRefractionIndex);
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

	constexpr int32 GroundCombustionChunkSize = 64;

	MatterFlux::Combustion::FGroundRuntimeSettings
	MakeGroundCombustionRuntimeSettings()
	{
		MatterFlux::Combustion::FGroundRuntimeSettings Settings;
		Settings.Width = MatterFlux::PlayableLevel::TerrainCellsX;
		Settings.Height = MatterFlux::PlayableLevel::TerrainCellsY;
		Settings.ChunkSize = GroundCombustionChunkSize;
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

	FMatterFluxSavedCombustionState ToSavedCombustionState(
		const MatterFlux::Combustion::FStateSnapshot& State)
	{
		FMatterFluxSavedCombustionState Saved;
		Saved.RuleId = State.RuleId;
		Saved.Width = State.Width;
		Saved.Height = State.Height;
		Saved.Seed = State.Seed;
		Saved.Tick = State.Tick;
		Saved.FuelMask = State.FuelMask;
		Saved.ResidueMask = State.ResidueMask;
		Saved.BurningMask = State.BurningMask;
		return Saved;
	}

	MatterFlux::Combustion::FStateSnapshot ToRuntimeCombustionState(
		const FMatterFluxSavedCombustionState& State)
	{
		MatterFlux::Combustion::FStateSnapshot Runtime;
		Runtime.RuleId = State.RuleId;
		Runtime.Width = State.Width;
		Runtime.Height = State.Height;
		Runtime.Seed = State.Seed;
		Runtime.Tick = State.Tick;
		Runtime.FuelMask = State.FuelMask;
		Runtime.ResidueMask = State.ResidueMask;
		Runtime.BurningMask = State.BurningMask;
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
		Saved.bHasCombustionState = State.bHasCombustionState;
		if (State.bHasCombustionState)
		{
			Saved.CombustionState = ToSavedCombustionState(
				State.CombustionState);
		}
		Saved.CombustionAccumulator = State.CombustionAccumulator;
		Saved.TotalSmokeEmissionCount = State.TotalSmokeEmissionCount;
		return Saved;
	}

	FFragment2DSourceStreamingState ToRuntimeFragmentState(
		const FMatterFluxSavedFragmentSourceState& State)
	{
		FFragment2DSourceStreamingState Runtime;
		Runtime.Revision = State.Revision;
		Runtime.bHasCombustionState = State.bHasCombustionState;
		if (State.bHasCombustionState)
		{
			Runtime.CombustionState = ToRuntimeCombustionState(
				State.CombustionState);
		}
		Runtime.CombustionAccumulator = State.CombustionAccumulator;
		Runtime.TotalSmokeEmissionCount = State.TotalSmokeEmissionCount;
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
				RebuildLevel();
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
		RebuildLevel();
	}
}

void AMatterFluxPlayableWorldActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	++GenerationRequestSerial;
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
	DestroyTerrainChunkMeshes();
	DestroyGeneratedHouse();
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->ResetSources();
	}
	PendingMaterialDisplacementCells.Reset();
	PreviousMaterialDisplacementCells.Reset();
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
}

void AMatterFluxPlayableWorldActor::Tick(const float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxWorldTick);
	Super::Tick(DeltaSeconds);
	if (bReplicatedFragmentSourceStatesDirty)
	{
		bReplicatedFragmentSourceStatesDirty = false;
		ApplyReplicatedFragmentSourceStates();
	}
	if (FragmentSourceProxy)
	{
		CombustionProxyFlushAccumulator +=
			FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
		if (CombustionProxyFlushAccumulator >= 0.5f)
		{
			CombustionProxyFlushAccumulator = FMath::Fmod(
				CombustionProxyFlushAccumulator,
				0.5f);
			FragmentSourceProxy->FlushDeferredCombustionChanges();
		}
		FragmentSourceProxy->FlushPendingChanges();
	}
	AdvanceAsyncGeneration();
	ProcessPendingTerrainChunkPrefetches();
	AdvanceLogicalSourceCombustion(DeltaSeconds);
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
			bMaterialVisualizationDeferredForStreaming;
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
		AdvanceGroundCombustion(DeltaSeconds);
		return;
	}

	const bool bFinishDeferredVisualization =
		bMaterialVisualizationDeferredForStreaming;
	TArray<FIntPoint> NextMaterialFocusCells;
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
	MatterFlux::Material::FRuntimeAdvanceResult MaterialAdvance =
		MaterialSimulation->AdvanceAuthority(
			DeltaSeconds,
			NextMaterialFocusCells);
	// Only a material step can flow liquid back into a body. Frames that merely
	// project the already constrained state do not need a second identical
	// solve. This keeps body occupancy deterministic while avoiding a redundant
	// full displacement search on most render frames.
	if (MaterialAdvance.Steps > 0)
	{
		DisplacedMaterialCells += MaterialSimulation->DisplaceLiquids(
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
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::Get().GetActiveRegistry();
		if (Registry.IsValid())
		{
			ApplyMaterialSourceContactReactions(*Registry);
		}
	}
	// Focus changes already archive/restore material chunks and refresh both
	// terrain streaming systems. Publish the now-dirty atomic snapshot on a
	// stable streaming frame so compression does not stack on either a material
	// or render/source chunk boundary.
	if (!MaterialAdvance.bFocusChanged
		&& !bStreamingFocusChanged
		&& MaterialSimulation->NeedsReplicationPublish())
	{
		PublishMaterialSimulationState();
		ForceNetUpdate();
	}
	if (bFinishDeferredVisualization
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
	PropagateCombustion(DeltaSeconds);
	AdvanceGroundCombustion(DeltaSeconds);
}

void AMatterFluxPlayableWorldActor::Regenerate(const int32 NewSeed)
{
	if (!HasAuthority())
	{
		return;
	}
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
		InitializeMaterialSimulation(
			*PendingGenerationRegistry,
			*PendingGeneratedLayout);
		InitializeGroundCombustion(
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
	RebuildLevel();
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

void AMatterFluxPlayableWorldActor::
	MarkReplicatedFragmentSourceStatesDirty()
{
	bReplicatedFragmentSourceStatesDirty = true;
}

void AMatterFluxPlayableWorldActor::RebuildLevel()
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxWorldRebuild);
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
			: 0.05f;
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
				0.05f,
				1.0f)
			: 0.10f;
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
}

void AMatterFluxPlayableWorldActor::ApplyGeneratedLayoutSynchronously(
	const FMatterFluxContentRegistry& Registry,
	const MatterFlux::PlayableLevel::FLevelLayout& Layout)
{
	InitializeMaterialSimulation(Registry, Layout);
	InitializeGroundCombustion(Registry, Layout);
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

	const FTransform HouseTransform(
		// 固定 2.5D 镜头的 yaw 为 -45°。房屋旋转 45° 后，山墙
		// 面向镜头、屋脊横跨屏幕，避免沿屋脊观察产生放射透视。
		FRotator(0.0f, 45.0f, 0.0f),
		FVector(
			Layout.HouseLocation.X,
			Layout.HouseLocation.Y,
			FoundationTop + 4.0f));
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = this;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	GeneratedHouse = GetWorld()->SpawnActor<AMatterFluxTwoStoreyHouseActor>(
		AMatterFluxTwoStoreyHouseActor::StaticClass(),
		HouseTransform,
		SpawnParameters);
	if (GeneratedHouse)
	{
		GeneratedHouse->Tags.AddUnique(TEXT("MatterFluxGeneratedHouse"));
	}
}

void AMatterFluxPlayableWorldActor::DestroyGeneratedHouse()
{
	if (IsValid(GeneratedHouse))
	{
		GeneratedHouse->Destroy();
	}
	GeneratedHouse = nullptr;
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

	if (GroundCombustion)
	{
		MatterFlux::Combustion::FGroundRuntimeSnapshot Snapshot;
		if (!GroundCombustion->CaptureState(Snapshot))
		{
			OutError = TEXT("ground combustion state could not be captured");
			return false;
		}
		OutState.bHasGroundCombustionState = true;
		OutState.GroundCombustionState =
			ToSavedCombustionState(Snapshot.CombustionState);
		OutState.GroundCombustionAccumulator =
			Snapshot.StepAccumulator;
		OutState.GroundCombustionRevision =
			Snapshot.Revision;
	}
	OutState.SourcesThatIgnitedGround.Reserve(
		SourcesThatIgnitedGround.Num());
	for (const FGuid SourceId : SourcesThatIgnitedGround)
	{
		OutState.SourcesThatIgnitedGround.Add(SourceId);
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
		// The save contains its own focus set. Force the next authority tick to
		// reconcile it with the currently possessed players.
		MaterialSimulation->RequireFocusReconciliation();
		bMaterialVisualizationDirty = true;
		PublishMaterialSimulationState();
	}

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

	if (State.bHasGroundCombustionState)
	{
		const FMatterFluxReactionDefinition* Rule = Registry->Reactions.Find(
			State.GroundCombustionState.RuleId);
		if (!Rule)
		{
			OutError = TEXT("saved ground combustion rule no longer exists");
			return false;
		}
		auto RestoredGround = MakeUnique<
			MatterFlux::Combustion::FGroundCombustionRuntime>();
		MatterFlux::Combustion::FGroundRuntimeSnapshot Snapshot;
		Snapshot.CombustionState =
			ToRuntimeCombustionState(State.GroundCombustionState);
		Snapshot.StepAccumulator = State.GroundCombustionAccumulator;
		Snapshot.Revision = State.GroundCombustionRevision;
		if (!RestoredGround->RestoreState(
			MakeGroundCombustionRuntimeSettings(),
			Snapshot,
			*Rule,
			OutError))
		{
			return false;
		}
		GroundCombustion = MoveTemp(RestoredGround);
		bGroundCombustionVisualDirty = true;
		bGroundCombustionVisualNeedsFullRebuild = true;
		PendingGroundCombustionVisualCellIndices.Reset();
		DestroyGroundStateChunks();
		InitializeGroundStateChunks();
	}
	SourcesThatIgnitedGround.Reset();
	for (const FGuid SourceId : State.SourcesThatIgnitedGround)
	{
		if (SourceId.IsValid())
		{
			SourcesThatIgnitedGround.Add(SourceId);
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
	ActiveSourceCombustions.Reset();
	LogicalSourceCombustionIndex.Reset();
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		LogicalSourceCombustionIndex.ApplySnapshot(
			Pair.Key,
			Pair.Value.bHasCombustionState,
			Pair.Value.CombustionState.BurningMask);
	}
	if (FragmentSourceProxy)
	{
		TArray<FGuid> RestoredSourceIds;
		RestoredSourceIds.Reserve(StreamedFragmentSourceStates.Num());
		for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
			: StreamedFragmentSourceStates)
		{
			const FMatterFluxReactionDefinition* Rule = nullptr;
			FName ResidueMaterial = NAME_None;
			FLinearColor ResidueColor = FLinearColor::Transparent;
			TArray<uint8> ResidueMask;
			if (Pair.Value.bHasCombustionState)
			{
				Rule = Registry->Reactions.Find(
					Pair.Value.CombustionState.RuleId);
				const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
					FindFragmentSourceDefinition(Pair.Key);
				if (!Rule || !Source)
				{
					OutError = TEXT("saved logical source combustion rule or source no longer exists");
					return false;
				}
				ResidueMask = Pair.Value.CombustionState.ResidueMask;
				ResidueMaterial = Rule->OutputA;
				ResidueColor = FLinearColor(0.08f, 0.07f, 0.06f);
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(Rule->OutputA))
				{
					ResidueColor = Material->Color;
				}
			}
			else
			{
				ResidueMask.Init(0, Pair.Value.GetRuntimeMask().Num());
			}
			const bool bCombustionActive = Rule
				&& Pair.Value.CombustionState.BurningMask.ContainsByPredicate(
					[](const uint8 Value)
					{
						return Value != 0;
					});
			if (FragmentSourceProxy->ApplySourceState(
				Pair.Key,
				Pair.Value.GetRuntimeMask(),
				ResidueMask,
				ResidueMaterial,
				ResidueColor,
				bCombustionActive)
				== EMatterFluxFragmentSourceProxyApplyResult::Invalid)
			{
				OutError = TEXT("saved logical source masks do not match the generated source");
				return false;
			}
			RestoredSourceIds.Add(Pair.Key);
			if (bCombustionActive)
			{
				auto Runtime = MakeUnique<
					MatterFlux::Combustion::FSourceCombustionRuntime>();
				if (!Runtime->RestoreState(
					MatterFlux::Combustion::FSourceRuntimeSettings(),
					Pair.Value,
					*Rule,
					OutError))
				{
					return false;
				}
				ActiveSourceCombustions.Add(Pair.Key, MoveTemp(Runtime));
			}
		}
		if (!PublishFragmentSourceStateBatch(RestoredSourceIds))
		{
			OutError = TEXT("saved fragment source states exceed the replication budget");
			return false;
		}
	}
	bSourceCombustionVisualDirty = true;
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
	bMaterialVisualizationDirty = true;
	return true;
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
	const int64 CellX = FMath::RoundToInt64(
		(Local.X - TerrainHeightField.FirstCellCenter.X)
			/ TerrainHeightField.CellSize);
	const int64 CellY = FMath::RoundToInt64(
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

bool AMatterFluxPlayableWorldActor::DisplaceLiquidInWorldBounds(
	const FVector& Center,
	const FVector& HorizontalExtent,
	const float BottomZ,
	const float TopZ,
	const bool bCapsuleShape,
	const bool bDeferMaterialSolve)
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
					|| !MaterialLiquidDensities.Contains(
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
			const FVector WorldReferenceSurface =
				GetActorTransform().TransformPosition(FVector(
					LocalCellCenter.X,
					LocalCellCenter.Y,
					static_cast<double>(State.SupportHeight)
						+ MaterialLiquidColumnHeight
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
						/ MaterialLiquidColumnHeight),
				0,
				static_cast<int32>(State.ReferenceAmount));
			if (OccupiedAmount <= 0)
			{
				continue;
			}
			State.MaximumRemainingAmount = FMath::Min(
				State.MaximumRemainingAmount,
				static_cast<uint8>(
					State.ReferenceAmount - OccupiedAmount));
			PendingMaterialDisplacementCells.Add(Cell, State);
			Constraints.Add({ Cell, State.MaximumRemainingAmount });
		}
	}
	// Buoyancy components tick before the playable world and submit into the
	// same sparse footprint map. Defer their solve so every character, creature,
	// and rigid body is handled by one merged material transaction. Direct
	// callers keep the immediate behavior used by tools and focused tests.
	if (!bDeferMaterialSolve
		&& MaterialSimulation->DisplaceLiquids(
			Constraints,
			BodyLiquidDisplacementSearchRadiusCells) > 0)
	{
		bMaterialVisualizationDirty = true;
	}
	return !Constraints.IsEmpty();
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

bool AMatterFluxPlayableWorldActor::IgniteLogicalFragmentAggregate(
	const FGuid& AggregateId,
	const FVector& WorldLocation,
	const FName FlameMaterial,
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
		if (Source && IgniteLogicalFragmentSource(
			*Source,
			WorldLocation,
			FlameMaterial,
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
			&& Source->GetComponentsBoundingBox(true).Intersect(Bounds))
		{
			OutSources.Add(Source);
		}
	}
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->FlushPendingChanges();
	}
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

int32 AMatterFluxPlayableWorldActor::
	MaterializeFragmentSourcesForFlame(
		const FVector& Start,
		const FVector& Direction,
		const float Range,
		const float StartRadius,
		const float EndRadius,
		const FName FlameMaterial)
{
	if (!HasAuthority()
		|| Start.ContainsNaN()
		|| Direction.ContainsNaN()
		|| !FMath::IsFinite(Range)
		|| !FMath::IsFinite(StartRadius)
		|| !FMath::IsFinite(EndRadius)
		|| Range <= 0.0f
		|| StartRadius <= 0.0f
		|| EndRadius < StartRadius
		|| FlameMaterial.IsNone())
	{
		return 0;
	}
	const FVector NormalizedDirection = Direction.GetSafeNormal();
	if (NormalizedDirection.IsNearlyZero())
	{
		return 0;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (!Registry.IsValid())
	{
		return 0;
	}
	TSet<FName> FuelMaterials;
	for (const TPair<FName, FMatterFluxReactionDefinition>& Pair
		: Registry->Reactions)
	{
		if (Pair.Value.Kind
				== FMatterFluxReactionDefinition::EKind::Propagating
			&& Pair.Value.InputB == FlameMaterial)
		{
			FuelMaterials.Add(Pair.Value.InputA);
		}
	}
	if (FuelMaterials.IsEmpty())
	{
		return 0;
	}

	int32 MaterializedCount = 0;
	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>
		Candidates;
	GatherLogicalFragmentSourceCandidates(
		BuildConeWorldBounds(
			Start,
			NormalizedDirection,
			Range,
			StartRadius,
			EndRadius),
		Candidates);
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
		: Candidates)
	{
		if (!Source
			|| !FuelMaterials.Contains(Source->MaterialId)
			|| RemovedFragmentSourceIds.Contains(Source->SourceId)
			|| GeneratedFragmentSources.Contains(Source->SourceId)
			|| !Source->Mask.IsValid())
		{
			continue;
		}
		const FBox SourceBounds = BuildFragmentSourceLocalBounds(*Source)
			.TransformBy(GetActorTransform().ToMatrixWithScale());
		const float Along = FVector::DotProduct(
			SourceBounds.GetCenter() - Start,
			NormalizedDirection);
		if (Along < 0.0f || Along > Range)
		{
			continue;
		}
		const FVector CenterlinePoint =
			Start + NormalizedDirection * Along;
		const float Radius = FMath::Lerp(
			StartRadius,
			EndRadius,
			Along / Range);
		if (SourceBounds.ComputeSquaredDistanceToPoint(CenterlinePoint)
			> FMath::Square(Radius))
		{
			continue;
		}
		SpawnFragmentSource(*Source);
		MaterializedCount +=
			GeneratedFragmentSources.Contains(Source->SourceId)
				? 1
				: 0;
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
	if (SourceState.bHasCombustionState)
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
		ActiveSourceCombustions.Remove(SourceId);
		LogicalSourceCombustionIndex.Remove(SourceId);
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
			Existing && Existing->bHasCombustionState)
		{
			FName ResidueMaterial = NAME_None;
			FLinearColor ResidueColor(0.08f, 0.07f, 0.06f);
			const FMatterFluxContentRegistryPtr Registry =
				IMatterFluxScriptRuntime::IsAvailable()
					? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
					: nullptr;
			if (Registry.IsValid())
			{
				if (const FMatterFluxReactionDefinition* Rule =
					Registry->Reactions.Find(
						Existing->CombustionState.RuleId))
				{
					ResidueMaterial = Rule->OutputA;
					if (const FMatterFluxMaterialDefinition* Material =
						Registry->Materials.Find(ResidueMaterial))
					{
						ResidueColor = Material->Color;
					}
				}
			}
			CarrierActor->ApplyAggregateSourceStreamingState(
				SourceId,
				*Existing,
				ResidueMaterial,
				ResidueColor);
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
	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime>
		PreparedRuntime;
	const FMatterFluxReactionDefinition* Rule = nullptr;
	FLinearColor ResidueColor(0.08f, 0.07f, 0.06f);
	if (State.bHasCombustionState)
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		Rule = Registry.IsValid()
			? Registry->Reactions.Find(State.CombustionState.RuleId)
			: nullptr;
		if (!Rule)
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Cannot archive source %s without combustion rule %s"),
				*SourceId.ToString(),
				*State.CombustionState.RuleId.ToString());
			return false;
		}
		if (const FMatterFluxMaterialDefinition* Material =
			Registry->Materials.Find(Rule->OutputA))
		{
			ResidueColor = Material->Color;
		}
		if (State.CombustionState.BurningMask.ContainsByPredicate(
			[](const uint8 Value)
			{
				return Value != 0;
			}))
		{
			PreparedRuntime = MakeUnique<
				MatterFlux::Combustion::FSourceCombustionRuntime>();
			FString Error;
			if (!PreparedRuntime->RestoreState(
				MatterFlux::Combustion::FSourceRuntimeSettings(),
				State,
				*Rule,
				Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Cannot resume archived source %s combustion: %s"),
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
	ActiveSourceCombustions.Remove(SourceId);
	if (PreparedRuntime)
	{
		ActiveSourceCombustions.Add(SourceId, MoveTemp(PreparedRuntime));
	}
	LogicalSourceCombustionIndex.ApplySnapshot(
		SourceId,
		State.bHasCombustionState,
		State.CombustionState.BurningMask);
	if (FragmentSourceProxy)
	{
		TArray<uint8> ResidueMask;
		ResidueMask.Init(0, State.GetRuntimeMask().Num());
		FName ResidueMaterial = NAME_None;
		if (State.bHasCombustionState && Rule)
		{
			ResidueMask = State.CombustionState.ResidueMask;
			ResidueMaterial = Rule->OutputA;
		}
		if (FragmentSourceProxy->ApplySourceState(
			SourceId,
			State.GetRuntimeMask(),
			ResidueMask,
			ResidueMaterial,
			ResidueColor,
			ActiveSourceCombustions.Contains(SourceId))
			== EMatterFluxFragmentSourceProxyApplyResult::Invalid)
		{
			return false;
		}
		FragmentSourceProxy->SetSourceMaterialized(SourceId, false);
	}
	bSourceCombustionVisualDirty |= State.bHasCombustionState;
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

bool AMatterFluxPlayableWorldActor::IgniteLogicalFragmentSource(
	const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
	const FVector& WorldLocation,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !Source.SourceId.IsValid()
		|| !Source.Mask.IsValid()
		|| RemovedFragmentSourceIds.Contains(Source.SourceId)
		|| GeneratedFragmentSources.Contains(Source.SourceId)
		|| WorldLocation.ContainsNaN()
		|| FlameMaterial.IsNone())
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
			*Registry, Source.MaterialId, FlameMaterial);
	if (!Rule)
	{
		return false;
	}

	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime> Candidate;
	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime>*
		ExistingRuntime = ActiveSourceCombustions.Find(Source.SourceId);
	MatterFlux::Combustion::FSourceCombustionRuntime* Runtime =
		ExistingRuntime ? ExistingRuntime->Get() : nullptr;
	if (!Runtime)
	{
		Candidate = MakeUnique<
			MatterFlux::Combustion::FSourceCombustionRuntime>();
		FString Error;
		const FFragment2DSourceStreamingState* Existing =
			StreamedFragmentSourceStates.Find(Source.SourceId);
		if (Existing && Existing->bHasCombustionState)
		{
			if (!Candidate->RestoreState(
				MatterFlux::Combustion::FSourceRuntimeSettings(),
				*Existing,
				*Rule,
				Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Logical source %s combustion restore failed: %s"),
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
				MatterFlux::Combustion::FSourceRuntimeSettings(),
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
	if (!Runtime->IgniteNearest(RequestedCell, FlameMaterial))
	{
		return false;
	}
	if (Candidate)
	{
		ActiveSourceCombustions.Add(
			Source.SourceId,
			MoveTemp(Candidate));
		Runtime = ActiveSourceCombustions.FindChecked(Source.SourceId).Get();
	}
	SynchronizeLogicalSourceCombustionState(
		Source.SourceId,
		Source,
		*Runtime,
		true);
	bSourceCombustionVisualDirty = true;
	return true;
}

bool AMatterFluxPlayableWorldActor::IgniteDynamicAggregateSource(
	AFragment2DActor& CarrierActor,
	const FGuid& SourceId,
	const FVector& WorldLocation,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	const TWeakObjectPtr<AFragment2DActor>* RegisteredCarrier =
		DynamicAggregateCarriers.Find(SourceId);
	if (!HasAuthority()
		|| CarrierActor.GetWorld() != GetWorld()
		|| !RegisteredCarrier
		|| RegisteredCarrier->Get() != &CarrierActor
		|| WorldLocation.ContainsNaN()
		|| FlameMaterial.IsNone())
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
			&& CandidateRule->InputB == FlameMaterial)
		{
			Rule = CandidateRule;
			break;
		}
	}
	if (!Rule)
	{
		return false;
	}

	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime> Candidate;
	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime>* RuntimePtr =
		ActiveSourceCombustions.Find(SourceId);
	MatterFlux::Combustion::FSourceCombustionRuntime* Runtime =
		RuntimePtr ? RuntimePtr->Get() : nullptr;
	if (!Runtime)
	{
		Candidate = MakeUnique<
			MatterFlux::Combustion::FSourceCombustionRuntime>();
		FString Error;
		if (CarrierState.bHasCombustionState)
		{
			MatterFlux::Combustion::FSourceRuntimeSnapshot Snapshot;
			Snapshot.CombustionState.RuleId = CarrierState.CombustionRuleId;
			Snapshot.CombustionState.Width = CarrierState.SourceMask.Width;
			Snapshot.CombustionState.Height = CarrierState.SourceMask.Height;
			Snapshot.CombustionState.Seed = CarrierState.CombustionSeed;
			Snapshot.CombustionState.Tick = CarrierState.CombustionTick;
			Snapshot.CombustionState.FuelMask =
				CarrierState.SourceMask.SolidMask;
			Snapshot.CombustionState.ResidueMask =
				CarrierState.ResidueMask.SolidMask;
			Snapshot.CombustionState.BurningMask =
				CarrierState.BurningMask.SolidMask;
			Snapshot.CombustionAccumulator =
				CarrierState.CombustionAccumulator;
			Snapshot.TotalSmokeEmissionCount =
				CarrierState.TotalSmokeEmissionCount;
			if (!Candidate->RestoreState(
				MatterFlux::Combustion::FSourceRuntimeSettings(),
				Snapshot,
				*Rule,
				Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Dynamic aggregate source %s combustion restore failed: %s"),
					*SourceId.ToString(),
					*Error);
				return false;
			}
		}
		else if (!Candidate->Initialize(
			MatterFlux::Combustion::FSourceRuntimeSettings(),
			CarrierState.SourceMask,
			*Rule,
			EventSeed,
			Error))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Dynamic aggregate source %s combustion initialization failed: %s"),
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
	if (!Runtime->IgniteNearest(RequestedCell, FlameMaterial))
	{
		return false;
	}
	if (Candidate)
	{
		ActiveSourceCombustions.Add(SourceId, MoveTemp(Candidate));
		Runtime = ActiveSourceCombustions.FindChecked(SourceId).Get();
	}
	if (!SynchronizeLogicalSourceCombustionState(
		SourceId,
		*Source,
		*Runtime,
		true))
	{
		return false;
	}
	bSourceCombustionVisualDirty = true;
	return true;
}

bool AMatterFluxPlayableWorldActor::
	SynchronizeLogicalSourceCombustionState(
		const FGuid& SourceId,
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const MatterFlux::Combustion::FSourceCombustionRuntime& Runtime,
		const bool bPublish)
{
	FFragment2DSourceStreamingState& State =
		StreamedFragmentSourceStates.FindOrAdd(SourceId);
	if (!State.CaptureCombustionState(Runtime))
	{
		return false;
	}
	State.Revision = FMath::Max(State.Revision, 0);
	LogicalSourceCombustionIndex.ApplySnapshot(
		SourceId,
		State.bHasCombustionState,
		State.CombustionState.BurningMask);
	const FName ResidueMaterial = Runtime.GetRule()
		? Runtime.GetRule()->OutputA
		: NAME_None;
	FLinearColor ResidueColor(0.08f, 0.07f, 0.06f);
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (Registry.IsValid())
	{
		if (const FMatterFluxMaterialDefinition* Material =
			Registry->Materials.Find(ResidueMaterial))
		{
			ResidueColor = Material->Color;
		}
	}
	if (FragmentSourceProxy)
	{
		if (FragmentSourceProxy->ApplySourceState(
			SourceId,
			State.GetRuntimeMask(),
			State.CombustionState.ResidueMask,
			ResidueMaterial,
			ResidueColor,
			Runtime.IsBurning())
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
				ResidueMaterial,
				ResidueColor))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Dynamic aggregate rejected combustion update for source %s"),
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
			TEXT("Logical source %s combustion state could not be published"),
			*SourceId.ToString());
		return false;
	}
	return true;
}

int32 AMatterFluxPlayableWorldActor::IgniteLogicalFragmentSourcesInCone(
	const FVector& Start,
	const FVector& Direction,
	const float Range,
	const float StartRadius,
	const float EndRadius,
	const FName FlameMaterial,
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
	int32 Ignited = 0;
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
			|| ActiveSourceCombustions.Contains(Source->SourceId)
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
			&& IgniteLogicalFragmentSource(
				*Source,
				SourceBounds.GetClosestPointTo(Centerline),
				FlameMaterial,
				EventSeed ^ static_cast<int32>(GetTypeHash(Source->SourceId))))
		{
			++Ignited;
		}
	}
	return Ignited;
}

int32 AMatterFluxPlayableWorldActor::IgniteLogicalFragmentSourcesInBounds(
	const FBox& Bounds,
	const FVector& IgnitionPoint,
	const FName FlameMaterial,
	const int32 EventSeed,
	const int32 MaxIgnitions)
{
	if (!HasAuthority()
		|| !Bounds.IsValid
		|| IgnitionPoint.ContainsNaN()
		|| MaxIgnitions <= 0)
	{
		return 0;
	}
	int32 Ignited = 0;
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
		if (Ignited >= MaxIgnitions)
		{
			break;
		}
		if (!Source
			|| GeneratedFragmentSources.Contains(Source->SourceId)
			|| ActiveSourceCombustions.Contains(Source->SourceId)
			|| RemovedFragmentSourceIds.Contains(Source->SourceId))
		{
			continue;
		}
		const FBox SourceBounds = BuildFragmentSourceLocalBounds(*Source)
			.TransformBy(GetActorTransform().ToMatrixWithScale());
		if (SourceBounds.Intersect(Bounds)
			&& IgniteLogicalFragmentSource(
			*Source,
			SourceBounds.GetClosestPointTo(IgnitionPoint),
			FlameMaterial,
			EventSeed ^ static_cast<int32>(GetTypeHash(Source->SourceId))))
		{
			++Ignited;
		}
	}
	return Ignited;
}

bool AMatterFluxPlayableWorldActor::IgniteFirstGeneratedTree(
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
	FName FlameMaterial = TEXT("fire");
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
			FlameMaterial = Rule->InputB;
		}
	}
	const FVector LocalIgnitionPoint(
		0.0f,
		0.0f,
		(-static_cast<float>(BestLogicalTree->Mask.Height) * 0.5f
			+ 0.5f)
			* BestLogicalTree->Mask.CellSize);
	const FVector WorldIgnitionPoint =
		(BestLogicalTree->Transform * GetActorTransform()).TransformPosition(
			LocalIgnitionPoint);
	const bool bIgnited = IgniteLogicalFragmentSource(
		*BestLogicalTree,
		WorldIgnitionPoint,
		FlameMaterial,
		EventSeed);
	if (bIgnited)
	{
		IgniteGroundAtWorldLocation(
			WorldIgnitionPoint,
			EventSeed ^ 0x47524f55);
	}
	return bIgnited;
}

int32 AMatterFluxPlayableWorldActor::GetCombustingSourceCount() const
{
	int32 Count = LogicalSourceCombustionIndex.Num();
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		Count += IsValid(Pair.Value)
			&& Pair.Value->IsCombusting()
				? 1
				: 0;
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			Count += It->IsRootCombusting() ? 1 : 0;
		}
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::GetCombustionResidueCellCount() const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		if (!GeneratedFragmentSources.Contains(Pair.Key)
			&& Pair.Value.bHasCombustionState)
		{
			for (const uint8 Value : Pair.Value.CombustionState.ResidueMask)
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
			Count += Pair.Value->GetResidueCellCount();
		}
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			Count += It->GetRootCombustionResidueCellCount();
		}
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::
	GetLogicalCombustionResidueCellCount(const FName MaterialId) const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
			FindFragmentSourceDefinition(Pair.Key);
		if (!Source
			|| Source->MaterialId != MaterialId
			|| !Pair.Value.bHasCombustionState)
		{
			continue;
		}
		for (const uint8 Value : Pair.Value.CombustionState.ResidueMask)
		{
			Count += Value != 0 ? 1 : 0;
		}
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::
	GetLogicalCombustionFuelCellCount(const FName MaterialId) const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
			FindFragmentSourceDefinition(Pair.Key);
		if (!Source
			|| Source->MaterialId != MaterialId
			|| !Pair.Value.bHasCombustionState)
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
	GetLogicalCombustionSmokeEmissionCount() const
{
	int32 Count = 0;
	for (const TPair<FGuid, FFragment2DSourceStreamingState>& Pair
		: StreamedFragmentSourceStates)
	{
		Count += Pair.Value.bHasCombustionState
			? Pair.Value.TotalSmokeEmissionCount
			: 0;
	}
	return Count;
}

int32 AMatterFluxPlayableWorldActor::GetScorchedGroundCellCount() const
{
	return GroundCombustion
		? GroundCombustion->CountResidueCells()
		: 0;
}

void AMatterFluxPlayableWorldActor::InitializeGroundCombustion(
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

	auto NextGroundCombustion = MakeUnique<
		MatterFlux::Combustion::FGroundCombustionRuntime>();
	FString RuntimeError;
	if (!NextGroundCombustion->Initialize(
		MakeGroundCombustionRuntimeSettings(),
		GroundMask,
		*GrassRule,
		MapSeed ^ 0x47524153,
		RuntimeError))
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Ground combustion runtime initialization failed: %s"),
			*RuntimeError);
		return;
	}
	if (HasAuthority())
	{
		DestroyGroundStateChunks();
	}
	GroundCombustion = MoveTemp(NextGroundCombustion);
	GroundSurfacePositions = MoveTemp(NextGroundSurfacePositions);
	SourcesThatIgnitedGround.Reset();
	GroundCombustionVisualAccumulator = 0.0f;
	bGroundCombustionVisualDirty = true;
	bGroundCombustionVisualNeedsFullRebuild = true;
	PendingGroundCombustionVisualCellIndices.Reset();
	if (HasAuthority())
	{
		InitializeGroundStateChunks();
	}
	EnsureGroundCombustionVisuals(Registry);
}

bool AMatterFluxPlayableWorldActor::IgniteGroundAtWorldLocation(
	const FVector& WorldLocation,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !GroundCombustion
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
	const FMatterFluxReactionDefinition* Rule =
		GroundCombustion->GetRule();
	if (!Rule)
	{
		return false;
	}
	const FName Flame = Rule->InputB;
	FIntPoint IgnitedCell = Requested;
	bool bIgnited = GroundCombustion->Ignite(Requested, Flame);
	for (int32 Radius = 1; !bIgnited && Radius <= 5; ++Radius)
	{
		for (int32 Y = -Radius; Y <= Radius && !bIgnited; ++Y)
		{
			for (int32 X = -Radius; X <= Radius; ++X)
			{
				if (FMath::Max(FMath::Abs(X), FMath::Abs(Y))
					!= Radius)
				{
					continue;
				}
				const FIntPoint Candidate =
					Requested + FIntPoint(X, Y);
				bIgnited = GroundCombustion->Ignite(Candidate, Flame);
				if (bIgnited)
				{
					IgnitedCell = Candidate;
					break;
				}
			}
		}
	}
	if (bIgnited)
	{
		PendingGroundCombustionVisualCellIndices.Add(
			IgnitedCell.Y * MatterFlux::PlayableLevel::TerrainCellsX
				+ IgnitedCell.X);
		bGroundCombustionVisualDirty = true;
		if (!bBatchingGroundIgnitions)
		{
			PublishGroundCombustionState();
		}
	}
	(void)EventSeed;
	return bIgnited;
}

void AMatterFluxPlayableWorldActor::AdvanceGroundCombustion(
	const float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxCombustion);
	const float ClampedDelta =
		FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (HasAuthority() && GroundCombustion)
	{
		SCOPE_CYCLE_COUNTER(STAT_MatterFluxGroundCombustionSimulation);
		const MatterFlux::Combustion::FGroundAdvanceResult Result =
			GroundCombustion->AdvanceAuthority(ClampedDelta);
		if (Result.bStateChanged)
		{
			for (const int32 CellIndex : Result.ChangedCellIndices)
			{
				PendingGroundCombustionVisualCellIndices.Add(CellIndex);
			}
			bGroundCombustionVisualDirty = true;
			PublishGroundCombustionState();
		}
	}
	GroundCombustionVisualAccumulator += ClampedDelta;
	if (bGroundCombustionVisualDirty
		&& GroundCombustionVisualAccumulator >= 0.2f)
	{
		GroundCombustionVisualAccumulator = 0.0f;
		if (bGroundCombustionVisualNeedsFullRebuild)
		{
			RebuildGroundCombustionVisualization();
		}
		else
		{
			TArray<int32> ChangedCellIndices;
			ChangedCellIndices.Reserve(
				PendingGroundCombustionVisualCellIndices.Num());
			for (const int32 CellIndex
				: PendingGroundCombustionVisualCellIndices)
			{
				ChangedCellIndices.Add(CellIndex);
			}
			ChangedCellIndices.Sort();
			ApplyGroundCombustionVisualChanges(ChangedCellIndices);
		}
	}
}

void AMatterFluxPlayableWorldActor::EnsureGroundCombustionVisuals(
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
		GroundResidueInstances,
		TEXT("GroundCombustionResidue"));
	CreateLayer(
		GroundFlameInstances,
		TEXT("GroundCombustionFlames"));
	CreateLayer(
		GroundSmokeInstances,
		TEXT("GroundCombustionSmoke"));
	// Smoke is rendered by the shared world pool. Keep this legacy component
	// empty so old serialized worlds cannot display one static grey cube per cell.
	GroundSmokeInstances->SetVisibility(false);
	GroundSmokeInstances->ClearInstances();

	const FMatterFluxReactionDefinition* Rule =
		GroundCombustion
			? GroundCombustion->GetRule()
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
		GroundResidueInstances,
		GroundResidueMaterial,
		Rule->OutputA,
		FLinearColor(0.24f, 0.23f, 0.21f));
	ApplyMaterial(
		GroundFlameInstances,
		GroundFlameMaterial,
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
	RebuildGroundCombustionVisualization()
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxGroundCombustionVisuals);
	if (GetNetMode() == NM_DedicatedServer)
	{
		bGroundCombustionVisualDirty = false;
		bGroundCombustionVisualNeedsFullRebuild = false;
		PendingGroundCombustionVisualCellIndices.Reset();
		return;
	}
	if (!GroundResidueInstances
		|| !GroundFlameInstances
		|| !GroundSmokeInstances
		|| !GroundCombustion
		|| GroundSurfacePositions.Num()
			!= GroundCombustion->GetResidueMask().Num()
		|| GroundSurfacePositions.Num()
			!= GroundCombustion->GetBurningMask().Num())
	{
		return;
	}
	TArray<FTransform> ResidueTransforms;
	TArray<FTransform> FlameTransforms;
	TArray<int32> ResidueCellIndices;
	TArray<int32> BurningCellIndices;
	TArray<int32> ResidueVisualCells;
	TArray<int32> FlameVisualCells;
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
	GroundCombustion->GatherVisibleCellIndicesForChunks(
		VisibleRuntimeChunks,
		ResidueCellIndices,
		BurningCellIndices);
	ResidueTransforms.Reserve(ResidueCellIndices.Num());
	FlameTransforms.Reserve(FMath::Min(BurningCellIndices.Num(), 256));
	for (const int32 CellIndex : ResidueCellIndices)
	{
		if (!GroundSurfacePositions.IsValidIndex(CellIndex))
		{
			continue;
		}
		ResidueTransforms.Emplace(
			FRotator::ZeroRotator,
			GroundSurfacePositions[CellIndex] + FVector(0.0f, 0.0f, 3.0f),
			FVector(0.61f, 0.61f, 0.06f));
		ResidueVisualCells.Add(CellIndex);
	}
	for (const int32 CellIndex : BurningCellIndices)
	{
		if (!GroundSurfacePositions.IsValidIndex(CellIndex))
		{
			continue;
		}
		const FVector Surface = GroundSurfacePositions[CellIndex];
		const float Jitter =
			static_cast<float>((CellIndex * 37) % 11) - 5.0f;
		if (FlameTransforms.Num() < 256)
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
		GroundResidueInstances,
		ResidueTransforms);
	ReplaceInstances(
		GroundFlameInstances,
		FlameTransforms);
	ReplaceInstances(
		GroundSmokeInstances,
		TArray<FTransform>());
	GroundResidueCellByInstance = MoveTemp(ResidueVisualCells);
	GroundFlameCellByInstance = MoveTemp(FlameVisualCells);
	GroundSmokeCellByInstance.Reset();
	GroundResidueInstanceByCell.Reset();
	GroundFlameInstanceByCell.Reset();
	GroundSmokeInstanceByCell.Reset();
	for (int32 InstanceIndex = 0;
		InstanceIndex < GroundResidueCellByInstance.Num();
		++InstanceIndex)
	{
		GroundResidueInstanceByCell.Add(
			GroundResidueCellByInstance[InstanceIndex],
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
	bGroundCombustionVisualDirty = false;
	bGroundCombustionVisualNeedsFullRebuild = false;
	PendingGroundCombustionVisualCellIndices.Reset();
	RebuildGroundSmokeAnchors();
}

bool AMatterFluxPlayableWorldActor::IsGroundCombustionCellVisible(
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

void AMatterFluxPlayableWorldActor::ApplyGroundCombustionVisualChanges(
	const TConstArrayView<int32> ChangedCellIndices)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxGroundCombustionVisuals);
	if (!GroundResidueInstances
		|| !GroundFlameInstances
		|| !GroundSmokeInstances
		|| !GroundCombustion)
	{
		bGroundCombustionVisualNeedsFullRebuild = true;
		return;
	}
	const TArray<uint8>& ResidueMask = GroundCombustion->GetResidueMask();
	const TArray<uint8>& BurningMask = GroundCombustion->GetBurningMask();
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
				bGroundCombustionVisualNeedsFullRebuild = true;
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
				bGroundCombustionVisualNeedsFullRebuild = true;
				return;
			}
			InstanceByCell.Add(CellIndex, InstanceIndex);
			CellByInstance.Add(CellIndex);
		};

	// Removals run before additions so expired particles immediately free the
	// fixed flame/smoke visual budget for newly burning cells.
	for (const int32 CellIndex : ChangedCellIndices)
	{
		const bool bVisible = IsGroundCombustionCellVisible(CellIndex);
		const bool bHasResidue = bVisible
			&& ResidueMask.IsValidIndex(CellIndex)
			&& ResidueMask[CellIndex] != 0;
		const bool bIsBurning = bVisible
			&& BurningMask.IsValidIndex(CellIndex)
			&& BurningMask[CellIndex] != 0;
		if (!bHasResidue)
		{
			RemoveCellInstance(
				*GroundResidueInstances,
				GroundResidueInstanceByCell,
				GroundResidueCellByInstance,
				CellIndex);
		}
		if (!bIsBurning)
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
		if (!IsGroundCombustionCellVisible(CellIndex)
			|| !GroundSurfacePositions.IsValidIndex(CellIndex)
			|| !ResidueMask.IsValidIndex(CellIndex)
			|| ResidueMask[CellIndex] == 0)
		{
			continue;
		}
		AddCellInstance(
			*GroundResidueInstances,
			GroundResidueInstanceByCell,
			GroundResidueCellByInstance,
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
	TArray<int32> VisibleResidueCells;
	TArray<int32> VisibleBurningCells;
	GroundCombustion->GatherVisibleCellIndicesForChunks(
		VisibleRuntimeChunks,
		VisibleResidueCells,
		VisibleBurningCells);
	for (const int32 CellIndex : VisibleBurningCells)
	{
		if (!GroundSurfacePositions.IsValidIndex(CellIndex))
		{
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
	PendingGroundCombustionVisualCellIndices.Reset();
	bGroundCombustionVisualDirty =
		bGroundCombustionVisualNeedsFullRebuild;
	RebuildGroundSmokeAnchors();
}

int32 AMatterFluxPlayableWorldActor::
	GetReplicatedGroundCombustionByteCount() const
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
	if (HasAuthority() || !GroundCombustion)
	{
		return false;
	}
	FString Error;
	const MatterFlux::Combustion::EGroundChunkApplyResult Result =
		GroundCombustion->ApplyReplicatedChunk(State, Error);
	if (Result == MatterFlux::Combustion::
		EGroundChunkApplyResult::Rejected)
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Rejected replicated ground chunk (%d,%d): %s"),
			State.ChunkCoordinate.X,
			State.ChunkCoordinate.Y,
			*Error);
		return false;
	}
	if (Result == MatterFlux::Combustion::
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
		bGroundCombustionVisualDirty = true;
		bGroundCombustionVisualNeedsFullRebuild = true;
		PendingGroundCombustionVisualCellIndices.Reset();
	}
	return true;
}

void AMatterFluxPlayableWorldActor::InitializeGroundStateChunks()
{
	if (!HasAuthority() || !GroundCombustion || !GetWorld())
	{
		return;
	}
	TArray<FMatterFluxGroundStateChunk> InitialStates;
	FString Error;
	if (!GroundCombustion->BuildInitialReplication(InitialStates, Error))
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

void AMatterFluxPlayableWorldActor::PublishGroundCombustionState()
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxGroundCombustionReplication);
	if (!HasAuthority() || !GroundCombustion
		|| !GroundCombustion->HasPendingReplication())
	{
		return;
	}
	TArray<FMatterFluxGroundStateChunk> Batch;
	FString Error;
	if (!GroundCombustion->BuildPendingReplication(Batch, Error))
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

void AMatterFluxPlayableWorldActor::AdvanceLogicalSourceCombustion(
	const float DeltaSeconds)
{
	const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (HasAuthority())
	{
		SourceCombustionActiveIdsScratch.Reset();
		SourceCombustionFinishedIdsScratch.Reset();
		SourceCombustionPublishIdsScratch.Reset();
		LogicalSourceCombustionIndex.GatherStableIds(
			SourceCombustionActiveIdsScratch);
		SourceCombustionFinishedIdsScratch.Reserve(
			SourceCombustionActiveIdsScratch.Num());
		SourceCombustionPublishIdsScratch.Reserve(
			SourceCombustionActiveIdsScratch.Num());
		for (const FGuid& SourceId : SourceCombustionActiveIdsScratch)
		{
			TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime>*
				RuntimePtr = ActiveSourceCombustions.Find(SourceId);
			const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
				FindFragmentSourceDefinition(SourceId);
			if (!RuntimePtr || !RuntimePtr->IsValid() || !Source)
			{
				SourceCombustionFinishedIdsScratch.Add(SourceId);
				continue;
			}
			MatterFlux::Combustion::FSourceCombustionRuntime& Runtime =
				*RuntimePtr->Get();
			const MatterFlux::Combustion::FSourceAdvanceResult Result =
				Runtime.AdvanceAuthority(ClampedDelta);
			if (Result.Steps > 0)
			{
				if (SynchronizeLogicalSourceCombustionState(
					SourceId,
					*Source,
					Runtime,
					false))
				{
					SourceCombustionPublishIdsScratch.Add(SourceId);
				}
				bSourceCombustionVisualDirty = true;
			}
			if (!Runtime.IsBurning())
			{
				SourceCombustionFinishedIdsScratch.Add(SourceId);
			}
		}
		if (!SourceCombustionPublishIdsScratch.IsEmpty()
			&& !PublishFragmentSourceStateBatch(
				SourceCombustionPublishIdsScratch))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Logical Source combustion batch of %d states could not be published"),
				SourceCombustionPublishIdsScratch.Num());
		}
		for (const FGuid& SourceId : SourceCombustionFinishedIdsScratch)
		{
			ActiveSourceCombustions.Remove(SourceId);
			LogicalSourceCombustionIndex.Remove(SourceId);
		}
	}

	SourceCombustionVisualAccumulator += ClampedDelta;
	if (bSourceCombustionVisualDirty
		&& SourceCombustionVisualAccumulator >= 0.1f)
	{
		SourceCombustionVisualAccumulator = 0.0f;
		RebuildLogicalSourceCombustionVisualization();
	}
}

void AMatterFluxPlayableWorldActor::
	RebuildLogicalSourceCombustionVisualization()
{
	if (GetNetMode() == NM_DedicatedServer || !SceneRoot || !CubeMesh)
	{
		bSourceCombustionVisualDirty = false;
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
	TArray<FGuid> ActiveVisualIds;
	LogicalSourceCombustionIndex.GatherStableIds(ActiveVisualIds);
	for (const FGuid& SourceId : ActiveVisualIds)
	{
		const FFragment2DSourceStreamingState* State =
			StreamedFragmentSourceStates.Find(SourceId);
		if (!State || !Registry.IsValid())
		{
			continue;
		}
		if (const FMatterFluxReactionDefinition* Rule =
			Registry->Reactions.Find(State->CombustionState.RuleId))
		{
			if (const FMatterFluxMaterialDefinition* Material =
				Registry->Materials.Find(Rule->InputB))
			{
				FlameColor = Material->Color;
			}
			if (const FMatterFluxMaterialDefinition* Material =
				Registry->Materials.Find(Rule->EmissionMaterial))
			{
				SmokeColor = Material->Color;
			}
			break;
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
		SourceFlameMaterial,
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
		const TArray<uint8>& Burning = State->CombustionState.BurningMask;
		float SmokeProbability = 0.7f;
		if (Registry.IsValid())
		{
			if (const FMatterFluxReactionDefinition* Rule =
				Registry->Reactions.Find(State->CombustionState.RuleId))
			{
				SmokeProbability = FMath::Clamp(
					static_cast<float>(Rule->EmissionChancePermille) / 1000.0f,
					0.0f,
					1.0f);
			}
		}
		TArray<int32, TInlineAllocator<64>> VisibleBurningCells;
		for (int32 Index = 0; Index < Burning.Num(); ++Index)
		{
			if (Burning[Index] != 0)
			{
				VisibleBurningCells.Add(Index);
			}
		}
		TSet<int32> SmokeSourceCells;
		if (Source->Mask.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks)
		{
			TArray<uint8, TInlineAllocator<256>> Occupied;
			Occupied.Append(State->GetRuntimeMask());
			if (State->CombustionState.ResidueMask.Num() == Occupied.Num())
			{
				for (int32 Index = 0; Index < Occupied.Num(); ++Index)
				{
					Occupied[Index] = Occupied[Index] != 0
						|| State->CombustionState.ResidueMask[Index] != 0;
				}
			}
			TArray<uint8> OccupiedArray(Occupied);
			TArray<int32> SurfaceCells;
			MatterFlux::FragmentGeometry::GatherTopExposedBurningMaskCells(
				OccupiedArray,
				Burning,
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
			for (const int32 VisibleBurningCell : VisibleBurningCells)
			{
				SmokeSourceCells.Add(VisibleBurningCell);
			}
		}
		for (const int32 Index : VisibleBurningCells)
		{
			if (FlameTransforms.Num() >= MaxVisualInstances)
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
			FlameTransforms.Emplace(
				VisualTransform.Rotator(),
				Position,
				FlameScale);
			if (SmokeSourceCells.Contains(Index)
				&& SourceSmokeAnchors.Num() < MaxVisualInstances)
			{
				MatterFlux::Rendering::FSmokeEmissionAnchor& Anchor =
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
			It->GatherRootCombustionVisualTransforms(
				FlameTransforms,
				SourceSmokeAnchors,
				MaxVisualInstances);
		}
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			It->GatherCombustionSmokeAnchors(
				SourceSmokeAnchors,
				MaxVisualInstances);
		}
	}
	MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
		*SourceFlameInstances,
		FlameTransforms);
	RefreshUnifiedSmokeAnchors();
	bSourceCombustionVisualDirty = false;
}

void AMatterFluxPlayableWorldActor::RefreshUnifiedSmokeAnchors()
{
	TArray<MatterFlux::Rendering::FSmokeEmissionAnchor> CombinedAnchors;
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
	if (!GroundCombustion)
	{
		RefreshUnifiedSmokeAnchors();
		return;
	}
	const FMatterFluxReactionDefinition* Rule = GroundCombustion->GetRule();
	const float SmokeProbability = Rule
		? FMath::Clamp(
			static_cast<float>(Rule->EmissionChancePermille) / 1000.0f,
			0.0f,
			1.0f)
		: 0.42f;
	const float VisualCellSize = FMath::Max(
		TerrainHeightField.CellSize,
		12.0f);
	GroundSmokeAnchors.Reserve(GroundFlameCellByInstance.Num());
	for (const int32 CellIndex : GroundFlameCellByInstance)
	{
		if (!GroundSurfacePositions.IsValidIndex(CellIndex))
		{
			continue;
		}
		MatterFlux::Rendering::FSmokeEmissionAnchor& Anchor =
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

void AMatterFluxPlayableWorldActor::PropagateCombustion(
	const float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxCombustion);
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxCombustionPropagation);
	if (!HasAuthority())
	{
		return;
	}
	CombustionPropagationAccumulator +=
		FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (CombustionPropagationAccumulator < 0.2f)
	{
		return;
	}
	CombustionPropagationAccumulator =
		FMath::Fmod(
			CombustionPropagationAccumulator,
			0.2f);

	TArray<FGuid> LogicalBurningIds;
	LogicalSourceCombustionIndex.GatherStableIds(LogicalBurningIds);
	TSet<FGuid> PropagatedAggregateIds;
	bBatchingGroundIgnitions = true;
	for (const FGuid& BurningId : LogicalBurningIds)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* BurningSource =
			FindFragmentSourceDefinition(BurningId);
		const FFragment2DSourceStreamingState* BurningState =
			StreamedFragmentSourceStates.Find(BurningId);
		if (!BurningSource
			|| !BurningState
			|| !BurningState->bHasCombustionState)
		{
			continue;
		}
		FBox FireBounds(ForceInit);
		TArray<FVector, TInlineAllocator<64>> BurningCellCenters;
		const TArray<uint8>& BurningMask =
			BurningState->CombustionState.BurningMask;
		FTransform SourceWorldTransform =
			BurningSource->Transform * GetActorTransform();
		AFragment2DActor* DynamicCarrier = nullptr;
		if (const TWeakObjectPtr<AFragment2DActor>* CarrierPtr =
			DynamicAggregateCarriers.Find(BurningId))
		{
			DynamicCarrier = CarrierPtr->Get();
			if (DynamicCarrier)
			{
				DynamicCarrier->GetAggregateSourceWorldTransform(
					BurningId,
					SourceWorldTransform);
			}
		}
		for (int32 Index = 0; Index < BurningMask.Num(); ++Index)
		{
			if (BurningMask[Index] == 0)
			{
				continue;
			}
			const int32 X = Index % BurningSource->Mask.Width;
			const int32 Y = Index / BurningSource->Mask.Width;
			const FVector Center = SourceWorldTransform.TransformPosition(FVector(
				(static_cast<float>(X) + 0.5f
					- BurningSource->Mask.Width * 0.5f)
					* BurningSource->Mask.CellSize,
				0.0f,
				(static_cast<float>(Y) + 0.5f
					- BurningSource->Mask.Height * 0.5f)
					* BurningSource->Mask.CellSize));
			BurningCellCenters.Add(Center);
			FireBounds += FBox::BuildAABB(
				Center,
				FVector(BurningSource->Mask.CellSize));
		}
		if (!FireBounds.IsValid)
		{
			continue;
		}
		float GroundHeight = 0.0f;
		const bool bCanReachGround =
			TrySampleTerrainHeightAtWorldLocation(
				FireBounds.GetCenter(),
				GroundHeight)
			&& FireBounds.Min.Z
				<= GroundHeight + BurningSource->Mask.CellSize * 1.5f;
		if (bCanReachGround
			&& !SourcesThatIgnitedGround.Contains(BurningId)
			&& IgniteGroundAtWorldLocation(
				FireBounds.GetCenter(),
				MapSeed ^ GetTypeHash(BurningId)
					^ ReplicatedMaterialSimulationStep))
		{
			SourcesThatIgnitedGround.Add(BurningId);
		}
		const FBox IgnitionBounds = FireBounds.ExpandBy(
			BurningSource->Mask.CellSize * 1.5f);
		FName FlameMaterial = TEXT("fire");
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (Registry.IsValid())
		{
			if (const FMatterFluxReactionDefinition* Rule =
				Registry->Reactions.Find(
					BurningState->CombustionState.RuleId))
			{
				FlameMaterial = Rule->InputB;
			}
		}
		const bool bAggregateAlreadyPropagated =
			BurningSource->AggregateId.IsValid()
			&& PropagatedAggregateIds.Contains(BurningSource->AggregateId);
		if (bAggregateAlreadyPropagated)
		{
			continue;
		}
		if (BurningSource->AggregateId.IsValid())
		{
			PropagatedAggregateIds.Add(BurningSource->AggregateId);
		}
		const int32 PropagationSeed = MapSeed ^ GetTypeHash(BurningId)
			^ ReplicatedMaterialSimulationStep;
		struct FAggregateContact
		{
			const MatterFlux::PlayableLevel::FLevelFragmentSource* Source = nullptr;
			FVector WorldLocation = FVector::ZeroVector;
			double DistanceSquared = TNumericLimits<double>::Max();
			int32 CellIndex = INDEX_NONE;
		};
		FAggregateContact BestContact;
		if (BurningSource->AggregateId.IsValid())
		{
			for (const TPair<
				FIntPoint,
				TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
				: FragmentSourceChunks)
			{
				for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Candidate
					: Pair.Value)
				{
					if (Candidate.SourceId == BurningId
						|| Candidate.AggregateId != BurningSource->AggregateId
						|| RemovedFragmentSourceIds.Contains(Candidate.SourceId)
						|| GeneratedFragmentSources.Contains(Candidate.SourceId)
						|| ActiveSourceCombustions.Contains(Candidate.SourceId))
					{
						continue;
					}
					const FFragment2DSourceStreamingState* CandidateState =
						StreamedFragmentSourceStates.Find(Candidate.SourceId);
					const TArray<uint8>& CandidateFuel = CandidateState
						? CandidateState->GetRuntimeMask()
						: Candidate.Mask.SolidMask;
					if (CandidateFuel.Num()
						!= Candidate.Mask.Width * Candidate.Mask.Height)
					{
						continue;
					}
					FTransform CandidateWorldTransform =
						Candidate.Transform * GetActorTransform();
					if (DynamicCarrier)
					{
						DynamicCarrier->GetAggregateSourceWorldTransform(
							Candidate.SourceId,
							CandidateWorldTransform);
					}
					const double ContactDistance = FMath::Max(
						BurningSource->Mask.CellSize,
						Candidate.Mask.CellSize) * 1.05;
					const double MaximumDistanceSquared =
						FMath::Square(ContactDistance);
					for (int32 CellIndex = 0;
						CellIndex < CandidateFuel.Num();
						++CellIndex)
					{
						if (CandidateFuel[CellIndex] == 0)
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
						for (const FVector& BurningCenter : BurningCellCenters)
						{
							const double DistanceSquared = FVector::DistSquared(
								BurningCenter,
								CandidateCenter);
							if (DistanceSquared > MaximumDistanceSquared)
							{
								continue;
							}
							const bool bStableTieBreak =
								FMath::IsNearlyEqual(
									DistanceSquared,
									BestContact.DistanceSquared)
								&& (!BestContact.Source
									|| Candidate.SourceId.ToString(EGuidFormats::Digits)
										< BestContact.Source->SourceId.ToString(
											EGuidFormats::Digits)
									|| (Candidate.SourceId == BestContact.Source->SourceId
										&& CellIndex < BestContact.CellIndex));
							if (!BestContact.Source
								|| DistanceSquared < BestContact.DistanceSquared
								|| bStableTieBreak)
							{
								BestContact.Source = &Candidate;
								BestContact.WorldLocation = CandidateCenter;
								BestContact.DistanceSquared = DistanceSquared;
								BestContact.CellIndex = CellIndex;
							}
						}
					}
				}
			}
		}
		bool bIgnitedAggregateNeighbor = false;
		if (BestContact.Source)
		{
			bIgnitedAggregateNeighbor = DynamicCarrier
				? IgniteDynamicAggregateSource(
					*DynamicCarrier,
					BestContact.Source->SourceId,
					BestContact.WorldLocation,
					FlameMaterial,
					PropagationSeed)
				: IgniteLogicalFragmentSource(
					*BestContact.Source,
					BestContact.WorldLocation,
					FlameMaterial,
					PropagationSeed);
		}
		if (!bIgnitedAggregateNeighbor
			&& !BurningSource->AggregateId.IsValid())
		{
			IgniteLogicalFragmentSourcesInBounds(
				IgnitionBounds,
				IgnitionBounds.GetCenter(),
				FlameMaterial,
				PropagationSeed,
				1);
		}
	}
	bBatchingGroundIgnitions = false;
	if (GroundCombustion && GroundCombustion->HasPendingReplication())
	{
		PublishGroundCombustionState();
	}

	UFragmentSimulationSubsystem* FragmentSubsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr;
	TArray<AFragment2DSourceActor*> BurningSources;
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		AFragment2DSourceActor* Source = Pair.Value;
		if (!IsValid(Source))
		{
			continue;
		}
		if (Source->IsCombusting())
		{
			BurningSources.Add(Source);
		}
	}
	bBatchingGroundIgnitions = true;
	for (AFragment2DSourceActor* BurningSource : BurningSources)
	{
		const FBox FireBounds =
			BurningSource->GetBurningWorldBounds();
		if (!FireBounds.IsValid)
		{
			continue;
		}
		float GroundHeight = 0.0f;
		const bool bCanReachGround =
			TrySampleTerrainHeightAtWorldLocation(
				FireBounds.GetCenter(),
				GroundHeight)
			&& FireBounds.Min.Z
				<= GroundHeight + BurningSource->GetCellSize() * 1.5f;
		if (bCanReachGround
			&& !SourcesThatIgnitedGround.Contains(BurningSource->SourceId)
			&& IgniteGroundAtWorldLocation(
				FireBounds.GetCenter(),
				MapSeed
					^ GetTypeHash(BurningSource->SourceId)
					^ ReplicatedMaterialSimulationStep))
		{
			SourcesThatIgnitedGround.Add(BurningSource->SourceId);
		}
		const FBox IgnitionBounds =
			FireBounds.ExpandBy(
				BurningSource->GetCellSize() * 1.5f);
		const int32 PropagationSeed =
			MapSeed
				^ GetTypeHash(BurningSource->SourceId)
				^ ReplicatedMaterialSimulationStep;
		const int32 LogicalIgnitions = IgniteLogicalFragmentSourcesInBounds(
			IgnitionBounds,
			IgnitionBounds.GetCenter(),
			BurningSource->GetCombustionFlameMaterial(),
			PropagationSeed,
			1);
		TArray<AFragment2DSourceActor*> MaterializedCandidates;
		if (FragmentSubsystem)
		{
			FragmentSubsystem->GatherSourcesInBounds(
				IgnitionBounds,
				MaterializedCandidates);
		}
		for (AFragment2DSourceActor* Candidate : MaterializedCandidates)
		{
			if (LogicalIgnitions > 0)
			{
				break;
			}
			if (!IsValid(Candidate)
				|| Candidate == BurningSource
				|| Candidate->IsCombusting()
				|| Candidate->GetRemainingFuelCellCount() == 0)
			{
				continue;
			}
			const FBox CandidateBounds =
				Candidate->GetComponentsBoundingBox(true);
			if (!CandidateBounds.IsValid)
			{
				continue;
			}
			const FVector Contact =
				CandidateBounds.GetClosestPointTo(
					IgnitionBounds.GetCenter());
			if (Candidate->IgniteAtWorldLocation(
				Contact,
				Candidate->GetCombustionFlameMaterial(),
				PropagationSeed ^ GetTypeHash(Candidate->SourceId)))
			{
				break;
			}
		}
	}
	bBatchingGroundIgnitions = false;
	if (GroundCombustion
		&& GroundCombustion->HasPendingReplication())
	{
		PublishGroundCombustionState();
	}

	if (!GroundCombustion
		|| GroundSurfacePositions.IsEmpty()
		|| !GroundCombustion->IsBurning())
	{
		return;
	}
	const TArray<uint8>& GroundBurning =
		GroundCombustion->GetBurningMask();
	const FVector GroundOrigin = GroundSurfacePositions[0];
	TArray<FIntPoint> BurningGroundChunks;
	GroundCombustion->GatherBurningChunkCoordinates(
		BurningGroundChunks);
	TArray<FBox> LogicalQueryBounds;
	LogicalQueryBounds.Reserve(BurningGroundChunks.Num());
	const double GroundCellHalfExtent =
		MatterFlux::PlayableLevel::TerrainCellSize * 0.5
		+ UE_KINDA_SMALL_NUMBER;
	for (const FIntPoint BurningChunk : BurningGroundChunks)
	{
		const int32 StartX = BurningChunk.X * GroundCombustionChunkSize;
		const int32 StartY = BurningChunk.Y * GroundCombustionChunkSize;
		const int32 EndX = FMath::Min(
			StartX + GroundCombustionChunkSize,
			MatterFlux::PlayableLevel::TerrainCellsX);
		const int32 EndY = FMath::Min(
			StartY + GroundCombustionChunkSize,
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
	TMap<FName, FName> FlameMaterialByFuel;
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
				&& !FlameMaterialByFuel.Contains(Rule->InputA))
			{
				FlameMaterialByFuel.Add(
					Rule->InputA,
					Rule->InputB);
			}
		}
	}
	for (const FGuid& CandidateId : LogicalCandidateIds)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Candidate =
			FindFragmentSourceDefinition(CandidateId);
		if (!Candidate
			|| GeneratedFragmentSources.Contains(CandidateId)
			|| ActiveSourceCombustions.Contains(CandidateId)
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
		if (!GroundBurning.IsValidIndex(Index)
			|| GroundBurning[Index] == 0)
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
		FName FlameMaterial(TEXT("fire"));
		if (const FName* ConfiguredFlameMaterial =
			FlameMaterialByFuel.Find(Candidate->MaterialId))
		{
			if (!ConfiguredFlameMaterial->IsNone())
			{
				FlameMaterial = *ConfiguredFlameMaterial;
			}
		}
		IgniteLogicalFragmentSource(
			*Candidate,
			FVector(
				CandidateBounds.GetCenter().X,
				CandidateBounds.GetCenter().Y,
				CandidateBounds.Min.Z
					+ Candidate->Mask.CellSize * 0.5f),
			FlameMaterial,
			MapSeed ^ GetTypeHash(CandidateId)
				^ GroundCombustion->GetRevision());
	}
	for (AFragment2DSourceActor* Candidate : MaterializedGroundCandidates)
	{
		if (!IsValid(Candidate)
			|| Candidate->IsCombusting()
			|| Candidate->GetRemainingFuelCellCount() == 0
			|| !Candidate->HasCombustionRule()
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
		if (!GroundBurning.IsValidIndex(Index)
			|| GroundBurning[Index] == 0)
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
		Candidate->IgniteAtWorldLocation(
			FVector(
				CandidateBounds.GetCenter().X,
				CandidateBounds.GetCenter().Y,
				CandidateBounds.Min.Z
					+ Candidate->GetCellSize() * 0.5f),
			Candidate->GetCombustionFlameMaterial(),
			MapSeed
				^ GetTypeHash(Candidate->SourceId)
				^ GroundCombustion->GetRevision());
	}
}

void AMatterFluxPlayableWorldActor::InitializeMaterialSimulation(
	const FMatterFluxContentRegistry& Registry,
	const MatterFlux::PlayableLevel::FLevelLayout& Layout)
{
	PendingMaterialDisplacementCells.Reset();
	PreviousMaterialDisplacementCells.Reset();
	MaterialLiquidDensities.Reset();
	for (const TPair<FName, FMatterFluxMaterialDefinition>& Pair
		: Registry.Materials)
	{
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
	Settings.bCullOutsideSurfaceBounds = true;
	Settings.LiquidFullColumnHeight = FMath::RoundToInt(
		MaterialLiquidColumnHeight);
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
	TArray<TPair<int32, int32>> CellsByHeight;
	SeedCells.Reserve(Terrain.Heights.Num());
	SeedIndices.Reserve(Terrain.Heights.Num());
	CellsByHeight.Reserve(Terrain.Heights.Num());
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
			CellsByHeight.Emplace(SurfaceHeight, SeedIndex);
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
				? FMath::Max(SeedCell.Amount, AuthoredAmount)
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
				? FMath::Max(SeedCell.Amount, LakeAmount)
				: LakeAmount;
		}
	}

	CellsByHeight.Sort([](
		const TPair<int32, int32>& A,
		const TPair<int32, int32>& B)
	{
		if (A.Key != B.Key)
		{
			return A.Key > B.Key;
		}
		return A.Value < B.Value;
	});
	int32 SandPlaced = 0;
	for (const TPair<int32, int32>& Pair : CellsByHeight)
	{
		if (SandPlaced >= 28)
		{
			break;
		}
		if (SeedCells[Pair.Value].MaterialId.IsNone())
		{
			SeedCells[Pair.Value].MaterialId = TEXT("sand");
			++SandPlaced;
		}
	}

	if (!Stream->Instances.IsEmpty())
	{
		// Do not seed reactive showcase materials beside the authored stream.
		// The stream and lake are one connected water state, so a nearby lava
		// sample becomes a permanent drain and turns shallow lake-edge cells
		// into real holes. Reactions are exercised in isolated maps/tests.
		for (int32 Index = 0;
			Index < Stream->Instances.Num();
			Index += 48)
		{
			const FIntPoint SteamCell =
				ToCell(Stream->Instances[Index].GetLocation())
					+ FIntPoint(1, 0);
			const int32* SeedIndex =
				SeedIndices.Find(SteamCell);
			if (SeedIndex
				&& SeedCells[*SeedIndex].MaterialId.IsNone())
			{
				SeedCells[*SeedIndex].MaterialId = TEXT("steam");
			}
		}
	}

	int32 SteamPlaced = 0;
	for (const MatterFlux::Material::FSeedCell& Cell : SeedCells)
	{
		SteamPlaced += Cell.MaterialId == TEXT("steam") ? 1 : 0;
	}
	for (int32 Index = 0;
		Index < CellsByHeight.Num() && SteamPlaced < 6;
		Index += FMath::Max(1, CellsByHeight.Num() / 17))
	{
		MatterFlux::Material::FSeedCell& Candidate =
			SeedCells[CellsByHeight[Index].Value];
		if (Candidate.MaterialId.IsNone())
		{
			Candidate.MaterialId = TEXT("steam");
			++SteamPlaced;
		}
	}

	int32 StonePlaced = 0;
	for (int32 Index = CellsByHeight.Num() - 1;
		Index >= 0 && StonePlaced < 4;
		--Index)
	{
		MatterFlux::Material::FSeedCell& Candidate =
			SeedCells[CellsByHeight[Index].Value];
		if (Candidate.MaterialId.IsNone())
		{
			Candidate.MaterialId = TEXT("stone");
			++StonePlaced;
		}
	}
	// Diagnostic material samples are secondary content. Never let them
	// overwrite authored stream/lake cells: the simulation state is the fact
	// from which the liquid surface is projected, so such an overwrite creates
	// a real square hole rather than a harmless visual marker.
	static const TPair<FIntPoint, FName> DistantShowcase[] =
	{
		{ FIntPoint(-12, -12), TEXT("water") },
		{ FIntPoint(12, -12), TEXT("lava") },
		{ FIntPoint(-12, 12), TEXT("sand") },
		{ FIntPoint(12, 12), TEXT("steam") }
	};
	for (const TPair<FIntPoint, FName>& Showcase
		: DistantShowcase)
	{
		if (const int32* SeedIndex =
			SeedIndices.Find(Showcase.Key))
		{
			MatterFlux::Material::FSeedCell& Cell = SeedCells[*SeedIndex];
			if (Cell.MaterialId.IsNone())
			{
				Cell.MaterialId = Showcase.Value;
				Cell.Amount = 255;
			}
		}
	}

	// 酸样本仍需相邻的水、草地和木头来覆盖反应链，但整组只放进
	// 已经为空的地表格，避免把地图生成器定义的液体或固体替换掉。
	static const FIntPoint ReactiveShowcaseAnchors[] =
	{
		FIntPoint(16, 16),
		FIntPoint(-16, 16),
		FIntPoint(16, -16),
		FIntPoint(-16, -16),
		FIntPoint(24, 0),
		FIntPoint(-24, 0)
	};
	static const TPair<FIntPoint, FName> ReactiveShowcase[] =
	{
		{ FIntPoint(0, 0), TEXT("acid") },
		{ FIntPoint(-1, 0), TEXT("water") },
		{ FIntPoint(1, 0), TEXT("grassland") },
		{ FIntPoint(0, 1), TEXT("wood") }
	};
	for (const FIntPoint Anchor : ReactiveShowcaseAnchors)
	{
		bool bFootprintIsEmpty = true;
		for (const TPair<FIntPoint, FName>& Showcase : ReactiveShowcase)
		{
			const int32* SeedIndex = SeedIndices.Find(Anchor + Showcase.Key);
			if (!SeedIndex || !SeedCells[*SeedIndex].MaterialId.IsNone())
			{
				bFootprintIsEmpty = false;
				break;
			}
		}
		if (!bFootprintIsEmpty)
		{
			continue;
		}
		for (const TPair<FIntPoint, FName>& Showcase : ReactiveShowcase)
		{
			MatterFlux::Material::FSeedCell& Cell = SeedCells[
				SeedIndices.FindChecked(Anchor + Showcase.Key)];
			Cell.MaterialId = Showcase.Value;
			Cell.Amount = 255;
		}
		break;
	}
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
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxSourceCombustionReplication);
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
	SCOPE_CYCLE_COUNTER(STAT_MatterFluxSourceCombustionReplication);
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
		bSourceCombustionVisualDirty = true;
		FragmentSourceProxy->FlushPendingChanges();
	}
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
	TArray<uint8> ResidueMask;
	TArray<uint8> BurningMask;
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
	if (Replicated.bHasCombustionState
		&& (!UnpackFragmentSourceMask(
				Replicated.PackedResidueMask,
				Definition->Mask.Width * Definition->Mask.Height,
				ResidueMask)
			|| !UnpackFragmentSourceMask(
				Replicated.PackedBurningMask,
				Definition->Mask.Width * Definition->Mask.Height,
				BurningMask)
			|| Replicated.CombustionRuleId.IsNone()
			|| !FMath::IsFinite(Replicated.CombustionAccumulator)
			|| Replicated.CombustionAccumulator < 0.0f
			|| Replicated.TotalSmokeEmissionCount < 0))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Client rejected logical source combustion state %s revision %d"),
			*Replicated.SourceId.ToString(),
			Replicated.Revision);
		return false;
	}
	const FFragment2DSourceStreamingState* ExistingState =
		StreamedFragmentSourceStates.Find(Replicated.SourceId);
	if (ExistingState && ExistingState->Revision > Replicated.Revision)
	{
		LogicalSourceCombustionIndex.ApplySnapshot(
			Replicated.SourceId,
			ExistingState->bHasCombustionState,
			ExistingState->CombustionState.BurningMask);
		return true;
	}

	FFragment2DSourceStreamingState CandidateState;
	CandidateState.Revision = Replicated.Revision;
	CandidateState.bHasCombustionState = Replicated.bHasCombustionState;
	CandidateState.CombustionAccumulator = Replicated.CombustionAccumulator;
	CandidateState.TotalSmokeEmissionCount =
		Replicated.TotalSmokeEmissionCount;
	if (Replicated.bHasCombustionState)
	{
		CandidateState.CombustionState.RuleId = Replicated.CombustionRuleId;
		CandidateState.CombustionState.Width = Definition->Mask.Width;
		CandidateState.CombustionState.Height = Definition->Mask.Height;
		CandidateState.CombustionState.Seed = Replicated.CombustionSeed;
		CandidateState.CombustionState.Tick = Replicated.CombustionTick;
		CandidateState.CombustionState.ResidueMask = ResidueMask;
		CandidateState.CombustionState.BurningMask = BurningMask;
	}
	else
	{
		ResidueMask.Init(0, Definition->Mask.Width * Definition->Mask.Height);
	}
	CandidateState.SetRuntimeMask(MoveTemp(RuntimeMask));
	FName ResidueMaterial = NAME_None;
	FLinearColor ResidueColor(0.08f, 0.07f, 0.06f);
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (Registry.IsValid() && Replicated.bHasCombustionState)
	{
		if (const FMatterFluxReactionDefinition* Rule =
			Registry->Reactions.Find(Replicated.CombustionRuleId))
		{
			ResidueMaterial = Rule->OutputA;
			if (const FMatterFluxMaterialDefinition* Material =
				Registry->Materials.Find(ResidueMaterial))
			{
				ResidueColor = Material->Color;
			}
		}
	}
	if (FragmentSourceProxy->ApplySourceState(
		Replicated.SourceId,
		CandidateState.GetRuntimeMask(),
		ResidueMask,
		ResidueMaterial,
		ResidueColor,
		CandidateState.bHasCombustionState
			&& CandidateState.CombustionState.BurningMask.Contains(1))
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
				ResidueMaterial,
				ResidueColor);
		}
		else
		{
			DynamicAggregateCarriers.Remove(Replicated.SourceId);
		}
	}
	LogicalSourceCombustionIndex.ApplySnapshot(
		Replicated.SourceId,
		RuntimeState.bHasCombustionState,
		RuntimeState.CombustionState.BurningMask);
	return true;
}

void AMatterFluxPlayableWorldActor::RemoveReplicatedFragmentSourceState(
	const FGuid& SourceId)
{
	StreamedFragmentSourceStates.Remove(SourceId);
	LogicalSourceCombustionIndex.Remove(SourceId);
	AppliedReplicatedFragmentSourceIds.Remove(SourceId);
	if (const MatterFlux::PlayableLevel::FLevelFragmentSource* Definition =
		FindFragmentSourceDefinition(SourceId))
	{
		TArray<uint8> EmptyResidue;
		EmptyResidue.Init(
			0,
			Definition->Mask.Width * Definition->Mask.Height);
		FragmentSourceProxy->ApplySourceState(
			SourceId,
			Definition->Mask.SolidMask,
			EmptyResidue,
			NAME_None,
			FLinearColor::Transparent,
			false);
	}
}

void AMatterFluxPlayableWorldActor::UpdateMaterialVisualization(
	const float DeltaSeconds)
{
	if (!bMaterialVisualizationDirty
		|| GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	MaterialVisualizationAccumulator +=
		FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	if (MaterialVisualizationAccumulator
		< FMath::Max(MaterialVisualizationInterval, 0.05f))
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

void AMatterFluxPlayableWorldActor::ApplyMaterialSourceContactReactions(
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
	for (const TPair<FName, FMatterFluxReactionDefinition>& Pair
		: Registry.Reactions)
	{
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
	if (ContactRuleIds.IsEmpty())
	{
		return;
	}

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	MaterialSimulation->GetAllCells(Cells);
	int32 AcceptedReactions = 0;
	TSet<FGuid> ReactedAggregateIds;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		if (AcceptedReactions >= MaxMaterialSourceReactionsPerFrame)
		{
			break;
		}
		const FMatterFluxMaterialDefinition* CellMaterial =
			Registry.Materials.Find(Cell.MaterialId);
		if (!CellMaterial
			|| CellMaterial->Phase != EMatterFluxMaterialPhase::Liquid
			|| !SourceReactiveLiquidMaterials.Contains(Cell.MaterialId))
		{
			continue;
		}

		const FVector LocalColumnBottom(
			(static_cast<double>(Cell.WorldCell.X) + 0.5)
				* MaterialSimulationCellSize,
			(static_cast<double>(Cell.WorldCell.Y) + 0.5)
				* MaterialSimulationCellSize,
			static_cast<double>(Cell.SupportHeight));
		const FVector LocalColumnCenter = LocalColumnBottom
			+ FVector(0.0f, 0.0f, MaterialLiquidColumnHeight * 0.5f);
		const FVector WorldColumnCenter =
			GetActorTransform().TransformPosition(LocalColumnCenter);
		const FVector WorldHalfExtent(
			MaterialSimulationCellSize * 0.52f,
			MaterialSimulationCellSize * 0.52f,
			MaterialLiquidColumnHeight * 0.5f);
		const FBox ContactBounds = FBox::BuildAABB(
			WorldColumnCenter,
			WorldHalfExtent);

		TArray<AFragment2DSourceActor*> Sources;
		GatherFragmentSourcesInBounds(ContactBounds, Sources);
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
			const FBox SourceBounds = Source->GetComponentsBoundingBox(true);
			const FVector ContactPoint = SourceBounds.IsValid
				? SourceBounds.GetClosestPointTo(WorldColumnCenter)
				: Source->GetActorLocation();
			FFragmentDamageEvent Damage;
			Damage.SourceId = ReactionSourceId;
			Damage.BaseRevision = Source->Revision;
			Damage.DamageShape.Type = EFragmentDamageShapeType::Circle;
			Damage.DamageShape.WorldTransform = FTransform(ContactPoint);
			Damage.DamageShape.Radius = FMath::Max(
				Source->GetCellSize() * 0.8f,
				MaterialSimulationCellSize * 0.2f);
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
			if (!SourceOutput
				|| SourceOutput->Phase != EMatterFluxMaterialPhase::Gas)
			{
				continue;
			}
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
	MaterialSimulation->GetAllCells(Cells);
	TMap<
		FMaterialVisualKey,
		TArray<MatterFlux::Material::FCellSnapshot>> MaterialCells;
	TMap<FName, TArray<MatterFlux::Material::FCellSnapshot>>
		LiquidMaterialCells;
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
			LiquidMaterialCells.FindOrAdd(Cell.MaterialId).Add(Cell);
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
				Thickness = 14.0f;
				HeightOffset = Thickness * 0.5f;
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
	for (TPair<FName, TArray<MatterFlux::Material::FCellSnapshot>>& Pair
		: LiquidMaterialCells)
	{
		Pair.Value.Sort([](
			const MatterFlux::Material::FCellSnapshot& Left,
			const MatterFlux::Material::FCellSnapshot& Right)
		{
			return Left.WorldCell.Y != Right.WorldCell.Y
				? Left.WorldCell.Y < Right.WorldCell.Y
				: Left.WorldCell.X < Right.WorldCell.X;
		});
	}

	TArray<FIntPoint> DirtyLiquidChunks;
	MaterialSimulation->ConsumeProjectionDirtyChunks(DirtyLiquidChunks);
	TMap<FIntPoint, TSet<FName>> CurrentMaterialsByChunk;
	for (const TPair<FName, TArray<MatterFlux::Material::FCellSnapshot>>& Pair
		: LiquidMaterialCells)
	{
		for (const MatterFlux::Material::FCellSnapshot& Cell : Pair.Value)
		{
			CurrentMaterialsByChunk.FindOrAdd(ToMaterialRenderChunk(
				Cell.WorldCell, MaterialSimulationChunkSize)).Add(Pair.Key);
		}
	}
	if (GeneratedLiquidLayerMeshes.IsEmpty())
	{
		CurrentMaterialsByChunk.GetKeys(DirtyLiquidChunks);
	}
	else
	{
		TSet<FIntPoint> RenderableLiquidChunks;
		CurrentMaterialsByChunk.GetKeys(RenderableLiquidChunks);
		for (const TPair<FName, FIntPoint>& Existing : LiquidProjectionChunks)
		{
			RenderableLiquidChunks.Add(Existing.Value);
		}
		DirtyLiquidChunks.RemoveAll(
			[&RenderableLiquidChunks](const FIntPoint Chunk)
			{
				return !RenderableLiquidChunks.Contains(Chunk);
			});
	}
	DirtyLiquidChunks.Sort([](const FIntPoint A, const FIntPoint B)
	{
		return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
	});
	LastLiquidProjectionDirtyChunkCount = DirtyLiquidChunks.Num();
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
			if (const TArray<MatterFlux::Material::FCellSnapshot>* LiquidCellsForMaterial =
				LiquidMaterialCells.Find(MaterialId))
			{
				for (const MatterFlux::Material::FCellSnapshot& Cell
					: *LiquidCellsForMaterial)
				{
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
	const bool bEnableCollision =
		bEnableMaterialSimulationCollision
		&& Material.Phase
			== EMatterFluxMaterialPhase::StaticSolid;
	Instances->SetCollisionEnabled(
		bEnableCollision
			? ECollisionEnabled::QueryAndPhysics
			: ECollisionEnabled::NoCollision);
	Instances->SetCollisionResponseToAllChannels(
		bEnableCollision ? ECR_Block : ECR_Ignore);
	Instances->SetCanEverAffectNavigation(bEnableCollision);

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
	// residency. The chunk proxy already reads RuntimeMask and residue overlays;
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
		&& (!ArchivedState->bHasCombustionState
			|| !ArchivedState->CombustionState.ResidueMask.Contains(1)))
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
	ActiveSourceCombustions.Remove(Source.SourceId);
	LogicalSourceCombustionIndex.Remove(Source.SourceId);
	bSourceCombustionVisualDirty = true;
	if (FragmentSourceProxy)
	{
		FragmentSourceProxy->SetSourceMaterialized(Source.SourceId, true);
	}
}

void AMatterFluxPlayableWorldActor::DestroyGeneratedFragmentSources()
{
	for (const TPair<FGuid, TObjectPtr<AFragment2DSourceActor>>& Pair
		: GeneratedFragmentSources)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	GeneratedFragmentSources.Reset();
	ActiveSourceCombustions.Reset();
	LogicalSourceCombustionIndex.Reset();
	FragmentSourceChunks.Reset();
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
	bSourceCombustionVisualDirty = false;
	SourceCombustionVisualAccumulator = 0.0f;
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

void AMatterFluxPlayableWorldActor::GatherStreamingFocusChunks(
	TArray<FIntPoint>& OutFocusChunks) const
{
	OutFocusChunks.Reset();
	const UWorld* World = GetWorld();
	if (World)
	{
		TSet<FIntPoint> UniqueChunks;
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
		OutFocusChunks = UniqueChunks.Array();
	}

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
}

bool AMatterFluxPlayableWorldActor::RefreshVisibleTerrainChunks(
	const bool bForce,
	const TArray<FIntPoint>& FocusChunks)
{
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
		if ((bForce || FocusChunks.Contains(Coordinate))
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
		bGroundCombustionVisualDirty = true;
		bGroundCombustionVisualNeedsFullRebuild = true;
		PendingGroundCombustionVisualCellIndices.Reset();
	}
	return true;
}

void AMatterFluxPlayableWorldActor::ProcessPendingTerrainChunkPrefetches()
{
	if (PendingTerrainChunkPrefetches.IsEmpty()
		|| !TerrainHeightField.IsValid())
	{
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	int32 CreatedChunkCount = 0;
	while (!PendingTerrainChunkPrefetches.IsEmpty()
		&& CreatedChunkCount < MaxTerrainChunkPrefetchesPerFrame)
	{
		if (CreatedChunkCount > 0
			&& (FPlatformTime::Seconds() - StartSeconds) * 1000.0
				>= TerrainChunkPrefetchBudgetMilliseconds)
		{
			break;
		}

		const FIntPoint Coordinate = PendingTerrainChunkPrefetches[0];
		PendingTerrainChunkPrefetches.RemoveAt(
			0,
			1,
			EAllowShrinking::No);
		if (GeneratedTerrainChunks.Contains(Coordinate))
		{
			continue;
		}

		UProceduralMeshComponent* Component =
			CreateTerrainChunkComponent(Coordinate);
		if (!Component)
		{
			continue;
		}
		++CreatedChunkCount;
		const bool bShouldBeActive =
			DesiredTerrainChunks.Contains(Coordinate);
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
		TerrainChunkLastUsed.Add(Coordinate, ++TerrainChunkUseCounter);
		if (bShouldBeActive)
		{
			ActiveTerrainChunks.Add(Coordinate);
			bGroundCombustionVisualDirty = true;
			bGroundCombustionVisualNeedsFullRebuild = true;
			PendingGroundCombustionVisualCellIndices.Reset();
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
}

UProceduralMeshComponent*
AMatterFluxPlayableWorldActor::CreateTerrainChunkComponent(
	const FIntPoint ChunkCoordinate)
{
	MatterFlux::TerrainMesh::FChunk Chunk;
	if (!MatterFlux::TerrainMesh::BuildChunk(
		TerrainHeightField,
		ChunkCoordinate,
		TerrainStreamingChunkSize,
		Chunk))
	{
		return nullptr;
	}

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
	// Chunk geometry is generated on the game thread, but Chaos triangle-mesh
	// cooking is substantially more expensive than filling the render buffers.
	// The active window already extends well beyond the camera, so asynchronous
	// cooking finishes while a prefetched chunk is still far from the player
	// instead of stalling the exact frame in which a chunk boundary is crossed.
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
	Colors.Init(FLinearColor::White, Projection.Vertices.Num());
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
		Material->SetScalarParameterValue(TEXT("Roughness"), 0.24f);
	}
}
