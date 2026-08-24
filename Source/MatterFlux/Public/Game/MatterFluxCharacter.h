#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "Game/MatterFluxPlayerOperation.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"
#include "MatterFluxCharacter.generated.h"

class UAbilitySystemComponent;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class UInstancedStaticMeshComponent;
class ULocalPlayer;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UMeshComponent;
class UMatterFluxBuoyancyComponent;
class USpringArmComponent;
class UStaticMeshComponent;
struct FInputActionValue;

UENUM(BlueprintType)
enum class EMatterFluxPlayerAbilityEffect : uint8
{
	Cut,
	FlameJet
};

UCLASS()
class MATTERFLUX_API AMatterFluxCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	explicit AMatterFluxCharacter(
		const FObjectInitializer& ObjectInitializer =
			FObjectInitializer::Get());

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(
		const EEndPlayReason::Type EndPlayReason) override;
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void PawnClientRestart() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	const UInputMappingContext* GetPlayableMappingContext() const
	{
		return PlayableMappingContext;
	}
	const UInputAction* GetCastWandAction(int32 EquipmentSlot) const
	{
		return CastWandActions.IsValidIndex(EquipmentSlot)
			? CastWandActions[EquipmentSlot]
			: nullptr;
	}
	static void BuildAbilityEffectTransforms(
		EMatterFluxPlayerAbilityEffect Effect,
		TArray<FTransform>& OutTransforms);
	void BroadcastAbilityEffect(EMatterFluxPlayerAbilityEffect Effect);
	void ApplyPlayerOperation(
		EMatterFluxPlayerOperation Operation,
		const FVector2D& Value = FVector2D::ZeroVector,
		int32 IntegerValue = 0);
	void RelayPlayerOperationToServer(
		EMatterFluxPlayerOperation Operation,
		const FVector2D& Value = FVector2D::ZeroVector,
		int32 IntegerValue = 0);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Visual")
	TObjectPtr<UStaticMeshComponent> CharacterVisual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Visual")
	TObjectPtr<UStaticMeshComponent> HeadVisual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Visual")
	TObjectPtr<UStaticMeshComponent> LeftArmVisual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Visual")
	TObjectPtr<UStaticMeshComponent> RightArmVisual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Visual")
	TObjectPtr<UStaticMeshComponent> LeftFootVisual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Visual")
	TObjectPtr<UStaticMeshComponent> RightFootVisual;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Ability")
	TObjectPtr<UInstancedStaticMeshComponent> CutEffectInstances;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Ability")
	TObjectPtr<UInstancedStaticMeshComponent> FlameEffectInstances;

	/** 主角使用的统一材质液体浮力组件。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playable|Liquid")
	TObjectPtr<UMatterFluxBuoyancyComponent> BuoyancyComponent;

private:
	void InitAbilityActorInfo();
	void EnsurePlayableInputAssets();
	void InstallPlayableInputContext();
	void HandleMove(const FInputActionValue& Value);
	void HandleMoveCompleted();
	void HandleCameraZoom(const FInputActionValue& Value);
	void HandleJumpStarted();
	void HandleJumpCompleted();
	void HandleCastWandStarted(int32 EquipmentSlot);
	void HandleCastWandStopped(int32 EquipmentSlot);
	void HandleCastWandRequested(int32 EquipmentSlot);
	void HandleRegenerateRequested();
	void TryActivateWandSlot(int32 EquipmentSlot);
	void ExecuteRegenerateRequest(int32 RequestedSeed = 0);
	void PlayAbilityEffect(EMatterFluxPlayerAbilityEffect Effect);
	void PublishPlayerOperation(
		EMatterFluxPlayerOperation Operation,
		const FVector2D& Value = FVector2D::ZeroVector,
		int32 IntegerValue = 0);
	void ClearCutEffect();
	void ClearFlameEffect();
	void UpdateItemOcclusionGhosting(float DeltaSeconds);
	void RestoreItemOcclusionGhosts();
	void SetGhostRevealOutlineEnabled(bool bEnabled);
	void ApplyItemOcclusionGhost(
		AActor& Actor,
		UMeshComponent& ItemMesh,
		const FLinearColor& Color);

	struct FItemOcclusionGhostState
	{
		TWeakObjectPtr<UMeshComponent> Mesh;
		TArray<TWeakObjectPtr<UMaterialInterface>> SolidMaterials;
		TArray<TWeakObjectPtr<UMaterialInterface>> GhostMaterials;
		float CurrentOpacity = 1.0f;
		bool bGhostDesired = false;
	};

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayAbilityEffect(
		EMatterFluxPlayerAbilityEffect Effect);

	UFUNCTION(Server, Reliable)
	void ServerRegenerateLevel();

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRelayPlayerOperation(
		uint8 Operation,
		FVector2D Value,
		int32 IntegerValue);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerActivateWandSlot(int32 EquipmentSlot);

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> PlayableMappingContext;

	TWeakObjectPtr<ULocalPlayer> PlayableInputLocalPlayer;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> CameraZoomAction;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> CastWandActions;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> RegenerateAction;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CutEffectMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FlameEffectMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PlayerOutlineMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PlayerGhostOutlineMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ItemOcclusionGhostMaterial;

	TMap<TWeakObjectPtr<AActor>, FItemOcclusionGhostState>
		ItemOcclusionGhostStates;
	bool bGhostRevealOutlineEnabled = false;

	FTimerHandle CutEffectTimer;
	FTimerHandle FlameEffectTimer;
	TArray<FTimerHandle> WandCastRepeatTimers;
	double LastRegenerateRequestTime = -1.0;
};
