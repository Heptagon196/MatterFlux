#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Fragment/FragmentTypes.h"
#include "Material/MatterFluxLocalMaterialReaction.h"
#include "Rendering/MatterFluxSmokeVisualPool.h"
#include "Fragment2DActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPhysicalMaterial;
class UMatterFluxBuoyancyComponent;
class AFragment2DSourceActor;
struct FFragment2DSourceStreamingState;

USTRUCT(BlueprintType)
struct MATTERFLUX_API FFragmentCarrierVolumeCellState
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Volume")
	FIntVector Cell = FIntVector::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Volume")
	FName MaterialId = NAME_None;

	UPROPERTY()
	uint16 Energy = 0;

	bool operator==(const FFragmentCarrierVolumeCellState& Other) const = default;
};

/** Immutable carrier element projection used to discover cross-body contacts. */
struct MATTERFLUX_API FFragmentCarrierMaterialElement
{
	FMaterialElementAddress Address;
	FMaterialElementState State;
	uint16 DefaultEnergy = 0;
	FVector WorldCenter = FVector::ZeroVector;
	float CellSize = 0.0f;
	bool bNonEnvironmentEnergy = false;
};

/**
 * One independently addressable logical source carried by a detached rigid
 * body. Rendering is rebuilt into the carrier's shared procedural mesh; this
 * record is not an Actor and therefore has no separate network/movement cost.
 */
USTRUCT(BlueprintType)
struct MATTERFLUX_API FFragmentAggregateSourceState
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	FGuid SourceId;

	/** 原始关卡定义的 SourceId；局部分割层拥有独立 SourceId，但复用原材质规则。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	FGuid DefinitionSourceId;

	/** true 表示该层已完整接管原逻辑 Source；false 表示只是从中切出的局部。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	bool bOwnsLogicalSource = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	int32 Revision = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	FFragmentSourceMask SourceMask;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	FTransform LocalTransform = FTransform::Identity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	TObjectPtr<UMaterialInterface> Material;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	FName MaterialId = NAME_None;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	bool bEnableCollision = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Volume")
	int32 VolumeTopologyRevision = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Volume")
	int32 VolumeFieldRevision = 0;

	UPROPERTY()
	uint16 VolumeEnvironmentEnergy = 0;

	/** Sparse material/energy overrides carried with the rigid body. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Volume")
	TArray<FFragmentCarrierVolumeCellState> VolumeCellStates;

	bool IsValid() const;
};

UCLASS(Blueprintable)
class MATTERFLUX_API AFragment2DActor : public AActor
{
	GENERATED_BODY()

public:
	AFragment2DActor();
	virtual ~AFragment2DActor() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual bool InitializeFromPayload(const FFragmentSpawnPayload& Payload);
	bool AbsorbAggregateSource(AFragment2DSourceActor& SourceActor);
	bool AbsorbAggregateSourceFragment(
		const AFragment2DSourceActor& SourceActor,
		const FFragmentSpawnPayload& Payload);
	int32 GetAggregateMemberCount() const { return AggregateSources.Num(); }
	bool ContainsAggregateSource(const FGuid& SourceId) const;
	FName GetAggregateSourceMaterialId(const FGuid& SourceId) const;
	bool ApplyAggregateSourceStreamingState(
		const FGuid& SourceId,
		const FFragment2DSourceStreamingState& State);
	bool GetAggregateSourceWorldTransform(
		const FGuid& SourceId,
		FTransform& OutWorldTransform) const;
	bool GetAggregateSourceState(
		const FGuid& SourceId,
		FFragmentAggregateSourceState& OutState) const;
	/** Resolves the nearest occupied carrier cell as one stable Volume element. */
	bool TryGetMaterialVolumeElementAtWorldLocation(
		const FVector& WorldLocation,
		const FLocalMaterialReactionProgram& Program,
		FMaterialElementAddress& OutAddress,
		FMaterialElementState& OutState,
		uint16& OutDefaultEnergy,
		FVector& OutWorldCellCenter) const;
	/** Atomically validates and commits one local-kernel result to the carrier. */
	bool CommitMaterialVolumeCellState(
		const FMaterialElementAddress& Address,
		uint16 DefaultEnergy,
		const FMaterialElementState& ExpectedBefore,
		const FMaterialElementState& After,
		const FLocalMaterialReactionProgram& Program,
		FString& OutError);
	/** Validates every touched carrier cell before publishing one replicated state. */
	bool CommitMaterialVolumeElementBatch(
		const FMaterialDeltaBatch& Batch,
		const FLocalMaterialReactionProgram& Program,
		FString& OutError);
	/** Atomically commits one kernel batch spanning two independent rigid carriers. */
	static bool CommitMaterialVolumePairBatch(
		AFragment2DActor& CarrierA,
		AFragment2DActor& CarrierB,
		const FMaterialDeltaBatch& Batch,
		const FLocalMaterialReactionProgram& Program,
		FString& OutError);
	/** Returns a stable immutable projection of every occupied carrier cell. */
	bool GatherMaterialVolumeElements(
		const FLocalMaterialReactionProgram& Program,
		TArray<FFragmentCarrierMaterialElement>& OutElements,
		FString& OutError) const;
	/** Runs one bounded fixed step over occupied neighbouring carrier cells. */
	bool AdvanceLocalMaterialVolumeReactions(
		const FLocalMaterialReactionProgram& Program,
		uint32 Seed,
		int32 LogicalStep,
		int32 MaxContacts,
		TArray<FMaterialParticleEmission>& OutEmissions,
		int32& OutProcessedContacts,
		FString& OutError);
	/** True while sparse carrier cells still contain non-ambient energy. */
	bool HasLocalMaterialVolumeReactionWork() const;
	/** Scheduler query that also retains isolated hot cells for cross-body contact. */
	bool HasNonEnvironmentMaterialVolumeEnergy() const;
	/** Resolves a stable carrier address without a nearest-cell search. */
	bool TryGetMaterialVolumeElementWorldLocation(
		const FMaterialElementAddress& Address,
		FVector& OutWorldLocation) const;
	bool ApplyMaterialStimulusAtWorldLocation(
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed);
	bool ApplyMaterialStimulusToAggregateAtWorldLocation(
		const FGuid& SourceId,
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed);
	bool ApplyMaterialStimulusInCone(
		const FVector& Start,
		const FVector& Direction,
		float Range,
		float StartRadius,
		float EndRadius,
		FName StimulusMaterial,
		int32 EventSeed);
	bool IsRootMaterialHot() const;
	bool IsAggregateSourceMaterialHot(const FGuid& SourceId) const;
	bool IsAnyAggregateMaterialHot(FName MaterialId) const;
	int32 GetRootMaterialOverrideCellCount() const;
	FBox GetReactiveWorldBounds() const;
	void GatherRootMaterialVisualTransforms(
		TArray<FTransform>& OutFlameTransforms,
		TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>& OutSmokeAnchors,
		int32 MaxVisualInstances) const;
	float GetTransientFadeAlpha() const { return TransientFadeAlpha; }
	float GetVisualDepthOffset() const { return VisualDepthOffset; }
	/**
	 * Applies one world-space cut to this detached logical item. Mask-backed
	 * material is removed immediately; disconnected remainders become separate
	 * physical actors, while a connected remainder rebuilds in place.
	 */
	bool TryAcceptWorldCut(const FFragmentDamageShape& CutShape);
	int32 GetAcceptedCutCount() const { return AcceptedCutCount; }
	int32 GetCutsBeforeFade() const { return CutsBeforeFade; }
	float GetCutFadeDuration() const { return CutExhaustionFadeDuration; }
	bool IsCutFadeActive() const { return ActiveCutFadeDuration > 0.0f; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	TObjectPtr<UProceduralMeshComponent> MeshComponent;

	/** Shared contact profile keeps sharp voxel hulls pushable on terrain. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Physics")
	TObjectPtr<UPhysicalMaterial> FragmentPhysicalMaterial;

	/** 服务器对该碎片载体施加材质密度浮力。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Liquid")
	TObjectPtr<UMatterFluxBuoyancyComponent> BuoyancyComponent;

	UPROPERTY(ReplicatedUsing = OnRep_SpawnPayload, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	FFragmentSpawnPayload SpawnPayload;

	UPROPERTY(ReplicatedUsing = OnRep_FragmentMaterial, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	TObjectPtr<UMaterialInterface> FragmentMaterial;

	UPROPERTY(ReplicatedUsing = OnRep_FragmentMaterial, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	FLinearColor FragmentColor = FLinearColor::White;

	UPROPERTY(ReplicatedUsing = OnRep_AggregateSources, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	TArray<FFragmentAggregateSourceState> AggregateSources;

	/** Material Volume state for the detached payload itself (for example the cut trunk). */
	UPROPERTY(ReplicatedUsing = OnRep_RootMaterialState, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Volume")
	FFragmentAggregateSourceState RootMaterialState;

	/** Telemetry: number of world cuts that changed this detached item. */
	UPROPERTY(ReplicatedUsing = OnRep_CutState, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Cut")
	int32 AcceptedCutCount = 0;

	/** Legacy saved-class setting retained for asset compatibility. */
	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "Fragment|Cut", meta = (ClampMin = "1", UIMin = "1"))
	int32 CutsBeforeFade = 10;

	/** Fade time used by legacy triangulated debris without an editable mask. */
	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "Fragment|Cut", meta = (ClampMin = "0.05", UIMin = "0.05"))
	float CutExhaustionFadeDuration = 0.8f;

	/** Replicated runtime signal; SpawnPayload remains immutable initial state. */
	UPROPERTY(ReplicatedUsing = OnRep_CutState, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Cut")
	float ActiveCutFadeDuration = 0.0f;

protected:
	UFUNCTION()
	void OnRep_SpawnPayload();

	UFUNCTION()
	void OnRep_FragmentMaterial();

	UFUNCTION()
	void OnRep_AggregateSources();

	UFUNCTION()
	void OnRep_RootMaterialState();

	UFUNCTION()
	void OnRep_CutState();

	void ApplyFragmentMaterial();
	bool RebuildMeshFromPayload();
	bool RebuildSimpleCollision();
	bool RebuildAggregateSourceSections();
	bool RebuildAggregateVisualSectionsOnly();
	bool AddAggregateSourceState(
		FFragmentAggregateSourceState&& Candidate,
		AFragment2DSourceActor* RetiredSource);
	void NotifyWorldOfAggregateSources();
	void ConfigureTransientFade();
	void SynchronizeCutFadeState();
	bool DoesCutShapeIntersect(const FFragmentDamageShape& CutShape) const;
	bool TrySplitDisconnectedRootMaterial(
		const FTransform& ParentWorldTransform,
		const FVector& PreservedLinearVelocity,
		const FVector& PreservedAngularVelocity,
		int32 NextAcceptedCutCount,
		bool& bOutSplit);
	void ApplyTransientFadeAlpha();
	void InitializeRootMaterialState();
	bool ApplyMaterialStimulusToRootAtWorldLocation(
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed);
	bool ApplyMaterialStimulusToDetachedAggregateAtWorldLocation(
		const FGuid& SourceId,
		const FVector& WorldLocation,
		FName StimulusMaterial,
		int32 EventSeed);
	void MarkReactionVisualizationDirty() const;
	void RefreshBuoyancyDensity();

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicFragmentMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicFragmentSideMaterial;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> AggregateDynamicMaterials;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> TransientFadeMaterial;

	float TransientFadeElapsed = 0.0f;
	float TransientFadeAlpha = 1.0f;
	float VisualDepthOffset = 0.0f;
};
