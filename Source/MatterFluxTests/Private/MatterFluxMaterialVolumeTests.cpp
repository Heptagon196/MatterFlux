#include "Fragment/FragmentTypes.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"
#include "Volume/MatterFluxMaterialVolume.h"

namespace MatterFluxVolumeTests
{
	FMaterialVolumeChunkSnapshot BuildTwoCellChunk(
		const EMaterialVolumeStorageAdapter Adapter)
	{
		FMaterialVolumeChunkBuilder Builder(FIntPoint::ZeroValue, 16);
		Builder.SetColumnSpans(
			FIntPoint(0, 0),
			{ FMaterialSpan(0, 1, 1) });
		Builder.SetColumnSpans(
			FIntPoint(1, 0),
			{ FMaterialSpan(0, 1, 2) });
		FMaterialVolumeChunkSnapshot Result;
		FString Error;
		verify(Builder.Build(Adapter, Result, Error));
		return Result;
	}

	FMaterialVolumeTopology BuildTopology(
		const FMaterialVolumeChunkSnapshot& Chunk)
	{
		FMaterialVolumeTopology Topology;
		Topology.DefinitionId = TEXT("test.volume");
		Topology.GridFrame.CellSize = 10.0;
		Topology.TopologyRevision = 1;
		Topology.Chunks.Add(Chunk.ChunkCoord, Chunk);
		return Topology;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialVolumeDeltaTransactionTest,
	"MatterFlux.Volume.Core.RevisionedDeltaIsAtomicAndRequestsSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialVolumeDeltaTransactionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FGuid InstanceId =
		FGuid::NewDeterministicGuid(TEXT("volume.delta.instance"), 1);
	FMaterialVolumeTopology Base = MatterFluxVolumeTests::BuildTopology(
		MatterFluxVolumeTests::BuildTwoCellChunk(
			EMaterialVolumeStorageAdapter::Span));
	FMaterialVolumeFields BaseFields;
	BaseFields.EnvironmentEnergy = 100;
	FMaterialVolumeTopology Target;
	FString Error;
	if (!TestTrue(TEXT("Target material edit commits"),
		FMaterialVolumeAlgorithms::SetCellMaterial(
			Base, FIntVector(1, 0, 0), 3, Target, Error)))
	{
		AddError(Error);
		return false;
	}
	FMaterialVolumeFields TargetFields = BaseFields;
	TestTrue(TEXT("Target field edit commits independently"),
		TargetFields.SetEnergy(FIntVector(0, 0, 0), 700));
	FMaterialVolumeDelta Delta;
	if (!TestTrue(TEXT("Topology and field delta builds"),
		FMaterialVolumeAlgorithms::BuildDelta(
			InstanceId,
			Base,
			BaseFields,
			Target,
			TargetFields,
			Delta,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Delta carries one changed immutable Chunk"),
		Delta.ChunkChanges.Num(), 1);
	TestEqual(TEXT("Delta carries one sparse energy change"),
		Delta.FieldChanges.Num(), 1);
	FMaterialVolumeTopology Applied = Base;
	FMaterialVolumeFields AppliedFields = BaseFields;
	TestEqual(TEXT("Matching revisions apply atomically"),
		FMaterialVolumeAlgorithms::ApplyDelta(
			Delta, Applied, AppliedFields, Error),
		EMaterialVolumeDeltaApplyResult::Applied);
	TestEqual(TEXT("Applied topology hash matches the authority"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Applied),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Target));
	TestEqual(TEXT("Applied field value remains exact"),
		AppliedFields.GetEnergy(FIntVector(0, 0, 0)),
		static_cast<uint16>(700));

	FMaterialVolumeTopology Stale = Base;
	Stale.TopologyRevision += 10;
	FMaterialVolumeFields StaleFields = BaseFields;
	const uint64 StaleHash =
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Stale);
	TestEqual(TEXT("A base revision mismatch requests a snapshot"),
		FMaterialVolumeAlgorithms::ApplyDelta(
			Delta, Stale, StaleFields, Error),
		EMaterialVolumeDeltaApplyResult::SnapshotRequired);
	TestEqual(TEXT("Snapshot request does not partially mutate topology"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Stale), StaleHash);
	TestEqual(TEXT("Snapshot request does not partially mutate fields"),
		StaleFields.FieldRevision, BaseFields.FieldRevision);

	FMaterialVolumeDelta Corrupt = Delta;
	Corrupt.ResultTopologyHash ^= 1;
	FMaterialVolumeTopology Rejected = Base;
	FMaterialVolumeFields RejectedFields = BaseFields;
	TestEqual(TEXT("A result hash mismatch rejects the whole transaction"),
		FMaterialVolumeAlgorithms::ApplyDelta(
			Corrupt, Rejected, RejectedFields, Error),
		EMaterialVolumeDeltaApplyResult::Invalid);
	TestEqual(TEXT("Rejected hash leaves topology unchanged"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Rejected),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Base));
	TestEqual(TEXT("Rejected hash leaves fields unchanged"),
		RejectedFields.FieldRevision, BaseFields.FieldRevision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLegacyTerrainVolumeConversionTest,
	"MatterFlux.Volume.Terrain.LegacyHeightFieldUsesXYZBasis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLegacyTerrainVolumeConversionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	MatterFlux::PlayableLevel::FLevelTerrain Terrain;
	Terrain.Seed = 1;
	Terrain.Width = 2;
	Terrain.Height = 2;
	Terrain.CellSize = 10.0f;
	Terrain.BottomZ = 0.0f;
	Terrain.FirstCellCenter = FVector2D(5.0f, 5.0f);
	Terrain.Heights = { 10.0f, 20.0f, 0.0f, 15.0f };
	Terrain.ColorBands = { 0, 0, 0, 0 };
	Terrain.BandColors = {
		FLinearColor::White,
		FLinearColor::White,
		FLinearColor::White };
	FMaterialVolumeTopology Topology;
	FString Error;
	if (!TestTrue(TEXT("Legacy height field converts"),
		FMaterialVolumeConverters::FromLegacyTerrainXYZ(
			Terrain, Topology, Error, 7)))
	{
		AddError(Error);
		return false;
	}
	TArray<FMaterialSpan> Column;
	TestTrue(TEXT("X/Y becomes the Volume U/V column"),
		FMaterialVolumeAlgorithms::TryGetColumnSpans(
			Topology, FIntPoint(1, 0), Column));
	TestEqual(TEXT("World Z becomes Volume N"),
		Column[0].EndNExclusive, 2);
	TestEqual(TEXT("Converted terrain keeps material"),
		static_cast<int32>(Column[0].MaterialIndex), 7);
	TestTrue(TEXT("A baseline-height column in a resident chunk is queryable"),
		FMaterialVolumeAlgorithms::TryGetColumnSpans(
			Topology, FIntPoint(0, 1), Column));
	TestTrue(TEXT("A baseline-height column contains no material spans"),
		Column.IsEmpty());
	TestEqual(TEXT("Frame U follows world X"),
		Topology.GridFrame.BasisU, FVector3d::ForwardVector);
	TestEqual(TEXT("Frame V follows world Y"),
		Topology.GridFrame.BasisV, FVector3d::RightVector);
	TestEqual(TEXT("Frame N follows world Z"),
		Topology.GridFrame.BasisN, FVector3d::UpVector);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTerrainSparseSpanOverlayTest,
	"MatterFlux.Volume.Terrain.SparseOverridesAndCaveSurfaceKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxTerrainSparseSpanOverlayTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FIntPoint ColumnCoord(12, -4);
	const TArray<FMaterialSpan> Baseline = {
		FMaterialSpan(0, 8, 1) };
	const TArray<FMaterialSpan> Carved = {
		FMaterialSpan(0, 2, 1),
		FMaterialSpan(5, 8, 2) };
	FMaterialTerrainSpanOverlay Overlay;
	FString Error;
	TestTrue(TEXT("A cave column commits sparsely"),
		Overlay.CommitColumn(ColumnCoord, Baseline, Carved, Error));
	TestEqual(TEXT("One changed column is materialized"),
		Overlay.ColumnOverrides.Num(), 1);
	TestEqual(TEXT("First material edit advances revision once"),
		Overlay.Revision, 1);
	FMaterialSurfaceKey Highest;
	uint16 MaterialIndex = 0;
	TestTrue(TEXT("Highest-surface compatibility query remains available"),
		Overlay.TryGetHighestSurface(
			ColumnCoord, Baseline, Highest, MaterialIndex, Error));
	TestEqual(TEXT("Highest surface uses the upper span"), Highest.SurfaceN, 8);
	TestEqual(TEXT("Highest surface material is preserved"),
		static_cast<int32>(MaterialIndex), 2);
	TestFalse(TEXT("A specified cave height is empty"),
		Overlay.TryGetSolidAtHeight(
			ColumnCoord, 3, Baseline, MaterialIndex, Error));
	TestTrue(TEXT("A specified ceiling height is solid"),
		Overlay.TryGetSolidAtHeight(
			ColumnCoord, 6, Baseline, MaterialIndex, Error));

	TArray<FMaterialSurfaceKey> Faces;
	const TArray<FMaterialSpan> Empty;
	TestTrue(TEXT("Cave exposed faces gather from bounded neighbours"),
		FMaterialTerrainSurfaceAlgorithms::GatherExposedFaces(
			ColumnCoord,
			Carved,
			Baseline,
			Empty,
			Carved,
			Carved,
			Faces,
			Error));
	const FMaterialSurfaceKey CaveFloor{
		ColumnCoord, 2, EMaterialSurfaceFace::PositiveN };
	const FMaterialSurfaceKey CaveCeiling{
		ColumnCoord, 5, EMaterialSurfaceFace::NegativeN };
	const FMaterialSurfaceKey CaveWall{
		ColumnCoord, 5, EMaterialSurfaceFace::PositiveU };
	TestTrue(TEXT("Cave floor has a stable upward face"),
		Faces.Contains(CaveFloor));
	TestTrue(TEXT("Cave ceiling has a stable downward face"),
		Faces.Contains(CaveCeiling));
	TestTrue(TEXT("An open cave side has a stable wall face"),
		Faces.Contains(CaveWall));
	TestTrue(TEXT("Restoring generated baseline deletes the override"),
		Overlay.CommitColumn(ColumnCoord, Baseline, Baseline, Error));
	TestTrue(TEXT("No edited terrain remains materialized"),
		Overlay.ColumnOverrides.IsEmpty());
	TestEqual(TEXT("Baseline restore is one authoritative edit"),
		Overlay.Revision, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTerrainDirtyRegionSupportTest,
	"MatterFlux.Volume.Terrain.DirtyRegionSupportUsesBottomAndBoundarySeeds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxTerrainDirtyRegionSupportTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TMap<FIntPoint, TArray<FMaterialSpan>> RegionWithHalo;
	RegionWithHalo.Add(FIntPoint(0, 0), { FMaterialSpan(5, 7, 1) });
	RegionWithHalo.Add(FIntPoint(1, 0), { FMaterialSpan(5, 7, 2) });
	RegionWithHalo.Add(FIntPoint(2, 0), { FMaterialSpan(5, 7, 2) });
	RegionWithHalo.Add(FIntPoint(0, 1), {
		FMaterialSpan(0, 1, 1), FMaterialSpan(8, 9, 1) });
	RegionWithHalo.Add(FIntPoint(1, 1), { FMaterialSpan(8, 9, 1) });
	TSet<FIntVector> Supported;
	FString Error;
	TestTrue(TEXT("Dirty support analysis stays within its explicit region"),
		FMaterialTerrainSupportAlgorithms::GatherSupportedCellsInDirtyRegion(
			RegionWithHalo,
			FIntRect(FIntPoint(0, 0), FIntPoint(2, 2)),
			0,
			32,
			Supported,
			Error));
	TestEqual(TEXT("Boundary and bottom seeds support five dirty cells"),
		Supported.Num(), 5);
	TestTrue(TEXT("Halo connectivity seeds the dirty boundary"),
		Supported.Contains(FIntVector(0, 0, 5))
			&& Supported.Contains(FIntVector(1, 0, 6)));
	TestTrue(TEXT("World bottom seeds vertical support"),
		Supported.Contains(FIntVector(0, 1, 0)));
	TestFalse(TEXT("Disconnected floating cells remain unsupported"),
		Supported.Contains(FIntVector(0, 1, 8))
			|| Supported.Contains(FIntVector(1, 1, 8)));
	TestFalse(TEXT("Visit budget rejects an oversized dirty transaction"),
		FMaterialTerrainSupportAlgorithms::GatherSupportedCellsInDirtyRegion(
			RegionWithHalo,
			FIntRect(FIntPoint(0, 0), FIntPoint(2, 2)),
			0,
			4,
			Supported,
			Error));
	TestTrue(TEXT("Budget rejection exposes no partial support set"),
		Supported.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialSpanNormalizationTest,
	"MatterFlux.Volume.Core.SpanNormalizationAndDifference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialSpanNormalizationTest::RunTest(
	const FString& Parameters)
{
	TArray<FMaterialSpan> Spans = {
		FMaterialSpan(7, 10, 3),
		FMaterialSpan(0, 3, 3),
		FMaterialSpan(3, 7, 3)
	};
	FString Error;
	TestTrue(TEXT("Compatible adjacent spans normalize"),
		FMaterialSpanAlgorithms::Normalize(Spans, Error));
	TestEqual(TEXT("Adjacent compatible spans merge"), Spans.Num(), 1);
	TestEqual(TEXT("Merged span begins at zero"), Spans[0].BeginN, 0);
	TestEqual(TEXT("Merged span keeps exclusive end"), Spans[0].EndNExclusive, 10);

	const TArray<FMaterialSpan> Difference =
		FMaterialSpanAlgorithms::SubtractInterval(Spans, 3, 7);
	TestEqual(TEXT("Middle subtraction creates two spans"), Difference.Num(), 2);
	TestEqual(TEXT("Left result ends at cut begin"), Difference[0].EndNExclusive, 3);
	TestEqual(TEXT("Right result begins at cut end"), Difference[1].BeginN, 7);

	TArray<FMaterialSpan> Invalid = {
		FMaterialSpan(0, 4, 1),
		FMaterialSpan(3, 5, 2)
	};
	TestFalse(TEXT("Overlapping material spans are rejected"),
		FMaterialSpanAlgorithms::Normalize(Invalid, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialVolumeAdapterHashTest,
	"MatterFlux.Volume.Core.AdapterIndependentLogicalHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialVolumeAdapterHashTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxVolumeTests;
	const FMaterialVolumeTopology SpanTopology =
		BuildTopology(BuildTwoCellChunk(EMaterialVolumeStorageAdapter::Span));
	const FMaterialVolumeTopology DenseTopology =
		BuildTopology(BuildTwoCellChunk(EMaterialVolumeStorageAdapter::Dense));
	TestEqual(TEXT("Physical adapter does not affect logical hash"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(SpanTopology),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(DenseTopology));

	FMaterialVolumeInstance Instance;
	Instance.InstanceId = FGuid::NewGuid();
	Instance.Topology = SpanTopology;
	const uint64 Before = FMaterialVolumeAlgorithms::ComputeLogicalHash(
		Instance.Topology);
	Instance.WorldTransform = FTransform(
		FRotator(25.0, 40.0, 10.0),
		FVector(1000.0, -250.0, 375.0));
	TestEqual(TEXT("World pose is outside topology hash"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Instance.Topology),
		Before);
	Instance.Topology.TopologyRevision = 99;
	TestEqual(TEXT("Revision bookkeeping is outside logical topology hash"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Instance.Topology),
		Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialVolumeBooleanCutTest,
	"MatterFlux.Volume.Core.ImmutableBooleanCutAndIdempotence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialVolumeBooleanCutTest::RunTest(
	const FString& Parameters)
{
	FMaterialVolumeChunkBuilder Builder(FIntPoint::ZeroValue, 16);
	Builder.SetColumnSpans(FIntPoint::ZeroValue, { FMaterialSpan(0, 5, 1) });
	FMaterialVolumeChunkSnapshot Chunk;
	FString Error;
	TestTrue(TEXT("Cut source chunk builds"),
		Builder.Build(EMaterialVolumeStorageAdapter::Span, Chunk, Error));
	FMaterialVolumeTopology Source = MatterFluxVolumeTests::BuildTopology(Chunk);
	const FMaterialVolumeCut Sphere = FMaterialVolumeCut::MakeSphere(
		FVector(5.0f, 5.0f, 25.0f), 6.0);
	FMaterialVolumeTopology Cut;
	TestTrue(TEXT("Sphere subtraction commits"),
		FMaterialVolumeAlgorithms::Subtract(Source, Sphere, Cut, Error));
	TArray<FMaterialSpan> Spans;
	TestTrue(TEXT("Cut column remains addressable"),
		FMaterialVolumeAlgorithms::TryGetColumnSpans(
			Cut, FIntPoint::ZeroValue, Spans));
	TestEqual(TEXT("Middle-cell cut splits one span"), Spans.Num(), 2);
	TestTrue(TEXT("Cut uses half-open output spans"),
		Spans[0] == FMaterialSpan(0, 2, 1)
			&& Spans[1] == FMaterialSpan(3, 5, 1));
	TestEqual(TEXT("Occupancy edit increments topology revision once"),
		Cut.TopologyRevision, Source.TopologyRevision + 1);
	FMaterialVolumeTopology Repeated;
	TestTrue(TEXT("Repeated cut succeeds"),
		FMaterialVolumeAlgorithms::Subtract(Cut, Sphere, Repeated, Error));
	TestEqual(TEXT("Repeated cut does not manufacture a revision"),
		Repeated.TopologyRevision, Cut.TopologyRevision);
	TestEqual(TEXT("Repeated cut preserves logical hash"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Repeated),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Cut));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialVolumeBoundedCutShapesTest,
	"MatterFlux.Volume.Prototype.AllBoundedCutShapesAreDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialVolumeBoundedCutShapesTest::RunTest(
	const FString& Parameters)
{
	FMaterialVolumeChunkBuilder Builder(FIntPoint::ZeroValue, 16);
	for (int32 V = 0; V < 4; ++V)
	{
		for (int32 U = 0; U < 4; ++U)
		{
			Builder.SetColumnSpans(FIntPoint(U, V), { FMaterialSpan(0, 4, 1) });
		}
	}
	FMaterialVolumeChunkSnapshot Chunk;
	FString Error;
	TestTrue(TEXT("Bounded cut fixture builds"),
		Builder.Build(EMaterialVolumeStorageAdapter::Span, Chunk, Error));
	const FMaterialVolumeTopology Source =
		MatterFluxVolumeTests::BuildTopology(Chunk);
	const TArray<FMaterialVolumeCut> Cuts = {
		FMaterialVolumeCut::MakeSphere(FVector(15.0f), 6.0),
		FMaterialVolumeCut::MakeOrientedBox(
			FTransform(FRotator(30.0f, 45.0f, 60.0f), FVector(15.0f)),
			FVector(7.0f)),
		FMaterialVolumeCut::MakeCapsule(
			FVector(5.0f), FVector(35.0f), 4.0),
		FMaterialVolumeCut::MakePlaneSlab(
			FVector(20.0f), FVector(1.0f, 1.0f, 1.0f), 3.0),
		FMaterialVolumeCut::MakeSweptBlade(
			FTransform(FRotator(0.0f, 30.0f, 0.0f),
				FVector(5.0f, 5.0f, 15.0f)),
			FTransform(FRotator(60.0f, 45.0f, 30.0f),
				FVector(35.0f, 35.0f, 25.0f)),
			FVector(6.0f)) };

	for (int32 CutIndex = 0; CutIndex < Cuts.Num(); ++CutIndex)
	{
		const FMaterialVolumeCut& Shape = Cuts[CutIndex];
		TestTrue(*FString::Printf(TEXT("Cut shape %d validates"), CutIndex),
			Shape.IsValid());
		FMaterialVolumeTopology Result;
		TestTrue(*FString::Printf(TEXT("Cut shape %d subtracts"), CutIndex),
			FMaterialVolumeAlgorithms::Subtract(Source, Shape, Result, Error));
		TArray<FMaterialVolumeComponent> Components;
		TestTrue(*FString::Printf(TEXT("Cut shape %d remains connected-data valid"), CutIndex),
			FMaterialVolumeAlgorithms::GatherComponents(Result, Components, Error));
		int64 RemainingCells = 0;
		for (const FMaterialVolumeComponent& Component : Components)
		{
			RemainingCells += Component.CellCount;
		}
		TestTrue(*FString::Printf(TEXT("Cut shape %d removes a bounded subset"), CutIndex),
			RemainingCells > 0 && RemainingCells < 64);

		FMaterialVolumeTopology Repeated;
		TestTrue(*FString::Printf(TEXT("Cut shape %d repeats"), CutIndex),
			FMaterialVolumeAlgorithms::Subtract(Result, Shape, Repeated, Error));
		TestEqual(*FString::Printf(TEXT("Cut shape %d is idempotent"), CutIndex),
			FMaterialVolumeAlgorithms::ComputeLogicalHash(Repeated),
			FMaterialVolumeAlgorithms::ComputeLogicalHash(Result));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialVolumeMassPropertiesTest,
	"MatterFlux.Volume.Core.MassCenterInertiaAndChildVelocity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialVolumeMassPropertiesTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxVolumeTests;
	const FMaterialVolumeTopology Topology =
		BuildTopology(BuildTwoCellChunk(EMaterialVolumeStorageAdapter::Dense));
	const TMap<uint16, double> Densities = { { 1, 1.0 }, { 2, 2.0 } };
	FMaterialVolumeMassProperties Properties;
	FString Error;
	TestTrue(TEXT("Mass properties compute"),
		FMaterialVolumeAlgorithms::ComputeMassProperties(
			Topology, Densities, Properties, Error));
	TestEqual(TEXT("Both logical cells contribute mass"),
		Properties.CellCount, static_cast<int64>(2));
	TestEqual(TEXT("Density and cell volume determine mass"),
		Properties.Mass, 3000.0, 0.001);
	TestEqual(TEXT("Center of mass is density weighted"),
		Properties.CenterOfMass.X, 35.0 / 3.0, 0.001);
	const FVector ChildVelocity =
		FMaterialVolumeAlgorithms::ComputeChildLinearVelocity(
			FVector(10.0f, 0.0f, 0.0f),
			FVector(0.0f, 0.0f, 2.0f),
			FVector::ZeroVector,
			FVector(0.0f, 3.0f, 0.0f));
	TestEqual(TEXT("Child velocity preserves angular point velocity"),
		ChildVelocity, FVector(4.0f, 0.0f, 0.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialVolumeConnectivityTest,
	"MatterFlux.Volume.Core.CrossMaterialConnectivityAndSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialVolumeConnectivityTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxVolumeTests;
	FMaterialVolumeTopology Topology =
		BuildTopology(BuildTwoCellChunk(EMaterialVolumeStorageAdapter::Span));
	TArray<FMaterialVolumeComponent> Components;
	FString Error;
	TestTrue(TEXT("Connectivity analysis succeeds"),
		FMaterialVolumeAlgorithms::GatherComponents(Topology, Components, Error));
	TestEqual(TEXT("Different adjacent materials remain connected"),
		Components.Num(), 1);

	Topology.StructuralSeams.Add(FStructuralSeam(
		FIntVector(0, 0, 0),
		FIntVector(1, 0, 0)));
	TestTrue(TEXT("Connectivity with seam succeeds"),
		FMaterialVolumeAlgorithms::GatherComponents(Topology, Components, Error));
	TestEqual(TEXT("Explicit face seam disconnects cells"), Components.Num(), 2);
	TestTrue(TEXT("Component order is deterministic"),
		Components[0].MinimumCell == FIntVector(0, 0, 0)
		&& Components[1].MinimumCell == FIntVector(1, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialVolumeChunkRollbackTest,
	"MatterFlux.Volume.Core.ImmutableChunkBuilderRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialVolumeChunkRollbackTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxVolumeTests;
	const FMaterialVolumeChunkSnapshot Committed =
		BuildTwoCellChunk(EMaterialVolumeStorageAdapter::Span);
	const uint64 Before = FMaterialVolumeAlgorithms::ComputeChunkLogicalHash(
		Committed);

	FMaterialVolumeChunkBuilder Builder(Committed);
	Builder.SetColumnSpans(
		FIntPoint(0, 0),
		{ FMaterialSpan(0, 4, 1), FMaterialSpan(3, 5, 2) });
	FMaterialVolumeChunkSnapshot Rejected;
	FString Error;
	TestFalse(TEXT("Invalid rebuild is rejected atomically"),
		Builder.Build(EMaterialVolumeStorageAdapter::Span, Rejected, Error));
	TestEqual(TEXT("Previously committed immutable chunk is unchanged"),
		FMaterialVolumeAlgorithms::ComputeChunkLogicalHash(Committed), Before);
	TestFalse(TEXT("Rejected build reports a useful error"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLegacyMaskVolumeConversionTest,
	"MatterFlux.Volume.Core.LegacyMaskUsesXZYBasis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLegacyMaskVolumeConversionTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 2;
	Mask.Height = 3;
	Mask.CellSize = 5.0f;
	Mask.SolidMask = {
		0, 0,
		0, 1,
		0, 0
	};
	FMaterialVolumeTopology Topology;
	FString Error;
	TestTrue(TEXT("Legacy mask converts"),
		FMaterialVolumeConverters::FromLegacyMaskXZY(
			Mask, 4, 9, Topology, Error));
	FMaterialSpan Converted;
	TestTrue(TEXT("Mask X/Z cell becomes Volume U/V column"),
		FMaterialVolumeAlgorithms::TryGetSingleSpan(
			Topology, FIntPoint(1, 1), Converted));
	TestEqual(TEXT("Extrusion starts on local Y/N"), Converted.BeginN, 4);
	TestEqual(TEXT("Extrusion ends exclusively on local Y/N"),
		Converted.EndNExclusive, 9);
	TestEqual(TEXT("Material survives conversion"),
		static_cast<int32>(Converted.MaterialIndex), 1);
	const FVector3d ConvertedCenter = Topology.GridFrame.Origin
		+ Topology.GridFrame.BasisU * 1.5 * Topology.GridFrame.CellSize
		+ Topology.GridFrame.BasisV * 1.5 * Topology.GridFrame.CellSize
		+ Topology.GridFrame.BasisN * 6.5 * Topology.GridFrame.CellSize;
	TestEqual(TEXT("Converted mask cell keeps centered legacy local X"),
		ConvertedCenter.X, 2.5, 0.001);
	TestEqual(TEXT("Converted mask row keeps centered legacy local Z"),
		ConvertedCenter.Z, 0.0, 0.001);
	TestEqual(TEXT("Converted extrusion is centered on legacy local Y"),
		ConvertedCenter.Y, 0.0, 0.001);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxVolumeCellMaterialTransactionTest,
	"MatterFlux.Volume.Core.CellMaterialTransactionIsAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxVolumeCellMaterialTransactionTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Mask;
	Mask.Width = 1;
	Mask.Height = 1;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 1;
	Mask.SolidMask = { 1 };
	FMaterialVolumeTopology Before;
	FString Error;
	TestTrue(TEXT("Single material cell converts"),
		FMaterialVolumeConverters::FromLegacyMaskXZY(
			Mask, 0, 3, Before, Error, 7));
	Before.TopologyRevision = 4;
	const uint64 BeforeHash = FMaterialVolumeAlgorithms::ComputeLogicalHash(Before);
	FMaterialVolumeTopology After;
	TestTrue(TEXT("Middle N cell changes material atomically"),
		FMaterialVolumeAlgorithms::SetCellMaterial(
			Before, FIntVector(0, 0, 1), 9, After, Error));
	uint16 Material = 0;
	TestTrue(TEXT("Changed cell is queryable"),
		FMaterialVolumeAlgorithms::TryGetCellMaterial(
			After, FIntVector(0, 0, 1), Material));
	TestEqual(TEXT("Changed cell carries new material"),
		static_cast<int32>(Material), 9);
	TestTrue(TEXT("Neighbor retains original material"),
		FMaterialVolumeAlgorithms::TryGetCellMaterial(
			After, FIntVector(0, 0, 0), Material));
	TestEqual(TEXT("Neighbor material is unchanged"),
		static_cast<int32>(Material), 7);
	TestEqual(TEXT("Material edit advances topology exactly once"),
		After.TopologyRevision, 5);
	TestNotEqual(TEXT("Logical material hash changes"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(After), BeforeHash);
	FMaterialVolumeTopology Repeated;
	TestTrue(TEXT("Repeated same material edit succeeds"),
		FMaterialVolumeAlgorithms::SetCellMaterial(
			After, FIntVector(0, 0, 1), 9, Repeated, Error));
	TestEqual(TEXT("Repeated edit does not advance topology"),
		Repeated.TopologyRevision, After.TopologyRevision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentVolumeShadowTest,
	"MatterFlux.Volume.Prototype.FragmentVolumeTopologyTracksCommittedCuts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentVolumeShadowTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	bool bPassed = TestNotNull(TEXT("Volume source spawns"), Source);
	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 3;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 8;
	Mask.SupportMode = EFragmentSupportMode::Bottom;
	Mask.SolidMask.Init(1, 9);
	const FGuid SourceId = FGuid::NewDeterministicGuid(
		TEXT("fragment.volume.shadow"), 1);
	bPassed &= Source && TestTrue(TEXT("Volume source initializes"),
		Source->InitializeFromProceduralMask(
			Mask, SourceId, FLinearColor::White, TEXT("wood")));
	FMaterialVolumeInstance Before;
	bPassed &= Source && TestTrue(TEXT("Initial Volume topology exists"),
		Source->BuildMaterialVolumeInstance(Before));
	uint16 SourceMaterialIndex = 0;
	bPassed &= TestTrue(TEXT("Source Volume carries a compiled material index"),
		FMaterialVolumeAlgorithms::TryGetCellMaterial(
			Before.Topology, FIntVector(0, 0, 0), SourceMaterialIndex));
	FMaterialVolumeMassProperties BeforeMass;
	FString Error;
	bPassed &= TestTrue(TEXT("Initial shadow mass computes"),
		FMaterialVolumeAlgorithms::ComputeMassProperties(
			Before.Topology, { { SourceMaterialIndex, 1.0 } }, BeforeMass, Error));
	bPassed &= TestEqual(TEXT("Initial shadow mirrors all mask cells"),
		BeforeMass.CellCount, static_cast<int64>(9));
	bPassed &= TestTrue(TEXT("Retained Volume cell accepts field-only heat"),
		Source->CommitMaterialVolumeCellEnergy(
			FIntVector(0, 0, 0), 100, 100, 700));
	bPassed &= TestTrue(TEXT("Soon-to-be-cut cell accepts independent heat"),
		Source->CommitMaterialVolumeCellEnergy(
			FIntVector(1, 2, 0), 100, 100, 800));
	FMaterialVolumeInstance Heated;
	bPassed &= TestTrue(TEXT("Heated Volume instance is queryable"),
		Source->BuildMaterialVolumeInstance(Heated));
	bPassed &= TestEqual(TEXT("Field edits do not change topology revision"),
		Heated.Topology.TopologyRevision, Before.Topology.TopologyRevision);
	bPassed &= TestEqual(TEXT("Each committed field edit advances field revision"),
		Heated.Fields.FieldRevision, 2);

	FFragmentDamageEvent Damage;
	Damage.SourceId = SourceId;
	Damage.BaseRevision = 0;
	Damage.DamageShape.Type = EFragmentDamageShapeType::Circle;
	Damage.DamageShape.WorldTransform = Source->GetActorTransform();
	Damage.DamageShape.WorldTransform.SetLocation(
		Source->GetActorTransform().TransformPosition(FVector(0.0, 0.0, 10.0)));
	Damage.DamageShape.Radius = 4.0f;
	Damage.DamagePower = 1000.0f;
	TArray<FFragmentSpawnPayload> Payloads;
	bPassed &= TestTrue(TEXT("Legacy cut commits"),
		Source->ApplyDamageEvent(Damage, Payloads));
	FMaterialVolumeInstance After;
	bPassed &= TestTrue(TEXT("Committed cut refreshes Volume topology"),
		Source->BuildMaterialVolumeInstance(After));
	FMaterialVolumeMassProperties AfterMass;
	bPassed &= TestTrue(TEXT("Cut shadow mass computes"),
		FMaterialVolumeAlgorithms::ComputeMassProperties(
			After.Topology, { { SourceMaterialIndex, 1.0 } }, AfterMass, Error));
	bPassed &= TestEqual(TEXT("Shadow and canonical mask occupancy agree"),
		AfterMass.CellCount,
		static_cast<int64>(Source->GetRemainingInputCellCount()));
	bPassed &= TestEqual(TEXT("Shadow topology revision follows source commit"),
		After.Topology.TopologyRevision, Source->Revision);
	bPassed &= TestEqual(TEXT("Retained cell heat survives topology rebuild"),
		static_cast<int32>(After.Fields.GetEnergy(FIntVector(0, 0, 0))), 700);
	bPassed &= TestFalse(TEXT("Cut cells retain no sparse energy fact"),
		After.Fields.EnergyOverrides.Contains(FIntVector(1, 2, 0)));
	bPassed &= TestEqual(TEXT("Pruning cut field values advances only field revision"),
		After.Fields.FieldRevision, 3);
	FFragment2DSourceStreamingState StreamingState;
	bPassed &= TestTrue(TEXT("V6 streaming state captures Volume fields"),
		Source->CaptureStreamingState(StreamingState, Error));
	AFragment2DSourceActor* RestoredSource = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	bPassed &= TestNotNull(TEXT("Restore target source spawns"), RestoredSource);
	bPassed &= RestoredSource && TestTrue(TEXT("Restore target initializes"),
		RestoredSource->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(TEXT("fragment.volume.restore"), 2),
			FLinearColor::White,
			TEXT("wood")));
	bPassed &= RestoredSource && TestTrue(TEXT("Volume streaming state restores"),
		RestoredSource->RestoreStreamingState(StreamingState, Error));
	FMaterialVolumeInstance Restored;
	bPassed &= RestoredSource && TestTrue(TEXT("Restored Volume is queryable"),
		RestoredSource->BuildMaterialVolumeInstance(Restored));
	bPassed &= TestEqual(TEXT("Restored topology hash matches"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Restored.Topology),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(After.Topology));
	bPassed &= TestEqual(TEXT("Restored field revision matches"),
		Restored.Fields.FieldRevision, After.Fields.FieldRevision);
	bPassed &= TestEqual(TEXT("Restored cell energy matches"),
		static_cast<int32>(Restored.Fields.GetEnergy(FIntVector(0, 0, 0))),
		700);
	return bPassed;
}
