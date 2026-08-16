#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Fragment/FragmentTypes.h"
#include "Fragment2DActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class AFragment2DSourceActor;
struct FFragment2DSourceStreamingState;

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

	/** Bit-packed by FFragmentSourceMask::NetSerialize. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	FFragmentSourceMask ResidueMask;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	FFragmentSourceMask BurningMask;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	bool bHasCombustionState = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	FName CombustionRuleId = NAME_None;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	FName ResidueMaterialId = NAME_None;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	FLinearColor ResidueColor = FLinearColor(0.08f, 0.07f, 0.06f);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	int32 CombustionSeed = 0;

	UPROPERTY()
	uint32 CombustionTick = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	float CombustionAccumulator = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate|Combustion")
	int32 TotalSmokeEmissionCount = 0;

	bool IsValid() const;
};

UCLASS(Blueprintable)
class MATTERFLUX_API AFragment2DActor : public AActor
{
	GENERATED_BODY()

public:
	AFragment2DActor();
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual bool InitializeFromPayload(const FFragmentSpawnPayload& Payload);
	bool AbsorbAggregateSource(AFragment2DSourceActor& SourceActor);
	int32 GetAggregateMemberCount() const { return AggregateSources.Num(); }
	bool ContainsAggregateSource(const FGuid& SourceId) const;
	FName GetAggregateSourceMaterialId(const FGuid& SourceId) const;
	bool ApplyAggregateSourceStreamingState(
		const FGuid& SourceId,
		const FFragment2DSourceStreamingState& State,
		FName ResidueMaterialId,
		const FLinearColor& ResidueColor);
	bool GetAggregateSourceWorldTransform(
		const FGuid& SourceId,
		FTransform& OutWorldTransform) const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	TObjectPtr<UProceduralMeshComponent> MeshComponent;

	UPROPERTY(ReplicatedUsing = OnRep_SpawnPayload, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	FFragmentSpawnPayload SpawnPayload;

	UPROPERTY(ReplicatedUsing = OnRep_FragmentMaterial, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	TObjectPtr<UMaterialInterface> FragmentMaterial;

	UPROPERTY(ReplicatedUsing = OnRep_FragmentMaterial, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment")
	FLinearColor FragmentColor = FLinearColor::White;

	UPROPERTY(ReplicatedUsing = OnRep_AggregateSources, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Aggregate")
	TArray<FFragmentAggregateSourceState> AggregateSources;

protected:
	UFUNCTION()
	void OnRep_SpawnPayload();

	UFUNCTION()
	void OnRep_FragmentMaterial();

	UFUNCTION()
	void OnRep_AggregateSources();

	void ApplyFragmentMaterial();
	bool RebuildMeshFromPayload();
	bool RebuildSimpleCollision();
	bool RebuildAggregateSourceSections();
	bool RebuildAggregateVisualSectionsOnly();
	void NotifyWorldOfAggregateSources();

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicFragmentMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicFragmentSideMaterial;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> AggregateDynamicMaterials;
};
