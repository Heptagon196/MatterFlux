#include "Game/MatterFluxFragmentSourceProxyComponent.h"

#include "Fragment/FragmentGeometry.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"

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
		const int32 IndexCount)
	{
		if (FirstIndex < 0
			|| IndexCount <= 0
			|| FirstIndex > SourceTriangles.Num() - IndexCount
			|| SourceNormals.Num() != SourceVertices.Num()
			|| SourceUVs.Num() != SourceVertices.Num())
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
	UMaterialInterface* InMaterialTemplate)
{
	AttachParent = InAttachParent;
	if (MaterialTemplate != InMaterialTemplate)
	{
		MaterialTemplate = InMaterialTemplate;
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
	if (!bCombustionActive
		&& !bWasCombustionActive
		&& (bRuntimeChanged || bResidueChanged))
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
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: *Sources)
	{
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
		if (const FCachedSourceMesh* Cached = FindOrBuildSourceMesh(Source))
		{
			AppendSource(
				*Cached,
				Source.MaterialId,
				Source.Color,
				Source.bEnableCollision);
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
		Mesh->SetCastShadow(false);
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
			TArray<FColor>(),
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
	if (!MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		Geometry.Vertices2D,
		Geometry.TriangleIndices,
		Geometry.OuterContours,
		Geometry.HoleContours,
		Source.Mask.CellSize,
		Mesh.Vertices,
		Mesh.Triangles,
		Mesh.Normals,
		Mesh.UVs))
	{
		return nullptr;
	}
	Mesh.FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
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
	if (!MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		Geometry.Vertices2D,
		Geometry.TriangleIndices,
		Geometry.OuterContours,
		Geometry.HoleContours,
		Source.Mask.CellSize,
		Mesh.Vertices,
		Mesh.Triangles,
		Mesh.Normals,
		Mesh.UVs))
	{
		return nullptr;
	}
	Mesh.FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
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
	if (!MaterialTemplate)
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
		UMaterialInstanceDynamic::Create(MaterialTemplate, this);
	const bool bLeaf = MaterialId == TEXT("leaf");
	const bool bGrass = MaterialId == TEXT("grass");
	const bool bFlower =
		MaterialId == TEXT("flower_pink")
		|| MaterialId == TEXT("flower_gold")
		|| MaterialId == TEXT("flower_blue");
	const bool bStone = MaterialId == TEXT("stone");
	const bool bSoftDecoration = bLeaf || bGrass || bFlower;
	const float SideBrightness = bSoftDecoration ? 0.88f : 0.78f;
	const FLinearColor FinalColor = bSide
		? FLinearColor(
			Color.R * SideBrightness,
			Color.G * SideBrightness,
			Color.B * SideBrightness,
			Color.A)
		: Color;
	Material->SetVectorParameterValue(TEXT("Color"), FinalColor);
	Material->SetScalarParameterValue(
		TEXT("FaceContrast"),
		bFlower ? 0.42f
			: (bLeaf ? 0.56f : (bGrass ? 0.52f : (bStone ? 0.72f : 0.70f))));
	Material->SetScalarParameterValue(
		TEXT("ColorVariation"),
		bFlower ? 0.012f : (bLeaf ? 0.022f : 0.03f));
	Material->SetScalarParameterValue(TEXT("PixelSize"), FMath::Max(CellSize, 4.0f));
	Material->SetScalarParameterValue(
		TEXT("Roughness"),
		bStone ? 0.96f : (bLeaf ? 0.88f : 0.82f));
	Material->SetScalarParameterValue(
		TEXT("ShadowLift"),
		bFlower ? 0.38f
			: (bLeaf ? 0.32f : (bGrass ? 0.28f : (bStone ? 0.12f : 0.18f))));
	Materials.Add(Key, Material);
	return Material;
}
