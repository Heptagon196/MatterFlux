#include "Material/MatterFluxLogicalSourceCombustionIndex.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLogicalSourceCombustionIndexTest,
	"MatterFlux.Combustion.LogicalSourceIndexTracksOnlyCurrentFire",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxLogicalSourceCombustionIndexTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Combustion::FLogicalSourceCombustionIndex Index;
	const FGuid Later(9, 0, 0, 0);
	const FGuid Earlier(1, 8, 7, 6);
	const TArray<uint8> Burning = { 0, 2, 0, 0 };
	const TArray<uint8> Extinguished = { 0, 0, 0, 0 };

	TestFalse(
		TEXT("Invalid source ids are rejected"),
		Index.ApplySnapshot(FGuid(), true, Burning));
	TestTrue(
		TEXT("A current burning snapshot is accepted"),
		Index.ApplySnapshot(Later, true, Burning));
	TestTrue(
		TEXT("A second current fire is accepted"),
		Index.ApplySnapshot(Earlier, true, Burning));
	TestEqual(TEXT("Only current fires are indexed"), Index.Num(), 2);

	TArray<FGuid> StableIds;
	Index.GatherStableIds(StableIds);
	TestTrue(
		TEXT("Active ids use component-wise stable GUID order"),
		StableIds == TArray<FGuid>({ Earlier, Later }));

	TestTrue(
		TEXT("A residue-only historical snapshot is accepted"),
		Index.ApplySnapshot(Earlier, true, Extinguished));
	TestFalse(
		TEXT("Extinguished history is removed from active work"),
		Index.Contains(Earlier));
	TestEqual(TEXT("Historical states do not grow active work"), Index.Num(), 1);

	TestTrue(
		TEXT("Removing combustion metadata is accepted"),
		Index.ApplySnapshot(Later, false, Burning));
	TestEqual(TEXT("No active fire leaves an empty index"), Index.Num(), 0);
	return true;
}

#endif
