#include "Material/MatterFluxMaterialWorld.h"
#include "Material/MatterFluxMaterialSimulationRuntime.h"
#include "Game/MatterFluxGroundStateChunkActor.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Misc/AutomationTest.h"

#include <limits>

namespace
{
	FMatterFluxContentRegistry MakeLiquidRegistry()
	{
		FMatterFluxContentRegistry Registry;

		FMatterFluxMaterialDefinition Water;
		Water.Id = TEXT("water");
		Water.Density = 1.0f;
		Water.Phase = EMatterFluxMaterialPhase::Liquid;
		Water.Mobility = 255;
		Registry.Materials.Add(Water.Id, Water);

		FMatterFluxMaterialDefinition Stone;
		Stone.Id = TEXT("stone");
		Stone.Density = 3.0f;
		Stone.Phase = EMatterFluxMaterialPhase::StaticSolid;
		Registry.Materials.Add(Stone.Id, Stone);

		FMatterFluxMaterialDefinition Sand;
		Sand.Id = TEXT("sand");
		Sand.Density = 1.8f;
		Sand.Phase = EMatterFluxMaterialPhase::Powder;
		Sand.Mobility = 255;
		Registry.Materials.Add(Sand.Id, Sand);

		FMatterFluxMaterialDefinition Steam;
		Steam.Id = TEXT("steam");
		Steam.Density = 0.1f;
		Steam.Phase = EMatterFluxMaterialPhase::Gas;
		Steam.Mobility = 255;
		Steam.Dispersion = 255;
		Registry.Materials.Add(Steam.Id, Steam);

		FMatterFluxMaterialDefinition Lava;
		Lava.Id = TEXT("lava");
		Lava.Density = 2.8f;
		Lava.Phase = EMatterFluxMaterialPhase::Liquid;
		Lava.Mobility = 96;
		Registry.Materials.Add(Lava.Id, Lava);

		FMatterFluxMaterialDefinition Acid;
		Acid.Id = TEXT("acid");
		Acid.Density = 1.22f;
		Acid.Phase = EMatterFluxMaterialPhase::Liquid;
		Acid.Mobility = 255;
		Acid.Dispersion = 210;
		Registry.Materials.Add(Acid.Id, Acid);

		FMatterFluxMaterialDefinition AcidGas;
		AcidGas.Id = TEXT("acid_gas");
		AcidGas.Density = 0.07f;
		AcidGas.Phase = EMatterFluxMaterialPhase::Gas;
		AcidGas.Mobility = 255;
		AcidGas.Dispersion = 255;
		Registry.Materials.Add(AcidGas.Id, AcidGas);

		FMatterFluxMaterialDefinition Wood;
		Wood.Id = TEXT("wood");
		Wood.Density = 0.82f;
		Wood.Phase = EMatterFluxMaterialPhase::StaticSolid;
		Registry.Materials.Add(Wood.Id, Wood);

		FMatterFluxMaterialDefinition Grassland;
		Grassland.Id = TEXT("grassland");
		Grassland.Density = 0.42f;
		Grassland.Phase = EMatterFluxMaterialPhase::StaticSolid;
		Registry.Materials.Add(Grassland.Id, Grassland);

		FMatterFluxReactionDefinition Quench;
		Quench.Id = TEXT("water_lava_quench");
		Quench.InputA = TEXT("water");
		Quench.InputB = TEXT("lava");
		Quench.OutputA = TEXT("steam");
		Quench.OutputB = TEXT("stone");
		Quench.ChancePermille = 1000;
		Registry.Reactions.Add(Quench.Id, Quench);

		FMatterFluxReactionDefinition CorrodeWood;
		CorrodeWood.Id = TEXT("acid_wood_corrosion");
		CorrodeWood.InputA = TEXT("acid");
		CorrodeWood.InputB = TEXT("wood");
		CorrodeWood.OutputA = TEXT("acid");
		CorrodeWood.OutputB = TEXT("acid_gas");
		CorrodeWood.ChancePermille = 1000;
		Registry.Reactions.Add(CorrodeWood.Id, CorrodeWood);

		FMatterFluxReactionDefinition CorrodeGrassland;
		CorrodeGrassland.Id = TEXT("acid_grassland_corrosion");
		CorrodeGrassland.InputA = TEXT("acid");
		CorrodeGrassland.InputB = TEXT("grassland");
		CorrodeGrassland.OutputA = TEXT("acid");
		CorrodeGrassland.OutputB = TEXT("acid_gas");
		CorrodeGrassland.ChancePermille = 1000;
		Registry.Reactions.Add(CorrodeGrassland.Id, CorrodeGrassland);
		return Registry;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDynamicBodyLiquidDisplacementTest,
	"MatterFlux.Material.DynamicBodyDisplacesLiquidAndConservesAmount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDynamicBodyLiquidDisplacementTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-4, -4);
	Settings.MaxSurfaceCellExclusive = FIntPoint(5, 5);
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Displacement world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 20260822, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<MatterFlux::Material::FSeedCell> Seeds;
	for (int32 Y = -4; Y <= 4; ++Y)
	{
		for (int32 X = -4; X <= 4; ++X)
		{
			const bool bWater = X == 0 && Y == 0;
			Seeds.Add({
				FIntPoint(X, Y),
				bWater ? FName(TEXT("water")) : NAME_None,
				0,
				bWater ? static_cast<uint8>(137) : static_cast<uint8>(0) });
		}
	}
	TestTrue(TEXT("Surface state is seeded"), World.SeedSurface(Seeds));

	const FIntPoint OccupiedFootprint[] = {
		FIntPoint::ZeroValue,
		FIntPoint(1, 0)
	};
	TestEqual(TEXT("One overlapping liquid cell is displaced"),
		World.DisplaceLiquids(OccupiedFootprint), 1);
	TestTrue(TEXT("Body center is materially empty"),
		World.GetMaterialAt(FIntPoint::ZeroValue).IsNone());
	TestTrue(TEXT("Whole body footprint remains materially empty"),
		World.GetMaterialAt(FIntPoint(1, 0)).IsNone());
	TestEqual(TEXT("Liquid cell identity is conserved"),
		World.CountMaterial(TEXT("water")), 1);

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	World.GetActiveCells(Cells);
	const MatterFlux::Material::FCellSnapshot* Water =
		Cells.FindByPredicate(
			[](const MatterFlux::Material::FCellSnapshot& Cell)
			{
				return Cell.MaterialId == TEXT("water");
			});
	if (!TestNotNull(TEXT("Displaced liquid has a destination"), Water))
	{
		return false;
	}
	TestEqual(TEXT("Liquid amount is conserved exactly"),
		Water->Amount, static_cast<uint8>(137));
	TestEqual(TEXT("World-level amount query observes exact conservation"),
		World.SumMaterialAmount(TEXT("water")), static_cast<int64>(137));
	TestFalse(TEXT("Destination is outside submitted body footprint"),
		Water->WorldCell == FIntPoint::ZeroValue
			|| Water->WorldCell == FIntPoint(1, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProjectionDirtyChunksAreSparseAndConsumableTest,
	"MatterFlux.Performance.LiquidProjectionDirtyChunksAreSparseAndConsumable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxProjectionDirtyChunksAreSparseAndConsumableTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(0, 0);
	Settings.MaxSurfaceCellExclusive = FIntPoint(16, 8);
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Dirty projection world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 41, Error)))
	{
		AddError(Error);
		return false;
	}

	TestTrue(TEXT("Boundary liquid seed is accepted"), World.SeedSurface({
		{ FIntPoint(7, 3), TEXT("water"), 0, 255 }
	}));
	TArray<FIntPoint> DirtyChunks;
	World.ConsumeProjectionDirtyChunks(DirtyChunks);
	TestTrue(TEXT("The owning render chunk becomes dirty"),
		DirtyChunks.Contains(FIntPoint(0, 0)));
	TestTrue(TEXT("The adjacent halo chunk becomes dirty at a changed boundary"),
		DirtyChunks.Contains(FIntPoint(1, 0)));

	World.ConsumeProjectionDirtyChunks(DirtyChunks);
	TestTrue(TEXT("Consuming projection dirtiness is destructive"),
		DirtyChunks.IsEmpty());
	TestTrue(TEXT("A local canonical mutation is accepted"),
		World.SetCell(FIntPoint(2, 2), TEXT("water")));
	World.ConsumeProjectionDirtyChunks(DirtyChunks);
	TestTrue(TEXT("A local mutation dirties its local render chunk"),
		DirtyChunks.Contains(FIntPoint(0, 0)));
	TestFalse(TEXT("A local mutation does not rebuild a distant render chunk"),
		DirtyChunks.Contains(FIntPoint(1, 0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPartialBodyLiquidDisplacementTest,
	"MatterFlux.Material.PartialBodyDisplacementIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPartialBodyLiquidDisplacementTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-2, -2);
	Settings.MaxSurfaceCellExclusive = FIntPoint(3, 3);
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Partial-displacement world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 20260822, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<MatterFlux::Material::FSeedCell> Seeds;
	for (int32 Y = -2; Y <= 2; ++Y)
	{
		for (int32 X = -2; X <= 2; ++X)
		{
			const bool bSource = X == 0 && Y == 0;
			const bool bNeighbor = X == 1 && Y == 0;
			Seeds.Add({
				FIntPoint(X, Y),
				bSource || bNeighbor ? FName(TEXT("water")) : NAME_None,
				0,
				bSource
					? static_cast<uint8>(200)
					: (bNeighbor ? static_cast<uint8>(100) : static_cast<uint8>(0)) });
		}
	}
	TestTrue(TEXT("Partial liquid surface is seeded"), World.SeedSurface(Seeds));
	const MatterFlux::Material::FLiquidDisplacementConstraint Constraint = {
		FIntPoint::ZeroValue,
		190
	};
	TestEqual(TEXT("Only the requested excess amount is displaced"),
		World.DisplaceLiquids(MakeArrayView(&Constraint, 1)), 1);
	MatterFlux::Material::FCellSnapshot SourceAfter;
	MatterFlux::Material::FCellSnapshot NeighborAfter;
	TestTrue(TEXT("Partially occupied source column remains liquid"),
		World.TryGetCellSnapshot(FIntPoint::ZeroValue, SourceAfter));
	TestTrue(TEXT("Existing same-liquid neighbor receives displaced volume first"),
		World.TryGetCellSnapshot(FIntPoint(1, 0), NeighborAfter));
	TestEqual(TEXT("Source retains the constrained amount"),
		SourceAfter.Amount, static_cast<uint8>(190));
	TestEqual(TEXT("Neighbor surface rises by the displaced amount"),
		NeighborAfter.Amount, static_cast<uint8>(110));
	TestEqual(TEXT("Partial displacement conserves total liquid amount"),
		World.SumMaterialAmount(TEXT("water")), static_cast<int64>(300));
	TestEqual(TEXT("Submitting the same body constraint is idempotent"),
		World.DisplaceLiquids(MakeArrayView(&Constraint, 1)), 0);
	MatterFlux::Material::FCellSnapshot SourceAfterRepeat;
	TestTrue(TEXT("Repeated constraint keeps source column"),
		World.TryGetCellSnapshot(FIntPoint::ZeroValue, SourceAfterRepeat));
	TestEqual(TEXT("Repeated constraint does not drain more liquid"),
		SourceAfterRepeat.Amount, static_cast<uint8>(190));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidDisplacementPressureDistributionTest,
	"MatterFlux.Material.LiquidDisplacementDistributesPressureWithoutPumpingSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidDisplacementPressureDistributionTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-1, -1);
	Settings.MaxSurfaceCellExclusive = FIntPoint(2, 2);
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Pressure-distribution world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 20260823, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<MatterFlux::Material::FSeedCell> Seeds;
	for (int32 Y = -1; Y <= 1; ++Y)
	{
		for (int32 X = -1; X <= 1; ++X)
		{
			const bool bSource = X == 0 && Y == 0;
			const int32 TerrainBand = bSource
				? -100
				: 20 * ((X + 1 + (Y + 1) * 3) % 4);
			Seeds.Add({
				FIntPoint(X, Y),
				TEXT("water"),
				TerrainBand,
				static_cast<uint8>(bSource ? 200 : 100 - TerrainBand) });
		}
	}
	TestTrue(TEXT("Level free surface is seeded over uneven support"),
		World.SeedSurface(Seeds));
	const int64 AmountBefore = World.SumMaterialAmount(TEXT("water"));
	const MatterFlux::Material::FLiquidDisplacementConstraint Constraint = {
		FIntPoint::ZeroValue,
		120
	};
	TestEqual(TEXT("Body displacement moves the requested source volume"),
		World.DisplaceLiquids(MakeArrayView(&Constraint, 1), 1), 1);

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	World.GetAllCells(Cells);
	int32 MaximumSurface = MIN_int32;
	int32 MinimumSurface = MAX_int32;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		if (Cell.MaterialId != TEXT("water")
			|| Cell.WorldCell == FIntPoint::ZeroValue)
		{
			continue;
		}
		const int32 Surface = Cell.SupportHeight + Cell.Amount;
		MaximumSurface = FMath::Max(MaximumSurface, Surface);
		MinimumSurface = FMath::Min(MinimumSurface, Surface);
	}
	TestEqual(TEXT("Pressure redistribution conserves exact liquid amount"),
		World.SumMaterialAmount(TEXT("water")), AmountBefore);
	TestTrue(TEXT("Displaced volume is shared by the surrounding free surface"),
		MaximumSurface <= 111);
	TestTrue(TEXT("Uneven support does not bias the resulting free surface"),
		MaximumSurface - MinimumSurface <= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidDensityStratificationTest,
	"MatterFlux.Material.LiquidsStratifyByDensity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidDensityStratificationTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Density world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 8102, Error)))
	{
		AddError(Error);
		return false;
	}

	// 石壁把测试约束成一格宽的液柱，避免液体从侧面绕开彼此。
	for (int32 Y = -1; Y <= 2; ++Y)
	{
		TestTrue(TEXT("Left wall is authored"),
			World.SetCell(FIntPoint(-1, Y), TEXT("stone")));
		TestTrue(TEXT("Right wall is authored"),
			World.SetCell(FIntPoint(1, Y), TEXT("stone")));
	}
	TestTrue(TEXT("Column floor is authored"),
		World.SetCell(FIntPoint(0, -1), TEXT("stone")));
	TestTrue(TEXT("Water starts below acid"),
		World.SetCell(FIntPoint(0, 0), TEXT("water")));
	TestTrue(TEXT("Denser acid starts above water"),
		World.SetCell(FIntPoint(0, 1), TEXT("acid")));

	const MatterFlux::Material::FStepStats Stats = World.Step();
	TestEqual(TEXT("Denser acid displaces water downward"),
		World.GetMaterialAt(FIntPoint(0, 0)), FName(TEXT("acid")));
	TestEqual(TEXT("Water is displaced upward"),
		World.GetMaterialAt(FIntPoint(0, 1)), FName(TEXT("water")));
	TestEqual(TEXT("Layering conserves acid"),
		World.CountMaterial(TEXT("acid")), 1);
	TestEqual(TEXT("Layering conserves water"),
		World.CountMaterial(TEXT("water")), 1);
	TestEqual(TEXT("Density exchange is one movement"), Stats.MovedCells, 1);
	TestEqual(TEXT("Acid and water do not chemically react"),
		Stats.ReactedPairs, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDenseDropSinksThroughLighterPoolTest,
	"MatterFlux.Material.DenseLiquidDropSinksThroughLighterPool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDenseDropSinksThroughLighterPoolTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 2;
	Settings.MaxActiveChunks = 25;
	Settings.MinWorldHeightCells = 0;
	Settings.MaxWorldHeightCells = 24;
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Dense-drop world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 8401, Error)))
	{
		AddError(Error);
		return false;
	}

	for (int32 X = -10; X <= 10; ++X)
	{
		World.SetCell(FIntPoint(X, 0), TEXT("stone"));
	}
	for (int32 Y = 1; Y < 24; ++Y)
	{
		World.SetCell(FIntPoint(-10, Y), TEXT("stone"));
		World.SetCell(FIntPoint(10, Y), TEXT("stone"));
	}
	for (int32 Y = 1; Y <= 5; ++Y)
	{
		for (int32 X = -9; X <= 9; ++X)
		{
			World.SetCell(FIntPoint(X, Y), TEXT("water"));
		}
	}
	constexpr int32 DropCenterY = 15;
	constexpr int32 DropRadius = 3;
	int32 InitialAcidCells = 0;
	for (int32 LocalY = -DropRadius; LocalY <= DropRadius; ++LocalY)
	{
		for (int32 X = -DropRadius; X <= DropRadius; ++X)
		{
			if (X * X + LocalY * LocalY <= DropRadius * DropRadius)
			{
				World.SetCell(FIntPoint(X, DropCenterY + LocalY), TEXT("acid"));
				++InitialAcidCells;
			}
		}
	}
	TestEqual(TEXT("Authored acid drop has a circular cross-section"),
		InitialAcidCells, 29);

	for (int32 Step = 0; Step < 120; ++Step)
	{
		World.Step();
	}

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	World.GetActiveCells(Cells);
	int32 AcidCount = 0;
	int32 WaterCount = 0;
	int32 AcidHeightSum = 0;
	int32 WaterHeightSum = 0;
	int32 HighestAcid = MIN_int32;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		if (Cell.MaterialId == TEXT("acid"))
		{
			++AcidCount;
			AcidHeightSum += Cell.WorldCell.Y;
			HighestAcid = FMath::Max(HighestAcid, Cell.WorldCell.Y);
		}
		else if (Cell.MaterialId == TEXT("water"))
		{
			++WaterCount;
			WaterHeightSum += Cell.WorldCell.Y;
		}
	}
	TestEqual(TEXT("Dense drop conserves every acid cell"),
		AcidCount, InitialAcidCells);
	TestEqual(TEXT("Pool conserves every water cell"), WaterCount, 95);
	if (AcidCount > 0 && WaterCount > 0)
	{
		const float AverageAcidHeight = static_cast<float>(AcidHeightSum)
			/ AcidCount;
		const float AverageWaterHeight = static_cast<float>(WaterHeightSum)
			/ WaterCount;
		AddInfo(FString::Printf(
			TEXT("Dense drop settled: acid average=%.2f, water average=%.2f, acid top=%d"),
			AverageAcidHeight,
			AverageWaterHeight,
			HighestAcid));
		TestTrue(TEXT("Denser acid settles below the displaced water"),
			AverageAcidHeight + 0.5f < AverageWaterHeight);
		TestTrue(TEXT("Acid drop enters the pool instead of resting on its surface"),
			HighestAcid <= 6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLightDropFloatsOnDenserPoolTest,
	"MatterFlux.Material.LightLiquidDropFloatsOnDenserPool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLightDropFloatsOnDenserPoolTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 2;
	Settings.MaxActiveChunks = 25;
	Settings.MinWorldHeightCells = 0;
	Settings.MaxWorldHeightCells = 24;
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Light-drop world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 8402, Error)))
	{
		AddError(Error);
		return false;
	}

	for (int32 X = -10; X <= 10; ++X)
	{
		World.SetCell(FIntPoint(X, 0), TEXT("stone"));
	}
	for (int32 Y = 1; Y < 24; ++Y)
	{
		World.SetCell(FIntPoint(-10, Y), TEXT("stone"));
		World.SetCell(FIntPoint(10, Y), TEXT("stone"));
	}
	for (int32 Y = 1; Y <= 5; ++Y)
	{
		for (int32 X = -9; X <= 9; ++X)
		{
			World.SetCell(FIntPoint(X, Y), TEXT("acid"));
		}
	}
	constexpr int32 DropCenterY = 15;
	constexpr int32 DropRadius = 3;
	int32 InitialWaterCells = 0;
	for (int32 LocalY = -DropRadius; LocalY <= DropRadius; ++LocalY)
	{
		for (int32 X = -DropRadius; X <= DropRadius; ++X)
		{
			if (X * X + LocalY * LocalY <= DropRadius * DropRadius)
			{
				World.SetCell(FIntPoint(X, DropCenterY + LocalY), TEXT("water"));
				++InitialWaterCells;
			}
		}
	}
	TestEqual(TEXT("Authored water drop has a circular cross-section"),
		InitialWaterCells, 29);

	for (int32 Step = 0; Step < 120; ++Step)
	{
		World.Step();
	}

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	World.GetActiveCells(Cells);
	int32 AcidCount = 0;
	int32 WaterCount = 0;
	int32 AcidHeightSum = 0;
	int32 WaterHeightSum = 0;
	int32 LowestWater = MAX_int32;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		if (Cell.MaterialId == TEXT("acid"))
		{
			++AcidCount;
			AcidHeightSum += Cell.WorldCell.Y;
		}
		else if (Cell.MaterialId == TEXT("water"))
		{
			++WaterCount;
			WaterHeightSum += Cell.WorldCell.Y;
			LowestWater = FMath::Min(LowestWater, Cell.WorldCell.Y);
		}
	}
	TestEqual(TEXT("Dense pool conserves every acid cell"), AcidCount, 95);
	TestEqual(TEXT("Light drop conserves every water cell"),
		WaterCount, InitialWaterCells);
	if (AcidCount > 0 && WaterCount > 0)
	{
		const float AverageAcidHeight = static_cast<float>(AcidHeightSum)
			/ AcidCount;
		const float AverageWaterHeight = static_cast<float>(WaterHeightSum)
			/ WaterCount;
		AddInfo(FString::Printf(
			TEXT("Light drop settled: water average=%.2f, acid average=%.2f, water bottom=%d"),
			AverageWaterHeight,
			AverageAcidHeight,
			LowestWater));
		TestTrue(TEXT("Lighter water settles above the acid pool"),
			AverageWaterHeight > AverageAcidHeight + 0.5f);
		TestTrue(TEXT("Water drop does not tunnel through denser acid"),
			LowestWater >= 6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSurfaceLiquidDensityStratificationTest,
	"MatterFlux.Material.SurfaceLiquidsDisplaceByDensityAndTerrainHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxSurfaceLiquidDensityStratificationTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-1, -1);
	Settings.MaxSurfaceCellExclusive = FIntPoint(3, 2);
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Surface density world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 8103, Error)))
	{
		AddError(Error);
		return false;
	}

	for (int32 Y = -1; Y <= 1; ++Y)
	{
		for (int32 X = -1; X <= 2; ++X)
		{
			World.SetSupportHeight(FIntPoint(X, Y), 40);
		}
	}
	World.SetSupportHeight(FIntPoint(0, 0), 20);
	World.SetSupportHeight(FIntPoint(1, 0), 0);
	World.SetCell(FIntPoint(0, 0), TEXT("acid"));
	World.SetCell(FIntPoint(1, 0), TEXT("water"));

	const MatterFlux::Material::FStepStats Stats = World.Step();
	TestEqual(TEXT("Denser acid takes the lower terrain column"),
		World.GetMaterialAt(FIntPoint(1, 0)), FName(TEXT("acid")));
	TestEqual(TEXT("Lighter water is displaced to the higher column"),
		World.GetMaterialAt(FIntPoint(0, 0)), FName(TEXT("water")));
	TestEqual(TEXT("Surface exchange conserves acid"),
		World.CountMaterial(TEXT("acid")), 1);
	TestEqual(TEXT("Surface exchange conserves water"),
		World.CountMaterial(TEXT("water")), 1);
	TestEqual(TEXT("Surface density exchange is one movement"),
		Stats.MovedCells, 1);
	TestEqual(TEXT("Surface acid and water do not chemically react"),
		Stats.ReactedPairs, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSurfaceLiquidSpreadsByAmountTest,
	"MatterFlux.Material.SurfaceLiquidSpreadsByAmountWithoutWholeCellRandomWalk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxSurfaceLiquidSpreadsByAmountTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-2, -2);
	Settings.MaxSurfaceCellExclusive = FIntPoint(3, 3);

	FMatterFluxContentRegistry Registry = MakeLiquidRegistry();
	// 高扩散率不应让整个液体格随机跳走；应只转移一部分守恒液量。
	Registry.Materials.FindChecked(TEXT("acid")).Dispersion = 255;
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Level-surface material world initializes"),
		World.Initialize(Settings, Registry, 8301, Error)))
	{
		AddError(Error);
		return false;
	}

	for (int32 Y = -2; Y <= 2; ++Y)
	{
		for (int32 X = -2; X <= 2; ++X)
		{
			World.SetSupportHeight(FIntPoint(X, Y), 20);
		}
	}
	TestTrue(TEXT("Acid sample is placed on level ground"),
		World.SetCell(FIntPoint::ZeroValue, TEXT("acid")));

	const MatterFlux::Material::FStepStats Stats = World.Step();
	MatterFlux::Material::FCellSnapshot SourceSnapshot;
	TestTrue(TEXT("Source keeps part of its liquid"),
		World.TryGetCellSnapshot(FIntPoint::ZeroValue, SourceSnapshot));
	TestTrue(TEXT("Source amount is split instead of moved wholesale"),
		SourceSnapshot.Amount > 0 && SourceSnapshot.Amount < 255);
	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	World.GetActiveCells(Cells);
	int32 TotalAmount = 0;
	int32 AcidCellCount = 0;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		if (Cell.MaterialId == TEXT("acid"))
		{
			TotalAmount += Cell.Amount;
			++AcidCellCount;
		}
	}
	TestTrue(TEXT("Partial flow occupies another cell"), AcidCellCount >= 2);
	TestEqual(TEXT("Partial flow conserves exact liquid amount"),
		TotalAmount, 255);
	TestEqual(TEXT("One source performs one bounded transfer"),
		Stats.MovedCells, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSurfaceLiquidRefillsAcrossUnevenSupportTest,
	"MatterFlux.Material.SurfaceLiquidRefillsDisplacementAcrossUnevenSupport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxSurfaceLiquidRefillsAcrossUnevenSupportTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-1, -1);
	Settings.MaxSurfaceCellExclusive = FIntPoint(2, 2);
	Settings.LiquidFullColumnHeight = 128;
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Uneven-support refill world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 8303, Error)))
	{
		AddError(Error);
		return false;
	}

	for (int32 Y = -1; Y <= 1; ++Y)
	{
		for (int32 X = -1; X <= 1; ++X)
		{
			World.SetSupportHeight(FIntPoint(X, Y), 200);
		}
	}
	World.SetSupportHeight(FIntPoint(0, 0), 0);
	World.SetSupportHeight(FIntPoint(1, 0), 8);
	TArray<MatterFlux::Material::FSeedCell> Seeds;
	for (int32 Y = -1; Y <= 1; ++Y)
	{
		for (int32 X = -1; X <= 1; ++X)
		{
			const bool bDisplacedCell = X == 1 && Y == 0;
			Seeds.Add({
				FIntPoint(X, Y),
				TEXT("water"),
				bDisplacedCell ? 8 : 0,
				static_cast<uint8>(120) });
		}
	}
	TestTrue(TEXT("Deep partial source column is seeded"),
		World.SeedSurface(Seeds));
	// Advance once so the subsequent empty timestamp unambiguously comes from
	// transient body displacement rather than authored shoreline state.
	World.Step();
	const int64 AmountBefore = World.SumMaterialAmount(TEXT("water"));
	const MatterFlux::Material::FLiquidDisplacementConstraint Constraint = {
		FIntPoint(1, 0), 0
	};
	TestEqual(TEXT("Body creates one recent empty column"),
		World.DisplaceLiquids(MakeArrayView(&Constraint, 1), 1), 1);

	const MatterFlux::Material::FStepStats Stats = World.Step();
	MatterFlux::Material::FCellSnapshot Source;
	MatterFlux::Material::FCellSnapshot Refilled;
	TestTrue(TEXT("Source retains liquid after bounded hydraulic transfer"),
		World.TryGetCellSnapshot(FIntPoint(0, 0), Source));
	TestTrue(TEXT("Empty displaced column refills despite different terrain support"),
		World.TryGetCellSnapshot(FIntPoint(1, 0), Refilled));
	TestTrue(TEXT("Refill transfers a bounded partial volume into the hole"),
		Refilled.Amount > 0 && Refilled.Amount < 255);
	TestEqual(TEXT("Uneven-support refill conserves exact liquid amount"),
		World.SumMaterialAmount(TEXT("water")), AmountBefore);
	TestTrue(TEXT("At least one surrounding liquid column participates"),
		Stats.MovedCells > 0);
	uint8 MaximumRefilledAmount = Refilled.Amount;
	for (int32 StepIndex = 0; StepIndex < 32; ++StepIndex)
	{
		World.Step();
		MatterFlux::Material::FCellSnapshot Current;
		if (World.TryGetCellSnapshot(FIntPoint(1, 0), Current))
		{
			MaximumRefilledAmount = FMath::Max(
				MaximumRefilledAmount, Current.Amount);
		}
	}
	MatterFlux::Material::FCellSnapshot Restored;
	TestTrue(TEXT("Displaced column remains material after restitution"),
		World.TryGetCellSnapshot(FIntPoint(1, 0), Restored));
	TestEqual(TEXT("Body vacancy restores its pre-displacement amount"),
		Restored.Amount, static_cast<uint8>(120));
	TestEqual(TEXT("Restitution never pumps above the reference amount"),
		MaximumRefilledAmount, static_cast<uint8>(120));
	TestEqual(TEXT("Long restitution conserves exact liquid amount"),
		World.SumMaterialAmount(TEXT("water")), AmountBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxBodyWakeRefillConsumesOnlyDisplacedSurplusTest,
	"MatterFlux.Material.BodyWakeRefillConsumesOnlyDisplacedSurplus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxBodyWakeRefillConsumesOnlyDisplacedSurplusTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 32;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-8, -8);
	Settings.MaxSurfaceCellExclusive = FIntPoint(9, 9);
	Settings.LiquidFullColumnHeight = 255;
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Wake-surplus world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 20260824, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<MatterFlux::Material::FSeedCell> Seeds;
	for (int32 Y = -8; Y <= 8; ++Y)
	{
		for (int32 X = -8; X <= 8; ++X)
		{
			Seeds.Add({ FIntPoint(X, Y), TEXT("water"), 0, 120 });
		}
	}
	TestTrue(TEXT("Flat lake is seeded"), World.SeedSurface(Seeds));
	const int64 AmountBefore = World.SumMaterialAmount(TEXT("water"));
	const MatterFlux::Material::FLiquidDisplacementConstraint Constraint = {
		FIntPoint::ZeroValue, 0
	};
	TestEqual(TEXT("Body displaces the central column"),
		World.DisplaceLiquids(MakeArrayView(&Constraint, 1), 2), 1);

	World.Step();
	for (const MatterFlux::Material::FSeedCell& Seed : Seeds)
	{
		if (Seed.WorldCell == FIntPoint::ZeroValue)
		{
			continue;
		}
		MatterFlux::Material::FCellSnapshot Current;
		TestTrue(TEXT("A donor column remains liquid"),
			World.TryGetCellSnapshot(Seed.WorldCell, Current));
		TestTrue(TEXT("Wake refill never borrows a donor's baseline particles"),
			Current.Amount >= Seed.Amount);
	}

	for (int32 StepIndex = 0; StepIndex < 64; ++StepIndex)
	{
		World.Step();
	}
	MatterFlux::Material::FCellSnapshot Restored;
	TestTrue(TEXT("Displaced center becomes liquid again"),
		World.TryGetCellSnapshot(FIntPoint::ZeroValue, Restored));
	TestEqual(TEXT("Displaced center recovers its conserved baseline volume"),
		Restored.Amount, static_cast<uint8>(120));
	TestEqual(TEXT("Wake restitution conserves the exact lake volume"),
		World.SumMaterialAmount(TEXT("water")), AmountBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxBodyWakeRestitutionWorkScaleTest,
	"MatterFlux.Performance.BodyWakeRestitutionVisitsEachActiveRegionOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxBodyWakeRestitutionWorkScaleTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 64;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-32, -32);
	Settings.MaxSurfaceCellExclusive = FIntPoint(33, 33);
	Settings.LiquidFullColumnHeight = 255;
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Restitution-scale world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 20260825, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<MatterFlux::Material::FSeedCell> Seeds;
	Seeds.Reserve(65 * 65);
	for (int32 Y = -32; Y <= 32; ++Y)
	{
		for (int32 X = -32; X <= 32; ++X)
		{
			Seeds.Add({ FIntPoint(X, Y), TEXT("water"), 0, 120 });
		}
	}
	TestTrue(TEXT("Large flat lake is seeded"), World.SeedSurface(Seeds));
	const int64 AmountBefore = World.SumMaterialAmount(TEXT("water"));

	TArray<MatterFlux::Material::FLiquidDisplacementConstraint> Constraints;
	for (int32 Y = -10; Y <= 10; ++Y)
	{
		for (int32 X = -10; X <= 10; ++X)
		{
			Constraints.Add({ FIntPoint(X, Y), 0 });
		}
	}
	TestEqual(TEXT("Every occupied lake column is displaced"),
		World.DisplaceLiquids(Constraints, 32), Constraints.Num());

	const MatterFlux::Material::FStepStats Stats = World.Step();
	TestTrue(TEXT("Wake restitution performs useful transfers"),
		Stats.MovedCells > 0);
	TestTrue(TEXT("Restitution search is bounded by one active-region traversal"),
		Stats.RestitutionVisitedCells <= Seeds.Num() * 2);
	TestEqual(TEXT("Batched restitution conserves exact lake volume"),
		World.SumMaterialAmount(TEXT("water")), AmountBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSurfaceLiquidRelaxesIntoOrganicPuddleTest,
	"MatterFlux.Material.SurfaceLiquidRelaxesIntoRoundSeededPuddle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxSurfaceLiquidRelaxesIntoOrganicPuddleTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 16;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-12, -12);
	Settings.MaxSurfaceCellExclusive = FIntPoint(13, 13);

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Puddle material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 8302, Error)))
	{
		AddError(Error);
		return false;
	}
	for (int32 Y = -12; Y <= 12; ++Y)
	{
		for (int32 X = -12; X <= 12; ++X)
		{
			World.SetSupportHeight(FIntPoint(X, Y), 20);
		}
	}
	for (int32 Y = -4; Y <= 4; ++Y)
	{
		for (int32 X = -2; X <= 1; ++X)
		{
			World.SetCell(FIntPoint(X, Y), TEXT("acid"));
		}
	}
	constexpr int32 InitialAmount = 4 * 9 * 255;
	for (int32 Step = 0; Step < 96; ++Step)
	{
		World.Step();
	}

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	World.GetActiveCells(Cells);
	TArray<MatterFlux::Material::FCellSnapshot> AcidCells;
	int32 TotalAmount = 0;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		if (Cell.MaterialId == TEXT("acid"))
		{
			AcidCells.Add(Cell);
			TotalAmount += Cell.Amount;
		}
	}
	TestEqual(TEXT("Puddle relaxation conserves exact liquid amount"),
		TotalAmount, InitialAmount);
	if (!TestTrue(TEXT("Relaxed puddle remains visible"),
		!AcidCells.IsEmpty()))
	{
		return false;
	}

	FIntPoint Minimum(MAX_int32, MAX_int32);
	FIntPoint Maximum(MIN_int32, MIN_int32);
	TMap<int32, int32> RowWidths;
	for (const MatterFlux::Material::FCellSnapshot& Cell : AcidCells)
	{
		Minimum.X = FMath::Min(Minimum.X, Cell.WorldCell.X);
		Minimum.Y = FMath::Min(Minimum.Y, Cell.WorldCell.Y);
		Maximum.X = FMath::Max(Maximum.X, Cell.WorldCell.X);
		Maximum.Y = FMath::Max(Maximum.Y, Cell.WorldCell.Y);
		++RowWidths.FindOrAdd(Cell.WorldCell.Y);
	}
	const int32 Width = Maximum.X - Minimum.X + 1;
	const int32 Height = Maximum.Y - Minimum.Y + 1;
	const float Aspect = static_cast<float>(FMath::Max(Width, Height))
		/ static_cast<float>(FMath::Max(FMath::Min(Width, Height), 1));
	AddInfo(FString::Printf(
		TEXT("Relaxed puddle bounds: %dx%d, occupied=%d, aspect=%.3f, fill=%.3f"),
		Width,
		Height,
		AcidCells.Num(),
		Aspect,
		static_cast<float>(AcidCells.Num())
			/ static_cast<float>(Width * Height)));
	TestTrue(TEXT("Relaxed puddle is approximately round"), Aspect <= 1.20f);
	const float BoundsFillRatio = static_cast<float>(AcidCells.Num())
		/ static_cast<float>(Width * Height);
	TestTrue(TEXT("Rounded puddle tapers its corners instead of filling a box"),
		BoundsFillRatio >= 0.62f && BoundsFillRatio <= 0.82f);
	TSet<int32> DistinctRowWidths;
	for (const TPair<int32, int32>& Row : RowWidths)
	{
		DistinctRowWidths.Add(Row.Value);
	}
	TestTrue(TEXT("Seeded surface tension leaves an irregular rounded edge"),
		DistinctRowWidths.Num() >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxAcidCorrosionTest,
	"MatterFlux.Material.AcidCorrodesSolidsIntoGasWithoutBeingConsumed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxAcidCorrosionTest::RunTest(const FString& Parameters)
{
	const auto RunCorrosion = [this](const FName SolidMaterial, const int32 Seed)
	{
		MatterFlux::Material::FWorldSettings Settings;
		Settings.ChunkSize = 8;
		Settings.ActiveChunkRadius = 1;
		Settings.MaxActiveChunks = 9;
		MatterFlux::Material::FChunkedMaterialWorld World;
		FString Error;
		if (!TestTrue(TEXT("Corrosion world initializes"),
			World.Initialize(Settings, MakeLiquidRegistry(), Seed, Error)))
		{
			AddError(Error);
			return false;
		}
		World.SetCell(FIntPoint(0, 0), TEXT("acid"));
		World.SetCell(FIntPoint(1, 0), SolidMaterial);
		const MatterFlux::Material::FStepStats Stats = World.Step();
		TestEqual(TEXT("Acid remains after corrosion"),
			World.GetMaterialAt(FIntPoint(0, 0)), FName(TEXT("acid")));
		TestEqual(TEXT("Corroded material becomes acid gas"),
			World.GetMaterialAt(FIntPoint(1, 0)), FName(TEXT("acid_gas")));
		TestEqual(TEXT("Exactly one corrosion pair reacts"),
			Stats.ReactedPairs, 1);
		return true;
	};

	return RunCorrosion(TEXT("wood"), 8201)
		&& RunCorrosion(TEXT("grassland"), 8202);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialRuntimeFocusDebtTest,
	"MatterFlux.Material.Runtime.FocusChangeDefersButPreservesFixedStepDebt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterialRuntimeFocusDebtTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FRuntimeSettings Settings;
	Settings.World.ChunkSize = 8;
	Settings.World.ActiveChunkRadius = 1;
	Settings.World.MaxActiveChunks = 9;
	Settings.StepSeconds = 0.05f;
	Settings.MaxStepsPerAdvance = 4;

	MatterFlux::Material::FSimulationRuntime Runtime;
	FString Error;
	const TArray<FIntPoint> InitialFocuses = { FIntPoint::ZeroValue };
	if (!TestTrue(
		TEXT("Material runtime initializes"),
		Runtime.Initialize(
			Settings,
			MakeLiquidRegistry(),
			20260808,
			InitialFocuses,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("Water can be placed in the next focus chunk"),
		Runtime.SetCell(FIntPoint(8, 1), TEXT("water")));

	const TArray<FIntPoint> NextFocuses = { FIntPoint(8, 0) };
	const MatterFlux::Material::FRuntimeAdvanceResult FocusFrame =
		Runtime.AdvanceAuthority(0.05f, NextFocuses);
	TestTrue(TEXT("Focus change is observable"), FocusFrame.bFocusChanged);
	TestEqual(TEXT("Focus frame performs no simulation steps"), FocusFrame.Steps, 0);
	TestEqual(
		TEXT("Focus frame preserves the unmoved cell"),
		Runtime.GetMaterialAt(FIntPoint(8, 1)),
		FName(TEXT("water")));

	const MatterFlux::Material::FRuntimeAdvanceResult DebtFrame =
		Runtime.AdvanceAuthority(0.0f, NextFocuses);
	TestFalse(TEXT("Stable focus is not reported as changed"), DebtFrame.bFocusChanged);
	TestEqual(TEXT("Deferred debt advances on the next frame"), DebtFrame.Steps, 1);
	TestTrue(TEXT("The deferred step changes simulation state"), DebtFrame.bStateChanged);
	TestEqual(TEXT("Logical step advances exactly once"), Runtime.GetLogicalStep(), 1);
	TestEqual(
		TEXT("Water moves after deferred debt is consumed"),
		Runtime.GetMaterialAt(FIntPoint(8, 0)),
		FName(TEXT("water")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialRuntimeReplicationTest,
	"MatterFlux.Material.Runtime.ReplicatedStateAppliesOnceAndRejectsCorruption",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterialRuntimeReplicationTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FRuntimeSettings Settings;
	Settings.World.ChunkSize = 8;
	Settings.World.ActiveChunkRadius = 0;
	Settings.World.MaxActiveChunks = 1;
	const TArray<FIntPoint> Focuses = { FIntPoint::ZeroValue };
	const FMatterFluxContentRegistry Registry = MakeLiquidRegistry();
	MatterFlux::Material::FSimulationRuntime Server;
	MatterFlux::Material::FSimulationRuntime Client;
	FString Error;
	if (!TestTrue(TEXT("Server runtime initializes"),
		Server.Initialize(Settings, Registry, 4141, Focuses, Error))
		|| !TestTrue(TEXT("Client runtime initializes"),
			Client.Initialize(Settings, Registry, 4141, Focuses, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Server water is seeded"),
		Server.SetCell(FIntPoint(0, 1), TEXT("water")));
	Server.AdvanceAuthority(Settings.StepSeconds, Focuses);

	FMatterFluxReplicatedMaterialState Snapshot;
	TestTrue(TEXT("Server builds an atomic replicated snapshot"),
		Server.BuildReplicatedState(4141, INDEX_NONE, Snapshot, Error));
	TestTrue(TEXT("Snapshot carries compressed payload"), Snapshot.HasPayload());
	TestEqual(TEXT("First runtime revision is zero"), Snapshot.Revision, 0);
	TestEqual(
		TEXT("Client applies a new valid revision"),
		Client.ApplyReplicatedState(4141, Snapshot, Error),
		MatterFlux::Material::EReplicatedStateApplyResult::Applied);
	TestEqual(TEXT("Client imports the logical step"),
		Client.GetLogicalStep(), Server.GetLogicalStep());
	TestEqual(TEXT("Client imports active material"),
		Client.GetMaterialAt(FIntPoint(0, 0)), FName(TEXT("water")));
	TestEqual(
		TEXT("Applying the same revision is idempotent"),
		Client.ApplyReplicatedState(4141, Snapshot, Error),
		MatterFlux::Material::EReplicatedStateApplyResult::NoChange);

	FMatterFluxReplicatedMaterialState Corrupt = Snapshot;
	Corrupt.Revision = 1;
	Corrupt.StateHash ^= 0x5a5a5a5au;
	TestEqual(
		TEXT("Corrupt next revision is rejected"),
		Client.ApplyReplicatedState(4141, Corrupt, Error),
		MatterFlux::Material::EReplicatedStateApplyResult::Rejected);
	TestEqual(TEXT("Rejected revision does not change applied state"),
		Client.GetAppliedStateRevision(), 0);
	TestEqual(
		TEXT("Repeated corrupt revision is ignored after one rejection"),
		Client.ApplyReplicatedState(4141, Corrupt, Error),
		MatterFlux::Material::EReplicatedStateApplyResult::NoChange);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLiquidFallsTest,
	"MatterFlux.Material.LiquidFallsAndConservesMatter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLiquidFallsTest::RunTest(const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 1337, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint::ZeroValue);
	TestTrue(TEXT("Water cell can be placed"),
		World.SetCell(FIntPoint(0, 1), TEXT("water")));
	TestEqual(TEXT("One water cell exists before stepping"),
		World.CountMaterial(TEXT("water")),
		1);

	const MatterFlux::Material::FStepStats Stats = World.Step();
	TestEqual(TEXT("Water moves down one cell"),
		World.GetMaterialAt(FIntPoint(0, 0)),
		FName(TEXT("water")));
	TestEqual(TEXT("The previous cell becomes empty"),
		World.GetMaterialAt(FIntPoint(0, 1)),
		NAME_None);
	TestEqual(TEXT("Liquid movement conserves matter"),
		World.CountMaterial(TEXT("water")),
		1);
	TestEqual(TEXT("Exactly one cell moved"), Stats.MovedCells, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMultipleSimulationFocusesTest,
	"MatterFlux.Material.SeparatedPlayerFocusesRemainActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMultipleSimulationFocusesTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 0;
	Settings.MaxActiveChunks = 2;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Two-focus material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 20260808, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("First player's water cell is seeded"),
		World.SetCell(FIntPoint(0, 1), TEXT("water")));
	TestTrue(TEXT("Second player's water cell is seeded"),
		World.SetCell(FIntPoint(16, 1), TEXT("water")));
	const TArray<FIntPoint> Focuses = {
		FIntPoint(0, 0),
		FIntPoint(16, 0)
	};
	World.SetSimulationFocuses(Focuses);

	TestEqual(TEXT("Both separated focus chunks are resident"),
		World.GetResidentChunkCount(), 2);
	TestEqual(TEXT("Neither focused chunk remains archived"),
		World.GetArchivedChunkCount(), 0);
	const MatterFlux::Material::FStepStats Stats = World.Step();
	TestEqual(TEXT("Both players' active material cells advance"),
		Stats.MovedCells, 2);
	TestEqual(TEXT("First player's water falls"),
		World.GetMaterialAt(FIntPoint(0, 0)), FName(TEXT("water")));
	TestEqual(TEXT("Second player's water falls"),
		World.GetMaterialAt(FIntPoint(16, 0)), FName(TEXT("water")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSimulationFocusBudgetFairnessTest,
	"MatterFlux.Material.MultiFocusBudgetIsFairAndDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxSimulationFocusBudgetFairnessTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Budgeted multi-focus world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 8080, Error)))
	{
		AddError(Error);
		return false;
	}

	// These cells occupy the first deterministic distance-one candidate around
	// each separated focus chunk. A focus-by-focus fill would starve the second.
	TestTrue(TEXT("First focus ring material seeds"),
		World.SetCell(FIntPoint(-8, -7), TEXT("water")));
	TestTrue(TEXT("Second focus ring material seeds"),
		World.SetCell(FIntPoint(72, -7), TEXT("water")));
	const TArray<FIntPoint> ReverseOrderedFocuses = {
		FIntPoint(80, 0),
		FIntPoint(0, 0)
	};
	World.SetSimulationFocuses(ReverseOrderedFocuses);
	const MatterFlux::Material::FStepStats Stats = World.Step();
	TestEqual(TEXT("Round-robin budget advances both focus rings"),
		Stats.MovedCells, 2);
	TestEqual(TEXT("First ring advances"),
		World.GetMaterialAt(FIntPoint(-8, -8)), FName(TEXT("water")));
	TestEqual(TEXT("Second ring advances"),
		World.GetMaterialAt(FIntPoint(72, -8)), FName(TEXT("water")));
	TestTrue(TEXT("Resident chunks never exceed the configured budget"),
		World.GetResidentChunkCount() <= Settings.MaxActiveChunks);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMultiFocusActiveStateRoundTripTest,
	"MatterFlux.Material.MultiFocusActiveStateRoundTripsWithoutFocusHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMultiFocusActiveStateRoundTripTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 0;
	Settings.MaxActiveChunks = 2;
	MatterFlux::Material::FChunkedMaterialWorld ServerWorld;
	MatterFlux::Material::FChunkedMaterialWorld LateClientWorld;
	FString Error;
	if (!TestTrue(TEXT("Server multi-focus world initializes"),
		ServerWorld.Initialize(
			Settings, MakeLiquidRegistry(), 6161, Error))
		|| !TestTrue(TEXT("Late client world initializes"),
			LateClientWorld.Initialize(
				Settings, MakeLiquidRegistry(), 6161, Error)))
	{
		AddError(Error);
		return false;
	}

	const TArray<FIntPoint> ReverseOrderedFocuses = {
		FIntPoint(16, 0),
		FIntPoint(0, 0)
	};
	ServerWorld.SetSimulationFocuses(ReverseOrderedFocuses);
	TestTrue(TEXT("First focus state seeds"),
		ServerWorld.SetCell(FIntPoint(0, 0), TEXT("stone")));
	TestTrue(TEXT("Second focus state seeds"),
		ServerWorld.SetCell(FIntPoint(16, 0), TEXT("water")));

	TArray<uint8> ActiveState;
	if (!TestTrue(TEXT("Server exports every active focus"),
		ServerWorld.ExportActiveState(27, ActiveState, Error)))
	{
		AddError(Error);
		return false;
	}
	MatterFlux::Material::FChunkedMaterialWorld CanonicalOrderWorld;
	if (!TestTrue(TEXT("Comparison world initializes"),
		CanonicalOrderWorld.Initialize(
			Settings, MakeLiquidRegistry(), 6161, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<FIntPoint> CanonicalOrderedFocuses = {
		FIntPoint(0, 0),
		FIntPoint(16, 0)
	};
	CanonicalOrderWorld.SetSimulationFocuses(CanonicalOrderedFocuses);
	CanonicalOrderWorld.SetCell(FIntPoint(0, 0), TEXT("stone"));
	CanonicalOrderWorld.SetCell(FIntPoint(16, 0), TEXT("water"));
	TArray<uint8> CanonicalOrderState;
	TestTrue(TEXT("Comparison world exports"),
		CanonicalOrderWorld.ExportActiveState(
			27, CanonicalOrderState, Error));
	TestTrue(TEXT("Input focus order does not change snapshot bytes"),
		ActiveState == CanonicalOrderState);

	// Deliberately give the receiving world unrelated focus history. A complete
	// snapshot must reconstruct all active windows without relying on it.
	LateClientWorld.SetSimulationFocus(FIntPoint(-80, 0));
	int32 ImportedStep = INDEX_NONE;
	FIntPoint ImportedPrimaryFocus = FIntPoint::ZeroValue;
	if (!TestTrue(TEXT("Late client imports the multi-focus snapshot"),
		LateClientWorld.ImportActiveState(
			ActiveState,
			ImportedStep,
			ImportedPrimaryFocus,
			Error)))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("Logical step round trips"), ImportedStep, 27);
	TestEqual(TEXT("Primary focus is canonical"),
		ImportedPrimaryFocus, FIntPoint(0, 0));
	TestEqual(TEXT("Both focused chunks become resident"),
		LateClientWorld.GetResidentChunkCount(), 2);
	TestEqual(TEXT("First focused chunk state round trips"),
		LateClientWorld.GetMaterialAt(FIntPoint(0, 0)),
		FName(TEXT("stone")));
	TestEqual(TEXT("Second focused chunk state round trips"),
		LateClientWorld.GetMaterialAt(FIntPoint(16, 0)),
		FName(TEXT("water")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxChemicalReactionTest,
	"MatterFlux.Material.ConfiguredReactionTransformsAdjacentMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxChemicalReactionTest::RunTest(const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 42, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint::ZeroValue);
	World.SetCell(FIntPoint(0, 0), TEXT("water"));
	World.SetCell(FIntPoint(1, 0), TEXT("lava"));
	const MatterFlux::Material::FStepStats Stats = World.Step();

	TestEqual(TEXT("Water side becomes configured steam"),
		World.GetMaterialAt(FIntPoint(0, 0)),
		FName(TEXT("steam")));
	TestEqual(TEXT("Lava side becomes configured stone"),
		World.GetMaterialAt(FIntPoint(1, 0)),
		FName(TEXT("stone")));
	TestEqual(TEXT("One adjacent pair reacts"), Stats.ReactedPairs, 1);
	TestEqual(TEXT("Reactants are consumed"), World.CountMaterial(TEXT("water")), 0);
	TestEqual(TEXT("Reactants are consumed"), World.CountMaterial(TEXT("lava")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxExtremeDensityQuantizationTest,
	"MatterFlux.Material.ExtremeFiniteDensityClampsBeforeQuantization",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxExtremeDensityQuantizationTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxContentRegistry Registry = MakeLiquidRegistry();
	Registry.Materials.FindChecked(TEXT("water")).Density =
		std::numeric_limits<float>::max();

	MatterFlux::Material::FWorldSettings Settings;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	TestTrue(
		FString::Printf(
			TEXT("Extreme finite density initializes safely: %s"),
			*Error),
		World.Initialize(Settings, Registry, 51, Error));
	TestTrue(
		TEXT("Clamped material remains usable"),
		World.SetCell(FIntPoint::ZeroValue, TEXT("water")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxDuplicateReactionPairTest,
	"MatterFlux.Material.DuplicateUnorderedReactionPairIsRejected",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxDuplicateReactionPairTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxContentRegistry Registry = MakeLiquidRegistry();
	FMatterFluxReactionDefinition Duplicate;
	Duplicate.Id = TEXT("lava_water_second_rule");
	Duplicate.InputA = TEXT("lava");
	Duplicate.InputB = TEXT("water");
	Duplicate.OutputA = TEXT("stone");
	Duplicate.OutputB = TEXT("steam");
	Duplicate.ChancePermille = 500;
	Registry.Reactions.Add(Duplicate.Id, Duplicate);

	MatterFlux::Material::FWorldSettings Settings;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	TestFalse(
		TEXT("Ambiguous reaction pair cannot initialize"),
		World.Initialize(Settings, Registry, 52, Error));
	TestTrue(
		TEXT("Reaction-pair error is actionable"),
		Error.Contains(TEXT("unordered input pair")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGasCeilingTest,
	"MatterFlux.Material.GasRisesAndCullsAtWorldCeiling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxGasCeilingTest::RunTest(const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.MinWorldHeightCells = -8;
	Settings.MaxWorldHeightCells = 3;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 99, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint::ZeroValue);
	World.SetCell(FIntPoint(0, 1), TEXT("steam"));
	const MatterFlux::Material::FStepStats RisingStats = World.Step();
	TestEqual(TEXT("Gas rises one cell"),
		World.GetMaterialAt(FIntPoint(0, 2)),
		FName(TEXT("steam")));
	TestEqual(TEXT("Rising gas is conserved"),
		World.CountMaterial(TEXT("steam")),
		1);
	TestEqual(TEXT("One gas cell moved"), RisingStats.MovedCells, 1);

	const MatterFlux::Material::FStepStats CullingStats = World.Step();
	TestEqual(TEXT("Gas above the configured ceiling is removed"),
		World.CountMaterial(TEXT("steam")),
		0);
	TestEqual(TEXT("Ceiling removal is observable"), CullingStats.CulledCells, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPowderStacksTest,
	"MatterFlux.Material.PowderFallsDiagonallyAndStacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPowderStacksTest::RunTest(const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 7331, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint::ZeroValue);
	World.SetCell(FIntPoint(0, 0), TEXT("stone"));
	World.SetCell(FIntPoint(0, 1), TEXT("sand"));
	const MatterFlux::Material::FStepStats Stats = World.Step();

	TestEqual(TEXT("Powder leaves its unsupported origin"),
		World.GetMaterialAt(FIntPoint(0, 1)),
		NAME_None);
	TestTrue(TEXT("Powder settles on one of the two diagonal cells"),
		World.GetMaterialAt(FIntPoint(-1, 0)) == TEXT("sand")
		|| World.GetMaterialAt(FIntPoint(1, 0)) == TEXT("sand"));
	TestEqual(TEXT("Powder stacking conserves matter"),
		World.CountMaterial(TEXT("sand")),
		1);
	TestEqual(TEXT("One powder cell moved"), Stats.MovedCells, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxActiveChunkBoundaryTest,
	"MatterFlux.Material.ActiveChunksExchangeCellsAcrossBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxActiveChunkBoundaryTest::RunTest(const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 17, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint(7, 1));
	World.SetCell(FIntPoint(7, 1), TEXT("water"));
	World.SetCell(FIntPoint(7, 0), TEXT("stone"));
	World.SetCell(FIntPoint(6, 0), TEXT("stone"));
	World.Step();

	TestEqual(TEXT("Liquid crosses into the adjacent active chunk"),
		World.GetMaterialAt(FIntPoint(8, 0)),
		FName(TEXT("water")));
	TestEqual(TEXT("Cross-chunk movement conserves liquid"),
		World.CountMaterial(TEXT("water")),
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxInactiveChunkBoundaryTest,
	"MatterFlux.Material.InactiveChunkBoundaryFreezesWithoutLosingCells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxInactiveChunkBoundaryTest::RunTest(const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 0;
	Settings.MaxActiveChunks = 1;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 23, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint(7, 1));
	World.SetCell(FIntPoint(7, 1), TEXT("water"));
	World.SetCell(FIntPoint(7, 0), TEXT("stone"));
	World.SetCell(FIntPoint(6, 0), TEXT("stone"));
	World.SetCell(FIntPoint(6, 1), TEXT("stone"));
	World.Step();

	TestEqual(TEXT("Liquid remains at a boundary whose neighbor is unloaded"),
		World.GetMaterialAt(FIntPoint(7, 1)),
		FName(TEXT("water")));
	TestEqual(TEXT("Inactive boundary does not delete liquid"),
		World.CountMaterial(TEXT("water")),
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxActivatedChunkBoundaryRetryTest,
	"MatterFlux.Material.ActivatedChunkBoundaryRetriesRetainedCells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxActivatedChunkBoundaryRetryTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 29, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint::ZeroValue);
	World.SetCell(FIntPoint(15, 1), TEXT("water"));
	World.SetCell(FIntPoint(15, 0), TEXT("stone"));
	World.SetCell(FIntPoint(14, 0), TEXT("stone"));
	World.SetCell(FIntPoint(14, 1), TEXT("stone"));
	World.Step();
	TestEqual(
		TEXT("Liquid waits while the destination chunk is inactive"),
		World.GetMaterialAt(FIntPoint(15, 1)),
		FName(TEXT("water")));

	World.SetSimulationFocus(FIntPoint(8, 0));
	World.Step();
	TestEqual(
		TEXT("Retained seam cell retries when its neighbor activates"),
		World.GetMaterialAt(FIntPoint(16, 0)),
		FName(TEXT("water")));
	TestEqual(
		TEXT("Activation retry conserves liquid"),
		World.CountMaterial(TEXT("water")),
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxChunkArchiveTest,
	"MatterFlux.Material.FocusArchivesAndRestoresDormantChunks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxChunkArchiveTest::RunTest(const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 0;
	Settings.MaxActiveChunks = 1;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 31, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint::ZeroValue);
	World.SetCell(FIntPoint(1, 1), TEXT("stone"));
	TestEqual(TEXT("Authored active chunk is resident"),
		World.GetResidentChunkCount(),
		1);

	World.SetSimulationFocus(FIntPoint(24, 0));
	TestEqual(TEXT("Dormant chunk leaves the resident simulation set"),
		World.GetResidentChunkCount(),
		0);
	TestEqual(TEXT("Dormant non-empty chunk is archived"),
		World.GetArchivedChunkCount(),
		1);
	TestEqual(TEXT("Archived cells remain queryable"),
		World.GetMaterialAt(FIntPoint(1, 1)),
		FName(TEXT("stone")));

	World.SetSimulationFocus(FIntPoint::ZeroValue);
	TestEqual(TEXT("Returning focus restores the archived chunk"),
		World.GetResidentChunkCount(),
		1);
	TestEqual(TEXT("Restoring consumes its archive"),
		World.GetArchivedChunkCount(),
		0);
	TestEqual(TEXT("Restored cell keeps its material"),
		World.GetMaterialAt(FIntPoint(1, 1)),
		FName(TEXT("stone")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxExtremeNegativeCoordinateTest,
	"MatterFlux.Material.ExtremeNegativeCoordinatesRemainDistinct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxExtremeNegativeCoordinateTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 64;
	Settings.ActiveChunkRadius = 0;
	Settings.MaxActiveChunks = 1;
	Settings.bCullOutsideVerticalBounds = false;

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 37, Error)))
	{
		AddError(Error);
		return false;
	}

	// A signed overflow in floor division used to map these two world
	// coordinates to the same chunk and local cell.
	const FIntPoint ExtremeNegative(MIN_int32, 0);
	const FIntPoint LegitimatePositive(2147483584, 0);
	TestTrue(TEXT("Extreme negative cell can be authored"),
		World.SetCell(ExtremeNegative, TEXT("water")));
	TestTrue(TEXT("Large positive cell can be authored"),
		World.SetCell(LegitimatePositive, TEXT("sand")));

	TestEqual(TEXT("Extreme negative material is not overwritten"),
		World.GetMaterialAt(ExtremeNegative),
		FName(TEXT("water")));
	TestEqual(TEXT("Large positive material remains independently queryable"),
		World.GetMaterialAt(LegitimatePositive),
		FName(TEXT("sand")));
	TestEqual(TEXT("Distinct distant cells occupy distinct archives"),
		World.GetArchivedChunkCount(),
		2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSurfaceLiquidDownhillTest,
	"MatterFlux.Material.SurfaceLiquidFlowsAcrossGroundHeightfield",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxSurfaceLiquidDownhillTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-2, -2);
	Settings.MaxSurfaceCellExclusive = FIntPoint(3, 3);

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Surface material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 71, Error)))
	{
		AddError(Error);
		return false;
	}

	World.SetSimulationFocus(FIntPoint::ZeroValue);
	for (int32 Y = -1; Y <= 1; ++Y)
	{
		for (int32 X = -1; X <= 1; ++X)
		{
			World.SetSupportHeight(FIntPoint(X, Y), 20);
		}
	}
	World.SetSupportHeight(FIntPoint(0, 0), 10);
	World.SetSupportHeight(FIntPoint(1, 0), 0);
	World.SetCell(FIntPoint(0, 0), TEXT("water"));

	const MatterFlux::Material::FStepStats Stats = World.Step();
	TestEqual(TEXT("Water begins flowing to the uniquely lowest ground neighbor"),
		World.GetMaterialAt(FIntPoint(1, 0)),
		FName(TEXT("water")));
	TestEqual(TEXT("The uphill source retains the untransferred particles"),
		World.GetMaterialAt(FIntPoint(0, 0)),
		FName(TEXT("water")));
	TestEqual(TEXT("Surface flow conserves every liquid particle"),
		World.SumMaterialAmount(TEXT("water")),
		static_cast<int64>(255));
	TestEqual(TEXT("Exactly one bounded surface transfer occurs"),
		Stats.MovedCells, 1);

	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	World.GetActiveCells(Cells);
	const MatterFlux::Material::FCellSnapshot* Water =
		Cells.FindByPredicate(
			[](const MatterFlux::Material::FCellSnapshot& Cell)
			{
				return Cell.WorldCell == FIntPoint(1, 0)
					&& Cell.MaterialId == TEXT("water");
			});
	TestTrue(TEXT("Snapshot carries the destination terrain elevation"),
		Water && Water->SupportHeight == 0);
	TestTrue(TEXT("Downhill flow transfers a bounded particle amount"),
		Water && Water->Amount > 0 && Water->Amount < 255);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxBulkSurfaceSeedTest,
	"MatterFlux.Material.BulkSurfaceSeedArchivesEachFarChunkOnce",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxBulkSurfaceSeedTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 0;
	Settings.MaxActiveChunks = 1;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(0, 0);
	Settings.MaxSurfaceCellExclusive = FIntPoint(24, 8);

	MatterFlux::Material::FChunkedMaterialWorld World;
	FString Error;
	if (!TestTrue(TEXT("Surface material world initializes"),
		World.Initialize(Settings, MakeLiquidRegistry(), 31337, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<MatterFlux::Material::FSeedCell> InvalidSeed{
		{FIntPoint(0, 0), TEXT("water"), 10},
		{FIntPoint(0, 0), TEXT("stone"), 20}
	};
	TestFalse(TEXT("Duplicate seed coordinates reject transactionally"),
		World.SeedSurface(InvalidSeed));
	TestEqual(TEXT("Rejected seed leaves the world empty"),
		World.CountMaterial(TEXT("water")),
		0);

	TArray<MatterFlux::Material::FSeedCell> SeedCells;
	for (int32 Y = 0; Y < 8; ++Y)
	{
		for (int32 X = 0; X < 24; ++X)
		{
			SeedCells.Add({
				FIntPoint(X, Y),
				X == 23 ? FName(TEXT("water")) : NAME_None,
				X + Y });
		}
	}
	TestTrue(TEXT("Large surface seed commits in one operation"),
		World.SeedSurface(SeedCells));
	TestEqual(TEXT("Only the focus chunk remains resident"),
		World.GetResidentChunkCount(),
		1);
	TestEqual(TEXT("Two distant chunks are archived"),
		World.GetArchivedChunkCount(),
		2);
	TestEqual(TEXT("Archived material remains queryable"),
		World.GetMaterialAt(FIntPoint(23, 7)),
		FName(TEXT("water")));
	MatterFlux::Material::FCellSnapshot Snapshot;
	TestTrue(TEXT("Archived material exposes a point snapshot"),
		World.TryGetCellSnapshot(FIntPoint(23, 7), Snapshot));
	TestEqual(TEXT("Archived snapshot preserves material"),
		Snapshot.MaterialId, FName(TEXT("water")));
	TestEqual(TEXT("Archived snapshot preserves support height"),
		Snapshot.SupportHeight, 30);
	TestFalse(TEXT("Empty archived cells do not masquerade as liquid"),
		World.TryGetCellSnapshot(FIntPoint(22, 7), Snapshot));

	TArray<MatterFlux::Material::FCellSnapshot> ActiveCells;
	World.GetActiveCells(ActiveCells);
	TestFalse(TEXT("Active enumeration excludes archived material facts"),
		ActiveCells.ContainsByPredicate(
			[](const MatterFlux::Material::FCellSnapshot& Cell)
			{
				return Cell.WorldCell == FIntPoint(23, 7);
			}));

	TArray<MatterFlux::Material::FCellSnapshot> AllCells;
	World.GetAllCells(AllCells);
	const MatterFlux::Material::FCellSnapshot* ArchivedWater =
		AllCells.FindByPredicate(
			[](const MatterFlux::Material::FCellSnapshot& Cell)
			{
				return Cell.WorldCell == FIntPoint(23, 7);
			});
	TestTrue(TEXT("Complete enumeration includes archived material facts"),
		ArchivedWater != nullptr);
	if (ArchivedWater)
	{
		TestEqual(TEXT("Complete enumeration preserves archived material"),
			ArchivedWater->MaterialId, FName(TEXT("water")));
		TestEqual(TEXT("Complete enumeration preserves archived support height"),
			ArchivedWater->SupportHeight, 30);
		TestEqual(TEXT("Complete enumeration preserves archived amount"),
			ArchivedWater->Amount, static_cast<uint8>(255));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMobilityRetryTest,
	"MatterFlux.Material.MobilityDelayRetriesWithoutWakingWholeChunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMobilityRetryTest::RunTest(const FString& Parameters)
{
	for (int32 Seed = 0; Seed < 256; ++Seed)
	{
		MatterFlux::Material::FWorldSettings Settings;
		Settings.ActiveChunkRadius = 1;
		Settings.MaxActiveChunks = 9;

		MatterFlux::Material::FChunkedMaterialWorld World;
		FString Error;
		if (!World.Initialize(
				Settings,
				MakeLiquidRegistry(),
				Seed,
				Error))
		{
			AddError(Error);
			return false;
		}
		World.SetSimulationFocus(FIntPoint::ZeroValue);
		World.SetCell(FIntPoint(0, 1), TEXT("lava"));
		if (World.Step().MovedCells != 0)
		{
			continue;
		}

		bool bEventuallyMoved = false;
		for (int32 Retry = 0; Retry < 64; ++Retry)
		{
			if (World.Step().MovedCells > 0)
			{
				bEventuallyMoved = true;
				break;
			}
		}
		TestTrue(
			TEXT("A mobility miss is retried on a later fixed step"),
			bEventuallyMoved);
		return true;
	}

	AddError(TEXT("Test setup did not find a deterministic first-step mobility miss."));
	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxAuthoritativeActiveStateTest,
	"MatterFlux.Material.AuthoritativeActiveStateRepairsDivergentClient",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxAuthoritativeActiveStateTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::Material::FWorldSettings Settings;
	Settings.ChunkSize = 8;
	Settings.ActiveChunkRadius = 1;
	Settings.MaxActiveChunks = 9;
	Settings.bUseSurfaceTopology = true;
	Settings.MinSurfaceCell = FIntPoint(-16, -16);
	Settings.MaxSurfaceCellExclusive = FIntPoint(32, 16);

	MatterFlux::Material::FChunkedMaterialWorld ServerWorld;
	MatterFlux::Material::FChunkedMaterialWorld LateClientWorld;
	FString Error;
	if (!TestTrue(
			TEXT("Authority material world initializes"),
			ServerWorld.Initialize(
				Settings,
				MakeLiquidRegistry(),
				1776,
				Error))
		|| !TestTrue(
			TEXT("Late client material world initializes"),
			LateClientWorld.Initialize(
				Settings,
				MakeLiquidRegistry(),
				1776,
				Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<MatterFlux::Material::FSeedCell> SeedCells;
	for (int32 Y = -8; Y < 8; ++Y)
	{
		for (int32 X = -8; X < 24; ++X)
		{
			SeedCells.Add({
				FIntPoint(X, Y),
				NAME_None,
				X * 3 + Y });
		}
	}
	TestTrue(
		TEXT("Authority surface topology seeds"),
		ServerWorld.SeedSurface(SeedCells));
	TestTrue(
		TEXT("Late client surface topology seeds"),
		LateClientWorld.SeedSurface(SeedCells));

	const FIntPoint Focus(9, 0);
	ServerWorld.SetSimulationFocus(Focus);
	LateClientWorld.SetSimulationFocus(Focus);
	ServerWorld.SetCell(FIntPoint(8, 0), TEXT("water"));
	ServerWorld.SetCell(FIntPoint(9, 0), TEXT("sand"));
	LateClientWorld.SetCell(FIntPoint(8, 0), TEXT("lava"));
	LateClientWorld.SetCell(FIntPoint(10, 0), TEXT("steam"));
	for (int32 Step = 0; Step < 7; ++Step)
	{
		ServerWorld.Step();
	}

	TArray<uint8> Snapshot;
	TestTrue(
		TEXT("Authority exports a self-contained active-state snapshot"),
		ServerWorld.ExportActiveState(
			73,
			Snapshot,
			Error));
	TestTrue(
		TEXT("Snapshot is non-empty and remains replication-sized"),
		!Snapshot.IsEmpty() && Snapshot.Num() < 1024 * 1024);

	int32 ImportedStep = INDEX_NONE;
	FIntPoint ImportedFocus = FIntPoint(MAX_int32, MAX_int32);
	TestTrue(
		TEXT("Late client imports the current authority snapshot without focus history"),
		LateClientWorld.ImportActiveState(
			Snapshot,
			ImportedStep,
			ImportedFocus,
			Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestEqual(TEXT("Snapshot carries the coherent logical step"),
		ImportedStep,
		73);
	TestEqual(TEXT("Snapshot carries the coherent simulation focus"),
		ImportedFocus,
		Focus);

	TArray<MatterFlux::Material::FCellSnapshot> ServerCells;
	TArray<MatterFlux::Material::FCellSnapshot> ClientCells;
	ServerWorld.GetActiveCells(ServerCells);
	LateClientWorld.GetActiveCells(ClientCells);
	TestEqual(
		TEXT("Late client has the same number of active material cells"),
		ClientCells.Num(),
		ServerCells.Num());
	for (int32 Index = 0;
		Index < FMath::Min(ServerCells.Num(), ClientCells.Num());
		++Index)
	{
		TestEqual(
			TEXT("Snapshot repairs each active cell coordinate"),
			ClientCells[Index].WorldCell,
			ServerCells[Index].WorldCell);
		TestEqual(
			TEXT("Snapshot repairs each active material identity"),
			ClientCells[Index].MaterialId,
			ServerCells[Index].MaterialId);
		TestEqual(
			TEXT("Snapshot carries support height for occupied cells"),
			ClientCells[Index].SupportHeight,
			ServerCells[Index].SupportHeight);
		TestEqual(
			TEXT("Snapshot carries exact per-cell material amount"),
			ClientCells[Index].Amount,
			ServerCells[Index].Amount);
	}

	const int32 MaterialCountBeforeRejectedImport =
		LateClientWorld.CountMaterial(TEXT("water"))
		+ LateClientWorld.CountMaterial(TEXT("sand"))
		+ LateClientWorld.CountMaterial(TEXT("lava"))
		+ LateClientWorld.CountMaterial(TEXT("steam"));
	TArray<uint8> CorruptedSnapshot = Snapshot;
	CorruptedSnapshot.Add(0xff);
	ImportedStep = 999;
	ImportedFocus = FIntPoint(999, 999);
	TestFalse(
		TEXT("Trailing snapshot data is rejected"),
		LateClientWorld.ImportActiveState(
			CorruptedSnapshot,
			ImportedStep,
			ImportedFocus,
			Error));
	TestEqual(
		TEXT("Rejected snapshot clears its logical-step output"),
		ImportedStep,
		INDEX_NONE);
	TestEqual(
		TEXT("Rejected snapshot does not mutate material occupancy"),
		LateClientWorld.CountMaterial(TEXT("water"))
			+ LateClientWorld.CountMaterial(TEXT("sand"))
			+ LateClientWorld.CountMaterial(TEXT("lava"))
			+ LateClientWorld.CountMaterial(TEXT("steam")),
		MaterialCountBeforeRejectedImport);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReplicatedSnapshotCompressionTest,
	"MatterFlux.Material.ReplicatedSnapshotCompressionIsBoundedAndLossless",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxReplicatedSnapshotCompressionTest::RunTest(
	const FString& Parameters)
{
	TArray<uint8> StructuredState;
	StructuredState.Reserve(8 * 1024);
	for (int32 Record = 0; Record < 1024; ++Record)
	{
		const uint16 Cell = static_cast<uint16>(Record % 4096);
		StructuredState.Add(static_cast<uint8>(Cell & 0xff));
		StructuredState.Add(static_cast<uint8>(Cell >> 8));
		StructuredState.Add(static_cast<uint8>(Record % 7));
		StructuredState.Add(0);
		StructuredState.Add(static_cast<uint8>((Record / 64) & 0xff));
		StructuredState.Add(0);
		StructuredState.Add(0);
		StructuredState.Add(0);
	}

	FMatterFluxReplicatedMaterialState State;
	FString Error;
	if (!TestTrue(
		TEXT("A structured active snapshot compresses inside one safe actor bunch"),
		State.EncodeActiveState(StructuredState, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("Compressed snapshot is materially smaller than its source"),
		State.GetCompressedByteCount() < StructuredState.Num() / 2);

	TArray<uint8> Decoded;
	TestTrue(
		TEXT("Compressed snapshot decodes"),
		State.DecodeActiveState(Decoded, Error));
	TestEqual(
		TEXT("Decoded snapshot matches every authoritative byte"),
		Decoded,
		StructuredState);

	FMatterFluxReplicatedMaterialState Corrupt = State;
	Corrupt.StateHash ^= 0x5a5a5a5au;
	Decoded = { 1, 2, 3 };
	TestFalse(
		TEXT("Corrupt compressed snapshot is rejected"),
		Corrupt.DecodeActiveState(Decoded, Error));
	TestTrue(
		TEXT("A rejected snapshot does not expose partial decoded bytes"),
		Decoded.IsEmpty());

	TArray<uint8> Incompressible;
	Incompressible.SetNumUninitialized(16 * 1024);
	uint32 RandomState = 0x6d2b79f5u;
	for (uint8& Byte : Incompressible)
	{
		RandomState ^= RandomState << 13;
		RandomState ^= RandomState >> 17;
		RandomState ^= RandomState << 5;
		Byte = static_cast<uint8>(RandomState & 0xff);
	}
	FMatterFluxReplicatedMaterialState Oversized;
	TestFalse(
		TEXT("An incompressible snapshot that cannot fit one actor bunch is rejected before replication"),
		Oversized.EncodeActiveState(Incompressible, Error));
	TestFalse(
		TEXT("Rejected oversized state leaves no replicated payload"),
		Oversized.HasPayload());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGroundStateChunkRoundTripTest,
	"MatterFlux.Material.GroundStateChunkIsAtomicBoundedAndLossless",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGroundStateChunkRoundTripTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 Width = MatterFlux::PlayableLevel::TerrainCellsX;
	constexpr int32 Height = MatterFlux::PlayableLevel::TerrainCellsY;
	const FIntPoint ChunkCoordinate(3, 2);
	TArray<uint8> Residue;
	TArray<uint8> Burning;
	Residue.Init(0, Width * Height);
	Burning.Init(0, Width * Height);
	for (int32 LocalY = 0; LocalY < 64; ++LocalY)
	{
		for (int32 LocalX = 0; LocalX < 64; ++LocalX)
		{
			const int32 X = ChunkCoordinate.X * 64 + LocalX;
			const int32 Y = ChunkCoordinate.Y * 64 + LocalY;
			const int32 Index = Y * Width + X;
			Residue[Index] = (LocalX + LocalY * 3) % 11 == 0 ? 1 : 0;
			Burning[Index] = (LocalX * 5 + LocalY) % 17 < 3
				? static_cast<uint8>(12 - (LocalY % 5))
				: 0;
		}
	}

	FMatterFluxGroundStateChunk State;
	FString Error;
	if (!TestTrue(
		TEXT("A 64x64 ground state chunk encodes"),
		State.Encode(
			ChunkCoordinate,
			37,
			Residue,
			Burning,
			Width,
			Height,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("Chunk payload always fits one actor bunch"),
		State.StateBytes.Num() > 0 && State.StateBytes.Num() <= 4608);

	TArray<uint8> DecodedResidue;
	TArray<uint8> DecodedBurning;
	DecodedResidue.Init(0, Width * Height);
	DecodedBurning.Init(0, Width * Height);
	TestTrue(
		TEXT("Chunk applies to a full client mask"),
		State.DecodeInto(
			DecodedResidue,
			DecodedBurning,
			Width,
			Height,
			Error));
	TestEqual(TEXT("Chunk residue is exact"), DecodedResidue, Residue);
	TestEqual(TEXT("Chunk burn timers are exact"), DecodedBurning, Burning);

	FMatterFluxGroundStateChunk Corrupt = State;
	Corrupt.StateHash ^= 0xdeadbeefu;
	TArray<uint8> UntouchedResidue;
	TArray<uint8> UntouchedBurning;
	UntouchedResidue.Init(0, Width * Height);
	UntouchedBurning.Init(0, Width * Height);
	TestFalse(
		TEXT("CRC-corrupt chunk is rejected before mask mutation"),
		Corrupt.DecodeInto(
			UntouchedResidue,
			UntouchedBurning,
			Width,
			Height,
			Error));
	TestFalse(
		TEXT("Rejected chunk leaves residue untouched"),
		UntouchedResidue.ContainsByPredicate(
			[](const uint8 Value) { return Value != 0; }));
	TestFalse(
		TEXT("Rejected chunk leaves burning state untouched"),
		UntouchedBurning.ContainsByPredicate(
			[](const uint8 Value) { return Value != 0; }));
	return true;
}
