#include "Misc/AutomationTest.h"

#include "Game/MatterFluxPlayerController.h"
#include "Engine/Engine.h"
#include "Engine/PendingNetGame.h"
#include "Progression/MatterFluxQuestTrackerWidget.h"
#include "UI/MatterFluxShellWidget.h"
#include "Tests/AutomationEditorCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

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

	Shell->ShowMultiplayerMenu();
	Shell->ShowSettings();
	TestTrue(
		TEXT("Settings opened from multiplayer remains in front-end context"),
		Shell->IsStartMenuOpen());
	Shell->ReturnFromSubmenu();
	TestEqual(
		TEXT("Settings returns to multiplayer"),
		Shell->GetView(),
		EMatterFluxShellView::MultiplayerMenu);

	Shell->ShowCreateRoomMenu();
	TestTrue(
		TEXT("Create-room selection remains in front-end context"),
		Shell->IsStartMenuOpen());
	Shell->ShowMultiplayerMenu();
	TestEqual(
		TEXT("Create-room back button returns to multiplayer"),
		Shell->GetView(),
		EMatterFluxShellView::MultiplayerMenu);

	Shell->ShowJoinRoomMenu();
	TestTrue(
		TEXT("Join-room selection remains in front-end context"),
		Shell->IsStartMenuOpen());
	Shell->ShowMultiplayerMenu();
	Shell->ShowStartMenu();
	TestEqual(
		TEXT("Multiplayer back button returns to the start menu"),
		Shell->GetView(),
		EMatterFluxShellView::StartMenu);
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
