#include "Game/MatterFluxGameMode.h"
#include "Game/MatterFluxGameState.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "IMatterFluxScriptRuntime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace MatterFluxLuaTests
{
	const TCHAR* ValidSource = TEXT(R"LUA(
content.set_manifest("test.pack", 7, 1)
content.register_material("soil", 1.5, 0.8, 0.3, 0.2, 0.1, 1.0)
content.register_decorator("forest.tree", "tree", "soil", 0.4, 2, 8, true)
content.register_entity("enemy.slime", "slime_wander", 35.0, 180.0)
)LUA");

	static void RestoreDefault(IMatterFluxScriptRuntime& Runtime)
	{
		FString IgnoredError;
		Runtime.ReloadDefaultContentPack(IgnoredError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaModularSpellApiTest,
	"MatterFlux.Lua.DefaultPackUsesModularCapabilityApi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaModularSpellApiTest::RunTest(const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Modular default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr First = Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Default registry exists"), First.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("All spell modules loaded"), First->Spells.Num(), 18);
	TestEqual(TEXT("All wand modules loaded"), First->Wands.Num(), 6);
	TestEqual(TEXT("All item modules loaded"), First->Items.Num(), 2);
	TestEqual(TEXT("All quest modules loaded"), First->Quests.Num(), 10);
	TestEqual(TEXT("Modular content revision"), First->Manifest.Revision, 5);
	const FString FirstHash = First->Manifest.VersionHash;

	const FString LuaRoot = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("Lua"));
	const FString EntryPath = FPaths::Combine(
		LuaRoot,
		TEXT("MatterFluxContent.lua"));
	FString EntrySource;
	TestTrue(
		TEXT("Manifest entry is readable"),
		FFileHelper::LoadFileToString(EntrySource, *EntryPath));
	TestFalse(
		TEXT("Manifest entry no longer mixes spell definitions"),
		EntrySource.Contains(TEXT("register_spell")));

	const FString ExampleSpellPath = FPaths::Combine(
		LuaRoot,
		TEXT("Spells"),
		TEXT("PaperMagic"),
		TEXT("Default.lua"));
	FString ExampleSpellSource;
	TestTrue(
		TEXT("Individual spell module is readable"),
		FFileHelper::LoadFileToString(
			ExampleSpellSource,
			*ExampleSpellPath));
	TestTrue(
		TEXT("Spell module uses the public capability API"),
		ExampleSpellSource.Contains(TEXT("spell.define"))
			&& ExampleSpellSource.Contains(TEXT("api.projectile")));
	TestFalse(
		TEXT("Spell module cannot depend on the raw compiler table"),
		ExampleSpellSource.Contains(TEXT("content.register_spell")));

	const FString ExampleItemPath = FPaths::Combine(
		LuaRoot, TEXT("Items"), TEXT("HealingPotion.lua"));
	FString ExampleItemSource;
	TestTrue(
		TEXT("Item module is readable"),
		FFileHelper::LoadFileToString(ExampleItemSource, *ExampleItemPath));
	TestTrue(
		TEXT("Item module uses the public capability API"),
		ExampleItemSource.Contains(TEXT("item.define"))
			&& ExampleItemSource.Contains(TEXT("use.restore_health")));
	TestFalse(
		TEXT("Item module cannot depend on the raw compiler table"),
		ExampleItemSource.Contains(TEXT("content.register_item")));

	Error.Reset();
	TestTrue(
		TEXT("Second deterministic bundle load succeeds"),
		Runtime.ReloadDefaultContentPack(Error));
	const FMatterFluxContentRegistryPtr Second = Runtime.GetActiveRegistry();
	if (TestTrue(TEXT("Second registry exists"), Second.IsValid()))
	{
		TestEqual(
			TEXT("Module order and bundle hash are deterministic"),
			Second->Manifest.VersionHash,
			FirstHash);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaProgressionDefinitionsTest,
	"MatterFlux.Lua.ProgressionDefinitionsAreValidatedTransactionally",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaProgressionDefinitionsTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString EngineSource;
	if (!TestTrue(
		TEXT("Progression capability engine source is readable"),
		FFileHelper::LoadFileToString(
			EngineSource,
			*Runtime.GetDefaultEngineConfigPath())))
	{
		return false;
	}

	const FString ValidProgressionSource = EngineSource + TEXT(R"LUA(
content.set_manifest("progression.test", 1, 1)
item.define({
    id = "item.coin", name = "Coin", category = "material",
    max_stack = 999
})
item.define({
    id = "item.heal", name = "Heal", category = "consumable",
    max_stack = 10, starter_count = 2
}, function(use)
    use.restore_health(25, 1)
end)
quest.define({
    id = "quest.spend", name = "Spend", description = "Spend one coin",
    category = "main", starter = true, focus_on_activate = true
}, function(q)
    q.spend_item({ target_id = "item.coin", target_count = 1 })
    q.reward("item", "item.heal", 1)
end)
)LUA");
	FString Error;
	if (!TestTrue(
		TEXT("Valid item and quest capability program loads"),
		Runtime.LoadContentPackFromSource(
			ValidProgressionSource,
			TEXT("ProgressionDefinitions"),
			Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (TestTrue(TEXT("Progression registry exists"), Registry.IsValid()))
	{
		const FMatterFluxItemDefinition& Heal =
			Registry->Items.FindChecked(TEXT("item.heal"));
		TestEqual(TEXT("Heal action compiled"), Heal.UseAction,
			EMatterFluxItemUseAction::RestoreHealth);
		TestEqual(TEXT("Heal magnitude compiled"), Heal.UseMagnitude, 25.0f);
		TestEqual(TEXT("Heal consumption compiled"), Heal.ConsumeCount, 1);
		const FMatterFluxQuestDefinition& Quest =
			Registry->Quests.FindChecked(TEXT("quest.spend"));
		TestEqual(TEXT("Quest objective compiled"), Quest.Objective,
			EMatterFluxQuestObjectiveKind::SpendItem);
		TestEqual(TEXT("Quest target compiled"), Quest.TargetId,
			FName(TEXT("item.coin")));
		TestEqual(TEXT("Quest reward compiled"),
			Quest.CompletionRewards.Num(), 1);
	}

	const FMatterFluxContentRegistryPtr Baseline = Runtime.GetActiveRegistry();
	const FString CyclicSource = EngineSource + TEXT(R"LUA(
content.set_manifest("progression.cycle", 1, 1)
quest.define({
    id = "quest.a", description = "A", category = "main",
    prerequisites = { "quest.b" }
}, function(q) q.never() end)
quest.define({
    id = "quest.b", description = "B", category = "main",
    prerequisites = { "quest.a" }
}, function(q) q.never() end)
)LUA");
	Error.Reset();
	TestFalse(
		TEXT("Cyclic quest graph is rejected"),
		Runtime.LoadContentPackFromSource(
			CyclicSource, TEXT("CyclicQuestGraph"), Error));
	TestTrue(TEXT("Cycle error is explicit"), Error.Contains(TEXT("cycle")));
	TestTrue(
		TEXT("Rejected quest graph preserves the active registry"),
		Runtime.GetActiveRegistry() == Baseline);
	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaSpellCapabilityBoundaryTest,
	"MatterFlux.Lua.SpellCapabilityApiRejectsRawFieldsTransactionally",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaSpellCapabilityBoundaryTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Default content establishes the baseline"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Baseline =
		Runtime.GetActiveRegistry();
	FString EngineSource;
	if (!TestTrue(
		TEXT("Spell capability engine source is readable"),
		FFileHelper::LoadFileToString(
			EngineSource,
			*Runtime.GetDefaultEngineConfigPath())))
	{
		return false;
	}
	const FString BypassAttempt = EngineSource + TEXT(R"LUA(
content.set_manifest("invalid.spell.capability", 1, 1)
spell.define({
    id = "invalid.raw_damage",
    name = "Bypass",
    mana_cost = 1,
    damage = 999
}, function(api)
    api.projectile({ speed = 100, lifetime = 1, radius = 1 })
end)
)LUA");
	Error.Reset();
	TestFalse(
		TEXT("Behavior fields cannot bypass the capability API via metadata"),
		Runtime.LoadContentPackFromSource(
			BypassAttempt,
			TEXT("SpellCapabilityBoundary"),
			Error));
	TestTrue(
		TEXT("Error identifies the forbidden metadata field"),
		Error.Contains(TEXT("unknown spell metadata field 'damage'")));
	TestTrue(
		TEXT("Rejected capability program preserves the active registry"),
		Runtime.GetActiveRegistry() == Baseline);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaRegistrationTest,
	"MatterFlux.Lua.ValidPackRegistersDefinitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaRegistrationTest::RunTest(const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	const bool bLoaded = Runtime.LoadContentPackFromSource(
		MatterFluxLuaTests::ValidSource,
		TEXT("ValidPackRegistersDefinitions"),
		Error);
	TestTrue(FString::Printf(TEXT("Valid pack loads: %s"), *Error), bLoaded);

	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (TestTrue(TEXT("A registry is active"), Registry.IsValid()))
	{
		TestEqual(TEXT("Pack id"), Registry->Manifest.PackId, TEXT("test.pack"));
		TestEqual(TEXT("Schema"), Registry->Manifest.SchemaVersion, 1);
		TestEqual(TEXT("Revision"), Registry->Manifest.Revision, 7);
		TestEqual(TEXT("Material count"), Registry->Materials.Num(), 1);
		TestEqual(TEXT("Decorator count"), Registry->Decorators.Num(), 1);
		TestEqual(TEXT("Entity count"), Registry->Entities.Num(), 1);
		TestTrue(TEXT("Version hash is present"), !Registry->Manifest.VersionHash.IsEmpty());
		TestTrue(TEXT("Decorator collision flag is registered"),
			Registry->Decorators.FindChecked(TEXT("forest.tree")).bEnableCollision);
	}

	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaFragmentationSettingsTest,
	"MatterFlux.Lua.FragmentationSettingsAreValidatedAndTransactional",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaFragmentationSettingsTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	const FString ConfiguredSource = TEXT(R"LUA(
content.configure_fragmentation(7)
content.set_manifest("fragmentation.pack", 1, 1)
)LUA");
	if (TestTrue(
		TEXT("Valid fragmentation settings load"),
		Runtime.LoadContentPackFromSource(
			ConfiguredSource,
			TEXT("FragmentationSettings"),
			Error)))
	{
		const FMatterFluxContentRegistryPtr Registry =
			Runtime.GetActiveRegistry();
		if (TestTrue(TEXT("Configured registry exists"), Registry.IsValid()))
		{
			TestEqual(
				TEXT("Minimum detached area reaches the registry"),
				Registry->Fragmentation.MinDetachedAreaPixels,
				7);
		}
	}

	const FMatterFluxContentRegistryPtr Baseline =
		Runtime.GetActiveRegistry();
	const FString InvalidSource = TEXT(R"LUA(
content.configure_fragmentation(0)
content.set_manifest("invalid.fragmentation", 1, 1)
)LUA");
	Error.Reset();
	TestFalse(
		TEXT("Zero detached area is rejected"),
		Runtime.LoadContentPackFromSource(
			InvalidSource,
			TEXT("InvalidFragmentationSettings"),
			Error));
	TestTrue(
		TEXT("Validation error explains the valid range"),
		Error.Contains(TEXT("between 1 and 65536")));
	TestTrue(
		TEXT("Invalid engine settings preserve the active registry"),
		Runtime.GetActiveRegistry() == Baseline);

	MatterFluxLuaTests::RestoreDefault(Runtime);
	const FMatterFluxContentRegistryPtr DefaultRegistry =
		Runtime.GetActiveRegistry();
	if (TestTrue(
		TEXT("Default engine and content scripts load together"),
		DefaultRegistry.IsValid()))
	{
		TestEqual(
			TEXT("Separate default engine script supplies the threshold"),
			DefaultRegistry->Fragmentation.MinDetachedAreaPixels,
			4);
	}
	TestTrue(
		TEXT("Engine settings live in their own Lua file"),
		Runtime.GetDefaultEngineConfigPath().EndsWith(
			TEXT("MatterFluxEngine.lua")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaMaterialSimulationRegistrationTest,
	"MatterFlux.Lua.MaterialSimulationDefinitionsRegister",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaMaterialSimulationRegistrationTest::RunTest(
	const FString& Parameters)
{
	const FString Source = TEXT(R"LUA(
content.set_manifest("simulation.pack", 1, 1)
content.register_material("water", 1.0, 0.1, 0.1, 0.4, 0.9, 0.8, "liquid", 255, 220)
content.register_material("lava", 2.8, 0.2, 1.0, 0.2, 0.0, 1.0, "liquid", 96, 32)
content.register_material("steam", 0.1, 0.0, 0.8, 0.8, 0.8, 0.5, "gas", 255, 255)
content.register_material("stone", 3.0, 1.0, 0.3, 0.3, 0.3, 1.0, "static", 0, 0)
content.register_material("wood", 0.8, 0.7, 0.3, 0.2, 0.1, 1.0, "static", 0, 0)
content.register_material("fire", 0.01, 0.0, 1.0, 0.3, 0.0, 0.9, "gas", 255, 255)
content.register_material("smoke", 0.05, 0.0, 0.2, 0.2, 0.2, 0.6, "gas", 255, 220)
content.register_material("charcoal", 0.7, 0.4, 0.08, 0.07, 0.06, 1.0, "static", 0, 0)
content.register_reaction("water_lava", "water", "lava", "steam", "stone", 1000)
content.register_combustion("wood_burn", "wood", "fire", "smoke", "charcoal", 1000, 650, 12, 700)
)LUA");

	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Simulation content loads"),
		Runtime.LoadContentPackFromSource(
			Source,
			TEXT("MaterialSimulationDefinitions"),
			Error)))
	{
		AddError(Error);
		MatterFluxLuaTests::RestoreDefault(Runtime);
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (TestTrue(TEXT("Simulation registry exists"), Registry.IsValid()))
	{
		const FMatterFluxMaterialDefinition& Water =
			Registry->Materials.FindChecked(TEXT("water"));
		TestTrue(TEXT("Lua material phase is parsed"),
			Water.Phase == EMatterFluxMaterialPhase::Liquid);
		TestEqual(TEXT("Lua material mobility is parsed"),
			static_cast<int32>(Water.Mobility),
			255);
		TestEqual(TEXT("Lua material dispersion is parsed"),
			static_cast<int32>(Water.Dispersion),
			220);
		const FMatterFluxReactionDefinition& Reaction =
			Registry->Reactions.FindChecked(TEXT("water_lava"));
		TestEqual(TEXT("Lua reaction input A"), Reaction.InputA, FName(TEXT("water")));
		TestEqual(TEXT("Lua reaction output B"), Reaction.OutputB, FName(TEXT("stone")));
		TestEqual(TEXT("Lua reaction probability"), Reaction.ChancePermille, 1000);
		const FMatterFluxCombustionDefinition& Combustion =
			Registry->Combustions.FindChecked(TEXT("wood_burn"));
		TestEqual(TEXT("Lua combustion fuel"), Combustion.FuelMaterial, FName(TEXT("wood")));
		TestEqual(TEXT("Lua combustion residue"), Combustion.ResidueMaterial, FName(TEXT("charcoal")));
		TestEqual(TEXT("Lua combustion duration"), Combustion.BurnDurationSteps, 12);
		TestEqual(TEXT("Lua combustion smoke probability"), Combustion.SmokeChancePermille, 700);
	}

	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaDeterminismTest,
	"MatterFlux.Lua.SameSourceProducesSameVersionAndDefinitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaDeterminismTest::RunTest(const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("First load succeeds"),
		Runtime.LoadContentPackFromSource(
			MatterFluxLuaTests::ValidSource,
			TEXT("DeterminismFirst"),
			Error)))
	{
		MatterFluxLuaTests::RestoreDefault(Runtime);
		return false;
	}
	const FMatterFluxContentRegistryPtr First = Runtime.GetActiveRegistry();
	const FString FirstHash = First->Manifest.VersionHash;
	const FMatterFluxMaterialDefinition FirstSoil =
		First->Materials.FindChecked(TEXT("soil"));
	const FMatterFluxDecoratorDefinition FirstTree =
		First->Decorators.FindChecked(TEXT("forest.tree"));

	if (!TestTrue(
		TEXT("Second load succeeds"),
		Runtime.LoadContentPackFromSource(
			MatterFluxLuaTests::ValidSource,
			TEXT("DeterminismSecond"),
			Error)))
	{
		MatterFluxLuaTests::RestoreDefault(Runtime);
		return false;
	}
	const FMatterFluxContentRegistryPtr Second = Runtime.GetActiveRegistry();
	TestEqual(TEXT("Version hash"), Second->Manifest.VersionHash, FirstHash);
	const FMatterFluxMaterialDefinition& SecondSoil =
		Second->Materials.FindChecked(TEXT("soil"));
	TestEqual(TEXT("Density"), SecondSoil.Density, FirstSoil.Density);
	TestEqual(TEXT("Hardness"), SecondSoil.Hardness, FirstSoil.Hardness);
	TestEqual(TEXT("Color"), SecondSoil.Color, FirstSoil.Color);
	TestEqual(TEXT("Collision flag"),
		Second->Decorators.FindChecked(TEXT("forest.tree")).bEnableCollision,
		FirstTree.bEnableCollision);

	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaDecoratorCollisionTypeTest,
	"MatterFlux.Lua.DecoratorCollisionRequiresBoolean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaDecoratorCollisionTypeTest::RunTest(const FString& Parameters)
{
	const FString Source = TEXT(R"LUA(
content.set_manifest("invalid.collision", 1, 1)
content.register_material("soil", 1.0, 1.0, 0.3, 0.2, 0.1, 1.0)
content.register_decorator("forest.tree", "tree", "soil", 1.0, 1, 1, "yes")
)LUA");

	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	const FMatterFluxContentRegistryPtr Baseline = Runtime.GetActiveRegistry();
	FString Error;
	TestFalse(TEXT("String collision flag is rejected"),
		Runtime.LoadContentPackFromSource(Source, TEXT("CollisionRequiresBoolean"), Error));
	TestTrue(TEXT("Collision type error names the boolean argument"),
		Error.Contains(TEXT("argument 7 must be a boolean")));
	TestTrue(TEXT("Invalid collision flag cannot replace active content"),
		Runtime.GetActiveRegistry() == Baseline);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaTransactionTest,
	"MatterFlux.Lua.InvalidReloadKeepsPreviousRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaTransactionTest::RunTest(const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Baseline load succeeds"),
		Runtime.LoadContentPackFromSource(
			MatterFluxLuaTests::ValidSource,
			TEXT("TransactionBaseline"),
			Error)))
	{
		MatterFluxLuaTests::RestoreDefault(Runtime);
		return false;
	}

	const FMatterFluxContentRegistryPtr Baseline = Runtime.GetActiveRegistry();
	const FString BaselineHash = Baseline->Manifest.VersionHash;
	const FString InvalidSource = TEXT(R"LUA(
content.set_manifest("broken.pack", 1, 1)
content.register_material("same", 1, 1, 1, 1, 1, 1)
content.register_material("same", 1, 1, 1, 1, 1, 1)
)LUA");
	const bool bInvalidLoaded = Runtime.LoadContentPackFromSource(
		InvalidSource,
		TEXT("InvalidReload"),
		Error);
	TestFalse(TEXT("Duplicate ids reject the reload"), bInvalidLoaded);
	TestTrue(TEXT("A useful validation error is returned"), Error.Contains(TEXT("duplicate")));

	const FMatterFluxContentRegistryPtr AfterFailure =
		Runtime.GetActiveRegistry();
	TestTrue(
		TEXT("The committed registry object is unchanged"),
		AfterFailure == Baseline);
	TestEqual(
		TEXT("The committed version is unchanged"),
		AfterFailure->Manifest.VersionHash,
		BaselineHash);

	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaDuplicateReactionPairTest,
	"MatterFlux.Lua.DuplicateUnorderedReactionPairIsTransactional",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaDuplicateReactionPairTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	const FMatterFluxContentRegistryPtr Baseline =
		Runtime.GetActiveRegistry();
	const FString Source = TEXT(R"LUA(
content.set_manifest("duplicate.reaction.pair", 1, 1)
content.register_material("water", 1.0, 0.0, 0.1, 0.2, 0.8, 1.0)
content.register_material("lava", 2.8, 0.0, 1.0, 0.2, 0.0, 1.0)
content.register_reaction("first", "water", "lava", "water", "lava", 500)
content.register_reaction("second", "lava", "water", "lava", "water", 500)
)LUA");

	FString Error;
	TestFalse(
		TEXT("Reversed duplicate input pair is rejected"),
		Runtime.LoadContentPackFromSource(
			Source,
			TEXT("DuplicateReactionPair"),
			Error));
	TestTrue(
		TEXT("Duplicate pair error identifies the ambiguity"),
		Error.Contains(TEXT("unordered input pair")));
	TestTrue(
		TEXT("Rejected reaction pair preserves active content"),
		Runtime.GetActiveRegistry() == Baseline);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaBoundedDecoratorCountTest,
	"MatterFlux.Lua.DecoratorCountIsBoundedBeforeGeneration",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaBoundedDecoratorCountTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	const FMatterFluxContentRegistryPtr Baseline =
		Runtime.GetActiveRegistry();
	const FString Source = TEXT(R"LUA(
content.set_manifest("oversized.decorator", 1, 1)
content.register_material("grass", 1.0, 0.0, 0.1, 0.8, 0.1, 1.0)
content.register_decorator("forest.grass", "surface_scatter", "grass", 1.0, 0, 2147483647, false)
)LUA");

	FString Error;
	TestFalse(
		TEXT("Unbounded decorator generation is rejected"),
		Runtime.LoadContentPackFromSource(
			Source,
			TEXT("BoundedDecoratorCount"),
			Error));
	TestTrue(
		TEXT("Decorator count error reports the supported range"),
		Error.Contains(TEXT("between 0 and 4096")));
	TestTrue(
		TEXT("Rejected decorator count preserves active content"),
		Runtime.GetActiveRegistry() == Baseline);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaEmbeddedNullSourceTest,
	"MatterFlux.Lua.EmbeddedNullSourceIsRejectedTransactionally",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaEmbeddedNullSourceTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	const FMatterFluxContentRegistryPtr Baseline =
		Runtime.GetActiveRegistry();
	const TCHAR SourceWithEmbeddedNull[] =
		TEXT(
			"content.set_manifest(\"null.source\", 1, 1)\n"
			"\0"
			"error(\"this suffix must not be hidden\")");
	const FString Source(
		UE_ARRAY_COUNT(SourceWithEmbeddedNull) - 1,
		SourceWithEmbeddedNull);
	TestTrue(
		TEXT("Test source really contains bytes after an embedded null"),
		Source.Len() > FCString::Strlen(*Source));

	FString Error;
	TestFalse(
		TEXT("Source text cannot hide bytes after a null"),
		Runtime.LoadContentPackFromSource(
			Source,
			TEXT("EmbeddedNullSource"),
			Error));
	TestTrue(
		TEXT("Embedded-null error is explicit"),
		Error.Contains(TEXT("embedded null")));
	TestTrue(
		TEXT("Rejected source preserves active content"),
		Runtime.GetActiveRegistry() == Baseline);

	const TCHAR OriginWithEmbeddedNull[] =
		TEXT("VisibleOrigin\0HiddenOrigin");
	const FString InvalidOrigin(
		UE_ARRAY_COUNT(OriginWithEmbeddedNull) - 1,
		OriginWithEmbeddedNull);
	Error.Reset();
	TestFalse(
		TEXT("Source origins cannot contain hidden suffixes"),
		Runtime.LoadContentPackFromSource(
			TEXT("content.set_manifest(\"null.origin\", 1, 1)"),
			InvalidOrigin,
			Error));
	TestTrue(
		TEXT("Embedded-null origin error is explicit"),
		Error.Contains(TEXT("origin contains an embedded null")));
	TestTrue(
		TEXT("Rejected origin preserves active content"),
		Runtime.GetActiveRegistry() == Baseline);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaContentStringLimitTest,
	"MatterFlux.Lua.ContentStringsAreBoundedBeforeConversion",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaContentStringLimitTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	const FMatterFluxContentRegistryPtr Baseline =
		Runtime.GetActiveRegistry();
	FString Source = TEXT("content.set_manifest(\"");
	Source += FString::ChrN(300, TEXT('a'));
	Source += TEXT("\", 1, 1)");

	FString Error;
	TestFalse(
		TEXT("Oversized Lua strings are rejected"),
		Runtime.LoadContentPackFromSource(
			Source,
			TEXT("OversizedContentString"),
			Error));
	TestTrue(
		TEXT("String limit rejection is explicit"),
		Error.Contains(TEXT("string length limit")));
	TestTrue(
		TEXT("Rejected string preserves active content"),
		Runtime.GetActiveRegistry() == Baseline);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaSandboxTest,
	"MatterFlux.Lua.SandboxExcludesUnsafeLibraries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaSandboxTest::RunTest(const FString& Parameters)
{
	const FString Source = TEXT(R"LUA(
assert(os == nil)
assert(io == nil)
assert(package == nil)
assert(debug == nil)
assert(dofile == nil)
assert(loadfile == nil)
assert(load == nil)
assert(collectgarbage == nil)
assert(math.random == nil)
assert(math.randomseed == nil)
content.set_manifest("sandbox.pack", 1, 1)
)LUA");

	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	TestTrue(
		FString::Printf(TEXT("Sandbox assertions pass: %s"), *Error),
		Runtime.LoadContentPackFromSource(
			Source,
			TEXT("SandboxExcludesUnsafeLibraries"),
			Error));
	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaInstructionBudgetTest,
	"MatterFlux.Lua.InstructionBudgetStopsInfiniteScript",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaInstructionBudgetTest::RunTest(const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	const FMatterFluxContentRegistryPtr Baseline = Runtime.GetActiveRegistry();
	FString Error;
	const bool bLoaded = Runtime.LoadContentPackFromSource(
		TEXT("while true do end"),
		TEXT("InstructionBudgetStopsInfiniteScript"),
		Error);
	TestFalse(TEXT("Infinite script is rejected"), bLoaded);
	TestTrue(TEXT("Instruction budget error is reported"), Error.Contains(TEXT("instruction budget")));
	TestTrue(
		TEXT("Infinite script cannot replace active content"),
		Runtime.GetActiveRegistry() == Baseline);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaGameStateTest,
	"MatterFlux.Lua.GameModeUsesReplicatedContentGameState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaGameStateTest::RunTest(const FString& Parameters)
{
	const AMatterFluxGameMode* GameMode =
		GetDefault<AMatterFluxGameMode>();
	TestNotNull(TEXT("Game mode default object"), GameMode);
	TestTrue(
		TEXT("Game mode selects the content-aware GameState"),
		GameMode->GameStateClass == AMatterFluxGameState::StaticClass());

	const AMatterFluxGameState* GameState =
		GetDefault<AMatterFluxGameState>();
	TestNotNull(TEXT("Game state default object"), GameState);
	TestTrue(TEXT("Content GameState replicates"), GameState->GetIsReplicated());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaPlayableSceneTest,
	"MatterFlux.Lua.PlayableSceneConsumesActiveDefinitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaPlayableSceneTest::RunTest(const FString& Parameters)
{
	const FString Source = TEXT(R"LUA(
content.set_manifest("scene.pack", 2, 1)
content.register_material("custom_grass", 0.3, 0.2, 0.8, 0.1, 0.4, 1.0)
content.register_decorator("forest.grass", "surface_scatter", "custom_grass", 1.0, 12, 12)
)LUA");

	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Scene content loads"),
		Runtime.LoadContentPackFromSource(Source, TEXT("SceneConsumesDefinitions"), Error)))
	{
		MatterFluxLuaTests::RestoreDefault(Runtime);
		return false;
	}

	MatterFlux::PlayableLevel::FLevelLayout Layout;
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	TestTrue(
		TEXT("Layout builds with the content registry"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			1337,
			Layout,
			Registry.Get()));
	TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*> GrassClusters;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& SourceLayout
		: Layout.FragmentSources)
	{
		if (SourceLayout.Name == TEXT("GrassCluster"))
		{
			GrassClusters.Add(&SourceLayout);
		}
	}
	if (TestEqual(TEXT("Lua fixes the generated grass count to three five-blade clusters"),
		GrassClusters.Num(),
		3))
	{
		TestEqual(
			TEXT("Lua supplies the grass source material color"),
			GrassClusters[0]->Color,
			FLinearColor(0.8f, 0.1f, 0.4f, 1.0f));
		TestEqual(
			TEXT("Lua material identity reaches the generated mask source"),
			GrassClusters[0]->MaterialId,
			FName(TEXT("custom_grass")));
		TestFalse(
			TEXT("Legacy six-argument decorators default collision off"),
			GrassClusters[0]->bEnableCollision);
	}

	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}
