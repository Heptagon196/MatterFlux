#include "Game/MatterFluxFragmentSourceProxyComponent.h"

#include "Fragment/FragmentGeometry.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MatterFluxLog.h"
#include "ProceduralMeshComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rendering/MatterFluxGhostFade.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "Rendering/MatterFluxWholeObjectGeometry.h"

namespace
{
	struct FProxyMeshGroup
	{
		FName MaterialId = NAME_None;
		FLinearColor Color = FLinearColor::White;
		float CellSize = 1.0f;
		bool bSide = false;
		bool bCollision = false;
		bool bGhost = false;
		FGuid GhostSourceId;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FColor> VertexColors;
	};

	FString MakeGroupKey(
		const FName MaterialId,
		const FLinearColor& Color,
		const float CellSize,
		const bool bSide,
		const bool bCollision = false,
		const bool bGhost = false,
		const FGuid& GhostSourceId = FGuid())
	{
		return FString::Printf(
			TEXT("%s|%08x|%08x|%s|%s|%s|%s"),
			*MaterialId.ToString(),
			Color.ToFColor(false).DWColor(),
			GetTypeHash(CellSize),
			bSide ? TEXT("side") : TEXT("face"),
			bCollision ? TEXT("collision") : TEXT("visual"),
			bGhost ? TEXT("ghost") : TEXT("solid"),
			bGhost ? *GhostSourceId.ToString(EGuidFormats::Digits) : TEXT("-"));
	}

	void AppendMeshPart(
		FProxyMeshGroup& Group,
		const FTransform& Transform,
		const TArray<FVector>& SourceVertices,
		const TArray<int32>& SourceTriangles,
		const TArray<FVector>& SourceNormals,
		const TArray<FVector2D>& SourceUVs,
		const int32 FirstIndex,
		const int32 IndexCount,
		const TArray<FColor>* SourceVertexColors = nullptr)
	{
		if (FirstIndex < 0
			|| IndexCount <= 0
			|| FirstIndex > SourceTriangles.Num() - IndexCount
			|| SourceNormals.Num() != SourceVertices.Num()
			|| SourceUVs.Num() != SourceVertices.Num()
			|| (SourceVertexColors
				&& SourceVertexColors->Num() != SourceVertices.Num()))
		{
			return;
		}
		for (int32 Index = FirstIndex;
			Index < FirstIndex + IndexCount;
			++Index)
		{
			if (!SourceVertices.IsValidIndex(SourceTriangles[Index]))
			{
				return;
			}
		}

		const int32 VertexOffset = Group.Vertices.Num();
		const int32 MaximumAddedVertexCount =
			FMath::Min(IndexCount, SourceVertices.Num());
		Group.Vertices.Reserve(
			Group.Vertices.Num() + MaximumAddedVertexCount);
		Group.Normals.Reserve(
			Group.Normals.Num() + MaximumAddedVertexCount);
		Group.UVs.Reserve(Group.UVs.Num() + MaximumAddedVertexCount);
		Group.VertexColors.Reserve(
			Group.VertexColors.Num() + MaximumAddedVertexCount);
		Group.Triangles.Reserve(Group.Triangles.Num() + IndexCount);
		TArray<int32, TInlineAllocator<256>> LocalVertexBySourceIndex;
		LocalVertexBySourceIndex.Init(INDEX_NONE, SourceVertices.Num());
		for (int32 Index = FirstIndex;
			Index < FirstIndex + IndexCount;
			++Index)
		{
			const int32 SourceVertexIndex = SourceTriangles[Index];
			int32& LocalVertexIndex =
				LocalVertexBySourceIndex[SourceVertexIndex];
			if (LocalVertexIndex == INDEX_NONE)
			{
				LocalVertexIndex = Group.Vertices.Num() - VertexOffset;
				Group.Vertices.Add(
					Transform.TransformPosition(
						SourceVertices[SourceVertexIndex]));
				Group.Normals.Add(
					Transform.TransformVectorNoScale(
						SourceNormals[SourceVertexIndex])
						.GetSafeNormal());
				Group.UVs.Add(SourceUVs[SourceVertexIndex]);
				Group.VertexColors.Add(SourceVertexColors
					? (*SourceVertexColors)[SourceVertexIndex]
					: FColor::White);
			}
			Group.Triangles.Add(VertexOffset + LocalVertexIndex);
		}
	}
}

UMatterFluxFragmentSourceProxyComponent::
	UMatterFluxFragmentSourceProxyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	GhostMaterialTemplate = LoadObject<UMaterialInterface>(
		nullptr,
		TEXT("/Game/MatterFlux/Materials/M_VoxelGas.M_VoxelGas"));
}

void UMatterFluxFragmentSourceProxyComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	TArray<FGuid, TInlineAllocator<16>> RestoredSourceIds;
	bool bHasTransition = false;
	for (const FGuid& SourceId : GhostedSourceIds)
	{
		float& Opacity = GhostOpacityBySourceId.FindOrAdd(SourceId, 1.0f);
		const bool bGhostDesired = TargetGhostedSourceIds.Contains(SourceId);
		Opacity = MatterFlux::GhostFade::AdvanceItemOpacity(
			Opacity, bGhostDesired, DeltaTime);
		UpdateGhostMaterialOpacity(SourceId, Opacity);
		if (!bGhostDesired && Opacity >= 0.999f)
		{
			RestoredSourceIds.Add(SourceId);
		}
		else
		{
			const float TargetOpacity = bGhostDesired
				? MatterFlux::GhostFade::DefaultItemOpacity : 1.0f;
			bHasTransition |= !FMath::IsNearlyEqual(
				Opacity, TargetOpacity, 0.001f);
		}
	}

	for (const FGuid& SourceId : RestoredSourceIds)
	{
		if (const FSourceLocator* Locator = SourceLocatorById.Find(SourceId))
		{
			DirtyChunks.Add(Locator->Chunk);
		}
		GhostedSourceIds.Remove(SourceId);
		GhostOpacityBySourceId.Remove(SourceId);
		RemoveGhostMaterials(SourceId);
	}
	if (!RestoredSourceIds.IsEmpty())
	{
		FlushPendingChanges();
	}
	SetComponentTickEnabled(bHasTransition);
}

void UMatterFluxFragmentSourceProxyComponent::Configure(
	USceneComponent* InAttachParent,
	UMaterialInterface* InMaterialTemplate,
	UMaterialInterface* InLeafMaterialTemplate,
	UMaterialInterface* InWoodMaterialTemplate)
{
	AttachParent = InAttachParent;
	if (MaterialTemplate != InMaterialTemplate
		|| LeafMaterialTemplate != InLeafMaterialTemplate
		|| WoodMaterialTemplate != InWoodMaterialTemplate)
	{
		MaterialTemplate = InMaterialTemplate;
		LeafMaterialTemplate = InLeafMaterialTemplate;
		WoodMaterialTemplate = InWoodMaterialTemplate;
		Materials.Reset();
		for (const FIntPoint Chunk : VisibleChunks)
		{
			RebuildChunk(Chunk);
		}
	}
}

void UMatterFluxFragmentSourceProxyComponent::SetSourceChunks(
	const TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& InChunks)
{
	ResetSources();
	for (const TPair<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& Pair
		: InChunks)
	{
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>& ProxySources =
			SourceChunks.FindOrAdd(Pair.Key);
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Pair.Value)
		{
			if (Source.SourceId.IsValid()
				&& Source.Mask.IsValid())
			{
				const int32 SourceIndex = ProxySources.Add(Source);
				SourceLocatorById.Add(
					Source.SourceId,
					{Pair.Key, SourceIndex});
			}
		}
		if (ProxySources.IsEmpty())
		{
			SourceChunks.Remove(Pair.Key);
		}
	}
}

void UMatterFluxFragmentSourceProxyComponent::ApplySourceChunkDelta(
	const TArray<FIntPoint>& RemovedChunks,
	const TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& UpdatedChunks)
{
	TArray<FIntPoint> OrderedRemovedChunks = RemovedChunks;
	OrderedRemovedChunks.Sort([](const FIntPoint Left, const FIntPoint Right)
	{
		return Left.Y == Right.Y ? Left.X < Right.X : Left.Y < Right.Y;
	});
	for (const FIntPoint Chunk : OrderedRemovedChunks)
	{
		RemoveSourceChunk(Chunk);
	}

	TArray<FIntPoint> OrderedUpdatedChunks;
	UpdatedChunks.GenerateKeyArray(OrderedUpdatedChunks);
	OrderedUpdatedChunks.Sort([](const FIntPoint Left, const FIntPoint Right)
	{
		return Left.Y == Right.Y ? Left.X < Right.X : Left.Y < Right.Y;
	});
	for (const FIntPoint Chunk : OrderedUpdatedChunks)
	{
		RemoveSourceChunk(Chunk);
		const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>& Sources =
			UpdatedChunks.FindChecked(Chunk);
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>& ProxySources =
			SourceChunks.FindOrAdd(Chunk);
		ProxySources.Reserve(Sources.Num());
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Sources)
		{
			if (!Source.SourceId.IsValid() || !Source.Mask.IsValid())
			{
				continue;
			}
			const int32 SourceIndex = ProxySources.Add(Source);
			SourceLocatorById.Add(Source.SourceId, {Chunk, SourceIndex});
		}
		if (ProxySources.IsEmpty())
		{
			SourceChunks.Remove(Chunk);
			continue;
		}
		if (VisibleChunks.Contains(Chunk))
		{
			DirtyChunks.Add(Chunk);
		}
		else
		{
			// Procedural population is resident one chunk ring beyond the
			// visible proxy window. Compile that one newly completed chunk while
			// it is still off-screen, so crossing a boundary only reveals cached
			// meshes instead of synchronously rebuilding the whole entering edge.
			RebuildChunk(Chunk);
			DirtyChunks.Remove(Chunk);
			DeferredReactionChunks.Remove(Chunk);
			if (UProceduralMeshComponent* Prepared = ChunkMeshes.FindRef(Chunk))
			{
				Prepared->SetVisibility(false, true);
				Prepared->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
		}
	}
}

void UMatterFluxFragmentSourceProxyComponent::SetVisibleChunks(
	const TSet<FIntPoint>& InVisibleChunks)
{
	TArray<FIntPoint> RemovedChunks;
	for (const FIntPoint Chunk : VisibleChunks)
	{
		if (!InVisibleChunks.Contains(Chunk))
		{
			RemovedChunks.Add(Chunk);
		}
	}
	for (const FIntPoint Chunk : RemovedChunks)
	{
		if (UProceduralMeshComponent* Mesh = ChunkMeshes.FindRef(Chunk))
		{
			Mesh->SetVisibility(false, true);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	TArray<FIntPoint> AddedChunks;
	for (const FIntPoint Chunk : InVisibleChunks)
	{
		if (!VisibleChunks.Contains(Chunk))
		{
			AddedChunks.Add(Chunk);
		}
	}
	VisibleChunks = InVisibleChunks;
	AddedChunks.Sort([](const FIntPoint Left, const FIntPoint Right)
	{
		return Left.Y == Right.Y ? Left.X < Right.X : Left.Y < Right.Y;
	});
	for (const FIntPoint Chunk : AddedChunks)
	{
		if (UProceduralMeshComponent* Prepared = ChunkMeshes.FindRef(Chunk);
			IsValid(Prepared)
				&& !DirtyChunks.Contains(Chunk)
				&& !DeferredReactionChunks.Contains(Chunk))
		{
			Prepared->SetVisibility(true, true);
			Prepared->SetCollisionEnabled(
				CollisionChunks.Contains(Chunk)
					? ECollisionEnabled::QueryAndPhysics
					: ECollisionEnabled::NoCollision);
		}
		else
		{
			RebuildChunk(Chunk);
		}
		DirtyChunks.Remove(Chunk);
		DeferredReactionChunks.Remove(Chunk);
	}
}

void UMatterFluxFragmentSourceProxyComponent::PrepareSourceChunks(
	const int32 MaximumPreparedChunkCount)
{
	if (MaximumPreparedChunkCount <= 0
		|| SourceChunks.Num() > MaximumPreparedChunkCount
		|| !GetOwner()
		|| GetOwner()->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	TArray<FIntPoint> OrderedChunks;
	SourceChunks.GenerateKeyArray(OrderedChunks);
	OrderedChunks.Sort([](const FIntPoint Left, const FIntPoint Right)
	{
		return Left.Y == Right.Y ? Left.X < Right.X : Left.Y < Right.Y;
	});
	for (const FIntPoint Chunk : OrderedChunks)
	{
		RebuildChunk(Chunk);
		DirtyChunks.Remove(Chunk);
		DeferredReactionChunks.Remove(Chunk);
		if (!VisibleChunks.Contains(Chunk))
		{
			if (UProceduralMeshComponent* Prepared = ChunkMeshes.FindRef(Chunk))
			{
				Prepared->SetVisibility(false, true);
				Prepared->SetCollisionEnabled(
					ECollisionEnabled::NoCollision);
			}
		}
	}
}

void UMatterFluxFragmentSourceProxyComponent::SetSourceMaterialized(
	const FGuid& SourceId,
	const bool bMaterialized)
{
	if (bMaterialized)
	{
		SetSourceReactionActive(SourceId, false);
	}
	const FSourceLocator* Locator = SourceLocatorById.Find(SourceId);
	if (!Locator)
	{
		return;
	}
	bool bChanged = false;
	if (bMaterialized && !MaterializedSourceIds.Contains(SourceId))
	{
		MaterializedSourceIds.Add(SourceId);
		bChanged = true;
	}
	else if (!bMaterialized)
	{
		bChanged = MaterializedSourceIds.Remove(SourceId) > 0;
	}
	if (bChanged)
	{
		DirtyChunks.Add(Locator->Chunk);
		DeferredReactionChunks.Remove(Locator->Chunk);
	}
}

void UMatterFluxFragmentSourceProxyComponent::SetGhostedSources(
	const TSet<FGuid>& SourceIds)
{
	TSet<FGuid> ValidSourceIds;
	for (const FGuid& SourceId : SourceIds)
	{
		if (SourceLocatorById.Contains(SourceId))
		{
			ValidSourceIds.Add(SourceId);
		}
	}
	bool bTargetsUnchanged =
		ValidSourceIds.Num() == TargetGhostedSourceIds.Num();
	if (bTargetsUnchanged)
	{
		for (const FGuid& SourceId : ValidSourceIds)
		{
			if (!TargetGhostedSourceIds.Contains(SourceId))
			{
				bTargetsUnchanged = false;
				break;
			}
		}
	}
	if (bTargetsUnchanged)
	{
		return;
	}
	TargetGhostedSourceIds = MoveTemp(ValidSourceIds);
	for (const FGuid& SourceId : TargetGhostedSourceIds)
	{
		if (!GhostedSourceIds.Contains(SourceId))
		{
			GhostedSourceIds.Add(SourceId);
			GhostOpacityBySourceId.Add(SourceId, 1.0f);
			DirtyChunks.Add(SourceLocatorById.FindChecked(SourceId).Chunk);
		}
	}
	FlushPendingChanges();
	SetComponentTickEnabled(!GhostedSourceIds.IsEmpty());
}

void UMatterFluxFragmentSourceProxyComponent::SetDebugIsolatedAggregate(
	const FGuid& AggregateId)
{
	if (DebugIsolatedAggregateId == AggregateId)
	{
		return;
	}
	DebugIsolatedAggregateId = AggregateId;
	for (const FIntPoint Chunk : VisibleChunks)
	{
		DirtyChunks.Add(Chunk);
	}
	FlushPendingChanges();
}

void UMatterFluxFragmentSourceProxyComponent::SetSourceReactionActive(
	const FGuid& SourceId,
	const bool bActive)
{
	const FSourceLocator* Locator = SourceLocatorById.Find(SourceId);
	if (!Locator)
	{
		return;
	}
	if (bActive)
	{
		ReactingSourceIds.Add(SourceId);
		return;
	}
	if (ReactingSourceIds.Remove(SourceId) > 0)
	{
		DeferredReactionChunks.Add(Locator->Chunk);
	}
}

void UMatterFluxFragmentSourceProxyComponent::
	FlushDeferredReactionChanges()
{
	for (const FIntPoint Chunk : DeferredReactionChunks)
	{
		DirtyChunks.Add(Chunk);
	}
	DeferredReactionChunks.Reset();
}

EMatterFluxFragmentSourceProxyApplyResult
UMatterFluxFragmentSourceProxyComponent::ApplySourceState(
	const FGuid& SourceId,
	const TArray<uint8>& RuntimeMask,
	const TArray<uint8>& OutputMask,
	const FName OutputMaterialId,
	const FLinearColor& OutputColor,
	const bool bReactionActive)
{
	const FSourceLocator* Locator = SourceLocatorById.Find(SourceId);
	TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
		Locator ? SourceChunks.Find(Locator->Chunk) : nullptr;
	MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
		Locator && Sources && Sources->IsValidIndex(Locator->SourceIndex)
			? &(*Sources)[Locator->SourceIndex]
			: nullptr;
	if (!Source || Source->SourceId != SourceId)
	{
		return EMatterFluxFragmentSourceProxyApplyResult::Invalid;
	}
	const int32 CellCount = Source->Mask.Width * Source->Mask.Height;
	if (RuntimeMask.Num() != CellCount || OutputMask.Num() != CellCount)
	{
		return EMatterFluxFragmentSourceProxyApplyResult::Invalid;
	}
	const bool bCanRenderOutput = OutputMaterialId != TEXT("empty");
	bool bHasOutput = false;
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		if (RuntimeMask[CellIndex] > 1 || OutputMask[CellIndex] > 1)
		{
			return EMatterFluxFragmentSourceProxyApplyResult::Invalid;
		}
		bHasOutput |= bCanRenderOutput && OutputMask[CellIndex] != 0;
	}

	const bool bRuntimeChanged = Source->Mask.SolidMask != RuntimeMask;
	const FSourceOutputState* ExistingOutput =
		SourceOutputs.Find(SourceId);
	const bool bOutputChanged = bHasOutput
		? !ExistingOutput
			|| ExistingOutput->Mask != OutputMask
			|| ExistingOutput->MaterialId != OutputMaterialId
			|| !ExistingOutput->Color.Equals(OutputColor)
		: ExistingOutput != nullptr;
	const bool bWasReactionActive =
		ReactingSourceIds.Contains(SourceId);
	const bool bReactionStateChanged =
		bWasReactionActive != bReactionActive;
	if (!bRuntimeChanged
		&& !bOutputChanged
		&& !bReactionStateChanged)
	{
		return EMatterFluxFragmentSourceProxyApplyResult::Unchanged;
	}

	if (bRuntimeChanged)
	{
		Source->Mask.SolidMask = RuntimeMask;
		CachedSourceMeshes.Remove(SourceId);
	}
	if (bHasOutput)
	{
		FSourceOutputState& State = SourceOutputs.FindOrAdd(SourceId);
		State.Mask = OutputMask;
		State.MaterialId = OutputMaterialId;
		State.Color = OutputColor;
	}
	else
	{
		SourceOutputs.Remove(SourceId);
	}
	if (bOutputChanged)
	{
		CachedOutputMeshes.Remove(SourceId);
	}
	if (bReactionActive)
	{
		ReactingSourceIds.Add(SourceId);
	}
	else if (ReactingSourceIds.Remove(SourceId) > 0)
	{
		DeferredReactionChunks.Add(Locator->Chunk);
	}
	// 燃烧状态与可视网格必须在同一个批次内提交。旧逻辑会在燃烧期间
	// 一直保留绿色的 chunk 网格，直到整份 source 燃尽才一次性变黑；
	// DirtyChunks 本身是集合，因此同一模拟批次内多格、多 source 的变化
	// 仍只会触发一次 chunk 重建。
	if (bRuntimeChanged || bOutputChanged)
	{
		DirtyChunks.Add(Locator->Chunk);
		DeferredReactionChunks.Remove(Locator->Chunk);
	}
	return EMatterFluxFragmentSourceProxyApplyResult::Changed;
}

void UMatterFluxFragmentSourceProxyComponent::FlushPendingChanges()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_Proxy_FlushPendingChanges);
	TArray<FIntPoint> Chunks;
	Chunks.Reserve(DirtyChunks.Num());
	for (const FIntPoint Chunk : DirtyChunks)
	{
		Chunks.Add(Chunk);
	}
	DirtyChunks.Reset();
	Chunks.Sort([](const FIntPoint Left, const FIntPoint Right)
	{
		return Left.Y == Right.Y ? Left.X < Right.X : Left.Y < Right.Y;
	});
	for (const FIntPoint Chunk : Chunks)
	{
		if (VisibleChunks.Contains(Chunk))
		{
			RebuildChunk(Chunk);
		}
		else
		{
			// A prepared hidden mesh may still contain the old proxy/Actor
			// ownership decision. Keep it dirty until SetVisibleChunks can
			// rebuild the completed replacement before showing the chunk.
			DirtyChunks.Add(Chunk);
		}
	}
}

void UMatterFluxFragmentSourceProxyComponent::ResetSources()
{
	TArray<FIntPoint> ExistingChunks;
	ChunkMeshes.GenerateKeyArray(ExistingChunks);
	for (const FIntPoint Chunk : ExistingChunks)
	{
		DestroyChunk(Chunk);
	}
	SourceChunks.Reset();
	SourceLocatorById.Reset();
	VisibleChunks.Reset();
	CollisionChunks.Reset();
	MaterializedSourceIds.Reset();
	TargetGhostedSourceIds.Reset();
	GhostedSourceIds.Reset();
	GhostOpacityBySourceId.Reset();
	DebugIsolatedAggregateId.Invalidate();
	ReactingSourceIds.Reset();
	DirtyChunks.Reset();
	DeferredReactionChunks.Reset();
	CachedSourceMeshes.Reset();
	SourceOutputs.Reset();
	CachedOutputMeshes.Reset();
	StandaloneTreeOutputProjectionCounts.Reset();
	Materials.Reset();
	GhostMaterials.Reset();
	GhostMaterialSourceIds.Reset();
	SetComponentTickEnabled(false);
}

int32 UMatterFluxFragmentSourceProxyComponent::GetVisibleSourceCount() const
{
	int32 Count = 0;
	for (const FIntPoint Chunk : VisibleChunks)
	{
		if (const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
			SourceChunks.Find(Chunk))
		{
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
				: *Sources)
			{
				Count += MaterializedSourceIds.Contains(Source.SourceId) ? 0 : 1;
			}
		}
	}
	return Count;
}

bool UMatterFluxFragmentSourceProxyComponent::IsProxySource(
	const FGuid& SourceId) const
{
	return SourceLocatorById.Contains(SourceId);
}

int32 UMatterFluxFragmentSourceProxyComponent::
	GetInputOutputOverlapCellCount() const
{
	int32 Count = 0;
	for (const TPair<FGuid, FSourceOutputState>& Pair : SourceOutputs)
	{
		const FSourceLocator* Locator = SourceLocatorById.Find(Pair.Key);
		const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
			Locator ? SourceChunks.Find(Locator->Chunk) : nullptr;
		const MatterFlux::PlayableLevel::FLevelFragmentSource* Source =
			Locator && Sources && Sources->IsValidIndex(Locator->SourceIndex)
				? &(*Sources)[Locator->SourceIndex]
				: nullptr;
		if (!Source || Pair.Value.Mask.Num() != Source->Mask.SolidMask.Num())
		{
			continue;
		}
		for (int32 CellIndex = 0;
			CellIndex < Pair.Value.Mask.Num();
			++CellIndex)
		{
			Count += Source->Mask.SolidMask[CellIndex] != 0
				&& Pair.Value.Mask[CellIndex] != 0
				? 1
				: 0;
		}
	}
	return Count;
}

int32 UMatterFluxFragmentSourceProxyComponent::
	GetStandaloneTreeOutputProjectionCount() const
{
	int32 Count = 0;
	for (const TPair<FIntPoint, int32>& Pair
		: StandaloneTreeOutputProjectionCounts)
	{
		Count += Pair.Value;
	}
	return Count;
}

void UMatterFluxFragmentSourceProxyComponent::RebuildChunk(
	const FIntPoint Chunk)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_Proxy_RebuildChunk);
	const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
		SourceChunks.Find(Chunk);
	if (!Sources || !AttachParent || !GetOwner()
		|| GetOwner()->GetNetMode() == NM_DedicatedServer)
	{
		StandaloneTreeOutputProjectionCounts.Remove(Chunk);
		return;
	}
	int32 StandaloneTreeOutputProjectionCount = 0;

	TMap<FString, FProxyMeshGroup> Groups;
	TMap<FGuid, TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>>
		TreeSourcesByAggregate;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: *Sources)
	{
		if (DebugIsolatedAggregateId.IsValid()
			&& Source.AggregateId != DebugIsolatedAggregateId)
		{
			continue;
		}
		const bool bVoxelAggregatePart = Source.AggregateId.IsValid()
			&& Source.Mask.GeometryStyle
				== EFragmentSourceGeometryStyle::VoxelBlocks;
		if (!MaterializedSourceIds.Contains(Source.SourceId)
			&& bVoxelAggregatePart)
		{
			TreeSourcesByAggregate.FindOrAdd(Source.AggregateId).Add(&Source);
		}
	}

	// 逻辑层仍保留可独立切割、燃烧的二维树干/枝条/叶片 source；
	// 静态代理则把同一棵树还原到一套多材质三维体素占用中。每种材质
	// 只输出朝向空气的面，木叶交界面以及重叠格都不会重复绘制。
	// 重叠格由叶子取得显示优先级，避免树冠表面出现棕色 L 形穿插。
	TSet<FGuid> VolumeRenderedTreeSourceIds;
	for (const TPair<
		FGuid,
		TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>>& Pair
		: TreeSourcesByAggregate)
	{
		const TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>&
			TreeSources = Pair.Value;
		if (TreeSources.IsEmpty())
		{
			continue;
		}
		const bool bGhostAggregate = TreeSources.ContainsByPredicate(
			[this](const MatterFlux::PlayableLevel::FLevelFragmentSource* Source)
			{
				return Source
					&& GhostedSourceIds.Contains(Source->SourceId);
			});
		FGuid GhostSourceId;
		if (bGhostAggregate)
		{
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource* Source
				: TreeSources)
			{
				if (Source && GhostedSourceIds.Contains(Source->SourceId))
				{
					GhostSourceId = Source->SourceId;
					break;
				}
			}
		}

		// 新整体物体 Adapter：静态树与脱落后的动态树都把相同的
		// mask 层交给 WholeObject 深模块。材料列表先按稳定键排序，
		// 因而 TMap 遍历次序不会改变 section、顶点或三角形顺序。
		struct FWholeObjectMaterial
		{
			FString StableKey;
			FName MaterialId = NAME_None;
			FLinearColor Color = FLinearColor::White;
			bool bCollision = false;
		};
		TArray<FWholeObjectMaterial> WholeMaterials;
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource* TreeSource
			: TreeSources)
		{
			if (!TreeSource)
			{
				continue;
			}
			const FString StableKey = MakeGroupKey(
				TreeSource->MaterialId,
				TreeSource->Color,
				TreeSource->Mask.CellSize,
				false,
				TreeSource->bEnableCollision);
			if (!WholeMaterials.ContainsByPredicate(
				[&StableKey](const FWholeObjectMaterial& Existing)
				{
					return Existing.StableKey == StableKey;
				}))
			{
				WholeMaterials.Add({
					StableKey,
					TreeSource->MaterialId,
					TreeSource->Color,
					TreeSource->bEnableCollision});
			}
			// 燃烧残留与尚未反应的材料必须由同一次体素表面编译生成。
			// 若把 charcoal/ash 作为额外二维网格追加，原树外壳不会参与
			// 它的遮挡与消面，画面就会像“正常树与烧焦树叠在一起”。
			if (const FSourceOutputState* Output =
				SourceOutputs.Find(TreeSource->SourceId))
			{
				const FString OutputStableKey = MakeGroupKey(
					Output->MaterialId,
					Output->Color,
					TreeSource->Mask.CellSize,
					false,
					false);
				if (!WholeMaterials.ContainsByPredicate(
					[&OutputStableKey](const FWholeObjectMaterial& Existing)
					{
						return Existing.StableKey == OutputStableKey;
					}))
				{
					WholeMaterials.Add({
						OutputStableKey,
						Output->MaterialId,
						Output->Color,
						false});
				}
			}
		}
		WholeMaterials.Sort([](
			const FWholeObjectMaterial& A,
			const FWholeObjectMaterial& B)
		{
			return A.StableKey < B.StableKey;
		});
		TArray<MatterFlux::WholeObject::FLayer> WholeLayers;
		const MatterFlux::PlayableLevel::FLevelFragmentSource* AggregateRoot =
			TreeSources[0];
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource* TreeSource
			: TreeSources)
		{
			if (TreeSource && TreeSource->bAggregateRoot)
			{
				AggregateRoot = TreeSource;
				break;
			}
		}
		FTransform AggregateFrame = AggregateRoot
			? AggregateRoot->Transform
			: FTransform::Identity;
		AggregateFrame.SetScale3D(FVector::OneVector);
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource* TreeSource
			: TreeSources)
		{
			if (!TreeSource)
			{
				continue;
			}
			const FString StableKey = MakeGroupKey(
				TreeSource->MaterialId,
				TreeSource->Color,
				TreeSource->Mask.CellSize,
				false,
				TreeSource->bEnableCollision);
			const int32 MaterialIndex = WholeMaterials.IndexOfByPredicate(
				[&StableKey](const FWholeObjectMaterial& Existing)
				{
					return Existing.StableKey == StableKey;
				});
			const int32 SourceLayerPriority =
				TreeSource->MaterialId == TEXT("leaf") ? 100 : 10;
			FTransform SourceLayerTransform =
				TreeSource->Transform.GetRelativeTransform(AggregateFrame);
			SourceLayerTransform.SetScale3D(FVector::OneVector);
			MatterFlux::WholeObject::FLayer& Layer =
				WholeLayers.AddDefaulted_GetRef();
			Layer.MaterialIndex = MaterialIndex;
			Layer.Priority = SourceLayerPriority;
			Layer.bEnableCollision = TreeSource->bEnableCollision;
			Layer.Width = TreeSource->Mask.Width;
			Layer.Height = TreeSource->Mask.Height;
			Layer.CellSize = TreeSource->Mask.CellSize;
			// 整棵树可以朝向镜头旋转，但编译器只需要处理树局部坐标中
			// 的 90 度格点旋转。编译结束后再一次性应用 AggregateFrame。
			Layer.LocalTransform = SourceLayerTransform;
			// 保留真实连接木体；若木叶占用同一体素，统一编译器会按
			// leaf=100、wood=10 的优先级只输出叶片外壳。
			Layer.SolidMask = TreeSource->Mask.SolidMask;

			const FSourceOutputState* Output =
				SourceOutputs.Find(TreeSource->SourceId);
			if (!Output || !Output->Mask.Contains(1))
			{
				continue;
			}
			const FString OutputStableKey = MakeGroupKey(
				Output->MaterialId,
				Output->Color,
				TreeSource->Mask.CellSize,
				false,
				false);
			const int32 OutputMaterialIndex = WholeMaterials.IndexOfByPredicate(
				[&OutputStableKey](const FWholeObjectMaterial& Existing)
				{
					return Existing.StableKey == OutputStableKey;
				});
			if (OutputMaterialIndex == INDEX_NONE)
			{
				continue;
			}
			MatterFlux::WholeObject::FLayer& OutputLayer =
				WholeLayers.AddDefaulted_GetRef();
			OutputLayer.MaterialIndex = OutputMaterialIndex;
			// 残留物继承原 source 的空间优先级，但不继承碰撞；这样叶片
			// 灰烬仍会遮住同格木材，而燃烧不会凭空制造新的阻挡体。
			OutputLayer.Priority = SourceLayerPriority;
			OutputLayer.bEnableCollision = false;
			OutputLayer.Width = TreeSource->Mask.Width;
			OutputLayer.Height = TreeSource->Mask.Height;
			OutputLayer.CellSize = TreeSource->Mask.CellSize;
			OutputLayer.LocalTransform = SourceLayerTransform;
			OutputLayer.SolidMask = Output->Mask;
		}
		WholeLayers.RemoveAll(
			[](const MatterFlux::WholeObject::FLayer& Layer)
			{
				return !Layer.SolidMask.Contains(1);
			});
		if (WholeLayers.IsEmpty())
		{
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource* TreeSource
				: TreeSources)
			{
				if (TreeSource)
				{
					VolumeRenderedTreeSourceIds.Add(TreeSource->SourceId);
				}
			}
			continue;
		}
		MatterFlux::WholeObject::FBuildResult WholeMesh;
		FString WholeObjectError;
		if (MatterFlux::WholeObject::BuildMesh(
				WholeLayers,
				WholeMesh,
				&WholeObjectError))
		{
			for (const MatterFlux::WholeObject::FMeshSection& Section
				: WholeMesh.Sections)
			{
				if (!WholeMaterials.IsValidIndex(Section.MaterialIndex))
				{
					continue;
				}
				const FWholeObjectMaterial& Material =
					WholeMaterials[Section.MaterialIndex];
				// 顶面必须使用基础颜色；旧代码把 Top/Bottom 一并当成
				// Side 再压暗一次，导致斜俯视中本应朝上的亮面成为深色
				// 菱形，整棵树看起来像向内凹。Bottom 已由法线材质压暗。
				const bool bSide = Section.FaceRole
					== MatterFlux::WholeObject::EFaceRole::Side;
				const float CellSize = WholeLayers[0].CellSize;
				const FString Key = MakeGroupKey(
					Material.MaterialId,
					Material.Color,
					CellSize,
					bSide,
					Section.bEnableCollision,
					bGhostAggregate,
					GhostSourceId);
				FProxyMeshGroup& Group = Groups.FindOrAdd(Key);
				Group.MaterialId = Material.MaterialId;
				Group.Color = Material.Color;
				Group.CellSize = CellSize;
				Group.bSide = bSide;
				Group.bCollision = Section.bEnableCollision;
				Group.bGhost = bGhostAggregate;
				Group.GhostSourceId = GhostSourceId;
				AppendMeshPart(
					Group,
					AggregateFrame,
					Section.Vertices,
					Section.Triangles,
					Section.Normals,
					Section.UVs,
					0,
					Section.Triangles.Num(),
					&Section.VertexColors);
			}
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource* TreeSource
				: TreeSources)
			{
				if (TreeSource)
				{
					VolumeRenderedTreeSourceIds.Add(TreeSource->SourceId);
				}
			}
			continue;
		}
		UE_LOG(
			LogMatterFlux,
			Warning,
			TEXT("Tree aggregate %s could not use unified voxel rendering: %s"),
			*Pair.Key.ToString(),
			*WholeObjectError);

		const MatterFlux::PlayableLevel::FLevelFragmentSource& First =
			*TreeSources[0];
		const float CellSize = First.Mask.CellSize;
		bool bCompatibleVolume = FMath::IsFinite(CellSize) && CellSize > 0.0f;
		FVector Anchor = FVector::ZeroVector;
		bool bHasAnchor = false;
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource* TreeSource
			: TreeSources)
		{
			if (!TreeSource
				|| !FMath::IsNearlyEqual(
					TreeSource->Mask.CellSize, CellSize, KINDA_SMALL_NUMBER)
				|| !TreeSource->Transform.GetScale3D().Equals(
					FVector::OneVector, KINDA_SMALL_NUMBER))
			{
				bCompatibleVolume = false;
				break;
			}
			for (int32 MaskY = 0;
				!bHasAnchor && MaskY < TreeSource->Mask.Height;
				++MaskY)
			{
				for (int32 MaskX = 0;
					MaskX < TreeSource->Mask.Width;
					++MaskX)
				{
					if (TreeSource->Mask.SolidMask[
						MaskY * TreeSource->Mask.Width + MaskX] == 0)
					{
						continue;
					}
					const FVector LocalCenter(
						(static_cast<float>(MaskX) + 0.5f
							- static_cast<float>(TreeSource->Mask.Width) * 0.5f)
							* CellSize,
						0.0f,
						(static_cast<float>(MaskY) + 0.5f
							- static_cast<float>(TreeSource->Mask.Height) * 0.5f)
							* CellSize);
					Anchor = TreeSource->Transform.TransformPosition(LocalCenter);
					bHasAnchor = true;
					break;
				}
			}
		}
		if (!bCompatibleVolume || !bHasAnchor)
		{
			continue;
		}

		struct FTreeVoxelMaterial
		{
			FName MaterialId = NAME_None;
			FLinearColor Color = FLinearColor::White;
			bool bCollision = false;
			bool bLeaf = false;
			TSet<FIntVector> Cells;
		};
		TArray<FTreeVoxelMaterial> TreeMaterials;
		TMap<FString, int32> MaterialIndexByKey;
		TSet<FIntVector> AllOccupiedCells;
		TSet<FIntVector> LeafCells;
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource* TreeSource
			: TreeSources)
		{
			const bool bLeaf = TreeSource->Name == TEXT("TreeLeaves");
			const FString MaterialKey = MakeGroupKey(
				TreeSource->MaterialId,
				TreeSource->Color,
				CellSize,
				false,
				TreeSource->bEnableCollision);
			int32* MaterialIndex = MaterialIndexByKey.Find(MaterialKey);
			if (!MaterialIndex)
			{
				const int32 NewIndex = TreeMaterials.AddDefaulted();
				MaterialIndexByKey.Add(MaterialKey, NewIndex);
				TreeMaterials[NewIndex].MaterialId = TreeSource->MaterialId;
				TreeMaterials[NewIndex].Color = TreeSource->Color;
				TreeMaterials[NewIndex].bCollision = TreeSource->bEnableCollision;
				TreeMaterials[NewIndex].bLeaf = bLeaf;
				MaterialIndex = MaterialIndexByKey.Find(MaterialKey);
			}
			FTreeVoxelMaterial& Material = TreeMaterials[*MaterialIndex];
			for (int32 MaskY = 0; MaskY < TreeSource->Mask.Height; ++MaskY)
			{
				for (int32 MaskX = 0; MaskX < TreeSource->Mask.Width; ++MaskX)
				{
					if (TreeSource->Mask.SolidMask[
						MaskY * TreeSource->Mask.Width + MaskX] == 0)
					{
						continue;
					}
					const FVector LocalCenter(
						(static_cast<float>(MaskX) + 0.5f
							- static_cast<float>(TreeSource->Mask.Width) * 0.5f)
							* CellSize,
						0.0f,
						(static_cast<float>(MaskY) + 0.5f
							- static_cast<float>(TreeSource->Mask.Height) * 0.5f)
							* CellSize);
					const FVector GridPosition =
						(TreeSource->Transform.TransformPosition(LocalCenter) - Anchor)
						/ CellSize;
					const FIntVector Cell(
						FMath::RoundToInt(GridPosition.X),
						FMath::RoundToInt(GridPosition.Y),
						FMath::RoundToInt(GridPosition.Z));
					if (!GridPosition.Equals(FVector(Cell), 0.01f))
					{
						bCompatibleVolume = false;
						break;
					}
					Material.Cells.Add(Cell);
					AllOccupiedCells.Add(Cell);
					if (bLeaf)
					{
						LeafCells.Add(Cell);
					}
				}
				if (!bCompatibleVolume)
				{
					break;
				}
			}
			if (!bCompatibleVolume)
			{
				break;
			}
		}
		if (!bCompatibleVolume)
		{
			continue;
		}

		const TArray<FIntVector> OccluderCells = AllOccupiedCells.Array();
		for (FTreeVoxelMaterial& Material : TreeMaterials)
		{
			TArray<FIntVector> SurfaceCells;
			for (const FIntVector& Cell : Material.Cells)
			{
				if (!Material.bLeaf && LeafCells.Contains(Cell))
				{
					continue;
				}
				const bool bExposed =
					!AllOccupiedCells.Contains(Cell + FIntVector(1, 0, 0))
					|| !AllOccupiedCells.Contains(Cell + FIntVector(-1, 0, 0))
					|| !AllOccupiedCells.Contains(Cell + FIntVector(0, 1, 0))
					|| !AllOccupiedCells.Contains(Cell + FIntVector(0, -1, 0))
					|| !AllOccupiedCells.Contains(Cell + FIntVector(0, 0, 1))
					|| !AllOccupiedCells.Contains(Cell + FIntVector(0, 0, -1));
				if (bExposed)
				{
					SurfaceCells.Add(Cell);
				}
			}
			if (SurfaceCells.IsEmpty())
			{
				continue;
			}
			FCachedSourceMesh VolumeMesh;
			if (!MatterFlux::FragmentGeometry::
				BuildVoxelBlockMeshFromCellsWithOccluders(
					SurfaceCells,
					OccluderCells,
					CellSize,
					VolumeMesh.Vertices,
					VolumeMesh.Triangles,
					VolumeMesh.Normals,
					VolumeMesh.UVs,
					VolumeMesh.FaceIndexCount))
			{
				bCompatibleVolume = false;
				break;
			}
			for (const bool bSide : {false, true})
			{
				const FString Key = MakeGroupKey(
					Material.MaterialId,
					Material.Color,
					CellSize,
					bSide,
					Material.bCollision,
					bGhostAggregate,
					GhostSourceId);
				FProxyMeshGroup& Group = Groups.FindOrAdd(Key);
				Group.MaterialId = Material.MaterialId;
				Group.Color = Material.Color;
				Group.CellSize = CellSize;
				Group.bSide = bSide;
				Group.bCollision = Material.bCollision;
				Group.bGhost = bGhostAggregate;
				Group.GhostSourceId = GhostSourceId;
				AppendMeshPart(
					Group,
					FTransform(Anchor),
					VolumeMesh.Vertices,
					VolumeMesh.Triangles,
					VolumeMesh.Normals,
					VolumeMesh.UVs,
					bSide ? VolumeMesh.FaceIndexCount : 0,
					bSide
						? VolumeMesh.Triangles.Num() - VolumeMesh.FaceIndexCount
						: VolumeMesh.FaceIndexCount);
			}
		}
		if (!bCompatibleVolume)
		{
			continue;
		}
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource* TreeSource
			: TreeSources)
		{
			VolumeRenderedTreeSourceIds.Add(TreeSource->SourceId);
		}
	}

	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: *Sources)
	{
		if (DebugIsolatedAggregateId.IsValid()
			&& Source.AggregateId != DebugIsolatedAggregateId)
		{
			continue;
		}
		if (MaterializedSourceIds.Contains(Source.SourceId))
		{
			continue;
		}
		const auto AppendSource =
			[this, &Groups, &Source](
				const FCachedSourceMesh& Cached,
				const FName MaterialId,
				const FLinearColor& Color,
				const bool bCollision)
		{
				for (const bool bSide : { false, true })
				{
					const bool bGhost =
						GhostedSourceIds.Contains(Source.SourceId);
					const FString Key = MakeGroupKey(
						MaterialId,
						Color,
						Source.Mask.CellSize,
						bSide,
						bCollision,
						bGhost,
						Source.SourceId);
					FProxyMeshGroup& Group = Groups.FindOrAdd(Key);
					Group.MaterialId = MaterialId;
					Group.Color = Color;
					Group.CellSize = Source.Mask.CellSize;
					Group.bSide = bSide;
					Group.bCollision = bCollision;
					Group.bGhost = bGhost;
					Group.GhostSourceId = bGhost
						? Source.SourceId : FGuid();
					AppendMeshPart(
						Group,
						Source.Transform,
						Cached.Vertices,
						Cached.Triangles,
						Cached.Normals,
						Cached.UVs,
						bSide ? Cached.FaceIndexCount : 0,
						bSide
							? Cached.Triangles.Num()
								- Cached.FaceIndexCount
							: Cached.FaceIndexCount);
				}
			};
		if (!VolumeRenderedTreeSourceIds.Contains(Source.SourceId))
		{
			if (const FCachedSourceMesh* Cached = FindOrBuildSourceMesh(Source))
			{
				AppendSource(
					*Cached,
					Source.MaterialId,
					Source.Color,
					Source.bEnableCollision);
			}
		}
		// A successfully compiled aggregate already contains both the source
		// material and any reaction-output layers. Gate both fallback paths on
		// the same success marker so residue can never be appended as a second
		// coplanar projection.
		if (!VolumeRenderedTreeSourceIds.Contains(Source.SourceId))
		{
			if (const FSourceOutputState* Output =
				SourceOutputs.Find(Source.SourceId))
			{
				if (const FCachedSourceMesh* Cached =
					FindOrBuildOutputMesh(Source, *Output))
				{
					AppendSource(
						*Cached,
						Output->MaterialId,
						Output->Color,
						false);
					StandaloneTreeOutputProjectionCount +=
						(Source.Name == TEXT("TreeTrunk")
							|| Source.Name == TEXT("TreeBranch")
							|| Source.Name == TEXT("TreeLeaves"))
						? 1
						: 0;
				}
			}
		}
	}
	StandaloneTreeOutputProjectionCounts.Add(
		Chunk,
		StandaloneTreeOutputProjectionCount);
	if (Groups.IsEmpty())
	{
		if (UProceduralMeshComponent* Existing = ChunkMeshes.FindRef(Chunk))
		{
			Existing->ClearAllMeshSections();
			Existing->SetVisibility(false, true);
			Existing->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		CollisionChunks.Remove(Chunk);
		return;
	}

	UProceduralMeshComponent* Mesh = ChunkMeshes.FindRef(Chunk);
	if (!IsValid(Mesh))
	{
		const FName ComponentName = MakeUniqueObjectName(
			GetOwner(),
			UProceduralMeshComponent::StaticClass(),
			*FString::Printf(
				TEXT("FragmentSourceProxy_%d_%d"),
				Chunk.X,
				Chunk.Y));
		Mesh = NewObject<UProceduralMeshComponent>(
			GetOwner(),
			ComponentName);
		Mesh->SetupAttachment(AttachParent);
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->bUseComplexAsSimpleCollision = true;
		Mesh->SetCollisionObjectType(ECC_WorldStatic);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(true);
		Mesh->ComponentTags.AddUnique(
			TEXT("MatterFluxFragmentSourceProxy"));
		GetOwner()->AddInstanceComponent(Mesh);
		Mesh->RegisterComponent();
		ChunkMeshes.Add(Chunk, Mesh);
	}
	else
	{
		Mesh->ClearAllMeshSections();
	}
	Mesh->SetVisibility(true, true);

	TArray<FString> Keys;
	Groups.GenerateKeyArray(Keys);
	Keys.Sort();
	int32 SectionIndex = 0;
	bool bHasCollision = false;
	for (const FString& Key : Keys)
	{
		FProxyMeshGroup& Group = Groups.FindChecked(Key);
		Mesh->CreateMeshSection(
			SectionIndex,
			Group.Vertices,
			Group.Triangles,
			Group.Normals,
			Group.UVs,
			Group.VertexColors,
			TArray<FProcMeshTangent>(),
			Group.bCollision);
		bHasCollision |= Group.bCollision;
		Mesh->SetMaterial(
			SectionIndex,
			Group.bGhost
				? FindOrCreateGhostMaterial(
					Group.GhostSourceId, Group.Color, Group.CellSize)
				: FindOrCreateMaterial(
					Group.MaterialId,
					Group.Color,
					Group.CellSize,
					Group.bSide));
		++SectionIndex;
	}
	Mesh->SetCollisionEnabled(
		bHasCollision
			? ECollisionEnabled::QueryAndPhysics
			: ECollisionEnabled::NoCollision);
	Mesh->SetCollisionResponseToAllChannels(
		bHasCollision ? ECR_Block : ECR_Ignore);
	if (bHasCollision)
	{
		CollisionChunks.Add(Chunk);
	}
	else
	{
		CollisionChunks.Remove(Chunk);
	}
}

const UMatterFluxFragmentSourceProxyComponent::FCachedSourceMesh*
	UMatterFluxFragmentSourceProxyComponent::FindOrBuildSourceMesh(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
{
	if (const FCachedSourceMesh* Existing =
		CachedSourceMeshes.Find(Source.SourceId))
	{
		return Existing;
	}
	MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
	if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		Source.Mask.SolidMask,
		Source.Mask.Width,
		Source.Mask.Height,
		Source.Mask.CellSize,
		Geometry))
	{
		return nullptr;
	}
	FCachedSourceMesh Mesh;
	const bool bBuilt = Source.Mask.GeometryStyle
		== EFragmentSourceGeometryStyle::RadialColumn
		? MatterFlux::FragmentGeometry::BuildRadialColumnMeshFromMask(
			Source.Mask.SolidMask,
			Source.Mask.Width,
			Source.Mask.Height,
			Source.Mask.CellSize,
			Mesh.Vertices,
			Mesh.Triangles,
			Mesh.Normals,
			Mesh.UVs,
			Mesh.FaceIndexCount)
		: Source.Mask.GeometryStyle == EFragmentSourceGeometryStyle::VoxelBlocks
		? MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromMask(
			Source.Mask.SolidMask,
			Source.Mask.Width,
			Source.Mask.Height,
			Source.Mask.CellSize,
			Mesh.Vertices,
			Mesh.Triangles,
			Mesh.Normals,
			Mesh.UVs,
			Mesh.FaceIndexCount)
		: MatterFlux::FragmentGeometry::BuildExtrudedMesh(
			Geometry.Vertices2D,
			Geometry.TriangleIndices,
			Geometry.OuterContours,
			Geometry.HoleContours,
			Source.Mask.CellSize,
			Mesh.Vertices,
			Mesh.Triangles,
			Mesh.Normals,
			Mesh.UVs);
	if (!bBuilt)
	{
		return nullptr;
	}
	if (Source.Mask.GeometryStyle
		== EFragmentSourceGeometryStyle::ExtrudedMask)
	{
		Mesh.FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
	}
	if (Mesh.FaceIndexCount <= 0
		|| Mesh.FaceIndexCount >= Mesh.Triangles.Num())
	{
		return nullptr;
	}
	return &CachedSourceMeshes.Add(Source.SourceId, MoveTemp(Mesh));
}

const UMatterFluxFragmentSourceProxyComponent::FCachedSourceMesh*
	UMatterFluxFragmentSourceProxyComponent::FindOrBuildOutputMesh(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const FSourceOutputState& Output)
{
	if (const FCachedSourceMesh* Existing =
		CachedOutputMeshes.Find(Source.SourceId))
	{
		return Existing;
	}
	MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
	if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		Output.Mask,
		Source.Mask.Width,
		Source.Mask.Height,
		Source.Mask.CellSize,
		Geometry))
	{
		return nullptr;
	}
	FCachedSourceMesh Mesh;
	const bool bBuilt = Source.Mask.GeometryStyle
		== EFragmentSourceGeometryStyle::RadialColumn
		? MatterFlux::FragmentGeometry::BuildRadialColumnMeshFromMask(
			Output.Mask,
			Source.Mask.Width,
			Source.Mask.Height,
			Source.Mask.CellSize,
			Mesh.Vertices,
			Mesh.Triangles,
			Mesh.Normals,
			Mesh.UVs,
			Mesh.FaceIndexCount)
		: Source.Mask.GeometryStyle == EFragmentSourceGeometryStyle::VoxelBlocks
		? MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromMask(
			Output.Mask,
			Source.Mask.Width,
			Source.Mask.Height,
			Source.Mask.CellSize,
			Mesh.Vertices,
			Mesh.Triangles,
			Mesh.Normals,
			Mesh.UVs,
			Mesh.FaceIndexCount)
		: MatterFlux::FragmentGeometry::BuildExtrudedMesh(
			Geometry.Vertices2D,
			Geometry.TriangleIndices,
			Geometry.OuterContours,
			Geometry.HoleContours,
			Source.Mask.CellSize,
			Mesh.Vertices,
			Mesh.Triangles,
			Mesh.Normals,
			Mesh.UVs);
	if (!bBuilt)
	{
		return nullptr;
	}
	if (Source.Mask.GeometryStyle
		== EFragmentSourceGeometryStyle::ExtrudedMask)
	{
		Mesh.FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
	}
	if (Mesh.FaceIndexCount <= 0
		|| Mesh.FaceIndexCount >= Mesh.Triangles.Num())
	{
		return nullptr;
	}
	return &CachedOutputMeshes.Add(Source.SourceId, MoveTemp(Mesh));
}

void UMatterFluxFragmentSourceProxyComponent::DestroyChunk(
	const FIntPoint Chunk)
{
	if (UProceduralMeshComponent* Mesh = ChunkMeshes.FindRef(Chunk))
	{
		Mesh->DestroyComponent();
		const FName RetiredName = MakeUniqueObjectName(
			GetTransientPackage(),
			UProceduralMeshComponent::StaticClass(),
			TEXT("RetiredMatterFluxFragmentSourceProxy"));
		Mesh->Rename(
			*RetiredName.ToString(),
			GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional);
	}
	ChunkMeshes.Remove(Chunk);
	CollisionChunks.Remove(Chunk);
}

void UMatterFluxFragmentSourceProxyComponent::RemoveSourceChunk(
	const FIntPoint Chunk)
{
	if (const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
		SourceChunks.Find(Chunk))
	{
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: *Sources)
		{
			SourceLocatorById.Remove(Source.SourceId);
			MaterializedSourceIds.Remove(Source.SourceId);
			TargetGhostedSourceIds.Remove(Source.SourceId);
			GhostedSourceIds.Remove(Source.SourceId);
			GhostOpacityBySourceId.Remove(Source.SourceId);
			ReactingSourceIds.Remove(Source.SourceId);
			CachedSourceMeshes.Remove(Source.SourceId);
			SourceOutputs.Remove(Source.SourceId);
			CachedOutputMeshes.Remove(Source.SourceId);
			RemoveGhostMaterials(Source.SourceId);
		}
	}
	SourceChunks.Remove(Chunk);
	DirtyChunks.Remove(Chunk);
	DeferredReactionChunks.Remove(Chunk);
	DestroyChunk(Chunk);
	SetComponentTickEnabled(!GhostedSourceIds.IsEmpty());
}

UMaterialInstanceDynamic*
	UMatterFluxFragmentSourceProxyComponent::FindOrCreateMaterial(
		const FName MaterialId,
		const FLinearColor& Color,
		const float CellSize,
		const bool bSide)
{
	const bool bLeaf = MaterialId == TEXT("leaf");
	const bool bWood = MaterialId == TEXT("wood");
	UMaterialInterface* SelectedTemplate =
		bLeaf && LeafMaterialTemplate
			? LeafMaterialTemplate.Get()
			: (bWood && WoodMaterialTemplate
				? WoodMaterialTemplate.Get()
				: MaterialTemplate.Get());
	if (!SelectedTemplate)
	{
		return nullptr;
	}
	const FName Key(*MakeGroupKey(
		MaterialId,
		Color,
		CellSize,
		bSide));
	if (UMaterialInstanceDynamic* Existing = Materials.FindRef(Key))
	{
		return Existing;
	}
	UMaterialInstanceDynamic* Material =
		UMaterialInstanceDynamic::Create(SelectedTemplate, this);
	MatterFlux::Rendering::ApplyVoxelMaterialProjection(
		*Material,
		MatterFlux::Rendering::ResolveVoxelMaterialProjection(
			Color,
			MaterialId,
			CellSize,
			bSide
				? MatterFlux::Rendering::EVoxelMaterialFaceRole::Side
				: MatterFlux::Rendering::EVoxelMaterialFaceRole::Primary));
	Materials.Add(Key, Material);
	return Material;
}

UMaterialInstanceDynamic*
	UMatterFluxFragmentSourceProxyComponent::FindOrCreateGhostMaterial(
		const FGuid& GhostSourceId,
		const FLinearColor& Color,
		const float CellSize)
{
	if (!GhostMaterialTemplate)
	{
		return nullptr;
	}
	const FName Key(*FString::Printf(
		TEXT("%s_%08x_%08x"),
		*GhostSourceId.ToString(EGuidFormats::Digits),
		Color.ToFColor(false).DWColor(),
		GetTypeHash(CellSize)));
	if (UMaterialInstanceDynamic* Existing = GhostMaterials.FindRef(Key))
	{
		Existing->SetScalarParameterValue(
			TEXT("Opacity"),
			GhostOpacityBySourceId.FindRef(GhostSourceId));
		return Existing;
	}
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(
		GhostMaterialTemplate, this);
	Material->SetVectorParameterValue(
		TEXT("Color"), FLinearColor(Color.R, Color.G, Color.B, 1.0f));
	Material->SetScalarParameterValue(
		TEXT("Opacity"),
		GhostOpacityBySourceId.FindRef(GhostSourceId));
	Material->SetScalarParameterValue(TEXT("FaceContrast"), 0.68f);
	Material->SetScalarParameterValue(
		TEXT("PixelSize"), FMath::Max(CellSize, 10.0f));
	Material->SetScalarParameterValue(TEXT("Roughness"), 0.82f);
	Material->SetScalarParameterValue(TEXT("ShadowLift"), 0.32f);
	GhostMaterials.Add(Key, Material);
	GhostMaterialSourceIds.Add(Key, GhostSourceId);
	return Material;
}

void UMatterFluxFragmentSourceProxyComponent::UpdateGhostMaterialOpacity(
	const FGuid& SourceId,
	const float Opacity)
{
	for (const TPair<FName, FGuid>& Pair : GhostMaterialSourceIds)
	{
		if (Pair.Value == SourceId)
		{
			if (UMaterialInstanceDynamic* Material =
				GhostMaterials.FindRef(Pair.Key))
			{
				Material->SetScalarParameterValue(TEXT("Opacity"), Opacity);
			}
		}
	}
}

void UMatterFluxFragmentSourceProxyComponent::RemoveGhostMaterials(
	const FGuid& SourceId)
{
	for (auto It = GhostMaterialSourceIds.CreateIterator(); It; ++It)
	{
		if (It.Value() == SourceId)
		{
			GhostMaterials.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}
