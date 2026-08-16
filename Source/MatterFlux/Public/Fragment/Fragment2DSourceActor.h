#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Fragment/FragmentTypes.h"
#include "Material/MatterFluxCombustion.h"
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
		FName InMaterialId = NAME_None);
	bool CaptureStreamingState(
		FFragment2DSourceStreamingState& OutState,
		FString& OutError) const;
	bool RestoreStreamingState(
		const FFragment2DSourceStreamingState& State,
		FString& OutError);
	bool IgniteAtWorldLocation(
		const FVector& WorldLocation,
		FName IgnitionMaterial = TEXT("fire"),
		int32 EventSeed = 0);
	void SetSourceCollisionEnabled(bool bEnabled);
	void ConfigureAggregate(
		const FGuid& InAggregateId,
		bool bInAggregateRoot);
	void TransferAggregateMembersTo(AActor& CarrierActor);
	void MarkBroken();

	int32 GetMaskWidth() const;
	int32 GetMaskHeight() const;
	float GetCellSize() const;
	int32 GetMinFragmentAreaPixels() const;
	int32 GetMaxFragmentsPerBreak() const;
	const TArray<uint8>& GetRuntimeMask() const { return RuntimeMask; }
	int32 GetRemainingFuelCellCount() const;
	int32 GetResidueCellCount() const;
	int32 GetBurningCellCount() const;
	int32 GetTotalSmokeEmissionCount() const
	{
		return TotalSmokeEmissionCount;
	}
	int32 GetReplicatedCombustionByteCount() const
	{
		return ReplicatedCombustionFuelMask.Num()
			+ ReplicatedCombustionResidueMask.Num()
			+ ReplicatedCombustionBurningMask.Num();
	}
	bool IsCombusting() const;
	FBox GetBurningWorldBounds() const;
	FName GetCombustionFlameMaterial() const;
	bool HasCombustionRule() const
	{
		return FindCombustionRule() != nullptr;
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

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Combustion")
	FName SourceMaterialId = NAME_None;

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
	void OnRep_CombustionState();

	void ApplyBrokenState();
	void ApplySourceCollisionState();
	void ApplySourceMaterial();
	void AdvanceCombustion(float DeltaSeconds);
	void RebuildCombustionVisualization();
	void RebuildResidueMesh();
	void EnsureCombustionVisualComponents();
	void AddSmokeEmissions(const TArray<FIntPoint>& Cells);
	FIntPoint WorldToMaskCell(const FVector& WorldLocation) const;
	void PublishCombustionState();
	const FMatterFluxCombustionDefinition* FindCombustionRule() const;
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
	TObjectPtr<UProceduralMeshComponent> ResidueMeshComponent;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> FlameInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> SmokeInstances;

	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> FireLight;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ResidueMaterialInstance;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FlameMaterialInstance;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SmokeMaterialInstance;

	UPROPERTY(Replicated)
	TArray<uint8> ReplicatedCombustionFuelMask;

	UPROPERTY(Replicated)
	TArray<uint8> ReplicatedCombustionResidueMask;

	UPROPERTY(Replicated)
	TArray<uint8> ReplicatedCombustionBurningMask;

	UPROPERTY(ReplicatedUsing = OnRep_CombustionState)
	int32 CombustionRevision = 0;

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

	bool PrepareDamageEvent(const FFragmentDamageEvent& DamageEvent, FPreparedFragmentDamage& OutTransaction) const;
	bool CommitPreparedDamage(FPreparedFragmentDamage& Transaction);

	struct FSmokeParticle
	{
		FVector LocalPosition = FVector::ZeroVector;
		FVector LocalVelocity = FVector::ZeroVector;
		float RemainingLife = 0.0f;
	};

	TUniquePtr<MatterFlux::Combustion::FMaskCombustion>
		CombustionSimulation;
	TArray<uint8> ResidueMask;
	TArray<uint8> VisibleBurningMask;
	TArray<FSmokeParticle> SmokeParticles;
	float CombustionAccumulator = 0.0f;
	float CombustionVisualAccumulator = 0.0f;
	int32 TotalSmokeEmissionCount = 0;
	FGuid RegisteredPresenceSourceId;
	bool bCombustionVisualDirty = false;
	bool bCombustionGeometryDirty = false;
};
