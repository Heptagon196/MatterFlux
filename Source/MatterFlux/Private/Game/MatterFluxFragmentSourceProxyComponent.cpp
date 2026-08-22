#include "Game/MatterFluxFragmentSourceProxyComponent.h"

#include "Fragment/FragmentGeometry.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MatterFluxLog.h"
#include "ProceduralMeshComponent.h"
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
		const bool bCollision = false)
	{
		return FString::Printf(
			TEXT("%s|%08x|%08x|%s|%s"),
			*MaterialId.ToString(),
			Color.ToFColor(false).DWColor(),
			GetTypeHash(CellSize),
			bSide ? TEXT("side") : TEXT("face"),
			bCollision ? TEXT("collision") : TEXT("visual"));
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
	PrimaryComponentTick.bCanEverTick = false;
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
				&& !DeferredCombustionChunks.Contains(Chunk))
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
		DeferredCombustionChunks.Remove(Chunk);
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
		DeferredCombustionChunks.Remove(Chunk);
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
		SetSourceCombustionActive(SourceId, false);
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
		DeferredCombustionChunks.Remove(Locator->Chunk);
	}
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

void UMatterFluxFragmentSourceProxyComponent::SetSourceCombustionActive(
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
		CombustingSourceIds.Add(SourceId);
		return;
	}
	if (CombustingSourceIds.Remove(SourceId) > 0)
	{
		DeferredCombustionChunks.Add(Locator->Chunk);
	}
}

void UMatterFluxFragmentSourceProxyComponent::
	FlushDeferredCombustionChanges()
{
	for (const FIntPoint Chunk : DeferredCombustionChunks)
	{
		DirtyChunks.Add(Chunk);
	}
	DeferredCombustionChunks.Reset();
}

EMatterFluxFragmentSourceProxyApplyResult
UMatterFluxFragmentSourceProxyComponent::ApplySourceState(
	const FGuid& SourceId,
	const TArray<uint8>& RuntimeMask,
	const TArray<uint8>& ResidueMask,
	const FName ResidueMaterialId,
	const FLinearColor& ResidueColor,
	const bool bCombustionActive)
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
	if (RuntimeMask.Num() != CellCount || ResidueMask.Num() != CellCount)
	{
		return EMatterFluxFragmentSourceProxyApplyResult::Invalid;
	}
	bool bHasResidue = false;
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		if (RuntimeMask[CellIndex] > 1 || ResidueMask[CellIndex] > 1)
		{
			return EMatterFluxFragmentSourceProxyApplyResult::Invalid;
		}
		bHasResidue |= ResidueMask[CellIndex] != 0;
	}

	const bool bRuntimeChanged = Source->Mask.SolidMask != RuntimeMask;
	const FSourceResidueState* ExistingResidue =
		SourceResidues.Find(SourceId);
	const bool bResidueChanged = bHasResidue
		? !ExistingResidue
			|| ExistingResidue->Mask != ResidueMask
			|| ExistingResidue->MaterialId != ResidueMaterialId
			|| !ExistingResidue->Color.Equals(ResidueColor)
		: ExistingResidue != nullptr;
	const bool bWasCombustionActive =
		CombustingSourceIds.Contains(SourceId);
	const bool bCombustionStateChanged =
		bWasCombustionActive != bCombustionActive;
	if (!bRuntimeChanged
		&& !bResidueChanged
		&& !bCombustionStateChanged)
	{
		return EMatterFluxFragmentSourceProxyApplyResult::Unchanged;
	}

	if (bRuntimeChanged)
	{
		Source->Mask.SolidMask = RuntimeMask;
		CachedSourceMeshes.Remove(SourceId);
	}
	if (bHasResidue)
	{
		FSourceResidueState& State = SourceResidues.FindOrAdd(SourceId);
		State.Mask = ResidueMask;
		State.MaterialId = ResidueMaterialId;
		State.Color = ResidueColor;
	}
	else
	{
		SourceResidues.Remove(SourceId);
	}
	if (bResidueChanged)
	{
		CachedResidueMeshes.Remove(SourceId);
	}
	if (bCombustionActive)
	{
		CombustingSourceIds.Add(SourceId);
	}
	else if (CombustingSourceIds.Remove(SourceId) > 0)
	{
		DeferredCombustionChunks.Add(Locator->Chunk);
	}
	// 燃烧状态与可视网格必须在同一个批次内提交。旧逻辑会在燃烧期间
	// 一直保留绿色的 chunk 网格，直到整份 source 燃尽才一次性变黑；
	// DirtyChunks 本身是集合，因此同一模拟批次内多格、多 source 的变化
	// 仍只会触发一次 chunk 重建。
	if (bRuntimeChanged || bResidueChanged)
	{
		DirtyChunks.Add(Locator->Chunk);
		DeferredCombustionChunks.Remove(Locator->Chunk);
	}
	return EMatterFluxFragmentSourceProxyApplyResult::Changed;
}

void UMatterFluxFragmentSourceProxyComponent::FlushPendingChanges()
{
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
	DebugIsolatedAggregateId.Invalidate();
	CombustingSourceIds.Reset();
	DirtyChunks.Reset();
	DeferredCombustionChunks.Reset();
	CachedSourceMeshes.Reset();
	SourceResidues.Reset();
	CachedResidueMeshes.Reset();
	Materials.Reset();
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

void UMatterFluxFragmentSourceProxyComponent::RebuildChunk(
	const FIntPoint Chunk)
{
	const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>* Sources =
		SourceChunks.Find(Chunk);
	if (!Sources || !AttachParent || !GetOwner()
		|| GetOwner()->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

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
		const bool bTreePart = Source.Name == TEXT("TreeTrunk")
			|| Source.Name == TEXT("TreeBranch")
			|| Source.Name == TEXT("TreeLeaves");
		if (!MaterializedSourceIds.Contains(Source.SourceId)
			&& bTreePart
			&& Source.AggregateId.IsValid()
			&& (Source.Name != TEXT("TreeLeaves")
				|| (Source.MaterialId == TEXT("leaf")
					&& Source.Mask.GeometryStyle
						== EFragmentSourceGeometryStyle::VoxelBlocks)))
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
			MatterFlux::WholeObject::FLayer& Layer =
				WholeLayers.AddDefaulted_GetRef();
			Layer.MaterialIndex = MaterialIndex;
			Layer.Priority = TreeSource->MaterialId == TEXT("leaf") ? 100 : 10;
			Layer.bEnableCollision = TreeSource->bEnableCollision;
			Layer.Width = TreeSource->Mask.Width;
			Layer.Height = TreeSource->Mask.Height;
			Layer.CellSize = TreeSource->Mask.CellSize;
			// 整棵树可以朝向镜头旋转，但编译器只需要处理树局部坐标中
			// 的 90 度格点旋转。编译结束后再一次性应用 AggregateFrame。
			Layer.LocalTransform =
				TreeSource->Transform.GetRelativeTransform(AggregateFrame);
			Layer.LocalTransform.SetScale3D(FVector::OneVector);
			// 保留真实连接木体；若木叶占用同一体素，统一编译器会按
			// leaf=100、wood=10 的优先级只输出叶片外壳。
			Layer.SolidMask = TreeSource->Mask.SolidMask;
		}
		WholeLayers.RemoveAll(
			[](const MatterFlux::WholeObject::FLayer& Layer)
			{
				return !Layer.SolidMask.Contains(1);
			});
		MatterFlux::WholeObject::FBuildResult WholeMesh;
		FString WholeObjectError;
		if (!WholeLayers.IsEmpty()
			&& MatterFlux::WholeObject::BuildMesh(
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
					Section.bEnableCollision);
				FProxyMeshGroup& Group = Groups.FindOrAdd(Key);
				Group.MaterialId = Material.MaterialId;
				Group.Color = Material.Color;
				Group.CellSize = CellSize;
				Group.bSide = bSide;
				Group.bCollision = Section.bEnableCollision;
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
					Material.bCollision);
				FProxyMeshGroup& Group = Groups.FindOrAdd(Key);
				Group.MaterialId = Material.MaterialId;
				Group.Color = Material.Color;
				Group.CellSize = CellSize;
				Group.bSide = bSide;
				Group.bCollision = Material.bCollision;
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
			[&Groups, &Source](
				const FCachedSourceMesh& Cached,
				const FName MaterialId,
				const FLinearColor& Color,
				const bool bCollision)
		{
				for (const bool bSide : { false, true })
				{
					const FString Key = MakeGroupKey(
						MaterialId,
						Color,
						Source.Mask.CellSize,
						bSide,
						bCollision);
					FProxyMeshGroup& Group = Groups.FindOrAdd(Key);
					Group.MaterialId = MaterialId;
					Group.Color = Color;
					Group.CellSize = Source.Mask.CellSize;
					Group.bSide = bSide;
					Group.bCollision = bCollision;
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
		if (const FSourceResidueState* Residue =
			SourceResidues.Find(Source.SourceId))
		{
			if (const FCachedSourceMesh* Cached =
				FindOrBuildResidueMesh(Source, *Residue))
			{
				AppendSource(
					*Cached,
					Residue->MaterialId,
					Residue->Color,
					false);
			}
		}
	}
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
			FindOrCreateMaterial(
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
	UMatterFluxFragmentSourceProxyComponent::FindOrBuildResidueMesh(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const FSourceResidueState& Residue)
{
	if (const FCachedSourceMesh* Existing =
		CachedResidueMeshes.Find(Source.SourceId))
	{
		return Existing;
	}
	MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
	if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		Residue.Mask,
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
			Residue.Mask,
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
			Residue.Mask,
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
	return &CachedResidueMeshes.Add(Source.SourceId, MoveTemp(Mesh));
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
