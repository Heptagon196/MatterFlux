#include "Fragment/Fragment2DAsset.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DDamageRequestActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Fragment/FragmentSourceSpatialIndex.h"
#include "FragmentTestActors.h"
#include "IMatterFluxScriptRuntime.h"
#include "Algo/Count.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Material/MatterFluxBuoyancyComponent.h"
#include "Misc/AutomationTest.h"
#include "ProceduralMeshComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "Tests/AutomationEditorCommon.h"

#include <limits>

namespace
{
	FFragmentDamageEvent MakeCircleEvent(const AFragment2DSourceActor& Source, const FVector& WorldCenter, const float Radius)
	{
		FFragmentDamageEvent Event;
		Event.SourceId = Source.SourceId;
		Event.BaseRevision = Source.Revision;
		Event.DamageShape.Type = EFragmentDamageShapeType::Circle;
		Event.DamageShape.WorldTransform = FTransform(WorldCenter);
		Event.DamageShape.Radius = Radius;
		Event.EventSeed = 17;
		return Event;
	}

	AFragment2DSourceActor* SpawnSupportedColumn(
		UWorld& World)
	{
		AFragment2DSourceActor* Source =
			World.SpawnActor<AFragment2DSourceActor>();
		if (!Source)
		{
			return nullptr;
		}
		FFragmentSourceMask Mask;
		Mask.Width = 3;
		Mask.Height = 7;
		Mask.CellSize = 10.0f;
		Mask.MinFragmentAreaPixels = 1;
		Mask.MaxFragmentsPerBreak = 4;
		Mask.SupportMode = EFragmentSupportMode::Bottom;
		Mask.SolidMask.Init(1, Mask.Width * Mask.Height);
		return Source->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(
				TEXT("SupportedColumn"),
				1),
			FLinearColor::White,
			TEXT("wood"))
			? Source
			: nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSupportedRemainderTest,
	"MatterFlux.Fragment.Support.SupportedRemainderStaysStatic",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSupportedRemainderTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? SpawnSupportedColumn(*World)
		: nullptr;
	if (!TestNotNull(TEXT("Supported source spawns"), Source))
	{
		return false;
	}

	FFragmentDamageEvent Event;
	Event.SourceId = Source->SourceId;
	Event.BaseRevision = Source->Revision;
	Event.DamageShape.Type = EFragmentDamageShapeType::Line;
	Event.DamageShape.WorldTransform =
		Source->GetActorTransform();
	Event.DamageShape.Extents.X = 100.0;
	Event.DamageShape.Thickness = 9.0f;
	Event.DamagePower = 0.0f;
	Event.EventSeed = 901;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem))
	{
		return false;
	}
	TestTrue(
		TEXT("Separating cut commits"),
		Subsystem->RequestFragmentDamage(Source, Event));

	int32 DynamicCount = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		++DynamicCount;
		TestEqual(
			TEXT("Detached entity has physics collision"),
			It->MeshComponent->GetCollisionEnabled(),
			ECollisionEnabled::QueryAndPhysics);
	}
	TestEqual(
		TEXT("Only the unsupported upper component detaches"),
		DynamicCount,
		1);
	TestFalse(
		TEXT("Ground-supported remainder is not broken"),
		Source->bBroken);
	TestFalse(
		TEXT("Ground-supported remainder remains visible"),
		Source->IsHidden());
	TestEqual(
		TEXT("Cut commits one source revision"),
		Source->Revision,
		1);
	TestEqual(
		TEXT("Only three supported rows remain"),
		static_cast<int32>(Algo::Count(Source->GetRuntimeMask(), static_cast<uint8>(1))),
		9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxNonCollidingSourceProducesNonCollidingFragmentsTest,
	"MatterFlux.Fragment.Collision.NonCollidingSourceProducesNonCollidingFragments",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxNonCollidingSourceProducesNonCollidingFragmentsTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? SpawnSupportedColumn(*World)
		: nullptr;
	if (!TestNotNull(TEXT("Non-colliding source spawns"), Source))
	{
		return false;
	}
	Source->SetSourceCollisionEnabled(false);

	FFragmentDamageEvent Event;
	Event.SourceId = Source->SourceId;
	Event.BaseRevision = Source->Revision;
	Event.DamageShape.Type = EFragmentDamageShapeType::Line;
	Event.DamageShape.WorldTransform = Source->GetActorTransform();
	Event.DamageShape.Extents.X = 100.0;
	Event.DamageShape.Thickness = 9.0f;
	Event.DamagePower = 600.0f;
	Event.EventSeed = 902;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem))
	{
		return false;
	}
	TestTrue(
		TEXT("Separating a non-colliding source still commits"),
		Subsystem->RequestFragmentDamage(Source, Event));

	int32 FragmentCount = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		++FragmentCount;
		TestFalse(
			TEXT("Fragment payload preserves the source collision policy"),
			It->SpawnPayload.bEnableCollision);
		TestTrue(
			TEXT("Render-only payload omits unused convex hulls"),
			It->SpawnPayload.CollisionContours.IsEmpty());
		TestEqual(
			TEXT("Non-colliding decoration debris has no collision"),
			It->MeshComponent->GetCollisionEnabled(),
			ECollisionEnabled::NoCollision);
		TestFalse(
			TEXT("Non-colliding decoration debris does not create a Chaos body"),
			It->MeshComponent->IsSimulatingPhysics());
	}
	TestEqual(TEXT("One unsupported component detaches"), FragmentCount, 1);
	TestFalse(
		TEXT("Non-colliding source rebuild omits procedural tri-mesh collision data"),
		Source->MeshComponent->ContainsPhysicsTriMeshData(true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaDetachedAreaThresholdTest,
	"MatterFlux.Fragment.Support.LuaMinimumFadesTinyDetachedComponent",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaDetachedAreaThresholdTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Default engine settings load"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Narrow supported source spawns"), Source))
	{
		return false;
	}
	FFragmentSourceMask Mask;
	Mask.Width = 1;
	Mask.Height = 7;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SupportMode = EFragmentSupportMode::Bottom;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);
	if (!TestTrue(
		TEXT("Narrow source initializes"),
		Source->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(
				TEXT("LuaThresholdColumn"),
				1),
			FLinearColor::White,
			TEXT("wood"))))
	{
		return false;
	}

	FFragmentDamageEvent Event;
	Event.SourceId = Source->SourceId;
	Event.BaseRevision = Source->Revision;
	Event.DamageShape.Type = EFragmentDamageShapeType::Line;
	Event.DamageShape.WorldTransform = Source->GetActorTransform();
	Event.DamageShape.Extents.X = 100.0;
	Event.DamageShape.Thickness = 9.0f;
	Event.DamagePower = 0.0f;
	Event.EventSeed = 902;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem))
	{
		return false;
	}
	TestTrue(
		TEXT("Cut with tiny unsupported part still commits"),
		Subsystem->RequestFragmentDamage(Source, Event));

	int32 DynamicCount = 0;
	AFragment2DActor* TinyDebris = nullptr;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		TinyDebris = *It;
		++DynamicCount;
	}
	TestEqual(
		TEXT("Three-cell detached part uses one bounded fade carrier"),
		DynamicCount,
		1);
	if (TestNotNull(TEXT("Tiny Lua-threshold debris remains visible briefly"), TinyDebris))
	{
		TestTrue(TEXT("Tiny Lua-threshold debris fades out"),
			TinyDebris->SpawnPayload.FadeOutDuration > 0.0f);
		TestFalse(TEXT("Tiny Lua-threshold debris has no collision"),
			TinyDebris->SpawnPayload.bEnableCollision);
	}
	TestEqual(
		TEXT("Supported lower three cells remain"),
		static_cast<int32>(
			Algo::Count(
				Source->GetRuntimeMask(),
				static_cast<uint8>(1))),
		3);
	TestEqual(TEXT("Discard-only cut advances revision"), Source->Revision, 1);
	TestFalse(TEXT("Supported remainder remains active"), Source->bBroken);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxAggregateDetachmentTest,
	"MatterFlux.Fragment.Aggregate.MembersShareOneCarrierActor",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxAggregateDetachmentTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Root = World
		? SpawnSupportedColumn(*World)
		: nullptr;
	AFragment2DSourceActor* Crown = World
		? World->SpawnActor<AFragment2DSourceActor>(
			FVector(0.0f, 0.0f, 90.0f),
			FRotator::ZeroRotator)
		: nullptr;
	if (!TestNotNull(TEXT("Aggregate root spawns"), Root)
		|| !TestNotNull(TEXT("Aggregate attachment spawns"), Crown))
	{
		return false;
	}

	FFragmentSourceMask CrownMask;
	CrownMask.Width = 2;
	CrownMask.Height = 2;
	CrownMask.CellSize = 10.0f;
	CrownMask.MinFragmentAreaPixels = 1;
	CrownMask.MaxFragmentsPerBreak = 4;
	CrownMask.SupportMode = EFragmentSupportMode::Bottom;
	CrownMask.SolidMask.Init(1, 4);
	if (!TestTrue(
		TEXT("Aggregate attachment initializes"),
		Crown->InitializeFromProceduralMask(
			CrownMask,
			FGuid::NewDeterministicGuid(
				TEXT("AggregateCrown"),
				1),
			FLinearColor(0.1f, 0.7f, 0.1f),
			TEXT("leaf"))))
	{
		return false;
	}
	// Collision is a logical member property. The detached crown must be
	// represented by the carrier's compound body, not by a fallback Actor.
	Crown->SetSourceCollisionEnabled(true);
	const FGuid CrownSourceId = Crown->SourceId;

	const FGuid AggregateId =
		FGuid::NewDeterministicGuid(
			TEXT("GenericAggregate"),
			1);
	Root->ConfigureAggregate(AggregateId, true);
	Crown->ConfigureAggregate(AggregateId, false);

	FFragmentDamageEvent Event;
	Event.SourceId = Root->SourceId;
	Event.BaseRevision = Root->Revision;
	Event.DamageShape.Type = EFragmentDamageShapeType::Line;
	Event.DamageShape.WorldTransform = Root->GetActorTransform();
	Event.DamageShape.Extents.X = 100.0;
	Event.DamageShape.Thickness = 9.0f;
	Event.DamagePower = 0.0f;
	Event.EventSeed = 903;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem)
		|| !TestTrue(
			TEXT("Aggregate root cut commits"),
			Subsystem->RequestFragmentDamage(Root, Event)))
	{
		return false;
	}

	AFragment2DActor* Carrier = nullptr;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		Carrier = *It;
		break;
	}
	if (TestNotNull(
		TEXT("Detached root component becomes the carrier"),
		Carrier))
	{
		TestEqual(
			TEXT("Carrier retains one independently addressable aggregate member"),
			Carrier->GetAggregateMemberCount(),
			1);
		TestTrue(
			TEXT("Carrier retains the crown SourceId"),
			Carrier->ContainsAggregateSource(CrownSourceId));
		TestEqual(
			TEXT("Carrier retains the crown material identity"),
			Carrier->GetAggregateSourceMaterialId(CrownSourceId),
			FName(TEXT("leaf")));
		TestEqual(
			TEXT("Trunk and crown render through one multi-section mesh"),
			Carrier->MeshComponent->GetNumSections(),
			4);
		const UBodySetup* BodySetup =
			Carrier->MeshComponent->GetBodySetup();
		TestTrue(
			TEXT("Collision-enabled crown contributes a convex element to the carrier compound body"),
			BodySetup
			&& BodySetup->AggGeom.ConvexElems.Num() >= 2);
	}
	int32 RemainingSourceActors = 0;
	bool bCrownActorStillExists = false;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed())
		{
			++RemainingSourceActors;
			bCrownActorStillExists |= It->SourceId == CrownSourceId;
		}
	}
	TestFalse(
		TEXT("The crown no longer consumes an attached Source Actor"),
		bCrownActorStillExists);
	TestEqual(
		TEXT("Only the supported trunk remainder stays as a Source Actor"),
		RemainingSourceActors,
		1);
	TestFalse(
		TEXT("Ground-supported root remainder stays in terrain"),
		Root->bBroken);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxVoxelAggregateUnifiedOcclusionTest,
	"MatterFlux.Fragment.Aggregate.VoxelRootAndLeavesShareOneOccupancy",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxVoxelAggregateUnifiedOcclusionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Voxel aggregate world exists"), World))
	{
		return false;
	}

	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	Component.Min = Component.Max = FIntPoint(0, 0);
	Component.Cells = {FIntPoint(0, 0)};
	TArray<FFragmentSpawnPayload> Payloads;
	if (!TestTrue(
		TEXT("Voxel root payload builds"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{Component},
			FGuid::NewDeterministicGuid(TEXT("UnifiedVoxelRoot"), 1),
			FTransform::Identity,
			1,
			1,
			1,
			10.0f,
			1,
			16,
			FVector::ZeroVector,
			0.0f,
			1,
			Payloads,
			EFragmentSourceGeometryStyle::VoxelBlocks))
		|| !TestEqual(TEXT("One voxel root payload exists"), Payloads.Num(), 1))
	{
		return false;
	}
	Payloads[0].MaterialId = TEXT("wood");
	Payloads[0].bEnableCollision = false;

	AFragment2DActor* Carrier = World->SpawnActor<AFragment2DActor>();
	AFragment2DSourceActor* Leaves =
		World->SpawnActor<AFragment2DSourceActor>();
	if (!TestNotNull(TEXT("Voxel carrier spawns"), Carrier)
		|| !TestNotNull(TEXT("Voxel leaf source spawns"), Leaves))
	{
		return false;
	}
	Carrier->FragmentColor = FLinearColor(0.32f, 0.12f, 0.04f);
	if (!TestTrue(TEXT("Voxel carrier initializes"),
		Carrier->InitializeFromPayload(Payloads[0])))
	{
		return false;
	}
	TestEqual(TEXT("Wood carrier inherits Lua material density"),
		Carrier->BuoyancyComponent->GetBodyDensity(), 0.82f);

	FFragmentSourceMask LeafMask;
	LeafMask.Width = 1;
	LeafMask.Height = 1;
	LeafMask.CellSize = 10.0f;
	LeafMask.MinFragmentAreaPixels = 1;
	LeafMask.MaxFragmentsPerBreak = 4;
	LeafMask.SupportMode = EFragmentSupportMode::None;
	LeafMask.GeometryStyle = EFragmentSourceGeometryStyle::VoxelBlocks;
	LeafMask.SolidMask = {1};
	if (!TestTrue(
		TEXT("Overlapping voxel leaf initializes"),
		Leaves->InitializeFromProceduralMask(
			LeafMask,
			FGuid::NewDeterministicGuid(TEXT("UnifiedVoxelLeaf"), 1),
			FLinearColor(0.08f, 0.55f, 0.12f),
			TEXT("leaf")))
		|| !TestTrue(TEXT("Leaf is absorbed into the carrier"),
			Carrier->AbsorbAggregateSource(*Leaves)))
	{
		return false;
	}
	TestEqual(TEXT("Mixed carrier uses volume-weighted density"),
		Carrier->BuoyancyComponent->GetBodyDensity(), 0.52f);

	TestTrue(TEXT("Root payload keeps its cropped voxel mask"),
		Carrier->SpawnPayload.DetachedVoxelMask.IsValid());
	TestEqual(
		TEXT("Leaf priority removes the overlapping wood voxel instead of drawing two meshes"),
		Carrier->MeshComponent->GetNumSections(),
		2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceSpatialIndexTest,
	"MatterFlux.Fragment.SpatialIndex.QueryReturnsOnlyIntersectingSourcesInStableOrder",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceSpatialIndexTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Fragment::FSourceSpatialIndex Index(100.0);
	const FGuid NearA = FGuid::NewDeterministicGuid(
		TEXT("SpatialNearA"),
		1);
	const FGuid NearB = FGuid::NewDeterministicGuid(
		TEXT("SpatialNearB"),
		1);
	const FGuid Far = FGuid::NewDeterministicGuid(
		TEXT("SpatialFar"),
		1);

	TestTrue(TEXT("Far source is indexed"), Index.Upsert(
		Far,
		FBox(FVector(2000.0), FVector(2020.0))));
	TestTrue(TEXT("Second near source is indexed"), Index.Upsert(
		NearB,
		FBox(FVector(80.0), FVector(120.0))));
	TestTrue(TEXT("First near source is indexed"), Index.Upsert(
		NearA,
		FBox(FVector(-20.0), FVector(20.0))));

	TArray<FGuid> Actual;
	Index.Query(
		FBox(FVector(-30.0), FVector(130.0)),
		Actual);
	TArray<FGuid> Expected = {NearA, NearB};
	Expected.Sort(
		[](const FGuid& A, const FGuid& B)
		{
			return A.A != B.A ? A.A < B.A
				: A.B != B.B ? A.B < B.B
				: A.C != B.C ? A.C < B.C
				: A.D < B.D;
		});
	TestEqual(TEXT("Only intersecting sources are returned"), Actual, Expected);
	TestEqual(TEXT("Index owns all inserted records"), Index.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceSpatialBatchQueryTest,
	"MatterFlux.Fragment.SpatialIndex.BatchQueryReturnsStableDeduplicatedUnion",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceSpatialBatchQueryTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Fragment::FSourceSpatialIndex Index(100.0);
	const FGuid Left = FGuid::NewDeterministicGuid(
		TEXT("SpatialBatchLeft"),
		1);
	const FGuid Bridge = FGuid::NewDeterministicGuid(
		TEXT("SpatialBatchBridge"),
		1);
	const FGuid Right = FGuid::NewDeterministicGuid(
		TEXT("SpatialBatchRight"),
		1);
	const FGuid Far = FGuid::NewDeterministicGuid(
		TEXT("SpatialBatchFar"),
		1);
	TestTrue(TEXT("Left source is indexed"), Index.Upsert(
		Left,
		FBox(FVector(-20.0), FVector(20.0))));
	TestTrue(TEXT("Bridge source is indexed"), Index.Upsert(
		Bridge,
		FBox(FVector(80.0), FVector(120.0))));
	TestTrue(TEXT("Right source is indexed"), Index.Upsert(
		Right,
		FBox(FVector(180.0), FVector(220.0))));
	TestTrue(TEXT("Far source is indexed"), Index.Upsert(
		Far,
		FBox(FVector(5000.0), FVector(5020.0))));

	const TArray<FBox> QueryBounds =
	{
		FBox(FVector(-30.0), FVector(100.0)),
		FBox(FVector(100.0), FVector(230.0))
	};
	TArray<FGuid> Actual;
	Index.QueryMany(QueryBounds, Actual);
	TArray<FGuid> Expected = { Left, Bridge, Right };
	Expected.Sort(
		[](const FGuid& A, const FGuid& B)
		{
			return A.A != B.A ? A.A < B.A
				: A.B != B.B ? A.B < B.B
				: A.C != B.C ? A.C < B.C
				: A.D < B.D;
		});
	TestEqual(
		TEXT("Overlapping query bounds return one stable union"),
		Actual,
		Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRegisteredSourceSpatialQueryTest,
	"MatterFlux.Fragment.SpatialIndex.SubsystemReturnsOnlyLocalRegisteredActors",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxRegisteredSourceSpatialQueryTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Spatial query world exists"), World))
	{
		return false;
	}

	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 3;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SupportMode = EFragmentSupportMode::None;
	Mask.SolidMask.Init(1, 9);
	auto SpawnSource =
		[World, &Mask](const FVector& Location, const TCHAR* Label)
		{
			AFragment2DSourceActor* Source =
				World->SpawnActor<AFragment2DSourceActor>(
					Location,
					FRotator::ZeroRotator);
			return Source
				&& Source->InitializeFromProceduralMask(
					Mask,
					FGuid::NewDeterministicGuid(Label, 1))
				? Source
				: nullptr;
		};
	AFragment2DSourceActor* Near =
		SpawnSource(FVector::ZeroVector, TEXT("RegisteredNear"));
	AFragment2DSourceActor* Far =
		SpawnSource(FVector(5000.0, 0.0, 0.0), TEXT("RegisteredFar"));
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Near source spawns"), Near)
		|| !TestNotNull(TEXT("Far source spawns"), Far)
		|| !TestNotNull(TEXT("Fragment subsystem exists"), Subsystem))
	{
		return false;
	}

	TArray<AFragment2DSourceActor*> Actual;
	Subsystem->GatherSourcesInBounds(
		FBox(FVector(-100.0), FVector(100.0)),
		Actual);
	TestEqual(TEXT("Only one registered actor is local"), Actual.Num(), 1);
	TestTrue(
		TEXT("The local actor is returned"),
		Actual.Num() == 1 && Actual[0] == Near);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRegisteredSourceBatchSpatialQueryTest,
	"MatterFlux.Fragment.SpatialIndex.SubsystemBatchQueryReturnsStableUniqueActors",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxRegisteredSourceBatchSpatialQueryTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Batch spatial query world exists"), World))
	{
		return false;
	}

	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 3;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SupportMode = EFragmentSupportMode::None;
	Mask.SolidMask.Init(1, 9);
	auto SpawnSource =
		[World, &Mask](const FVector& Location, const TCHAR* Label)
		{
			AFragment2DSourceActor* Source =
				World->SpawnActor<AFragment2DSourceActor>(
					Location,
					FRotator::ZeroRotator);
			return Source
				&& Source->InitializeFromProceduralMask(
					Mask,
					FGuid::NewDeterministicGuid(Label, 1))
				? Source
				: nullptr;
		};
	AFragment2DSourceActor* Left =
		SpawnSource(FVector(0.0, 0.0, 0.0), TEXT("BatchLeft"));
	AFragment2DSourceActor* Bridge =
		SpawnSource(FVector(120.0, 0.0, 0.0), TEXT("BatchBridge"));
	AFragment2DSourceActor* Right =
		SpawnSource(FVector(240.0, 0.0, 0.0), TEXT("BatchRight"));
	AFragment2DSourceActor* Far =
		SpawnSource(FVector(5000.0, 0.0, 0.0), TEXT("BatchFar"));
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Left source spawns"), Left)
		|| !TestNotNull(TEXT("Bridge source spawns"), Bridge)
		|| !TestNotNull(TEXT("Right source spawns"), Right)
		|| !TestNotNull(TEXT("Far source spawns"), Far)
		|| !TestNotNull(TEXT("Fragment subsystem exists"), Subsystem))
	{
		return false;
	}

	const TArray<FBox> QueryBounds =
	{
		FBox(FVector(-100.0), FVector(140.0)),
		FBox(FVector(100.0, -100.0, -100.0), FVector(340.0, 100.0, 100.0))
	};
	TArray<AFragment2DSourceActor*> Actual;
	Subsystem->GatherSourcesInBoundsMany(QueryBounds, Actual);
	TArray<AFragment2DSourceActor*> Expected = { Left, Bridge, Right };
	Expected.Sort(
		[](const AFragment2DSourceActor& A, const AFragment2DSourceActor& B)
		{
			const FGuid& LeftId = A.SourceId;
			const FGuid& RightId = B.SourceId;
			return LeftId.A != RightId.A ? LeftId.A < RightId.A
				: LeftId.B != RightId.B ? LeftId.B < RightId.B
				: LeftId.C != RightId.C ? LeftId.C < RightId.C
				: LeftId.D < RightId.D;
		});
	TestEqual(
		TEXT("Overlapping bounds return one stable registered actor union"),
		Actual,
		Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWorldCutServiceTest,
	"MatterFlux.Fragment.Cut.WorldRequestTargetsIntersectingSources",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxWorldCutServiceTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Cut service world exists"), World))
	{
		return false;
	}

	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 3;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SupportMode = EFragmentSupportMode::None;
	Mask.SolidMask.Init(1, 9);
	auto SpawnCutTarget =
		[World, &Mask](
			const FVector& Location,
			const TCHAR* IdText)
		{
			AFragment2DSourceActor* Source =
				World->SpawnActor<AFragment2DSourceActor>(
					Location,
					FRotator::ZeroRotator);
			return Source
				&& Source->InitializeFromProceduralMask(
					Mask,
					FGuid::NewDeterministicGuid(
						IdText,
						1))
				? Source
				: nullptr;
		};
	AFragment2DSourceActor* Near =
		SpawnCutTarget(FVector::ZeroVector, TEXT("WorldCutNear"));
	AFragment2DSourceActor* Far =
		SpawnCutTarget(FVector(2000.0f, 0.0f, 0.0f), TEXT("WorldCutFar"));
	if (!TestNotNull(TEXT("Near cut target spawns"), Near)
		|| !TestNotNull(TEXT("Far cut target spawns"), Far))
	{
		return false;
	}

	FFragmentWorldCutRequest Request;
	Request.CutShape.Type = EFragmentDamageShapeType::Circle;
	Request.CutShape.WorldTransform = FTransform::Identity;
	Request.CutShape.Radius = 6.0f;
	Request.DamagePower = 0.0f;
	Request.EventSeed = 904;
	Request.TargetPadding = 10.0f;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("World cut service exists"), Subsystem))
	{
		return false;
	}
	TestEqual(
		TEXT("One generic mask source accepts the world cut"),
		Subsystem->RequestWorldCut(Request),
		1);
	TestEqual(TEXT("Intersecting source commits"), Near->Revision, 1);
	TestEqual(TEXT("Distant source is untouched"), Far->Revision, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWorldCutTargetBudgetTest,
	"MatterFlux.Fragment.Cut.WorldRequestCapsAndOrdersTargets",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxWorldCutTargetBudgetTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Target-budget world exists"), World))
	{
		return false;
	}

	FFragmentSourceMask Mask;
	Mask.Width = 1;
	Mask.Height = 1;
	Mask.CellSize = 8.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 1;
	Mask.SupportMode = EFragmentSupportMode::None;
	Mask.SolidMask = {1};
	TArray<AFragment2DSourceActor*> Sources;
	for (int32 Index = 4; Index >= 0; --Index)
	{
		AFragment2DSourceActor* Source =
			World->SpawnActor<AFragment2DSourceActor>(
				FVector(Index * 20.0f, 0.0f, 0.0f),
				FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Budgeted cut target spawns"), Source))
		{
			return false;
		}
		Source->bDestroySourceOnFirstBreak = false;
		if (!TestTrue(TEXT("Budgeted cut target initializes"),
			Source->InitializeFromProceduralMask(
				Mask,
				FGuid::NewDeterministicGuid(
					FString::Printf(TEXT("WorldCutBudget%d"), Index),
					1))))
		{
			return false;
		}
		Sources.Insert(Source, 0);
	}

	FFragmentWorldCutRequest Request;
	Request.CutShape.Type = EFragmentDamageShapeType::Circle;
	Request.CutShape.WorldTransform = FTransform::Identity;
	Request.CutShape.Radius = 120.0f;
	Request.DamagePower = 0.0f;
	Request.EventSeed = 905;
	Request.MaxAffectedSources = 2;
	TestEqual(TEXT("A bounded world cut commits only its target budget"),
		UFragmentSimulationSubsystem::ExecuteWorldCut(World, Request),
		2);
	TestEqual(TEXT("Nearest source is cut first"), Sources[0]->Revision, 1);
	TestEqual(TEXT("Second-nearest source is cut second"), Sources[1]->Revision, 1);
	for (int32 Index = 2; Index < Sources.Num(); ++Index)
	{
		TestEqual(TEXT("Sources outside the target budget remain untouched"),
			Sources[Index]->Revision,
			0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDetachedItemRepeatedCutTest,
	"MatterFlux.Fragment.Cut.DetachedItemFadesAfterRepeatedCuts",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxDetachedItemRepeatedCutTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Detached-item cut world exists"), World))
	{
		return false;
	}

	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	for (int32 Y = 0; Y < 3; ++Y)
	{
		for (int32 X = 0; X < 3; ++X)
		{
			Component.Cells.Add(FIntPoint(X, Y));
		}
	}
	Component.Min = FIntPoint::ZeroValue;
	Component.Max = FIntPoint(2, 2);
	TArray<FFragmentSpawnPayload> Payloads;
	if (!TestTrue(
		TEXT("Detached item payload builds from canonical cells"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{Component},
			FGuid::NewDeterministicGuid(TEXT("RepeatedCutItem"), 1),
			FTransform::Identity,
			3,
			3,
			1,
			10.0f,
			1,
			1,
			FVector::ZeroVector,
			0.0f,
			907,
			Payloads))
		|| !TestEqual(TEXT("One item payload is produced"), Payloads.Num(), 1))
	{
		return false;
	}
	Payloads[0].MaterialId = TEXT("wood");
	Payloads[0].bEnableCollision = true;
	AFragment2DActor* Item = World->SpawnActor<AFragment2DActor>();
	if (!TestNotNull(TEXT("Detached item actor spawns"), Item)
		|| !TestTrue(
			TEXT("Detached item initializes"),
			Item->InitializeFromPayload(Payloads[0])))
	{
		return false;
	}

	FFragmentWorldCutRequest Request;
	Request.CutShape.Type = EFragmentDamageShapeType::Line;
	Request.CutShape.WorldTransform = Item->GetActorTransform();
	Request.CutShape.Extents.X = 40.0f;
	Request.CutShape.Thickness = 8.0f;
	Request.DamagePower = 0.0f;
	Request.EventSeed = 908;
	Request.MaxAffectedSources = 1;
	const int32 RequiredCuts = Item->GetCutsBeforeFade();
	if (!TestTrue(TEXT("Detached items require multiple cuts"), RequiredCuts > 1))
	{
		return false;
	}
	for (int32 CutIndex = 0; CutIndex < RequiredCuts; ++CutIndex)
	{
		Request.EventSeed += CutIndex;
		TestEqual(
			TEXT("The world cut accepts the detached logical item"),
			UFragmentSimulationSubsystem::ExecuteWorldCut(World, Request),
			1);
		TestEqual(
			TEXT("One accepted cut advances item durability once"),
			Item->GetAcceptedCutCount(),
			CutIndex + 1);
		if (CutIndex + 1 < RequiredCuts)
		{
			TestFalse(
				TEXT("Item remains physical before its cut threshold"),
				Item->IsCutFadeActive());
		}
	}
	TestTrue(
		TEXT("The threshold starts a fade instead of deleting immediately"),
		Item->IsCutFadeActive());
	TestTrue(TEXT("Threshold item is still present for the fade"), IsValid(Item));
	TestEqual(
		TEXT("Fading item no longer blocks or simulates physics"),
		Item->MeshComponent->GetCollisionEnabled(),
		ECollisionEnabled::NoCollision);
	const float InitialAlpha = Item->GetTransientFadeAlpha();
	Item->Tick(Item->GetCutFadeDuration() * 0.5f);
	TestTrue(
		TEXT("Repeated-cut item opacity decreases over time"),
		Item->GetTransientFadeAlpha() < InitialAlpha);
	TestEqual(
		TEXT("A fading item does not consume another cut target"),
		UFragmentSimulationSubsystem::ExecuteWorldCut(World, Request),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWorldCutAggregateBudgetTest,
	"MatterFlux.Fragment.Cut.WorldRequestTreatsAggregateAsOneTarget",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxWorldCutAggregateBudgetTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Aggregate cut world exists"), World))
	{
		return false;
	}

	FFragmentSourceMask Mask;
	Mask.Width = 2;
	Mask.Height = 8;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SupportMode = EFragmentSupportMode::Bottom;
	Mask.GeometryStyle = EFragmentSourceGeometryStyle::VoxelBlocks;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

	const FGuid AggregateId = FGuid::NewDeterministicGuid(
		TEXT("WorldCutAggregateBudget"),
		1);
	const auto SpawnAggregateLayer =
		[World, &Mask, &AggregateId](
			const TCHAR* IdText,
			const FVector& Location,
			const bool bRoot)
		{
			AFragment2DSourceActor* Source =
				World->SpawnActor<AFragment2DSourceActor>(
					Location,
					FRotator::ZeroRotator);
			if (!Source
				|| !Source->InitializeFromProceduralMask(
					Mask,
					FGuid::NewDeterministicGuid(IdText, 1),
					FLinearColor::White,
					TEXT("wood")))
			{
				return static_cast<AFragment2DSourceActor*>(nullptr);
			}
			Source->bDestroySourceOnFirstBreak = false;
			Source->ConfigureAggregate(AggregateId, bRoot);
			return Source;
		};

	// The front slice is closest to the broad-phase query center. The aggregate
	// root deliberately sits behind it so the old per-source budget selected a
	// non-root vertical slice and never executed the whole-tree transaction.
	AFragment2DSourceActor* FrontSlice = SpawnAggregateLayer(
		TEXT("WorldCutAggregateFront"),
		FVector(0.0f, 0.0f, 0.0f),
		false);
	AFragment2DSourceActor* BackSlice = SpawnAggregateLayer(
		TEXT("WorldCutAggregateBack"),
		FVector(0.0f, 20.0f, 0.0f),
		false);
	AFragment2DSourceActor* Root = SpawnAggregateLayer(
		TEXT("WorldCutAggregateRoot"),
		FVector(0.0f, 40.0f, 0.0f),
		true);
	if (!TestNotNull(TEXT("Aggregate root spawns"), Root)
		|| !TestNotNull(TEXT("Front aggregate slice spawns"), FrontSlice)
		|| !TestNotNull(TEXT("Back aggregate slice spawns"), BackSlice))
	{
		return false;
	}
	FFragmentSourceMask DecoyMask = Mask;
	DecoyMask.Width = 1;
	DecoyMask.Height = 1;
	DecoyMask.SupportMode = EFragmentSupportMode::None;
	DecoyMask.SolidMask = {1};
	AFragment2DSourceActor* CenterCloserDecoy =
		World->SpawnActor<AFragment2DSourceActor>(
			FVector(0.0f, 25.0f, 0.0f),
			FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Competing small cut target spawns"), CenterCloserDecoy)
		|| !TestTrue(TEXT("Competing small cut target initializes"),
			CenterCloserDecoy->InitializeFromProceduralMask(
				DecoyMask,
				FGuid::NewDeterministicGuid(
					TEXT("WorldCutAggregateSurfaceDistanceDecoy"), 1))))
	{
		return false;
	}
	CenterCloserDecoy->bDestroySourceOnFirstBreak = false;

	FFragmentWorldCutRequest Request;
	Request.CutShape.Type = EFragmentDamageShapeType::Line;
	Request.CutShape.WorldTransform = FTransform::Identity;
	Request.CutShape.Extents.X = 80.0f;
	Request.CutShape.Thickness = 11.0f;
	Request.DamagePower = 0.0f;
	Request.EventSeed = 906;
	Request.TargetPadding = 60.0f;
	Request.MaxAffectedSources = 1;

	TestEqual(
		TEXT("One aggregate consumes one world-cut target budget"),
		UFragmentSimulationSubsystem::ExecuteWorldCut(World, Request),
		1);
	TestEqual(
		TEXT("The aggregate root owns the horizontal cut transaction"),
		Root->Revision,
		1);
	TestEqual(
		TEXT("Target budget uses distance to the intersected object surface, not the aggregate root center"),
		CenterCloserDecoy->Revision,
		0);

	AFragment2DActor* Carrier = nullptr;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		if (It->AggregateSources.ContainsByPredicate(
			[FrontSlice, BackSlice](
				const FFragmentAggregateSourceState& Member)
			{
				return Member.DefinitionSourceId == FrontSlice->SourceId
					|| Member.DefinitionSourceId == BackSlice->SourceId;
			}))
		{
			Carrier = *It;
			break;
		}
	}
	if (TestNotNull(
		TEXT("Horizontal root cut creates one carrier for the aggregate layers"),
		Carrier))
	{
		TestTrue(
			TEXT("The carrier contains both detached depth slices"),
			Carrier->AggregateSources.ContainsByPredicate(
				[FrontSlice](const FFragmentAggregateSourceState& Member)
				{
					return Member.DefinitionSourceId == FrontSlice->SourceId;
				})
				&& Carrier->AggregateSources.ContainsByPredicate(
					[BackSlice](const FFragmentAggregateSourceState& Member)
					{
						return Member.DefinitionSourceId == BackSlice->SourceId;
					}));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxNavigationDefaultsTest, "MatterFlux.Fragment.Actor.ProceduralMeshesDoNotAffectNavigation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxNavigationDefaultsTest::RunTest(const FString& Parameters)
{
	const AFragment2DSourceActor* SourceDefault = GetDefault<AFragment2DSourceActor>();
	const AFragment2DActor* FragmentDefault = GetDefault<AFragment2DActor>();
	if (!TestNotNull(TEXT("Source actor default exists"), SourceDefault)
		|| !TestNotNull(TEXT("Fragment actor default exists"), FragmentDefault))
	{
		return false;
	}
	if (!TestNotNull(TEXT("Source mesh default exists"), SourceDefault->MeshComponent.Get())
		|| !TestNotNull(TEXT("Fragment mesh default exists"), FragmentDefault->MeshComponent.Get()))
	{
		return false;
	}

	TestFalse(TEXT("Source procedural mesh does not trigger navigation updates"),
		SourceDefault->MeshComponent->CanEverAffectNavigation());
	TestFalse(TEXT("Dynamic fragment procedural mesh does not trigger navigation updates"),
		FragmentDefault->MeshComponent->CanEverAffectNavigation());
	TestFalse(TEXT("Dynamic fragments use spatial relevancy instead of replicating globally"),
		FragmentDefault->bAlwaysRelevant);
	TestTrue(TEXT("Dynamic fragments have a finite default lifetime"),
		FragmentDefault->InitialLifeSpan > 0.0f);
	const AFragment2DDamageRequestActor* RequestDefault = GetDefault<AFragment2DDamageRequestActor>();
	TestFalse(TEXT("Damage request helpers do not consume a per-frame tick"),
		RequestDefault->PrimaryActorTick.bCanEverTick);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxDamageRollbackTest, "MatterFlux.Fragment.Damage.InvalidAndNoChangeRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDamageRollbackTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World ? World->SpawnActor<AFragment2DSourceActor>() : nullptr;
	if (!TestNotNull(TEXT("Source actor spawned"), Source)) return false;
	const TArray<uint8> OriginalMask = Source->GetRuntimeMask();
	const int32 OriginalRevision = Source->Revision;
	TArray<FFragmentSpawnPayload> Payloads;

	FFragmentDamageEvent WrongSource = MakeCircleEvent(*Source, Source->GetActorLocation(), 20.0f);
	WrongSource.SourceId = FGuid(9, 9, 9, 9);
	TestFalse(TEXT("Mismatched SourceId is rejected"), Source->ApplyDamageEvent(WrongSource, Payloads));
	TestTrue(TEXT("SourceId rejection preserves mask"), Source->GetRuntimeMask() == OriginalMask);
	TestEqual(TEXT("SourceId rejection preserves revision"), Source->Revision, OriginalRevision);

	FFragmentDamageEvent WrongRevision = MakeCircleEvent(*Source, Source->GetActorLocation(), 20.0f);
	WrongRevision.BaseRevision++;
	TestFalse(TEXT("Mismatched revision is rejected"), Source->ApplyDamageEvent(WrongRevision, Payloads));
	TestTrue(TEXT("Revision rejection preserves mask"), Source->GetRuntimeMask() == OriginalMask);
	TestEqual(TEXT("Revision rejection preserves revision"), Source->Revision, OriginalRevision);

	FFragmentDamageEvent NoChange = MakeCircleEvent(*Source, FVector(100000.0, 0.0, 100000.0), 1.0f);
	TestFalse(TEXT("Damage outside the mask is not accepted"), Source->ApplyDamageEvent(NoChange, Payloads));
	TestTrue(TEXT("No-change event preserves mask"), Source->GetRuntimeMask() == OriginalMask);
	TestEqual(TEXT("No-change event preserves revision"), Source->Revision, OriginalRevision);

	FFragmentDamageEvent InvalidShape = MakeCircleEvent(*Source, Source->GetActorLocation(), -1.0f);
	TestFalse(TEXT("Invalid geometry input is rejected"), Source->ApplyDamageEvent(InvalidShape, Payloads));
	TestTrue(TEXT("Invalid geometry input preserves mask"), Source->GetRuntimeMask() == OriginalMask);
	TestEqual(TEXT("Invalid geometry input preserves revision"), Source->Revision, OriginalRevision);

	Source->Revision = MAX_int32;
	FFragmentDamageEvent ExhaustedRevision = MakeCircleEvent(*Source, Source->GetActorLocation(), 20.0f);
	AddExpectedError(
		TEXT("revision 2147483647 cannot be incremented safely"),
		EAutomationExpectedErrorFlags::Contains,
		1,
		false);
	TestFalse(TEXT("A revision that cannot be incremented is rejected"), Source->ApplyDamageEvent(ExhaustedRevision, Payloads));
	TestTrue(TEXT("Revision exhaustion preserves mask"), Source->GetRuntimeMask() == OriginalMask);
	TestEqual(TEXT("Revision exhaustion does not wrap"), Source->Revision, MAX_int32);
	Source->Revision = OriginalRevision;

	UFragment2DAsset* MismatchedAsset = NewObject<UFragment2DAsset>();
	MismatchedAsset->MaskWidth = 4;
	MismatchedAsset->MaskHeight = 4;
	Source->FragmentAsset = MismatchedAsset;
	FFragmentDamageEvent InvalidMaskGeometry = MakeCircleEvent(*Source, Source->GetActorLocation(), 20.0f);
	AddExpectedError(
		TEXT("runtime mask dimensions are invalid"),
		EAutomationExpectedErrorFlags::Contains,
		1,
		false);
	TestFalse(TEXT("Geometry build with mismatched mask dimensions is rejected"), Source->ApplyDamageEvent(InvalidMaskGeometry, Payloads));
	TestTrue(TEXT("Geometry failure preserves mask"), Source->GetRuntimeMask() == OriginalMask);
	TestEqual(TEXT("Geometry failure preserves revision"), Source->Revision, OriginalRevision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxSourceConstructionMaskTest, "MatterFlux.Fragment.Source.EditorConstructionRefreshesAssetMask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceConstructionMaskTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor world exists"), World)) return false;

	UFragment2DAsset* Asset = NewObject<UFragment2DAsset>();
	Asset->MaskWidth = 2;
	Asset->MaskHeight = 2;
	Asset->SolidMask = { 1, 0, 0, 1 };

	AFragment2DSourceActor* Source = World->SpawnActorDeferred<AFragment2DSourceActor>(
		AFragment2DSourceActor::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Deferred source spawned"), Source)) return false;
	Source->FragmentAsset = Asset;
	Source->FinishSpawning(FTransform::Identity);
	TestTrue(TEXT("Initial construction copies the asset mask"), Source->GetRuntimeMask() == Asset->SolidMask);

	Asset->SolidMask = { 1, 1, 1, 1 };
	Source->OnConstruction(Source->GetActorTransform());
	TestTrue(TEXT("Same-size asset edits refresh the editor runtime mask"), Source->GetRuntimeMask() == Asset->SolidMask);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceAcceptsExtremeFiniteMaskTest,
	"MatterFlux.Fragment.Source.ExtremeFiniteMaskRemainsBuildable",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceAcceptsExtremeFiniteMaskTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Source actor spawns"), Source))
	{
		return false;
	}

	FFragmentSourceMask Mask;
	Mask.Width = 2;
	Mask.Height = 1;
	Mask.CellSize = std::numeric_limits<float>::max();
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 1;
	Mask.SupportMode = EFragmentSupportMode::Bottom;
	Mask.SolidMask = {1, 1};
	TestTrue(
		TEXT("Extreme mask is structurally valid before geometry derivation"),
		Mask.IsValid());
	TestTrue(
		TEXT("Double-precision geometry accepts a finite float-scale mask"),
		Source->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(
				TEXT("ExtremeFiniteMask"),
				1)));
	TestTrue(
		TEXT("Accepted source retains the requested identity"),
		Source->SourceId.IsValid());
	TestEqual(
		TEXT("Accepted source retains its runtime cells"),
		Source->GetRuntimeMask().Num(),
		2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxFilteredBreakCommitTest, "MatterFlux.Fragment.Damage.AllDebrisFilteredStillCommits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxFilteredBreakCommitTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor world exists"), World)) return false;
	UFragment2DAsset* Asset = NewObject<UFragment2DAsset>();
	Asset->MaskWidth = 3;
	Asset->MaskHeight = 1;
	Asset->CellSize = 10.0f;
	Asset->MinFragmentAreaPixels = 10;
	Asset->SupportMode = EFragmentSupportMode::None;
	Asset->SolidMask = { 1, 1, 1 };
	AFragment2DSourceActor* Source = World->SpawnActorDeferred<AFragment2DSourceActor>(AFragment2DSourceActor::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Deferred source spawned"), Source)) return false;
	Source->FragmentAsset = Asset;
	Source->bDestroySourceOnFirstBreak = false;
	Source->FinishSpawning(FTransform::Identity);
	FFragmentDamageEvent Event = MakeCircleEvent(*Source, Source->GetActorLocation(), 6.0f);
	UFragmentSimulationSubsystem* Subsystem = World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem)) return false;
	TestTrue(TEXT("Accepted damage reports committed with only fading debris"), Subsystem->RequestFragmentDamage(Source, Event));
	TestEqual(TEXT("Committed damage increments revision"), Source->Revision, 1);
	TestTrue(TEXT("Committed filtered break marks source broken"), Source->bBroken);
	TestTrue(TEXT("Non-destroy source is hidden"), Source->IsHidden());
	TestFalse(TEXT("Non-destroy source collision is disabled"), Source->GetActorEnableCollision());

	AFragment2DActor* FadingDebris = nullptr;
	int32 FragmentCount = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		FadingDebris = *It;
		++FragmentCount;
	}
	TestEqual(TEXT("Filtered debris is merged into one bounded render carrier"), FragmentCount, 1);
	if (TestNotNull(TEXT("Filtered debris remains visible briefly"), FadingDebris))
	{
		TestTrue(TEXT("Filtered debris has a finite fade duration"),
			FadingDebris->SpawnPayload.FadeOutDuration > 0.0f
			&& FMath::IsFinite(FadingDebris->SpawnPayload.FadeOutDuration));
		TestFalse(TEXT("Filtered debris never creates a physics body"),
			FadingDebris->SpawnPayload.bEnableCollision);
		TestEqual(TEXT("Filtered debris collision is disabled"),
			FadingDebris->MeshComponent->GetCollisionEnabled(),
			ECollisionEnabled::NoCollision);
		const float InitialAlpha = FadingDebris->GetTransientFadeAlpha();
		FadingDebris->Tick(FadingDebris->SpawnPayload.FadeOutDuration * 0.5f);
		TestTrue(TEXT("Filtered debris fades continuously before destruction"),
			FadingDebris->GetTransientFadeAlpha() < InitialAlpha
			&& FadingDebris->GetTransientFadeAlpha() > 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxMaterializationRollbackTest, "MatterFlux.Fragment.Damage.MaterializationFailureRollsBack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterializationRollbackTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World ? World->SpawnActor<AFragment2DSourceActor>() : nullptr;
	if (!TestNotNull(TEXT("Source actor spawned"), Source)) return false;
	Source->bDestroySourceOnFirstBreak = false;
	Source->DefaultSupportMode = EFragmentSupportMode::None;
	Source->FragmentActorClass = nullptr;
	const TArray<uint8> OriginalMask = Source->GetRuntimeMask();
	const int32 OriginalRevision = Source->Revision;
	const FFragmentDamageEvent Event = MakeCircleEvent(*Source, Source->GetActorLocation(), 20.0f);
	UFragmentSimulationSubsystem* Subsystem = World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem)) return false;
	AddExpectedError(
		TEXT("Fragment actor class is invalid"),
		EAutomationExpectedErrorFlags::Contains,
		1,
		false);
	TestFalse(TEXT("A fragment materialization failure rejects the transaction"),
		Subsystem->RequestFragmentDamage(Source, Event));
	TestTrue(TEXT("Materialization failure preserves the mask"), Source->GetRuntimeMask() == OriginalMask);
	TestEqual(TEXT("Materialization failure preserves the revision"), Source->Revision, OriginalRevision);
	TestFalse(TEXT("Materialization failure does not mark the source broken"), Source->bBroken);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxInitializationRollbackTest, "MatterFlux.Fragment.Damage.InitializationFailureRollsBack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxInitializationRollbackTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World ? World->SpawnActor<AFragment2DSourceActor>() : nullptr;
	if (!TestNotNull(TEXT("Source actor spawned"), Source)) return false;
	Source->bDestroySourceOnFirstBreak = false;
	Source->DefaultSupportMode = EFragmentSupportMode::None;
	Source->FragmentActorClass = AMatterFluxFailingFragmentActor::StaticClass();
	const TArray<uint8> OriginalMask = Source->GetRuntimeMask();
	const int32 OriginalRevision = Source->Revision;
	const FFragmentDamageEvent Event = MakeCircleEvent(*Source, Source->GetActorLocation(), 20.0f);
	UFragmentSimulationSubsystem* Subsystem = World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem)) return false;
	AddExpectedError(
		TEXT("could not initialize from its payload"),
		EAutomationExpectedErrorFlags::Contains,
		1,
		false);

	TestFalse(TEXT("A fragment initialization failure rejects the transaction"),
		Subsystem->RequestFragmentDamage(Source, Event));
	TestTrue(TEXT("Initialization failure preserves the mask"), Source->GetRuntimeMask() == OriginalMask);
	TestEqual(TEXT("Initialization failure preserves the revision"), Source->Revision, OriginalRevision);
	TestFalse(TEXT("Initialization failure does not mark the source broken"), Source->bBroken);

	int32 RemainingFragments = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		++RemainingFragments;
	}
	TestEqual(TEXT("Initialization rollback destroys all deferred fragments"), RemainingFragments, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxFragmentMaterialTest, "MatterFlux.Fragment.Actor.SourceMaterialPropagatesToFragments", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentMaterialTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World ? World->SpawnActor<AFragment2DSourceActor>() : nullptr;
	if (!TestNotNull(TEXT("Source actor spawned"), Source)) return false;
	Source->bDestroySourceOnFirstBreak = false;
	Source->DefaultSupportMode = EFragmentSupportMode::None;
	Source->FragmentMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	const FFragmentDamageEvent Event = MakeCircleEvent(*Source, Source->GetActorLocation(), 20.0f);
	UFragmentSimulationSubsystem* Subsystem = World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem)) return false;
	if (!TestTrue(TEXT("Damage with a source material is accepted"),
		Subsystem->RequestFragmentDamage(Source, Event)))
	{
		return false;
	}

	int32 FragmentCount = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		++FragmentCount;
		TestEqual(TEXT("Spawned fragment keeps the source material"),
			It->FragmentMaterial.Get(), Source->FragmentMaterial.Get());
		UMaterialInstanceDynamic* FragmentProjection =
			Cast<UMaterialInstanceDynamic>(
				It->MeshComponent->GetMaterial(0));
		if (TestNotNull(
			TEXT("Spawned fragment mesh owns a canonical material projection"),
			FragmentProjection))
		{
			TestEqual(
				TEXT("Fragment projection retains the source parent material"),
				FragmentProjection->GetMaterial(),
				Source->FragmentMaterial->GetMaterial());
			const float CellSize =
				It->SpawnPayload.DetachedVoxelMask.CellSize > 0.0f
					? It->SpawnPayload.DetachedVoxelMask.CellSize
					: 12.0f;
			const MatterFlux::Rendering::FVoxelMaterialProjection Expected =
				MatterFlux::Rendering::ResolveVoxelMaterialProjection(
					It->FragmentColor,
					It->SpawnPayload.MaterialId,
					CellSize,
					MatterFlux::Rendering::EVoxelMaterialFaceRole::Primary);
			TestTrue(
				TEXT("Fragment projection resolves the canonical source color"),
				FragmentProjection->K2_GetVectorParameterValue(TEXT("Color"))
					.Equals(Expected.ResolvedColor, 1.0e-4f));
			for (const TPair<FName, float>& Parameter : {
				TPair<FName, float>(TEXT("FaceContrast"), Expected.Style.FaceContrast),
				TPair<FName, float>(TEXT("ColorVariation"), Expected.Style.ColorVariation),
				TPair<FName, float>(TEXT("Roughness"), Expected.Style.Roughness),
				TPair<FName, float>(TEXT("ShadowLift"), Expected.Style.ShadowLift)})
			{
				TestEqual(
					*FString::Printf(
						TEXT("Fragment projection preserves canonical %s"),
						*Parameter.Key.ToString()),
					FragmentProjection->K2_GetScalarParameterValue(Parameter.Key),
					Parameter.Value);
			}
		}
	}
	TestTrue(TEXT("At least one materialized fragment was inspected"), FragmentCount > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxInvalidPayloadActorTest, "MatterFlux.Fragment.Actor.InvalidPayloadDisablesCollision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxInvalidPayloadActorTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DActor* Fragment = World ? World->SpawnActor<AFragment2DActor>() : nullptr;
	if (!TestNotNull(TEXT("Fragment actor spawned"), Fragment)) return false;
	FFragmentSpawnPayload InvalidPayload;
	InvalidPayload.Thickness = 10.0f;
	InvalidPayload.Vertices2D = { FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(0.0, 1.0) };
	InvalidPayload.TriangleIndices = { 0, 1, 99 };
	TestFalse(TEXT("Invalid visual geometry is rejected"), Fragment->InitializeFromPayload(InvalidPayload));
	TestEqual(TEXT("Invalid visual geometry leaves collision disabled"),
		Fragment->MeshComponent->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	TestFalse(TEXT("Invalid visual geometry cannot simulate physics"), Fragment->MeshComponent->IsSimulatingPhysics());

	FFragmentSpawnPayload InvalidCollisionPayload;
	InvalidCollisionPayload.Thickness = 10.0f;
	InvalidCollisionPayload.Vertices2D = { FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(0.0, 1.0) };
	InvalidCollisionPayload.TriangleIndices = { 0, 1, 2 };
	FFragmentContour Outer;
	Outer.Vertices = InvalidCollisionPayload.Vertices2D;
	InvalidCollisionPayload.OuterContours = { Outer };
	FFragmentContour CollinearCollision;
	CollinearCollision.Vertices = { FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(2.0, 0.0) };
	InvalidCollisionPayload.CollisionContours = { CollinearCollision };
	TestFalse(TEXT("Invalid convex collision is rejected"), Fragment->InitializeFromPayload(InvalidCollisionPayload));
	TestEqual(TEXT("Degenerate convex collision is not submitted to physics"),
		Fragment->MeshComponent->GetCollisionEnabled(), ECollisionEnabled::NoCollision);

	FFragmentSpawnPayload InvalidPhysicsPayload = InvalidCollisionPayload;
	InvalidPhysicsPayload.CollisionContours = { Outer };
	InvalidPhysicsPayload.Mass = std::numeric_limits<float>::quiet_NaN();
	InvalidPhysicsPayload.InitialLinearVelocity.X = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("Invalid physics state is rejected"), Fragment->InitializeFromPayload(InvalidPhysicsPayload));
	TestEqual(TEXT("Invalid payload physics state disables collision"),
		Fragment->MeshComponent->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	TestFalse(TEXT("Invalid payload physics state cannot simulate"),
		Fragment->MeshComponent->IsSimulatingPhysics());
	return true;
}
