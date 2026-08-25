#pragma once

#include "CoreMinimal.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Fragment/FragmentSourceSpatialIndex.h"
#include "Game/MatterFluxFragmentSourceReplication.h"
#include "GameFramework/Actor.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Material/MatterFluxGroundReactionRuntime.h"
#include "Material/MatterFluxCustomMap.h"
#include "Material/MatterFluxLogicalSourceReactionIndex.h"
#include "Material/MatterFluxSourceReactionRuntime.h"
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
class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UMatterFluxFragmentSourceProxyComponent;
class AFragment2DSourceActor;
class AFragment2DActor;
class AMatterFluxGroundStateChunkActor;
class AMatterFluxPlayableWorldActor;
class AMatterFluxTwoStoreyHouseActor;
struct FMatterFluxAsyncPopulationBuildState;
struct FMatterFluxAsyncTerrainBuildState;

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

namespace MatterFlux::TerrainMesh
{
	struct FChunk;
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
	EMatterFluxMaterialPhase Phase = EMatterFluxMaterialPhase::StaticSolid;
	int32 SupportHeight = 0;
	uint16 ReferenceAmount = 0;
	uint16 MaximumRemainingAmount = 0;
};

/** A vertical liquid or powder column that resists body movement. */
struct FMatterFluxMovementMediumColumn
{
	FName MaterialId = NAME_None;
	EMatterFluxMaterialPhase Phase = EMatterFluxMaterialPhase::StaticSolid;
	float MovementResistance = 0.0f;
	float BottomZ = 0.0f;
	float SurfaceZ = 0.0f;

	bool IsValid() const
	{
		return !MaterialId.IsNone()
			&& (Phase == EMatterFluxMaterialPhase::Liquid
				|| Phase == EMatterFluxMaterialPhase::Powder)
			&& FMath::IsFinite(MovementResistance)
			&& MovementResistance >= 0.0f
			&& FMath::IsFinite(BottomZ)
			&& FMath::IsFinite(SurfaceZ)
			&& SurfaceZ > BottomZ;
	}
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
	static constexpr int32 PaperMagicStorySeed = 8403;

	AMatterFluxPlayableWorldActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

	void Regenerate(int32 NewSeed = 0);
	/** Sets a deterministic seed before deferred spawning reaches BeginPlay. */
	bool SetInitialMapSeed(int32 InitialSeed);
	bool RequestRegenerateAsync(
		int32 NewSeed = 0,
		bool bForceExactSeed = false);
	bool LoadCustomMap(FName MapId, int32 Seed, FString& OutError);
	FName GetActiveCustomMapId() const { return ActiveCustomMapId; }
	int32 GetCustomMapLoadSerial() const { return CustomMapLoadSerial; }
	bool IsCustomMapActive() const { return !ActiveCustomMapId.IsNone(); }
	bool TryGetCustomMapMarker(
		FName MarkerId,
		FVector& OutWorldLocation) const;
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
	 * Places a requested capsule center above every canonical terrain sample
	 * that can contribute to the smoothed collision surface under its footprint.
	 */
	bool TryResolveTerrainSpawnLocation(
		const FVector& RequestedWorldLocation,
		float HorizontalRadius,
		float CapsuleHalfHeight,
		float Clearance,
		FVector& OutWorldLocation) const;
	/**
	 * Pins a not-yet-possessed player's spawn chunk as a streaming focus and
	 * synchronously commits its terrain behind the entry/loading gate.
	 */
	bool RequestPlayerSpawnRegion(
		const FVector& WorldLocation,
		FIntPoint& OutTerrainChunk);
	void ReleasePlayerSpawnRegion(const FIntPoint& TerrainChunk);
	bool IsPlayerSpawnRegionTerrainReady(
		const FIntPoint& TerrainChunk) const;
	/**
	 * Initial-entry barrier: every requested terrain/population transaction and
	 * its material presentation are complete, including river projections.
	 * Later joins use the per-region terrain check so active traversal elsewhere
	 * cannot indefinitely delay a new player.
	 */
	bool IsInitialWorldEntryReady() const;
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
	/** Samples liquid or powder, including the column vacated by a body last frame. */
	bool TrySampleMovementMediumColumnAtWorldLocation(
		const FVector& WorldLocation,
		FMatterFluxMovementMediumColumn& OutColumn) const;
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
	/** Shared body-volume transaction for liquids and surface powders. */
	bool DisplaceMaterialInWorldBounds(
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
	int32 GetProceduralPopulationChunkCount() const
	{
		return ProceduralPopulationChunks.Num();
	}
	int32 GetPendingProceduralPopulationUpdateCount() const
	{
		int32 PendingCount = PendingProceduralPopulationChunks.Num()
			+ PendingProceduralPopulationRemovals.Num()
			+ ProceduralPopulationChunksBuilding.Num()
			+ PendingInitialLiquidProjectionChunks.Num();
		// Surface topology and river particles are streamed on the same bounded
		// queue as decorations. Treat the unseeded near-focus window as pending
		// work as well, so loading/drain callers do not observe a visually complete
		// forest before its ground and water simulation exists.
		TSet<FIntPoint> PendingSurfaceChunks;
		for (const FIntPoint Focus : ProceduralPopulationFocusChunks)
		{
			for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
			{
				for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					const FIntPoint Candidate =
						Focus + FIntPoint(OffsetX, OffsetY);
					if (DesiredProceduralPopulationChunks.Contains(Candidate)
						&& !SeededProceduralSurfaceChunks.Contains(Candidate))
					{
						PendingSurfaceChunks.Add(Candidate);
					}
				}
			}
		}
		for (const FIntPoint Chunk : ProceduralSurfaceSeedPriorityChunks)
		{
			if (DesiredProceduralPopulationChunks.Contains(Chunk)
				&& ProceduralRiverChunks.Contains(Chunk)
				&& !PrefetchedProceduralRiverSurfaceChunks.Contains(Chunk))
			{
				PendingSurfaceChunks.Add(Chunk);
			}
		}
		// Surface seeding dirties the material presentation before the liquid
		// projection queue is populated. Keep the stream non-ready across that
		// hand-off as well; otherwise callers can reveal a river in the one-frame
		// gap between canonical water creation and projection discovery.
		const int32 PendingVisualizationHandoff =
			bMaterialVisualizationDeferredForStreaming
			&& bMaterialVisualizationDirty
				? 1
				: 0;
		return PendingCount
			+ PendingSurfaceChunks.Num()
			+ PendingVisualizationHandoff;
	}
	int32 GetPendingTerrainChunkPrefetchCount() const
	{
		return PendingTerrainChunkPrefetches.Num()
			+ TerrainChunksBuilding.Num();
	}
	bool HasVisibleTerrainChunk(const FIntPoint Chunk) const
	{
		return ActiveTerrainChunks.Contains(Chunk);
	}
	int32 GetProceduralTreeAggregateCount() const;
	bool HasProceduralPopulationChunk(FIntPoint Chunk) const
	{
		return ProceduralPopulationChunks.Contains(Chunk);
	}
	int32 GetGeneratedStreamedHouseCount() const
	{
		return GeneratedStreamedHouses.Num();
	}

	UFUNCTION(BlueprintPure, Category = "Playable World|Streaming")
	int32 GetVisibleFragmentSourceProxyCount() const;
	/** Updates the local camera-only ghost projection for merged material items. */
	bool UpdateLocalFragmentItemOcclusion(
		const FVector& CameraLocation,
		const FBox& ViewerBounds);
	/** Restricts merged source presentation for deterministic visual QA. Invalid restores all. */
	void SetFragmentSourceDebugIsolatedAggregate(const FGuid& AggregateId);
	bool FindNearestTreeAggregateForVisualInspection(
		const FVector& Focus,
		FGuid& OutAggregateId,
		FGuid& OutRootSourceId,
		FBox& OutWorldBounds,
		FTransform& OutRootWorldTransform) const;
	bool ApplyMaterialStimulusToLogicalFragmentAggregate(
		const FGuid& AggregateId,
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed);

	/** Materializes pristine mask proxies intersecting Bounds, then returns all active sources there. */
	void GatherFragmentSourcesInBounds(
		const FBox& Bounds,
		TArray<AFragment2DSourceActor*>& OutSources);
	/** Sweeps only fixed authored sources; characters and detached fragments are excluded. */
	bool SweepFixedFragmentSource(
		const FVector& Start,
		const FVector& End,
		float Radius,
		FVector& OutImpactLocation,
		FVector& OutImpactNormal,
		AFragment2DSourceActor*& OutSource);
	int32 MaterializeFragmentSourcesForDamage(
		const FFragmentDamageShape& DamageShape);
	/** Commits a cut to the canonical sparse terrain facts and refreshes resident projections. */
	int32 ApplyTerrainDamage(
		const FFragmentDamageShape& DamageShape,
		float DamagePower);
	int32 ApplyMaterialStimulusToLogicalFragmentSourcesInCone(
		const FVector& Start,
		const FVector& Direction,
		float Range,
		float StartRadius,
		float EndRadius,
		FName StimulusMaterial,
		int32 EventSeed);
	int32 ApplyMaterialStimulusToLogicalFragmentSourcesInBounds(
		const FBox& Bounds,
		const FVector& StimulusPoint,
		FName StimulusMaterial,
		int32 EventSeed,
		int32 MaxActivations = MAX_int32);
	bool ApplyMaterialStimulusToDynamicAggregateSource(
		AFragment2DActor& CarrierActor,
		const FGuid& SourceId,
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed);
	void MarkSourceReactionVisualizationDirty()
	{
		bSourceReactionVisualDirty = true;
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
	/** True only after the canonical liquid projection covering this point is renderable. */
	bool HasLiquidProjectionAtWorldLocation(
		FName MaterialId,
		const FVector& WorldLocation) const;

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

	/** Finds the first occupied 2.5D material column touched by a swept sphere. */
	bool SweepSimulatedMaterial(
		const FVector& Start,
		const FVector& End,
		float Radius,
		FVector& OutImpactLocation,
		FName& OutMaterialId) const;

	/** Deposits a compact deterministic patch without overwriting its neighbors. */
	int32 DepositSimulatedMaterialAtWorldLocation(
		const FVector& WorldLocation,
		FName MaterialId,
		int32 CellCount);
	/** Deposits a projectile payload and preserves the physical actor it contacted. */
	int32 DepositSimulatedMaterialFromImpact(
		const FVector& WorldLocation,
		FName MaterialId,
		int32 CellCount,
		AActor* ImpactActor,
		float ImpactRadius = 0.0f);
	/** Read-only proof that powder is currently carried by object support. */
	int32 GetExternalMaterialSupportCellCount() const
	{
		return ExternalMaterialSupportCells.Num();
	}
	int64 GetExternalMaterialSupportedAmount(FName MaterialId) const;

	/**
	 * Resolves one world-space material particle against every reactive storage
	 * adapter. The caller supplies only material identity and geometry; target
	 * selection is derived from Lua reaction rules.
	 */
	int32 ApplyMaterialStimulusAtWorldLocation(
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed,
		float ContactRadius = 0.0f);

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

	UFUNCTION(BlueprintCallable, Category = "Playable World|Reaction")
	bool ApplyMaterialStimulusToFirstGeneratedTree(int32 EventSeed = 404);

	UFUNCTION(BlueprintPure, Category = "Playable World|Reaction")
	int32 GetReactingSourceCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Reaction")
	int32 GetReactionOutputCellCount() const;
	int32 GetLogicalReactionOutputCellCount(FName MaterialId) const;
	int32 GetLogicalReactionInputCellCount(FName MaterialId) const;
	int32 GetLogicalReactionActiveCellCount(FName MaterialId) const;
	int32 GetLogicalReactionMaterialEmissionCount() const;
	/** Read-only audit for duplicate normal/burned proxy cells. */
	int32 GetLogicalReactionProjectionOverlapCellCount() const;
	/** Number of burned tree layers still rendered as a separate 2D overlay. */
	int32 GetStandaloneTreeReactionOutputProjectionCount() const;
	/** Read-only count of currently rendered logical-source flame cells. */
	int32 GetLogicalReactionFlameInstanceCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Reaction")
	int32 GetReactedGroundCellCount() const;
	int32 GetActiveGroundReactionCellCount() const;

	UFUNCTION(BlueprintPure, Category = "Playable World|Reaction")
	int32 GetReplicatedGroundReactionByteCount() const;

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

	UPROPERTY(ReplicatedUsing = OnRep_ActiveCustomMapId)
	FName ActiveCustomMapId = NAME_None;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "0.01", ClampMax = "0.25"))
	float MaterialSimulationStepSeconds = 0.05f;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "4", ClampMax = "256"))
	int32 MaterialSimulationChunkSize = 64;

	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "0", ClampMax = "8"))
	int32 MaterialSimulationActiveChunkRadius = 1;

	/**
	 * Newly generated or projectile-deposited water/powder may enter the camera
	 * window during this interval and receive one settling wake outside the
	 * smaller player-focus simulation window.
	 */
	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation", meta = (ClampMin = "0.25", ClampMax = "30.0"))
	float MaterialRecentViewWakeSeconds = 5.0f;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "8", ClampMax = "128"))
	int32 TerrainStreamingChunkSize = 64;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "0", ClampMax = "5"))
	int32 TerrainStreamingChunkRadius = 4;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "9", ClampMax = "256"))
	int32 TerrainChunkCacheLimit = 192;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxTerrainChunkPrefetchesPerFrame = 1;

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
	void OnRep_ActiveCustomMapId();

	UFUNCTION()
	void OnRep_MaterialSimulationState();

	UFUNCTION()
	void OnRep_FragmentSourceStates();

	UFUNCTION()
	void OnRep_TerrainHeightOverrides();

	void RebuildLevel();
	bool RebuildActiveCustomMap(FString& OutError);
	bool RebuildProceduralStoryMap(
		const FMatterFluxContentRegistry& Registry,
		FString& OutError);
	void PrepareForProceduralWorld();
	void BuildCustomMapSceneBoxes(
		const FMatterFluxContentRegistry& Registry);
	void BuildCustomMapStructures(
		const FMatterFluxContentRegistry& Registry);
	void DestroyCustomMapSceneBoxes();
	void SanitizeGenerationSettings();
	void AdvanceAsyncGeneration();
	void CompleteAsyncGeneration(bool bSuccess, const FString& Message);
	void RecaptureStaticSky();
	void RebuildGeneratedHouse(
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void DestroyGeneratedHouse();
	void PrewarmStreamedHousePool(const FTransform& TemplateTransform);
	AMatterFluxTwoStoreyHouseActor* AcquireStreamedHouse(
		const FTransform& HouseTransform,
		FName StructureDefinitionId);
	void ReleaseStreamedHouse(AMatterFluxTwoStoreyHouseActor* House);
	void RefreshProceduralPopulation(
		TConstArrayView<FIntPoint> OrderedDesiredChunks,
		TConstArrayView<FIntPoint> FocusChunks);
	bool ProcessPendingProceduralPopulationUpdates(
		bool bAllowSurfaceSeed = true,
		bool bAllowChunkCommit = true,
		bool bAllowSurfaceFinalization = true,
		bool* bOutDidChunkCommit = nullptr);
	void DestroyGeneratedStreamedHouses();
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
	bool ApplyPersistentFragmentSourceStateToProxy(
		const FGuid& SourceId,
		const FFragment2DSourceStreamingState& State);
	void RemoveReplicatedFragmentSourceState(const FGuid& SourceId);
	void MarkReplicatedFragmentSourceStatesDirty();
	void UpdateMaterialVisualization(
		float DeltaSeconds,
		bool bAllowRebuild = true);
	void RebuildMaterialVisualization(
		const FMatterFluxContentRegistry& Registry);
	/** 执行动态材料格与可切割 Source 之间由 Lua contact 规则定义的接触反应。 */
	void ResolveMaterialInteractions(
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
	void EmitActiveReactionParticles(float DeltaSeconds);
	void AdvanceLogicalSourceReaction(float DeltaSeconds);
	bool ApplyMaterialStimulusToLogicalFragmentSource(
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed);
	bool ApplyMaterialStimulusToGroundAtWorldLocation(
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed);
	const MatterFlux::PlayableLevel::FLevelFragmentSource*
		FindFragmentSourceDefinition(const FGuid& SourceId) const;
	void GatherLogicalFragmentSourceCandidates(
		const FBox& WorldBounds,
		TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*>&
			OutCandidates) const;
	bool SynchronizeLogicalSourceReactionState(
		const FGuid& SourceId,
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Source,
		const MatterFlux::Reaction::FSourceReactionRuntime& Runtime,
		bool bPublish);
	void RebuildLogicalSourceReactionVisualization();
	void AdvanceUnifiedSmokeVisualization(float DeltaSeconds);
	void RefreshUnifiedSmokeAnchors();
	void RebuildGroundSmokeAnchors();
	void InitializeGroundReaction(
		const FMatterFluxContentRegistry& Registry,
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void AdvanceGroundReaction(float DeltaSeconds);
	void EnsureGroundReactionVisuals(
		const FMatterFluxContentRegistry& Registry);
	void RebuildGroundReactionVisualization();
	void ApplyGroundReactionVisualChanges(
		TConstArrayView<int32> ChangedCellIndices);
	bool IsGroundReactionCellVisible(int32 CellIndex) const;
	void InitializeGroundStateChunks();
	void DestroyGroundStateChunks();
	void PublishGroundReactionState();
	void BuildLayerStreamingCache(
		const MatterFlux::PlayableLevel::FLevelLayout& Layout);
	void BuildTerrainStreamingCache(
		const MatterFlux::PlayableLevel::FLevelTerrain& Terrain);
	void GatherStreamingFocusChunks(
		TArray<FIntPoint>& OutFocusChunks) const;
	void GatherMaterialSimulationFocusCells(
		TArray<FIntPoint>& OutFocusCells) const;
	void RegisterRecentMaterialWakeCells(
		TConstArrayView<FIntPoint> WorldCells);
	void RegisterRecentMaterialWakeSeedCells(
		TConstArrayView<MatterFlux::Material::FSeedCell> SeedCells);
	void WakeRecentVisibleMaterialChunks();
	void PruneExternalMaterialSupports();
	bool IsMaterialCellInsideTerrainView(const FIntPoint& WorldCell) const;
	void RefreshVisibleLevelLayers(bool bForce);
	bool RefreshVisibleTerrainChunks(
		bool bForce,
		const TArray<FIntPoint>& FocusChunks);
	bool ProcessPendingTerrainChunkPrefetches(bool bAllowChunkCommit = true);
	UProceduralMeshComponent* CreateTerrainChunkComponent(
		FIntPoint ChunkCoordinate);
	UProceduralMeshComponent* CreateTerrainChunkComponentFromData(
		FIntPoint ChunkCoordinate,
		const MatterFlux::TerrainMesh::FChunk& Chunk);
	void ApplyTerrainHeightOverrides(
		TConstArrayView<FMatterFluxTerrainHeightOverride> Overrides,
		bool bRebuildResidentChunks);
	void RebuildResidentTerrainChunksForCells(
		TConstArrayView<FIntPoint> WorldCells);
	void RefreshTerrainBackdropForCells(
		TConstArrayView<FIntPoint> WorldCells);
	void PublishTerrainHeightOverrides();
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
	TArray<TObjectPtr<UStaticMeshComponent>> GeneratedCustomMapSceneBoxes;
	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UProceduralMeshComponent>> GeneratedLiquidLayerMeshes;
	TMap<FName, FName> LiquidProjectionMaterials;
	TMap<FName, FIntPoint> LiquidProjectionChunks;
	// A loading barrier waits for each newly seeded visible liquid chunk to be
	// projected once. The ordinary dirty queue is deliberately excluded: flowing
	// water can dirty the same chunks forever and is not finite initialization work.
	TSet<FIntPoint> PendingInitialLiquidProjectionChunks;
	TSet<FIntPoint> PendingLiquidProjectionDirtyChunks;
	bool bCaptureInitialLiquidProjectionRequirements = false;
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
	TMap<FIntPoint, int32> PlayerSpawnRegionFocusCounts;
	TArray<FIntPoint> PendingTerrainChunkPrefetches;
	TSet<FIntPoint> TerrainChunksBuilding;
	TSharedPtr<FMatterFluxAsyncTerrainBuildState, ESPMode::ThreadSafe>
		AsyncTerrainBuildState;
	TSharedPtr<const MatterFlux::PlayableLevel::FLevelTerrain,
		ESPMode::ThreadSafe> AsyncTerrainHeightField;
	TMap<FIntPoint, uint64> TerrainChunkLastUsed;
	uint64 TerrainChunkUseCounter = 0;
	bool bTerrainCacheCoversWholeMap = false;

	UPROPERTY(Transient)
	TMap<FGuid, TObjectPtr<AFragment2DSourceActor>> GeneratedFragmentSources;

	UPROPERTY(Transient)
	TObjectPtr<AMatterFluxTwoStoreyHouseActor> GeneratedHouse;
	UPROPERTY(Transient)
	TMap<FIntPoint, TObjectPtr<AMatterFluxTwoStoreyHouseActor>>
		GeneratedStreamedHouses;
	UPROPERTY(Transient)
	TArray<TObjectPtr<AMatterFluxTwoStoreyHouseActor>>
		StreamedHousePool;
	static constexpr int32 PrewarmedStreamedHouseCount = 2;

	TMap<FIntPoint, TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>>
		FragmentSourceChunks;
	TSet<FIntPoint> ProceduralPopulationChunks;
	TSet<FIntPoint> DesiredProceduralPopulationChunks;
	TArray<FIntPoint> PendingProceduralPopulationChunks;
	TSet<FIntPoint> ProceduralPopulationChunksBuilding;
	TSharedPtr<FMatterFluxAsyncPopulationBuildState, ESPMode::ThreadSafe>
		AsyncPopulationBuildState;
	TArray<FIntPoint> PendingProceduralPopulationRemovals;
	TArray<FIntPoint> ProceduralPopulationFocusChunks;
	/** Visible chunks first, followed by the off-screen prefetch ring. */
	TArray<FIntPoint> ProceduralSurfaceSeedPriorityChunks;
	/** Population-build result used to preseed only water-bearing distant chunks. */
	TSet<FIntPoint> ProceduralRiverChunks;
	/** Exact river facts retained from the async population build for sparse preseed. */
	TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FStreamingRiverCell>>
		ProceduralRiverCellsByChunk;
	/** Chunks whose river is renderable but whose full dry topology may be absent. */
	TSet<FIntPoint> PrefetchedProceduralRiverSurfaceChunks;
	bool bPreferProceduralSurfaceSeed = false;
	TSet<FIntPoint> SeededProceduralSurfaceChunks;
	TOptional<FIntPoint> ProceduralSurfaceSeedChunkInProgress;
	bool bProceduralSurfaceSeedIsRiverPrefetch = false;
	int32 ProceduralSurfaceSeedNextTerrainCell = 0;
	TSet<FIntPoint> ProceduralSurfaceSeedMaterialCells;
	TOptional<MatterFlux::Material::FSeedCell>
		ProceduralSurfaceSeedFirstCell;
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

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxProceduralPopulationUpdatesPerFrame = 2;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "0.25", ClampMax = "16.0"))
	float ProceduralPopulationBudgetMilliseconds = 4.0f;

	/** Maximum terrain samples applied by one incremental surface-seed frame. */
	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "64", ClampMax = "2048"))
	int32 ProceduralSurfaceSeedCellsPerFrame = 512;

	UPROPERTY(EditAnywhere, Category = "Playable World|Streaming", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxAsyncStreamingBuildTasks = 4;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UInstancedStaticMeshComponent>>
		GeneratedMaterialInstances;
	TMap<FName, uint64> MaterialVisualizationHashes;
	TMap<FName, FName> MaterialVisualizationMaterials;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> GroundOutputInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> GroundFlameInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> GroundSmokeInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> SourceFlameInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> SourceSmokeInstances;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundOutputMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundStimulusMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundSmokeMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SourceStimulusMaterial;

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

	UPROPERTY(ReplicatedUsing = OnRep_TerrainHeightOverrides)
	TArray<FMatterFluxTerrainHeightOverride>
		ReplicatedTerrainHeightOverrides;

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

	/** 深度感知透明度由 Lua 液体定义驱动。 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> VoxelLiquidMaterialTemplate;

	TUniquePtr<MatterFlux::Material::FSimulationRuntime>
		MaterialSimulation;
	struct FRecentMaterialWake
	{
		FIntPoint SampleCell = FIntPoint::ZeroValue;
		double ExpiresAtWorldSeconds = 0.0;
	};
	/** One pending eligibility record per material chunk, independent of focus budget. */
	TMap<FIntPoint, FRecentMaterialWake> RecentMaterialChunkWakes;
	struct FPendingMaterialStimulus
	{
		FVector WorldLocation = FVector::ZeroVector;
		/** Exact 2.5D cell written by the deposit; do not reconstruct it from a wall impact point. */
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		/** Sources resolved while the impact projection still existed. */
		TArray<TWeakObjectPtr<AFragment2DSourceActor>> AuthoredSources;
		FName MaterialId = NAME_None;
		int32 EventSeed = 0;
	};
	TArray<FPendingMaterialStimulus> PendingMaterialStimuli;
	struct FExternalMaterialSupportState
	{
		FName MaterialId = NAME_None;
		/** Fixed source only. Players and detached fragments use displacement. */
		TWeakObjectPtr<AFragment2DSourceActor> FixedSource;
	};
	/** Object-carried flow columns; heights live in the runtime's disposable support overlay. */
	TMap<FIntPoint, FExternalMaterialSupportState>
		ExternalMaterialSupportCells;
	/** 热路径液柱查询只读缓存，避免每个浮力采样重复访问脚本注册表。 */
	TMap<FName, float> MaterialLiquidDensities;
	struct FMovementMediumDefinition
	{
		EMatterFluxMaterialPhase Phase = EMatterFluxMaterialPhase::StaticSolid;
		float MovementResistance = 0.0f;
		float FullColumnHeight = 0.0f;
	};
	/** Hot-path physical properties for body-displaceable materials. */
	TMap<FName, FMovementMediumDefinition> MaterialMovementMedia;
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
	TUniquePtr<MatterFlux::Reaction::FGroundReactionRuntime>
		GroundReaction;
	TMap<FGuid, TUniquePtr<
		MatterFlux::Reaction::FSourceReactionRuntime>>
		ActiveSourceReactions;
	MatterFlux::Reaction::FLogicalSourceReactionIndex
		LogicalSourceReactionIndex;
	TArray<FGuid> SourceReactionActiveIdsScratch;
	TArray<FGuid> SourceReactionFinishedIdsScratch;
	TArray<FGuid> SourceReactionPublishIdsScratch;
	TArray<FMatterFluxFragmentSourceStateBatchUpdate>
		FragmentSourceReplicationUpdatesScratch;
	TArray<FVector> GroundSurfacePositions;
	TSet<FGuid> SourcesThatActivatedGround;
	float MaterialVisualizationAccumulator = 0.0f;
	/** 每帧最多提交的材料-Source 接触反应，防止大面积泄漏触发切割尖峰。 */
	UPROPERTY(EditAnywhere, Category = "Playable World|Material Simulation",
		meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxMaterialSourceReactionsPerFrame = 8;
	bool bMaterialVisualizationDirty = false;
	bool bMaterialVisualizationDeferredForStreaming = false;
	float ReactionPropagationAccumulator = 0.0f;
	float ReactionProxyFlushAccumulator = 0.0f;
	float GroundReactionVisualAccumulator = 0.0f;
	bool bGroundReactionVisualDirty = false;
	bool bGroundReactionVisualNeedsFullRebuild = true;
	TSet<int32> PendingGroundReactionVisualCellIndices;
	TMap<int32, int32> GroundOutputInstanceByCell;
	TArray<int32> GroundOutputCellByInstance;
	TMap<int32, int32> GroundFlameInstanceByCell;
	TArray<int32> GroundFlameCellByInstance;
	TMap<int32, int32> GroundSmokeInstanceByCell;
	TArray<int32> GroundSmokeCellByInstance;
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>
		GroundSmokeAnchors;
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>
		SourceSmokeAnchors;
	MatterFlux::Rendering::FSmokeVisualPool SmokeVisualPool;
	float SourceReactionVisualAccumulator = 0.0f;
	bool bSourceReactionVisualDirty = false;
	bool bBatchingGroundReactions = false;
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
	MatterFlux::Material::FCustomMapScene ActiveCustomMapScene;
	int32 CustomMapLoadSerial = 0;
	float ProceduralMaterialSimulationCellSize =
		MatterFlux::PlayableLevel::TerrainCellSize;
};
