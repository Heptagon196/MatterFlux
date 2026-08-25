#include "UI/MatterFluxShellWidget.h"
#include "UI/MatterFluxShellSlate.h"

#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "Engine/Engine.h"
#include "InputCoreTypes.h"
#include "Kismet/KismetSystemLibrary.h"
#include "MatterFluxLog.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "Save/MatterFluxSaveSubsystem.h"
#include "Save/MatterFluxSaveGame.h"

void UMatterFluxShellWidget::InitializeForPlayer(
	AMatterFluxPlayerController* Controller)
{
	OwnerController = Controller;
	bHostRestorePending = IsMultiplayerEntryEnabled()
		&& GetWorld()
		&& GetWorld()->URL.HasOption(TEXT("MatterFluxHostSlot="));
	SetIsFocusable(true);
	RefreshShell();
}

int32 UMatterFluxShellWidget::ResolveOwnedCoinQuantity(
	const AMatterFluxPlayerState* PlayerState)
{
	const UMatterFluxProgressionComponent* Progression = PlayerState
		? PlayerState->GetProgression()
		: nullptr;
	return Progression
		? FMath::Max(0, Progression->GetItemQuantity(TEXT("std.coin")))
		: 0;
}

int32 UMatterFluxShellWidget::GetOwnedCoinQuantity() const
{
	return ResolveOwnedCoinQuantity(OwnerController
		? OwnerController->GetPlayerState<AMatterFluxPlayerState>()
		: nullptr);
}

AMatterFluxPlayerController* UMatterFluxShellWidget::GetMatterFluxController() const
{
	return OwnerController;
}

UMatterFluxSaveSubsystem* UMatterFluxShellWidget::GetSaveSubsystem() const
{
	return GetGameInstance()
		? GetGameInstance()->GetSubsystem<UMatterFluxSaveSubsystem>()
		: nullptr;
}

void UMatterFluxShellWidget::ShowStartMenu()
{
	SubmenuReturnView = EMatterFluxShellView::StartMenu;
	SetView(EMatterFluxShellView::StartMenu);
}

void UMatterFluxShellWidget::ShowSinglePlayerMenu()
{
	CancelPendingJoinForSinglePlayer();
	SetView(EMatterFluxShellView::SinglePlayerMenu);
}

void UMatterFluxShellWidget::CancelPendingJoinForSinglePlayer()
{
	UWorld* World = GetWorld();
	if (GEngine && World && GEngine->PendingNetGameFromWorld(World))
	{
		GEngine->CancelPending(World);
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("MatterFlux single-player flow cancelled a pending multiplayer join."));
	}
}

void UMatterFluxShellWidget::ShowMultiplayerMenu()
{
	if (!IsMultiplayerEntryEnabled())
	{
		RejectMultiplayerEntry();
		return;
	}
	SetView(EMatterFluxShellView::MultiplayerMenu);
}

void UMatterFluxShellWidget::ShowCreateRoomMenu()
{
	if (!IsMultiplayerEntryEnabled())
	{
		RejectMultiplayerEntry();
		return;
	}
	SelectedHostSlotIndex = INDEX_NONE;
	SetView(EMatterFluxShellView::CreateRoomMenu);
}

void UMatterFluxShellWidget::ShowJoinRoomMenu()
{
	if (!IsMultiplayerEntryEnabled())
	{
		RejectMultiplayerEntry();
		return;
	}
	SetView(EMatterFluxShellView::JoinRoomMenu);
}

void UMatterFluxShellWidget::RejectMultiplayerEntry()
{
	const EMatterFluxShellView FallbackView = bFrontEndContext
		? EMatterFluxShellView::StartMenu
		: EMatterFluxShellView::PauseMenu;
	SetView(FallbackView);
	TransientNotice = TEXT("联机模式暂未开放");
	RefreshShell();
}

bool UMatterFluxShellWidget::IsFrontEndView() const
{
	return View == EMatterFluxShellView::StartMenu
		|| View == EMatterFluxShellView::SinglePlayerMenu
		|| View == EMatterFluxShellView::MultiplayerMenu
		|| View == EMatterFluxShellView::CreateRoomMenu
		|| View == EMatterFluxShellView::JoinRoomMenu;
}

void UMatterFluxShellWidget::ShowPauseMenu()
{
	SubmenuReturnView = EMatterFluxShellView::PauseMenu;
	SetView(EMatterFluxShellView::PauseMenu);
}

void UMatterFluxShellWidget::ShowSettings()
{
	if (View == EMatterFluxShellView::Settings)
	{
		return;
	}
	if (IsFrontEndView())
	{
		SubmenuReturnView = View;
	}
	else if (!IsStartMenuOpen())
	{
		SubmenuReturnView = EMatterFluxShellView::PauseMenu;
	}
	SetView(EMatterFluxShellView::Settings);
}

void UMatterFluxShellWidget::ShowSaveSlots()
{
	if (View == EMatterFluxShellView::SaveSlots)
	{
		return;
	}
	if (IsFrontEndView())
	{
		SubmenuReturnView = View;
	}
	else if (!IsStartMenuOpen())
	{
		SubmenuReturnView = EMatterFluxShellView::PauseMenu;
	}
	SetView(EMatterFluxShellView::SaveSlots);
}

void UMatterFluxShellWidget::ShowLoadSlots()
{
	if (View == EMatterFluxShellView::LoadSlots)
	{
		return;
	}
	if (IsFrontEndView())
	{
		SubmenuReturnView = View;
	}
	else if (!IsStartMenuOpen())
	{
		SubmenuReturnView = EMatterFluxShellView::PauseMenu;
	}
	SetView(EMatterFluxShellView::LoadSlots);
}

void UMatterFluxShellWidget::CloseMenus()
{
	if (IsStartMenuOpen())
	{
		return;
	}
	SetView(EMatterFluxShellView::Gameplay);
}

void UMatterFluxShellWidget::EnterGameplayAfterSuccessfulOperation()
{
	bFrontEndContext = false;
	SetView(EMatterFluxShellView::Gameplay);
}

void UMatterFluxShellWidget::SetView(const EMatterFluxShellView NewView)
{
	switch (NewView)
	{
	case EMatterFluxShellView::StartMenu:
	case EMatterFluxShellView::SinglePlayerMenu:
	case EMatterFluxShellView::MultiplayerMenu:
	case EMatterFluxShellView::CreateRoomMenu:
	case EMatterFluxShellView::JoinRoomMenu:
		bFrontEndContext = true;
		break;
	case EMatterFluxShellView::Gameplay:
	case EMatterFluxShellView::PauseMenu:
		bFrontEndContext = false;
		break;
	default:
		break;
	}
	View = NewView;
	RenamingSlotIndex = INDEX_NONE;
	RenameDraft.Reset();
	TransientNotice.Reset();
	RefreshShell();
	NotifyControllerState(false);
}

void UMatterFluxShellWidget::ReturnFromSubmenu()
{
	SetView(SubmenuReturnView);
}

void UMatterFluxShellWidget::RequestNewGame()
{
	// A join request may still be connecting even after the player navigates
	// back to this panel. Shared-world operations deliberately reject clients,
	// so restore the existing local world before starting generation.
	CancelPendingJoinForSinglePlayer();
	if (UMatterFluxSaveSubsystem* Save = GetSaveSubsystem())
	{
		bCloseAfterSuccessfulOperation = true;
		const bool bStarted = Save->RequestNewGame(OwnerController);
		if (!bStarted
			&& Save->GetOperation() == EMatterFluxSaveOperation::Failed)
		{
			TransientNotice = Save->GetLastResultMessage();
			Save->AcknowledgeResult();
			bCloseAfterSuccessfulOperation = false;
		}
		LastObservedOperation = Save->GetOperation();
		RefreshShell();
		NotifyControllerState(Save->IsBusy());
	}
}

void UMatterFluxShellWidget::RequestStoryMode()
{
	CancelPendingJoinForSinglePlayer();
	if (UMatterFluxSaveSubsystem* Save = GetSaveSubsystem())
	{
		bCloseAfterSuccessfulOperation = true;
		const bool bStarted = Save->RequestStoryGame(
			OwnerController,
			GetStoryMapId(),
			AMatterFluxPlayableWorldActor::PaperMagicStorySeed);
		if (!bStarted
			&& Save->GetOperation() == EMatterFluxSaveOperation::Failed)
		{
			TransientNotice = Save->GetLastResultMessage();
			Save->AcknowledgeResult();
			bCloseAfterSuccessfulOperation = false;
		}
		LastObservedOperation = Save->GetOperation();
		RefreshShell();
		NotifyControllerState(Save->IsBusy());
	}
}

void UMatterFluxShellWidget::RequestContinue()
{
	if (UMatterFluxSaveSubsystem* Save = GetSaveSubsystem())
	{
		const int32 SlotIndex = Save->GetMostRecentSlotIndex();
		if (SlotIndex != INDEX_NONE)
		{
			RequestSlotOperation(SlotIndex, true);
		}
	}
}

void UMatterFluxShellWidget::RequestHostRoom(const int32 SaveSlotIndex)
{
	if (!IsMultiplayerEntryEnabled())
	{
		RejectMultiplayerEntry();
		return;
	}
	TransientNotice.Reset();
	if (!OwnerController)
	{
		TransientNotice = TEXT("找不到本地玩家控制器");
		RefreshShell();
		return;
	}
	FString Error;
	if (!OwnerController->HostListenRoom(SaveSlotIndex, Error))
	{
		TransientNotice = Error.IsEmpty() ? TEXT("无法创建房间") : Error;
		RefreshShell();
		return;
	}
	TransientNotice = TEXT("正在创建房间……");
	RefreshShell();
}

void UMatterFluxShellWidget::SelectHostSlot(const int32 SlotIndex)
{
	const UMatterFluxSaveSubsystem* Save = GetSaveSubsystem();
	SelectedHostSlotIndex = Save && Save->FindSlot(SlotIndex)
		? SlotIndex
		: INDEX_NONE;
	TransientNotice.Reset();
	RefreshShell();
}

void UMatterFluxShellWidget::RequestJoinRoom()
{
	if (!IsMultiplayerEntryEnabled())
	{
		RejectMultiplayerEntry();
		return;
	}
	TransientNotice.Reset();
	if (!OwnerController)
	{
		TransientNotice = TEXT("找不到本地玩家控制器");
		RefreshShell();
		return;
	}
	FString NormalizedAddress;
	FString Error;
	if (!OwnerController->JoinRoomByAddress(
		JoinAddress, NormalizedAddress, Error))
	{
		TransientNotice = Error.IsEmpty() ? TEXT("无法加入房间") : Error;
		RefreshShell();
		return;
	}
	JoinAddress = MoveTemp(NormalizedAddress);
	TransientNotice = TEXT("正在连接房间……");
	RefreshShell();
}

void UMatterFluxShellWidget::RequestSlotOperation(
	const int32 SlotIndex,
	const bool bLoad)
{
	if (UMatterFluxSaveSubsystem* Save = GetSaveSubsystem())
	{
		bCloseAfterSuccessfulOperation = bLoad;
		bool bStarted = false;
		if (bLoad)
		{
			bStarted = Save->RequestLoad(OwnerController, SlotIndex);
		}
		else
		{
			bStarted = Save->RequestSave(OwnerController, SlotIndex);
		}
		if (!bStarted
			&& Save->GetOperation() == EMatterFluxSaveOperation::Failed)
		{
			TransientNotice = Save->GetLastResultMessage();
			Save->AcknowledgeResult();
			bCloseAfterSuccessfulOperation = false;
		}
		LastObservedOperation = Save->GetOperation();
		RefreshShell();
		NotifyControllerState(Save->IsBusy());
	}
}

void UMatterFluxShellWidget::RequestDuplicateSlot(const int32 SlotIndex)
{
	if (UMatterFluxSaveSubsystem* Save = GetSaveSubsystem())
	{
		RenamingSlotIndex = INDEX_NONE;
		RenameDraft.Reset();
		bCloseAfterSuccessfulOperation = false;
		const bool bStarted = Save->RequestDuplicate(SlotIndex);
		if (!bStarted
			&& Save->GetOperation() == EMatterFluxSaveOperation::Failed)
		{
			TransientNotice = Save->GetLastResultMessage();
			Save->AcknowledgeResult();
		}
		else if (!bStarted)
		{
			TransientNotice = TEXT("存档系统正忙");
		}
		LastObservedOperation = Save->GetOperation();
		RefreshShell();
		NotifyControllerState(Save->IsBusy());
	}
}

void UMatterFluxShellWidget::RequestDeleteSlot(const int32 SlotIndex)
{
	if (UMatterFluxSaveSubsystem* Save = GetSaveSubsystem())
	{
		if (SelectedHostSlotIndex == SlotIndex)
		{
			SelectedHostSlotIndex = INDEX_NONE;
		}
		if (RenamingSlotIndex == SlotIndex)
		{
			RenamingSlotIndex = INDEX_NONE;
			RenameDraft.Reset();
		}
		TransientNotice = Save->DeleteSlot(SlotIndex)
			? TEXT("存档已删除") : TEXT("无法删除这个存档");
		RefreshShell();
	}
}

void UMatterFluxShellWidget::BeginRenameSlot(const int32 SlotIndex)
{
	const UMatterFluxSaveSubsystem* Save = GetSaveSubsystem();
	const FMatterFluxSaveSlotInfo* SlotInfo = Save
		? Save->FindSlot(SlotIndex)
		: nullptr;
	if (!SlotInfo)
	{
		TransientNotice = TEXT("找不到这个存档");
		RefreshShell();
		return;
	}
	RenamingSlotIndex = SlotIndex;
	RenameDraft = MatterFluxShellUI::GetSlotDisplayName(*SlotInfo);
	TransientNotice.Reset();
	RefreshShell();
}

void UMatterFluxShellWidget::CommitRenameSlot()
{
	if (RenamingSlotIndex == INDEX_NONE)
	{
		return;
	}
	FString Error;
	const bool bRenamed = GetSaveSubsystem()
		&& GetSaveSubsystem()->RenameSlot(
			RenamingSlotIndex, RenameDraft, Error);
	RenamingSlotIndex = INDEX_NONE;
	RenameDraft.Reset();
	TransientNotice = bRenamed
		? TEXT("存档已重命名")
		: (Error.IsEmpty() ? TEXT("无法重命名这个存档") : Error);
	RefreshShell();
}

void UMatterFluxShellWidget::CancelRenameSlot()
{
	RenamingSlotIndex = INDEX_NONE;
	RenameDraft.Reset();
	TransientNotice.Reset();
	RefreshShell();
}

void UMatterFluxShellWidget::RequestQuit()
{
	UKismetSystemLibrary::QuitGame(
		this,
		OwnerController,
		EQuitPreference::Quit,
		false);
}

void UMatterFluxShellWidget::RequestMagicWorkbench()
{
	if (OwnerController)
	{
		CloseMenus();
		OwnerController->ToggleMagicWorkbench();
	}
}

void UMatterFluxShellWidget::NativeConstruct()
{
	Super::NativeConstruct();
	BindProgression();
	RefreshShell();
}

void UMatterFluxShellWidget::NativeDestruct()
{
	UnbindProgression();
	Super::NativeDestruct();
}

void UMatterFluxShellWidget::NativeTick(
	const FGeometry& MyGeometry,
	const float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	UMatterFluxSaveSubsystem* Save = GetSaveSubsystem();
	if (!Save)
	{
		return;
	}
	const EMatterFluxSaveOperation Current = Save->GetOperation();
	if ((Current == EMatterFluxSaveOperation::Complete
		|| Current == EMatterFluxSaveOperation::Failed)
		&& Current != LastObservedOperation)
	{
		const bool bSucceeded = Current == EMatterFluxSaveOperation::Complete;
		TransientNotice = Save->GetLastResultMessage();
		Save->AcknowledgeResult();
		LastObservedOperation = Save->GetOperation();
		if (bHostRestorePending)
		{
			bHostRestorePending = false;
			if (!bSucceeded)
			{
				View = EMatterFluxShellView::CreateRoomMenu;
				RefreshShell();
				NotifyControllerState(false);
				return;
			}
		}
		if (bSucceeded && bCloseAfterSuccessfulOperation)
		{
			bCloseAfterSuccessfulOperation = false;
			EnterGameplayAfterSuccessfulOperation();
		}
		else
		{
			bCloseAfterSuccessfulOperation = false;
			RefreshShell();
			NotifyControllerState(false);
		}
	}
	else
	{
		LastObservedOperation = Current;
	}
}

FReply UMatterFluxShellWidget::NativeOnKeyDown(
	const FGeometry& InGeometry,
	const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (!IsStartMenuOpen())
		{
			IsMenuOpen() ? CloseMenus() : ShowPauseMenu();
		}
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

TSharedRef<SWidget> UMatterFluxShellWidget::RebuildWidget()
{
	Shell = MatterFluxShellUI::CreateShell(this);
	return Shell.ToSharedRef();
}

void UMatterFluxShellWidget::ReleaseSlateResources(
	const bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	Shell.Reset();
}

void UMatterFluxShellWidget::RefreshShell()
{
	BindProgression();
	MatterFluxShellUI::RefreshShell(Shell);
}

UMatterFluxProgressionComponent*
UMatterFluxShellWidget::ResolveProgression() const
{
	const AMatterFluxPlayerState* PlayerState = OwnerController
		? OwnerController->GetPlayerState<AMatterFluxPlayerState>()
		: nullptr;
	return PlayerState ? PlayerState->GetProgression() : nullptr;
}

void UMatterFluxShellWidget::BindProgression()
{
	UMatterFluxProgressionComponent* Progression = ResolveProgression();
	if (BoundProgression.Get() == Progression)
	{
		return;
	}
	UnbindProgression();
	BoundProgression = Progression;
	if (Progression)
	{
		ProgressionChangedHandle =
			Progression->OnProgressionChanged().AddUObject(
				this,
				&UMatterFluxShellWidget::RefreshShell);
	}
}

void UMatterFluxShellWidget::UnbindProgression()
{
	if (BoundProgression.IsValid() && ProgressionChangedHandle.IsValid())
	{
		BoundProgression->OnProgressionChanged().Remove(
			ProgressionChangedHandle);
	}
	ProgressionChangedHandle.Reset();
	BoundProgression.Reset();
}

void UMatterFluxShellWidget::NotifyControllerState(
	const bool bOperationActive) const
{
	if (OwnerController)
	{
		OwnerController->HandleShellStateChanged(
			IsMenuOpen(),
			bOperationActive);
	}
}
