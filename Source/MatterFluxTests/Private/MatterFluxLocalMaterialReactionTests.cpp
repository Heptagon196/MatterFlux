#include "Material/MatterFluxLocalMaterialReaction.h"
#include "MatterFluxContentTypes.h"
#include "Misc/AutomationTest.h"

namespace MatterFluxLocalReactionTests
{
	FMaterialElementAddress World(const int32 X)
	{
		return FMaterialElementAddress::MakeWorldCell(FIntVector(X, 0, 0));
	}

	FMaterialThermalDefinition Thermal(
		const uint16 Material,
		const uint16 Conductivity = 0)
	{
		FMaterialThermalDefinition Result;
		Result.MaterialIndex = Material;
		Result.DefaultEnergy = 100;
		Result.ConductivityPermille = Conductivity;
		return Result;
	}

	FMaterialElementStore MakeStore()
	{
		FMaterialElementStore Store;
		Store.SetInitialState(World(0), { 1, 255, 1000, 0 });
		Store.SetInitialState(World(1), { 2, 255, 0, 0 });
		return Store;
	}

	FLocalMaterialReactionContext Context()
	{
		FLocalMaterialReactionContext Result;
		Result.Seed = 77;
		Result.LogicalStep = 10;
		Result.MaxContacts = 64;
		Result.MaxElementDeltas = 64;
		Result.MaxEmissions = 16;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialEnergyFieldTest,
	"MatterFlux.Material.LocalReaction.SparseEnergyFieldIsNotTopology",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxMaterialEnergyFieldTest::RunTest(const FString& Parameters)
{
	FMaterialVolumeTopology Topology;
	Topology.DefinitionId = TEXT("energy.test");
	Topology.TopologyRevision = 8;
	const uint64 TopologyHash =
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Topology);

	FMaterialVolumeFields Fields;
	Fields.EnvironmentEnergy = 100;
	Fields.FieldRevision = 3;
	TestEqual(TEXT("Missing field reads environment energy"),
		static_cast<int32>(Fields.GetEnergy(FIntVector(1, 2, 3))), 100);
	TestTrue(TEXT("Non-environment energy writes a sparse override"),
		Fields.SetEnergy(FIntVector(1, 2, 3), 700));
	TestEqual(TEXT("Field revision advances"), Fields.FieldRevision, 4);
	TestEqual(TEXT("Topology revision does not advance"),
		Topology.TopologyRevision, 8);
	TestEqual(TEXT("Topology hash is independent of energy"),
		FMaterialVolumeAlgorithms::ComputeLogicalHash(Topology), TopologyHash);
	TestTrue(TEXT("Restoring environment deletes the sparse override"),
		Fields.SetEnergy(FIntVector(1, 2, 3), 100));
	TestEqual(TEXT("No redundant field values remain"),
		Fields.EnergyOverrides.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialConductionTest,
	"MatterFlux.Material.LocalReaction.IntegerConductionConservesEnergy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialConductionTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxLocalReactionTests;
	FMaterialElementStore Store = MakeStore();
	const TArray<FMaterialThermalDefinition> Materials = {
		Thermal(1, 500), Thermal(2, 500)
	};
	const TArray<FMaterialContact> Contacts = {
		FMaterialContact(World(1), World(0), 1)
	};
	FMaterialDeltaBatch Batch;
	FString Error;
	TestTrue(TEXT("Kernel evaluates conduction"),
		FLocalMaterialReactionKernel::Evaluate(
			Store, Materials, {}, Contacts, Context(), Batch, Error));
	TestEqual(TEXT("One delta is emitted per touched element"),
		Batch.ElementDeltas.Num(), 2);
	TestEqual(TEXT("Total specific energy starts conserved"),
		Batch.ComputeExpectedEnergyBefore(),
		Batch.ComputeExpectedEnergyAfterIncludingExplicitSources());

	TArray<FMaterialParticleEmission> Emissions;
	TestTrue(TEXT("Validated batch commits atomically"),
		Store.ApplyBatch(Batch, Emissions, Error));
	FMaterialElementState Hot;
	FMaterialElementState Cold;
	Store.TryGetState(World(0), Hot);
	Store.TryGetState(World(1), Cold);
	TestEqual(TEXT("Hot element transfers half its difference"),
		static_cast<int32>(Hot.Energy), 500);
	TestEqual(TEXT("Cold element receives the same energy"),
		static_cast<int32>(Cold.Energy), 500);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialMultiContactConductionTest,
	"MatterFlux.Material.LocalReaction.MultiContactMapGrowthConservesEnergy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialMultiContactConductionTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxLocalReactionTests;
	FMaterialElementStore Store;
	Store.SetInitialState(World(0), { 1, 255, 50000, 0 });
	Store.SetInitialState(World(1), { 1, 255, 100, 0 });
	Store.SetInitialState(World(2), { 1, 255, 100, 0 });
	Store.SetInitialState(World(3), { 1, 255, 100, 0 });
	FLocalMaterialReactionContext MultiContext = Context();
	MultiContext.bApplyCooling = false;
	FMaterialDeltaBatch Batch;
	FString Error;
	TestTrue(TEXT("A hot element can contact several newly loaded neighbours"),
		FLocalMaterialReactionKernel::Evaluate(
			Store,
			{ Thermal(1, 500) },
			{},
			{
				FMaterialContact(World(0), World(1), 1),
				FMaterialContact(World(0), World(2), 1),
				FMaterialContact(World(0), World(3), 1),
			},
			MultiContext,
			Batch,
			Error));
	TestEqual(TEXT("Conduction has no authored energy source"),
		Batch.ExplicitEnergyDelta, static_cast<int64>(0));
	TestEqual(TEXT("Multi-contact energy remains exact"),
		Batch.ComputeExpectedEnergyBefore(),
		Batch.ComputeExpectedEnergyAfterIncludingExplicitSources());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialIsolatedCoolingTest,
	"MatterFlux.Material.LocalReaction.IsolatedHotElementCoolsWithoutContact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialIsolatedCoolingTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxLocalReactionTests;
	FMaterialElementStore Store;
	Store.SetInitialState(World(0), { 1, 255, 50000, 0 });
	FMaterialThermalDefinition HotSolid = Thermal(1);
	HotSolid.CoolingPerStep = 200;
	FLocalMaterialReactionContext CoolingContext = Context();
	CoolingContext.CoolingElements.Add(World(0));
	FMaterialDeltaBatch Batch;
	FString Error;
	TestTrue(TEXT("The kernel accepts a cooling-only fixed step"),
		FLocalMaterialReactionKernel::Evaluate(
			Store,
			{ HotSolid },
			{},
			{},
			CoolingContext,
			Batch,
			Error));
	const FMaterialElementDelta* Delta = Batch.ElementDeltas.FindByPredicate(
		[](const FMaterialElementDelta& Candidate)
		{
			return Candidate.Address == World(0);
		});
	TestNotNull(TEXT("Cooling-only evaluation produces one atomic delta"), Delta);
	if (Delta)
	{
		TestEqual(TEXT("The isolated element loses configured specific energy"),
			static_cast<int32>(Delta->After.Energy), 49800);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialOrderingTest,
	"MatterFlux.Material.LocalReaction.InputOrderDoesNotAffectBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialOrderingTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxLocalReactionTests;
	FMaterialElementStore Store;
	Store.SetInitialState(World(0), { 1, 1, 900, 0 });
	Store.SetInitialState(World(1), { 1, 1, 300, 0 });
	Store.SetInitialState(World(2), { 1, 1, 0, 0 });
	const TArray<FMaterialThermalDefinition> Materials = { Thermal(1, 500) };
	const TArray<FMaterialContact> Forward = {
		FMaterialContact(World(0), World(1), 1),
		FMaterialContact(World(1), World(2), 1)
	};
	const TArray<FMaterialContact> Reverse = { Forward[1], Forward[0] };
	FMaterialDeltaBatch First;
	FMaterialDeltaBatch Second;
	FString Error;
	TestTrue(TEXT("Forward input evaluates"),
		FLocalMaterialReactionKernel::Evaluate(
			Store, Materials, {}, Forward, Context(), First, Error));
	TestTrue(TEXT("Reverse input evaluates"),
		FLocalMaterialReactionKernel::Evaluate(
			Store, Materials, {}, Reverse, Context(), Second, Error));
	TestEqual(TEXT("Canonical batch hashes match"),
		First.ComputeDeterministicHash(), Second.ComputeDeterministicHash());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialReactionEmissionTest,
	"MatterFlux.Material.LocalReaction.ContactTransformAndEmissionConserveAmount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialReactionEmissionTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxLocalReactionTests;
	FMaterialElementStore Store;
	Store.SetInitialState(World(0), { 1, 2, 200, 0 });
	Store.SetInitialState(World(1), { 2, 2, 400, 0 });
	FLocalMaterialContactRule Rule;
	Rule.RuleId = TEXT("water_lava");
	Rule.InputA = 1;
	Rule.InputB = 2;
	Rule.OutputA = 3;
	Rule.OutputB = 4;
	Rule.ChancePermille = 1000;
	Rule.Emissions.Add({ 5, 1, 300, EMaterialEmissionSourceSide::A });
	FMaterialDeltaBatch Batch;
	FString Error;
	TestTrue(TEXT("Contact rule evaluates"),
		FLocalMaterialReactionKernel::Evaluate(
			Store,
			{ Thermal(1), Thermal(2), Thermal(3), Thermal(4), Thermal(5) },
			{ Rule },
			{ FMaterialContact(World(1), World(0), 1) },
			Context(),
			Batch,
			Error));
	TestEqual(TEXT("Element amount plus emitted amount is conserved"),
		Batch.ComputeExpectedAmountBefore(),
		Batch.ComputeExpectedAmountAfterIncludingEmissions());
	TArray<FMaterialParticleEmission> Emissions;
	TestTrue(TEXT("Reaction batch commits"),
		Store.ApplyBatch(Batch, Emissions, Error));
	TestEqual(TEXT("Exactly one ordinary material emission is produced"),
		Emissions.Num(), 1);
	FMaterialElementState WaterSide;
	FMaterialElementState LavaSide;
	Store.TryGetState(World(0), WaterSide);
	Store.TryGetState(World(1), LavaSide);
	TestEqual(TEXT("Paired output follows matched input A"),
		static_cast<int32>(WaterSide.MaterialIndex), 3);
	TestEqual(TEXT("Reverse contact preserves paired output B"),
		static_cast<int32>(LavaSide.MaterialIndex), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialAtomicRollbackTest,
	"MatterFlux.Material.LocalReaction.StaleBatchHasZeroSideEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialAtomicRollbackTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxLocalReactionTests;
	FMaterialElementStore Store = MakeStore();
	FMaterialDeltaBatch Batch;
	FString Error;
	TestTrue(TEXT("Initial batch evaluates"),
		FLocalMaterialReactionKernel::Evaluate(
			Store,
			{ Thermal(1, 500), Thermal(2, 500) },
			{},
			{ FMaterialContact(World(0), World(1), 1) },
			Context(),
			Batch,
			Error));
	Store.SetInitialState(World(0), { 1, 1, 750, 0 });
	const uint64 Before = Store.ComputeDeterministicHash();
	TArray<FMaterialParticleEmission> Emissions;
	TestFalse(TEXT("Stale base revision rejects the whole batch"),
		Store.ApplyBatch(Batch, Emissions, Error));
	TestEqual(TEXT("Rejected commit leaves every element unchanged"),
		Store.ComputeDeterministicHash(), Before);
	TestEqual(TEXT("Rejected commit emits nothing"), Emissions.Num(), 0);
	TestFalse(TEXT("Rejection reports a useful error"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialExplicitSinkTest,
	"MatterFlux.Material.LocalReaction.EmptyOutputIsAnAccountedAtomicSink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialExplicitSinkTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxLocalReactionTests;
	FMaterialElementStore Store;
	Store.SetInitialState(World(0), { 1, 3, 200, 0 });
	Store.SetInitialState(World(1), { 2, 2, 400, 0 });
	FLocalMaterialContactRule Rule;
	Rule.RuleId = TEXT("acid_corrosion");
	Rule.InputA = 1;
	Rule.InputB = 2;
	Rule.OutputA = 0;
	Rule.OutputB = 0;
	FMaterialDeltaBatch Batch;
	FString Error;
	TestTrue(TEXT("Empty-output contact evaluates"),
		FLocalMaterialReactionKernel::Evaluate(
			Store, { Thermal(1), Thermal(2) }, { Rule },
			{ FMaterialContact(World(0), World(1), 1) },
			Context(), Batch, Error));
	TestEqual(TEXT("Removed amount is explicitly accounted"),
		Batch.ExplicitAmountDelta, static_cast<int64>(-5));
	TestEqual(TEXT("Removed total energy is explicitly accounted"),
		Batch.ExplicitEnergyDelta, static_cast<int64>(-1400));
	TArray<FMaterialParticleEmission> Emissions;
	TestTrue(TEXT("Sink batch commits atomically"),
		Store.ApplyBatch(Batch, Emissions, Error));
	FMaterialElementState First;
	FMaterialElementState Second;
	Store.TryGetState(World(0), First);
	Store.TryGetState(World(1), Second);
	TestEqual(TEXT("First element becomes empty"),
		static_cast<int32>(First.Amount), 0);
	TestEqual(TEXT("Second element becomes empty"),
		static_cast<int32>(Second.Amount), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialProgramTest,
	"MatterFlux.Material.LocalReaction.ProgramCompilesNamesOnceForEveryElementKind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialProgramTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxContentRegistry Registry;
	FMatterFluxMaterialDefinition Acid;
	Acid.Id = TEXT("acid");
	Acid.DefaultEnergy = 120;
	Acid.ConductivityPermille = 500;
	Registry.Materials.Add(Acid.Id, Acid);
	FMatterFluxMaterialDefinition Wood;
	Wood.Id = TEXT("wood");
	Wood.DefaultEnergy = 80;
	Wood.ConductivityPermille = 180;
	Registry.Materials.Add(Wood.Id, Wood);
	FMatterFluxReactionDefinition Corrosion;
	Corrosion.Id = TEXT("acid_wood_corrosion");
	Corrosion.InputA = Acid.Id;
	Corrosion.InputB = Wood.Id;
	Corrosion.OutputA = TEXT("empty");
	Corrosion.OutputB = TEXT("empty");
	Registry.Reactions.Add(Corrosion.Id, Corrosion);

	FLocalMaterialReactionProgram Program;
	FString Error;
	TestTrue(TEXT("Authored registry compiles once"),
		Program.Compile(Registry, Error));
	FMaterialElementState AcidState;
	FMaterialElementState WoodState;
	TestTrue(TEXT("World material name resolves"),
		Program.MakeState(Acid.Id, 5, TOptional<uint16>(), AcidState));
	TestTrue(TEXT("Volume material name resolves through the same table"),
		Program.MakeState(Wood.Id, 255, TOptional<uint16>(), WoodState));
	TestEqual(TEXT("Authored default energy is applied"),
		static_cast<int32>(WoodState.Energy), 80);

	FLocalMaterialReactionContext Context;
	Context.Seed = 41;
	Context.LogicalStep = 7;
	Context.MaxContacts = 1;
	Context.MaxElementDeltas = 2;
	Context.MaxEmissions = 2;
	Context.bApplyCooling = false;
	FMaterialDeltaBatch Batch;
	const FMaterialElementAddress WorldAddress =
		FMaterialElementAddress::MakeWorldCell(FIntVector(2, 3, 4));
	const FMaterialElementAddress VolumeAddress =
		FMaterialElementAddress::MakeVolumeCell(
			FGuid(1, 2, 3, 4), FIntVector(5, 6, 7));
	TestTrue(TEXT("World and Volume elements use the compiled kernel program"),
		Program.EvaluatePair(
			WorldAddress, AcidState,
			VolumeAddress, WoodState,
			0, Context, Batch, Error));
	TestEqual(TEXT("Both contact sides produce one atomic delta"),
		Batch.ElementDeltas.Num(), 2);
	for (const FMaterialElementDelta& Delta : Batch.ElementDeltas)
	{
		TestEqual(TEXT("Corroded element becomes empty"),
			static_cast<int32>(Delta.After.Amount), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLocalMaterialIgnitionSourceTest,
	"MatterFlux.Material.LocalReaction.IgnitionExhaustIsAnExplicitConfiguredSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FMatterFluxLocalMaterialIgnitionSourceTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxLocalReactionTests;
	FMaterialElementStore Store;
	Store.SetInitialState(World(0), { 1, 255, 600, 0 });
	Store.SetInitialState(World(1), { 2, 255, 600, 0 });
	FMaterialThermalDefinition Wood = Thermal(1);
	Wood.IgnitionThreshold = 500;
	Wood.IgnitionProductMaterialIndex = 3;
	Wood.IgnitionProductEnergy = 1000;
	Wood.IgnitionEmissionMaterialIndex = 4;
	Wood.IgnitionEmissionAmount = 1;
	Wood.IgnitionSecondaryEmissionMaterialIndex = 5;
	Wood.IgnitionSecondaryEmissionAmount = 1;
	FLocalMaterialReactionContext IgnitionContext = Context();
	IgnitionContext.bApplyCooling = false;
	FMaterialDeltaBatch Batch;
	FString Error;
	TestTrue(TEXT("Hot combustible material ignites locally"),
		FLocalMaterialReactionKernel::Evaluate(
			Store,
			{ Wood, Thermal(2), Thermal(3), Thermal(4), Thermal(5) },
			{},
			{ FMaterialContact(World(0), World(1), 1) },
			IgnitionContext,
			Batch,
			Error));
	TestEqual(TEXT("Two configured exhausts create two ordinary particles"),
		Batch.ParticleEmissions.Num(), 2);
	TestEqual(TEXT("Exhaust is explicitly accounted material input"),
		Batch.ExplicitAmountDelta, static_cast<int64>(2));
	const FMaterialElementDelta* WoodDelta = Batch.ElementDeltas.FindByPredicate(
		[](const FMaterialElementDelta& Delta)
		{
			return Delta.Address == World(0);
		});
	TestNotNull(TEXT("Ignited cell has a material delta"), WoodDelta);
	if (WoodDelta)
	{
		TestEqual(TEXT("Ignition changes the cell material"),
			static_cast<int32>(WoodDelta->After.MaterialIndex), 3);
		TestEqual(TEXT("Topology cell amount remains full"),
			static_cast<int32>(WoodDelta->After.Amount), 255);
		TestEqual(TEXT("Configured combustion heat becomes product energy"),
			static_cast<int32>(WoodDelta->After.Energy), 1000);
	}
	TestEqual(TEXT("Combustion heat and exhaust are explicit energy sources"),
		Batch.ExplicitEnergyDelta, static_cast<int64>(104000));
	return true;
}
