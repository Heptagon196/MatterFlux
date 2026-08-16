#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "MatterFluxFragmentSourceProxyComponent.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UProceduralMeshComponent;
class USceneComponent;

enum class EMatterFluxFragmentSourceProxyApplyResult : uint8
{
	Invalid,
	Unchanged,
	Changed
};

/**
 * Renders pristine mask sources and their static collision in chunk batches.
 * Logical sources remain individually addressable without allocating an Actor;
 * a source is removed from its batch only while an interactive Actor owns its
 * mutable gameplay state.
 */
UCLASS(ClassGroup = (MatterFlux), meta = (BlueprintSpawnableComponent))
class MATTERFLUX_API UMatterFluxFragmentSourceProxyComponent
	: public UActorComponent
{
	GENERATED_BODY()

public:
	UMatterFluxFragmentSourceProxyComponent();

	void Configure(
		USceneComponent* InAttachParent,
		UMaterialInterface* InMaterialTemplate);
	void SetSourceChunks(const TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& InChunks);
	void PrepareSourceChunks(int32 MaximumPreparedChunkCount);
	void SetVisibleChunks(const TSet<FIntPoint>& InVisibleChunks);
	void SetSourceMaterialized(const FGuid& SourceId, bool bMaterialized);
	void FlushDeferredCombustionChanges();
	EMatterFluxFragmentSourceProxyApplyResult ApplySourceState(
		const FGuid& SourceId,
		const TArray<uint8>& RuntimeMask,
		const TArray<uint8>& ResidueMask,
		FName ResidueMaterialId,
		const FLinearColor& ResidueColor,
		bool bCombustionActive);
	void FlushPendingChanges();
	void ResetSources();

	int32 GetVisibleSourceCount() const;
	int32 GetVisibleChunkCount() const { return VisibleChunks.Num(); }
	bool IsProxySource(const FGuid& SourceId) const;

private:
	struct FCachedSourceMesh
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		int32 FaceIndexCount = 0;
	};
	struct FSourceResidueState
	{
		TArray<uint8> Mask;
		FName MaterialId = NAME_None;
		FLinearColor Color = FLinearColor::White;
	};
	struct FSourceLocator
	{
		FIntPoint Chunk = FIntPoint::ZeroValue;
		int32 SourceIndex = INDEX_NONE;
	};

	void RebuildChunk(FIntPoint Chunk);
	void DestroyChunk(FIntPoint Chunk);
	void SetSourceCombustionActive(const FGuid& SourceId, bool bActive);
	const FCachedSourceMesh* FindOrBuildSourceMesh(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source);
	const FCachedSourceMesh* FindOrBuildResidueMesh(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const FSourceResidueState& Residue);
	UMaterialInstanceDynamic* FindOrCreateMaterial(
		FName MaterialId,
		const FLinearColor& Color,
		float CellSize,
		bool bSide);

	TMap<FIntPoint, TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>
		SourceChunks;
	TMap<FGuid, FSourceLocator> SourceLocatorById;
	TSet<FIntPoint> VisibleChunks;
	TSet<FIntPoint> CollisionChunks;
	TSet<FGuid> MaterializedSourceIds;
	TSet<FGuid> CombustingSourceIds;
	TSet<FIntPoint> DirtyChunks;
	TSet<FIntPoint> DeferredCombustionChunks;
	TMap<FGuid, FCachedSourceMesh> CachedSourceMeshes;
	TMap<FGuid, FSourceResidueState> SourceResidues;
	TMap<FGuid, FCachedSourceMesh> CachedResidueMeshes;

	UPROPERTY(Transient)
	TMap<FIntPoint, TObjectPtr<UProceduralMeshComponent>> ChunkMeshes;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> AttachParent;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> MaterialTemplate;
};
