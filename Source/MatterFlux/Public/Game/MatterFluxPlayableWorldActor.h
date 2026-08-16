#pragma once

#include "CoreMinimal.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Fragment/FragmentSourceSpatialIndex.h"
#include "Game/MatterFluxFragmentSourceReplication.h"
#include "GameFramework/Actor.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Material/MatterFluxGroundCombustionRuntime.h"
#include "Material/MatterFluxLogicalSourceCombustionIndex.h"
#include "Material/MatterFluxSourceCombustionRuntime.h"
#include "Material/MatterFluxMaterialSimulationRuntime.h"
#include "Material/MatterFluxReplicatedMaterialState.h"
#include "Save/MatterFluxSaveTypes.h"
#include "MatterFluxPlayableWorldActor.generated.h"

class UDirectionalLightComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UInstancedStaticMeshComponent;
class USceneComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UMatterFluxFragmentSourceProxyComponent;
class AFragment2DSourceActor;
class AFragment2DActor;
class AMatterFluxGroundStateChunkActor;
class AMatterFluxPlayableWorldActor;

namespace MatterFlux::PlayableLevel
{
	struct FLevelLayer;
	struct FLevelFragmentSource;
	struct FLevelLayout;
}

UENUM(BlueprintType)
enum class EMatterFluxWorldGenerationPhase : uint8
{
	Idle,
	BuildingLayout,
	InitializingSimulation,
	BuildingStreaming,
	SpawningWorldObjects,
	Complete,
	Failed
};

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnMatterFluxWorldGenerationFinished,
	bool,
	const FString&);

UCLASS()
class MATTERFLUX_API AMatterFluxPlayableWorldActor : public AActor
{
	GENERATED_BODY()

public:
	AMatterFluxPlayableWorldActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

	void Regenerate(int32 NewSeed = 0);
	bool RequestRegenerateAsync(
		int32 NewSeed = 0,
		bool bForceExactSeed = false);
	bool IsGenerationInProgress() const;
	float GetGenerationProgress() const { return GenerationProgress; }
	const FString& GetGenerationStatusText() const
	{
		return GenerationStatusText;
	}
	EMatterFluxWorldGenerationPhase GetGenerationPhase() const
	{
		return GenerationPhase;
	}
	FOnMatterFluxWorldGenerationFinished& OnGenerationFinished()
	{
		return GenerationFinished;
	}
	bool CaptureSaveState(
		FMatterFluxWorldSaveState& OutState,
		FString& OutError) const;
	bool RestoreSaveState(
		const FMatterFluxWorldSaveState& State,
		FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Playable World")
	int32 GetMapSeed() const { return MapSeed; }

	UFUNCTION(BlueprintPure, Category = "Playable World")
	int32 GetGeneratedLayerCount() const { return GeneratedLayerInstances.Num(); }

	UFUNCTION(BlueprintPure, Category = "Playable World")
	int32 GetGeneratedFragmentSourceCount() const { return GeneratedFragmentSources.Num(); }

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetCachedFragmentSourceCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetVisibleFragmentSourceProxyCount() const;

	/** Materializes pristine mask proxies intersecting Bounds, then returns all active sources there. */
	void GatherFragmentSourcesInBounds(
		const FBox& Bounds,
		TArray<AFragment2DSourceActor*>& OutSources);
	int32 MaterializeFragmentSourcesForDamage(
		const FFragmentDamageShape& DamageShape);
	int32 MaterializeFragmentSourcesForFlame(
		const FVector& Start,
		const FVector& Direction,
		float Range,
		float StartRadius,
		float EndRadius,
		FName FlameMaterial);
	int32 IgniteLogicalFragmentSourcesInCone(
		const FVector& Start,
		const FVector& Direction,
		float Range,
		float StartRadius,
		float EndRadius,
		FName FlameMaterial,
		int32 EventSeed);
	int32 IgniteLogicalFragmentSourcesInBounds(
		const FBox& Bounds,
		const FVector& IgnitionPoint,
		FName FlameMaterial,
		int32 EventSeed);
	void MaterializeFragmentAggregate(const FGuid& AggregateId);
	bool DematerializeFragmentSource(const FGuid& SourceId);
	bool RetireFragmentSourceIntoDynamicAggregate(
		AFragment2DSourceActor& SourceActor,
		AFragment2DActor& CarrierActor);
	void NotifyDynamicAggregateOwnsSource(
		const FGuid& SourceId,
		AFragment2DActor* CarrierActor = nullptr);
	void ReleaseDynamicAggregateCarrier(
		const AFragment2DActor& CarrierActor);
	bool GetFragmentSourceRuntimeState(
		const FGuid& SourceId,
		int32& OutRevision,
		TArray<uint8>& OutRuntimeMask) const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetPendingFragmentSourceSpawnCount() const
	{
		return PendingFragmentSourceSpawns.Num();
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Material Simulation")
	int32 GetMaterialSimulationStep() const
	{
		return MaterialSimulation
			? MaterialSimulation->GetLogicalStep()
			: 0;
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Material Simulation")
	int32 GetReplicatedMaterialStateByteCount() const
	{
		return ReplicatedMaterialState.GetCompressedByteCount();
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Material Simulation")
	int32 GetAppliedMaterialStateRevision() const
	{
		return MaterialSimulation
			? MaterialSimulation->GetAppliedStateRevision()
			: INDEX_NONE;
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Material Simulation")
	int32 GetGeneratedMaterialLayerCount() const
	{
		return GeneratedMaterialInstances.Num();
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Material Simulation")
	int32 GetSimulatedMaterialCount(FName MaterialId) const;

	UFUNCTION(BlueprintCallable, Category = "Playable World|Streaming")
	void SetWorldStreamingFocus(const FVector& WorldLocation);

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetResidentMaterialChunkCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetArchivedMaterialChunkCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetMaterialSimulationFocusCount() const
	{
		return MaterialSimulation
			? MaterialSimulation->GetSimulationFocusCount()
			: 0;
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetVisibleTerrainInstanceCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetVisibleTerrainChunkCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetCachedTerrainChunkCount() const
	{
		return GeneratedTerrainChunks.Num();
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetVisibleTerrainTriangleCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetTerrainStreamingChunkRadius() const
	{
		return TerrainStreamingChunkRadius;
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetTerrainChunkCacheLimit() const
	{
		return TerrainChunkCacheLimit;
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetMaterialActiveChunkDiameter() const
	{
		return MaterialSimulationActiveChunkRadius * 2 + 1;
	}

	UFUNCTION(BlueprintCallable, Category = "Playable World|Combustion")
	bool IgniteFirstGeneratedTree(int32 EventSeed = 404);

	UFUNCTION(BlueprintCallable, Category = "Playable World|Combustion")
	bool IgniteGroundAtWorldLocation(
		const FVector& WorldLocation,
		int32 EventSeed);

	UFUNCTION(BlueprintPure, Category = "Playable World|Combustion")
	int32 GetCombustingSourceCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Combustion")
	int32 GetCombustionResidueCellCount() const;
	int32 GetLogicalCombustionResidueCellCount(FName MaterialId) const;
	int32 GetLogicalCombustionFuelCellCount(FName MaterialId) const;
	int32 GetLogicalCombustionSmokeEmissionCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Combustion")
	int32 GetScorchedGroundCellCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Combustion")
	int32 GetReplicatedGroundCombustionByteCount() const;

	bool ApplyReplicatedGroundStateChunk(
		const FMatterFluxGroundStateChunk& State);

	UFUNCTION(BlueprintPure, Category = "Playable World|Material Simulation")
	int32 GetMaterialSimulationMinHeight() const { return MaterialSimulationMinHeightCells; }

	UFUNCTION(BlueprintPure, Category = "Playable World|Material Simulation")
	int32 GetMaterialSimulationMaxHeight() const { return MaterialSimulationMaxHeightCells; }

	UMaterialInterface* GetVoxelColorMaterialTemplate() const
	{
		return VoxelColorMaterialTemplate;
	}

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable World")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable World")
	TObjectPtr<UDirectionalLightComponent> SunLight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable World")
	TObjectPtr<USkyLightComponent> SkyLight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable World")
	TObjectPtr<USkyAtmosphereComponent> SkyAtmosphere;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable World|Streaming")
	TObjectPtr<UMatterFluxFragmentSourceProxyComponent> FragmentSourceProxy;

protected:
	UPROPERTY(EditAnywhere, ReplicatedUsing = OnRep_MapSeed, Category = "Playable World", meta = (ClampMin = "1"))
	int32 MapSeed = 0;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "0.01", ClampMax = "0.25"))
	float MaterialSimulationStepSeconds = 0.05f;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "4", ClampMax = "256"))
	int32 MaterialSimulationChunkSize = 64;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "0", ClampMax = "8"))
	int32 MaterialSimulationActiveChunkRadius = 1;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "8", ClampMax = "128"))
	int32 TerrainStreamingChunkSize = 64;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "0", ClampMax = "5"))
	int32 TerrainStreamingChunkRadius = 4;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "9", ClampMax = "256"))
	int32 TerrainChunkCacheLimit = 192;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxTerrainChunkPrefetchesPerFrame = 2;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "0.5", ClampMax = "16.0"))
	float TerrainChunkPrefetchBudgetMilliseconds = 8.0f;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "9", ClampMax = "256"))
	int32 FragmentSourceProxyCacheLimit = 128;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation")
	int32 MaterialSimulationMinHeightCells = -64;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation")
	int32 MaterialSimulationMaxHeightCells = 64;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "4.0", ClampMax = "100.0"))
	float MaterialSimulationCellSize =
		MatterFlux::PlayableLevel::TerrainCellSize;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float MaterialVisualizationInterval = 0.10f;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation")
	bool bEnableMaterialSimulationCollision = false;

private:
	friend struct FMatterFluxReplicatedFragmentSourceStateList;
	UFUNCTION()
	void OnRep_MapSeed();

	UFUNCTION()
	void OnRep_MaterialSimulationState();

	UFUNCTION()
	void OnRep_FragmentSourceStates();

	void RebuildLevel();
	void SanitizeGenerationSettings();
	void AdvanceAsyncGeneration();
	void CompleteAsyncGeneration(bool bSuccess, const FString& Message);
	void RecaptureStaticSky();
	void HandleFragmentSourcePresenceChanged(
		const FGuid& SourceId,
		bool bMaterialized);
	void ApplyGeneratedLayoutSynchronously(
		const FMatterFluxContentRegistry& Registry,
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void InitializeMaterialSimulation(
		const FMatterFluxContentRegistry& Registry,
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void SeedMaterialSimulation(
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void ApplyReplicatedMaterialSimulationState();
	void PublishMaterialSimulationState();
	bool PublishFragmentSourceState(
		const FGuid& SourceId,
		const FFragment2DSourceStreamingState& State);
	bool PublishFragmentSourceStateBatch(
		TConstArrayView<FGuid> SourceIds);
	bool ArchiveFragmentSourceState(
		const FGuid& SourceId,
		const FFragment2DSourceStreamingState& State);
	void ApplyReplicatedFragmentSourceStates();
	bool ApplyReplicatedFragmentSourceState(
		const FMatterFluxReplicatedFragmentSourceState& Replicated);
	void RemoveReplicatedFragmentSourceState(const FGuid& SourceId);
	void MarkReplicatedFragmentSourceStatesDirty();
	void UpdateMaterialVisualization(float DeltaSeconds);
	void RebuildMaterialVisualization(
		const FMatterFluxContentRegistry& Registry);
	void DestroyMaterialVisualization();
	UInstancedStaticMeshComponent*
		FindOrCreateMaterialComponent(
			FName ComponentKey,
			FName MaterialId,
			const FMatterFluxMaterialDefinition& Material);
	void RebuildFragmentSources(
		const TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>& Sources,
		bool bImmediate = true);
	void RefreshVisibleFragmentSources(bool bImmediate = false);
	void ProcessPendingFragmentSourceSpawns();
	void SpawnFragmentSource(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source);
	void DestroyGeneratedFragmentSources();
	void PropagateCombustion(float DeltaSeconds);
	void AdvanceLogicalSourceCombustion(float DeltaSeconds);
	bool IgniteLogicalFragmentSource(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const FVector& WorldLocation,
		FName FlameMaterial,
		int32 EventSeed);
	const MatterFlux::PlayableLevel::FLevelFragmentSource*
		FindFragmentSourceDefinition(const FGuid& SourceId) const;
	void GatherLogicalFragmentSourceCandidates(
		const FBox& WorldBounds,
		TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>&
			OutCandidates) const;
	bool SynchronizeLogicalSourceCombustionState(
		const FGuid& SourceId,
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const MatterFlux::Combustion::FSourceCombustionRuntime& Runtime,
		bool bPublish);
	void RebuildLogicalSourceCombustionVisualization();
	void InitializeGroundCombustion(
		const FMatterFluxContentRegistry& Registry,
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void AdvanceGroundCombustion(float DeltaSeconds);
	void EnsureGroundCombustionVisuals(
		const FMatterFluxContentRegistry& Registry);
	void RebuildGroundCombustionVisualization();
	void ApplyGroundCombustionVisualChanges(
		TConstArrayView<int32> ChangedCellIndices);
	bool IsGroundCombustionCellVisible(int32 CellIndex) const;
	void InitializeGroundStateChunks();
	void DestroyGroundStateChunks();
	void PublishGroundCombustionState();
	void BuildLayerStreamingCache(
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void BuildTerrainStreamingCache(
		const MatterFlux::PlayableLevel::FLevelTerrain& Terrain);
	void GatherStreamingFocusChunks(
		TArray<FIntPoint>& OutFocusChunks) const;
	void GatherMaterialSimulationFocusCells(
		TArray<FIntPoint>& OutFocusCells) const;
	void RefreshVisibleLevelLayers(bool bForce);
	bool RefreshVisibleTerrainChunks(
		bool bForce,
		const TArray<FIntPoint>& FocusChunks);
	void ProcessPendingTerrainChunkPrefetches();
	UProceduralMeshComponent* CreateTerrainChunkComponent(
		FIntPoint ChunkCoordinate);
	void DestroyTerrainChunkMeshes();
	UHierarchicalInstancedStaticMeshComponent* FindOrCreateLayerComponent(
		const MatterFlux::PlayableLevel::FLevelLayer& Layer);

	struct FLayerStreamingCache
	{
		MatterFlux::PlayableLevel::FLevelLayer Layer;
		TMap<FIntPoint, TArray<FTransform>> ChunkInstances;
		TArray<FTransform> AlwaysLoadedInstances;
	};

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> GeneratedLayerInstances;
	TMap<FName, FLayerStreamingCache> LayerStreamingCaches;
	TArray<FIntPoint> VisibleLayerFocusChunks;
	MatterFlux::PlayableLevel::FLevelTerrain TerrainHeightField;

	UPROPERTY(Transient)
	TMap<FIntPoint, TObjectPtr<UProceduralMeshComponent>>
		GeneratedTerrainChunks;
	TSet<FIntPoint> DesiredTerrainChunks;
	TSet<FIntPoint> ActiveTerrainChunks;
	TArray<FIntPoint> PendingTerrainChunkPrefetches;
	TMap<FIntPoint, uint64> TerrainChunkLastUsed;
	uint64 TerrainChunkUseCounter = 0;
	bool bTerrainCacheCoversWholeMap = false;

	UPROPERTY(Transient)
	TMap<FGuid, TObjectPtr<AFragment2DSourceActor>> GeneratedFragmentSources;

	TMap<FIntPoint, TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>
		FragmentSourceChunks;
	MatterFlux::Fragment::FSourceSpatialIndex
		FragmentSourceDefinitionIndex;
	TMap<FGuid, FIntPoint> FragmentSourceChunkById;
	TMap<FGuid, FFragment2DSourceStreamingState>
		StreamedFragmentSourceStates;
	TArray<FIntPoint> VisibleFragmentFocusChunks;
	TSet<FGuid> RemovedFragmentSourceIds;
	TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>
		PendingFragmentSourceSpawns;
	TSet<FGuid> PendingFragmentSourceDespawns;
	TMap<FGuid, TWeakObjectPtr<AFragment2DActor>>
		DynamicAggregateCarriers;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxDecorationSpawnsPerFrame = 1;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "0.25", ClampMax = "16.0"))
	float DecorationSpawnBudgetMilliseconds = 4.0f;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UInstancedStaticMeshComponent>>
		GeneratedMaterialInstances;
	TMap<FName, uint64> MaterialVisualizationHashes;
	TMap<FName, FName> MaterialVisualizationMaterials;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> GroundResidueInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> GroundFlameInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> GroundSmokeInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> SourceFlameInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> SourceSmokeInstances;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundResidueMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundFlameMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundSmokeMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SourceFlameMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SourceSmokeMaterial;

	UPROPERTY(Replicated)
	int32 ReplicatedMaterialSimulationStep = 0;

	UPROPERTY(Replicated)
	FIntPoint ReplicatedMaterialSimulationFocus = FIntPoint::ZeroValue;

	UPROPERTY(ReplicatedUsing = OnRep_MaterialSimulationState)
	FMatterFluxReplicatedMaterialState ReplicatedMaterialState;

	UPROPERTY(ReplicatedUsing = OnRep_FragmentSourceStates)
	FMatterFluxReplicatedFragmentSourceStateList
		ReplicatedFragmentSourceStates;

	TSet<FGuid> AppliedReplicatedFragmentSourceIds;
	bool bReplicatedFragmentSourceStatesDirty = false;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AMatterFluxGroundStateChunkActor>>
		GroundStateChunkActors;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> SphereMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> ConeMesh;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> ColorMaterialTemplate;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> VoxelColorMaterialTemplate;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> VoxelGasMaterialTemplate;

	TUniquePtr<MatterFlux::Material::FSimulationRuntime>
		MaterialSimulation;
	TUniquePtr<MatterFlux::Combustion::FGroundCombustionRuntime>
		GroundCombustion;
	TMap<FGuid, TUniquePtr<
		MatterFlux::Combustion::FSourceCombustionRuntime>>
		ActiveSourceCombustions;
	MatterFlux::Combustion::FLogicalSourceCombustionIndex
		LogicalSourceCombustionIndex;
	TArray<FGuid> SourceCombustionActiveIdsScratch;
	TArray<FGuid> SourceCombustionFinishedIdsScratch;
	TArray<FGuid> SourceCombustionPublishIdsScratch;
	TArray<FMatterFluxFragmentSourceStateBatchUpdate>
		FragmentSourceReplicationUpdatesScratch;
	TArray<FVector> GroundSurfacePositions;
	TSet<FGuid> SourcesThatIgnitedGround;
	float MaterialVisualizationAccumulator = 0.0f;
	bool bMaterialVisualizationDirty = false;
	bool bMaterialVisualizationDeferredForStreaming = false;
	float CombustionPropagationAccumulator = 0.0f;
	float CombustionProxyFlushAccumulator = 0.0f;
	float GroundCombustionVisualAccumulator = 0.0f;
	bool bGroundCombustionVisualDirty = false;
	bool bGroundCombustionVisualNeedsFullRebuild = true;
	TSet<int32> PendingGroundCombustionVisualCellIndices;
	TMap<int32, int32> GroundResidueInstanceByCell;
	TArray<int32> GroundResidueCellByInstance;
	TMap<int32, int32> GroundFlameInstanceByCell;
	TArray<int32> GroundFlameCellByInstance;
	TMap<int32, int32> GroundSmokeInstanceByCell;
	TArray<int32> GroundSmokeCellByInstance;
	float SourceCombustionVisualAccumulator = 0.0f;
	bool bSourceCombustionVisualDirty = false;
	bool bBatchingGroundIgnitions = false;
	FDelegateHandle ContentReloadHandle;
	FDelegateHandle FragmentSourcePresenceHandle;
	TUniquePtr<MatterFlux::PlayableLevel::FLevelLayout>
		PendingGeneratedLayout;
	FMatterFluxContentRegistryPtr PendingGenerationRegistry;
	uint64 GenerationRequestSerial = 0;
	int32 GenerationPreviousSeed = 0;
	int32 InitialPendingGenerationObjects = 0;
	EMatterFluxWorldGenerationPhase GenerationPhase =
		EMatterFluxWorldGenerationPhase::Idle;
	float GenerationProgress = 0.0f;
	FString GenerationStatusText;
	FOnMatterFluxWorldGenerationFinished GenerationFinished;
};
