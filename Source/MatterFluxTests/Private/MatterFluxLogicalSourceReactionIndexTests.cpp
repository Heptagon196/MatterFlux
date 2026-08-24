#include "Material/MatterFluxLogicalSourceReactionIndex.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLogicalSourceReactionIndexTest,
	"MatterFlux.Reaction.LogicalSourceIndexTracksOnlyCurrentFire",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxLogicalSourceReactionIndexTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Reaction::FLogicalSourceReactionIndex Index;
	const FGuid Later(9, 0, 0, 0);
	const FGuid Earlier(1, 8, 7, 6);
	const TArray<uint8> Active = { 0, 2, 0, 0 };
	const TArray<uint8> Extinguished = { 0, 0, 0, 0 };

	TestFalse(
		TEXT("Invalid source ids are rejected"),
		Index.ApplySnapshot(FGuid(), true, Active));
	TestTrue(
		TEXT("A current active snapshot is accepted"),
		Index.ApplySnapshot(Later, true, Active));
	TestTrue(
		TEXT("A second current fire is accepted"),
		Index.ApplySnapshot(Earlier, true, Active));
	TestEqual(TEXT("Only current fires are indexed"), Index.Num(), 2);

	TArray<FGuid> StableIds;
	Index.GatherStableIds(StableIds);
	TestTrue(
		TEXT("Active ids use component-wise stable GUID order"),
		StableIds == TArray<FGuid>({ Earlier, Later }));

	TestTrue(
		TEXT("A output-only historical snapshot is accepted"),
		Index.ApplySnapshot(Earlier, true, Extinguished));
	TestFalse(
		TEXT("Extinguished history is removed from active work"),
		Index.Contains(Earlier));
	TestEqual(TEXT("Historical states do not grow active work"), Index.Num(), 1);

	TestTrue(
		TEXT("Removing reaction metadata is accepted"),
		Index.ApplySnapshot(Later, false, Active));
	TestEqual(TEXT("No active fire leaves an empty index"), Index.Num(), 0);
	return true;
}

#endif
