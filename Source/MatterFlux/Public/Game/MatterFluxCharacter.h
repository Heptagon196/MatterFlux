#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "Game/MatterFluxPlayerOperation.h"
#include "GameFramework/Character.h"
#include "MatterFluxCharacter.generated.h"

class UAbilitySystemComponent;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class UInstancedStaticMeshComponent;
class ULocalPlayer;
class UMaterialInstanceDynamic;
class UMaterialInterface;
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
	AMatterFluxCharacter();

	virtual void BeginPlay() override;
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

private:
	void InitAbilityActorInfo();
	void EnsurePlayableInputAssets();
	void InstallPlayableInputContext();
	void HandleMove(const FInputActionValue& Value);
	void HandleMoveCompleted();
	void HandleCameraZoom(const FInputActionValue& Value);
	void HandleJumpStarted();
	void HandleJumpCompleted();
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

	FTimerHandle CutEffectTimer;
	FTimerHandle FlameEffectTimer;
	double LastRegenerateRequestTime = -1.0;
};
