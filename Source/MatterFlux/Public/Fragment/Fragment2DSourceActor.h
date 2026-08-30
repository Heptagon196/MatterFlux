#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Fragment/FragmentTypes.h"
#include "Rendering/MatterFluxSmokeVisualPool.h"
#include "Volume/MatterFluxMaterialVolume.h"
#include "Volume/MatterFluxMaterialVolumeReplication.h"
#include "Fragment2DSourceActor.generated.h"

class UFragment2DAsset;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UInstancedStaticMeshComponent;
class UPointLightComponent;
class AFragment2DActor;
class AMatterFluxPlayableWorldActor;
class UFragmentSimulationSubsystem;
struct FMaterialElementState;
struct FMaterialDeltaBatch;
struct FMaterialElementDelta;

UCLASS(Blueprintable)
class MATTERFLUX_API AFragment2DSourceActor : public AActor
{
	GENERATED_BODY()

public:
	AFragment2DSourceActor();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual bool IsNetRelevantFor(
		const AActor* RealViewer,
		const AActor* ViewTarget,
		const FVector& SrcLocation) const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	bool ApplyDamageEvent(const FFragmentDamageEvent& DamageEvent, TArray<FFragmentSpawnPayload>& OutPayloads);
	bool InitializeFromProceduralMask(
		const FFragmentSourceMask& InMask,
		const FGuid& InSourceId,
		const FLinearColor& InColor = FLinearColor::White,
		FName InMaterialId = NAME_None,
		EMatterFluxMaterialStructuralRole InStructuralRole =
			EMatterFluxMaterialStructuralRole::None);
	/** Restore an existing procedural source to its pristine mask under a new ID. */
	bool ResetForStreamingReuse(const FGuid& InSourceId);
	bool CaptureStreamingState(
		FFragment2DSourceStreamingState& OutState,
		FString& OutError) const;
	bool RestoreStreamingState(
		const FFragment2DSourceStreamingState& State,
		FString& OutError);
	bool ApplyMaterialStimulusAtWorldLocation(
		const FVector& WorldLocation,
		FName StimulusMaterial = NAME_None,
		int32 EventSeed = 0);
	void SetSourceCollisionEnabled(bool bEnabled);
	/** Keep the logical source while another component owns its rendering. */
	void SetSourceMeshProjectionEnabled(bool bEnabled);
	void ConfigureAggregate(
		const FGuid& InAggregateId,
		bool bInAggregateRoot);
	void TransferAggregateMembersTo(
		AActor& CarrierActor,
		const FFragmentDamageEvent* SharedDamageEvent = nullptr);
	/**
	 * 将参考切割投影到同一 aggregate 中平行、同尺寸的深度切片。
	 * 这样方柱的前后两层会在完全相同的 mask 行被切断。
	 */
	bool BuildSynchronizedDamageEventFrom(
		const AFragment2DSourceActor& ReferenceSource,
		const FFragmentDamageEvent& ReferenceEvent,
		FFragmentDamageEvent& OutEvent) const;
	/**
	 * 整体物体刚被切断时，动态载体与留下的根部仍可能发生接触。
	 * 在两者真正分开前只关闭根部碰撞；视觉始终来自已提交的材质状态，
	 * 不允许计时器稍后再把根部“变”出来。
	 */
	void BeginAggregateSeparationGracePeriod(
		AActor& CarrierActor,
		float MaxDurationSeconds = 2.0f);
	bool IsAggregateSeparationCollisionSuppressed() const
	{
		return bAggregateSeparationCollisionSuppressed;
	}
	void MarkBroken();

	int32 GetMaskWidth() const;
	int32 GetMaskHeight() const;
	float GetCellSize() const;
	const FGuid& GetSourceId() const { return SourceId; }
	int32 GetMinFragmentAreaPixels() const;
	int32 GetMaxFragmentsPerBreak() const;
	const TArray<uint8>& GetRuntimeMask() const { return RuntimeMask; }
	int32 GetRemainingInputCellCount() const;
	int32 GetOutputCellCount() const;
	int32 GetActiveCellCount() const;
	/** Diagnostic: cells simultaneously present in base and replacement render masks. */
	int32 GetMaterialProjectionOverlapCellCount() const;
	int32 GetReplicatedMaterialVolumeCellCount() const
	{
		return ReplicatedMaterialVolumeState.Cells.Num();
	}
	int32 GetReplicatedMaterialVolumeFieldRevision() const
	{
		return ReplicatedMaterialVolumeState.FieldRevision;
	}
	/** Emissions are ordinary particles owned by MaterialWorld, never Source state. */
	int32 GetTotalMaterialEmissionCount() const { return 0; }
	/** Legacy object reaction payload was removed; Volume state uses the world Fast Array. */
	int32 GetReplicatedReactionByteCount() const { return 0; }
	/** Cheap scheduler query; sparse fields are the only non-ambient reaction work. */
	bool HasNonEnvironmentMaterialVolumeEnergy() const
	{
		return !MaterialVolumeFields.EnergyOverrides.IsEmpty();
	}
	bool IsMaterialHot() const;
	/** Stable mask-derived bounds used by world simulation indexes. */
	FBox GetCanonicalWorldBounds() const;
	FBox GetActiveWorldBounds() const;
	/** Sweeps against occupied cells in the current cut/reaction input mask. */
	bool SweepRuntimeMask(
		const FVector& Start,
		const FVector& End,
		float Radius,
		FVector& OutImpactLocation,
		FVector& OutImpactNormal) const;
	FName GetReactionStimulusMaterial() const;
	/** Derives shared flame instances and smoke anchors from immutable cell state. */
	void GatherReactionVisualTransforms(
		TArray<FTransform>& OutFlameTransforms,
		TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>& OutSmokeAnchors,
		int32 MaxVisualInstances) const;
	/** Compatibility projection for callers that only consume smoke. */
	void GatherReactionSmokeAnchors(
		TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>& OutAnchors,
		int32 MaxAnchors) const;
	bool HasLocalMaterialRule() const;
	/** Read-only shadow used while fragment authority migrates from Mask to Volume. */
	bool BuildMaterialVolumeInstance(FMaterialVolumeInstance& OutInstance) const;
	/** Resolve an occupied legacy X/Z cell to its stable Volume U/V/N address. */
	bool TryGetMaterialVolumeCellAtWorldLocation(
		const FVector& WorldLocation,
		FIntVector& OutVolumeCell) const;
	uint16 GetMaterialVolumeCellEnergy(
		const FIntVector& VolumeCell,
		uint16 DefaultEnergy) const;
	/** Validate and commit one field-only update without touching topology revision. */
	bool CommitMaterialVolumeCellEnergy(
		const FIntVector& VolumeCell,
		uint16 DefaultEnergy,
		uint16 ExpectedEnergy,
		uint16 AfterEnergy);
	/** Atomically commits material identity plus energy for one occupied Volume cell. */
	bool CommitMaterialVolumeCellState(
		const FIntVector& VolumeCell,
		uint16 DefaultEnergy,
		const FMaterialElementState& ExpectedBefore,
		const FMaterialElementState& After,
		FString& OutError);
	/** Validates every touched cell, then swaps topology and fields as one commit. */
	bool CommitMaterialVolumeElementBatch(
		const FMaterialDeltaBatch& Batch,
		FString& OutError);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	TObjectPtr<UProceduralMeshComponent> MeshComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fragment")
	TObjectPtr<UFragment2DAsset> FragmentAsset;

	UPROPERTY(EditAnywhere, ReplicatedUsing = OnRep_SourceAppearance, BlueprintReadOnly, Category = "Fragment")
	TObjectPtr<UMaterialInterface> FragmentMaterial;

	UPROPERTY(EditAnywhere, ReplicatedUsing = OnRep_SourceAppearance, BlueprintReadOnly, Category = "Fragment")
	FLinearColor FragmentColor = FLinearColor::White;

	UPROPERTY(ReplicatedUsing = OnRep_ProceduralSource, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	FFragmentSourceMask ProceduralSource;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	bool bDestroySourceOnFirstBreak = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	EFragmentSupportMode DefaultSupportMode =
		EFragmentSupportMode::Bottom;

	UPROPERTY(EditAnywhere, ReplicatedUsing = OnRep_SourceCollision, BlueprintReadOnly, Category = "Fragment")
	bool bEnableSourceCollision = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fragment")
	TSubclassOf<AFragment2DActor> FragmentActorClass;

	UPROPERTY(ReplicatedUsing = OnRep_SourceId, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	FGuid SourceId;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Reaction")
	FName SourceMaterialId = NAME_None;

	/** Structural material semantics used by generic visibility projection. */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Material")
	EMatterFluxMaterialStructuralRole StructuralRole =
		EMatterFluxMaterialStructuralRole::None;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	int32 Revision = 0;

	UPROPERTY(ReplicatedUsing = OnRep_Broken, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	bool bBroken = false;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	FGuid AggregateId;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	bool bAggregateRoot = false;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	bool bDetachedFromTerrain = false;

	UPROPERTY(ReplicatedUsing = OnRep_AggregateSeparationCollisionSuppressed, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	bool bAggregateSeparationCollisionSuppressed = false;

protected:
	UFUNCTION()
	void OnRep_Broken();

	UFUNCTION()
	void OnRep_SourceId();

	UFUNCTION()
	void OnRep_ProceduralSource();

	UFUNCTION()
	void OnRep_MaterialVolumeState();

	UFUNCTION()
	void OnRep_SourceAppearance();

	UFUNCTION()
	void OnRep_SourceCollision();

	UFUNCTION()
	void OnRep_AggregateSeparationCollisionSuppressed();

	void ApplyBrokenState();
	void ApplySourceCollisionState();
	void UpdateAggregateSeparationGracePeriod();
	void EndAggregateSeparationGracePeriod();
	void ApplySourceMaterial();
	void RebuildMaterialVisualization();
	void RefreshMaterialReactionVisualization();
	void ApplyMaterialReactionVisualProjection(const TArray<uint8>& HotCells);
	void RebuildOutputMesh(const TArray<uint8>& OutputCells, FName OutputMaterialId);
	void EnsureReactionVisualComponents();
	void MarkSharedSmokeVisualizationDirty() const;
	FIntPoint WorldToMaskCell(const FVector& WorldLocation) const;
	bool BuildMaterialProjection(
		TArray<uint8>& OutOutputCells,
		TArray<uint8>& OutHotCells,
		FName& OutOutputMaterialId,
		uint16& OutIgnitionThreshold) const;
	void EnsureInitialized();
	bool RebuildSourceMesh(const TArray<uint8>* RenderMask = nullptr);
	void BuildDefaultMask(TArray<uint8>& OutMask) const;
	void RefreshPresenceRegistration();
	void RefreshMaterialVolumeTopology();
	void PublishReplicatedMaterialVolumeState();
	bool ApplyReplicatedMaterialVolumeState(FString& OutError);

	UPROPERTY(Transient)
	TArray<uint8> RuntimeMask;

	UPROPERTY(ReplicatedUsing = OnRep_MaterialVolumeState)
	FMatterFluxReplicatedMaterialVolumeState ReplicatedMaterialVolumeState;

	UPROPERTY(Transient)
	TArray<uint8> SupportAnchorMask;

	UPROPERTY(Transient)
	TArray<uint8> RenderedSourceMask;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicFragmentMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicFragmentSideMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> OutputMeshComponent;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> FlameInstances;

	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> FireLight;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> OutputMaterialInstance;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> StimulusMaterialInstance;
private:
	friend class UFragmentSimulationSubsystem;
	friend class AMatterFluxPlayableWorldActor;

	bool PrepareMaterialVolumeElementDeltas(
		TConstArrayView<FMaterialElementDelta> Deltas,
		FMaterialVolumeTopology& OutTopology,
		FMaterialVolumeFields& OutFields,
		FString& OutError) const;
	void CommitPreparedMaterialVolumeState(
		FMaterialVolumeTopology&& Topology,
		FMaterialVolumeFields&& Fields);

	struct FPreparedFragmentDamage
	{
		FGuid SourceId;
		int32 BaseRevision = INDEX_NONE;
		int32 NewRevision = INDEX_NONE;
		TArray<uint8> SupportedMask;
		TArray<FFragmentSpawnPayload> Payloads;
	};

	bool PrepareDamageEvent(
		const FFragmentDamageEvent& DamageEvent,
		FPreparedFragmentDamage& OutTransaction,
		bool bForceDetachedPhysics = false) const;
	bool CommitPreparedDamage(FPreparedFragmentDamage& Transaction);

	FGuid RegisteredPresenceSourceId;
	TWeakObjectPtr<AActor> AggregateSeparationCarrier;
	FTimerHandle AggregateSeparationTimerHandle;
	double AggregateSeparationEarliestEndSeconds = 0.0;
	double AggregateSeparationDeadlineSeconds = 0.0;
	bool bSourceMeshProjectionEnabled = true;
	TOptional<FMaterialVolumeTopology> MaterialVolumeTopology;
	FMaterialVolumeFields MaterialVolumeFields;
};
