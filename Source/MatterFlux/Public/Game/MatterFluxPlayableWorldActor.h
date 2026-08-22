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
#include "Rendering/MatterFluxSmokeVisualPool.h"
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
class AMatterFluxTwoStoreyHouseActor;

namespace MatterFlux::PlayableLevel
{
	struct FLevelLayer;
	struct FLevelFragmentSource;
	struct FLevelLayout;
}

namespace MatterFlux::Liquid
{
	struct FLiquidColumn;
}

namespace MatterFlux::Rendering
{
	struct FLiquidSurfaceProjection;
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

struct FMatterFluxMaterialDisplacementState
{
	FName MaterialId = NAME_None;
	int32 SupportHeight = 0;
	uint8 ReferenceAmount = 0;
	uint8 MaximumRemainingAmount = 0;
};

struct FMatterFluxLiquidProjectionHeightAudit
{
	float CanonicalMedianSurfaceZ = 0.0f;
	float RenderedMedianSurfaceZ = 0.0f;
	float MedianOffset = 0.0f;
	float MaximumAbsoluteLocalOffset = 0.0f;
	float MaximumTriangleHeightSpan = 0.0f;
	int32 CanonicalCellCount = 0;
	int32 ProjectedCellCount = 0;
	int32 SurfacePatchCount = 0;
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
	int32 GetGeneratedLayerCount() const
	{
		return GeneratedLayerInstances.Num()
			+ GeneratedLiquidLayerMeshes.Num();
	}
	/** 连续液面使用合并顶面网格，避免透明 HISM 单元互相排序产生格缝。 */
	int32 GetGeneratedLiquidLayerCount() const
	{
		return GeneratedLiquidLayerMeshes.Num();
	}
	int32 GetLastLiquidProjectionDirtyChunkCount() const
	{
		return LastLiquidProjectionDirtyChunkCount;
	}
	int32 GetLastLiquidProjectionRebuiltChunkCount() const
	{
		return LastLiquidProjectionRebuiltChunkCount;
	}
	int32 GetLastLiquidProjectionCheckerboardPassCount() const
	{
		return LastLiquidProjectionCheckerboardPassCount;
	}

	UFUNCTION(BlueprintPure, Category = "Playable World")
	int32 GetGeneratedFragmentSourceCount() const { return GeneratedFragmentSources.Num(); }
	bool TrySampleTerrainHeightAtWorldLocation(
		const FVector& WorldLocation,
		float& OutWorldHeight) const;
	/**
	 * 将世界坐标映射到材质格，并返回与渲染一致的液柱。
	 * 非液体、空格和未初始化状态均返回 false。
	 */
	bool TrySampleLiquidColumnAtWorldLocation(
		const FVector& WorldLocation,
		MatterFlux::Liquid::FLiquidColumn& OutColumn) const;
	/**
	 * Buoyancy-only query. A body that vacated its own column still receives
	 * pressure from the surrounding liquid surface recorded on the prior frame.
	 */
	bool TrySampleAmbientLiquidColumnAtWorldLocation(
		const FVector& WorldLocation,
		MatterFlux::Liquid::FLiquidColumn& OutColumn) const;
	/**
	 * Authority-only material transaction used by every buoyant body. Liquid
	 * overlapping the submitted body volume is moved to nearby free cells.
	 */
	bool DisplaceLiquidInWorldBounds(
		const FVector& Center,
		const FVector& HorizontalExtent,
		float BottomZ,
		float TopZ,
		bool bCapsuleShape = false,
		bool bDeferMaterialSolve = false);
	/** Read-only visual acceptance data; never participates in material simulation. */
	bool TryGetLiquidProjectionHeightAudit(
		FName MaterialId,
		FMatterFluxLiquidProjectionHeightAudit& OutAudit) const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetCachedFragmentSourceCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetVisibleFragmentSourceProxyCount() const;
	/** Restricts merged source presentation for deterministic visual QA. Invalid restores all. */
	void SetFragmentSourceDebugIsolatedAggregate(const FGuid& AggregateId);
	bool FindNearestTreeAggregateForVisualInspection(
		const FVector& Focus,
		FGuid& OutAggregateId,
		FGuid& OutRootSourceId,
		FBox& OutWorldBounds,
		FTransform& OutRootWorldTransform) const;
	bool IgniteLogicalFragmentAggregate(
		const FGuid& AggregateId,
		const FVector& WorldLocation,
		FName FlameMaterial,
		int32 EventSeed);

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
		int32 EventSeed,
		int32 MaxIgnitions = MAX_int32);
	bool IgniteDynamicAggregateSource(
		AFragment2DActor& CarrierActor,
		const FGuid& SourceId,
		const FVector& WorldLocation,
		FName FlameMaterial,
		int32 EventSeed);
	void MarkSourceCombustionVisualizationDirty()
	{
		bSourceCombustionVisualDirty = true;
	}
	void MaterializeFragmentAggregate(const FGuid& AggregateId);
	/** Keeps an interaction source resident across asynchronous presentation steps. */
	void SetFragmentSourceStreamingPinned(
		const FGuid& SourceId,
		bool bPinned);
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
	int64 GetSimulatedMaterialAmount(FName MaterialId) const;

	/**
	 * 在权威端把 Lua 材质写入对应的地表模拟格。法术、容器和测试场景
	 * 共用这一入口，避免各自复制世界坐标到材料格的换算规则。
	 */
	UFUNCTION(BlueprintCallable, Category = "Playable World|Material Simulation")
	bool SetSimulatedMaterialAtWorldLocation(
		const FVector& WorldLocation,
		FName MaterialId);

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

	UMaterialInterface* GetVoxelLiquidMaterialTemplate() const
	{
		return VoxelLiquidMaterialTemplate;
	}

	AMatterFluxTwoStoreyHouseActor* GetGeneratedHouse() const
	{
		return GeneratedHouse;
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

	/** 一个液体材质格在世界 Z 方向占据的高度（厘米）。 */
	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "8.0", ClampMax = "300.0"))
	float MaterialLiquidColumnHeight = 128.0f;

	/**
	 * 液体格的可视表面厚度（厘米）。它不等于浮力采样深度：
	 * 2.5D 模拟中一个格可以表示有深度的液体，但不应把每个格渲染成独立高柱。
	 */
	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "1.0", ClampMax = "32.0"))
	float MaterialLiquidVisualThickness = 8.0f;

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
	void RebuildGeneratedHouse(
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void DestroyGeneratedHouse();
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
	/** 执行动态材料格与可切割 Source 之间由 Lua contact 规则定义的接触反应。 */
	void ApplyMaterialSourceContactReactions(
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
	void AdvanceUnifiedSmokeVisualization(float DeltaSeconds);
	void RefreshUnifiedSmokeAnchors();
	void RebuildGroundSmokeAnchors();
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
	void ApplyLiquidMaterialChunkMesh(
		FName ComponentKey,
		FName MaterialId,
		FIntPoint ChunkCoordinate,
		const FMatterFluxMaterialDefinition& Material,
		MatterFlux::Rendering::FLiquidSurfaceProjection& Projection);

	struct FLayerStreamingCache
	{
		MatterFlux::PlayableLevel::FLevelLayer Layer;
		TMap<FIntPoint, TArray<FTransform>> ChunkInstances;
		TArray<FTransform> AlwaysLoadedInstances;
	};

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> GeneratedLayerInstances;
	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UProceduralMeshComponent>> GeneratedLiquidLayerMeshes;
	TMap<FName, FName> LiquidProjectionMaterials;
	TMap<FName, FIntPoint> LiquidProjectionChunks;
	TMap<FName, FLayerStreamingCache> LayerStreamingCaches;
	TMap<FName, MatterFlux::PlayableLevel::FLevelLayer>
		LiquidLayerDefinitions;
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

	UPROPERTY(Transient)
	TObjectPtr<AMatterFluxTwoStoreyHouseActor> GeneratedHouse;

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
	TSet<FGuid> StreamingPinnedFragmentSourceIds;
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

	/** 动态液体只渲染可拼接顶面，避免透明立方体侧壁形成格栅和空洞。 */
	UPROPERTY()
	TObjectPtr<UStaticMesh> LiquidSurfaceMesh;

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

	/** 叶片专用的点采样方块材质；仍按区块/材质合并渲染。 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> VoxelLeafMaterialTemplate;

	/** 每个木体素独立重复一格树皮纹理，避免树干读成长木板。 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> VoxelWoodMaterialTemplate;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> VoxelGasMaterialTemplate;

	/** 深度感知透明度和折射参数均由 Lua 液体定义驱动。 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> VoxelLiquidMaterialTemplate;

	TUniquePtr<MatterFlux::Material::FSimulationRuntime>
		MaterialSimulation;
	/** 热路径液柱查询只读缓存，避免每个浮力采样重复访问脚本注册表。 */
	TMap<FName, float> MaterialLiquidDensities;
	/** Idempotent per-column volume constraints submitted by bodies this frame. */
	TMap<FIntPoint, FMatterFluxMaterialDisplacementState>
		PendingMaterialDisplacementCells;
	/** Last committed constraints retain the undisturbed amount and pressure footprint. */
	TMap<FIntPoint, FMatterFluxMaterialDisplacementState>
		PreviousMaterialDisplacementCells;
	/** Read-only diagnostics for proving the disposable mesh follows current facts. */
	TMap<FName, FMatterFluxLiquidProjectionHeightAudit>
		LiquidProjectionHeightAudits;
	TMap<FName, FMatterFluxLiquidProjectionHeightAudit>
		LiquidChunkProjectionHeightAudits;
	int32 LastLiquidProjectionDirtyChunkCount = 0;
	int32 LastLiquidProjectionRebuiltChunkCount = 0;
	int32 LastLiquidProjectionCheckerboardPassCount = 0;
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
	/** 每帧最多提交的材料-Source 接触反应，防止大面积泄漏触发切割尖峰。 */
	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation",
		meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxMaterialSourceReactionsPerFrame = 8;
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
	TArray<MatterFlux::Rendering::FSmokeEmissionAnchor>
		GroundSmokeAnchors;
	TArray<MatterFlux::Rendering::FSmokeEmissionAnchor>
		SourceSmokeAnchors;
	MatterFlux::Rendering::FSmokeVisualPool SmokeVisualPool;
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
