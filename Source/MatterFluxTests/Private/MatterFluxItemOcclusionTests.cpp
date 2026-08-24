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
	const FGuid IndependentTouchingId(2, 0, 0, 0);
	const FGuid SameAggregateId(3, 0, 0, 0);
	const FGuid SeparateId(4, 0, 0, 0);
	const FGuid AggregateId(10, 0, 0, 0);
	const TArray<FItem> Items = {
		{BlockerId, AggregateId,
			FBox(FVector(38.0f, -20.0f, 20.0f), FVector(58.0f, 20.0f, 100.0f)), 10.0f},
		{IndependentTouchingId, FGuid(),
			FBox(FVector(38.0f, 20.0f, 20.0f), FVector(58.0f, 40.0f, 100.0f)), 10.0f},
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
	TestFalse(
		TEXT("An independent touching item stays solid"),
		Result.GhostItemIds.Contains(IndependentTouchingId));
	TestTrue(TEXT("Same aggregate ghosts"), Result.GhostItemIds.Contains(SameAggregateId));
	TestFalse(TEXT("Disconnected item stays solid"), Result.GhostItemIds.Contains(SeparateId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxItemOcclusionIndependentContactChainTest,
	"MatterFlux.Rendering.ItemOcclusion.IndependentContactChainStaysSolid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMatterFluxItemOcclusionIndependentContactChainTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFlux::ItemOcclusion;
	const FGuid TrunkId(11, 0, 0, 0);
	const FGuid CrownId(12, 0, 0, 0);
	const FGuid DecorationId(13, 0, 0, 0);
	const FGuid DecorationNeighborId(14, 0, 0, 0);
	const FGuid TreeAggregateId(20, 0, 0, 0);
	const TArray<FItem> Items = {
		{TrunkId, TreeAggregateId,
			FBox(FVector(38.0f, -10.0f, 20.0f), FVector(58.0f, 10.0f, 100.0f)), 10.0f},
		{CrownId, TreeAggregateId,
			FBox(FVector(38.0f, 40.0f, 20.0f), FVector(58.0f, 60.0f, 100.0f)), 10.0f},
		{DecorationId, FGuid(),
			FBox(FVector(38.0f, 60.0f, 20.0f), FVector(58.0f, 80.0f, 100.0f)), 10.0f},
		{DecorationNeighborId, FGuid(),
			FBox(FVector(38.0f, 80.0f, 20.0f), FVector(58.0f, 100.0f, 100.0f)), 10.0f}
	};
	const TArray<FItem> DecorationsOnly = {Items[2], Items[3]};
	FResult DecorationsOnlyResult;
	TestFalse(
		TEXT("The decorations do not independently obstruct the viewer"),
		Resolve(
			FVector::ZeroVector,
			FBox(FVector(90.0f, -20.0f, 0.0f), FVector(130.0f, 20.0f, 120.0f)),
			DecorationsOnly,
			DecorationsOnlyResult));
	FResult Result;
	TestTrue(
		TEXT("The tree trunk obstructs the viewer"),
		Resolve(
			FVector::ZeroVector,
			FBox(FVector(90.0f, -20.0f, 0.0f), FVector(130.0f, 20.0f, 120.0f)),
			Items,
			Result));
	TestTrue(TEXT("Directly blocking trunk ghosts"), Result.GhostItemIds.Contains(TrunkId));
	TestTrue(TEXT("The rest of the same tree ghosts"), Result.GhostItemIds.Contains(CrownId));
	TestFalse(
		TEXT("An independent decoration touching the tree stays solid"),
		Result.GhostItemIds.Contains(DecorationId));
	TestFalse(
		TEXT("Contact cannot chain into neighboring decorations"),
		Result.GhostItemIds.Contains(DecorationNeighborId));
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
