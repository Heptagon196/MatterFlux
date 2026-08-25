#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "MatterFluxPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class ULocalPlayer;
class SWidget;
class UMatterFluxMagicWorkbenchWidget;
class UMatterFluxShellWidget;
class UMatterFluxQuestTrackerWidget;
class UMatterFluxPlayerStatusWidget;
class UMatterFluxInteractionWidget;
class AMatterFluxCreatureActor;

UCLASS()
class MATTERFLUX_API AMatterFluxPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AMatterFluxPlayerController();

	virtual void BeginPlay() override;
	virtual void EndPlay(
		const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnRep_PlayerState() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;
	bool AreDebugControlsEnabled() const { return bEnableDebugControls; }
	void ToggleMagicWorkbench();
	void ToggleQuestJournal();
	bool IsMagicWorkbenchOpen() const;
	void ShowMagicWorkbenchPage(bool bShowWandBackpack);
	bool ShowMagicWorkbenchNamedPage(const FString& PageName);
	bool SelectMagicWorkbenchEquipmentSlot(int32 EquipmentSlot);
	void TogglePauseMenu();
	void ShowStartMenu();
	void ShowSinglePlayerMenu();
	void ShowMultiplayerMenu();
	void ShowCreateRoomMenu();
	void ShowJoinRoomMenu();
	void ShowSettingsMenu();
	void ShowSaveMenu();
	void ShowLoadMenu();
	void CloseShellMenu();
	/** 仅供项目内可视验收命令绕过前端菜单，不执行存档操作。 */
	void EnterGameplayForVisualCapture();
	/** 隐藏可视验收截图中的所有本地 UI，不改变实际游戏状态。 */
	void HideUIForVisualCapture();
	/** 通过实际交互弹窗打开指定商店，供项目内可视验收使用。 */
	bool OpenCreatureShopForVisualCapture(
		AMatterFluxCreatureActor* Creature,
		FName DialogueId,
		FName ShopId);
	bool IsShellMenuOpen() const;
	void HandleShellStateChanged(bool bMenuOpen, bool bOperationActive);
	bool HostListenRoom(int32 SaveSlotIndex, FString& OutError);
	bool JoinRoomByAddress(
		const FString& Address,
		FString& OutNormalizedAddress,
		FString& OutError);
	static bool NormalizeJoinAddress(
		const FString& Address,
		FString& OutNormalizedAddress,
		FString& OutError);
	/** Resolve a world-space cursor target into a level horizontal aim direction. */
	bool TryGetHorizontalMouseAimDirection(
		const FVector& AimOrigin,
		FVector& OutDirection) const;
	/** Pure helper shared with automation tests and replay-safe aiming code. */
	static bool MakeHorizontalAimDirection(
		const FVector& AimOrigin,
		const FVector& TargetWorldLocation,
		FVector& OutDirection);
	void RequestCreaturePurchase(
		AMatterFluxCreatureActor* Creature,
		int32 OfferIndex,
		int32 ExpectedProgressionRevision);
	void CloseCreatureInteraction();

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input|Debug")
	bool bEnableDebugControls = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TSoftObjectPtr<UInputMappingContext> DebugMappingContext;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TSoftObjectPtr<UInputAction> DebugDamageAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	int32 DebugMappingPriority = 0;

private:
	friend class FMatterFluxGameplayFocusRestorationTest;
	/** Return keyboard routing to the viewport without capturing the mouse. */
	static void RestoreGameplayViewportFocus(
		const TSharedPtr<SWidget>& GameplayViewport);
	/** Repair focus after the Slate event that closed a menu has fully unwound. */
	static void RestoreGameplayViewportFocusAfterSlateEvent(
		const TSharedPtr<SWidget>& GameplayViewport,
		TFunction<bool()> ShouldRestoreFocus);
	void ApplyGameplayMouseInputMode();
	void CreateMagicWorkbench();
	void CreateShell();
	void CreateQuestTracker();
	void CreatePlayerStatusHud();
	void CreateInteractionWidget();
	void RefreshInteractionPrompt();
	AMatterFluxCreatureActor* FindNearestInteractableCreature() const;
	void CloseMagicWorkbench();
	void TryInteract();
	void AddDebugMappingContext();
	void GrantDebugAbilityIfEnabled();
	void HandleDebugDamageInput();
	UInputAction* GetOrCreateDebugDamageAction();
	UInputMappingContext* GetOrCreateDebugMappingContext();

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerInteract(AMatterFluxCreatureActor* Creature);
	bool ServerInteract_Validate(AMatterFluxCreatureActor* Creature);
	void ServerInteract_Implementation(AMatterFluxCreatureActor* Creature);

	UFUNCTION(Client, Reliable)
	void ClientOpenCreatureInteraction(
		AMatterFluxCreatureActor* Creature,
		FName DialogueId);
	void ClientOpenCreatureInteraction_Implementation(
		AMatterFluxCreatureActor* Creature,
		FName DialogueId);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerPurchaseCreatureOffer(
		AMatterFluxCreatureActor* Creature,
		int32 OfferIndex,
		int32 ExpectedProgressionRevision);
	bool ServerPurchaseCreatureOffer_Validate(
		AMatterFluxCreatureActor* Creature,
		int32 OfferIndex,
		int32 ExpectedProgressionRevision);
	void ServerPurchaseCreatureOffer_Implementation(
		AMatterFluxCreatureActor* Creature,
		int32 OfferIndex,
		int32 ExpectedProgressionRevision);

	UFUNCTION(Client, Reliable)
	void ClientCreaturePurchaseResult(
		bool bSuccess,
		int32 OfferIndex,
		int32 RemainingPurchases,
		const FString& Message);
	void ClientCreaturePurchaseResult_Implementation(
		bool bSuccess,
		int32 OfferIndex,
		int32 RemainingPurchases,
		const FString& Message);

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> RuntimeDebugDamageAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> RuntimeDebugMappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> AppliedDebugMappingContext;

	TWeakObjectPtr<ULocalPlayer> DebugInputLocalPlayer;

	UPROPERTY(Transient)
	TObjectPtr<UMatterFluxMagicWorkbenchWidget> MagicWorkbench;

	UPROPERTY(Transient)
	TObjectPtr<UMatterFluxShellWidget> ShellWidget;

	UPROPERTY(Transient)
	TObjectPtr<UMatterFluxQuestTrackerWidget> QuestTracker;

	UPROPERTY(Transient)
	TObjectPtr<UMatterFluxPlayerStatusWidget> PlayerStatusHud;

	UPROPERTY(Transient)
	TObjectPtr<UMatterFluxInteractionWidget> InteractionWidget;

	float InteractionPromptRefreshRemaining = 0.0f;
};
