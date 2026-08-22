#include "Material/MatterFluxMaterialReactionEngine.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxUnifiedMaterialReactionEngineTest,
	"MatterFlux.Material.ReactionEngineRunsContactAndPropagatingRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxUnifiedMaterialReactionEngineTest::RunTest(const FString& Parameters)
{
	FMatterFluxReactionDefinition Contact;
	Contact.Id = TEXT("water_lava_quench");
	Contact.InputA = TEXT("water");
	Contact.InputB = TEXT("lava");
	Contact.OutputA = TEXT("steam");
	Contact.OutputB = TEXT("stone");
	Contact.ChancePermille = 1000;

	MatterFlux::Reaction::FContactResult ContactResult;
	MatterFlux::Reaction::FDeterministicContext Context;
	Context.Seed = 77;
	TestTrue(TEXT("Contact rule is accepted"),
		MatterFlux::Reaction::FMaterialReactionEngine::EvaluateContact(
			Contact, TEXT("lava"), TEXT("water"), Context, ContactResult));
	TestTrue(TEXT("Contact rule reacts"), ContactResult.bReacted);
	TestEqual(TEXT("Reverse input preserves paired output"),
		ContactResult.FirstMaterial, FName(TEXT("stone")));

	FMatterFluxReactionDefinition Burning;
	Burning.Id = TEXT("leaf_oxidation");
	Burning.Kind = FMatterFluxReactionDefinition::EKind::Propagating;
	Burning.InputA = TEXT("leaf");
	Burning.InputB = TEXT("fire");
	Burning.OutputA = TEXT("ash");
	Burning.OutputB = TEXT("fire");
	Burning.EmissionMaterial = TEXT("smoke");
	Burning.ChancePermille = 1000;
	Burning.PropagationChancePermille = 1000;
	Burning.DurationSteps = 2;
	Burning.EmissionChancePermille = 1000;

	FFragmentSourceMask Mask;
	Mask.Width = 3;
	Mask.Height = 1;
	Mask.CellSize = 4.0f;
	Mask.SolidMask.Init(1, 3);
	MatterFlux::Reaction::FMaterialReactionEngine Engine;
	FString Error;
	if (!TestTrue(TEXT("Propagating rule initializes the same engine"),
		Engine.InitializeGrid(Mask, Burning, 77, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Configured stimulus activates input material"),
		Engine.Activate(FIntPoint(0, 0), TEXT("fire")));
	const MatterFlux::Reaction::FGridStepResult First = Engine.Step(1);
	TestEqual(TEXT("Propagation obeys per-step budget"), First.ActivatedCells, 1);
	TestEqual(TEXT("Active reaction emits configured material event"),
		First.EmissionCells.Num(), 1);
	const MatterFlux::Reaction::FGridStepResult Second = Engine.Step(1);
	TestEqual(TEXT("Duration completion transforms input into output"),
		Second.CompletedCells, 1);
	TestEqual(TEXT("Completed cell leaves the input mask"),
		static_cast<int32>(Engine.GetInputMask()[0]), 0);
	TestEqual(TEXT("Completed cell enters the output mask"),
		static_cast<int32>(Engine.GetOutputMask()[0]), 1);

	FMatterFluxReactionDefinition Corrosion = Burning;
	Corrosion.Id = TEXT("metal_acid_corrosion");
	Corrosion.InputA = TEXT("metal");
	Corrosion.InputB = TEXT("acid");
	Corrosion.OutputA = TEXT("empty");
	Corrosion.OutputB = TEXT("acid");
	Corrosion.EmissionMaterial = TEXT("empty");
	Corrosion.EmissionChancePermille = 0;
	FFragmentSourceMask MetalMask = Mask;
	MatterFlux::Reaction::FMaterialReactionEngine CorrosionEngine;
	TestTrue(TEXT("A non-fire propagating rule needs no fake emission"),
		CorrosionEngine.InitializeGrid(MetalMask, Corrosion, 19, Error));
	TestTrue(TEXT("A non-fire stimulus activates the generic grid"),
		CorrosionEngine.Activate(FIntPoint(1, 0), TEXT("acid")));
	TestEqual(TEXT("No emission produces no presentation events"),
		CorrosionEngine.Step(0).EmissionCells.Num(), 0);

	FMatterFluxContentRegistry Registry;
	FMatterFluxReactionDefinition Later = Corrosion;
	Later.Id = TEXT("z_rule");
	FMatterFluxReactionDefinition Earlier = Corrosion;
	Earlier.Id = TEXT("a_rule");
	Registry.Reactions.Add(Later.Id, Later);
	Registry.Reactions.Add(Earlier.Id, Earlier);
	const FMatterFluxReactionDefinition* Found =
		MatterFlux::Reaction::FMaterialReactionEngine::FindPropagatingRule(
			Registry, TEXT("metal"), TEXT("acid"));
	TestTrue(TEXT("Generic rule lookup finds a match"), Found != nullptr);
	if (Found)
	{
		TestEqual(TEXT("Rule lookup is independent of TMap iteration order"),
			Found->Id, FName(TEXT("a_rule")));
	}
	return true;
}
