#include "Misc/AutomationTest.h"
#include "Rendering/MatterFluxItemOcclusion.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxItemOcclusionConnectedItemsTest,
	"MatterFlux.Rendering.ItemOcclusion.BlockerGhostsConnectedItems",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMatterFluxItemOcclusionConnectedItemsTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFlux::ItemOcclusion;
	const FGuid BlockerId(1, 0, 0, 0);
	const FGuid TouchingId(2, 0, 0, 0);
	const FGuid SameAggregateId(3, 0, 0, 0);
	const FGuid SeparateId(4, 0, 0, 0);
	const FGuid AggregateId(10, 0, 0, 0);
	const TArray<FItem> Items = {
		{BlockerId, AggregateId,
			FBox(FVector(38.0f, -20.0f, 20.0f), FVector(58.0f, 20.0f, 100.0f)), 10.0f},
		{TouchingId, FGuid(),
			FBox(FVector(58.0f, -20.0f, 20.0f), FVector(78.0f, 20.0f, 100.0f)), 10.0f},
		{SameAggregateId, AggregateId,
			FBox(FVector(220.0f, -20.0f, 20.0f), FVector(240.0f, 20.0f, 100.0f)), 10.0f},
		{SeparateId, FGuid(),
			FBox(FVector(220.0f, 100.0f, 20.0f), FVector(240.0f, 120.0f, 100.0f)), 10.0f}
	};
	FResult Result;
	TestTrue(
		TEXT("A foreground item obstructs the viewer"),
		Resolve(
			FVector::ZeroVector,
			FBox(FVector(90.0f, -20.0f, 0.0f), FVector(130.0f, 20.0f, 120.0f)),
			Items,
			Result));
	TestTrue(TEXT("Direct blocker ghosts"), Result.GhostItemIds.Contains(BlockerId));
	TestTrue(TEXT("Touching item ghosts"), Result.GhostItemIds.Contains(TouchingId));
	TestTrue(TEXT("Same aggregate ghosts"), Result.GhostItemIds.Contains(SameAggregateId));
	TestFalse(TEXT("Disconnected item stays solid"), Result.GhostItemIds.Contains(SeparateId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxItemOcclusionBehindViewerTest,
	"MatterFlux.Rendering.ItemOcclusion.ItemsBehindViewerStaySolid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMatterFluxItemOcclusionBehindViewerTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFlux::ItemOcclusion;
	const FGuid BehindId(20, 0, 0, 0);
	const TArray<FItem> Items = {
		{BehindId, FGuid(),
			FBox(FVector(150.0f, -20.0f, 20.0f), FVector(180.0f, 20.0f, 100.0f)), 10.0f}
	};
	FResult Result;
	TestFalse(
		TEXT("An item beyond the viewer is not an occluder"),
		Resolve(
			FVector::ZeroVector,
			FBox(FVector(90.0f, -20.0f, 0.0f), FVector(130.0f, 20.0f, 120.0f)),
			Items,
			Result));
	TestTrue(TEXT("No item ghosts"), Result.IsEmpty());
	return true;
}

#endif
