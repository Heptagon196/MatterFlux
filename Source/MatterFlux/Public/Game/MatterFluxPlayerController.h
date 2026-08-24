#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "MatterFluxPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class ULocalPlayer;
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
	void CreateMagicWorkbench();
	void CreateShell();
	void CreateQuestTracker();
	void CreatePlayerStatusHud();
	void CreateInteractionWidget();
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
};
