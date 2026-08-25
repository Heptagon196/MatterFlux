#include "GeneralProjectSettings.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxGameMode.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerOperation.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UI/MatterFluxShellWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTitleBackgroundSeedTest,
	"MatterFlux.Project.TitleBackgroundUsesFixedSeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxTitleBackgroundSeedTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("The title screen resolves to its stable map seed"),
		AMatterFluxGameMode::ResolveInitialPlayableWorldSeed(true),
		AMatterFluxGameMode::TitleBackgroundMapSeed);
	TestEqual(
		TEXT("Started gameplay leaves the world actor free to choose its runtime seed"),
		AMatterFluxGameMode::ResolveInitialPlayableWorldSeed(false),
		0);
	TestTrue(
		TEXT("The fixed title seed is a valid procedural map seed"),
		AMatterFluxGameMode::TitleBackgroundMapSeed > 0);
	TestTrue(
		TEXT("The completed seed 1337 title capture is present"),
		FPaths::FileExists(FPaths::Combine(
			FPaths::ProjectContentDir(),
			TEXT("UI/TitleBackgroundSeed1337.png"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStorySeedTest,
	"MatterFlux.Project.StoryModeUsesFixedSeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxStorySeedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("Story mode has a valid fixed procedural seed"),
		AMatterFluxPlayableWorldActor::PaperMagicStorySeed > 0);
	TestEqual(TEXT("Story mode keeps the PaperMagic map id"),
		UMatterFluxShellWidget::GetStoryMapId(),
		FName(TEXT("story.paper_magic")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProjectVersionTest,
	"MatterFlux.Project.VersionMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxProjectVersionTest::RunTest(const FString& Parameters)
{
	const UGeneralProjectSettings* Settings = GetDefault<UGeneralProjectSettings>();
	if (!TestNotNull(TEXT("General project settings are available"), Settings))
	{
		return false;
	}

	TestEqual(TEXT("MatterFlux exposes the 0.5.0 minor version"), Settings->ProjectVersion, TEXT("0.5.0"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDefaultMapPlayerStartTest,
	"MatterFlux.Project.DefaultMapHasPlayerStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDefaultMapPlayerStartTest::RunTest(const FString& Parameters)
{
	const UWorld* DefaultWorld = LoadObject<UWorld>(nullptr, TEXT("/Game/Default.Default"));
	if (!TestNotNull(TEXT("Default map can be loaded"), DefaultWorld)
		|| !TestNotNull(TEXT("Default map has a persistent level"), DefaultWorld ? DefaultWorld->PersistentLevel.Get() : nullptr))
	{
		return false;
	}

	int32 PlayerStartCount = 0;
	for (const AActor* Actor : DefaultWorld->PersistentLevel->Actors)
	{
		if (IsValid(Actor) && Actor->IsA<APlayerStart>())
		{
			++PlayerStartCount;
		}
	}
	TestTrue(TEXT("Default map contains at least one PlayerStart"), PlayerStartCount > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDeveloperCaptureModuleTest,
	"MatterFlux.Project.DeveloperCaptureIsIsolated",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxDeveloperCaptureModuleTest::RunTest(
	const FString& Parameters)
{
	FModuleManager& Modules = FModuleManager::Get();
	TestTrue(
		TEXT("Developer capture module exists"),
		Modules.ModuleExists(TEXT("MatterFluxDeveloper")));
	TestTrue(
		TEXT("Developer capture module is loaded in Editor"),
		Modules.IsModuleLoaded(TEXT("MatterFluxDeveloper")));

	IConsoleManager& Console = IConsoleManager::Get();
	for (const TCHAR* Command : {
		TEXT("mf.Visual.Capture"),
		TEXT("mf.Visual.CaptureOccludedPlayer"),
		TEXT("mf.UI.Capture"),
		TEXT("mf.Visual.TreeCutSequence"),
		TEXT("mf.Visual.TreeBatchCut"),
		TEXT("mf.Visual.PhysicsPush"),
		TEXT("mf.Visual.StabilitySequence"),
		TEXT("mf.Player.Ability"),
		TEXT("mf.Reaction.ActivateTree") })
	{
		TestNotNull(
			*FString::Printf(
				TEXT("Developer command is registered: %s"),
				Command),
			Console.FindConsoleObject(Command));
	}
	TestNotNull(
		TEXT("Fragment cut logging command is registered"),
		Console.FindConsoleObject(TEXT("mf.Fragment.CutLog")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayerOperationDelegateTest,
	"MatterFlux.PlayerOperation.MulticastIsStableAndUnsubscribes",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayerOperationDelegateTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxPlayerOperationDelegate& Delegate =
		MatterFlux::PlayerOperations::OnApplied();
	AMatterFluxCharacter* Character =
		GetMutableDefault<AMatterFluxCharacter>();
	if (!TestNotNull(TEXT("Character CDO is available"), Character))
	{
		return false;
	}

	int32 FirstCallCount = 0;
	int32 SecondCallCount = 0;
	EMatterFluxPlayerOperation ReceivedOperation =
		EMatterFluxPlayerOperation::Move;
	FVector2D ReceivedValue = FVector2D::ZeroVector;
	int32 ReceivedInteger = 0;
	bool bReceivedRelayFlag = false;

	const FDelegateHandle FirstHandle = Delegate.AddLambda(
		[&](
			AMatterFluxCharacter& ReceivedCharacter,
			const EMatterFluxPlayerOperation Operation,
			const FVector2D Value,
			const int32 IntegerValue,
			const bool bRelayedFromClient)
		{
			++FirstCallCount;
			TestTrue(
				TEXT("Delegate forwards the character reference"),
				&ReceivedCharacter == Character);
			ReceivedOperation = Operation;
			ReceivedValue = Value;
			ReceivedInteger = IntegerValue;
			bReceivedRelayFlag = bRelayedFromClient;
		});
	const FDelegateHandle SecondHandle = Delegate.AddLambda(
		[&](
			AMatterFluxCharacter&,
			EMatterFluxPlayerOperation,
			FVector2D,
			int32,
			bool)
		{
			++SecondCallCount;
		});

	Delegate.Broadcast(
		*Character,
		EMatterFluxPlayerOperation::CastWand,
		FVector2D(0.25, -0.5),
		3,
		true);

	TestEqual(TEXT("First listener receives one event"), FirstCallCount, 1);
	TestEqual(TEXT("Second listener receives one event"), SecondCallCount, 1);
	TestEqual(
		TEXT("Operation value is preserved"),
		ReceivedOperation,
		EMatterFluxPlayerOperation::CastWand);
	TestTrue(
		TEXT("Vector value is preserved"),
		ReceivedValue.Equals(FVector2D(0.25, -0.5)));
	TestEqual(TEXT("Integer value is preserved"), ReceivedInteger, 3);
	TestTrue(TEXT("Relay flag is preserved"), bReceivedRelayFlag);

	Delegate.Remove(FirstHandle);
	Delegate.Remove(SecondHandle);
	Delegate.Broadcast(
		*Character,
		EMatterFluxPlayerOperation::Move,
		FVector2D::ZeroVector,
		0,
		false);
	TestEqual(
		TEXT("Removed first listener is not called again"),
		FirstCallCount,
		1);
	TestEqual(
		TEXT("Removed second listener is not called again"),
		SecondCallCount,
		1);
	return true;
}
