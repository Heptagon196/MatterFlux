#include "Game/MatterFluxPlayerController.h"

#include "AbilitySystemComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Game/MatterFluxPlayerState.h"
#include "GAS/GA_FragmentDebugDamage.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "MatterFluxGameplayTags.h"
#include "MatterFluxLog.h"
#include "Magic/MatterFluxMagicWorkbenchWidget.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Settings/MatterFluxGameUserSettings.h"
#include "UI/MatterFluxShellWidget.h"
#include "UI/MatterFluxInteractionWidget.h"
#include "Progression/MatterFluxQuestTrackerWidget.h"
#include "Save/MatterFluxSaveSubsystem.h"

AMatterFluxPlayerController::AMatterFluxPlayerController()
{
	DebugMappingContext = TSoftObjectPtr<UInputMappingContext>(FSoftObjectPath(TEXT("/Game/MatterFlux/Input/IMC_MatterFluxDebug.IMC_MatterFluxDebug")));
	DebugDamageAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/MatterFlux/Input/IA_FragmentDebugDamage.IA_FragmentDebugDamage")));
}

void AMatterFluxPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (UMatterFluxGameUserSettings* Settings =
		UMatterFluxGameUserSettings::Get())
	{
		Settings->ApplyNonResolutionSettings();
	}
	CreateShell();
	CreateMagicWorkbench();
	CreateQuestTracker();
	CreateInteractionWidget();
	if (ShellWidget
		&& HasAuthority()
		&& GetNetMode() != NM_DedicatedServer
		&& !FApp::IsUnattended()
		&& !FParse::Param(
			FCommandLine::Get(),
			TEXT("MatterFluxSkipStartMenu"))
		&& (!GetWorld()
			|| !GetWorld()->URL.HasOption(TEXT("MatterFluxStarted"))))
	{
		ShellWidget->ShowStartMenu();
	}
	if (HasAuthority() && GetWorld())
	{
		if (const TCHAR* HostSlotOption = GetWorld()->URL.GetOption(
			TEXT("MatterFluxHostSlot="), nullptr))
		{
			const int32 HostSlotIndex = FCString::Atoi(HostSlotOption);
			GetWorld()->GetTimerManager().SetTimerForNextTick(
				FTimerDelegate::CreateWeakLambda(this, [this, HostSlotIndex]()
				{
					UMatterFluxSaveSubsystem* Save = GetGameInstance()
						? GetGameInstance()->GetSubsystem<UMatterFluxSaveSubsystem>()
						: nullptr;
					if (!Save || !Save->RequestLoad(this, HostSlotIndex))
					{
						UE_LOG(LogMatterFlux, Error,
							TEXT("Hosted save slot %d could not be restored"),
							HostSlotIndex);
					}
				}));
		}
	}

	GrantDebugAbilityIfEnabled();
	if (bEnableDebugControls && IsLocalController())
	{
		SetInputMode(FInputModeGameOnly());
		AddDebugMappingContext();
	}
}

void AMatterFluxPlayerController::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (ShellWidget)
	{
		ShellWidget->RemoveFromParent();
		ShellWidget = nullptr;
	}
	if (MagicWorkbench)
	{
		MagicWorkbench->RemoveFromParent();
		MagicWorkbench = nullptr;
	}
	if (QuestTracker)
	{
		QuestTracker->RemoveFromParent();
		QuestTracker = nullptr;
	}
	if (InteractionWidget)
	{
		InteractionWidget->RemoveFromParent();
		InteractionWidget = nullptr;
	}
	if (AppliedDebugMappingContext)
	{
		if (ULocalPlayer* LocalPlayer = DebugInputLocalPlayer.Get())
		{
			if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
				ULocalPlayer::GetSubsystem<
					UEnhancedInputLocalPlayerSubsystem>(
					LocalPlayer))
			{
				InputSubsystem->RemoveMappingContext(
					AppliedDebugMappingContext);
			}
		}
		AppliedDebugMappingContext = nullptr;
	}
	DebugInputLocalPlayer.Reset();
	Super::EndPlay(EndPlayReason);
}

void AMatterFluxPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	GrantDebugAbilityIfEnabled();
	if (MagicWorkbench)
	{
		MagicWorkbench->RefreshWorkbench();
	}
	if (QuestTracker)
	{
		QuestTracker->RefreshTracker();
	}
}

void AMatterFluxPlayerController::GrantDebugAbilityIfEnabled()
{
	if (!bEnableDebugControls || !HasAuthority())
	{
		return;
	}
	AMatterFluxPlayerState* MatterFluxPlayerState =
		GetPlayerState<AMatterFluxPlayerState>();
	UAbilitySystemComponent* ASC = MatterFluxPlayerState
		? MatterFluxPlayerState->GetAbilitySystemComponent()
		: nullptr;
	if (ASC
		&& !ASC->FindAbilitySpecFromClass(
			UGA_FragmentDebugDamage::StaticClass()))
	{
		ASC->GiveAbility(FGameplayAbilitySpec(
			UGA_FragmentDebugDamage::StaticClass(),
			1,
			INDEX_NONE,
			this));
	}
}

void AMatterFluxPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	InputComponent->BindKey(
		EKeys::I,
		IE_Pressed,
		this,
		&AMatterFluxPlayerController::ToggleMagicWorkbench);
	InputComponent->BindKey(
		EKeys::Tab,
		IE_Pressed,
		this,
		&AMatterFluxPlayerController::ToggleMagicWorkbench);
	InputComponent->BindKey(
		EKeys::J,
		IE_Pressed,
		this,
		&AMatterFluxPlayerController::ToggleQuestJournal);
	InputComponent->BindKey(
		EKeys::E,
		IE_Pressed,
		this,
		&AMatterFluxPlayerController::TryInteract);
	InputComponent->BindKey(
		EKeys::Escape,
		IE_Pressed,
		this,
		&AMatterFluxPlayerController::TogglePauseMenu);

	if (!bEnableDebugControls)
	{
		return;
	}

	UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EnhancedInputComponent)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("%s has no EnhancedInputComponent; debug GAS input will not bind."), *GetName());
		return;
	}

	UInputAction* InputAction = GetOrCreateDebugDamageAction();
	if (!InputAction)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Debug damage input action is missing: %s"), *DebugDamageAction.ToString());
		return;
	}

	EnhancedInputComponent->BindAction(InputAction, ETriggerEvent::Started, this, &AMatterFluxPlayerController::HandleDebugDamageInput);
}

void AMatterFluxPlayerController::CreateMagicWorkbench()
{
	if (!IsLocalController() || MagicWorkbench)
	{
		return;
	}

	MagicWorkbench = CreateWidget<UMatterFluxMagicWorkbenchWidget>(
		this,
		UMatterFluxMagicWorkbenchWidget::StaticClass());
	if (!MagicWorkbench)
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Failed to create the magic workbench widget."));
		return;
	}

	MagicWorkbench->InitializeForPlayer(this);
	MagicWorkbench->AddToPlayerScreen(20);
	MagicWorkbench->SetVisibility(ESlateVisibility::Collapsed);
}

void AMatterFluxPlayerController::CreateShell()
{
	if (!IsLocalController() || ShellWidget)
	{
		return;
	}
	ShellWidget = CreateWidget<UMatterFluxShellWidget>(
		this,
		UMatterFluxShellWidget::StaticClass());
	if (!ShellWidget)
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Failed to create the game shell widget."));
		return;
	}
	ShellWidget->InitializeForPlayer(this);
	ShellWidget->AddToPlayerScreen(10);
}

void AMatterFluxPlayerController::CreateQuestTracker()
{
	if (!IsLocalController() || QuestTracker)
	{
		return;
	}
	QuestTracker = CreateWidget<UMatterFluxQuestTrackerWidget>(
		this, UMatterFluxQuestTrackerWidget::StaticClass());
	if (!QuestTracker)
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Failed to create quest tracker widget."));
		return;
	}
	QuestTracker->InitializeForPlayer(this);
	QuestTracker->AddToPlayerScreen(5);
}

void AMatterFluxPlayerController::CreateInteractionWidget()
{
	if (!IsLocalController() || InteractionWidget) return;
	InteractionWidget = CreateWidget<UMatterFluxInteractionWidget>(
		this, UMatterFluxInteractionWidget::StaticClass());
	if (!InteractionWidget)
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Failed to create NPC interaction widget."));
		return;
	}
	InteractionWidget->InitializeForPlayer(this);
	InteractionWidget->AddToPlayerScreen(30);
	InteractionWidget->SetVisibility(ESlateVisibility::Collapsed);
}

void AMatterFluxPlayerController::TryInteract()
{
	if (!IsLocalController() || IsShellMenuOpen() || IsMagicWorkbenchOpen()
		|| (InteractionWidget && InteractionWidget->IsInteractionOpen()))
	{
		return;
	}
	APawn* Interactor = GetPawn();
	if (!Interactor || !GetWorld()) return;
	AMatterFluxCreatureActor* Nearest = nullptr;
	float NearestDistanceSquared = FMath::Square(360.0f);
	for (TActorIterator<AMatterFluxCreatureActor> It(GetWorld()); It; ++It)
	{
		AMatterFluxCreatureActor* Candidate = *It;
		if (!Candidate || !Candidate->CanInteract(*Interactor)) continue;
		const float DistanceSquared = FVector::DistSquared(
			Candidate->GetActorLocation(), Interactor->GetActorLocation());
		if (DistanceSquared <= NearestDistanceSquared)
		{
			Nearest = Candidate;
			NearestDistanceSquared = DistanceSquared;
		}
	}
	if (Nearest) ServerInteract(Nearest);
}

bool AMatterFluxPlayerController::ServerInteract_Validate(
	AMatterFluxCreatureActor* Creature)
{
	return Creature != nullptr;
}

void AMatterFluxPlayerController::ServerInteract_Implementation(
	AMatterFluxCreatureActor* Creature)
{
	APawn* Interactor = GetPawn();
	if (!Creature || !Interactor || !Creature->CanInteract(*Interactor)) return;
	const FMatterFluxCreatureDefinition* Definition = Creature->ResolveDefinition();
	if (Definition) ClientOpenCreatureInteraction(Creature, Definition->DialogueId);
}

void AMatterFluxPlayerController::ClientOpenCreatureInteraction_Implementation(
	AMatterFluxCreatureActor* Creature,
	const FName DialogueId)
{
	if (!InteractionWidget) CreateInteractionWidget();
	if (!InteractionWidget || !Creature) return;
	if (ShellWidget && ShellWidget->IsMenuOpen()) ShellWidget->CloseMenus();
	if (IsMagicWorkbenchOpen()) CloseMagicWorkbench();
	InteractionWidget->OpenInteraction(Creature, DialogueId);
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(InteractionWidget->TakeWidget());
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	bShowMouseCursor = true;
}

void AMatterFluxPlayerController::RequestCreaturePurchase(
	AMatterFluxCreatureActor* Creature,
	const int32 OfferIndex,
	const int32 ExpectedProgressionRevision)
{
	if (Creature)
	{
		ServerPurchaseCreatureOffer(
			Creature, OfferIndex, ExpectedProgressionRevision);
	}
}

bool AMatterFluxPlayerController::ServerPurchaseCreatureOffer_Validate(
	AMatterFluxCreatureActor* Creature,
	const int32 OfferIndex,
	const int32 ExpectedProgressionRevision)
{
	return Creature != nullptr && OfferIndex >= 0 && OfferIndex < 1024
		&& ExpectedProgressionRevision >= 0;
}

void AMatterFluxPlayerController::ServerPurchaseCreatureOffer_Implementation(
	AMatterFluxCreatureActor* Creature,
	const int32 OfferIndex,
	const int32 ExpectedProgressionRevision)
{
	APawn* Interactor = GetPawn();
	AMatterFluxPlayerState* Buyer = GetPlayerState<AMatterFluxPlayerState>();
	FString Error;
	int32 Remaining = INDEX_NONE;
	const bool bSuccess = Creature && Interactor && Buyer
		&& Creature->CanInteract(*Interactor)
		&& Creature->PurchaseOfferAuthority(
			*Buyer, OfferIndex, ExpectedProgressionRevision, Remaining, Error);
	if (!bSuccess && Error.IsEmpty())
	{
		Error = TEXT("player is no longer close enough to the merchant");
	}
	ClientCreaturePurchaseResult(bSuccess, OfferIndex, Remaining, Error);
}

void AMatterFluxPlayerController::ClientCreaturePurchaseResult_Implementation(
	const bool bSuccess,
	const int32 OfferIndex,
	const int32 RemainingPurchases,
	const FString& Message)
{
	if (InteractionWidget)
	{
		InteractionWidget->HandlePurchaseResult(
			bSuccess, OfferIndex, RemainingPurchases, Message);
	}
}

void AMatterFluxPlayerController::CloseCreatureInteraction()
{
	if (InteractionWidget)
	{
		InteractionWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
	SetInputMode(FInputModeGameOnly());
	bShowMouseCursor = false;
}

bool AMatterFluxPlayerController::IsMagicWorkbenchOpen() const
{
	return MagicWorkbench
		&& MagicWorkbench->GetVisibility() != ESlateVisibility::Collapsed;
}

void AMatterFluxPlayerController::ShowMagicWorkbenchPage(
	const bool bShowWandBackpack)
{
	if (!MagicWorkbench)
	{
		CreateMagicWorkbench();
	}
	if (!MagicWorkbench)
	{
		return;
	}
	if (bShowWandBackpack)
	{
		MagicWorkbench->ShowWandBackpack();
	}
	else
	{
		MagicWorkbench->ShowSpellEditor();
	}
}

bool AMatterFluxPlayerController::ShowMagicWorkbenchNamedPage(
	const FString& PageName)
{
	if (!MagicWorkbench) CreateMagicWorkbench();
	if (!MagicWorkbench) return false;
	if (PageName.Equals(TEXT("spell"), ESearchCase::IgnoreCase))
	{
		MagicWorkbench->ShowSpellEditor();
	}
	else if (PageName.Equals(TEXT("wand"), ESearchCase::IgnoreCase))
	{
		MagicWorkbench->ShowWandBackpack();
	}
	else if (PageName.Equals(TEXT("item"), ESearchCase::IgnoreCase))
	{
		MagicWorkbench->ShowItemBackpack();
	}
	else if (PageName.Equals(TEXT("quest"), ESearchCase::IgnoreCase))
	{
		MagicWorkbench->ShowQuestJournal();
	}
	else if (PageName.Equals(TEXT("settings"), ESearchCase::IgnoreCase))
	{
		MagicWorkbench->ShowSettingsPage();
	}
	else
	{
		return false;
	}
	return true;
}

bool AMatterFluxPlayerController::SelectMagicWorkbenchEquipmentSlot(
	const int32 EquipmentSlot)
{
	if (!MagicWorkbench)
	{
		CreateMagicWorkbench();
	}
	UMatterFluxMagicInventoryComponent* Inventory = MagicWorkbench
		? MagicWorkbench->ResolveInventory()
		: nullptr;
	if (!Inventory)
	{
		return false;
	}
	const FGuid WandId = Inventory->GetEquippedWandId(EquipmentSlot);
	if (!WandId.IsValid())
	{
		return false;
	}
	MagicWorkbench->SelectWand(WandId);
	return MagicWorkbench->GetSelectedWandId() == WandId;
}

void AMatterFluxPlayerController::ToggleMagicWorkbench()
{
	if (InteractionWidget && InteractionWidget->IsInteractionOpen())
	{
		CloseCreatureInteraction();
	}
	if (!MagicWorkbench)
	{
		CreateMagicWorkbench();
	}
	if (!MagicWorkbench)
	{
		return;
	}

	if (IsMagicWorkbenchOpen())
	{
		CloseMagicWorkbench();
		return;
	}
	if (ShellWidget && ShellWidget->IsMenuOpen())
	{
		ShellWidget->CloseMenus();
	}

	MagicWorkbench->RefreshWorkbench();
	MagicWorkbench->SetVisibility(ESlateVisibility::Visible);
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(MagicWorkbench->TakeWidget());
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	bShowMouseCursor = true;
	if (GEngine)
	{
		if (APawn* ControlledPawn = GetPawn())
		{
			GEngine->RemoveOnScreenDebugMessage(
				reinterpret_cast<uint64>(ControlledPawn));
		}
	}
}

void AMatterFluxPlayerController::ToggleQuestJournal()
{
	if (!MagicWorkbench)
	{
		CreateMagicWorkbench();
	}
	if (!MagicWorkbench)
	{
		return;
	}
	if (IsMagicWorkbenchOpen()
		&& MagicWorkbench->GetPage()
			== EMatterFluxWorkbenchPage::QuestJournal)
	{
		CloseMagicWorkbench();
		return;
	}
	MagicWorkbench->ShowQuestJournal();
	if (!IsMagicWorkbenchOpen())
	{
		ToggleMagicWorkbench();
	}
}

void AMatterFluxPlayerController::CloseMagicWorkbench()
{
	if (MagicWorkbench)
	{
		MagicWorkbench->SetVisibility(ESlateVisibility::Collapsed);
	}
	SetInputMode(FInputModeGameOnly());
	bShowMouseCursor = false;
}

bool AMatterFluxPlayerController::IsShellMenuOpen() const
{
	return ShellWidget && ShellWidget->IsMenuOpen();
}

void AMatterFluxPlayerController::TogglePauseMenu()
{
	if (InteractionWidget && InteractionWidget->IsInteractionOpen())
	{
		CloseCreatureInteraction();
		return;
	}
	if (!ShellWidget)
	{
		CreateShell();
	}
	if (!ShellWidget || ShellWidget->IsStartMenuOpen())
	{
		return;
	}
	if (IsMagicWorkbenchOpen())
	{
		CloseMagicWorkbench();
	}
	IsShellMenuOpen()
		? ShellWidget->CloseMenus()
		: ShellWidget->ShowPauseMenu();
}

void AMatterFluxPlayerController::ShowStartMenu()
{
	if (!ShellWidget) CreateShell();
	if (ShellWidget) ShellWidget->ShowStartMenu();
}

void AMatterFluxPlayerController::ShowSinglePlayerMenu()
{
	if (!ShellWidget) CreateShell();
	if (ShellWidget) ShellWidget->ShowSinglePlayerMenu();
}

void AMatterFluxPlayerController::ShowMultiplayerMenu()
{
	if (!ShellWidget) CreateShell();
	if (ShellWidget) ShellWidget->ShowMultiplayerMenu();
}

void AMatterFluxPlayerController::ShowCreateRoomMenu()
{
	if (!ShellWidget) CreateShell();
	if (ShellWidget) ShellWidget->ShowCreateRoomMenu();
}

void AMatterFluxPlayerController::ShowJoinRoomMenu()
{
	if (!ShellWidget) CreateShell();
	if (ShellWidget) ShellWidget->ShowJoinRoomMenu();
}

bool AMatterFluxPlayerController::HostListenRoom(
	const int32 SaveSlotIndex,
	FString& OutError)
{
	OutError.Reset();
	if (!IsLocalController())
	{
		OutError = TEXT("只有本地玩家可以创建房间");
		return false;
	}
	if (!HasAuthority() || GetNetMode() == NM_Client)
	{
		OutError = TEXT("客户端不能创建房间；请先返回本地开始界面");
		return false;
	}
	if (SaveSlotIndex != INDEX_NONE)
	{
		const UMatterFluxSaveSubsystem* Save = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UMatterFluxSaveSubsystem>()
			: nullptr;
		if (!Save || !Save->FindSlot(SaveSlotIndex))
		{
			OutError = TEXT("选择的存档不存在");
			return false;
		}
	}
	const FString Options = SaveSlotIndex == INDEX_NONE
		? TEXT("listen?MatterFluxStarted=1")
		: FString::Printf(
			TEXT("listen?MatterFluxStarted=1?MatterFluxHostSlot=%d"),
			SaveSlotIndex);
	UGameplayStatics::OpenLevel(
		this,
		FName(TEXT("/Game/Default")),
		true,
		Options);
	return true;
}

bool AMatterFluxPlayerController::JoinRoomByAddress(
	const FString& Address,
	FString& OutNormalizedAddress,
	FString& OutError)
{
	if (!IsLocalController())
	{
		OutNormalizedAddress.Reset();
		OutError = TEXT("只有本地玩家可以加入房间");
		return false;
	}
	if (!NormalizeJoinAddress(
		Address, OutNormalizedAddress, OutError))
	{
		return false;
	}
	ClientTravel(OutNormalizedAddress, TRAVEL_Absolute);
	return true;
}

bool AMatterFluxPlayerController::NormalizeJoinAddress(
	const FString& Address,
	FString& OutNormalizedAddress,
	FString& OutError)
{
	OutNormalizedAddress.Reset();
	OutError.Reset();
	const FString Candidate = Address.TrimStartAndEnd();
	if (Candidate.IsEmpty())
	{
		OutError = TEXT("请输入房主地址");
		return false;
	}
	for (const TCHAR Character : Candidate)
	{
		if (FChar::IsWhitespace(Character)
			|| Character == TEXT('?') || Character == TEXT('#')
			|| Character == TEXT('/') || Character == TEXT('\\'))
		{
			OutError = TEXT("地址包含不允许的字符");
			return false;
		}
	}

	FString Host;
	FString PortText;
	if (Candidate.StartsWith(TEXT("[")))
	{
		const int32 ClosingBracket = Candidate.Find(TEXT("]"));
		if (ClosingBracket <= 1)
		{
			OutError = TEXT("IPv6 地址需要写成 [地址]:端口");
			return false;
		}
		Host = Candidate.Left(ClosingBracket + 1);
		const FString Remainder = Candidate.Mid(ClosingBracket + 1);
		if (!Remainder.IsEmpty())
		{
			if (!Remainder.StartsWith(TEXT(":")))
			{
				OutError = TEXT("IPv6 地址格式不正确");
				return false;
			}
			PortText = Remainder.Mid(1);
		}
	}
	else
	{
		int32 ColonIndex = INDEX_NONE;
		if (Candidate.FindLastChar(TEXT(':'), ColonIndex))
		{
			Host = Candidate.Left(ColonIndex);
			PortText = Candidate.Mid(ColonIndex + 1);
			if (Host.Contains(TEXT(":")))
			{
				OutError = TEXT("IPv6 地址需要放在方括号中");
				return false;
			}
		}
		else
		{
			Host = Candidate;
		}
	}
	if (Host.IsEmpty())
	{
		OutError = TEXT("房主地址不能为空");
		return false;
	}
	if (PortText.IsEmpty())
	{
		PortText = TEXT("7777");
	}
	if (!PortText.IsNumeric())
	{
		OutError = TEXT("端口必须是数字");
		return false;
	}
	const int32 Port = FCString::Atoi(*PortText);
	if (Port < 1 || Port > 65535)
	{
		OutError = TEXT("端口必须位于 1 到 65535 之间");
		return false;
	}
	OutNormalizedAddress = FString::Printf(
		TEXT("%s:%d"), *Host, Port);
	return true;
}

void AMatterFluxPlayerController::ShowSettingsMenu()
{
	if (!ShellWidget) CreateShell();
	if (ShellWidget) ShellWidget->ShowSettings();
}

void AMatterFluxPlayerController::ShowSaveMenu()
{
	if (!ShellWidget) CreateShell();
	if (ShellWidget) ShellWidget->ShowSaveSlots();
}

void AMatterFluxPlayerController::ShowLoadMenu()
{
	if (!ShellWidget) CreateShell();
	if (ShellWidget) ShellWidget->ShowLoadSlots();
}

void AMatterFluxPlayerController::CloseShellMenu()
{
	if (ShellWidget) ShellWidget->CloseMenus();
}

void AMatterFluxPlayerController::EnterGameplayForVisualCapture()
{
	if (!ShellWidget)
	{
		CreateShell();
	}
	if (ShellWidget)
	{
		ShellWidget->EnterGameplayAfterSuccessfulOperation();
	}
}

void AMatterFluxPlayerController::HideUIForVisualCapture()
{
	if (ShellWidget)
	{
		ShellWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (QuestTracker)
	{
		QuestTracker->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (MagicWorkbench)
	{
		MagicWorkbench->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (InteractionWidget)
	{
		InteractionWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void AMatterFluxPlayerController::HandleShellStateChanged(
	const bool bMenuOpen,
	const bool bOperationActive)
{
	if (!IsLocalController())
	{
		return;
	}
	if (QuestTracker)
	{
		QuestTracker->SetSuppressedByFrontEnd(
			ShellWidget && ShellWidget->IsStartMenuOpen());
	}
	if (GetNetMode() == NM_Standalone)
	{
		SetPause(bMenuOpen && !bOperationActive);
	}
	if (bMenuOpen)
	{
		if (InteractionWidget && InteractionWidget->IsInteractionOpen())
		{
			CloseCreatureInteraction();
		}
		if (GEngine)
		{
			if (APawn* ControlledPawn = GetPawn())
			{
				GEngine->RemoveOnScreenDebugMessage(
					reinterpret_cast<uint64>(ControlledPawn));
			}
		}
		if (IsMagicWorkbenchOpen())
		{
			CloseMagicWorkbench();
		}
		FInputModeUIOnly InputMode;
		if (ShellWidget)
		{
			InputMode.SetWidgetToFocus(ShellWidget->TakeWidget());
		}
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(InputMode);
		bShowMouseCursor = true;
	}
	else
	{
		SetInputMode(FInputModeGameOnly());
		bShowMouseCursor = false;
	}
}

void AMatterFluxPlayerController::AddDebugMappingContext()
{
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LocalPlayer
		? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer)
		: nullptr;
	if (!InputSubsystem)
	{
		return;
	}

	UInputMappingContext* MappingContext = GetOrCreateDebugMappingContext();
	if (!MappingContext)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Debug input mapping context is missing: %s"), *DebugMappingContext.ToString());
		return;
	}

	InputSubsystem->AddMappingContext(MappingContext, DebugMappingPriority);
	AppliedDebugMappingContext = MappingContext;
	DebugInputLocalPlayer = LocalPlayer;
}

UInputAction* AMatterFluxPlayerController::GetOrCreateDebugDamageAction()
{
	if (UInputAction* LoadedAction = DebugDamageAction.LoadSynchronous())
	{
		return LoadedAction;
	}

	if (!RuntimeDebugDamageAction)
	{
		RuntimeDebugDamageAction = NewObject<UInputAction>(this, TEXT("Runtime_IA_FragmentDebugDamage"));
		RuntimeDebugDamageAction->ValueType = EInputActionValueType::Boolean;
	}

	return RuntimeDebugDamageAction;
}

UInputMappingContext* AMatterFluxPlayerController::GetOrCreateDebugMappingContext()
{
	if (UInputMappingContext* LoadedMappingContext = DebugMappingContext.LoadSynchronous())
	{
		UInputAction* InputAction = GetOrCreateDebugDamageAction();
		bool bHasDebugMapping = false;
		for (const FEnhancedActionKeyMapping& Mapping : LoadedMappingContext->GetMappings())
		{
			if (Mapping.Action == InputAction && Mapping.Key == EKeys::F)
			{
				bHasDebugMapping = true;
				break;
			}
		}

		if (bHasDebugMapping || !InputAction)
		{
			return LoadedMappingContext;
		}

		if (!RuntimeDebugMappingContext)
		{
			RuntimeDebugMappingContext = DuplicateObject<UInputMappingContext>(
				LoadedMappingContext, this, TEXT("Runtime_IMC_MatterFluxDebug_Copy"));
			if (RuntimeDebugMappingContext)
			{
				RuntimeDebugMappingContext->MapKey(InputAction, EKeys::F);
			}
		}
		return RuntimeDebugMappingContext;
	}

	if (!RuntimeDebugMappingContext)
	{
		RuntimeDebugMappingContext = NewObject<UInputMappingContext>(this, TEXT("Runtime_IMC_MatterFluxDebug"));
		RuntimeDebugMappingContext->MapKey(GetOrCreateDebugDamageAction(), EKeys::F);
	}

	return RuntimeDebugMappingContext;
}

void AMatterFluxPlayerController::HandleDebugDamageInput()
{
	AMatterFluxPlayerState* MatterFluxPlayerState = GetPlayerState<AMatterFluxPlayerState>();
	UAbilitySystemComponent* ASC = MatterFluxPlayerState ? MatterFluxPlayerState->GetAbilitySystemComponent() : nullptr;
	if (!ASC)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Debug damage input ignored: missing AbilitySystemComponent."));
		return;
	}

	FGameplayTagContainer AbilityTags;
	AbilityTags.AddTag(TAG_Ability_Fragment_DebugDamage);
	const bool bActivated = ASC->TryActivateAbilitiesByTag(AbilityTags, true);
	if (!bActivated)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Debug damage input did not activate Ability.Fragment.DebugDamage."));
	}
}
