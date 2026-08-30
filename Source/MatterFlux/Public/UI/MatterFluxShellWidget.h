#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MatterFluxShellWidget.generated.h"

class AMatterFluxPlayerController;
class AMatterFluxPlayerState;
class UMatterFluxProgressionComponent;
class UMatterFluxSaveSubsystem;
enum class EMatterFluxSaveOperation : uint8;

UENUM()
enum class EMatterFluxShellView : uint8
{
	Gameplay,
	StartMenu,
	SinglePlayerMenu,
	MultiplayerMenu,
	CreateRoomMenu,
	JoinRoomMenu,
	PauseMenu,
	Settings,
	SaveSlots,
	LoadSlots
};

UCLASS()
class MATTERFLUX_API UMatterFluxShellWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// Keep multiplayer availability centralized so the menu and its guarded
	// actions cannot drift apart.
	static constexpr bool IsMultiplayerEntryEnabled() { return true; }
	static FName GetStoryMapId() { return TEXT("story.paper_magic"); }
	static int32 ResolveOwnedCoinQuantity(
		const AMatterFluxPlayerState* PlayerState);

	void InitializeForPlayer(AMatterFluxPlayerController* Controller);
	int32 GetOwnedCoinQuantity() const;
	void ShowStartMenu();
	void ShowSinglePlayerMenu();
	void ShowMultiplayerMenu();
	void ShowCreateRoomMenu();
	void ShowJoinRoomMenu();
	void ShowPauseMenu();
	void ShowSettings();
	void ShowSaveSlots();
	void ShowLoadSlots();
	void CloseMenus();
	bool IsMenuOpen() const { return View != EMatterFluxShellView::Gameplay; }
	bool IsStartMenuOpen() const
	{
		return bFrontEndContext && IsMenuOpen();
	}
	bool IsFrontEndView() const;
	EMatterFluxShellView GetView() const { return View; }
	AMatterFluxPlayerController* GetMatterFluxController() const;
	UMatterFluxSaveSubsystem* GetSaveSubsystem() const;
	const FString& GetTransientNotice() const { return TransientNotice; }
	const FString& GetJoinAddress() const { return JoinAddress; }
	void SetJoinAddress(const FString& Address) { JoinAddress = Address; }

	void RequestNewGame();
	void RequestStoryMode();
	void RequestContinue();
	void RequestHostRoom(int32 SaveSlotIndex = INDEX_NONE);
	void SelectHostSlot(int32 SlotIndex);
	int32 GetSelectedHostSlotIndex() const
	{
		return SelectedHostSlotIndex;
	}
	void RequestJoinRoom();
	void RequestSlotOperation(int32 SlotIndex, bool bLoad);
	void RequestDuplicateSlot(int32 SlotIndex);
	void RequestDeleteSlot(int32 SlotIndex);
	void BeginRenameSlot(int32 SlotIndex);
	void CommitRenameSlot();
	void CancelRenameSlot();
	bool IsRenamingSlot(int32 SlotIndex) const
	{
		return RenamingSlotIndex == SlotIndex;
	}
	const FString& GetRenameDraft() const { return RenameDraft; }
	void SetRenameDraft(const FString& Value) { RenameDraft = Value; }
	void RequestQuit();
	void RequestMagicWorkbench();
	void ReturnFromSubmenu();
	void RefreshShell();

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(
		const FGeometry& MyGeometry,
		float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(
		const FGeometry& InGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	// PlayerController owns the only project-internal visual-capture bypass;
	// ordinary UI callers must still complete a real save/session operation.
	friend class AMatterFluxPlayerController;
	void CancelPendingJoinForSinglePlayer();
	void RejectMultiplayerEntry();
	void SetView(EMatterFluxShellView NewView);
	void EnterGameplayAfterSuccessfulOperation();
	void NotifyControllerState(bool bOperationActive = false) const;
	UMatterFluxProgressionComponent* ResolveProgression() const;
	void BindProgression();
	void UnbindProgression();

	UPROPERTY(Transient)
	TObjectPtr<AMatterFluxPlayerController> OwnerController;
	TWeakObjectPtr<UMatterFluxProgressionComponent> BoundProgression;
	FDelegateHandle ProgressionChangedHandle;

	EMatterFluxShellView View = EMatterFluxShellView::Gameplay;
	EMatterFluxShellView SubmenuReturnView = EMatterFluxShellView::PauseMenu;
	EMatterFluxSaveOperation LastObservedOperation =
		static_cast<EMatterFluxSaveOperation>(0);
	bool bCloseAfterSuccessfulOperation = false;
	bool bHostRestorePending = false;
	bool bFrontEndContext = false;
	int32 RenamingSlotIndex = INDEX_NONE;
	int32 SelectedHostSlotIndex = INDEX_NONE;
	FString RenameDraft;
	FString JoinAddress = TEXT("127.0.0.1:7777");
	FString TransientNotice;
	TSharedPtr<SWidget> Shell;
};
