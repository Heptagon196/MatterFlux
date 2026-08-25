#include "Misc/AutomationTest.h"
#include "Rendering/MatterFluxLiquidSurfaceProjection.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceBuildsExposedSideWallsTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionBuildsExposedSideWalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceBuildsExposedSideWallsTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	const TArray<FCellSnapshot> Cells = {
		{ FIntPoint(4, -2), TEXT("water"), 35, 128 }
	};

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		10.0f,
		100.0f,
		Projection);

	TestEqual(TEXT("One occupied column owns exactly two top triangles"),
		Projection.TopTriangleIndexCount, 6);
	TestTrue(TEXT("An exposed liquid column also owns vertical side triangles"),
		Projection.Triangles.Num() > Projection.TopTriangleIndexCount);
	float MinimumProjectedZ = TNumericLimits<float>::Max();
	for (int32 VertexIndex = 0; VertexIndex < Projection.Vertices.Num();
		++VertexIndex)
	{
		const FVector Vertex = Projection.Vertices[VertexIndex];
		MinimumProjectedZ = FMath::Min(MinimumProjectedZ, Vertex.Z);
	}
	TestEqual(TEXT("The side envelope reaches the canonical support height"),
		MinimumProjectedZ, 35.0f, 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceUsesConnectedShapeTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionUsesConnectedShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceUsesConnectedShapeTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	Cells.Add({ FIntPoint(0, 0), TEXT("water"), 0, 255 });
	Cells.Add({ FIntPoint(1, 0), TEXT("water"), 20, 179 });

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		10.0f,
		100.0f,
		Projection);

	TestEqual(TEXT("Adjacent liquid facts form one external shape"),
		Projection.SurfacePatchCount, 1);
	TestTrue(TEXT("Two adjacent cells share top vertices and add only envelope vertices"),
		Projection.Vertices.Num() >= 6);
	TestEqual(TEXT("The connected shape retains both top faces"),
		Projection.TopTriangleIndexCount, 12);

	FBox Bounds(ForceInit);
	for (const FVector Vertex : Projection.Vertices)
	{
		Bounds += Vertex;
	}
	TestEqual(TEXT("Amount does not shrink the projected footprint"),
		Bounds.GetSize().X, 20.0);
	TestEqual(TEXT("The shared surface spans one cell in Y"),
		Bounds.GetSize().Y, 10.0);

	int32 SharedEdgeVertexCount = 0;
	for (int32 VertexIndex = 0; VertexIndex < Projection.Vertices.Num();
		++VertexIndex)
	{
		const FVector Vertex = Projection.Vertices[VertexIndex];
		if (FMath::IsNearlyEqual(Vertex.X, 10.0f))
		{
			if (!Projection.Normals[VertexIndex].Equals(FVector::UpVector))
			{
				continue;
			}
			++SharedEdgeVertexCount;
			TestTrue(
				TEXT("Shared corners blend both column heights without a crack"),
				Vertex.Z > 70.0f && Vertex.Z < 100.0f);
		}
	}
	TestEqual(TEXT("The common edge exists exactly once"),
		SharedEdgeVertexCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceSeparatesDisconnectedShapesTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionSeparatesDisconnectedShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceSeparatesDisconnectedShapesTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	Cells.Add({ FIntPoint(-4, 2), TEXT("water"), 3, 255 });
	Cells.Add({ FIntPoint(8, 2), TEXT("water"), 3, 255 });

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		12.0f,
		90.0f,
		Projection);

	TestEqual(TEXT("Disconnected facts remain separate liquid shapes"),
		Projection.SurfacePatchCount, 2);
	TestTrue(TEXT("Separate droplets do not collapse their projection vertices"),
		Projection.Vertices.Num() >= 8);
	TestEqual(TEXT("Each droplet owns two top triangles"),
		Projection.TopTriangleIndexCount, 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceSeparatesWaterfallHeightsTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionSeparatesWaterfallHeights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceSeparatesWaterfallHeightsTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	Cells.Add({ FIntPoint(0, 0), TEXT("water"), 0, 255 });
	Cells.Add({ FIntPoint(1, 0), TEXT("water"), 180, 255 });

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		16.0f,
		128.0f,
		Projection);

	TestEqual(TEXT("A large fall becomes two continuous surface patches"),
		Projection.SurfacePatchCount, 2);
	TestTrue(TEXT("The fall cannot collapse its separate top patches"),
		Projection.Vertices.Num() >= 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfacePreservesBasinParticlesTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionPreservesBasinParticles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfacePreservesBasinParticlesTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	for (int32 Y = 0; Y < 7; ++Y)
	{
		for (int32 X = 0; X < 7; ++X)
		{
			if (Y == 3 && X <= 3)
			{
				continue;
			}
			const bool bTransientLowColumn = X >= 2 && X <= 4
				&& Y >= 2 && Y <= 4;
			Cells.Add({
				FIntPoint(X, Y),
				TEXT("water"),
				0,
				static_cast<uint8>(bTransientLowColumn ? 1 : 255) });
		}
	}

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		10.0f,
		100.0f,
		Projection);

	TestEqual(TEXT("The canonical hole is not inserted into MaterialWorld"),
		Cells.Num(), 45);
	TestEqual(TEXT("Projection does not invent particles inside an open trail"),
		Projection.ProjectedCellCount, 45);
	TestEqual(TEXT("Every current particle column owns one top face"),
		Projection.TopTriangleIndexCount, 45 * 6);
	float MinimumSurfaceHeight = TNumericLimits<float>::Max();
	for (const FVector Vertex : Projection.Vertices)
	{
		MinimumSurfaceHeight = FMath::Min(
			MinimumSurfaceHeight,
			static_cast<float>(Vertex.Z));
	}
	TestTrue(TEXT("A low current particle column is not lifted for presentation"),
		MinimumSurfaceHeight < 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfacePreservesWideOpenTrailTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionPreservesWideOpenTrail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfacePreservesWideOpenTrailTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	for (int32 Y = 0; Y < 25; ++Y)
	{
		for (int32 X = 0; X < 25; ++X)
		{
			// Seventeen cells wide and connected to the exterior: this covers
			// both the player capsule and the larger runtime creature wake.
			if (Y >= 4 && Y <= 20 && X <= 18)
			{
				continue;
			}
			Cells.Add({ FIntPoint(X, Y), TEXT("water"), 0, 255 });
		}
	}

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		10.0f,
		100.0f,
		Projection);

	TestEqual(TEXT("Projection never changes canonical falling-sand facts"),
		Cells.Num(), 302);
	TestEqual(TEXT("Projection preserves a wide shore-connected vacancy"),
		Projection.ProjectedCellCount, 302);
	TestEqual(TEXT("Current occupied columns render one face each"),
		Projection.TopTriangleIndexCount, 302 * 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfacePreservesShoreConnectedWakeTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionPreservesShoreConnectedWake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfacePreservesShoreConnectedWakeTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	for (int32 Y = 0; Y < 41; ++Y)
	{
		for (int32 X = 0; X < 41; ++X)
		{
			// A 25-cell-wide vacancy connected to the left shore is wider than
			// the round closing kernel. Its vertical banks still identify it as an
			// internal wake in the current outer fluid body.
			if (Y >= 8 && Y <= 32 && X <= 30)
			{
				continue;
			}
			Cells.Add({ FIntPoint(X, Y), TEXT("water"), 0, 255 });
		}
	}

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		10.0f,
		100.0f,
		Projection);

	TestEqual(TEXT("Directional shell reconstruction preserves canonical facts"),
		Cells.Num(), 906);
	TestEqual(TEXT("Outer shape does not bridge a current shore-connected vacancy"),
		Projection.ProjectedCellCount, 906);
	TestEqual(TEXT("Only current occupied columns render top faces"),
		Projection.TopTriangleIndexCount, 906 * 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceChunkUsesHaloWithoutBoundaryWallsTest,
	"MatterFlux.Playable.Liquid.SurfaceChunkUsesHaloWithoutBoundaryWalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceChunkUsesHaloWithoutBoundaryWallsTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	const TArray<FCellSnapshot> Cells = {
		{ FIntPoint(3, 0), TEXT("water"), 0, 255 },
		{ FIntPoint(4, 0), TEXT("water"), 0, 255 }
	};
	MatterFlux::Rendering::FLiquidSurfaceProjection LeftChunk;
	MatterFlux::Rendering::BuildLiquidSurfaceChunkProjection(
		Cells, 10.0f, 100.0f, FIntPoint(0, 0), 4, LeftChunk);

	TestEqual(TEXT("Only the core cell emits a top face"),
		LeftChunk.TopTriangleIndexCount, 6);
	bool bHasFalseEastBoundaryWall = false;
	for (int32 VertexIndex = 0; VertexIndex < LeftChunk.Vertices.Num();
		++VertexIndex)
	{
		bHasFalseEastBoundaryWall |=
			FMath::IsNearlyEqual(LeftChunk.Vertices[VertexIndex].X, 40.0f)
			&& LeftChunk.Normals[VertexIndex].Equals(FVector::XAxisVector);
	}
	TestFalse(TEXT("A same-liquid halo cell suppresses the chunk seam wall"),
		bHasFalseEastBoundaryWall);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidChunksPartitionDeterministicallyTest,
	"MatterFlux.Playable.Liquid.ProjectionChunksUseCheckerboardPasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidChunksPartitionDeterministicallyTest::RunTest(
	const FString& Parameters)
{
	const TArray<FIntPoint> Chunks = {
		FIntPoint(1, 1), FIntPoint(0, 1), FIntPoint(1, 0),
		FIntPoint(0, 0), FIntPoint(0, 0), FIntPoint(-1, 0)
	};
	TArray<FIntPoint> EvenChunks;
	TArray<FIntPoint> OddChunks;
	MatterFlux::Rendering::PartitionLiquidProjectionChunksCheckerboard(
		Chunks, EvenChunks, OddChunks);
	TestEqual(TEXT("Duplicate chunk requests collapse"),
		EvenChunks.Num() + OddChunks.Num(), 5);
	for (const FIntPoint Even : EvenChunks)
	{
		for (const FIntPoint Other : EvenChunks)
		{
			if (Even != Other)
			{
				TestTrue(TEXT("One checkerboard pass contains no cardinal neighbours"),
					FMath::Abs(Even.X - Other.X)
						+ FMath::Abs(Even.Y - Other.Y) != 1);
			}
		}
	}
	for (const FIntPoint Odd : OddChunks)
	{
		for (const FIntPoint Other : OddChunks)
		{
			if (Odd != Other)
			{
				TestTrue(TEXT("The second pass contains no cardinal neighbours"),
					FMath::Abs(Odd.X - Other.X)
						+ FMath::Abs(Odd.Y - Other.Y) != 1);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfacePreservesWideLowWakeTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionPreservesWideLowWake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfacePreservesWideLowWakeTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	for (int32 Y = 0; Y < 41; ++Y)
	{
		for (int32 X = 0; X < 41; ++X)
		{
			const bool bLowWake = Y >= 8 && Y <= 32;
			Cells.Add({
				FIntPoint(X, Y),
				TEXT("water"),
				0,
				static_cast<uint8>(bLowWake ? 1 : 255) });
		}
	}

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		10.0f,
		100.0f,
		Projection);

	float MinimumVertexHeight = TNumericLimits<float>::Max();
	for (const FVector Vertex : Projection.Vertices)
	{
		MinimumVertexHeight = FMath::Min(
			MinimumVertexHeight, static_cast<float>(Vertex.Z));
	}
	TestEqual(TEXT("Low wake remains canonical material input"),
		Cells.Num(), 41 * 41);
	TestTrue(TEXT("Low current columns remain low in the disposable mesh"),
		MinimumVertexHeight < 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceProjectionEnvelopeIsDisposableTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionEnvelopeIsDisposable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceProjectionEnvelopeIsDisposableTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> CanonicalCells;
	CanonicalCells.Add({ FIntPoint(1, 1), TEXT("water"), 0, 1 });

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		CanonicalCells,
		10.0f,
		100.0f,
		Projection);

	TestEqual(TEXT("Projection history cannot mutate canonical material input"),
		CanonicalCells.Num(), 1);
	TestEqual(TEXT("No historical silhouette survives current material facts"),
		Projection.ProjectedCellCount, 1);
	TestTrue(TEXT("Projection history cannot lift the current material fact"),
		Projection.SurfaceHeights.FindChecked(FIntPoint(1, 1)) <= 1.0f);
	float MaximumProjectedHeight = -TNumericLimits<float>::Max();
	for (const FVector Vertex : Projection.Vertices)
	{
		MaximumProjectedHeight = FMath::Max(
			MaximumProjectedHeight,
			static_cast<float>(Vertex.Z));
	}
	TestTrue(TEXT("Historical silhouette uses the current liquid level"),
		MaximumProjectedHeight <= 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfacePreservesLocalCanonicalHeightsTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionPreservesLocalCanonicalHeights",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfacePreservesLocalCanonicalHeightsTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	Cells.Add({ FIntPoint(0, 0), TEXT("water"), 0, 255 });
	Cells.Add({ FIntPoint(1, 0), TEXT("water"), 8, 255 });

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		10.0f,
		100.0f,
		Projection);

	TArray<float> LeftOuterHeights;
	TArray<float> SharedEdgeHeights;
	TArray<float> RightOuterHeights;
	for (int32 VertexIndex = 0; VertexIndex < Projection.Vertices.Num();
		++VertexIndex)
	{
		if (!Projection.Normals[VertexIndex].Equals(FVector::UpVector))
		{
			continue;
		}
		const FVector Vertex = Projection.Vertices[VertexIndex];
		if (FMath::IsNearlyEqual(Vertex.X, 0.0f))
		{
			LeftOuterHeights.Add(Vertex.Z);
		}
		else if (FMath::IsNearlyEqual(Vertex.X, 10.0f))
		{
			SharedEdgeHeights.Add(Vertex.Z);
		}
		else if (FMath::IsNearlyEqual(Vertex.X, 20.0f))
		{
			RightOuterHeights.Add(Vertex.Z);
		}
	}
	TestEqual(TEXT("Left shoreline owns two corners"),
		LeftOuterHeights.Num(), 2);
	TestEqual(TEXT("Shared edge owns two corners"),
		SharedEdgeHeights.Num(), 2);
	TestEqual(TEXT("Right shoreline owns two corners"),
		RightOuterHeights.Num(), 2);
	for (const float Height : LeftOuterHeights)
	{
		TestEqual(TEXT("Lower column stays at its canonical surface"),
			Height, 100.0f, 0.01f);
	}
	for (const float Height : SharedEdgeHeights)
	{
		TestEqual(TEXT("Shared corners interpolate adjacent material facts"),
			Height, 104.0f, 0.01f);
	}
	for (const float Height : RightOuterHeights)
	{
		TestEqual(TEXT("Higher column stays at its canonical surface"),
			Height, 108.0f, 0.01f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceSeparatesAbruptCurrentStepTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionSeparatesAbruptCurrentStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceSeparatesAbruptCurrentStepTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	Cells.Add({ FIntPoint(0, 0), TEXT("water"), 0, 255 });
	Cells.Add({ FIntPoint(1, 0), TEXT("water"), 60, 255 });

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		16.0f,
		128.0f,
		Projection);

	TestEqual(TEXT("A column-scale step becomes two current-height patches"),
		Projection.SurfacePatchCount, 2);
	for (int32 TriangleIndex = 0;
		TriangleIndex + 2 < Projection.TopTriangleIndexCount;
		TriangleIndex += 3)
	{
		const float Height0 = Projection.Vertices[
			Projection.Triangles[TriangleIndex]].Z;
		const float Height1 = Projection.Vertices[
			Projection.Triangles[TriangleIndex + 1]].Z;
		const float Height2 = Projection.Vertices[
			Projection.Triangles[TriangleIndex + 2]].Z;
		TestTrue(TEXT("No triangle stretches across the abrupt height step"),
			FMath::Max3(Height0, Height1, Height2)
				- FMath::Min3(Height0, Height1, Height2) <= 0.01f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceKeepsGradualRiverContinuousTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionKeepsGradualRiverContinuous",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceKeepsGradualRiverContinuousTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	for (int32 X = 0; X < 12; ++X)
	{
		Cells.Add({ FIntPoint(X, 0), TEXT("water"),
			static_cast<int16>(X * 8), 255 });
	}

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		16.0f,
		128.0f,
		Projection);

	TestEqual(
		TEXT("A river whose local steps are gentle remains one continuous surface"),
		Projection.SurfacePatchCount,
		1);
	for (int32 TriangleIndex = 0;
		TriangleIndex + 2 < Projection.TopTriangleIndexCount;
		TriangleIndex += 3)
	{
		const float Height0 = Projection.Vertices[
			Projection.Triangles[TriangleIndex]].Z;
		const float Height1 = Projection.Vertices[
			Projection.Triangles[TriangleIndex + 1]].Z;
		const float Height2 = Projection.Vertices[
			Projection.Triangles[TriangleIndex + 2]].Z;
		TestTrue(TEXT("Every river triangle follows only its local gentle slope"),
			FMath::Max3(Height0, Height1, Height2)
				- FMath::Min3(Height0, Height1, Height2) <= 8.01f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfacePreservesTransitiveGentleSlopeTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionPreservesTransitiveGentleSlope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfacePreservesTransitiveGentleSlopeTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	// Every traversable link around this 2x2 loop differs by at most 16cm. The
	// complete component spans 48cm, but the water surface is still one locally
	// gentle slope rather than several bands with artificial internal walls.
	TArray<FCellSnapshot> Cells;
	Cells.Add({ FIntPoint(0, 0), TEXT("water"), 0, 255 });
	Cells.Add({ FIntPoint(1, 0), TEXT("water"), 16, 255 });
	Cells.Add({ FIntPoint(0, 1), TEXT("water"), 48, 255 });
	Cells.Add({ FIntPoint(1, 1), TEXT("water"), 32, 255 });

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		16.0f,
		128.0f,
		Projection);

	TestEqual(TEXT("Transitive locally gentle staircase stays connected"),
		Projection.SurfacePatchCount, 1);
	for (int32 TriangleIndex = 0;
		TriangleIndex + 2 < Projection.TopTriangleIndexCount;
		TriangleIndex += 3)
	{
		const float Height0 = Projection.Vertices[
			Projection.Triangles[TriangleIndex]].Z;
		const float Height1 = Projection.Vertices[
			Projection.Triangles[TriangleIndex + 1]].Z;
		const float Height2 = Projection.Vertices[
			Projection.Triangles[TriangleIndex + 2]].Z;
		TestTrue(TEXT("No triangle exceeds one local continuous step"),
			FMath::Max3(Height0, Height1, Height2)
				- FMath::Min3(Height0, Height1, Height2) <= 32.01f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidSurfaceDoesNotLiftLakeToConnectedRiverTest,
	"MatterFlux.Playable.Liquid.SurfaceProjectionDoesNotLiftLakeToConnectedRiver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidSurfaceDoesNotLiftLakeToConnectedRiverTest::RunTest(
	const FString& Parameters)
{
	using MatterFlux::Material::FCellSnapshot;
	TArray<FCellSnapshot> Cells;
	for (int32 Y = 0; Y < 40; ++Y)
	{
		for (int32 X = 0; X < 50; ++X)
		{
			// The high channel occupies 40% of this connected body. The old 75th
			// percentile continuum lifted the larger lake to the channel surface.
			Cells.Add({
				FIntPoint(X, Y),
				TEXT("water"),
				static_cast<uint16>(X < 30 ? 0 : 80),
				255 });
		}
	}

	MatterFlux::Rendering::FLiquidSurfaceProjection Projection;
	MatterFlux::Rendering::BuildLiquidSurfaceProjection(
		Cells,
		16.0f,
		100.0f,
		Projection);

	TestEqual(TEXT("Broad lake remains at its canonical level"),
		Projection.SurfaceHeights.FindChecked(FIntPoint(10, 20)), 100.0f, 0.01f);
	TestEqual(TEXT("Connected high channel keeps its own level"),
		Projection.SurfaceHeights.FindChecked(FIntPoint(40, 20)), 180.0f, 0.01f);
	return true;
}

#endif
