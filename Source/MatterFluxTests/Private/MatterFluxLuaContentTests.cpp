#include "Game/MatterFluxGameMode.h"
#include "Game/MatterFluxGameState.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "IMatterFluxScriptRuntime.h"
#include "Material/MatterFluxCustomMap.h"
#include "Material/MatterFluxCustomMapPour.h"
#include "Material/MatterFluxMaterialSimulationRuntime.h"
#include "Material/MatterFluxMaterialWorld.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace MatterFluxLuaTests
{
	const TCHAR* ValidSource = TEXT(R"LUA(
content.set_manifest("test.pack", 7, 2)
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
	TestEqual(TEXT("All spell modules loaded"), First->Spells.Num(), 21);
	TestEqual(TEXT("All wand modules loaded"), First->Wands.Num(), 9);
	TestEqual(TEXT("All item modules loaded"), First->Items.Num(), 2);
	TestEqual(TEXT("All quest modules loaded"), First->Quests.Num(), 10);
	TestEqual(TEXT("All structure modules loaded"), First->Structures.Num(), 1);
	TestEqual(TEXT("All custom map modules loaded"), First->CustomMaps.Num(), 3);
	TestEqual(TEXT("Modular content revision"), First->Manifest.Revision, 23);
	const FMatterFluxMaterialDefinition* Fire = First->Materials.Find(TEXT("fire"));
	if (TestNotNull(TEXT("Default fire material compiled"), Fire))
	{
		TestTrue(TEXT("Fire remains a generic gas material"),
			Fire->Phase == EMatterFluxMaterialPhase::Gas);
		TestEqual(TEXT("Named lifetime_steps field is compiled"),
			static_cast<int32>(Fire->LifetimeSteps), 6);
	}
	const FMatterFluxMaterialDefinition* Water = First->Materials.Find(TEXT("water"));
	const FMatterFluxMaterialDefinition* Sand = First->Materials.Find(TEXT("sand"));
	const FMatterFluxMaterialDefinition* Lava = First->Materials.Find(TEXT("lava"));
	if (TestNotNull(TEXT("Default water material compiled"), Water)
		&& TestNotNull(TEXT("Default sand material compiled"), Sand)
		&& TestNotNull(TEXT("Default lava material compiled"), Lava))
	{
		TestTrue(TEXT("Powder resists movement more than water"),
			Sand->MovementResistance > Water->MovementResistance);
		TestTrue(TEXT("Viscous lava resists movement more than sand"),
			Lava->MovementResistance > Sand->MovementResistance);
	}
	const FMatterFluxStructureDefinition* House = First->Structures.Find(
		TEXT("structure.house.two_storey"));
	if (TestNotNull(TEXT("Two-storey house profile compiled"), House))
	{
		TestEqual(TEXT("Lua selects the bounded house generator"),
			House->GeneratorId, FName(TEXT("two_storey_house")));
		TestEqual(TEXT("Lua configures connected-wall tolerance"),
			House->ContactToleranceCentimeters, 12.0f);
		TestEqual(TEXT("Lua keeps furniture out of wall opacity policy"),
			House->WallGhostOpacity, 0.055f);
	}
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

	const FString ChemistryPath = FPaths::Combine(
		LuaRoot, TEXT("World"), TEXT("Chemistry.lua"));
	FString ChemistrySource;
	TestTrue(
		TEXT("Chemistry module is readable"),
		FFileHelper::LoadFileToString(
			ChemistrySource, *ChemistryPath));
	TestTrue(
		TEXT("Chemistry uses the unified reaction DSL"),
		ChemistrySource.Contains(TEXT("reaction.define")));
	TestFalse(
		TEXT("Chemistry has no separate reaction registration path"),
		ChemistrySource.Contains(TEXT("register_reaction")));
	TestFalse(
		TEXT("Chemistry cannot depend on the raw reaction compiler table"),
		ChemistrySource.Contains(TEXT("content.register_reaction")));

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
content.set_manifest("progression.test", 1, 2)
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
content.set_manifest("progression.cycle", 1, 2)
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
content.set_manifest("invalid.spell.capability", 1, 2)
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

	const FString ImpactOnlyAttempt = EngineSource + TEXT(R"LUA(
content.set_manifest("invalid.impact.only", 1, 2)
material.define({
    id = "fire", density = 0.01, hardness = 0,
    color_r = 1, color_g = 0.2, color_b = 0.02
})
spell.define({
    id = "invalid.impact_only", name = "Impact Only", mana_cost = 1
}, function(api)
    api.projectile({
        speed = 100, lifetime = 1, radius = 4,
        impact_material = "fire"
    })
end)
)LUA");
	Error.Reset();
	TestFalse(
		TEXT("Direct impact-state mutation is not a spell capability"),
		Runtime.LoadContentPackFromSource(
			ImpactOnlyAttempt,
			TEXT("ImpactOnlyMaterialSpell"),
			Error));
	TestFalse(TEXT("Impact-only rejection returns a diagnostic"),
		Error.IsEmpty());
	TestTrue(TEXT("Rejected impact-only spell preserves the active registry"),
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
		TestEqual(TEXT("Schema"), Registry->Manifest.SchemaVersion, 2);
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
content.set_manifest("fragmentation.pack", 1, 2)
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
content.set_manifest("invalid.fragmentation", 1, 2)
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
content.set_manifest("simulation.pack", 1, 2)
content.register_material("water", 1.0, 0.1, 0.1, 0.4, 0.9, 0.8, "liquid", 255, 220)
content.register_material("lava", 2.8, 0.2, 1.0, 0.2, 0.0, 1.0, "liquid", 96, 32)
content.register_material("steam", 0.1, 0.0, 0.8, 0.8, 0.8, 0.5, "gas", 255, 255)
content.register_material("stone", 3.0, 1.0, 0.3, 0.3, 0.3, 1.0, "static", 0, 0)
content.register_material("wood", 0.8, 0.7, 0.3, 0.2, 0.1, 1.0, "static", 0, 0)
content.register_material("fire", 0.01, 0.0, 1.0, 0.3, 0.0, 0.9, "gas", 255, 255)
content.register_material("smoke", 0.05, 0.0, 0.2, 0.2, 0.2, 0.6, "gas", 255, 220)
content.register_material("charcoal", 0.7, 0.4, 0.08, 0.07, 0.06, 1.0, "static", 0, 0)
content.register_reaction("water_lava", "water", "lava", "steam", "stone", 1000)
content.register_reaction({
    id = "wood_burn",
    kind = "propagating",
    input_a = "wood",
    input_b = "fire",
    output_a = "charcoal",
    output_b = "fire",
    chance_permille = 1000,
    propagation_permille = 650,
    duration_steps = 12,
    emission_material = "smoke",
    emission_permille = 700,
})
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
		const FMatterFluxReactionDefinition& PropagatingReaction =
			Registry->Reactions.FindChecked(TEXT("wood_burn"));
		TestTrue(TEXT("Lua compiles fire as a generic propagating reaction"),
			PropagatingReaction.Kind
				== FMatterFluxReactionDefinition::EKind::Propagating);
		TestEqual(TEXT("Lua propagating input"),
			PropagatingReaction.InputA, FName(TEXT("wood")));
		TestEqual(TEXT("Lua propagating output"),
			PropagatingReaction.OutputA, FName(TEXT("charcoal")));
		TestEqual(TEXT("Lua reaction duration"),
			PropagatingReaction.DurationSteps, 12);
		TestEqual(TEXT("Lua reaction emission probability"),
			PropagatingReaction.EmissionChancePermille, 700);
	}

	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaCustomMapDefinitionTest,
	"MatterFlux.Lua.CustomMapsCompileFromCapabilityDsl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaCustomMapDefinitionTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content with custom maps loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Custom-map registry exists"), Registry.IsValid()))
	{
		return false;
	}
	const FMatterFluxCustomMapDefinition* Map = Registry->CustomMaps.Find(
		TEXT("test.liquid_density_drops"));
	if (!TestNotNull(TEXT("Liquid density test map is Lua-authored"), Map))
	{
		return false;
	}
	TestEqual(TEXT("Map owns both liquid test pools"), Map->Stamps.Num(), 4);
	TestEqual(TEXT("Map exposes player and scenario markers"), Map->Markers.Num(), 3);
	TestEqual(TEXT("Map owns its horizontal arena fixtures"), Map->SceneBoxes.Num(), 5);
	TestEqual(TEXT("Map owns its 3D inspection camera"), Map->Cameras.Num(), 1);
	TestTrue(TEXT("Map bounds contain its authored chambers"),
		Map->MinimumCell == FIntPoint(-20, -16)
			&& Map->MaximumCellExclusive == FIntPoint(21, 17));

	const FString MapPath = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("Lua"),
		TEXT("Maps"),
		TEXT("LiquidDensityDrops.lua"));
	FString MapSource;
	TestTrue(TEXT("Custom map module is readable"),
		FFileHelper::LoadFileToString(MapSource, *MapPath));
	TestTrue(TEXT("Custom map uses the public capability DSL"),
		MapSource.Contains(TEXT("map.define"))
			&& MapSource.Contains(TEXT("fill_circle")));
	TestFalse(TEXT("Custom map cannot call the raw compiler interface"),
		MapSource.Contains(TEXT("content.register_custom_map")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaStoryMapTest,
	"MatterFlux.Lua.StoryMapPlacesReferenceQuestActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaStoryMapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content with story map loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const FMatterFluxCustomMapDefinition* Map = Registry.IsValid()
		? Registry->CustomMaps.Find(TEXT("story.paper_magic"))
		: nullptr;
	if (!TestNotNull(TEXT("PaperMagic story map is registered"), Map))
	{
		return false;
	}
	TSet<FName> MarkerIds;
	for (const FMatterFluxCustomMapMarkerDefinition& Marker : Map->Markers)
	{
		MarkerIds.Add(Marker.Id);
	}
	TestTrue(TEXT("Story map places the player"),
		MarkerIds.Contains(TEXT("player_start")));
	TestTrue(TEXT("Story map places the camp merchant"),
		MarkerIds.Contains(TEXT("creature.std.merchant_base.0")));
	TestTrue(TEXT("Story camp places one fixed two-storey house"),
		MarkerIds.Contains(TEXT("structure.house.two_storey.0")));
	TestTrue(TEXT("Story map places both tutorial slimes"),
		MarkerIds.Contains(TEXT("creature.std.slime.0"))
			&& MarkerIds.Contains(TEXT("creature.std.slime.1")));
	TestTrue(TEXT("Story map places the tutorial elite"),
		MarkerIds.Contains(TEXT("creature.std.elite_patrol.0")));
	TestTrue(TEXT("Story map places both final enemies"),
		MarkerIds.Contains(TEXT("creature.std.test_boss.0"))
			&& MarkerIds.Contains(TEXT("creature.std.test_boss.1")));
	const int32 MapWidth = Map->MaximumCellExclusive.X - Map->MinimumCell.X;
	const int32 MapHeight = Map->MaximumCellExclusive.Y - Map->MinimumCell.Y;
	TestTrue(TEXT("Story corridor is compact instead of free-mode scale"),
		MapWidth <= 55 && MapHeight <= 20);

	const FMatterFluxQuestDefinition* EquipWand = Registry->Quests.Find(
		TEXT("std.init_quest.equip_wand"));
	if (TestNotNull(TEXT("Story starts with the wand equipment objective"),
		EquipWand))
	{
		TestEqual(TEXT("Initial wand objective targets the attack slot"),
			EquipWand->EquipmentSlot, 0);
		TestEqual(TEXT("Initial story reward contains one wand and one spell"),
			EquipWand->ActivationRewards.Num(), 2);
		if (EquipWand->ActivationRewards.Num() == 2)
		{
			TestEqual(TEXT("Initial reward grants the default wand"),
				EquipWand->ActivationRewards[0].ContentId,
				FName(TEXT("std.default")));
			TestEqual(TEXT("Initial reward grants one attack spell"),
				EquipWand->ActivationRewards[1].ContentId,
				FName(TEXT("std.default")));
		}
	}
	const FMatterFluxQuestDefinition* EquipSpell = Registry->Quests.Find(
		TEXT("std.init_quest.equip_spell"));
	if (TestNotNull(TEXT("Story has the attack spell objective"), EquipSpell))
	{
		TestEqual(TEXT("Attack spell objective targets the default spell"),
			EquipSpell->TargetId, FName(TEXT("std.default")));
		TestEqual(TEXT("Attack spell objective targets the attack slot"),
			EquipSpell->EquipmentSlot, 0);
		TestTrue(TEXT("Attack spell objective grants no bonus spell bundle"),
			EquipSpell->ActivationRewards.IsEmpty());
	}
	const FMatterFluxQuestDefinition* KillEnemies = Registry->Quests.Find(
		TEXT("std.init_quest.kill_enemy"));
	if (TestNotNull(TEXT("Story has the enclosed arena combat objective"),
		KillEnemies))
	{
		TestEqual(TEXT("Combat objective requires all three enemies"),
			KillEnemies->TargetCount, 3);
		TestEqual(TEXT("Combat completion grants shoe and jump"),
			KillEnemies->CompletionRewards.Num(), 2);
		if (KillEnemies->CompletionRewards.Num() == 2)
		{
			TestEqual(TEXT("Shoe reward is bound to the leg slot"),
				KillEnemies->CompletionRewards[0].EquipmentSlot, 4);
			TestEqual(TEXT("Jump reward is explicit"),
				KillEnemies->CompletionRewards[1].ContentId,
				FName(TEXT("std.jump")));
		}
	}

	MatterFlux::Material::FSimulationRuntime PlayableRuntime;
	MatterFlux::Material::FCustomMapScene Scene;
	TestTrue(TEXT("Story map is adopted by the playable simulation runtime"),
		MatterFlux::Material::BuildPlayableCustomMap(
			TEXT("story.paper_magic"),
			*Registry,
			8403,
			0.05f,
			4,
			PlayableRuntime,
			Scene,
			Error));
	TestTrue(TEXT("Playable story runtime is initialized"),
		PlayableRuntime.IsInitialized());
	TestTrue(TEXT("Playable scene retains the merchant marker"),
		Scene.MarkerLocations.Contains(
			TEXT("creature.std.merchant_base.0")));
	TestTrue(TEXT("Playable scene retains the fixed camp house marker"),
		Scene.MarkerLocations.Contains(
			TEXT("structure.house.two_storey.0")));
	const FVector* PlayerMarker = Scene.MarkerLocations.Find(
		TEXT("player_start"));
	const FVector* LeftBarrierMarker = Scene.MarkerLocations.Find(
		TEXT("story.left_barrier"));
	const FVector* RightBarrierMarker = Scene.MarkerLocations.Find(
		TEXT("story.right_barrier"));
	if (TestNotNull(TEXT("Story exposes the left region boundary"),
		LeftBarrierMarker)
		&& TestNotNull(TEXT("Story exposes the right region boundary"),
			RightBarrierMarker)
		&& TestNotNull(TEXT("Story retains the player location"), PlayerMarker))
	{
		TestTrue(TEXT("Training region is compact"),
			LeftBarrierMarker->X - PlayerMarker->X <= 900.0f);
		TestTrue(TEXT("Camp region is smaller than the boss arena"),
			RightBarrierMarker->X - LeftBarrierMarker->X <= 1800.0f
				&& Scene.MaximumCellExclusive.X * Scene.CellSizeCentimeters
					- RightBarrierMarker->X >= 2000.0f);
		for (const FName EnemyMarker : {
			FName(TEXT("creature.std.slime.0")),
			FName(TEXT("creature.std.slime.1")),
			FName(TEXT("creature.std.elite_patrol.0")) })
		{
			const FVector* Enemy = Scene.MarkerLocations.Find(EnemyMarker);
			TestTrue(TEXT("Every tutorial enemy is inside the initial region"),
				Enemy && Enemy->X > PlayerMarker->X
					&& Enemy->X < LeftBarrierMarker->X);
		}
	}
	MatterFlux::Material::FChunkedMaterialWorld StoryWorld;
	MatterFlux::Material::FCustomMapScene StoryWorldScene;
	if (TestTrue(TEXT("Story material world compiles for terrain assertions"),
		MatterFlux::Material::BuildCustomMap(
			TEXT("story.paper_magic"),
			*Registry,
			8403,
			StoryWorld,
			StoryWorldScene,
			Error)))
	{
		TestEqual(TEXT("Story compiler retains its bounded grassland fallback"),
			StoryWorld.GetMaterialAt(FIntPoint(-2, 0)),
			FName(TEXT("grassland")));
		TestEqual(TEXT("Story map no longer authors a fixed river cell"),
			StoryWorld.GetMaterialAt(FIntPoint(31, 0)),
			FName(TEXT("grassland")));
	}
	MatterFlux::PlayableLevel::FLevelLayout FirstGeneratedStory;
	MatterFlux::PlayableLevel::FLevelLayout SecondGeneratedStory;
	if (TestTrue(TEXT("Story seed generates its natural environment"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			8403, FirstGeneratedStory, Registry.Get()))
		&& TestTrue(TEXT("The same story seed regenerates successfully"),
			MatterFlux::PlayableLevel::BuildLevelLayout(
				8403, SecondGeneratedStory, Registry.Get())))
	{
		TestTrue(TEXT("Generated story terrain is valid"),
			FirstGeneratedStory.Terrain.IsValid());
		TestNotNull(TEXT("Generated story contains a river"),
			FirstGeneratedStory.FindLayer(TEXT("Stream")));
		TestTrue(TEXT("Generated story contains trees, flowers and grass"),
			!FirstGeneratedStory.FragmentSources.IsEmpty());
		TestTrue(TEXT("Fixed story seed reproduces terrain heights"),
			FirstGeneratedStory.Terrain.Heights
				== SecondGeneratedStory.Terrain.Heights);
		TestEqual(TEXT("Fixed story seed reproduces decoration count"),
			FirstGeneratedStory.FragmentSources.Num(),
			SecondGeneratedStory.FragmentSources.Num());
	}
	int32 LeftRockCount = 0;
	int32 RightRockCount = 0;
	bool bAllBarrierRocksAreJumpable = true;
	for (const MatterFlux::Material::FCustomMapSceneBox& Box : Scene.Boxes)
	{
		const FString BoxId = Box.Id.ToString();
		const bool bLeftRock = BoxId.StartsWith(TEXT("story.left_rocks."));
		const bool bRightRock = BoxId.StartsWith(TEXT("story.right_rocks."));
		LeftRockCount += bLeftRock ? 1 : 0;
		RightRockCount += bRightRock ? 1 : 0;
		if (bLeftRock || bRightRock)
		{
			bAllBarrierRocksAreJumpable &= Box.bCollision
				&& Box.Size.Z >= 70.0f
				&& Box.Size.Z <= 80.0f;
		}
	}
	TestTrue(TEXT("Training arena has a complete jumpable rock row"),
		LeftRockCount >= 8);
	TestTrue(TEXT("Camp right edge has a complete rock row"),
		RightRockCount >= 8);
	TestTrue(TEXT("Both rock rows are collidable jump-height barriers"),
		bAllBarrierRocksAreJumpable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaStackedContainerPourMapTest,
	"MatterFlux.Lua.StackedContainerPourMapIsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaStackedContainerPourMapTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content with custom maps loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	return TestTrue(TEXT("Stacked water and acid pour map is Lua-authored"),
		Registry.IsValid()
			&& Registry->CustomMaps.Contains(
				TEXT("test.stacked_container_pour")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaStackedContainerPourBehaviorTest,
	"MatterFlux.Lua.StackedContainerPourIsSynchronizedAndDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaStackedContainerPourBehaviorTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content for pour behavior loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Pour behavior registry exists"), Registry.IsValid()))
	{
		return false;
	}

	MatterFlux::Material::FChunkedMaterialWorld World;
	MatterFlux::Material::FCustomMapScene Scene;
	if (!TestTrue(TEXT("Stacked-container gameplay map builds"),
		MatterFlux::Material::BuildCustomMap(
			TEXT("test.stacked_container_pour"),
			*Registry,
			8403,
			World,
			Scene,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Map contains two authored pour containers"),
		Scene.PourContainers.Num(), 2);
	if (Scene.PourContainers.Num() != 2)
	{
		return false;
	}
	const auto FindContainer = [&Scene](const FName Id)
	{
		return Scene.PourContainers.FindByPredicate(
			[Id](const MatterFlux::Material::FCustomMapPourContainer& Container)
			{
				return Container.Id == Id;
			});
	};
	const MatterFlux::Material::FCustomMapPourContainer* Water =
		FindContainer(TEXT("water.lower"));
	const MatterFlux::Material::FCustomMapPourContainer* Acid =
		FindContainer(TEXT("acid.upper"));
	if (!TestNotNull(TEXT("Lower water container exists"), Water)
		|| !TestNotNull(TEXT("Upper acid container exists"), Acid))
	{
		return false;
	}
	TestTrue(TEXT("Containers share X/Y and acid starts above water"),
		FMath::IsNearlyEqual(Water->Center.X, Acid->Center.X)
			&& FMath::IsNearlyEqual(Water->Center.Y, Acid->Center.Y)
			&& Acid->Center.Z > Water->Center.Z);
	TestTrue(TEXT("Both containers use one synchronized motion profile"),
		Water->StartStep == Acid->StartStep
			&& Water->TiltDurationSteps == Acid->TiltDurationSteps
			&& FMath::IsNearlyEqual(Water->TiltDegrees, Acid->TiltDegrees)
			&& Water->PourCellsPerStep == Acid->PourCellsPerStep);

	MatterFlux::Material::FCustomMapPourSimulation First;
	MatterFlux::Material::FCustomMapPourSimulation Second;
	if (!TestTrue(TEXT("First pour simulation initializes"),
		First.Initialize(Scene, *Registry, 8403, Error))
		|| !TestTrue(TEXT("Second pour simulation initializes"),
			Second.Initialize(Scene, *Registry, 8403, Error)))
	{
		AddError(Error);
		return false;
	}
	MatterFlux::Material::FCustomMapPourSnapshot Initial;
	First.GetSnapshot(Initial);
	TestEqual(TEXT("Both full interiors produce 350 liquid voxels"),
		Initial.HeldVoxels.Num(), 350);
	TestEqual(TEXT("No liquid falls before the tilt starts"),
		Initial.FallingVoxels.Num(), 0);

	for (int32 Step = 0; Step < 32; ++Step)
	{
		First.Step();
		Second.Step();
	}
	MatterFlux::Material::FCustomMapPourSnapshot Mid;
	First.GetSnapshot(Mid);
	TestEqual(TEXT("Both snapshots expose two synchronized containers"),
		Mid.Containers.Num(), 2);
	if (Mid.Containers.Num() == 2)
	{
		TestTrue(TEXT("Water and acid containers have the same live tilt"),
			FMath::IsNearlyEqual(
				Mid.Containers[0].TiltDegrees,
				Mid.Containers[1].TiltDegrees));
	}
	TestTrue(TEXT("Both liquids have begun pouring downward"),
		Mid.FallingVoxels.ContainsByPredicate([](const auto& Voxel)
		{
			return Voxel.MaterialId == TEXT("water");
		}) && Mid.FallingVoxels.ContainsByPredicate([](const auto& Voxel)
		{
			return Voxel.MaterialId == TEXT("acid");
		}));

	for (int32 Step = 32; Step < 180; ++Step)
	{
		First.Step();
		Second.Step();
	}
	MatterFlux::Material::FCustomMapPourSnapshot FinalA;
	MatterFlux::Material::FCustomMapPourSnapshot FinalB;
	First.GetSnapshot(FinalA);
	Second.GetSnapshot(FinalB);
	const auto CountMaterial = [](const auto& Voxels, const FName MaterialId)
	{
		int32 Count = 0;
		for (const auto& Voxel : Voxels)
		{
			Count += Voxel.MaterialId == MaterialId ? 1 : 0;
		}
		return Count;
	};
	for (const FName MaterialId : { FName(TEXT("water")), FName(TEXT("acid")) })
	{
		const int32 Conserved = CountMaterial(FinalA.HeldVoxels, MaterialId)
			+ CountMaterial(FinalA.FallingVoxels, MaterialId)
			+ CountMaterial(FinalA.SettledVoxels, MaterialId);
		TestEqual(*FString::Printf(TEXT("%s voxel count is conserved"),
			*MaterialId.ToString()), Conserved, 175);
	}
	TestEqual(TEXT("All poured liquid eventually settles"),
		FinalA.SettledVoxels.Num(), 350);
	TMap<FIntPoint, int32> SettledColumnHeights;
	for (const auto& Voxel : FinalA.SettledVoxels)
	{
		const FIntPoint Column(
			FMath::RoundToInt(Voxel.Position.X * 100.0f),
			FMath::RoundToInt(Voxel.Position.Y * 100.0f));
		++SettledColumnHeights.FindOrAdd(Column);
	}
	int32 MaximumColumnHeight = 0;
	for (const TPair<FIntPoint, int32>& Pair : SettledColumnHeights)
	{
		MaximumColumnHeight = FMath::Max(MaximumColumnHeight, Pair.Value);
	}
	TestTrue(TEXT("Settled liquid spreads into a puddle instead of a tall tower"),
		SettledColumnHeights.Num() >= 100 && MaximumColumnHeight <= 4);
	TestEqual(TEXT("Same seed and steps keep falling count deterministic"),
		FinalA.FallingVoxels.Num(), FinalB.FallingVoxels.Num());
	TestEqual(TEXT("Same seed and steps keep settled count deterministic"),
		FinalA.SettledVoxels.Num(), FinalB.SettledVoxels.Num());
	bool bSnapshotsMatch = FinalA.SettledVoxels.Num()
		== FinalB.SettledVoxels.Num();
	for (int32 Index = 0; bSnapshotsMatch
		&& Index < FinalA.SettledVoxels.Num(); ++Index)
	{
		bSnapshotsMatch =
			FinalA.SettledVoxels[Index].MaterialId
				== FinalB.SettledVoxels[Index].MaterialId
			&& FinalA.SettledVoxels[Index].Position.Equals(
				FinalB.SettledVoxels[Index].Position);
	}
	TestTrue(TEXT("Same seed produces identical settled voxel fields"),
		bSnapshotsMatch);

	bool bFoundDensityLayer = false;
	for (const auto& AcidVoxel : FinalA.SettledVoxels)
	{
		if (AcidVoxel.MaterialId != TEXT("acid"))
		{
			continue;
		}
		bFoundDensityLayer |= FinalA.SettledVoxels.ContainsByPredicate(
			[&AcidVoxel](const auto& WaterVoxel)
			{
				return WaterVoxel.MaterialId == TEXT("water")
					&& FMath::IsNearlyEqual(
						WaterVoxel.Position.X, AcidVoxel.Position.X)
					&& FMath::IsNearlyEqual(
						WaterVoxel.Position.Y, AcidVoxel.Position.Y)
					&& WaterVoxel.Position.Z > AcidVoxel.Position.Z;
			});
		if (bFoundDensityLayer)
		{
			break;
		}
	}
	TestTrue(TEXT("Denser acid settles below water in shared columns"),
		bFoundDensityLayer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaCustomMapBoundsTest,
	"MatterFlux.Lua.CustomMapRejectsUnsafeCoordinatesTransactionally",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaCustomMapBoundsTest::RunTest(const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!Runtime.ReloadDefaultContentPack(Error))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Baseline = Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Baseline registry exists"), Baseline.IsValid()))
	{
		return false;
	}

	FString EngineSource;
	const FString EnginePath = FPaths::Combine(
		FPaths::ProjectContentDir(), TEXT("Lua"), TEXT("MatterFluxEngine.lua"));
	if (!TestTrue(TEXT("Map DSL source is readable"),
		FFileHelper::LoadFileToString(EngineSource, *EnginePath)))
	{
		return false;
	}
	const FString InvalidSource = EngineSource + TEXT(R"LUA(
content.set_manifest("unsafe.map", 1, 2)
material.define({
    id = "stone", density = 2.0, hardness = 1.0,
    color_r = 0.3, color_g = 0.3, color_b = 0.3, color_a = 1.0
})
map.define({
    id = "test.unsafe_origin", name = "Unsafe",
    min_x = 1000001, min_y = 0,
    max_x_exclusive = 1000005, max_y_exclusive = 4,
}, function(m)
    m.fill_rectangle("stone", 1000001, 0, 1000004, 3)
end)
)LUA");
	TestFalse(TEXT("Unsafe custom-map coordinates are rejected"),
		Runtime.LoadContentPackFromSource(
			InvalidSource, TEXT("UnsafeCustomMap"), Error));
	TestTrue(TEXT("Custom-map rejection reports its invalid bounds"),
		Error.Contains(TEXT("invalid bounds or counts")));
	const FMatterFluxContentRegistryPtr AfterFailure =
		Runtime.GetActiveRegistry();
	TestTrue(TEXT("Rejected map keeps the previous registry active"),
		AfterFailure.IsValid()
			&& AfterFailure->Manifest.VersionHash
				== Baseline->Manifest.VersionHash);
	MatterFluxLuaTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaCustomMapBuildTest,
	"MatterFlux.Lua.CustomMapBuildsPlayableSurfaceFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaCustomMapBuildTest::RunTest(const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!Runtime.ReloadDefaultContentPack(Error))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Custom-map build registry exists"), Registry.IsValid()))
	{
		return false;
	}
	MatterFlux::Material::FChunkedMaterialWorld World;
	MatterFlux::Material::FCustomMapScene Scene;
	if (!TestTrue(TEXT("Shared liquid test map builds"),
		MatterFlux::Material::BuildCustomMap(
			TEXT("test.liquid_density_drops"),
			*Registry,
			8403,
			World,
			Scene,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Horizontal map authors equal visible water footprint"),
		World.CountMaterial(TEXT("water")), 161);
	TestEqual(TEXT("Horizontal map authors equal visible acid footprint"),
		World.CountMaterial(TEXT("acid")), 161);
	TestEqual(TEXT("Terrain fixtures are separate from simulated material cells"),
		World.CountMaterial(TEXT("stone")), 0);
	TestEqual(TEXT("Left arena starts with the smaller water patch"),
		World.GetMaterialAt(FIntPoint(-10, -8)), FName(TEXT("water")));
	TestEqual(TEXT("Right arena starts with the smaller acid patch"),
		World.GetMaterialAt(FIntPoint(10, -8)), FName(TEXT("acid")));
	TestEqual(TEXT("Custom scene uses the authored voxel size"),
		Scene.CellSizeCentimeters, 28.0f);
	TestTrue(TEXT("Surface material is a shallow horizontal layer"),
		Scene.MaterialDepthCells > 0.0f
			&& Scene.MaterialDepthCells < 1.0f);
	TestEqual(TEXT("Custom map authors floor and four arena edges"),
		Scene.Boxes.Num(), 5);
	TestTrue(TEXT("Custom map exposes a playable spawn on the XY plane"),
		Scene.MarkerLocations.Contains(TEXT("player_start"))
			&& FMath::IsNearlyZero(
				Scene.MarkerLocations.FindChecked(TEXT("player_start")).Z));
	TestEqual(TEXT("Custom map authors one perspective inspection camera"),
		Scene.Cameras.Num(), 1);
	if (Scene.Boxes.Num() == 5 && Scene.Cameras.Num() == 1)
	{
		FBox SceneBounds(ForceInit);
		for (const MatterFlux::Material::FCustomMapSceneBox& Box
			: Scene.Boxes)
		{
			SceneBounds += Box.Center - Box.Size * 0.5;
			SceneBounds += Box.Center + Box.Size * 0.5;
		}
		TestTrue(TEXT("Horizontal arena occupies both ground axes and has thickness"),
			SceneBounds.IsValid
				&& SceneBounds.GetSize().X > 20.0
				&& SceneBounds.GetSize().Y > 20.0
				&& SceneBounds.GetSize().Z > 1.0);
		TestTrue(TEXT("At least the floor has gameplay collision"),
			Scene.Boxes.ContainsByPredicate(
				[](const MatterFlux::Material::FCustomMapSceneBox& Box)
				{
					return Box.Id == TEXT("arena.floor") && Box.bCollision;
				}));
		const MatterFlux::Material::FCustomMapSceneCamera& Camera =
			Scene.Cameras[0];
		TestTrue(TEXT("Authored camera is oblique and looks downward"),
			!FMath::IsNearlyEqual(Camera.Location.X, Camera.Target.X)
				&& !FMath::IsNearlyEqual(Camera.Location.Y, Camera.Target.Y)
				&& Camera.Location.Z > Camera.Target.Z
				&& Camera.FieldOfViewDegrees > 20.0f
				&& Camera.FieldOfViewDegrees < 90.0f);
	}

	for (int32 Step = 0; Step < 120; ++Step)
	{
		World.Step();
	}
	TArray<MatterFlux::Material::FCellSnapshot> Cells;
	World.GetActiveCells(Cells);
	bool bAllCellsRemainOnHorizontalSurface = true;
	for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
	{
		bAllCellsRemainOnHorizontalSurface &= Cell.SupportHeight == 0;
	}
	TestTrue(TEXT("Simulation stays on the XY terrain instead of becoming an XZ slice"),
		bAllCellsRemainOnHorizontalSurface);
	TestTrue(TEXT("Water remains present after surface spreading"),
		World.CountMaterial(TEXT("water")) > 0);
	TestTrue(TEXT("Acid remains present after surface spreading"),
		World.CountMaterial(TEXT("acid")) > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaAcidChemistryTest,
	"MatterFlux.Lua.AcidChemistryIsDenseCorrosiveAndWaterInert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaAcidChemistryTest::RunTest(const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default chemistry pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Default chemistry registry exists"), Registry.IsValid()))
	{
		return false;
	}

	const FMatterFluxMaterialDefinition* Water =
		Registry->Materials.Find(TEXT("water"));
	const FMatterFluxMaterialDefinition* Acid =
		Registry->Materials.Find(TEXT("acid"));
	const FMatterFluxMaterialDefinition* AcidGas =
		Registry->Materials.Find(TEXT("acid_gas"));
	if (!TestNotNull(TEXT("Water is registered"), Water)
		|| !TestNotNull(TEXT("Acid is registered"), Acid)
		|| !TestNotNull(TEXT("Acid gas is registered"), AcidGas))
	{
		return false;
	}
	TestTrue(TEXT("Acid is liquid"),
		Acid->Phase == EMatterFluxMaterialPhase::Liquid);
	TestTrue(TEXT("Acid is denser than water"), Acid->Density > Water->Density);
	TestTrue(TEXT("Acid gas uses gas simulation"),
		AcidGas->Phase == EMatterFluxMaterialPhase::Gas);
	TestTrue(TEXT("Acid optics remain a valid opacity ramp"),
		Acid->ShallowOpacity >= 0.0f
			&& Acid->DeepOpacity >= Acid->ShallowOpacity
			&& Acid->DeepOpacity <= 1.0f
			&& Acid->OpacityDepth > 0.0f);
	TestTrue(
		TEXT("Acid remains visibly magenta over bright terrain"),
		Acid->Color.R >= 0.70f
			&& Acid->Color.B >= 0.70f
			&& Acid->Color.G <= 0.25f
			&& Acid->ShallowOpacity >= 0.65f);

	static const FName CorrodibleMaterials[] = {
		TEXT("wood"), TEXT("leaf"), TEXT("grass"), TEXT("grassland"),
		TEXT("flower_pink"), TEXT("flower_gold"), TEXT("flower_blue"),
		TEXT("soil"), TEXT("stone"), TEXT("sand")
	};
	for (const FName Target : CorrodibleMaterials)
	{
		const FMatterFluxReactionDefinition* Found = nullptr;
		for (const TPair<FName, FMatterFluxReactionDefinition>& Pair
			: Registry->Reactions)
		{
			const FMatterFluxReactionDefinition& Rule = Pair.Value;
			if (Rule.Kind == FMatterFluxReactionDefinition::EKind::Contact
				&& Rule.InputA == TEXT("acid")
				&& Rule.InputB == Target)
			{
				Found = &Rule;
				break;
			}
		}
		if (TestNotNull(
			*FString::Printf(TEXT("Acid corrosion rule exists for %s"),
				*Target.ToString()),
			Found))
		{
			TestEqual(TEXT("Corrosion consumes its acid input"),
				Found->OutputA, FName(TEXT("empty")));
			TestEqual(TEXT("Corrosion creates no acid-family product"),
				Found->OutputB, FName(TEXT("empty")));
			TestTrue(TEXT("Contact corrosion has no additional emission"),
				Found->EmissionMaterial.IsNone()
					|| Found->EmissionMaterial == TEXT("empty"));
		}
	}

	bool bHasAcidWaterReaction = false;
	bool bHasPropagatingAcidReaction = false;
	for (const TPair<FName, FMatterFluxReactionDefinition>& Pair
		: Registry->Reactions)
	{
		const FMatterFluxReactionDefinition& Rule = Pair.Value;
		bHasAcidWaterReaction |=
			(Rule.InputA == TEXT("acid") && Rule.InputB == TEXT("water"))
			|| (Rule.InputA == TEXT("water") && Rule.InputB == TEXT("acid"));
		bHasPropagatingAcidReaction |=
			Rule.Kind == FMatterFluxReactionDefinition::EKind::Propagating
			&& (Rule.InputA == TEXT("acid") || Rule.InputB == TEXT("acid"));
	}
	TestFalse(TEXT("Acid and water have no chemical reaction rule"),
		bHasAcidWaterReaction);
	TestFalse(TEXT("Acid corrosion never self-propagates through solids"),
		bHasPropagatingAcidReaction);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxLuaLiquidOpticsRegistrationTest,
	"MatterFlux.Lua.LiquidOpticsUseNamedConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLuaLiquidOpticsRegistrationTest::RunTest(
	const FString& Parameters)
{
	const FString Source = TEXT(R"LUA(
content.set_manifest("liquid.optics.pack", 1, 2)
content.register_material({
    id = "water",
    density = 1.0,
    hardness = 0.05,
    color_r = 0.12,
    color_g = 0.46,
    color_b = 0.78,
    color_a = 0.82,
    phase = "liquid",
    mobility = 255,
    dispersion = 220,
	shallow_opacity = 0.16,
	deep_opacity = 0.88,
	opacity_depth = 160.0,
})
)LUA");

	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Named liquid optics content loads"),
		Runtime.LoadContentPackFromSource(
			Source,
			TEXT("LiquidOpticsRegistration"),
			Error)))
	{
		AddError(Error);
		MatterFluxLuaTests::RestoreDefault(Runtime);
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (TestTrue(TEXT("Liquid optics registry exists"), Registry.IsValid()))
	{
		const FMatterFluxMaterialDefinition& Water =
			Registry->Materials.FindChecked(TEXT("water"));
		TestEqual(TEXT("Shallow-water opacity is retained"),
			Water.ShallowOpacity, 0.16f);
		TestEqual(TEXT("Deep-water opacity is retained"),
			Water.DeepOpacity, 0.88f);
		TestEqual(TEXT("Opacity depth is retained in centimeters"),
			Water.OpacityDepth, 160.0f);
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
content.set_manifest("invalid.collision", 1, 2)
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
content.set_manifest("broken.pack", 1, 2)
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
content.set_manifest("duplicate.reaction.pair", 1, 2)
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
content.set_manifest("oversized.decorator", 1, 2)
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
			"content.set_manifest(\"null.source\", 1, 2)\n"
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
			TEXT("content.set_manifest(\"null.origin\", 1, 2)"),
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
content.set_manifest("sandbox.pack", 1, 2)
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
content.set_manifest("scene.pack", 2, 2)
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
