#pragma once

#include "CoreMinimal.h"
#include "Creatures/MatterFluxCreatureCastProgram.h"
#include "GameFramework/Character.h"
#include "MatterFluxContentTypes.h"
#include "MatterFluxCreatureActor.generated.h"

class AMatterFluxCreatureAIController;
class AMatterFluxPlayerState;
class UMaterialInstanceDynamic;
class UMatterFluxBuoyancyComponent;
class UStaticMeshComponent;

UENUM(BlueprintType)
enum class EMatterFluxCreatureRuntimeState : uint8
{
	Passive,
	Patrol,
	Chase,
	Retreat,
	Attack,
	Skill,
	Dead
};

/** Replicated world creature whose behavior program comes from Lua content. */
UCLASS()
class MATTERFLUX_API AMatterFluxCreatureActor : public ACharacter
{
	GENERATED_BODY()

public:
	AMatterFluxCreatureActor();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void InitializeCreature(FName InDefinitionId);
	FName GetDefinitionId() const { return DefinitionId; }
	float GetCurrentHealth() const { return CurrentHealth; }
	EMatterFluxCreatureRuntimeState GetRuntimeState() const
	{
		return RuntimeState;
	}
	const FMatterFluxCreatureDefinition* ResolveDefinition() const;
	bool CanInteract(const APawn& Interactor) const;
	bool PurchaseOfferAuthority(
		AMatterFluxPlayerState& Buyer,
		int32 OfferIndex,
		int32 ExpectedProgressionRevision,
		int32& OutRemainingPurchases,
		FString& OutError);
	bool ApplyDamageAuthority(float Damage, AActor* DamageSource);
	bool CastConfiguredSpellAuthority(
		AActor& Target,
		bool bUseSkill,
		int32 EventSeed);
	bool IsCastSequenceActive() const { return bCastSequenceActive; }
	void SetRuntimeStateAuthority(EMatterFluxCreatureRuntimeState NewState);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Creature|Visual")
	TObjectPtr<UStaticMeshComponent> BodyVisual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Creature|Visual")
	TObjectPtr<UStaticMeshComponent> HeadVisual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Creature|Visual")
	TObjectPtr<UStaticMeshComponent> AccentVisual;

	/** NPC 与敌人共享的材质液体浮力组件。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Creature|Liquid")
	TObjectPtr<UMatterFluxBuoyancyComponent> BuoyancyComponent;

private:
	void ApplyDefinitionPresentation();
	void HandleDeathAuthority(AActor* DamageSource);
	AMatterFluxPlayerState* ResolveKillerPlayerState(AActor* DamageSource) const;
	bool SpawnCastShotAuthority(
		const FMatterFluxCreatureCastProgramDefinition& Program,
		const FMatterFluxCreatureCastShot& Shot);
	void SpawnNextPendingCastShotAuthority();
	void FinishCastSequenceAuthority();

	UFUNCTION()
	void OnRep_Definition();

	UFUNCTION()
	void OnRep_Health();

	UPROPERTY(ReplicatedUsing = OnRep_Definition)
	FName DefinitionId;

	UPROPERTY(ReplicatedUsing = OnRep_Health)
	float CurrentHealth = 1.0f;

	UPROPERTY(Replicated)
	EMatterFluxCreatureRuntimeState RuntimeState =
		EMatterFluxCreatureRuntimeState::Passive;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> VisualMaterials;
	FMatterFluxCreatureCastProgramDefinition PendingCastProgram;
	TArray<FMatterFluxCreatureCastShot> PendingCastShots;
	int32 PendingCastShotIndex = 0;
	FTimerHandle PendingCastTimer;
	FTimerHandle CastRecoveryTimer;
	bool bCastSequenceActive = false;
	bool bDeathHandled = false;
};
