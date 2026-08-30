#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Fragment/FragmentTypes.h"
#include "Material/MatterFluxReaction.h"
#include "Rendering/MatterFluxSmokeVisualPool.h"
#include "Fragment2DSourceActor.generated.h"

class UFragment2DAsset;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UInstancedStaticMeshComponent;
class UPointLightComponent;
class AFragment2DActor;
class UFragmentSimulationSubsystem;

UCLASS(Blueprintable)
class MATTERFLUX_API AFragment2DSourceActor : public AActor
{
	GENERATED_BODY()

public:
	AFragment2DSourceActor();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
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
			EMatterFluxMaterialStructuralRole::None,
		int32 InStructuralFloorTier = 0);
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
	int32 GetMinFragmentAreaPixels() const;
	int32 GetMaxFragmentsPerBreak() const;
	const TArray<uint8>& GetRuntimeMask() const { return RuntimeMask; }
	int32 GetRemainingInputCellCount() const;
	int32 GetOutputCellCount() const;
	int32 GetActiveCellCount() const;
	int32 GetTotalMaterialEmissionCount() const
	{
		return TotalMaterialEmissionCount;
	}
	int32 GetReplicatedReactionByteCount() const
	{
		return ReplicatedReactionInputMask.Num()
			+ ReplicatedReactionOutputMask.Num()
			+ ReplicatedReactionActiveMask.Num();
	}
	bool IsReacting() const;
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
	void GatherReactionSmokeAnchors(
		TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>& OutAnchors,
		int32 MaxAnchors) const;
	bool HasReactionRule() const
	{
		return FindReactionRule() != nullptr;
	}

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

	/** Stable projection tier for multi-floor structures; replicated with spawn state. */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Material")
	int32 StructuralFloorTier = 0;

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
	void OnRep_SourceAppearance();

	UFUNCTION()
	void OnRep_SourceCollision();

	UFUNCTION()
	void OnRep_AggregateSeparationCollisionSuppressed();

	UFUNCTION()
	void OnRep_ReactionState();

	void ApplyBrokenState();
	void ApplySourceCollisionState();
	void UpdateAggregateSeparationGracePeriod();
	void EndAggregateSeparationGracePeriod();
	void ApplySourceMaterial();
	void AdvanceReaction(float DeltaSeconds);
	void RebuildReactionVisualization();
	void RebuildOutputMesh();
	void EnsureReactionVisualComponents();
	void AddMaterialEmissions(const TArray<FIntPoint>& Cells);
	void MarkSharedSmokeVisualizationDirty() const;
	FIntPoint WorldToMaskCell(const FVector& WorldLocation) const;
	void PublishReactionState();
	const FMatterFluxReactionDefinition* FindReactionRule() const;
	const FMatterFluxReactionDefinition* FindReactionRule(
		FName StimulusMaterial) const;
	void EnsureInitialized();
	bool RebuildSourceMesh();
	void BuildDefaultMask(TArray<uint8>& OutMask) const;
	void RefreshPresenceRegistration();

	UPROPERTY(Transient)
	TArray<uint8> RuntimeMask;

	UPROPERTY(Transient)
	TArray<uint8> SupportAnchorMask;

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


	UPROPERTY(Replicated)
	TArray<uint8> ReplicatedReactionInputMask;

	UPROPERTY(Replicated)
	TArray<uint8> ReplicatedReactionOutputMask;

	UPROPERTY(Replicated)
	TArray<uint8> ReplicatedReactionActiveMask;

	UPROPERTY(ReplicatedUsing = OnRep_ReactionState)
	int32 ReactionRevision = 0;

private:
	friend class UFragmentSimulationSubsystem;

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

	TUniquePtr<MatterFlux::Reaction::FMaskReaction>
		ReactionSimulation;
	TArray<uint8> OutputMask;
	TArray<uint8> VisibleActiveMask;
	float ReactionAccumulator = 0.0f;
	float ReactionVisualAccumulator = 0.0f;
	int32 TotalMaterialEmissionCount = 0;
	FGuid RegisteredPresenceSourceId;
	TWeakObjectPtr<AActor> AggregateSeparationCarrier;
	FTimerHandle AggregateSeparationTimerHandle;
	double AggregateSeparationEarliestEndSeconds = 0.0;
	double AggregateSeparationDeadlineSeconds = 0.0;
	bool bReactionVisualDirty = false;
	bool bReactionGeometryDirty = false;
	bool bSourceMeshProjectionEnabled = true;
};
