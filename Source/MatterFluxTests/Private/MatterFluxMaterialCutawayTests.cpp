#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/World.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Rendering/MatterFluxMaterialCutaway.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
	AFragment2DSourceActor* SpawnCutawaySource(
		UWorld& World,
		const TCHAR* StableId,
		const EMatterFluxMaterialStructuralRole Role,
		const FVector& Location,
		const FRotator& Rotation,
		const FVector& Scale,
		const int32 Width,
		const int32 Height)
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AFragment2DSourceActor* Source = World.SpawnActor<AFragment2DSourceActor>(
			AFragment2DSourceActor::StaticClass(), Location, Rotation, Parameters);
		if (!Source)
		{
			return nullptr;
		}
		Source->SetActorScale3D(Scale);
		FFragmentSourceMask Mask;
		Mask.Width = Width;
		Mask.Height = Height;
		Mask.CellSize = 10.0f;
		Mask.MinFragmentAreaPixels = 1;
		Mask.MaxFragmentsPerBreak = 4;
		Mask.SupportMode = EFragmentSupportMode::Bottom;
		Mask.GeometryStyle = EFragmentSourceGeometryStyle::VoxelBlocks;
		Mask.SolidMask.Init(1, Width * Height);
		if (!Source->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(StableId, 1337),
			FLinearColor::White,
			TEXT("wood"),
			Role))
		{
			Source->Destroy();
			return nullptr;
		}
		return Source;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialCutawayConnectedWallTest,
	"MatterFlux.Rendering.MaterialCutaway.ConnectedWallExcludesFurniture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterialCutawayConnectedWallTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Floor = SpawnCutawaySource(
		*World, TEXT("cutaway.floor"),
		EMatterFluxMaterialStructuralRole::Floor,
		FVector::ZeroVector, FRotator(0.0f, 0.0f, 90.0f),
		FVector(1.0f, 2.0f, 1.0f), 10, 10);
	AFragment2DSourceActor* Wall = SpawnCutawaySource(
		*World, TEXT("cutaway.wall.connected"),
		EMatterFluxMaterialStructuralRole::Wall,
		FVector(40.0f, 0.0f, 60.0f), FRotator::ZeroRotator,
		FVector(1.0f, 2.0f, 1.0f), 2, 10);
	AFragment2DSourceActor* Furniture = SpawnCutawaySource(
		*World, TEXT("cutaway.furniture"),
		EMatterFluxMaterialStructuralRole::Furniture,
		FVector(0.0f, 0.0f, 30.0f), FRotator::ZeroRotator,
		FVector(1.0f, 2.0f, 1.0f), 2, 4);
	AFragment2DSourceActor* DisconnectedWall = SpawnCutawaySource(
		*World, TEXT("cutaway.wall.disconnected"),
		EMatterFluxMaterialStructuralRole::Wall,
		FVector(200.0f, 0.0f, 60.0f), FRotator::ZeroRotator,
		FVector(1.0f, 2.0f, 1.0f), 2, 10);
	if (!TestNotNull(TEXT("Floor source spawns"), Floor)
		|| !TestNotNull(TEXT("Connected wall source spawns"), Wall)
		|| !TestNotNull(TEXT("Furniture source spawns"), Furniture)
		|| !TestNotNull(TEXT("Disconnected wall source spawns"), DisconnectedWall))
	{
		return false;
	}

	TArray<AFragment2DSourceActor*> Sources = {
		Floor, Wall, Furniture, DisconnectedWall };
	MatterFlux::MaterialCutaway::FResult Result;
	TestTrue(TEXT("Material cutaway resolves from the floor Source"),
		MatterFlux::MaterialCutaway::Resolve(
			FVector(0.0f, 0.0f, 15.0f),
			Sources,
			FGuid(),
			Result));
	TestEqual(TEXT("The material below the viewer is the floor anchor"),
		Result.FloorSourceId, Floor->SourceId);
	TestTrue(TEXT("Wall material connected above the floor is ghosted"),
		Result.GhostSourceIds.Contains(Wall->SourceId));
	TestFalse(TEXT("Furniture touching the floor remains visible"),
		Result.GhostSourceIds.Contains(Furniture->SourceId));
	TestFalse(TEXT("Disconnected wall material remains visible"),
		Result.GhostSourceIds.Contains(DisconnectedWall->SourceId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialCutawayWallChainTest,
	"MatterFlux.Rendering.MaterialCutaway.WallConnectivityPropagates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterialCutawayWallChainTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Floor = SpawnCutawaySource(
		*World, TEXT("cutaway.chain.floor"),
		EMatterFluxMaterialStructuralRole::Floor,
		FVector::ZeroVector, FRotator(0.0f, 0.0f, 90.0f),
		FVector(1.0f, 2.0f, 1.0f), 10, 10);
	AFragment2DSourceActor* LowerWall = SpawnCutawaySource(
		*World, TEXT("cutaway.chain.lower"),
		EMatterFluxMaterialStructuralRole::Wall,
		FVector(40.0f, 0.0f, 60.0f), FRotator::ZeroRotator,
		FVector(1.0f, 2.0f, 1.0f), 2, 10);
	AFragment2DSourceActor* UpperWall = SpawnCutawaySource(
		*World, TEXT("cutaway.chain.upper"),
		EMatterFluxMaterialStructuralRole::Wall,
		FVector(40.0f, 0.0f, 160.0f), FRotator::ZeroRotator,
		FVector(1.0f, 2.0f, 1.0f), 2, 10);
	if (!TestNotNull(TEXT("Floor source spawns"), Floor)
		|| !TestNotNull(TEXT("Lower wall source spawns"), LowerWall)
		|| !TestNotNull(TEXT("Upper wall source spawns"), UpperWall))
	{
		return false;
	}

	TArray<AFragment2DSourceActor*> Sources = { Floor, LowerWall, UpperWall };
	MatterFlux::MaterialCutaway::FResult Result;
	TestTrue(TEXT("Floor-connected wall graph resolves"),
		MatterFlux::MaterialCutaway::Resolve(
			FVector(0.0f, 0.0f, 15.0f), Sources, FGuid(), Result));
	TestTrue(TEXT("Directly connected wall is ghosted"),
		Result.GhostSourceIds.Contains(LowerWall->SourceId));
	TestTrue(TEXT("Wall connected through another wall is ghosted"),
		Result.GhostSourceIds.Contains(UpperWall->SourceId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialCutawayFloorTierTest,
	"MatterFlux.Rendering.MaterialCutaway.SplitFloorAndStairVoidRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterialCutawayFloorTierTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* GroundLeft = SpawnCutawaySource(
		*World, TEXT("cutaway.tier.ground.left"),
		EMatterFluxMaterialStructuralRole::Floor,
		FVector(-40.0f, 0.0f, 0.0f), FRotator(0.0f, 0.0f, 90.0f),
		FVector(1.0f, 2.0f, 1.0f), 8, 8);
	AFragment2DSourceActor* GroundRight = SpawnCutawaySource(
		*World, TEXT("cutaway.tier.ground.right"),
		EMatterFluxMaterialStructuralRole::Floor,
		FVector(40.0f, 0.0f, 0.0f), FRotator(0.0f, 0.0f, 90.0f),
		FVector(1.0f, 2.0f, 1.0f), 8, 8);
	AFragment2DSourceActor* Upper = SpawnCutawaySource(
		*World, TEXT("cutaway.tier.upper"),
		EMatterFluxMaterialStructuralRole::Floor,
		FVector(0.0f, 0.0f, 100.0f), FRotator(0.0f, 0.0f, 90.0f),
		FVector(1.0f, 2.0f, 1.0f), 4, 4);
	if (!TestNotNull(TEXT("Left ground-floor Source spawns"), GroundLeft)
		|| !TestNotNull(TEXT("Right ground-floor Source spawns"), GroundRight)
		|| !TestNotNull(TEXT("Upper-floor Source spawns"), Upper))
	{
		return false;
	}

	TArray<AFragment2DSourceActor*> Sources = {
		GroundLeft, GroundRight, Upper };
	MatterFlux::MaterialCutaway::FResult UpperResult;
	TestTrue(TEXT("Upper floor resolves from its live material"),
		MatterFlux::MaterialCutaway::Resolve(
			FVector(0.0f, 0.0f, 115.0f), Sources, FGuid(), UpperResult));
	TestEqual(TEXT("Two pieces at one height count as one lower floor"),
		UpperResult.FloorOrdinal, 1);

	MatterFlux::MaterialCutaway::FResult VoidResult;
	TestTrue(TEXT("Stair void retains the previously established floor"),
		MatterFlux::MaterialCutaway::Resolve(
			FVector(60.0f, 0.0f, 115.0f),
			Sources,
			UpperResult.FloorSourceId,
			VoidResult));
	TestEqual(TEXT("A deep ground floor does not steal the stair-void sample"),
		VoidResult.FloorSourceId, Upper->SourceId);
	return true;
}

#endif
