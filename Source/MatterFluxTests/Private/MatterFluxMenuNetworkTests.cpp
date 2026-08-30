#include "Misc/AutomationTest.h"

#include "Game/MatterFluxPlayerController.h"
#include "Engine/Engine.h"
#include "Engine/PendingNetGame.h"
#include "Framework/Application/SlateApplication.h"
#include "Progression/MatterFluxQuestTrackerWidget.h"
#include "UI/MatterFluxShellWidget.h"
#include "Tests/AutomationEditorCommon.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
class FVerifyDeferredGameplayFocus final : public IAutomationLatentCommand
{
public:
	FVerifyDeferredGameplayFocus(
		FAutomationTestBase* InTest,
		const TSharedRef<SWidget>& InGameplayFocusTarget,
		const TSharedRef<SWindow>& InTestWindow)
		: Test(InTest)
		, GameplayFocusTarget(InGameplayFocusTarget)
		, TestWindow(InTestWindow)
	{
	}

	virtual bool Update() override
	{
		if (!FSlateApplication::IsInitialized())
		{
			Test->AddError(TEXT("Slate shut down before deferred gameplay focus could be verified"));
			return true;
		}

		FSlateApplication& Slate = FSlateApplication::Get();
		Test->TestEqual(
			TEXT("Gameplay focus survives the remainder of the menu activation frame"),
			Slate.GetUserFocusedWidget(0),
			GameplayFocusTarget);
		Slate.RequestDestroyWindow(TestWindow.ToSharedRef());
		return true;
	}

private:
	FAutomationTestBase* Test = nullptr;
	TSharedPtr<SWidget> GameplayFocusTarget;
	TSharedPtr<SWindow> TestWindow;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGameplayFocusRestorationTest,
	"MatterFlux.Menu.Input.GameplayRestoresViewportKeyboardFocus",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGameplayFocusRestorationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	if (!TestTrue(TEXT("Slate is initialized"),
		FSlateApplication::IsInitialized()))
	{
		return false;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedRef<SButton> MenuFocusTarget = SNew(SButton);
	const TSharedRef<SButton> GameplayFocusTarget = SNew(SButton);
	const TSharedRef<SWindow> TestWindow = SNew(SWindow)
		.Title(FText::FromString(TEXT("MatterFlux Focus Test")))
		.ClientSize(FVector2D(320.0f, 180.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			[
				MenuFocusTarget
			]
			+ SVerticalBox::Slot()
			[
				GameplayFocusTarget
			]
		];
	Slate.AddWindow(TestWindow, false);
	Slate.SetUserFocus(0, MenuFocusTarget, EFocusCause::SetDirectly);
	TestEqual(TEXT("Fixture begins with keyboard focus on menu content"),
		Slate.GetUserFocusedWidget(0),
		TSharedPtr<SWidget>(MenuFocusTarget));

	AMatterFluxPlayerController::RestoreGameplayViewportFocus(
		GameplayFocusTarget);
	TestEqual(TEXT("Entering gameplay restores keyboard focus to the viewport"),
		Slate.GetUserFocusedWidget(0),
		TSharedPtr<SWidget>(GameplayFocusTarget));
	AMatterFluxPlayerController::RestoreGameplayViewportFocusAfterSlateEvent(
		GameplayFocusTarget,
		[]() { return true; });

	// A menu button can finish processing its Slate reply after the gameplay
	// transition callback returns. Model that ordering explicitly: the old menu
	// focus wins the current frame unless gameplay also repairs focus after the
	// event has unwound.
	Slate.SetUserFocus(0, MenuFocusTarget, EFocusCause::SetDirectly);
	TestEqual(TEXT("Fixture models a late menu focus reply"),
		Slate.GetUserFocusedWidget(0),
		TSharedPtr<SWidget>(MenuFocusTarget));
	ADD_LATENT_AUTOMATION_COMMAND(FVerifyDeferredGameplayFocus(
		this,
		GameplayFocusTarget,
		TestWindow));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxJoinAddressNormalizationTest,
	"MatterFlux.Menu.Multiplayer.JoinAddressNormalization",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxJoinAddressNormalizationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FString Normalized;
	FString Error;

	TestTrue(TEXT("Host without port is accepted"),
		AMatterFluxPlayerController::NormalizeJoinAddress(
			TEXT("  localhost  "), Normalized, Error));
	TestEqual(TEXT("Default port is appended"),
		Normalized, FString(TEXT("localhost:7777")));

	TestTrue(TEXT("Explicit IPv4 port is accepted"),
		AMatterFluxPlayerController::NormalizeJoinAddress(
			TEXT("192.168.1.20:7788"), Normalized, Error));
	TestEqual(TEXT("Explicit port is preserved"),
		Normalized, FString(TEXT("192.168.1.20:7788")));

	TestTrue(TEXT("Bracketed IPv6 is accepted"),
		AMatterFluxPlayerController::NormalizeJoinAddress(
			TEXT("[::1]:7779"), Normalized, Error));
	TestEqual(TEXT("IPv6 remains bracketed"),
		Normalized, FString(TEXT("[::1]:7779")));

	TestFalse(TEXT("URL options cannot be injected"),
		AMatterFluxPlayerController::NormalizeJoinAddress(
			TEXT("127.0.0.1?listen"), Normalized, Error));
	TestFalse(TEXT("Out-of-range port is rejected"),
		AMatterFluxPlayerController::NormalizeJoinAddress(
			TEXT("localhost:70000"), Normalized, Error));
	TestFalse(TEXT("Raw IPv6 requires brackets"),
		AMatterFluxPlayerController::NormalizeJoinAddress(
			TEXT("::1"), Normalized, Error));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFrontEndNavigationCannotEnterGameplayTest,
	"MatterFlux.Menu.Navigation.FrontEndSubmenusCannotEnterGameplay",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFrontEndNavigationCannotEnterGameplayTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UMatterFluxShellWidget* Shell =
		NewObject<UMatterFluxShellWidget>();
	if (!TestNotNull(TEXT("Shell navigation model created"), Shell))
	{
		return false;
	}

	Shell->ShowStartMenu();
	Shell->ShowSettings();
	TestTrue(
		TEXT("Settings opened from the start menu remains in front-end context"),
		Shell->IsStartMenuOpen());
	Shell->CloseMenus();
	TestEqual(
		TEXT("Closing a front-end submenu cannot enter gameplay"),
		Shell->GetView(),
		EMatterFluxShellView::Settings);
	Shell->ReturnFromSubmenu();
	TestEqual(
		TEXT("Settings returns to the start menu"),
		Shell->GetView(),
		EMatterFluxShellView::StartMenu);

	Shell->ShowSinglePlayerMenu();
	Shell->ShowSettings();
	Shell->ShowSettings();
	TestTrue(
		TEXT("Repeated settings clicks preserve front-end context"),
		Shell->IsStartMenuOpen());
	Shell->ReturnFromSubmenu();
	TestEqual(
		TEXT("Repeated settings clicks preserve the single-player return target"),
		Shell->GetView(),
		EMatterFluxShellView::SinglePlayerMenu);

	TestTrue(
		TEXT("Multiplayer entry is enabled for this build"),
		UMatterFluxShellWidget::IsMultiplayerEntryEnabled());
	Shell->ShowMultiplayerMenu();
	TestEqual(
		TEXT("Multiplayer navigation opens the multiplayer menu"),
		Shell->GetView(),
		EMatterFluxShellView::MultiplayerMenu);

	Shell->ShowCreateRoomMenu();
	TestEqual(
		TEXT("Create-room navigation opens the host flow"),
		Shell->GetView(),
		EMatterFluxShellView::CreateRoomMenu);
	Shell->RequestHostRoom();
	TestEqual(
		TEXT("A host request without a controller remains in the host flow"),
		Shell->GetView(),
		EMatterFluxShellView::CreateRoomMenu);
	Shell->ShowJoinRoomMenu();
	TestEqual(
		TEXT("Join-room navigation opens the join flow"),
		Shell->GetView(),
		EMatterFluxShellView::JoinRoomMenu);
	Shell->RequestJoinRoom();
	TestEqual(
		TEXT("A join request without a controller remains in the join flow"),
		Shell->GetView(),
		EMatterFluxShellView::JoinRoomMenu);
	TestTrue(
		TEXT("All non-launching button paths finish in front-end context"),
		Shell->IsStartMenuOpen());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSinglePlayerCancelsPendingJoinTest,
	"MatterFlux.Menu.SinglePlayer.CancelsPendingJoinBeforeNewGame",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSinglePlayerCancelsPendingJoinTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayerController* Controller = World
		? World->SpawnActor<AMatterFluxPlayerController>()
		: nullptr;
	UMatterFluxShellWidget* Shell = Controller
		? NewObject<UMatterFluxShellWidget>(Controller)
		: nullptr;
	if (!TestNotNull(TEXT("Test world exists"), World)
		|| !TestNotNull(TEXT("Local controller exists"), Controller)
		|| !TestNotNull(TEXT("Shell exists"), Shell)
		|| !TestNotNull(TEXT("Engine exists"), GEngine))
	{
		return false;
	}

	Shell->InitializeForPlayer(Controller);
	FWorldContext* WorldContext = GEngine->GetWorldContextFromWorld(World);
	if (!TestNotNull(TEXT("World context exists"), WorldContext))
	{
		return false;
	}
	WorldContext->PendingNetGame = NewObject<UPendingNetGame>();
	TestNotNull(
		TEXT("Fixture represents an in-flight join request"),
		WorldContext->PendingNetGame.Get());

	Shell->ShowSinglePlayerMenu();
	const bool bPendingJoinCancelled =
		WorldContext->PendingNetGame == nullptr;
	if (!bPendingJoinCancelled)
	{
		WorldContext->PendingNetGame = nullptr;
	}
	TestTrue(
		TEXT("Entering single-player cancels an in-flight multiplayer join"),
		bPendingJoinCancelled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFrontEndSuppressesQuestTrackerTest,
	"MatterFlux.Menu.Presentation.FrontEndSuppressesQuestTracker",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFrontEndSuppressesQuestTrackerTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UMatterFluxQuestTrackerWidget* Tracker =
		NewObject<UMatterFluxQuestTrackerWidget>();
	if (!TestNotNull(TEXT("Quest tracker presentation model created"), Tracker))
	{
		return false;
	}

	Tracker->SetSuppressedByFrontEnd(true);
	TestEqual(
		TEXT("The quest tracker is collapsed while the start flow is open"),
		Tracker->GetVisibility(),
		ESlateVisibility::Collapsed);
	Tracker->SetSuppressedByFrontEnd(false);
	TestEqual(
		TEXT("The quest tracker returns after gameplay starts"),
		Tracker->GetVisibility(),
		ESlateVisibility::SelfHitTestInvisible);
	return true;
}

#endif
