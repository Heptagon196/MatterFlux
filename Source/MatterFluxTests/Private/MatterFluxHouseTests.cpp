#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "GAS/GA_CastWand.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxTwoStoreyHouseActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "IMatterFluxScriptRuntime.h"
#include "Magic/MatterFluxMagicProjectile.h"
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

	TSet<uint32> CollectProjectionTriangleKeysInsideBounds(
		UProceduralMeshComponent& Projection,
		const FBox& WorldBounds)
	{
		TSet<uint32> TriangleKeys;
		for (int32 SectionIndex = 0;
			SectionIndex < Projection.GetNumSections(); ++SectionIndex)
		{
			const FProcMeshSection* Section =
				Projection.GetProcMeshSection(SectionIndex);
			if (!Section)
			{
				continue;
			}
			for (int32 TriangleIndex = 0;
				TriangleIndex + 2 < Section->ProcIndexBuffer.Num();
				TriangleIndex += 3)
			{
				const int32 A = Section->ProcIndexBuffer[TriangleIndex];
				const int32 B = Section->ProcIndexBuffer[TriangleIndex + 1];
				const int32 C = Section->ProcIndexBuffer[TriangleIndex + 2];
				if (!Section->ProcVertexBuffer.IsValidIndex(A)
					|| !Section->ProcVertexBuffer.IsValidIndex(B)
					|| !Section->ProcVertexBuffer.IsValidIndex(C))
				{
					continue;
				}
				const FVector WorldA = Projection.GetComponentTransform()
					.TransformPosition(Section->ProcVertexBuffer[A].Position);
				const FVector WorldB = Projection.GetComponentTransform()
					.TransformPosition(Section->ProcVertexBuffer[B].Position);
				const FVector WorldC = Projection.GetComponentTransform()
					.TransformPosition(Section->ProcVertexBuffer[C].Position);
				const FVector WorldCentroid = (WorldA + WorldB + WorldC) / 3.0f;
				if (WorldBounds.IsInsideOrOn(WorldCentroid))
				{
					const FVector WorldNormal = FVector::CrossProduct(
						WorldB - WorldA, WorldC - WorldA).GetSafeNormal();
					const FIntVector QuantizedCentroid(
						FMath::RoundToInt(WorldCentroid.X * 10.0f),
						FMath::RoundToInt(WorldCentroid.Y * 10.0f),
						FMath::RoundToInt(WorldCentroid.Z * 10.0f));
					const FIntVector QuantizedNormal(
						FMath::RoundToInt(WorldNormal.X * 1000.0f),
						FMath::RoundToInt(WorldNormal.Y * 1000.0f),
						FMath::RoundToInt(WorldNormal.Z * 1000.0f));
					TriangleKeys.Add(HashCombineFast(
						GetTypeHash(QuantizedCentroid),
						GetTypeHash(QuantizedNormal)));
				}
			}
		}
		return TriangleKeys;
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
			FMath::Abs(Location.X - First.HouseLocation.X) <= 950.0f
			&& FMath::Abs(Location.Y - First.HouseLocation.Y) <= 950.0f;
		TestFalse(
			*FString::Printf(TEXT("%s stays outside the house reserve"),
				*Source.Name.ToString()),
			bInsideReservedFootprint);
	}
	float MinimumWalkRingHeight = TNumericLimits<float>::Max();
	float MaximumWalkRingHeight = -TNumericLimits<float>::Max();
	const int64 FirstWorldCellX = FMath::FloorToInt64(
		static_cast<double>(First.Terrain.FirstCellCenter.X)
		/ First.Terrain.CellSize);
	const int64 FirstWorldCellY = FMath::FloorToInt64(
		static_cast<double>(First.Terrain.FirstCellCenter.Y)
		/ First.Terrain.CellSize);
	for (int32 SampleIndex = 0; SampleIndex < 16; ++SampleIndex)
	{
		const float Angle = 2.0f * PI
			* static_cast<float>(SampleIndex) / 16.0f;
		const FVector2D SampleWorld = FVector2D(First.HouseLocation)
			+ FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * 650.0f;
		const int64 CellX = FirstWorldCellX + FMath::RoundToInt64(
			(SampleWorld.X - First.Terrain.FirstCellCenter.X)
			/ First.Terrain.CellSize);
		const int64 CellY = FirstWorldCellY + FMath::RoundToInt64(
			(SampleWorld.Y - First.Terrain.FirstCellCenter.Y)
			/ First.Terrain.CellSize);
		float Height = 0.0f;
		uint8 ColorBand = 0;
		if (TestTrue(TEXT("House walking-ring terrain sample exists"),
			First.Terrain.TrySampleWorldCell(CellX, CellY, Height, ColorBand)))
		{
			MinimumWalkRingHeight = FMath::Min(MinimumWalkRingHeight, Height);
			MaximumWalkRingHeight = FMath::Max(MaximumWalkRingHeight, Height);
		}
	}
	const int64 CenterCellX = FirstWorldCellX + FMath::RoundToInt64(
		(First.HouseLocation.X - First.Terrain.FirstCellCenter.X)
		/ First.Terrain.CellSize);
	const int64 CenterCellY = FirstWorldCellY + FMath::RoundToInt64(
		(First.HouseLocation.Y - First.Terrain.FirstCellCenter.Y)
		/ First.Terrain.CellSize);
	float CenterHeight = 0.0f;
	uint8 CenterColorBand = 0;
	if (TestTrue(TEXT("House-center terrain sample exists"),
		First.Terrain.TrySampleWorldCell(
			CenterCellX, CenterCellY, CenterHeight, CenterColorBand)))
	{
		MinimumWalkRingHeight = FMath::Min(MinimumWalkRingHeight, CenterHeight);
		MaximumWalkRingHeight = FMath::Max(MaximumWalkRingHeight, CenterHeight);
	}
	TestTrue(TEXT("House walking ring is a continuous level surface"),
		MaximumWalkRingHeight - MinimumWalkRingHeight <= 0.1f);
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
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(
			*World,
			FVector(0.0f, 0.0f, 3000.0f))
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
	UFragmentSimulationSubsystem* FragmentSubsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (TestNotNull(TEXT("House world owns the fragment simulation subsystem"),
		FragmentSubsystem))
	{
		AFragment2DSourceActor* InitialWallSource = nullptr;
		for (AFragment2DSourceActor* Source : HouseSources)
		{
			if (IsValid(Source)
				&& Source->SourceMaterialId == TEXT("stone")
				&& Source->GetMaskWidth() >= 8
				&& Source->GetMaskHeight() >= 8
				&& !Source->Tags.Contains(TEXT("MatterFluxHouseFurniture")))
			{
				InitialWallSource = Source;
				break;
			}
		}
		if (TestNotNull(
			TEXT("An initially spawned stone wall source is available"),
			InitialWallSource))
		{
			const float CellSize = InitialWallSource->GetCellSize();
			const FVector WallEdgeWorld =
				InitialWallSource->GetActorTransform().TransformPosition(
					FVector(
						(static_cast<float>(InitialWallSource->GetMaskWidth())
							* 0.5f - 0.5f) * CellSize,
						0.0f,
						0.0f));
			TArray<AFragment2DSourceActor*> IndexedSources;
			FragmentSubsystem->GatherSourcesInBounds(
				FBox::BuildAABB(
					WallEdgeWorld,
					FVector(1.0f)),
				IndexedSources);
			TestTrue(
				TEXT("An initially spawned house wall is immediately discoverable by world cuts"),
				IndexedSources.Contains(InitialWallSource));
		}
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
	int32 CollidableFurnitureSourceCount = 0;
	for (const AFragment2DSourceActor* Source : HouseSources)
	{
		if (!Source->Tags.Contains(TEXT("MatterFluxHouseFurniture")))
		{
			continue;
		}
		++CollidableFurnitureSourceCount;
		TestEqual(TEXT("Furniture uses physical box collision"),
			Source->MeshComponent->GetCollisionEnabled(),
			ECollisionEnabled::QueryAndPhysics);
		TestEqual(TEXT("Furniture boxes block the player capsule"),
			Source->MeshComponent->GetCollisionResponseToChannel(ECC_Pawn),
			ECR_Block);
	}
	TestTrue(TEXT("The house exposes collidable furniture sources"),
		CollidableFurnitureSourceCount >= 8);
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
		TestEqual(TEXT("The cuttable roof keeps physical collision enabled"),
			RoofSource->MeshComponent->GetCollisionEnabled(),
			ECollisionEnabled::QueryAndPhysics);
		TestEqual(TEXT("The roof blocks the player capsule"),
			RoofSource->MeshComponent->GetCollisionResponseToChannel(ECC_Pawn),
			ECR_Block);
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
		FVector(-380.0f, -365.0f, 34.0f),
		FVector(380.0f, -120.0f,
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
		if (Source->StructuralRole
			!= EMatterFluxMaterialStructuralRole::Floor)
		{
			TestFalse(*FString::Printf(
				TEXT("Non-floor house source %s stays outside stair clearance; bounds=%s"),
				*Source->GetName(),
				*Bounds.ToString()),
				Bounds.Intersect(StairClearanceWorld));
		}
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
	FMatterFluxPooledHouseStairTransformTest,
	"MatterFlux.Playable.House.PooledHouseMovesStairsWithBuilding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPooledHouseStairTransformTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(
			*World,
			FVector(120.0f, -80.0f, 25.0f))
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for pooled stair transform test"), House))
	{
		return false;
	}
	StartTestWorld(*World);
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}
	World->WorldType = EWorldType::Game;

	TArray<UInstancedStaticMeshComponent*> Groups;
	House->GetComponents(Groups);
	UInstancedStaticMeshComponent* Stairs = nullptr;
	for (UInstancedStaticMeshComponent* Group : Groups)
	{
		if (Group && Group->GetFName() == TEXT("Stairs"))
		{
			Stairs = Group;
			break;
		}
	}
	if (!TestNotNull(TEXT("House exposes its authored stair instances"), Stairs)
		|| !TestTrue(TEXT("Stair group contains an instance"),
			Stairs && Stairs->GetInstanceCount() > 0))
	{
		return false;
	}

	FTransform StairLocalTransform;
	if (!TestTrue(TEXT("First stair instance has a local transform"),
		Stairs->GetInstanceTransform(0, StairLocalTransform, false)))
	{
		return false;
	}
	const FVector RampRelativeLocation =
		House->StairRampCollision->GetRelativeLocation();
	const FTransform ReactivatedTransform(
		FRotator(0.0f, 90.0f, 0.0f),
		FVector(2500.0f, -1800.0f, 150.0f));

	House->DeactivateForStreamingPool();
	House->ReactivateFromStreamingPool(
		ReactivatedTransform,
		TEXT("structure.house.two_storey"));

	FTransform StairWorldTransform;
	if (!TestTrue(TEXT("First stair instance has a world transform after reuse"),
		Stairs->GetInstanceTransform(0, StairWorldTransform, true)))
	{
		return false;
	}
	const FVector ExpectedStairLocation = ReactivatedTransform.TransformPosition(
		StairLocalTransform.GetLocation());
	const FVector ExpectedRampLocation = ReactivatedTransform.TransformPosition(
		RampRelativeLocation);
	TestTrue(*FString::Printf(
		TEXT("Pooled stair remains inside the moved house; expected=%s actual=%s"),
		*ExpectedStairLocation.ToCompactString(),
		*StairWorldTransform.GetLocation().ToCompactString()),
		StairWorldTransform.GetLocation().Equals(ExpectedStairLocation, 0.1f));
	TestTrue(*FString::Printf(
		TEXT("Pooled stair ramp follows the moved house; expected=%s actual=%s"),
		*ExpectedRampLocation.ToCompactString(),
		*House->StairRampCollision->GetComponentLocation().ToCompactString()),
		House->StairRampCollision->GetComponentLocation().Equals(
			ExpectedRampLocation,
			0.1f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseProjectilePlaneCutTest,
	"MatterFlux.Playable.House.HorizontalAndVerticalProjectileCutsChangeWalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseProjectilePlaneCutTest::RunTest(
	const FString& Parameters)
{
	const auto RunOrientation = [this](const bool bVertical)
	{
		const TCHAR* Orientation = bVertical
			? TEXT("Vertical")
			: TEXT("Horizontal");
		UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
		AMatterFluxTwoStoreyHouseActor* House = World
			? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(
				*World,
				FVector::ZeroVector)
			: nullptr;
		if (!TestNotNull(
			*FString::Printf(TEXT("%s-cut house spawns"), Orientation),
			House))
		{
			return false;
		}
		StartTestWorld(*World);
		if (!House->HasActorBegunPlay())
		{
			House->DispatchBeginPlay();
		}
		World->WorldType = EWorldType::Game;
		House->DeactivateForStreamingPool();
		const FTransform ReactivatedTransform(
			FVector(2500.0f, -1800.0f, 150.0f));
		House->ReactivateFromStreamingPool(
			ReactivatedTransform,
			TEXT("structure.house.two_storey"));
		if (!TestTrue(
			*FString::Printf(TEXT("%s pooled house reaches its reactivated transform"), Orientation),
			House->GetActorTransform().Equals(
				ReactivatedTransform,
				0.01f)))
		{
			return false;
		}

		AFragment2DSourceActor* Target = nullptr;
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			if (It->GetOwner() == House
				&& It->SourceMaterialId == TEXT("stone")
				&& It->ActorHasTag(TEXT("MatterFluxHouseGroup.LowerWalls")))
			{
				Target = *It;
				break;
			}
		}
		if (!TestNotNull(
			*FString::Printf(TEXT("%s cut finds a lower wall Source"), Orientation),
			Target))
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
		if (!TestNotNull(
			*FString::Printf(TEXT("%s cut has a whole-object projection"), Orientation),
			WholeObjectMesh))
		{
			return false;
		}

		const auto CountTriangles = [WholeObjectMesh]()
		{
			int32 Count = 0;
			for (int32 SectionIndex = 0;
				SectionIndex < WholeObjectMesh->GetNumSections();
				++SectionIndex)
			{
				if (const FProcMeshSection* Section =
					WholeObjectMesh->GetProcMeshSection(SectionIndex))
				{
					Count += Section->ProcIndexBuffer.Num() / 3;
				}
			}
			return Count;
		};
		const auto CountSolidCells = [](const TArray<uint8>& Mask)
		{
			int32 Count = 0;
			for (const uint8 Cell : Mask)
			{
				Count += Cell != 0 ? 1 : 0;
			}
			return Count;
		};

		const TArray<uint8>& Mask = Target->GetRuntimeMask();
		const int32 Width = Target->GetMaskWidth();
		const int32 Height = Target->GetMaskHeight();
		FIntPoint ImpactCell(INDEX_NONE, INDEX_NONE);
		int32 BestCenterDistance = MAX_int32;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				if (Mask[Y * Width + X] == 0)
				{
					continue;
				}
				const int32 CenterDistance =
					FMath::Abs(2 * X + 1 - Width)
					+ FMath::Abs(2 * Y + 1 - Height);
				if (CenterDistance < BestCenterDistance)
				{
					BestCenterDistance = CenterDistance;
					ImpactCell = FIntPoint(X, Y);
				}
			}
		}
		if (!TestTrue(
			*FString::Printf(TEXT("%s cut finds a solid wall cell"), Orientation),
			ImpactCell.X != INDEX_NONE))
		{
			return false;
		}

		const float CellSize = Target->GetCellSize();
		const FVector LocalImpactCenter(
				(static_cast<float>(ImpactCell.X) + 0.5f
					- static_cast<float>(Width) * 0.5f) * CellSize,
				0.0f,
				(static_cast<float>(ImpactCell.Y) + 0.5f
					- static_cast<float>(Height) * 0.5f) * CellSize);
		const FVector TraceStart = Target->GetActorTransform().TransformPosition(
			LocalImpactCenter - FVector(0.0f, 200.0f, 0.0f));
		const FVector TraceEnd = Target->GetActorTransform().TransformPosition(
			LocalImpactCenter + FVector(0.0f, 200.0f, 0.0f));
		FHitResult SurfaceHit;
		FCollisionQueryParams QueryParams(
			TEXT("HouseProjectilePlaneCut"),
			true);
		if (!TestTrue(
			*FString::Printf(TEXT("%s projectile sweep hits the logical wall collision"), Orientation),
			Target->MeshComponent->LineTraceComponent(
				SurfaceHit,
				TraceStart,
				TraceEnd,
				QueryParams)))
		{
			return false;
		}
		FHitResult WorldHit;
		if (!TestTrue(
			*FString::Printf(TEXT("%s world sweep reaches house collision"), Orientation),
			World->LineTraceSingleByObjectType(
				WorldHit,
				TraceStart,
				TraceEnd,
				FCollisionObjectQueryParams(ECC_WorldStatic),
				QueryParams)))
		{
			return false;
		}
		if (!TestEqual(
			*FString::Printf(TEXT("%s world sweep resolves the logical wall Source"), Orientation),
			WorldHit.GetActor(),
			static_cast<AActor*>(Target)))
		{
			return false;
		}
		const FVector ProjectileForward =
			(TraceEnd - TraceStart).GetSafeNormal();
		FMatterFluxMagicProjectilePlan Plan;
		Plan.SpellId = bVertical
			? TEXT("spell.vertical_terrain_cut")
			: TEXT("spell.terrain_cut");
		Plan.Damage = 12.0f;
		Plan.Speed = 1040.0f;
		Plan.Lifetime = 0.5f;
		Plan.Radius = 60.0f;
		Plan.bUsePlaneVisual = true;
		Plan.bUseVerticalPlaneVisual = bVertical;
		AMatterFluxCharacter* Caster = SpawnAlways<AMatterFluxCharacter>(
			*World,
			WorldHit.ImpactPoint
				- ProjectileForward * 250.0f
				- FVector(0.0f, 0.0f, 25.0f));
		if (!TestNotNull(
			*FString::Printf(TEXT("%s close-range caster spawns"), Orientation),
			Caster))
		{
			return false;
		}
		Caster->SetActorRotation(ProjectileForward.Rotation());
		FMatterFluxWandCastPlan CastPlan;
		CastPlan.Projectiles.Add(Plan);
		if (!TestTrue(
			*FString::Printf(TEXT("%s close-range cast spawns its projectile"), Orientation),
			UGA_CastWand::SpawnCastPlan(
				*Caster,
				CastPlan,
				bVertical ? 8832 : 8831,
				ProjectileForward)))
		{
			return false;
		}
		AMatterFluxMagicProjectile* Projectile = nullptr;
		for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
		{
			if (It->GetPresentation().SpellId == Plan.SpellId)
			{
				Projectile = *It;
				break;
			}
		}
		if (!TestNotNull(
			*FString::Printf(TEXT("%s close-range projectile exists"), Orientation),
			Projectile))
		{
			return false;
		}
		if (!Projectile->HasActorBegunPlay())
		{
			Projectile->DispatchBeginPlay();
		}

		const int32 RevisionBefore = Target->Revision;
		const int32 SolidBefore = CountSolidCells(Target->GetRuntimeMask());
		const int32 TrianglesBefore = CountTriangles();
		for (int32 TickIndex = 0;
			TickIndex < 20 && Target->Revision == RevisionBefore;
			++TickIndex)
		{
			AdvanceTestWorld(*World, 0.02f);
		}
		TestEqual(
			*FString::Printf(TEXT("%s moving projectile advances the wall revision"), Orientation),
			Target->Revision,
			RevisionBefore + 1);
		TestTrue(
			*FString::Printf(TEXT("%s moving projectile removes wall cells"), Orientation),
			CountSolidCells(Target->GetRuntimeMask()) < SolidBefore);
		AdvanceTestWorld(*World, 0.06f);
		TestNotEqual(
			*FString::Printf(TEXT("%s projectile cut changes the visible house mesh"), Orientation),
			CountTriangles(),
			TrianglesBefore);
		return true;
	};

	const bool bHorizontalPassed = RunOrientation(false);
	const bool bVerticalPassed = RunOrientation(true);
	return bHorizontalPassed && bVerticalPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseAcidProjectileConservationTest,
	"MatterFlux.Playable.House.AcidProjectileCorrosionIsLocalAndConserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseAcidProjectileConservationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const FMatterFluxSpellDefinition* AcidSpell = Registry.IsValid()
		? Registry->Spells.Find(TEXT("spell.acid_spray"))
		: nullptr;
	if (!TestTrue(TEXT("Default registry exists"), Registry.IsValid())
		|| !TestNotNull(TEXT("Acid spray is registered"), AcidSpell))
	{
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* MaterialWorld = World
		? SpawnAlways<AMatterFluxPlayableWorldActor>(*World, FVector::ZeroVector)
		: nullptr;
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("Material world spawns for acid-wall test"), MaterialWorld)
		|| !TestNotNull(TEXT("House spawns for acid-wall test"), House))
	{
		return false;
	}
	MaterialWorld->Regenerate(1337);
	StartTestWorld(*World);
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}

	const auto CountSolidCells = [](const TArray<uint8>& Mask)
	{
		int32 Count = 0;
		for (const uint8 Cell : Mask)
		{
			Count += Cell != 0 ? 1 : 0;
		}
		return Count;
	};
	AFragment2DSourceActor* Target = nullptr;
	int32 InitialWallCells = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (It->GetOwner() != House
			|| It->SourceMaterialId != TEXT("stone")
			|| !It->ActorHasTag(TEXT("MatterFluxHouseGroup.LowerWalls")))
		{
			continue;
		}
		const int32 CandidateCells = CountSolidCells(It->GetRuntimeMask());
		if (CandidateCells > InitialWallCells)
		{
			Target = *It;
			InitialWallCells = CandidateCells;
		}
	}
	if (!TestNotNull(TEXT("Acid projectile finds a lower-storey stone wall"), Target)
		|| !TestTrue(TEXT("Selected wall has enough area to detect runaway corrosion"),
			InitialWallCells > AcidSpell->MaterialAmount * 6))
	{
		return false;
	}

	const TArray<uint8> InitialMask = Target->GetRuntimeMask();
	const int32 Width = Target->GetMaskWidth();
	const int32 Height = Target->GetMaskHeight();
	FIntPoint ImpactCell(INDEX_NONE, INDEX_NONE);
	int32 BestCenterDistance = MAX_int32;
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			if (InitialMask[Y * Width + X] == 0)
			{
				continue;
			}
			const int32 CenterDistance = FMath::Abs(2 * X + 1 - Width)
				+ FMath::Abs(2 * Y + 1 - Height);
			if (CenterDistance < BestCenterDistance)
			{
				BestCenterDistance = CenterDistance;
				ImpactCell = FIntPoint(X, Y);
			}
		}
	}
	if (!TestTrue(TEXT("Acid projectile finds a solid wall impact cell"),
		ImpactCell.X != INDEX_NONE))
	{
		return false;
	}

	const float CellSize = Target->GetCellSize();
	const FVector LocalImpactCenter(
		(static_cast<float>(ImpactCell.X) + 0.5f
			- static_cast<float>(Width) * 0.5f) * CellSize,
		0.0f,
		(static_cast<float>(ImpactCell.Y) + 0.5f
			- static_cast<float>(Height) * 0.5f) * CellSize);
	const FVector TraceStart = Target->GetActorTransform().TransformPosition(
		LocalImpactCenter - FVector(0.0f, 200.0f, 0.0f));
	const FVector TraceEnd = Target->GetActorTransform().TransformPosition(
		LocalImpactCenter + FVector(0.0f, 200.0f, 0.0f));
	FHitResult WallHit;
	FCollisionQueryParams QueryParams(TEXT("HouseAcidProjectile"), true);
	if (!TestTrue(TEXT("Acid projectile trace reaches the selected wall"),
		Target->MeshComponent->LineTraceComponent(
			WallHit,
			TraceStart,
			TraceEnd,
			QueryParams)))
	{
		return false;
	}
	const int64 InitialAcidFamilyAmount =
		MaterialWorld->GetSimulatedMaterialAmount(TEXT("acid"))
		+ MaterialWorld->GetSimulatedMaterialAmount(TEXT("acid_gas"));
	const int32 InitialAcidCells =
		MaterialWorld->GetSimulatedMaterialCount(TEXT("acid"));
	const int64 InitialAcidAmount =
		MaterialWorld->GetSimulatedMaterialAmount(TEXT("acid"));
	FMatterFluxMagicProjectilePlan Plan;
	Plan.SpellId = AcidSpell->Id;
	Plan.Damage = AcidSpell->Damage;
	Plan.Speed = AcidSpell->Speed;
	Plan.Lifetime = AcidSpell->Lifetime;
	Plan.Radius = AcidSpell->Radius;
	Plan.GravityScale = AcidSpell->GravityScale;
	Plan.BodyMaterial = AcidSpell->BodyMaterial;
	Plan.MaterialAmount = AcidSpell->MaterialAmount;
	AMatterFluxMagicProjectile* Projectile = SpawnAlways<AMatterFluxMagicProjectile>(
		*World,
		TraceStart,
		(TraceEnd - TraceStart).Rotation());
	if (!TestNotNull(TEXT("Acid material projectile spawns"), Projectile))
	{
		return false;
	}
	Projectile->InitializeProjectile(Plan, 62491);
	TestTrue(TEXT("Acid material projectile resolves its wall impact"),
		Projectile->ResolveImpactAuthority(WallHit));
	const int32 DepositedAcidCells =
		MaterialWorld->GetSimulatedMaterialCount(TEXT("acid"))
			- InitialAcidCells;
	TestTrue(TEXT("Acid projectile deposits a bounded material patch at the wall"),
		DepositedAcidCells > 0
			&& DepositedAcidCells <= AcidSpell->MaterialAmount);
	TestTrue(TEXT("Acid projectile adds positive conserved material volume"),
		MaterialWorld->GetSimulatedMaterialAmount(TEXT("acid"))
			> InitialAcidAmount);
	const int64 DepositedAcidAmount =
		MaterialWorld->GetSimulatedMaterialAmount(TEXT("acid"))
			- InitialAcidAmount;
	// Drive the material authority directly once so the impact contact is
	// resolved deterministically before the synthetic world begins advancing.
	MaterialWorld->Tick(0.0f);
	TestTrue(TEXT("Impact contact consumes acid before material flow"),
		MaterialWorld->GetSimulatedMaterialAmount(TEXT("acid"))
			< InitialAcidAmount + DepositedAcidAmount);

	for (int32 Frame = 0; Frame < 24; ++Frame)
	{
		AdvanceTestWorld(*World, 0.1f);
	}
	const int32 RemainingWallCells = IsValid(Target)
		? CountSolidCells(Target->GetRuntimeMask())
		: 0;
	const int32 RemovedWallCells = InitialWallCells - RemainingWallCells;
	AddInfo(FString::Printf(
		TEXT("Acid wall locality: initial=%d removed=%d remaining=%d payload_voxels=%d"),
		InitialWallCells,
		RemovedWallCells,
		RemainingWallCells,
		AcidSpell->MaterialAmount));
	TestTrue(*FString::Printf(
		TEXT("One acid shot remains local instead of dissolving the wall; initial=%d removed=%d"),
		InitialWallCells,
		RemovedWallCells),
		RemovedWallCells > 0
			&& RemovedWallCells <= AcidSpell->MaterialAmount * 3
			&& RemainingWallCells > InitialWallCells / 2);

	int32 UnchangedFarCells = 0;
	int32 InitialFarCells = 0;
	if (IsValid(Target))
	{
		const TArray<uint8>& FinalMask = Target->GetRuntimeMask();
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 Index = Y * Width + X;
				if (InitialMask[Index] == 0
					|| FMath::Abs(X - ImpactCell.X) <= 3)
				{
					continue;
				}
				++InitialFarCells;
				UnchangedFarCells += FinalMask.IsValidIndex(Index)
					&& FinalMask[Index] != 0 ? 1 : 0;
			}
		}
	}
	TestTrue(TEXT("Wall cells away from the acid impact remain intact"),
		InitialFarCells > 0 && UnchangedFarCells == InitialFarCells);
	const int64 FinalAcidFamilyAmount =
		MaterialWorld->GetSimulatedMaterialAmount(TEXT("acid"))
		+ MaterialWorld->GetSimulatedMaterialAmount(TEXT("acid_gas"));
	AddInfo(FString::Printf(
		TEXT("Acid family conservation: before=%lld after=%lld deposited=%lld"),
		InitialAcidFamilyAmount,
		FinalAcidFamilyAmount,
		DepositedAcidAmount));
	TestTrue(*FString::Printf(
		TEXT("Acid-family amount never exceeds the deposited payload; before=%lld after=%lld deposited=%lld"),
		InitialAcidFamilyAmount,
		FinalAcidFamilyAmount,
		DepositedAcidAmount),
		FinalAcidFamilyAmount - InitialAcidFamilyAmount
			<= DepositedAcidAmount);
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
	const FVector TableTopLocal(-115.0f, 120.0f, 118.0f);
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
	const auto GetCanonicalSourceBounds = [](
		const AFragment2DSourceActor& Source)
	{
		const float CellSize = Source.GetCellSize();
		const FVector HalfExtent(
			static_cast<float>(Source.GetMaskWidth()) * CellSize * 0.5f,
			CellSize * 0.5f,
			static_cast<float>(Source.GetMaskHeight()) * CellSize * 0.5f);
		return FBox(-HalfExtent, HalfExtent).TransformBy(
			Source.GetActorTransform().ToMatrixWithScale());
	};

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
		// House source actors deliberately disable their individual projections
		// after the canonical WholeObject mesh is built. Reconstruct bounds from
		// the authoritative mask layout and extrusion transform instead of the
		// empty component/visible-mask bounds exposed by NullRHI.
		const FBox LowerBounds = GetCanonicalSourceBounds(*MatchingLower);
		const FBox UpperBounds = GetCanonicalSourceBounds(*Upper);
		TestTrue(*FString::Printf(
			TEXT("Wall envelope is continuous through the floor band; gap=%.2f"),
			UpperBounds.Min.Z - LowerBounds.Max.Z),
			UpperBounds.Min.Z <= LowerBounds.Max.Z + 0.5f);
	}

	const auto MeasureFacadeGap = [&GetCanonicalSourceBounds](
		const TArray<AFragment2DSourceActor*>& Walls,
		const float SampleZ,
		const bool bWestFacade)
	{
		TArray<FVector2D> Intervals;
		for (const AFragment2DSourceActor* Wall : Walls)
		{
			const FBox Bounds = GetCanonicalSourceBounds(*Wall);
			if (SampleZ < Bounds.Min.Z - 0.5f
				|| SampleZ > Bounds.Max.Z + 0.5f)
			{
				continue;
			}
			// Source masks are snapped outward to the shared 17 cm voxel grid,
			// so their rendered bounds intentionally do not land on the design
			// envelope exactly. Classify the facade by its canonical transform;
			// continue measuring coverage from the actual projected bounds.
			const FVector WallLocation = Wall->GetActorLocation();
			const bool bOnFacade = bWestFacade
				? WallLocation.X <
					-AMatterFluxTwoStoreyHouseActor::HalfSizeX + 80.0f
				: WallLocation.Y >
					AMatterFluxTwoStoreyHouseActor::HalfSizeY - 80.0f;
			if (bOnFacade)
			{
				Intervals.Add(bWestFacade
					? FVector2D(Bounds.Min.Y, Bounds.Max.Y)
					: FVector2D(Bounds.Min.X, Bounds.Max.X));
			}
		}
		Intervals.Sort([](const FVector2D& Left, const FVector2D& Right)
		{
			return Left.X < Right.X;
		});

		if (Intervals.IsEmpty())
		{
			return bWestFacade
				? AMatterFluxTwoStoreyHouseActor::HalfSizeY * 2.0f
				: AMatterFluxTwoStoreyHouseActor::HalfSizeX * 2.0f;
		}
		// Perpendicular walls and corner posts close the facade ends. This gate
		// is specifically for holes between coplanar wall pieces, so do not
		// mistake voxel-grid inset at the design envelope for an internal seam.
		float Cursor = Intervals[0].Y;
		float MaximumGap = 0.0f;
		for (int32 IntervalIndex = 1;
			IntervalIndex < Intervals.Num(); ++IntervalIndex)
		{
			const FVector2D& Interval = Intervals[IntervalIndex];
			MaximumGap = FMath::Max(MaximumGap, Interval.X - Cursor);
			Cursor = FMath::Max(Cursor, Interval.Y);
		}
		return MaximumGap;
	};

	const float GroundTopBandZ = House->GetFloorSurfaceWorldZ(0)
		+ AMatterFluxTwoStoreyHouseActor::StoreyHeight - 60.0f;
	const float UpperTopBandZ = House->GetFloorSurfaceWorldZ(1)
		+ AMatterFluxTwoStoreyHouseActor::StoreyHeight - 60.0f;
	for (const auto& Storey : {
		TTuple<const TCHAR*, const TArray<AFragment2DSourceActor*>*, float>(
			TEXT("ground"), &LowerWalls, GroundTopBandZ),
		TTuple<const TCHAR*, const TArray<AFragment2DSourceActor*>*, float>(
			TEXT("upper"), &UpperWalls, UpperTopBandZ) })
	{
		const float WestGap = MeasureFacadeGap(
			*Storey.Get<1>(), Storey.Get<2>(), true);
		TestTrue(*FString::Printf(
			TEXT("%s-storey west facade top band is watertight; gap=%.2f"),
			Storey.Get<0>(), WestGap),
			WestGap <= 0.5f);
		const float NorthGap = MeasureFacadeGap(
			*Storey.Get<1>(), Storey.Get<2>(), false);
		TestTrue(*FString::Printf(
			TEXT("%s-storey north facade top band is watertight; gap=%.2f"),
			Storey.Get<0>(), NorthGap),
			NorthGap <= 0.5f);
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
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}

	const float GroundZ = House->GetFloorSurfaceWorldZ(0);
	const float UpperZ = House->GetFloorSurfaceWorldZ(1);
	AMatterFluxCharacter* Viewer = SpawnAlways<AMatterFluxCharacter>(
		*World, FVector(-180.0f, 0.0f, GroundZ + 88.0f));
	AMatterFluxCreatureActor* Creature =
		SpawnAlways<AMatterFluxCreatureActor>(
			*World, FVector(210.0f, 0.0f, UpperZ + 80.0f));
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
	TestEqual(TEXT("Furniture above the viewer is not wall material"),
		House->GetFurnitureOpacity(1), 1.0f);
	TestEqual(TEXT("Cutaway starts from the current solid opacity"),
		House->GetCurrentStructureOpacity(), 1.0f);
	House->Tick(0.05f);
	TestTrue(TEXT("Connected wall material begins fading without a snap"),
		House->GetCurrentStructureOpacity() < 1.0f
			&& House->GetCurrentStructureOpacity() > 0.24f);
	TestEqual(TEXT("Viewer's ground-floor slab remains solid"),
		House->GetFloorSurfaceOpacity(0), 1.0f);
	TestTrue(TEXT("Upper floor begins fading with the structure above the viewer"),
		House->GetFloorSurfaceOpacity(1) < 1.0f
			&& House->GetFloorSurfaceOpacity(1) > 0.24f);
	TestEqual(TEXT("Upper-floor creature is not wall material"),
		House->GetTrackedInteriorActorCount(), 0);

	for (int32 FadeTick = 0; FadeTick < 20; ++FadeTick)
	{
		House->Tick(0.05f);
	}
	TestTrue(TEXT("Upper floor settles at a readable ghost opacity above the viewer"),
		House->GetFloorSurfaceOpacity(1) < 0.20f
			&& House->GetFloorSurfaceOpacity(1) > 0.04f);
	const auto VerifyIndoorWalkSample = [this, House, Viewer](
		const TCHAR* FloorName,
		const int32 FloorIndex,
		const FVector& Location)
	{
		Viewer->SetActorLocation(Location);
		House->RefreshCutawayImmediately();
		for (int32 FadeTick = 0; FadeTick < 20; ++FadeTick)
		{
			House->Tick(0.05f);
		}
		TestEqual(*FString::Printf(
			TEXT("Walking on the %s floor keeps its cutaway selected"), FloorName),
			House->GetCurrentCutawayFloor(), FloorIndex);
		TestTrue(*FString::Printf(
			TEXT("Walking on the %s floor keeps connected walls ghosted"), FloorName),
			House->GetCurrentStructureOpacity() <= 0.06f);
		TestEqual(*FString::Printf(
			TEXT("Walking on the %s floor keeps the ground slab opaque"), FloorName),
			House->GetFloorSurfaceOpacity(0), 1.0f);
		if (FloorIndex == 0)
		{
			TestTrue(*FString::Printf(
				TEXT("Walking on the %s floor keeps the upper slab ghosted"), FloorName),
				House->GetFloorSurfaceOpacity(1) < 0.20f
					&& House->GetFloorSurfaceOpacity(1) > 0.04f);
		}
		else
		{
			TestEqual(*FString::Printf(
				TEXT("Walking on the %s floor restores its slab"), FloorName),
				House->GetFloorSurfaceOpacity(1), 1.0f);
		}
	};

	for (const FVector2D GroundWalk : {
		FVector2D(-180.0f, 0.0f),
		FVector2D(220.0f, 180.0f),
		FVector2D(350.0f, -120.0f) })
	{
		VerifyIndoorWalkSample(TEXT("ground"), 0,
			FVector(GroundWalk, GroundZ + 88.0f));
	}
	for (const FVector2D UpperWalk : {
		FVector2D(-180.0f, 0.0f),
		FVector2D(220.0f, 160.0f),
		FVector2D(-320.0f, -100.0f) })
	{
		VerifyIndoorWalkSample(TEXT("upper"), 1,
			FVector(UpperWalk, UpperZ + 88.0f));
	}
	VerifyIndoorWalkSample(TEXT("ground after returning"), 0,
		FVector(-180.0f, 0.0f, GroundZ + 88.0f));
	const auto VerifyAirborneCutaway = [this, House, Viewer](
		const TCHAR* FloorName,
		const int32 FloorIndex,
		const FVector& TakeoffLocation)
	{
		Viewer->SetActorLocation(TakeoffLocation);
		House->RefreshCutawayImmediately();
		float PreviousSlabOpacity =
			House->GetFloorSurfaceOpacity(FloorIndex);
		for (int32 AirborneFrame = 0; AirborneFrame < 10; ++AirborneFrame)
		{
			Viewer->SetActorLocation(
				TakeoffLocation + FVector(0.0f, 0.0f,
					FMath::Sin((AirborneFrame + 1) * PI / 11.0f) * 155.0f));
			House->Tick(0.05f);
			TestEqual(*FString::Printf(
				TEXT("%s-floor jump frame %d keeps its cutaway selected"),
				FloorName, AirborneFrame),
				House->GetCurrentCutawayFloor(), FloorIndex);
			TestTrue(*FString::Printf(
				TEXT("%s-floor jump frame %d keeps walls ghosted"),
				FloorName, AirborneFrame),
				House->GetCurrentStructureOpacity() <= 0.06f);
			const float SlabOpacity =
				House->GetFloorSurfaceOpacity(FloorIndex);
			if (FloorIndex == 0)
			{
				TestEqual(*FString::Printf(
					TEXT("%s-floor jump frame %d keeps its slab opaque"),
					FloorName, AirborneFrame), SlabOpacity, 1.0f);
			}
			else
			{
				TestTrue(*FString::Printf(
					TEXT("%s-floor jump frame %d fades its slab back in"),
					FloorName, AirborneFrame),
					SlabOpacity + KINDA_SMALL_NUMBER >= PreviousSlabOpacity);
			}
			PreviousSlabOpacity = SlabOpacity;
		}
		if (FloorIndex == 1)
		{
			TestTrue(TEXT("Upper slab finishes fading in after landing"),
				House->GetFloorSurfaceOpacity(1) > 0.95f);
		}
	};
	VerifyAirborneCutaway(TEXT("ground"), 0,
		FVector(-180.0f, 0.0f, GroundZ + 88.0f));
	VerifyAirborneCutaway(TEXT("upper"), 1,
		FVector(220.0f, 160.0f, UpperZ + 88.0f));
	VerifyIndoorWalkSample(TEXT("ground after airborne checks"), 0,
		FVector(-180.0f, 0.0f, GroundZ + 88.0f));
	TestEqual(TEXT("Upper-floor furniture remains visible beside the viewer"),
		House->GetFurnitureOpacity(1), 1.0f);
	TestEqual(TEXT("Same-floor creature remains fully visible"),
		House->GetTrackedInteriorActorCount(), 0);

	Viewer->SetActorLocation(FVector(
		0.0f,
		AMatterFluxTwoStoreyHouseActor::HalfSizeY + 200.0f,
		GroundZ + 88.0f));
	House->RefreshCutawayImmediately();
	TestEqual(TEXT("Standing just outside the house clears local-floor cutaway"),
		House->GetCurrentCutawayFloor(), INDEX_NONE);
	const float ExitStartOpacity = House->GetCurrentStructureOpacity();
	TestTrue(TEXT("Restored house does not jump directly to opaque"),
		ExitStartOpacity < 1.0f);
	House->Tick(0.05f);
	TestTrue(TEXT("Restored house fades toward opaque"),
		House->GetCurrentStructureOpacity() > ExitStartOpacity
			&& House->GetCurrentStructureOpacity() < 1.0f);
	TestEqual(TEXT("Creature materials are restored after leaving"),
		House->GetTrackedInteriorActorCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseCutawayWallDemolitionTest,
	"MatterFlux.Playable.House.IndoorCutawaySurvivesWallDemolition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseCutawayWallDemolitionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for demolition cutaway"), House))
	{
		return false;
	}
	StartTestWorld(*World);
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}

	const float GroundZ = House->GetFloorSurfaceWorldZ(0);
	AMatterFluxCharacter* Viewer = SpawnAlways<AMatterFluxCharacter>(
		*World, FVector(-180.0f, 0.0f, GroundZ + 88.0f));
	if (!TestNotNull(TEXT("Indoor viewer spawns"), Viewer))
	{
		return false;
	}

	TArray<AFragment2DSourceActor*> LiveWalls;
	TArray<AFragment2DSourceActor*> StructureSources;
	AFragment2DSourceActor* WallToDemolish = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		AFragment2DSourceActor* Source = *It;
		if (Source->GetOwner() != House)
		{
			continue;
		}
		StructureSources.Add(Source);
		if (Source->StructuralRole
				!= EMatterFluxMaterialStructuralRole::Wall
			|| Source->bBroken)
		{
			continue;
		}
		LiveWalls.Add(Source);
		if (!WallToDemolish
			&& Source->ActorHasTag(TEXT("MatterFluxHouseGroup.LowerWalls")))
		{
			WallToDemolish = Source;
		}
	}
	if (!TestTrue(TEXT("House owns live wall sources"), !LiveWalls.IsEmpty())
		|| !TestNotNull(TEXT("A lower wall can be demolished"), WallToDemolish))
	{
		return false;
	}

	House->SetCutawayViewerOverride(Viewer);
	House->RefreshCutawayImmediately();
	constexpr float FadeTickSeconds = 0.05f;
	for (int32 FadeTick = 0; FadeTick < 20; ++FadeTick)
	{
		House->Tick(FadeTickSeconds);
	}

	MatterFlux::MaterialCutaway::FResult InitialCutaway;
	TestTrue(TEXT("Canonical indoor cutaway resolves before demolition"),
		MatterFlux::MaterialCutaway::Resolve(
			FVector(Viewer->GetActorLocation().X,
				Viewer->GetActorLocation().Y,
				CharacterFeetZ(*Viewer)),
			StructureSources,
			FGuid(),
			InitialCutaway));
	int32 GhostedWallCount = 0;
	for (const AFragment2DSourceActor* Wall : LiveWalls)
	{
		GhostedWallCount += InitialCutaway.GhostSourceIds.Contains(Wall->SourceId)
			? 1 : 0;
	}
	TestEqual(TEXT("Every live wall belongs to the indoor cutaway"),
		GhostedWallCount, LiveWalls.Num());
	TestTrue(TEXT("Indoor wall presentation preserves the authored house cutaway"),
		House->GetCurrentStructureOpacity() <= 0.06f);

	const FGuid DemolishedSourceId = WallToDemolish->SourceId;
	UProceduralMeshComponent* WholeObjectProjection =
		House->FindComponentByClass<UProceduralMeshComponent>();
	if (!TestNotNull(TEXT("House owns a merged cuttable projection"),
		WholeObjectProjection))
	{
		return false;
	}
	const FBox DemolishedWallBounds =
		WallToDemolish->GetComponentsBoundingBox(true);
	const TSet<uint32> ProjectedTrianglesBeforeDemolition =
		CollectProjectionTriangleKeysInsideBounds(
			*WholeObjectProjection, DemolishedWallBounds);
	TestTrue(TEXT("Wall selected for demolition owns visible projection geometry"),
		!ProjectedTrianglesBeforeDemolition.IsEmpty());
	WallToDemolish->MarkBroken();
	for (int32 FadeTick = 0; FadeTick < 20; ++FadeTick)
	{
		House->Tick(FadeTickSeconds);
	}
	TestEqual(TEXT("Demolished wall source collision is disabled"),
		WallToDemolish->MeshComponent->GetCollisionEnabled(),
		ECollisionEnabled::NoCollision);
	const TSet<uint32> ProjectedTrianglesAfterDemolition =
		CollectProjectionTriangleKeysInsideBounds(
			*WholeObjectProjection, DemolishedWallBounds);
	int32 RemovedTriangleCount = 0;
	for (const uint32 TriangleKey : ProjectedTrianglesBeforeDemolition)
	{
		RemovedTriangleCount +=
			ProjectedTrianglesAfterDemolition.Contains(TriangleKey) ? 0 : 1;
	}
	TestTrue(TEXT("Demolished wall is removed from the merged projection"),
		RemovedTriangleCount > 0);

	MatterFlux::MaterialCutaway::FResult RebuiltCutaway;
	TestTrue(TEXT("Canonical indoor cutaway resolves after demolition"),
		MatterFlux::MaterialCutaway::Resolve(
			FVector(Viewer->GetActorLocation().X,
				Viewer->GetActorLocation().Y,
				CharacterFeetZ(*Viewer)),
			StructureSources,
			FGuid(),
			RebuiltCutaway));
	TestFalse(TEXT("Demolished wall leaves the active ghost set"),
		RebuiltCutaway.GhostSourceIds.Contains(DemolishedSourceId));

	int32 RemainingWallCount = 0;
	int32 RemainingGhostedCount = 0;
	for (const AFragment2DSourceActor* Wall : LiveWalls)
	{
		if (Wall->bBroken)
		{
			continue;
		}
		++RemainingWallCount;
		RemainingGhostedCount +=
			RebuiltCutaway.GhostSourceIds.Contains(Wall->SourceId) ? 1 : 0;
	}
	TestEqual(TEXT("All remaining walls still belong to the cutaway"),
		RemainingGhostedCount, RemainingWallCount);
	TestTrue(TEXT("Remaining wall presentation returns to the authored target"),
		House->GetCurrentStructureOpacity() <= 0.06f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseExteriorOcclusionTest,
	"MatterFlux.Playable.House.ExteriorViewerOccludedByHouseFadesStructure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseExteriorOcclusionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for exterior occlusion"), House))
	{
		return false;
	}
	StartTestWorld(*World);
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}

	const float GroundZ = House->GetFloorSurfaceWorldZ(0);
	AMatterFluxCharacter* Viewer = SpawnAlways<AMatterFluxCharacter>(
		*World, FVector(900.0f, 0.0f, GroundZ + 88.0f));
	if (!TestNotNull(TEXT("Exterior viewer spawns"), Viewer)
		|| !TestNotNull(
			TEXT("Viewer has a follow camera"), Viewer->FollowCamera.Get()))
	{
		return false;
	}

	Viewer->FollowCamera->SetWorldLocation(FVector(
		700.0f, 0.0f, Viewer->GetActorLocation().Z));
	const FVector SameSideCameraLocation =
		Viewer->FollowCamera->GetComponentLocation();
	TestTrue(TEXT("Viewer is outside the east wall"),
		Viewer->GetActorLocation().X > AMatterFluxTwoStoreyHouseActor::HalfSizeX);
	TestTrue(TEXT("Same-side camera is outside the east wall"),
		SameSideCameraLocation.X > AMatterFluxTwoStoreyHouseActor::HalfSizeX);
	House->SetCutawayViewerOverride(Viewer);
	House->RefreshCutawayImmediately();
	House->Tick(0.05f);
	TestEqual(TEXT("An exterior house that does not block the viewer stays solid"),
		House->GetCurrentStructureOpacity(), 1.0f);
	TestEqual(TEXT("The ground floor stays solid when the house does not occlude the viewer"),
		House->GetFloorSurfaceOpacity(0), 1.0f);
	TestEqual(TEXT("The upper floor stays solid when the house does not occlude the viewer"),
		House->GetFloorSurfaceOpacity(1), 1.0f);

	Viewer->FollowCamera->SetWorldLocation(FVector(
		-1200.0f, 0.0f, Viewer->GetActorLocation().Z));
	const FVector CameraLocation = Viewer->FollowCamera->GetComponentLocation();
	TestTrue(TEXT("Camera is outside the west wall"),
		CameraLocation.X < -AMatterFluxTwoStoreyHouseActor::HalfSizeX);
	TestTrue(TEXT("Camera-to-viewer line crosses the house footprint"),
		FMath::Abs(CameraLocation.Y) < AMatterFluxTwoStoreyHouseActor::HalfSizeY
			&& FMath::Abs(Viewer->GetActorLocation().Y)
				< AMatterFluxTwoStoreyHouseActor::HalfSizeY);

	House->RefreshCutawayImmediately();
	TestEqual(TEXT("Exterior occlusion does not pretend the viewer is indoors"),
		House->GetCurrentCutawayFloor(), INDEX_NONE);
	House->Tick(0.05f);
	TestTrue(TEXT("A house between camera and exterior viewer begins fading"),
		House->GetCurrentStructureOpacity() < 1.0f);
	TestTrue(TEXT("The ground floor fades with an exterior-occluding house"),
		House->GetFloorSurfaceOpacity(0) < 1.0f);
	TestTrue(TEXT("The upper floor fades with an exterior-occluding house"),
		House->GetFloorSurfaceOpacity(1) < 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHouseCutawayBoundaryJitterTest,
	"MatterFlux.Playable.House.UpperFloorCutawayRejectsSingleFrameBoundaryJitter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHouseCutawayBoundaryJitterTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxTwoStoreyHouseActor* House = World
		? SpawnAlways<AMatterFluxTwoStoreyHouseActor>(*World, FVector::ZeroVector)
		: nullptr;
	if (!TestNotNull(TEXT("House spawns for cutaway stability test"), House))
	{
		return false;
	}
	StartTestWorld(*World);
	if (!House->HasActorBegunPlay())
	{
		House->DispatchBeginPlay();
	}

	const float UpperZ = House->GetFloorSurfaceWorldZ(1);
	AMatterFluxCharacter* Viewer = SpawnAlways<AMatterFluxCharacter>(
		*World, FVector(480.0f, 0.0f, UpperZ + 88.0f));
	if (!TestNotNull(TEXT("Viewer spawns on the upper floor"), Viewer))
	{
		return false;
	}

	House->SetCutawayViewerOverride(Viewer);
	House->RefreshCutawayImmediately();
	TestEqual(TEXT("Upper-floor cutaway starts active"),
		House->GetCurrentCutawayFloor(), 1);
	constexpr float HouseTickSeconds = 0.05f;
	for (int32 FadeTick = 0; FadeTick < 8; ++FadeTick)
	{
		House->Tick(HouseTickSeconds);
	}
	const float StableOpacity = House->GetCurrentStructureOpacity();

	// CharacterMovement can produce a one-frame depenetration / floor-edge
	// correction close to an exterior wall.  A single ambiguous sample must not
	// restore the whole facade and flash it opaque before the capsule settles.
	Viewer->SetActorLocation(FVector(515.0f, 0.0f, UpperZ + 88.0f));
	House->Tick(HouseTickSeconds);
	TestEqual(TEXT("One boundary sample preserves the last stable floor"),
		House->GetCurrentCutawayFloor(), 1);
	TestTrue(TEXT("One boundary sample does not brighten the facade"),
		House->GetCurrentStructureOpacity() <= StableOpacity);

	Viewer->SetActorLocation(FVector(480.0f, 0.0f, UpperZ + 88.0f));
	House->Tick(HouseTickSeconds);
	TestEqual(TEXT("Returning inside keeps the upper cutaway stable"),
		House->GetCurrentCutawayFloor(), 1);

	Viewer->SetActorLocation(FVector(1400.0f, 0.0f, UpperZ + 88.0f));
	for (int32 Frame = 0; Frame < 4; ++Frame)
	{
		House->Tick(HouseTickSeconds);
	}
	TestEqual(TEXT("Sustained movement away still restores the facade"),
		House->GetCurrentCutawayFloor(), INDEX_NONE);
	TestTrue(TEXT("Confirmed exit starts restoring structure opacity"),
		House->GetCurrentStructureOpacity() > StableOpacity);
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
	const float StairY = House->StairRampCollision->GetRelativeLocation().Y;
	TestTrue(TEXT("The staircase occupies the rear half of the house"),
		StairY < 0.0f);
	const float GroundZ = House->GetFloorSurfaceWorldZ(0);
	const float UpperZ = House->GetFloorSurfaceWorldZ(1);

	// 三条竖直射线必须依次命中越来越高的连续坡面；可见台阶本身不
	// 承担碰撞，所以这里同时防止日后重新引入逐级抖动。
	float PreviousHitZ = -TNumericLimits<float>::Max();
	for (const float X : {-260.0f, 0.0f, 260.0f})
	{
		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByChannel(
			Hit,
			FVector(X, StairY, 620.0f),
			FVector(X, StairY, -80.0f),
			ECC_Visibility);
		TestTrue(TEXT("A stair sample hits walkable collision"), bHit);
		if (bHit)
		{
			TestTrue(TEXT("Stair collision rises monotonically"),
				Hit.ImpactPoint.Z > PreviousHitZ + 80.0f);
			PreviousHitZ = Hit.ImpactPoint.Z;
		}
	}
	FHitResult UpperLandingHit;
	const bool bUpperLandingHit = World->LineTraceSingleByChannel(
		UpperLandingHit,
		FVector(330.0f, StairY, UpperZ + 80.0f),
		FVector(330.0f, StairY, UpperZ - 100.0f),
		ECC_Visibility);
	TestTrue(TEXT("The stair ramp reaches its upper landing"),
		bUpperLandingHit);
	if (bUpperLandingHit)
	{
		TestTrue(TEXT("The last stair is within walking tolerance of the upper floor"),
			UpperLandingHit.ImpactPoint.Z >= UpperZ - 12.0f);
	}

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
			FVector(X, StairY, 620.0f),
			FVector(X, StairY, -80.0f),
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
			X, StairY, FloorHit.ImpactPoint.Z + 100.0f);
		const FVector CreatureCenter(
			X, StairY, FloorHit.ImpactPoint.Z + 86.0f);
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
