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
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	void Configure(
		USceneComponent* InAttachParent,
		UMaterialInterface* InMaterialTemplate,
		UMaterialInterface* InLeafMaterialTemplate = nullptr,
		UMaterialInterface* InWoodMaterialTemplate = nullptr);
	void SetSourceChunks(const TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& InChunks);
	void ApplySourceChunkDelta(
		const TArray<FIntPoint>& RemovedChunks,
		const TMap<
			FIntPoint,
			TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>& UpdatedChunks);
	void PrepareSourceChunks(int32 MaximumPreparedChunkCount);
	void SetVisibleChunks(const TSet<FIntPoint>& InVisibleChunks);
	void SetSourceMaterialized(const FGuid& SourceId, bool bMaterialized);
	/** Local-only camera cutaway; logical source state and collision stay unchanged. */
	void SetGhostedSources(const TSet<FGuid>& SourceIds);
	bool HasActiveGhosting() const { return !GhostedSourceIds.IsEmpty(); }
	/** Visual-QA only: when valid, merged rendering is restricted to one aggregate. */
	void SetDebugIsolatedAggregate(const FGuid& AggregateId);
	void FlushDeferredReactionChanges();
	EMatterFluxFragmentSourceProxyApplyResult ApplySourceState(
		const FGuid& SourceId,
		const TArray<uint8>& RuntimeMask,
		const TArray<uint8>& OutputMask,
		FName OutputMaterialId,
		const FLinearColor& OutputColor,
		bool bReactionActive);
	void FlushPendingChanges();
	void ResetSources();

	int32 GetVisibleSourceCount() const;
	int32 GetVisibleChunkCount() const { return VisibleChunks.Num(); }
	bool IsProxySource(const FGuid& SourceId) const;
	/**
	 * Visual invariant audit: canonical input and reaction output must never
	 * occupy the same rendered source cell.
	 */
	int32 GetInputOutputOverlapCellCount() const;
	/** Tree residue must be compiled into the unified voxel object, not overlaid. */
	int32 GetStandaloneTreeOutputProjectionCount() const;

private:
	struct FCachedSourceMesh
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		int32 FaceIndexCount = 0;
	};
	struct FSourceOutputState
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
	void RemoveSourceChunk(FIntPoint Chunk);
	void SetSourceReactionActive(const FGuid& SourceId, bool bActive);
	const FCachedSourceMesh* FindOrBuildSourceMesh(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source);
	const FCachedSourceMesh* FindOrBuildOutputMesh(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const FSourceOutputState& Output);
	UMaterialInstanceDynamic* FindOrCreateMaterial(
		FName MaterialId,
		const FLinearColor& Color,
		float CellSize,
		bool bSide);
	UMaterialInstanceDynamic* FindOrCreateGhostMaterial(
		const FGuid& GhostSourceId,
		const FLinearColor& Color,
		float CellSize);
	void UpdateGhostMaterialOpacity(const FGuid& SourceId, float Opacity);
	void RemoveGhostMaterials(const FGuid& SourceId);

	TMap<FIntPoint, TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>
		SourceChunks;
	TMap<FGuid, FSourceLocator> SourceLocatorById;
	TSet<FIntPoint> VisibleChunks;
	TSet<FIntPoint> CollisionChunks;
	TSet<FGuid> MaterializedSourceIds;
	TSet<FGuid> TargetGhostedSourceIds;
	TSet<FGuid> GhostedSourceIds;
	TMap<FGuid, float> GhostOpacityBySourceId;
	FGuid DebugIsolatedAggregateId;
	TSet<FGuid> ReactingSourceIds;
	TSet<FIntPoint> DirtyChunks;
	TSet<FIntPoint> DeferredReactionChunks;
	TMap<FGuid, FCachedSourceMesh> CachedSourceMeshes;
	TMap<FGuid, FSourceOutputState> SourceOutputs;
	TMap<FGuid, FCachedSourceMesh> CachedOutputMeshes;
	TMap<FIntPoint, int32> StandaloneTreeOutputProjectionCounts;

	UPROPERTY(Transient)
	TMap<FIntPoint, TObjectPtr<UProceduralMeshComponent>> ChunkMeshes;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UMaterialInstanceDynamic>> GhostMaterials;
	TMap<FName, FGuid> GhostMaterialSourceIds;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> AttachParent;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> MaterialTemplate;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> LeafMaterialTemplate;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WoodMaterialTemplate;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> GhostMaterialTemplate;
};
