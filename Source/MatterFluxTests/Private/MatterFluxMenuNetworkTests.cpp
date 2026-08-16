#include "Misc/AutomationTest.h"

#include "Game/MatterFluxPlayerController.h"

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

#endif
