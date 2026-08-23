#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxTwoStoreyHouseActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "IMatterFluxScriptRuntime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
	void AdvanceTestWorld(UWorld& World, const float DeltaSeconds)
	{
		++GFrameCounter;
		World.Tick(LEVELTICK_All, DeltaSeconds);
	}

	void StartTestWorld(UWorld& World)
	{
		World.InitializeActorsForPlay(FURL(), true);
		World.BeginPlay();
	}

	float CharacterFeetZ(const ACharacter& Character)
	{
		const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
		return Character.GetActorLocation().Z
			- (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);
	}

	template <typename TActor>
	TActor* SpawnAlways(
		UWorld& World,
		const FVector& Location,
		const FRotator& Rotation = FRotator::ZeroRotator)
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World.SpawnActor<TActor>(
			TActor::StaticClass(),
			Location,
			Rotation,
			Parameters);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseLayoutTest,
	"MatterFlux.Playable.House.DeterministicLayoutReservesBuildingFootprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseLayoutTest::RunTest(const FString& Parameters)
{
	MatterFlux::PlayableLevel::FLevelLayout First;
	MatterFlux::PlayableLevel::FLevelLayout Second;
	if (!TestTrue(TEXT("First deterministic level builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, First))
		|| !TestTrue(TEXT("Second deterministic level builds"),
			MatterFlux::PlayableLevel::BuildLevelLayout(1337, Second)))
	{
		return false;
	}
	TestTrue(TEXT("A house site is selected"),
		!First.HouseLocation.IsNearlyZero());
	TestTrue(TEXT("Equal seed selects the identical house site"),
		First.HouseLocation.Equals(Second.HouseLocation, 0.0f));

	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: First.FragmentSources)
	{
		if (Source.Name != TEXT("TreeTrunk")
			&& Source.Name != TEXT("RockCluster"))
		{
			continue;
		}
		const FVector Location = Source.Transform.GetLocation();
		const bool bInsideReservedFootprint =
			FMath::Abs(Location.X - First.HouseLocation.X) <= 850.0f
			&& FMath::Abs(Location.Y - First.HouseLocation.Y) <= 710.0f;
		TestFalse(
			*FString::Printf(TEXT("%s stays outside the house reserve"),
				*Source.Name.ToString()),
			bInsideReservedFootprint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseVoxelRoofTest,
	"MatterFlux.Playable.House.RoofUsesThinSteppedVoxelShell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseVoxelRoofTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(
			*World,
			FVector::ZeroVector,
			FRotator(0.0f, 45.0f, 0.0f))
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for roof shape test"), House))
	{
		return false;
	}

	TArray<UInstancedStaticMeshComponent*> Groups;
	House->GetComponents(Groups);
	UInstancedStaticMeshComponent* Roof = nullptr;
	for (UInstancedStaticMeshComponent* Group : Groups)
	{
		if (Group && Group->GetFName() == TEXT("Roof"))
		{
			Roof = Group;
		}
	}
	if (!TestNotNull(TEXT("Roof voxel group exists"), Roof))
	{
		return false;
	}
	TestEqual(TEXT("Twelve paired stair rows plus one ridge form the roof shell"),
		Roof->GetInstanceCount(), 25);
	TSet<int32> QuantizedHeightLevels;
	TMap<int32, int32> RowsPerHeight;
	for (int32 Index = 0; Index < Roof->GetInstanceCount(); ++Index)
	{
		FTransform Transform;
		if (!TestTrue(TEXT("Every roof row exposes its local transform"),
			Roof->GetInstanceTransform(Index, Transform, false)))
		{
			continue;
		}
		const FVector Size = Transform.GetScale3D().GetAbs() * 100.0f;
		TestTrue(TEXT("Every roof tier spans the camera-readable ridge axis"),
			Size.X >= 1190.0f);
		TestTrue(TEXT("Roof rows are thin voxel beams rather than filled slabs"),
			Size.Z >= 34.0f && Size.Z <= 38.0f
				&& Size.Y <= 56.0f);
		const int32 Height = FMath::RoundToInt(Transform.GetLocation().Z);
		QuantizedHeightLevels.Add(Height);
		++RowsPerHeight.FindOrAdd(Height);
	}
	TArray<int32> Heights = QuantizedHeightLevels.Array();
	Heights.Sort();
	for (int32 Index = 0; Index < Heights.Num(); ++Index)
	{
		TestEqual(TEXT("Every slope level is paired except the closed ridge"),
			RowsPerHeight.FindRef(Heights[Index]),
			Index + 1 == Heights.Num() ? 1 : 2);
	}
	TestEqual(TEXT("The thin roof silhouette has twelve steps and a closed ridge"),
		QuantizedHeightLevels.Num(), 13);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseStructureCutTest,
	"MatterFlux.Playable.House.StructureUsesRegisteredCuttableSources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseStructureCutTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for structure cut test"), House))
	{
		return false;
	}
	StartTestWorld(*World);
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}

	TArray<AFragment2DSourceActor*> HouseSources;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (It->GetOwner() == House)
		{
			HouseSources.Add(*It);
		}
	}
	if (!TestTrue(*FString::Printf(
		TEXT("House exposes independently cuttable structure sources (found %d)"),
		HouseSources.Num()),
		HouseSources.Num() >= 8))
	{
		return false;
	}
	TArray<UProceduralMeshComponent*> ProceduralMeshes;
	House->GetComponents(ProceduralMeshes);
	UProceduralMeshComponent* WholeObjectMesh = nullptr;
	for (UProceduralMeshComponent* Mesh : ProceduralMeshes)
	{
		if (Mesh && Mesh->GetFName() == TEXT("CuttableWholeObjectMesh"))
		{
			WholeObjectMesh = Mesh;
			break;
		}
	}
	if (TestNotNull(TEXT("House owns one combined cuttable render mesh"),
		WholeObjectMesh))
	{
		TestTrue(TEXT("Combined house mesh has compiled render sections"),
			WholeObjectMesh->GetNumSections() > 0);
		TestTrue(TEXT("Combined house mesh casts one consistent shadow"),
			WholeObjectMesh->CastShadow);
	}
	for (const AFragment2DSourceActor* Source : HouseSources)
	{
		TestFalse(TEXT("Logical house sources do not double-render"),
			Source->MeshComponent->IsVisible());
	}
	int32 RoofSourceCount = 0;
	int32 RoofGableSourceCount = 0;
	const AFragment2DSourceActor* RoofSource = nullptr;
	const AFragment2DSourceActor* RoofGableSource = nullptr;
	for (const AFragment2DSourceActor* Source : HouseSources)
	{
		if (Source->Tags.Contains(TEXT("MatterFluxHouseGroup.Roof")))
		{
			++RoofSourceCount;
			RoofSource = Source;
		}
		if (Source->Tags.Contains(TEXT("MatterFluxHouseGroup.RoofGable")))
		{
			++RoofGableSourceCount;
			RoofGableSource = Source;
		}
	}
	TestEqual(TEXT("The stepped roof is one unioned cuttable source"),
		RoofSourceCount, 1);
	TestEqual(TEXT("Both roof ends have cuttable gable closure sources"),
		RoofGableSourceCount, 2);
	if (TestNotNull(TEXT("The roof source is available for shell validation"),
		RoofSource))
	{
		const TArray<uint8>& Mask = RoofSource->GetRuntimeMask();
		const int32 Width = RoofSource->GetMaskWidth();
		const int32 Height = RoofSource->GetMaskHeight();
		int32 SolidCount = 0;
		int32 MaximumColumnThickness = 0;
		for (int32 X = 0; X < Width; ++X)
		{
			int32 ColumnSolidCount = 0;
			for (int32 Y = 0; Y < Height; ++Y)
			{
				ColumnSolidCount += Mask[Y * Width + X] != 0 ? 1 : 0;
			}
			SolidCount += ColumnSolidCount;
			MaximumColumnThickness = FMath::Max(
				MaximumColumnThickness, ColumnSolidCount);
		}
		TestTrue(TEXT("The roof mask is a thin shell, not a filled gable"),
			SolidCount * 4 < Width * Height);
		TestTrue(TEXT("No roof column exceeds two voxel cells of thickness"),
			MaximumColumnThickness <= 2);
		TestEqual(TEXT("The lower centre stays open below the ridge"),
			Mask[(Width / 2)], static_cast<uint8>(0));
		TestEqual(TEXT("The left eave begins at the lowest shell row"),
			Mask[0], static_cast<uint8>(1));
		TestEqual(TEXT("The right eave begins at the lowest shell row"),
			Mask[Width - 1], static_cast<uint8>(1));
	}
	if (TestNotNull(TEXT("A roof gable source is available for validation"),
		RoofGableSource))
	{
		const TArray<uint8>& Mask = RoofGableSource->GetRuntimeMask();
		const int32 Width = RoofGableSource->GetMaskWidth();
		const int32 Height = RoofGableSource->GetMaskHeight();
		TestEqual(TEXT("The gable closes the lower centre below the ridge"),
			Mask[Width / 2], static_cast<uint8>(1));
		TestEqual(TEXT("The gable leaves the outer eave corner open"),
			Mask[0], static_cast<uint8>(0));
		TestEqual(TEXT("The gable stops below the roof shell"),
			Mask[(Height - 1) * Width + Width / 2],
			static_cast<uint8>(0));
	}
	const FBox StairClearanceLocal(
		FVector(-380.0f, 120.0f, 34.0f),
		FVector(380.0f, 365.0f,
			AMatterFluxTwoStoreyHouseActor::StoreyHeight + 14.0f));
	const FBox StairClearanceWorld = StairClearanceLocal.TransformBy(
		House->GetActorTransform().ToMatrixWithScale());
	for (const AFragment2DSourceActor* Source : HouseSources)
	{
		const FBox Bounds = Source->GetComponentsBoundingBox(true);
		TestTrue(*FString::Printf(
			TEXT("House source %s keeps its authored box scale; size=%s"),
			*Source->GetName(),
			*Bounds.GetSize().ToString()),
			Bounds.GetSize().GetMax() <= 1250.0f);
		TestFalse(*FString::Printf(
			TEXT("House source %s stays outside stair clearance; bounds=%s"),
			*Source->GetName(),
			*Bounds.ToString()),
			Bounds.Intersect(StairClearanceWorld));
	}

	AFragment2DSourceActor* Target = HouseSources[0];
	for (AFragment2DSourceActor* Source : HouseSources)
	{
		if (Source
			&& Source->SourceMaterialId == TEXT("stone")
			&& Source->ActorHasTag(TEXT("MatterFluxHouseGroup.LowerWalls")))
		{
			Target = Source;
			break;
		}
	}
	TestEqual(
		TEXT("Cuttable house state keeps voxel geometry semantics for both static and rigid projections"),
		Target->ProceduralSource.GeometryStyle,
		EFragmentSourceGeometryStyle::VoxelBlocks);
	UMaterialInstanceDynamic* SourceMaterial =
		Cast<UMaterialInstanceDynamic>(Target->MeshComponent->GetMaterial(0));
	UMaterialInstanceDynamic* WholeObjectMaterial = nullptr;
	if (WholeObjectMesh && SourceMaterial)
	{
		const FLinearColor SourceColor =
			SourceMaterial->K2_GetVectorParameterValue(TEXT("Color"));
		for (int32 Slot = 0; Slot < WholeObjectMesh->GetNumMaterials(); ++Slot)
		{
			UMaterialInstanceDynamic* Candidate =
				Cast<UMaterialInstanceDynamic>(WholeObjectMesh->GetMaterial(Slot));
			if (Candidate
				&& Candidate->K2_GetVectorParameterValue(TEXT("Color"))
					.Equals(SourceColor, 1.0e-4f))
			{
				WholeObjectMaterial = Candidate;
				break;
			}
		}
	}
	if (TestNotNull(TEXT("Wall source has a dynamic voxel material"),
		SourceMaterial)
		&& TestNotNull(TEXT("Combined wall resolves the same material color"),
			WholeObjectMaterial))
	{
		for (const FName Parameter : {
			TEXT("FaceContrast"),
			TEXT("ColorVariation"),
			TEXT("Roughness"),
			TEXT("ShadowLift")})
		{
			TestEqual(
				*FString::Printf(
					TEXT("Wall %s stays stable across source and whole-object representations"),
					*Parameter.ToString()),
				WholeObjectMaterial->K2_GetScalarParameterValue(Parameter),
				SourceMaterial->K2_GetScalarParameterValue(Parameter));
		}
	}
	if (WholeObjectMesh)
	{
		const FLinearColor ExpectedSideColor =
			MatterFlux::Rendering::ResolveVoxelMaterialColor(
				Target->FragmentColor,
				Target->SourceMaterialId,
				true);
		bool bFoundCanonicalSideProjection = false;
		for (int32 Slot = 0; Slot < WholeObjectMesh->GetNumMaterials(); ++Slot)
		{
			UMaterialInstanceDynamic* Candidate =
				Cast<UMaterialInstanceDynamic>(WholeObjectMesh->GetMaterial(Slot));
			bFoundCanonicalSideProjection |= Candidate
				&& Candidate->K2_GetVectorParameterValue(TEXT("Color"))
					.Equals(ExpectedSideColor, 1.0e-4f);
		}
		TestTrue(
			TEXT("Combined house mesh preserves the canonical darker side-face projection before any cut"),
			bFoundCanonicalSideProjection);
	}
	const int32 RevisionBefore = Target->Revision;
	const auto CountWholeObjectTriangles = [WholeObjectMesh]()
	{
		int32 TriangleCount = 0;
		if (!WholeObjectMesh)
		{
			return TriangleCount;
		}
		for (int32 SectionIndex = 0;
			SectionIndex < WholeObjectMesh->GetNumSections();
			++SectionIndex)
		{
			if (const FProcMeshSection* Section =
				WholeObjectMesh->GetProcMeshSection(SectionIndex))
			{
				TriangleCount += Section->ProcIndexBuffer.Num() / 3;
			}
		}
		return TriangleCount;
	};
	const int32 TrianglesBeforeCut = CountWholeObjectTriangles();
	FFragmentDamageEvent Event;
	Event.SourceId = Target->SourceId;
	Event.BaseRevision = RevisionBefore;
	Event.DamageShape.Type = EFragmentDamageShapeType::Circle;
	Event.DamageShape.WorldTransform = Target->GetActorTransform();
	Event.DamageShape.Radius = Target->GetCellSize() * 1.1f;
	Event.DamagePower = 1000.0f;
	Event.EventSeed = 7819;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	TestNotNull(TEXT("Fragment simulation subsystem exists"), Subsystem);
	if (Subsystem)
	{
		TestTrue(TEXT("A house structure cut is accepted and committed"),
			Subsystem->RequestFragmentDamage(Target, Event));
		TestEqual(TEXT("House source revision advances exactly once"),
			Target->Revision, RevisionBefore + 1);
		AdvanceTestWorld(*World, 0.06f);
		if (WholeObjectMesh)
		{
			TestTrue(TEXT("Combined house mesh remains valid after a cut"),
				WholeObjectMesh->GetNumSections() > 0);
			TestNotEqual(
				TEXT("The committed material mask changes the combined visible projection"),
				CountWholeObjectTriangles(),
				TrianglesBeforeCut);
		}
		TestFalse(
			TEXT("Damage never re-enables a logical house Source as a second visible projection"),
			Target->MeshComponent->IsVisible());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseFurnitureCutTest,
	"MatterFlux.Playable.House.FurnitureUsesRegisteredCuttableSources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseFurnitureCutTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for furniture cut test"), House))
	{
		return false;
	}
	StartTestWorld(*World);
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}

	TArray<AFragment2DSourceActor*> FurnitureSources;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (It->GetOwner() == House
			&& It->ActorHasTag(TEXT("MatterFluxHouseFurniture")))
		{
			FurnitureSources.Add(*It);
		}
	}
	if (!TestTrue(*FString::Printf(
		TEXT("Every authored furniture piece enters canonical cuttable state (found %d)"),
		FurnitureSources.Num()),
		FurnitureSources.Num() >= 12))
	{
		return false;
	}

	AFragment2DSourceActor* Target = FurnitureSources[0];
	double BestTableDistanceSquared = TNumericLimits<double>::Max();
	const FVector TableTopLocal(-115.0f, -120.0f, 118.0f);
	for (AFragment2DSourceActor* Source : FurnitureSources)
	{
		TestTrue(TEXT("Furniture keeps a semantic wood or fabric material fact"),
			Source->SourceMaterialId == TEXT("wood")
				|| Source->SourceMaterialId == TEXT("fabric"));
		TestFalse(TEXT("Furniture logical sources never double-render"),
			Source->MeshComponent->IsVisible());
		if (Source->ActorHasTag(
			TEXT("MatterFluxHouseGroup.LowerFurnitureWood")))
		{
			const FVector SourceLocal = House->GetActorTransform()
				.InverseTransformPosition(Source->GetActorLocation());
			const double DistanceSquared = FVector::DistSquared(
				SourceLocal, TableTopLocal);
			if (DistanceSquared < BestTableDistanceSquared)
			{
				BestTableDistanceSquared = DistanceSquared;
				Target = Source;
			}
		}
	}

	const int32 RevisionBefore = Target->Revision;
	FFragmentWorldCutRequest Request;
	Request.CutShape.Type = EFragmentDamageShapeType::Circle;
	Request.CutShape.WorldTransform = Target->GetActorTransform();
	Request.CutShape.Radius = Target->GetCellSize() * 1.15f;
	Request.DamagePower = 1000.0f;
	Request.EventSeed = 0x4655524E;
	Request.TargetPadding = 1.0f;
	Request.MaxAffectedSources = 1;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("World cut service exists for furniture"), Subsystem))
	{
		return false;
	}
	TestEqual(TEXT("The generic world cut accepts one furniture target"),
		Subsystem->RequestWorldCut(Request), 1);
	TestEqual(TEXT("The furniture material revision advances once"),
		Target->Revision, RevisionBefore + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseWallEnvelopeContinuityTest,
	"MatterFlux.Playable.House.WallEnvelopeHasNoStoreySeams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseWallEnvelopeContinuityTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for wall continuity test"), House))
	{
		return false;
	}
	StartTestWorld(*World);
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}

	TArray<AFragment2DSourceActor*> LowerWalls;
	TArray<AFragment2DSourceActor*> UpperWalls;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (It->GetOwner() != House)
		{
			continue;
		}
		if (It->ActorHasTag(TEXT("MatterFluxHouseGroup.LowerWalls")))
		{
			LowerWalls.Add(*It);
		}
		else if (It->ActorHasTag(TEXT("MatterFluxHouseGroup.UpperWalls")))
		{
			UpperWalls.Add(*It);
		}
	}
	if (!TestEqual(TEXT("Every upper wall piece has a lower-storey counterpart"),
		UpperWalls.Num(), LowerWalls.Num())
		|| UpperWalls.IsEmpty())
	{
		return false;
	}
	TestTrue(TEXT("Both storeys project the identical plaster material fact"),
		UpperWalls[0]->FragmentColor.Equals(
			LowerWalls[0]->FragmentColor, 1.0e-4f));

	for (AFragment2DSourceActor* Upper : UpperWalls)
	{
		// 门窗上方的短横梁在上下层之间刻意留下开口，不属于承重
		// 外壳的竖向连续面。这里只检查贯穿楼层的墙段和角柱。
		if (Upper->GetMaskHeight() < 10)
		{
			continue;
		}
		AFragment2DSourceActor* MatchingLower = nullptr;
		double BestPlanarDistance = TNumericLimits<double>::Max();
		for (AFragment2DSourceActor* Lower : LowerWalls)
		{
			if (Lower->GetMaskHeight() < 10)
			{
				continue;
			}
			const FVector Delta =
				Upper->GetActorLocation() - Lower->GetActorLocation();
			const double PlanarDistance =
				Delta.X * Delta.X + Delta.Y * Delta.Y;
			if (PlanarDistance < BestPlanarDistance)
			{
				BestPlanarDistance = PlanarDistance;
				MatchingLower = Lower;
			}
		}
		if (!TestNotNull(TEXT("Upper wall resolves its lower counterpart"),
			MatchingLower))
		{
			continue;
		}
		const FBox LowerBounds =
			MatchingLower->GetComponentsBoundingBox(true);
		const FBox UpperBounds = Upper->GetComponentsBoundingBox(true);
		TestTrue(*FString::Printf(
			TEXT("Wall envelope is continuous through the floor band; gap=%.2f"),
			UpperBounds.Min.Z - LowerBounds.Max.Z),
			UpperBounds.Min.Z <= LowerBounds.Max.Z + 0.5f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseCutawayTest,
	"MatterFlux.Playable.House.LocalFloorCutawayFadesStructureAndCreatures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseCutawayTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("Two-storey house spawns"), House))
	{
		return false;
	}
	StartTestWorld(*World);

	const float GroundZ = House->GetFloorSurfaceWorldZ(0);
	const float UpperZ = House->GetFloorSurfaceWorldZ(1);
	AMatterFluxCharacter* Viewer = SpawnAlways<AMatterFluxCharacter>(
		*World, FVector(-180.0f, -120.0f, GroundZ + 88.0f));
	AMatterFluxCreatureActor* Creature =
		SpawnAlways<AMatterFluxCreatureActor>(
			*World, FVector(210.0f, -120.0f, UpperZ + 80.0f));
	if (!TestNotNull(TEXT("Viewer character spawns indoors"), Viewer)
		|| !TestNotNull(TEXT("Creature spawns on the upper floor"), Creature))
	{
		return false;
	}

	House->SetCutawayViewerOverride(Viewer);
	House->RefreshCutawayImmediately();
	TestEqual(TEXT("Viewer feet select the ground floor cutaway"),
		House->GetCurrentCutawayFloor(), 0);
	TestEqual(TEXT("Furniture on the viewer's floor remains fully visible"),
		House->GetFurnitureOpacity(0), 1.0f);
	TestTrue(TEXT("Furniture above the viewer is hidden with the upper storey"),
		House->GetFurnitureOpacity(1) > 0.0f
			&& House->GetFurnitureOpacity(1) < 0.30f);
	TestTrue(TEXT("Walls, furniture and upper structure use ghost opacity"),
		House->GetCurrentStructureOpacity() > 0.0f
			&& House->GetCurrentStructureOpacity() < 0.10f);
	TestTrue(TEXT("Upper floor fades while viewed from the ground floor"),
		House->GetFloorSurfaceOpacity(1) > 0.10f
			&& House->GetFloorSurfaceOpacity(1) < 0.25f);
	TestTrue(TEXT("Stacked roof and walls fade more strongly than one floor"),
		House->GetCurrentStructureOpacity()
			< House->GetFloorSurfaceOpacity(1));
	TestEqual(TEXT("Upper-floor creature is included in the local cutaway"),
		House->GetTrackedInteriorActorCount(), 1);

	Viewer->SetActorLocation(FVector(-180.0f, -120.0f, UpperZ + 88.0f));
	House->RefreshCutawayImmediately();
	TestEqual(TEXT("Viewer feet select the upper floor cutaway"),
		House->GetCurrentCutawayFloor(), 1);
	TestEqual(TEXT("The floor beneath the viewer remains opaque"),
		House->GetFloorSurfaceOpacity(1), 1.0f);
	TestEqual(TEXT("Upper-floor furniture remains visible beside the viewer"),
		House->GetFurnitureOpacity(1), 1.0f);
	TestEqual(TEXT("Same-floor creature remains a cutaway participant"),
		House->GetTrackedInteriorActorCount(), 1);

	Viewer->SetActorLocation(FVector(1400.0f, 0.0f, GroundZ + 88.0f));
	House->RefreshCutawayImmediately();
	TestEqual(TEXT("Leaving the house restores the full facade"),
		House->GetCurrentCutawayFloor(), INDEX_NONE);
	TestEqual(TEXT("Restored house returns to opaque material"),
		House->GetCurrentStructureOpacity(), 1.0f);
	TestEqual(TEXT("Creature materials are restored after leaving"),
		House->GetTrackedInteriorActorCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseStairTraversalTest,
	"MatterFlux.Playable.House.PlayerAndCreatureTraverseBothStoreys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseStairTraversalTest::RunTest(const FString& Parameters)
{
	FString ContentError;
	if (!TestTrue(TEXT("Creature content reloads for indoor AI"),
		IMatterFluxScriptRuntime::Get().ReloadDefaultContentPack(ContentError)))
	{
		AddError(ContentError);
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for traversal test"), House))
	{
		return false;
	}
	StartTestWorld(*World);
	TestEqual(TEXT("Stair ramp blocks pawns and physics queries"),
		House->StairRampCollision->GetCollisionEnabled(),
		ECollisionEnabled::QueryAndPhysics);

	// 三条竖直射线必须依次命中越来越高的连续坡面；可见台阶本身不
	// 承担碰撞，所以这里同时防止日后重新引入逐级抖动。
	float PreviousHitZ = -TNumericLimits<float>::Max();
	for (const float X : {-260.0f, 0.0f, 260.0f})
	{
		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByChannel(
			Hit,
			FVector(X, 245.0f, 620.0f),
			FVector(X, 245.0f, -80.0f),
			ECC_Visibility);
		TestTrue(TEXT("A stair sample hits walkable collision"), bHit);
		if (bHit)
		{
			TestTrue(TEXT("Stair collision rises monotonically"),
				Hit.ImpactPoint.Z > PreviousHitZ + 80.0f);
			PreviousHitZ = Hit.ImpactPoint.Z;
		}
	}

	const float GroundZ = House->GetFloorSurfaceWorldZ(0);
	const float UpperZ = House->GetFloorSurfaceWorldZ(1);
	const FCollisionShape PlayerCapsule = FCollisionShape::MakeCapsule(
		42.0f, 88.0f);
	const FCollisionShape CreatureCapsule = FCollisionShape::MakeCapsule(
		34.0f, 74.0f);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(HouseTraversal), false);

	// 沿实际坡面逐点放置玩家和生物胶囊，验证从一楼到二楼没有
	// 头顶楼板、扶手或墙体造成的阻挡。运动是否按时间到达由后续
	// 真实 Game World 的可视序列继续验收。
	float LastSurfaceZ = GroundZ;
	for (int32 Sample = 0; Sample <= 12; ++Sample)
	{
		const float Alpha = static_cast<float>(Sample) / 12.0f;
		const float X = FMath::Lerp(-260.0f, 260.0f, Alpha);
		FHitResult FloorHit;
		const bool bHasFloor = World->LineTraceSingleByChannel(
			FloorHit,
			FVector(X, 245.0f, 620.0f),
			FVector(X, 245.0f, -80.0f),
			ECC_Visibility,
			QueryParams);
		if (!TestTrue(TEXT("Every traversal sample has a floor"), bHasFloor))
		{
			continue;
		}
		TestTrue(TEXT("Traversal surface never steps downward"),
			FloorHit.ImpactPoint.Z + 2.0f >= LastSurfaceZ);
		LastSurfaceZ = FloorHit.ImpactPoint.Z;
		const FVector PlayerCenter(
			X, 245.0f, FloorHit.ImpactPoint.Z + 100.0f);
		const FVector CreatureCenter(
			X, 245.0f, FloorHit.ImpactPoint.Z + 86.0f);
		TestFalse(TEXT("Player capsule has clearance along the entire stair"),
			World->OverlapBlockingTestByChannel(
				PlayerCenter, FQuat::Identity, ECC_Pawn,
				PlayerCapsule, QueryParams));
		TestFalse(TEXT("Creature capsule has clearance along the entire stair"),
			World->OverlapBlockingTestByChannel(
				CreatureCenter, FQuat::Identity, ECC_Pawn,
				CreatureCapsule, QueryParams));
	}

	float MinimumWaypointZ = TNumericLimits<float>::Max();
	float MaximumWaypointZ = -TNumericLimits<float>::Max();
	for (int32 Index = 0;
		Index < House->GetIndoorPatrolWaypointCount(); ++Index)
	{
		const FVector Waypoint = House->GetIndoorPatrolWaypoint(Index);
		MinimumWaypointZ = FMath::Min(MinimumWaypointZ, Waypoint.Z);
		MaximumWaypointZ = FMath::Max(MaximumWaypointZ, Waypoint.Z);
		TestTrue(TEXT("Every AI waypoint remains in the house"),
			AMatterFluxTwoStoreyHouseActor::FindContainingHouse(
				*World, Waypoint, 80.0f) == House);
	}
	TestTrue(TEXT("Indoor AI route starts on the ground floor"),
		MinimumWaypointZ <= GroundZ + 2.0f);
	TestTrue(TEXT("Indoor AI route reaches the upper floor"),
		MaximumWaypointZ >= UpperZ - 2.0f);
	return true;
}

#endif
