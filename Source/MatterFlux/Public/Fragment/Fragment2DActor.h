#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Fragment/FragmentTypes.h"
#include "Rendering/MatterFluxSmokeVisualPool.h"
#include "Fragment2DActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPhysicalMaterial;
class UMatterFluxBuoyancyComponent;
class AFragment2DSourceActor;
struct FFragment2DSourceStreamingState;
namespace MatterFlux::Combustion
{
	class FSourceCombustionRuntime;
}

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
		const FFragment2DSourceStreamingState& State,
		FName ResidueMaterialId,
		const FLinearColor& ResidueColor);
	bool GetAggregateSourceWorldTransform(
		const FGuid& SourceId,
		FTransform& OutWorldTransform) const;
	bool GetAggregateSourceState(
		const FGuid& SourceId,
		FFragmentAggregateSourceState& OutState) const;
	bool IgniteAtWorldLocation(
		const FVector& WorldLocation,
		FName FlameMaterial,
		int32 EventSeed);
	bool IgniteAggregateSourceAtWorldLocation(
		const FGuid& SourceId,
		const FVector& WorldLocation,
		FName FlameMaterial,
		int32 EventSeed);
	bool IgniteInCone(
		const FVector& Start,
		const FVector& Direction,
		float Range,
		float StartRadius,
		float EndRadius,
		FName FlameMaterial,
		int32 EventSeed);
	bool IsRootCombusting() const;
	bool IsAggregateSourceCombusting(const FGuid& SourceId) const;
	bool IsAnyAggregateMaterialCombusting(FName MaterialId) const;
	int32 GetRootCombustionResidueCellCount() const;
	FBox GetCombustibleWorldBounds() const;
	void GatherRootCombustionVisualTransforms(
		TArray<FTransform>& OutFlameTransforms,
		TArray<MatterFlux::Rendering::FSmokeEmissionAnchor>& OutSmokeAnchors,
		int32 MaxVisualInstances) const;
	float GetTransientFadeAlpha() const { return TransientFadeAlpha; }
	float GetVisualDepthOffset() const { return VisualDepthOffset; }

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

	/** Combustion state for the detached payload itself (for example the cut trunk). */
	UPROPERTY(ReplicatedUsing = OnRep_RootCombustionState, VisibleAnywhere, BlueprintReadOnly, Category = "Fragment|Combustion")
	FFragmentAggregateSourceState RootCombustionState;

protected:
	UFUNCTION()
	void OnRep_SpawnPayload();

	UFUNCTION()
	void OnRep_FragmentMaterial();

	UFUNCTION()
	void OnRep_AggregateSources();

	UFUNCTION()
	void OnRep_RootCombustionState();

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
	void ApplyTransientFadeAlpha();
	void InitializeRootCombustionState();
	bool IgniteRootAtWorldLocation(
		const FVector& WorldLocation,
		FName FlameMaterial,
		int32 EventSeed);
	bool SynchronizeRootCombustionState();
	void AdvanceRootCombustion(float DeltaSeconds);
	bool IgniteDetachedAggregateAtWorldLocation(
		const FGuid& SourceId,
		const FVector& WorldLocation,
		FName FlameMaterial,
		int32 EventSeed);
	bool SynchronizeDetachedAggregateCombustionState(const FGuid& SourceId);
	void AdvanceDetachedAggregateCombustion(float DeltaSeconds);
	bool PropagateCombustionToAdjacentLayer(
		const FFragmentAggregateSourceState& BurningSource,
		const FTransform& BurningWorldTransform,
		FName FlameMaterial,
		int32 EventSeed);
	void MarkCombustionVisualizationDirty() const;
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
	float RootCombustionPropagationAccumulator = 0.0f;
	float DetachedAggregateCombustionPropagationAccumulator = 0.0f;
	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime>
		RootCombustionRuntime;
	TMap<FGuid, TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime>>
		DetachedAggregateCombustionRuntimes;
};
