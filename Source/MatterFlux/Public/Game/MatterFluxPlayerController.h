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
	void CloseMagicWorkbench();
	void AddDebugMappingContext();
	void GrantDebugAbilityIfEnabled();
	void HandleDebugDamageInput();
	UInputAction* GetOrCreateDebugDamageAction();
	UInputMappingContext* GetOrCreateDebugMappingContext();

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
};
