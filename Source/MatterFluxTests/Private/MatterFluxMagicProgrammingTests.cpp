#include "IMatterFluxScriptRuntime.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentGeometry.h"
#include "GAS/GA_CastWand.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Magic/MatterFluxWandProgram.h"
#include "Magic/MatterFluxSpellProgramLayout.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Magic/MatterFluxMagicIconResolver.h"
#include "Magic/MatterFluxMagicWorkbenchInteraction.h"
#include "Magic/MatterFluxMagicProjectile.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

namespace MatterFluxMagicTests
{
	const TCHAR* BasicPack = TEXT(R"LUA(
content.set_manifest("magic.test", 1, 2)
content.register_spell({
	id = "spell.spark_bolt",
	name = "Spark Bolt",
	description = "A quick luminous projectile.",
	icon = "spark_bolt",
	kind = "projectile",
	mana_cost = 8,
	damage = 12,
	speed = 1200,
	lifetime = 2.0,
	radius = 12,
	cast_delay = 0.05
})
content.register_wand({
	id = "wand.apprentice",
	name = "Apprentice Wand",
	description = "Reliable and non-shuffling.",
	icon = "wand_apprentice",
	capacity = 8,
	shuffle = false,
	draw_count = 1,
	cast_delay = 0.15,
	recharge_time = 0.40,
	mana_max = 100,
	mana_recharge = 20,
	spread = 0
})
)LUA");

	const TCHAR* ProgramPack = TEXT(R"LUA(
content.set_manifest("magic.program", 1, 2)
content.register_spell({
	id = "spell.add_five", name = "Add Five", kind = "modifier",
	mana_cost = 1, damage_add = 5, draw_count = 1
})
content.register_spell({
	id = "spell.double_cast", name = "Double Cast", kind = "multicast",
	mana_cost = 1, draw_count = 2
})
content.register_spell({
	id = "spell.trigger", name = "Trigger", kind = "trigger",
	mana_cost = 1, damage = 1, speed = 900, lifetime = 1,
	radius = 8, trigger_draw_count = 1
})
content.register_spell({
	id = "spell.bolt", name = "Bolt", kind = "projectile",
	mana_cost = 1, damage = 10, speed = 1000, lifetime = 1,
	radius = 8
})
content.register_wand({
	id = "wand.program", name = "Program Wand", capacity = 8,
	shuffle = false, draw_count = 1, cast_delay = 0.1,
	recharge_time = 0.3, mana_max = 20, mana_recharge = 5
})
)LUA");

	const TCHAR* DirectCutPack = TEXT(R"LUA(
content.set_manifest("magic.direct_cut", 1, 2)
content.register_spell({
	id = "spell.direct_cut", name = "Direct Cut", kind = "cut",
	damage = 12, range = 500, radius = 60, thickness = 30
})
)LUA");

	const TCHAR* DirectFlamePack = TEXT(R"LUA(
content.set_manifest("magic.direct_flame", 1, 2)
content.register_material({
	id = "fire", density = 0.01, hardness = 0,
	color_r = 1, color_g = 0.24, color_b = 0.01, color_a = 0.92,
	phase = "gas"
})
content.register_spell({
	id = "spell.direct_flame", name = "Direct Flame", kind = "flame",
	range = 800, radius = 45, end_radius = 180
})
)LUA");

	void RestoreDefault(IMatterFluxScriptRuntime& Runtime)
	{
		FString Ignored;
		Runtime.ReloadDefaultContentPack(Ignored);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicWorkbenchDragBehaviorTest,
	"MatterFlux.Magic.Workbench.DragDropExposesEverySlotAndResolvesEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicWorkbenchDragBehaviorTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxOwnedWand Wand;
	Wand.InstanceId = FGuid(1, 2, 3, 4);
	Wand.SpellSlots = {
		TEXT("spell.bolt"), NAME_None, TEXT("spell.trigger"), NAME_None
	};
	TArray<int32> Targets;
	FMatterFluxMagicWorkbenchInteraction::BuildSpellDropTargets(Wand, Targets);
	TestEqual(TEXT("Every capacity slot remains a visible drop target"),
		Targets, TArray<int32>({0, 1, 2, 3}));

	FMatterFluxMagicDragPayload InventoryPayload;
	InventoryPayload.Source = EMatterFluxMagicDragSource::SpellInventory;
	InventoryPayload.SpellId = TEXT("spell.bolt");
	FMatterFluxMagicEdit Edit;
	TestTrue(TEXT("Inventory spell can drop into an empty wand slot"),
		FMatterFluxMagicWorkbenchInteraction::ResolveSpellDrop(
			InventoryPayload, Wand.InstanceId, 1, Edit));
	TestEqual(TEXT("Inventory drop becomes assign edit"),
		Edit.Type, EMatterFluxMagicEditType::AssignSpell);
	TestEqual(TEXT("Inventory drop keeps target slot"), Edit.ToSpellSlot, 1);

	FMatterFluxMagicDragPayload SlotPayload;
	SlotPayload.Source = EMatterFluxMagicDragSource::WandSpellSlot;
	SlotPayload.WandId = Wand.InstanceId;
	SlotPayload.SpellSlot = 0;
	TestTrue(TEXT("Wand spell can move to another slot"),
		FMatterFluxMagicWorkbenchInteraction::ResolveSpellDrop(
			SlotPayload, Wand.InstanceId, 3, Edit));
	TestEqual(TEXT("Slot drop becomes swap edit"),
		Edit.Type, EMatterFluxMagicEditType::SwapSpellSlots);
	TestEqual(TEXT("Swap remembers source"), Edit.FromSpellSlot, 0);
	TestEqual(TEXT("Swap remembers target"), Edit.ToSpellSlot, 3);
	TestFalse(TEXT("Cross-wand drops are rejected without mutation"),
		FMatterFluxMagicWorkbenchInteraction::ResolveSpellDrop(
			SlotPayload, FGuid(9, 8, 7, 6), 2, Edit));
	TestFalse(TEXT("Dropping onto the same slot is a no-op"),
		FMatterFluxMagicWorkbenchInteraction::ResolveSpellDrop(
			SlotPayload, Wand.InstanceId, 0, Edit));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicProgramColumnsTest,
	"MatterFlux.Magic.ProgramLayout.ExposesEverySpellColumn",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicProgramColumnsTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Program pack loads"),
		Runtime.LoadContentPackFromSource(
			MatterFluxMagicTests::ProgramPack,
			TEXT("ProgramColumns"),
			Error)))
	{
		AddError(Error);
		MatterFluxMagicTests::RestoreDefault(Runtime);
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const TArray<FName> Slots = {
		TEXT("spell.add_five"),
		TEXT("spell.double_cast"),
		TEXT("spell.bolt"),
		TEXT("spell.trigger"),
		TEXT("spell.bolt"),
		NAME_None,
		NAME_None,
		NAME_None
	};
	FMatterFluxSpellProgramLayout Layout;
	TestTrue(
		TEXT("Program layout builds"),
		FMatterFluxSpellProgramLayoutBuilder::Build(
			*Registry,
			Slots,
			Layout,
			Error));
	TestEqual(TEXT("Nested program exposes four columns"),
		Layout.Columns.Num(), 4);
	if (Layout.Columns.Num() == 4)
	{
		TestEqual(TEXT("Root column has one spell"),
			Layout.Columns[0].Nodes.Num(), 1);
		TestEqual(TEXT("Branch column has two spells"),
			Layout.Columns[2].Nodes.Num(), 2);
		TestEqual(TEXT("Trigger payload is in the fourth column"),
			Layout.Columns[3].Nodes[0].SlotIndex, 4);
		TestEqual(TEXT("Trigger payload keeps its parent"),
			Layout.Columns[3].Nodes[0].ParentSlotIndex, 3);
	}
	TestEqual(TEXT("Every capacity slot is accounted for"),
		Layout.GetAccountedSlotCount(), Slots.Num());
	TestEqual(TEXT("Unused capacity remains available"),
		Layout.ReserveSlotIndices.Num(), 3);

	MatterFluxMagicTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicProgramForestTest,
	"MatterFlux.Magic.ProgramLayout.ShowsEveryIndependentProgramRoot",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicProgramForestTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Program pack loads"),
		Runtime.LoadContentPackFromSource(
			MatterFluxMagicTests::ProgramPack,
			TEXT("ProgramForest"),
			Error)))
	{
		AddError(Error);
		MatterFluxMagicTests::RestoreDefault(Runtime);
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const TArray<FName> Slots = {
		TEXT("spell.bolt"),
		TEXT("spell.double_cast"),
		TEXT("spell.add_five"),
		TEXT("spell.bolt"),
		TEXT("spell.trigger"),
		TEXT("spell.bolt"),
		NAME_None,
		TEXT("spell.bolt")
	};
	FMatterFluxSpellProgramLayout Layout;
	TestTrue(
		TEXT("Program forest builds"),
		FMatterFluxSpellProgramLayoutBuilder::Build(
			*Registry,
			Slots,
			Layout,
			Error));
	TestEqual(TEXT("Three independent roots remain visible"),
		Layout.Columns[0].Nodes.Num(), 3);
	if (Layout.Columns[0].Nodes.Num() == 3)
	{
		TestEqual(TEXT("First root slot"),
			Layout.Columns[0].Nodes[0].SlotIndex, 0);
		TestEqual(TEXT("Second root slot"),
			Layout.Columns[0].Nodes[1].SlotIndex, 1);
		TestEqual(TEXT("Root after an empty capacity slot"),
			Layout.Columns[0].Nodes[2].SlotIndex, 7);
		TestEqual(TEXT("Roots keep stable program ordinals"),
			Layout.Columns[0].Nodes[2].RootIndex, 2);
	}
	if (Layout.Columns.Num() >= 3
		&& Layout.Columns[1].Nodes.Num() == 2
		&& Layout.Columns[2].Nodes.Num() == 2)
	{
		const FMatterFluxSpellProgramNode& FirstBranch =
			Layout.Columns[1].Nodes[0];
		const FMatterFluxSpellProgramNode& SecondBranch =
			Layout.Columns[1].Nodes[1];
		TestEqual(TEXT("First branch points to the second root"),
			FirstBranch.RootIndex, 1);
		TestEqual(TEXT("First branch has ordinal one of two"),
			FirstBranch.ChildIndex, 0);
		TestEqual(TEXT("First branch exposes sibling count"),
			FirstBranch.SiblingCount, 2);
		TestEqual(TEXT("Second branch has ordinal two of two"),
			SecondBranch.ChildIndex, 1);
		TestEqual(TEXT("Nested child keeps its root"),
			Layout.Columns[2].Nodes[1].RootIndex, 1);
	}
	TestEqual(TEXT("Only the empty capacity slot is unconnected"),
		Layout.ReserveSlotIndices.Num(), 1);
	TestEqual(TEXT("Every slot is represented exactly once"),
		Layout.GetAccountedSlotCount(), Slots.Num());

	MatterFluxMagicTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicProgramCombinationMatrixTest,
	"MatterFlux.Magic.ProgramLayout.CommonCombinationsRemainReadable",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicProgramCombinationMatrixTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Program pack loads"),
		Runtime.LoadContentPackFromSource(
			MatterFluxMagicTests::ProgramPack,
			TEXT("ProgramCombinationMatrix"),
			Error)))
	{
		AddError(Error);
		MatterFluxMagicTests::RestoreDefault(Runtime);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();

	struct FCase
	{
		const TCHAR* Name;
		TArray<FName> Slots;
		TArray<int32> NodesPerColumn;
		int32 ReserveCount = 0;
	};
	const TArray<FCase> Cases = {
		{
			TEXT("All empty"),
			{NAME_None, NAME_None, NAME_None, NAME_None},
			{},
			4
		},
		{
			TEXT("Three sequential projectiles"),
			{TEXT("spell.bolt"), TEXT("spell.bolt"),
				TEXT("spell.bolt"), NAME_None},
			{3},
			1
		},
		{
			TEXT("Modifier chain then independent root"),
			{TEXT("spell.add_five"), TEXT("spell.add_five"),
				TEXT("spell.bolt"), TEXT("spell.bolt")},
			{2, 1, 1},
			0
		},
		{
			TEXT("Nested multicast and trigger"),
			{TEXT("spell.double_cast"), TEXT("spell.double_cast"),
				TEXT("spell.bolt"), TEXT("spell.bolt"),
				TEXT("spell.trigger"), TEXT("spell.bolt")},
			{1, 2, 3},
			0
		},
		{
			TEXT("Missing first multicast child"),
			{TEXT("spell.double_cast"), NAME_None,
				TEXT("spell.bolt"), NAME_None},
			{1, 2},
			1
		}
	};

	for (const FCase& Case : Cases)
	{
		FMatterFluxSpellProgramLayout Layout;
		Error.Reset();
		if (!TestTrue(
			FString::Printf(TEXT("%s builds"), Case.Name),
			FMatterFluxSpellProgramLayoutBuilder::Build(
				*Registry,
				Case.Slots,
				Layout,
				Error)))
		{
			AddError(Error);
			continue;
		}
		TestEqual(
			FString::Printf(TEXT("%s column count"), Case.Name),
			Layout.Columns.Num(),
			Case.NodesPerColumn.Num());
		for (int32 ColumnIndex = 0;
			ColumnIndex < Case.NodesPerColumn.Num()
				&& ColumnIndex < Layout.Columns.Num();
			++ColumnIndex)
		{
			TestEqual(
				FString::Printf(
					TEXT("%s nodes in column %d"),
					Case.Name,
					ColumnIndex),
				Layout.Columns[ColumnIndex].Nodes.Num(),
				Case.NodesPerColumn[ColumnIndex]);
		}
		TestEqual(
			FString::Printf(TEXT("%s reserve count"), Case.Name),
			Layout.ReserveSlotIndices.Num(),
			Case.ReserveCount);
		TestEqual(
			FString::Printf(TEXT("%s accounts for every slot"), Case.Name),
			Layout.GetAccountedSlotCount(),
			Case.Slots.Num());
	}

	FMatterFluxSpellProgramLayout InvalidLayout;
	const TArray<FName> InvalidSlots = {
		TEXT("spell.bolt"),
		TEXT("spell.unknown")
	};
	Error.Reset();
	TestFalse(
		TEXT("Unknown spell ids are rejected"),
		FMatterFluxSpellProgramLayoutBuilder::Build(
			*Registry,
			InvalidSlots,
			InvalidLayout,
			Error));
	TestEqual(TEXT("Failed layouts expose no partial tree"),
		InvalidLayout.GetAccountedSlotCount(), 0);

	MatterFluxMagicTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicIncompleteBranchCastTest,
	"MatterFlux.Magic.ProgramLayout.IncompleteMulticastBranchRemainsCastable",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicIncompleteBranchCastTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry =
		Runtime.GetActiveRegistry();
	const FMatterFluxWandDefinition* CuttingWand = Registry.IsValid()
		? Registry->Wands.Find(TEXT("wand.cutting"))
		: nullptr;
	if (!TestTrue(TEXT("Default registry exists"), Registry.IsValid())
		|| !TestNotNull(TEXT("Cutting wand exists"), CuttingWand))
	{
		return false;
	}

	// This is the exact prefix program reported from the workbench screenshot:
	// spark trigger -> double cast -> flame jet + one deliberately empty branch.
	const TArray<FName> Slots = {
		TEXT("spell.spark_trigger"),
		TEXT("spell.double_cast"),
		TEXT("spell.flame_jet"),
		NAME_None
	};
	FMatterFluxSpellProgramLayout Layout;
	TestTrue(
		TEXT("The screenshot program has a valid structural layout"),
		FMatterFluxSpellProgramLayoutBuilder::Build(
			*Registry,
			Slots,
			Layout,
			Error));
	TestEqual(TEXT("Trigger, multicast and payload occupy three columns"),
		Layout.Columns.Num(), 3);
	if (Layout.Columns.Num() == 3)
	{
		TestEqual(TEXT("Double cast exposes both child slots"),
			Layout.Columns[2].Nodes.Num(), 2);
		if (Layout.Columns[2].Nodes.Num() == 2)
		{
			TestEqual(TEXT("The second multicast branch is the empty slot"),
				Layout.Columns[2].Nodes[1].SlotIndex, 3);
			TestEqual(TEXT("The empty branch remains connected to double cast"),
				Layout.Columns[2].Nodes[1].ParentSlotIndex, 1);
		}
	}
	TestEqual(TEXT("A structural empty child is not reserve capacity"),
		Layout.ReserveSlotIndices.Num(), 0);

	FMatterFluxWandProgramState State;
	State.Mana = CuttingWand->ManaMax;
	FMatterFluxWandCastPlan Plan;
	Error.Reset();
	if (TestTrue(
		TEXT("An incomplete multicast branch still casts its populated branch"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			CuttingWand->Id,
			Slots,
			State,
			20260824,
			Plan,
			Error)))
	{
		TestEqual(TEXT("The trigger emits one carrier projectile"),
			Plan.Projectiles.Num(), 1);
		if (Plan.Projectiles.Num() == 1)
		{
			TestEqual(TEXT("Only the populated multicast branch is attached"),
				Plan.Projectiles[0].OnImpactProjectiles.Num(), 1);
			if (Plan.Projectiles[0].OnImpactProjectiles.Num() == 1)
			{
				TestEqual(TEXT("The populated branch is the flame jet"),
					Plan.Projectiles[0].OnImpactProjectiles[0].SpellId,
					FName(TEXT("spell.flame_jet")));
			}
		}
		TestEqual(TEXT("Every populated spell card consumes mana"),
			Plan.ManaSpent, 30.0f);
	}
	else
	{
		AddError(Error);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicPaperMagicLibraryTest,
	"MatterFlux.Magic.Content.PaperMagicSpellLibraryIsComplete",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicPaperMagicLibraryTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Default registry exists"), Registry.IsValid()))
	{
		return false;
	}

	const TArray<FName> PaperMagicSpellIds = {
		TEXT("std.add_damage"),
		TEXT("std.circle_trail"),
		TEXT("std.default"),
		TEXT("std.double_cast"),
		TEXT("std.jump"),
		TEXT("std.set_color_red"),
		TEXT("std.trigger_on_collision"),
		TEXT("std.trigger_on_expired"),
		TEXT("std.triple_cast")
	};
	for (const FName SpellId : PaperMagicSpellIds)
	{
		const FMatterFluxSpellDefinition* Spell = Registry->Spells.Find(SpellId);
		TestNotNull(
			*FString::Printf(TEXT("PaperMagic spell %s is registered"),
				*SpellId.ToString()),
			Spell);
		if (Spell)
		{
			FString IconPath;
			TestTrue(
				*FString::Printf(TEXT("PaperMagic spell %s icon resolves"),
					*SpellId.ToString()),
				FMatterFluxMagicIconResolver::TryResolveIconPath(
					Spell->Icon,
					IconPath));
			TestTrue(TEXT("Resolved magic icon stays under the Lua icon root"),
				IconPath.StartsWith(
					FMatterFluxMagicIconResolver::GetIconRoot(),
					ESearchCase::IgnoreCase));
		}
	}
	const TArray<FName> MatterFluxSpellIds = {
		TEXT("spell.accelerate"),
		TEXT("spell.add_damage"),
		TEXT("spell.acid_spray"),
		TEXT("spell.double_cast"),
		TEXT("spell.flame_jet"),
		TEXT("spell.heavy_orb"),
		TEXT("spell.sand_spray"),
		TEXT("spell.spark_bolt"),
		TEXT("spell.spark_trigger"),
		TEXT("spell.terrain_cut"),
		TEXT("spell.vertical_terrain_cut"),
		TEXT("spell.water_spray")
	};
	for (const FName SpellId : MatterFluxSpellIds)
	{
		const FMatterFluxSpellDefinition* Spell = Registry->Spells.Find(SpellId);
		if (TestNotNull(
			*FString::Printf(TEXT("MatterFlux spell %s is registered"),
				*SpellId.ToString()),
			Spell))
		{
			FString IconPath;
			TestTrue(
				*FString::Printf(TEXT("MatterFlux spell %s icon resolves"),
					*SpellId.ToString()),
				FMatterFluxMagicIconResolver::TryResolveIconPath(
					Spell->Icon,
					IconPath));
		}
	}
	const TArray<FName> MigratedWandIds = {
		TEXT("std.default"),
		TEXT("std.default_shoe")
	};
	for (const FName WandId : MigratedWandIds)
	{
		const FMatterFluxWandDefinition* Wand = Registry->Wands.Find(WandId);
		if (TestNotNull(
			*FString::Printf(TEXT("PaperMagic wand %s is registered"),
				*WandId.ToString()),
			Wand))
		{
			FString IconPath;
			TestTrue(
				*FString::Printf(TEXT("PaperMagic wand %s icon resolves"),
					*WandId.ToString()),
				FMatterFluxMagicIconResolver::TryResolveIconPath(
					Wand->Icon,
					IconPath));
		}
	}
	const TArray<FName> MigratedItemIds = {
		TEXT("std.coin"),
		TEXT("std.heal_item")
	};
	for (const FName ItemId : MigratedItemIds)
	{
		const FMatterFluxItemDefinition* Item = Registry->Items.Find(ItemId);
		if (TestNotNull(
			*FString::Printf(TEXT("PaperMagic item %s is registered"),
				*ItemId.ToString()),
			Item))
		{
			FString IconPath;
			TestTrue(
				*FString::Printf(TEXT("PaperMagic item %s icon resolves"),
					*ItemId.ToString()),
				FMatterFluxMagicIconResolver::TryResolveIconPath(
					Item->Icon,
					IconPath));
		}
	}
	FString AddSignIconPath;
	TestTrue(TEXT("PaperMagic empty-slot add icon resolves"),
		FMatterFluxMagicIconResolver::TryResolveIconPath(
			TEXT("paper/add_sign"),
			AddSignIconPath));
	FString RejectedIconPath;
	TestFalse(TEXT("Magic icon keys cannot traverse out of the Lua icon root"),
		FMatterFluxMagicIconResolver::TryResolveIconPath(
			TEXT("../Spells/PaperMagic/Default.lua"),
			RejectedIconPath));
	TestEqual(TEXT("MatterFlux and PaperMagic libraries coexist"),
		Registry->Spells.Num(), 21);

	const FMatterFluxSpellDefinition* DefaultProjectile =
		Registry->Spells.Find(TEXT("std.default"));
	const FMatterFluxSpellDefinition* AddDamage =
		Registry->Spells.Find(TEXT("std.add_damage"));
	const FMatterFluxSpellDefinition* TripleCast =
		Registry->Spells.Find(TEXT("std.triple_cast"));
	if (DefaultProjectile && AddDamage && TripleCast)
	{
		TestEqual(TEXT("Default projectile keeps PaperMagic damage"),
			DefaultProjectile->Damage, 10.0f);
		TestEqual(TEXT("Add damage keeps PaperMagic bonus"),
			AddDamage->DamageAdd, 7.0f);
		TestEqual(TEXT("Triple cast exposes three children"),
			TripleCast->DrawCount, 3);
	}
	const FMatterFluxWandDefinition* PrecisionWand =
		Registry->Wands.Find(TEXT("wand.precision"));
	if (TestNotNull(TEXT("Visible-stat wand exists"), PrecisionWand))
	{
		TestEqual(TEXT("Wand spell capacity is configured"),
			PrecisionWand->Capacity, 12);
		TestEqual(TEXT("Wand mana maximum is configured"),
			PrecisionWand->ManaMax, 220.0f);
		TestEqual(TEXT("Wand mana regeneration is configured"),
			PrecisionWand->ManaRechargePerSecond, 22.0f);
		TestEqual(TEXT("Wand cast interval is configured"),
			PrecisionWand->CastDelay, 0.10f);
	}
	const FMatterFluxWandDefinition* PaperMagicDefaultWand =
		Registry->Wands.Find(TEXT("std.default"));
	if (TestNotNull(TEXT("PaperMagic default wand is migrated"),
		PaperMagicDefaultWand))
	{
		TestEqual(TEXT("Default wand capacity"),
			PaperMagicDefaultWand->Capacity, 10);
		TestEqual(TEXT("Default wand mana maximum"),
			PaperMagicDefaultWand->ManaMax, 100.0f);
		TestEqual(TEXT("Default wand mana recovery"),
			PaperMagicDefaultWand->ManaRechargePerSecond, 10.0f);
		TestEqual(TEXT("Default wand cast interval"),
			PaperMagicDefaultWand->CastDelay, 0.5f);
		TestEqual(TEXT("Default wand starts owned but unbound"),
			PaperMagicDefaultWand->StarterCount, 1);
	}
	const FMatterFluxWandDefinition* PaperMagicShoe =
		Registry->Wands.Find(TEXT("std.default_shoe"));
	if (TestNotNull(TEXT("PaperMagic shoe caster is migrated"), PaperMagicShoe))
	{
		TestEqual(TEXT("Shoe caster capacity"), PaperMagicShoe->Capacity, 1);
		TestEqual(TEXT("Shoe caster mana recovery"),
			PaperMagicShoe->ManaRechargePerSecond, 50.0f);
		TestEqual(TEXT("Shoe caster interval"),
			PaperMagicShoe->CastDelay, 0.5f);
		TestEqual(TEXT("Shoe caster starts owned but unbound"),
			PaperMagicShoe->StarterCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicPaperMagicSemanticsTest,
	"MatterFlux.Magic.Content.PaperMagicSpellsCompileWithEquivalentSemantics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicPaperMagicSemanticsTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Default registry exists"), Registry.IsValid()))
	{
		return false;
	}

	const auto Compile = [this, &Registry, &Error](
		const TCHAR* Label,
		const TArray<FName>& Slots,
		FMatterFluxWandCastPlan& OutPlan)
	{
		FMatterFluxWandProgramState State;
		State.Mana = 220.0f;
		Error.Reset();
		const bool bCompiled = FMatterFluxWandProgram::Evaluate(
			*Registry,
			TEXT("wand.precision"),
			Slots,
			State,
			1337,
			OutPlan,
			Error);
		if (!TestTrue(Label, bCompiled))
		{
			AddError(Error);
		}
		return bCompiled;
	};

	FMatterFluxWandCastPlan DefaultPlan;
	if (Compile(TEXT("Default projectile compiles"),
		{TEXT("std.default")}, DefaultPlan))
	{
		TestEqual(TEXT("Default projectile damage"),
			DefaultPlan.Projectiles[0].Damage, 10.0f);
	}

	FMatterFluxWandCastPlan DamagePlan;
	if (Compile(TEXT("Add damage compiles"),
		{TEXT("std.add_damage"), TEXT("std.default")}, DamagePlan))
	{
		TestEqual(TEXT("Add damage affects its child"),
			DamagePlan.Projectiles[0].Damage, 17.0f);
	}

	FMatterFluxWandCastPlan CirclePlan;
	if (Compile(TEXT("Circle trail compiles"),
		{TEXT("std.circle_trail"), TEXT("std.default")}, CirclePlan))
	{
		TestEqual(TEXT("Circle trail doubles lifetime"),
			CirclePlan.Projectiles[0].Lifetime, 6.0f);
		TestEqual(TEXT("Circle trail carries orbit radius"),
			CirclePlan.Projectiles[0].OrbitRadius, 300.0f);
	}

	FMatterFluxWandCastPlan ColorPlan;
	if (Compile(TEXT("Red modifier compiles"),
		{TEXT("std.set_color_red"), TEXT("std.default")}, ColorPlan))
	{
		TestTrue(TEXT("Red modifier overrides color"),
			ColorPlan.Projectiles[0].bOverrideColor);
		TestEqual(TEXT("Red modifier red channel"),
			ColorPlan.Projectiles[0].Color.R, 1.0f);
		TestEqual(TEXT("Red modifier green channel"),
			ColorPlan.Projectiles[0].Color.G, 0.0f);
	}

	FMatterFluxWandCastPlan DoublePlan;
	if (Compile(TEXT("Double cast compiles"),
		{TEXT("std.double_cast"), TEXT("std.default"), TEXT("std.default")},
		DoublePlan))
	{
		TestEqual(TEXT("Double cast emits two projectiles"),
			DoublePlan.Projectiles.Num(), 2);
		TestEqual(TEXT("Double cast adds target spread"),
			DoublePlan.Projectiles[0].SpreadDegrees, 10.5f);
	}

	FMatterFluxWandCastPlan TriplePlan;
	if (Compile(TEXT("Triple cast compiles"),
		{TEXT("std.triple_cast"), TEXT("std.default"), TEXT("std.default"),
			TEXT("std.default")}, TriplePlan))
	{
		TestEqual(TEXT("Triple cast emits three projectiles"),
			TriplePlan.Projectiles.Num(), 3);
	}

	FMatterFluxWandCastPlan CollisionTriggerPlan;
	if (Compile(TEXT("Collision trigger compiles"),
		{TEXT("std.trigger_on_collision"), TEXT("std.default"),
			TEXT("std.default")}, CollisionTriggerPlan))
	{
		TestEqual(TEXT("Collision trigger keeps one carrier"),
			CollisionTriggerPlan.Projectiles.Num(), 1);
		TestEqual(TEXT("Collision payload uses impact channel"),
			CollisionTriggerPlan.Projectiles[0].OnImpactProjectiles.Num(), 1);
		TestEqual(TEXT("Collision trigger has no expiry payload"),
			CollisionTriggerPlan.Projectiles[0].OnExpireProjectiles.Num(), 0);
		TestTrue(TEXT("Collision payload uses deterministic random direction"),
			CollisionTriggerPlan.Projectiles[0].bTriggerRandomDirection);
	}

	FMatterFluxWandCastPlan ExpireTriggerPlan;
	if (Compile(TEXT("Expiry trigger compiles"),
		{TEXT("std.trigger_on_expired"), TEXT("std.default"),
			TEXT("std.default")}, ExpireTriggerPlan))
	{
		TestEqual(TEXT("Expiry trigger keeps one carrier"),
			ExpireTriggerPlan.Projectiles.Num(), 1);
		TestEqual(TEXT("Expiry payload uses expiry channel"),
			ExpireTriggerPlan.Projectiles[0].OnExpireProjectiles.Num(), 1);
		TestEqual(TEXT("Expiry trigger has no impact payload"),
			ExpireTriggerPlan.Projectiles[0].OnImpactProjectiles.Num(), 0);
	}

	FMatterFluxWandCastPlan JumpPlan;
	if (Compile(TEXT("Jump compiles"), {TEXT("std.jump")}, JumpPlan))
	{
		TestEqual(TEXT("Jump emits one caster effect"),
			JumpPlan.CasterEffects.Num(), 1);
		TestEqual(TEXT("Jump caster effect type"),
			JumpPlan.CasterEffects[0].Type,
			EMatterFluxMagicCasterEffectType::Jump);
		TestEqual(TEXT("Jump impulse"),
			JumpPlan.CasterEffects[0].VerticalImpulse, 600.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicPaperMagicRuntimeEffectsTest,
	"MatterFlux.Magic.Runtime.PaperMagicPresentationExpiryAndJumpExecute",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicPaperMagicRuntimeEffectsTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxCharacter* Avatar = World
		? World->SpawnActor<AMatterFluxCharacter>()
		: nullptr;
	if (!TestNotNull(TEXT("Authority character spawns"), Avatar))
	{
		return false;
	}

	FMatterFluxWandCastPlan JumpPlan;
	FMatterFluxMagicCasterEffectPlan& Jump =
		JumpPlan.CasterEffects.AddDefaulted_GetRef();
	Jump.SpellId = TEXT("std.jump");
	Jump.Type = EMatterFluxMagicCasterEffectType::Jump;
	Jump.VerticalImpulse = 600.0f;
	AActor* NonCharacter = World->SpawnActor<AActor>();
	if (TestNotNull(TEXT("Non-character authority avatar spawns"), NonCharacter))
	{
		TestFalse(TEXT("Jump rejects an incompatible avatar transactionally"),
			UGA_CastWand::SpawnCastPlan(*NonCharacter, JumpPlan, 40));
	}
	TestTrue(TEXT("Jump plan executes through generic GAS cast path"),
		UGA_CastWand::SpawnCastPlan(*Avatar, JumpPlan, 41));
	World->Tick(LEVELTICK_All, 0.016f);
	TestTrue(TEXT("Jump changes authoritative vertical velocity"),
		Avatar->GetVelocity().Z > 500.0);

	FMatterFluxWandCastPlan ProjectilePlan;
	FMatterFluxMagicProjectilePlan& Carrier =
		ProjectilePlan.Projectiles.AddDefaulted_GetRef();
	Carrier.SpellId = TEXT("std.default");
	Carrier.Speed = 600.0f;
	Carrier.Lifetime = 2.0f;
	Carrier.Radius = 12.0f;
	Carrier.bOverrideColor = true;
	Carrier.Color = FLinearColor::Red;
	Carrier.OrbitRadius = 300.0f;
	FMatterFluxMagicProjectilePlan& Payload =
		Carrier.OnExpireProjectiles.AddDefaulted_GetRef();
	Payload.SpellId = TEXT("std.default.payload");
	Payload.Speed = 500.0f;
	Payload.Lifetime = 1.0f;
	Payload.Radius = 8.0f;
	TestTrue(TEXT("Projectile plan spawns"),
		UGA_CastWand::SpawnCastPlan(*Avatar, ProjectilePlan, 73));

	AMatterFluxMagicProjectile* SpawnedCarrier = nullptr;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (It->GetPresentation().SpellId == TEXT("std.default"))
		{
			SpawnedCarrier = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("Carrier projectile exists"), SpawnedCarrier))
	{
		return false;
	}
	TestTrue(TEXT("Red color reaches replicated presentation"),
		SpawnedCarrier->GetPresentation().bOverrideColor);
	TestEqual(TEXT("Orbit radius reaches replicated presentation"),
		SpawnedCarrier->GetPresentation().OrbitRadius, 300.0f);

	const FVector BeforeVelocity =
		SpawnedCarrier->ProjectileMovement->Velocity;
	SpawnedCarrier->Tick(0.05f);
	SpawnedCarrier->SetActorLocation(
		SpawnedCarrier->GetActorLocation()
			+ BeforeVelocity.GetSafeNormal() * 30.0f);
	SpawnedCarrier->Tick(0.05f);
	TestFalse(TEXT("Circle trail bends projectile velocity"),
		SpawnedCarrier->ProjectileMovement->Velocity.GetSafeNormal().Equals(
			BeforeVelocity.GetSafeNormal(), 0.001f));

	SpawnedCarrier->LifeSpanExpired();
	int32 PayloadCount = 0;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (It->GetPresentation().SpellId == TEXT("std.default.payload"))
		{
			++PayloadCount;
		}
	}
	TestEqual(TEXT("Expiry channel spawns its payload"), PayloadCount, 1);

	FMatterFluxWandCastPlan ImpactPlan;
	FMatterFluxMagicProjectilePlan& ImpactCarrier =
		ImpactPlan.Projectiles.AddDefaulted_GetRef();
	ImpactCarrier.SpellId = TEXT("std.impact.carrier");
	ImpactCarrier.Speed = 600.0f;
	ImpactCarrier.Lifetime = 2.0f;
	ImpactCarrier.Radius = 12.0f;
	ImpactCarrier.bTriggerRandomDirection = true;
	FMatterFluxMagicProjectilePlan& ImpactPayload =
		ImpactCarrier.OnImpactProjectiles.AddDefaulted_GetRef();
	ImpactPayload.SpellId = TEXT("std.impact.payload");
	ImpactPayload.Speed = 500.0f;
	ImpactPayload.Lifetime = 1.0f;
	ImpactPayload.Radius = 8.0f;
	TestTrue(TEXT("Impact trigger carrier spawns"),
		UGA_CastWand::SpawnCastPlan(*Avatar, ImpactPlan, 97));
	AMatterFluxMagicProjectile* SpawnedImpactCarrier = nullptr;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (It->GetPresentation().SpellId == TEXT("std.impact.carrier"))
		{
			SpawnedImpactCarrier = *It;
			break;
		}
	}
	if (TestNotNull(TEXT("Impact carrier exists"), SpawnedImpactCarrier))
	{
		FHitResult Hit;
		Hit.ImpactPoint = SpawnedImpactCarrier->GetActorLocation()
			+ FVector(20.0f, 0.0f, 0.0f);
		TestTrue(TEXT("Authority impact resolves once"),
			SpawnedImpactCarrier->ResolveImpactAuthority(Hit));
		TestFalse(TEXT("The same impact cannot resolve twice"),
			SpawnedImpactCarrier->ResolveImpactAuthority(Hit));
	}
	int32 ImpactPayloadCount = 0;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (It->GetPresentation().SpellId == TEXT("std.impact.payload"))
		{
			++ImpactPayloadCount;
		}
	}
	TestEqual(TEXT("Impact channel spawns its payload"),
		ImpactPayloadCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicModifierMulticastTriggerTest,
	"MatterFlux.Magic.ModifierMulticastAndTriggerCompileTransactionally",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicModifierMulticastTriggerTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Program pack loads"),
		Runtime.LoadContentPackFromSource(
			MatterFluxMagicTests::ProgramPack,
			TEXT("ModifierMulticastTrigger"),
			Error)))
	{
		AddError(Error);
		MatterFluxMagicTests::RestoreDefault(Runtime);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		Runtime.GetActiveRegistry();
	const TArray<FName> Slots = {
		TEXT("spell.add_five"),
		TEXT("spell.double_cast"),
		TEXT("spell.bolt"),
		TEXT("spell.trigger"),
		TEXT("spell.bolt")
	};
	FMatterFluxWandProgramState State;
	State.Mana = 20.0f;
	FMatterFluxWandCastPlan Plan;
	if (TestTrue(
		TEXT("Nested program evaluates"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			TEXT("wand.program"),
			Slots,
			State,
			99,
			Plan,
			Error)))
	{
		TestEqual(TEXT("Two root projectiles"),
			Plan.Projectiles.Num(),
			2);
		if (Plan.Projectiles.Num() == 2)
		{
			TestEqual(TEXT("Modifier reaches first projectile"),
				Plan.Projectiles[0].Damage,
				15.0f);
			TestEqual(TEXT("Trigger has one compiled payload"),
				Plan.Projectiles[1].OnImpactProjectiles.Num(),
				1);
			if (Plan.Projectiles[1].OnImpactProjectiles.Num() == 1)
			{
				TestEqual(TEXT("Payload projectile damage"),
					Plan.Projectiles[1].OnImpactProjectiles[0].Damage,
					10.0f);
			}
		}
		TestEqual(TEXT("Every compiled card consumes mana"),
			Plan.ManaSpent,
			5.0f);
		TestEqual(TEXT("Nested draw advances one shared cursor"),
			Plan.NextState.DeckCursor,
			5);
	}

	State.Mana = 4.0f;
	FMatterFluxWandCastPlan RejectedPlan;
	Error.Reset();
	TestFalse(
		TEXT("Insufficient mana rejects the entire cast"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			TEXT("wand.program"),
			Slots,
			State,
			99,
			RejectedPlan,
			Error));
	TestTrue(TEXT("Mana rejection is explained"),
		Error.Contains(TEXT("insufficient mana")));
	TestEqual(TEXT("Rejected plan exposes no partial projectiles"),
		RejectedPlan.Projectiles.Num(),
		0);
	TestEqual(TEXT("Rejected plan exposes no partial mana spend"),
		RejectedPlan.ManaSpent,
		0.0f);

	MatterFluxMagicTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicRegularWorldSpellProjectileTest,
	"MatterFlux.Magic.RegularWorldSpellsCompileAsProjectiles",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicRegularWorldSpellProjectileTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Default registry exists"), Registry.IsValid()))
	{
		return false;
	}
	const FMatterFluxWandDefinition* CuttingWand =
		Registry->Wands.Find(TEXT("wand.cutting"));
	const FMatterFluxWandDefinition* FlameWand =
		Registry->Wands.Find(TEXT("wand.flame"));
	const FMatterFluxSpellDefinition* VerticalCut =
		Registry->Spells.Find(TEXT("spell.vertical_terrain_cut"));
	if (!TestNotNull(TEXT("Cutting starter wand exists"), CuttingWand)
		|| !TestNotNull(TEXT("Flame starter wand exists"), FlameWand)
		|| !TestNotNull(TEXT("Vertical cut spell exists"), VerticalCut))
	{
		return false;
	}
	TestEqual(TEXT("Cutting wand is bound to left mouse slot"),
		CuttingWand->StarterEquipmentSlot, 0);
	TestEqual(TEXT("Flame wand is bound to right mouse slot"),
		FlameWand->StarterEquipmentSlot, 1);

	FMatterFluxWandProgramState State;
	State.Mana = 100.0f;
	FMatterFluxWandCastPlan CutPlan;
	TestTrue(TEXT("Cut wand evaluates through the generic compiler"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			CuttingWand->Id,
			CuttingWand->StarterDeck,
			State,
			7,
			CutPlan,
			Error));
	TestEqual(TEXT("Cut emits one projectile"),
		CutPlan.Projectiles.Num(), 1);
	TestEqual(TEXT("Cut does not bypass the projectile layer"),
		CutPlan.CasterEffects.Num(), 0);
	if (CutPlan.Projectiles.Num() == 1)
	{
		TestTrue(TEXT("Cut projectile travels forward"),
			CutPlan.Projectiles[0].Speed > 0.0f);
		TestTrue(TEXT("Cut projectile can damage material on impact"),
			CutPlan.Projectiles[0].Damage > 0.0f);
		TestTrue(TEXT("Cut projectile requests the Lua-configured plane visual"),
			CutPlan.Projectiles[0].bUsePlaneVisual);
		TestFalse(TEXT("Original cut remains the horizontal plane variant"),
			CutPlan.Projectiles[0].bUseVerticalPlaneVisual);
	}

	FMatterFluxContentRegistry VerticalRegistry = *Registry;
	FMatterFluxWandDefinition VerticalWand = *CuttingWand;
	VerticalWand.Id = TEXT("wand.vertical_cut_test");
	VerticalWand.StarterDeck = { VerticalCut->Id };
	VerticalRegistry.Wands.Add(VerticalWand.Id, VerticalWand);
	FMatterFluxWandCastPlan VerticalPlan;
	State.Mana = VerticalWand.ManaMax;
	Error.Reset();
	TestTrue(TEXT("Vertical cut evaluates through the generic compiler"),
		FMatterFluxWandProgram::Evaluate(
			VerticalRegistry,
			VerticalWand.Id,
			VerticalWand.StarterDeck,
			State,
			9,
			VerticalPlan,
			Error));
	TestEqual(TEXT("Vertical cut emits one projectile"),
		VerticalPlan.Projectiles.Num(), 1);
	if (VerticalPlan.Projectiles.Num() == 1)
	{
		TestTrue(TEXT("Vertical cut keeps a plane visual"),
			VerticalPlan.Projectiles[0].bUsePlaneVisual);
		TestTrue(TEXT("Vertical cut orientation comes from Lua"),
			VerticalPlan.Projectiles[0].bUseVerticalPlaneVisual);
	}

	FMatterFluxWandCastPlan FlamePlan;
	State.Mana = FlameWand->ManaMax;
	Error.Reset();
	TestTrue(TEXT("Flame wand evaluates through the generic compiler"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			FlameWand->Id,
			FlameWand->StarterDeck,
			State,
			8,
			FlamePlan,
			Error));
	TestEqual(TEXT("Flame emits one projectile"),
		FlamePlan.Projectiles.Num(), 1);
	TestEqual(TEXT("Flame does not bypass the projectile layer"),
		FlamePlan.CasterEffects.Num(), 0);
	if (FlamePlan.Projectiles.Num() == 1)
	{
		TestEqual(TEXT("Flame projectile body is made from fire material"),
			FlamePlan.Projectiles[0].BodyMaterial,
			FName(TEXT("fire")));
		TestEqual(TEXT("Flame carries authored material volume"),
			FlamePlan.Projectiles[0].MaterialAmount, 5);
		TestTrue(TEXT("Flame projectile travels forward"),
			FlamePlan.Projectiles[0].Speed > 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicWorkbenchDragVisualTest,
	"MatterFlux.Magic.Workbench.DragVisualMatchesSlotAndCentersCursor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicWorkbenchDragVisualTest::RunTest(
	const FString& Parameters)
{
	const FVector2D SlotSize =
		FMatterFluxMagicWorkbenchInteraction::GetSpellSlotSize();
	TestEqual(TEXT("Spell workbench uses the enlarged readable slot size"),
		SlotSize, FVector2D(72.0f, 72.0f));
	const FVector2D DecoratorSize =
		FMatterFluxMagicWorkbenchInteraction::GetSpellDragDecoratorSize();
	TestEqual(TEXT("Dragged icon has exactly the same size as every spell slot"),
		DecoratorSize, SlotSize);

	const FVector2D CursorPosition(640.0f, 360.0f);
	const FVector2D DecoratorPosition =
		FMatterFluxMagicWorkbenchInteraction::
			CalculateSpellDragDecoratorPosition(CursorPosition);
	TestEqual(TEXT("Mouse hotspot is the dragged item center"),
		DecoratorPosition + DecoratorSize * 0.5f,
		CursorPosition);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicProjectileSpawnClearanceTest,
	"MatterFlux.Magic.ProjectileSpawnClearsCasterAndCutUsesPlane",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicProjectileSpawnClearanceTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		Runtime.GetActiveRegistry();
	const FMatterFluxWandDefinition* CuttingWand = Registry.IsValid()
		? Registry->Wands.Find(TEXT("wand.cutting"))
		: nullptr;
	const FMatterFluxWandDefinition* FlameWand = Registry.IsValid()
		? Registry->Wands.Find(TEXT("wand.flame"))
		: nullptr;
	const FMatterFluxSpellDefinition* VerticalCut = Registry.IsValid()
		? Registry->Spells.Find(TEXT("spell.vertical_terrain_cut"))
		: nullptr;
	if (!TestNotNull(TEXT("Cutting wand exists"), CuttingWand)
		|| !TestNotNull(TEXT("Flame wand exists"), FlameWand)
		|| !TestNotNull(TEXT("Vertical cut spell exists"), VerticalCut))
	{
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxCharacter* Avatar = World
		? World->SpawnActor<AMatterFluxCharacter>()
		: nullptr;
	if (!TestNotNull(TEXT("Caster character spawns"), Avatar)
		|| !TestNotNull(TEXT("Caster capsule exists"),
			Avatar ? Avatar->GetCapsuleComponent() : nullptr))
	{
		return false;
	}

	const auto SpawnWandProjectile = [
		this,
		Registry,
		Avatar,
		World,
		&Error](
		const FMatterFluxWandDefinition& Wand,
		const FName ExpectedSpellId,
		const int32 EventSeed)
		-> AMatterFluxMagicProjectile*
	{
		FMatterFluxWandProgramState State;
		State.Mana = Wand.ManaMax;
		FMatterFluxWandCastPlan Plan;
		Error.Reset();
		if (!TestTrue(
			*FString::Printf(TEXT("%s compiles"), *Wand.Id.ToString()),
			FMatterFluxWandProgram::Evaluate(
				*Registry,
				Wand.Id,
				Wand.StarterDeck,
				State,
				EventSeed,
				Plan,
				Error)))
		{
			AddError(Error);
			return nullptr;
		}
		if (!TestTrue(
			*FString::Printf(TEXT("%s spawns"), *ExpectedSpellId.ToString()),
			UGA_CastWand::SpawnCastPlan(*Avatar, Plan, EventSeed)))
		{
			return nullptr;
		}
		for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
		{
			if (It->GetPresentation().SpellId == ExpectedSpellId)
			{
				return *It;
			}
		}
		return nullptr;
	};

	AMatterFluxMagicProjectile* CutProjectile =
		SpawnWandProjectile(*CuttingWand, TEXT("spell.terrain_cut"), 6101);
	AMatterFluxMagicProjectile* FlameProjectile =
		SpawnWandProjectile(*FlameWand, TEXT("spell.flame_jet"), 6102);
	FMatterFluxWandCastPlan VerticalCastPlan;
	FMatterFluxMagicProjectilePlan& VerticalProjectilePlan =
		VerticalCastPlan.Projectiles.AddDefaulted_GetRef();
	VerticalProjectilePlan.SpellId = VerticalCut->Id;
	VerticalProjectilePlan.Damage = VerticalCut->Damage;
	VerticalProjectilePlan.Speed = VerticalCut->Speed;
	VerticalProjectilePlan.Lifetime = VerticalCut->Lifetime;
	VerticalProjectilePlan.Radius = VerticalCut->Radius;
	VerticalProjectilePlan.bUsePlaneVisual = VerticalCut->bUsePlaneVisual;
	VerticalProjectilePlan.bUseVerticalPlaneVisual =
		VerticalCut->bUseVerticalPlaneVisual;
	TestTrue(TEXT("Vertical cut spawns"),
		UGA_CastWand::SpawnCastPlan(*Avatar, VerticalCastPlan, 6103));
	AMatterFluxMagicProjectile* VerticalProjectile = nullptr;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (It->GetPresentation().SpellId == VerticalCut->Id)
		{
			VerticalProjectile = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("Cut projectile exists"), CutProjectile)
		|| !TestNotNull(TEXT("Flame projectile exists"), FlameProjectile)
		|| !TestNotNull(TEXT("Vertical cut projectile exists"), VerticalProjectile))
	{
		return false;
	}
	// CreateNewMap produces an editor world, so explicitly cross the same
	// BeginPlay seam that applies collision radius and visual presentation in a
	// game world before asserting the first-frame geometry.
	if (!CutProjectile->HasActorBegunPlay())
	{
		CutProjectile->DispatchBeginPlay();
	}
	if (!FlameProjectile->HasActorBegunPlay())
	{
		FlameProjectile->DispatchBeginPlay();
	}
	if (!VerticalProjectile->HasActorBegunPlay())
	{
		VerticalProjectile->DispatchBeginPlay();
	}

	const auto TestSpawnClearance = [
		this,
		Avatar](
		const TCHAR* Label,
		const AMatterFluxMagicProjectile& Projectile)
	{
		const UCapsuleComponent* Capsule = Avatar->GetCapsuleComponent();
		const FVector Relative =
			Projectile.GetActorLocation() - Avatar->GetActorLocation();
		const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		const float CapsuleSegmentHalfLength = FMath::Max(
			0.0f,
			Capsule->GetScaledCapsuleHalfHeight() - CapsuleRadius);
		const FVector ClosestCapsulePoint =
			Avatar->GetActorLocation() + FVector(
				0.0f,
				0.0f,
				FMath::Clamp(
					Relative.Z,
					-CapsuleSegmentHalfLength,
					CapsuleSegmentHalfLength));
		const float RequiredSeparation =
			CapsuleRadius + Projectile.Collision->GetScaledSphereRadius();
		TestTrue(
			Label,
			FVector::DistSquared(
				Projectile.GetActorLocation(),
				ClosestCapsulePoint)
				> FMath::Square(RequiredSeparation));
	};
	TestSpawnClearance(
		TEXT("Cut projectile is born clear of the caster capsule"),
		*CutProjectile);
	TestSpawnClearance(
		TEXT("Flame projectile is born clear of the caster capsule"),
		*FlameProjectile);

	const FVector CutScale = CutProjectile->Visual->GetRelativeScale3D();
	TestTrue(TEXT("Cut projectile is a thin horizontal plane at cast height"),
		CutScale.Z <= 0.10f
			&& CutScale.X >= CutScale.Z * 5.0f
			&& CutScale.Y >= CutScale.Z * 5.0f
			&& FVector::DotProduct(
				CutProjectile->Visual->GetUpVector(),
				FVector::UpVector) >= 0.99f);
	const FVector VerticalCutScale =
		VerticalProjectile->Visual->GetRelativeScale3D();
	TestTrue(TEXT("Vertical cut travels edge-first as an upright blade"),
		VerticalCutScale.Y <= 0.10f
			&& VerticalCutScale.X >= VerticalCutScale.Y * 5.0f
			&& VerticalCutScale.Z >= VerticalCutScale.Y * 5.0f
			&& FVector::DotProduct(
				VerticalProjectile->Visual->GetForwardVector(),
				VerticalProjectile->GetActorForwardVector()) >= 0.99f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCutProjectileImpactShapeTest,
	"MatterFlux.Magic.Projectile.CutImpactIsSingleCellLine",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxCutProjectileImpactShapeTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FMatterFluxMagicProjectilePlan HorizontalPlan;
	HorizontalPlan.Radius = 60.0f;
	HorizontalPlan.bUsePlaneVisual = true;
	FMatterFluxMagicProjectilePlan VerticalPlan = HorizontalPlan;
	VerticalPlan.bUseVerticalPlaneVisual = true;
	FMatterFluxMagicProjectilePlan OrbPlan;
	OrbPlan.Radius = 20.0f;

	const FFragmentDamageShape Horizontal =
		AMatterFluxMagicProjectile::BuildImpactCutShape(
			HorizontalPlan,
			FVector::ForwardVector,
			FVector::ZeroVector);
	const FFragmentDamageShape Vertical =
		AMatterFluxMagicProjectile::BuildImpactCutShape(
			VerticalPlan,
			FVector::ForwardVector,
			FVector::ZeroVector);
	const FFragmentDamageShape Orb =
		AMatterFluxMagicProjectile::BuildImpactCutShape(
			OrbPlan,
			FVector::ForwardVector,
			FVector::ZeroVector);

	TestEqual(TEXT("Ordinary projectile damage remains radial"),
		Orb.Type, EFragmentDamageShapeType::Circle);
	TestTrue(TEXT("Ordinary projectile keeps its configured radius"),
		FMath::IsNearlyEqual(Orb.Radius, 20.0f));
	TestEqual(TEXT("Horizontal cut uses a line shape"),
		Horizontal.Type, EFragmentDamageShapeType::Line);
	TestEqual(TEXT("Vertical cut uses a line shape"),
		Vertical.Type, EFragmentDamageShapeType::Line);
	TestTrue(TEXT("Horizontal cut spans the projectile diameter"),
		FMath::IsNearlyEqual(Horizontal.Extents.X, 120.0f));
	TestTrue(TEXT("Vertical cut spans the projectile diameter"),
		FMath::IsNearlyEqual(Vertical.Extents.X, 120.0f));
	TestTrue(TEXT("Horizontal cut is no thicker than one material cell"),
		Horizontal.Thickness <= 10.0f);
	TestTrue(TEXT("Vertical cut is no thicker than one material cell"),
		Vertical.Thickness <= 10.0f);
	TestTrue(TEXT("Horizontal cut runs along world right"),
		FMath::Abs(FVector::DotProduct(
			Horizontal.WorldTransform.GetUnitAxis(EAxis::X),
			FVector::RightVector)) >= 0.99f);
	TestTrue(TEXT("Horizontal cut thickness is vertical"),
		FMath::Abs(FVector::DotProduct(
			Horizontal.WorldTransform.GetUnitAxis(EAxis::Z),
			FVector::UpVector)) >= 0.99f);
	TestTrue(TEXT("Vertical cut runs along world up"),
		FMath::Abs(FVector::DotProduct(
			Vertical.WorldTransform.GetUnitAxis(EAxis::X),
			FVector::UpVector)) >= 0.99f);
	TestTrue(TEXT("Vertical cut thickness runs across world right"),
		FMath::Abs(FVector::DotProduct(
			Vertical.WorldTransform.GetUnitAxis(EAxis::Z),
			FVector::RightVector)) >= 0.99f);

	// Fragment masks occupy local XZ with Y as their face normal. Build a
	// second pair aimed into that normal so the raster audit measures a real
	// face-on projectile impact rather than a coplanar/edge-on intersection.
	const FFragmentDamageShape FaceOnHorizontal =
		AMatterFluxMagicProjectile::BuildImpactCutShape(
			HorizontalPlan,
			FVector::RightVector,
			FVector::ZeroVector);
	const FFragmentDamageShape FaceOnVertical =
		AMatterFluxMagicProjectile::BuildImpactCutShape(
			VerticalPlan,
			FVector::RightVector,
			FVector::ZeroVector);
	const auto CountRemovedCells = [](
		const FFragmentDamageShape& Shape)
	{
		constexpr int32 Side = 21;
		TArray<uint8> Mask;
		Mask.Init(1, Side * Side);
		MatterFlux::FragmentGeometry::ApplyDamageShape(
			Mask,
			Side,
			Side,
			10.0f,
			Shape);
		int32 RemovedCells = 0;
		for (const uint8 Cell : Mask)
		{
			RemovedCells += Cell == 0 ? 1 : 0;
		}
		return RemovedCells;
	};
	TestEqual(TEXT("Horizontal cut removes exactly one 13-cell line"),
		CountRemovedCells(FaceOnHorizontal), 13);
	TestEqual(TEXT("Vertical cut removes exactly one 13-cell line"),
		CountRemovedCells(FaceOnVertical), 13);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicMaterialSpraySpellTest,
	"MatterFlux.Magic.Content.WaterAndSandSpraysCompileAsForwardMaterialProjectiles",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicMaterialSpraySpellTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		Runtime.GetActiveRegistry();
	const FMatterFluxWandDefinition* SprayWand = Registry.IsValid()
		? Registry->Wands.Find(TEXT("wand.flame"))
		: nullptr;
	if (!TestTrue(TEXT("Default registry exists"), Registry.IsValid())
		|| !TestNotNull(TEXT("A rapid-casting wand exists"), SprayWand))
	{
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* MaterialWorld = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	AActor* Avatar = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("Material world spawns"), MaterialWorld)
		|| !TestNotNull(TEXT("Authority avatar spawns"), Avatar))
	{
		return false;
	}
	MaterialWorld->Regenerate(24680);

	struct FExpectedSpray
	{
		FName SpellId;
		FName MaterialId;
		FVector ImpactPoint;
		float GravityScale;
		int32 MaterialAmount;
	};
	const FExpectedSpray ExpectedSprays[] = {
		{ TEXT("spell.water_spray"), TEXT("water"),
			FVector(0.0f, 0.0f, 0.0f), 0.85f, 5 },
		{ TEXT("spell.sand_spray"), TEXT("sand"),
			FVector(100.0f, 0.0f, 0.0f), 1.0f, 5 },
		{ TEXT("spell.acid_spray"), TEXT("acid"),
			FVector(200.0f, 0.0f, 0.0f), 0.45f, 5 }
	};

	for (const FExpectedSpray& Expected : ExpectedSprays)
	{
		const FMatterFluxSpellDefinition* Definition =
			Registry->Spells.Find(Expected.SpellId);
		if (!TestNotNull(
			*FString::Printf(TEXT("%s is registered"),
				*Expected.SpellId.ToString()),
			Definition))
		{
			continue;
		}
		TestEqual(TEXT("Spray uses the generic projectile compiler"),
			Definition->Kind, EMatterFluxSpellKind::Projectile);
		TestEqual(TEXT("In-flight spray uses its MatterFlux material"),
			Definition->BodyMaterial, Expected.MaterialId);
		TestEqual(TEXT("Spray authors its carried material volume"),
			Definition->MaterialAmount, Expected.MaterialAmount);
		TestTrue(TEXT("Spray is included in the starter spell inventory"),
			Definition->StarterCount > 0);

		FMatterFluxWandProgramState State;
		State.Mana = SprayWand->ManaMax;
		const TArray<FName> Slots = { Expected.SpellId };
		FMatterFluxWandCastPlan Plan;
		Error.Reset();
		if (!TestTrue(TEXT("Spray compiles through the wand program"),
			FMatterFluxWandProgram::Evaluate(
				*Registry,
				SprayWand->Id,
				Slots,
				State,
				77,
				Plan,
				Error)))
		{
			AddError(Error);
			continue;
		}
		if (!TestEqual(TEXT("Spray emits one short-lived material body"),
			Plan.Projectiles.Num(), 1))
		{
			continue;
		}
		TestTrue(TEXT("Spray projectile has forward travel speed"),
			Plan.Projectiles[0].Speed > 0.0f);
		TestEqual(TEXT("Compiled spray retains its carried material volume"),
			Plan.Projectiles[0].MaterialAmount,
			Expected.MaterialAmount);
		TestTrue(TEXT("Spray keeps its authored gravity scale"),
			FMath::IsNearlyEqual(
				Plan.Projectiles[0].GravityScale,
				Expected.GravityScale));
		TestTrue(TEXT("Spray remains short range"),
			Plan.Projectiles[0].Lifetime < 1.0f);
		if (!TestTrue(TEXT("Spray projectile spawns"),
			UGA_CastWand::SpawnCastPlan(*Avatar, Plan, 77)))
		{
			continue;
		}

		AMatterFluxMagicProjectile* Projectile = nullptr;
		for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
		{
			if (It->GetPresentation().SpellId == Expected.SpellId)
			{
				Projectile = *It;
				break;
			}
		}
		if (!TestNotNull(TEXT("Spawned spray projectile exists"), Projectile))
		{
			continue;
		}
		TestTrue(TEXT("Spawned spray travels in front of the caster"),
			FVector::DotProduct(
				Projectile->ProjectileMovement->Velocity,
				Avatar->GetActorForwardVector()) > 0.0f);
		TestTrue(TEXT("Material projectile uses swept collision"),
			Projectile->ProjectileMovement->bSweepCollision);
		TestTrue(TEXT("Blocking movement stop is wired to material impact"),
			Projectile->ProjectileMovement->OnProjectileStop.IsBound());
		TestTrue(TEXT("Material projectile blocks world geometry"),
			Projectile->Collision->GetCollisionResponseToChannel(ECC_WorldStatic)
				== ECR_Block);
		TestTrue(TEXT("Projectile movement applies authored gravity"),
			FMath::IsNearlyEqual(
				Projectile->ProjectileMovement->ProjectileGravityScale,
				Expected.GravityScale));

		for (int32 Y = -4; Y <= 4; ++Y)
		{
			for (int32 X = -4; X <= 4; ++X)
			{
				MaterialWorld->SetSimulatedMaterialAtWorldLocation(
					Expected.ImpactPoint
						+ FVector(X * 8.0f, Y * 8.0f, 0.0f),
					NAME_None);
			}
		}
		const int32 MaterialCellsBefore =
			MaterialWorld->GetSimulatedMaterialCount(Expected.MaterialId);
		const int64 MaterialAmountBefore =
			MaterialWorld->GetSimulatedMaterialAmount(Expected.MaterialId);
		FHitResult Hit;
		Hit.ImpactPoint = Expected.ImpactPoint;
		TestTrue(TEXT("Spray resolves its authority impact"),
			Projectile->ResolveImpactAuthority(Hit));
		TestTrue(TEXT("Impact retires the flight-only projectile state"),
			Projectile->IsActorBeingDestroyed());
		TestEqual(TEXT("Spray conserves its authored material volume"),
			MaterialWorld->GetSimulatedMaterialAmount(Expected.MaterialId)
				- MaterialAmountBefore,
			static_cast<int64>(Expected.MaterialAmount) * 255);
		const int32 DepositedCells =
			MaterialWorld->GetSimulatedMaterialCount(Expected.MaterialId)
				- MaterialCellsBefore;
		if (Expected.MaterialId == TEXT("water")
			|| Expected.MaterialId == TEXT("acid"))
		{
			TestTrue(TEXT("Liquid spray lands as a shallow connected splash"),
				DepositedCells > Expected.MaterialAmount);
		}
		else
		{
			TestEqual(TEXT("Powder spray enters one stackable impact column"),
				DepositedCells,
				1);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicAcidCorrosionSpellTest,
	"MatterFlux.Magic.Content.AcidSprayCorrodesSourcesAndEmitsGas",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicAcidCorrosionSpellTest::RunTest(
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
	if (!TestTrue(TEXT("Default registry exists"), Registry.IsValid()))
	{
		return false;
	}
	const FMatterFluxReactionDefinition* TerrainCorrosion =
		Registry->Reactions.Find(TEXT("acid_stone_corrosion"));
	const FMatterFluxReactionDefinition* SourceCorrosion =
		Registry->Reactions.Find(TEXT("wood_corrosion_acid"));
	const FMatterFluxSpellDefinition* AcidSpell =
		Registry->Spells.Find(TEXT("spell.acid_spray"));
	if (!TestNotNull(TEXT("Acid corrodes stone terrain"), TerrainCorrosion)
		|| !TestNotNull(TEXT("Acid corrodes wood sources"), SourceCorrosion)
		|| !TestNotNull(TEXT("Acid spell is registered"), AcidSpell))
	{
		return false;
	}
	TestTrue(TEXT("Acid relies on corrosion instead of generic impact damage"),
		FMath::IsNearlyZero(AcidSpell->Damage));
	TestTrue(TEXT("Terrain corrosion is a contact reaction"),
		TerrainCorrosion->Kind
			== FMatterFluxReactionDefinition::EKind::Contact);
	TestTrue(TEXT("Source corrosion propagates through the item"),
		SourceCorrosion->Kind
			== FMatterFluxReactionDefinition::EKind::Propagating);
	TestEqual(TEXT("Source corrosion emits acid gas"),
		SourceCorrosion->EmissionMaterial, FName(TEXT("acid_gas")));
	TestEqual(TEXT("Corroded source cells leave no solid residue"),
		SourceCorrosion->OutputA, FName(TEXT("empty")));

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AFragment2DSourceActor* Source = World
		? World->SpawnActor<AFragment2DSourceActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Corrodible source spawns"), Source))
	{
		return false;
	}
	FFragmentSourceMask Mask;
	Mask.Width = 5;
	Mask.Height = 5;
	Mask.CellSize = 10.0f;
	Mask.MinFragmentAreaPixels = 1;
	Mask.MaxFragmentsPerBreak = 4;
	Mask.SolidMask.Init(1, Mask.Width * Mask.Height);
	if (!TestTrue(TEXT("Wood source initializes"),
		Source->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(TEXT("AcidSpellSource"), 1),
			FLinearColor::White,
			TEXT("wood"))))
	{
		return false;
	}
	TestTrue(TEXT("Acid activates corrosion on an item source"),
		Source->ApplyMaterialStimulusAtWorldLocation(
			Source->GetActorLocation()
				+ FVector(0.0f, 0.0f, Mask.CellSize * 2.0f),
			TEXT("acid"),
			991));
	TestTrue(TEXT("Acid source starts reacting"), Source->IsReacting());
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor> SmokeAnchors;
	Source->GatherReactionSmokeAnchors(SmokeAnchors, 16);
	TestTrue(TEXT("Active corrosion exposes smoke anchors"),
		!SmokeAnchors.IsEmpty());
	for (int32 Step = 0; Step < 5; ++Step)
	{
		Source->Tick(0.1f);
	}
	UInstancedStaticMeshComponent* CorrosionFlames = nullptr;
	UPointLightComponent* CorrosionFireLight = nullptr;
	TInlineComponentArray<UInstancedStaticMeshComponent*> InstanceComponents(
		Source);
	for (UInstancedStaticMeshComponent* Component : InstanceComponents)
	{
		if (Component && Component->GetFName() == TEXT("ReactionFlames"))
		{
			CorrosionFlames = Component;
			break;
		}
	}
	TInlineComponentArray<UPointLightComponent*> PointLights(Source);
	for (UPointLightComponent* Component : PointLights)
	{
		if (Component && Component->GetFName() == TEXT("ReactionFireLight"))
		{
			CorrosionFireLight = Component;
			break;
		}
	}
	TestTrue(TEXT("Corrosion owns no flame instances"),
		!CorrosionFlames || CorrosionFlames->GetInstanceCount() == 0);
	TestTrue(TEXT("Corrosion does not illuminate wood like fire"),
		!CorrosionFireLight || !CorrosionFireLight->IsVisible());
	TestTrue(TEXT("Acid consumes source material"),
		Source->GetRemainingInputCellCount() < Mask.SolidMask.Num());

	AFragment2DSourceActor* BurningSource =
		World->SpawnActor<AFragment2DSourceActor>();
	if (!TestNotNull(TEXT("Combustible control source spawns"), BurningSource))
	{
		return false;
	}
	BurningSource->SetActorLocation(FVector(1000.0f, 0.0f, 0.0f));
	if (!TestTrue(TEXT("Combustible control source initializes"),
		BurningSource->InitializeFromProceduralMask(
			Mask,
			FGuid::NewDeterministicGuid(TEXT("AcidSpellFireControl"), 1),
			FLinearColor::White,
			TEXT("wood"))))
	{
		return false;
	}
	TestTrue(TEXT("Fire still activates combustion"),
		BurningSource->ApplyMaterialStimulusAtWorldLocation(
			BurningSource->GetActorLocation(),
			TEXT("fire"),
			992));
	BurningSource->Tick(0.1f);
	UInstancedStaticMeshComponent* FireFlames = nullptr;
	TInlineComponentArray<UInstancedStaticMeshComponent*> FireInstances(
		BurningSource);
	for (UInstancedStaticMeshComponent* Component : FireInstances)
	{
		if (Component && Component->GetFName() == TEXT("ReactionFlames"))
		{
			FireFlames = Component;
			break;
		}
	}
	UPointLightComponent* FireLight = nullptr;
	TInlineComponentArray<UPointLightComponent*> FireLights(BurningSource);
	for (UPointLightComponent* Component : FireLights)
	{
		if (Component && Component->GetFName() == TEXT("ReactionFireLight"))
		{
			FireLight = Component;
			break;
		}
	}
	TestTrue(TEXT("Real combustion retains flame instances"),
		FireFlames && FireFlames->GetInstanceCount() > 0);
	TestTrue(TEXT("Real combustion retains its fire light"),
		FireLight && FireLight->IsVisible());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicDirectWorldSpellKindRejectionTest,
	"MatterFlux.Magic.DirectWorldSpellKindsAreRejected",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicDirectWorldSpellKindRejectionTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	TestFalse(TEXT("A cut spell cannot bypass projectile compilation"),
		Runtime.LoadContentPackFromSource(
			MatterFluxMagicTests::DirectCutPack,
			TEXT("DirectCutKindRejection"),
			Error));
	TestTrue(*FString::Printf(
		TEXT("Cut rejection identifies the unsupported kind: %s"), *Error),
		Error.Contains(TEXT("kind")));

	Error.Reset();
	TestFalse(TEXT("A flame spell cannot bypass projectile compilation"),
		Runtime.LoadContentPackFromSource(
			MatterFluxMagicTests::DirectFlamePack,
			TEXT("DirectFlameKindRejection"),
			Error));
	TestTrue(*FString::Printf(
		TEXT("Flame rejection identifies the unsupported kind: %s"), *Error),
		Error.Contains(TEXT("kind")));
	MatterFluxMagicTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicMaterialBodyProjectileTest,
	"MatterFlux.Magic.MaterialBodyProjectileUsesVoxelSphereWithoutGravity",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicMaterialBodyProjectileTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content pack loads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const FMatterFluxWandDefinition* FlameWand = Registry.IsValid()
		? Registry->Wands.Find(TEXT("wand.flame"))
		: nullptr;
	if (!TestNotNull(TEXT("Flame wand exists"), FlameWand))
	{
		return false;
	}
	FMatterFluxWandProgramState State;
	State.Mana = FlameWand->ManaMax;
	FMatterFluxWandCastPlan Plan;
	if (!TestTrue(TEXT("Flame wand compiles"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			FlameWand->Id,
			FlameWand->StarterDeck,
			State,
			91,
			Plan,
			Error)))
	{
		AddError(Error);
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AActor* Avatar = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("Authority avatar spawns"), Avatar)
		|| !TestTrue(TEXT("Flame cast plan spawns"),
			UGA_CastWand::SpawnCastPlan(*Avatar, Plan, 91)))
	{
		return false;
	}
	AMatterFluxMagicProjectile* FlameProjectile = nullptr;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (It->GetPresentation().SpellId == TEXT("spell.flame_jet"))
		{
			FlameProjectile = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("Flame projectile exists"), FlameProjectile))
	{
		return false;
	}
	const int32 FlameVoxelCount =
		FlameProjectile->GetMaterialBodyVoxelCount();
	TestTrue(*FString::Printf(
		TEXT("Flame body contains multiple material voxels: %d"),
		FlameVoxelCount),
		FlameVoxelCount > 1);
	TestEqual(TEXT("Flame body material is replicated presentation data"),
		FlameProjectile->GetPresentation().BodyMaterial,
		FName(TEXT("fire")));
	TestEqual(TEXT("Flame projectile ignores gravity"),
		FlameProjectile->ProjectileMovement->ProjectileGravityScale,
		0.0f);
	TestTrue(TEXT("Flame projectile moves forward"),
		FVector::DotProduct(
			FlameProjectile->ProjectileMovement->Velocity,
			Avatar->GetActorForwardVector()) > 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicProjectileMaterialImpactTest,
	"MatterFlux.Magic.ProjectileImpactHandsMaterialToFallingSandWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicProjectileMaterialImpactTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* MaterialWorld = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	AActor* Avatar = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("Material world spawns"), MaterialWorld)
		|| !TestNotNull(TEXT("Authority avatar spawns"), Avatar))
	{
		return false;
	}
	MaterialWorld->Regenerate(13579);
	float ContactSurfaceZ = 0.0f;
	if (!TestTrue(
		TEXT("Material impact contact has a simulated terrain surface"),
		MaterialWorld->TrySampleTerrainHeightAtWorldLocation(
			FVector::ZeroVector,
			ContactSurfaceZ)))
	{
		return false;
	}
	const FVector ContactLocation(0.0f, 0.0f, ContactSurfaceZ);
	AFragment2DSourceActor* ReactiveSource =
		World->SpawnActor<AFragment2DSourceActor>();
	if (!TestNotNull(TEXT("Reactive source spawns"), ReactiveSource))
	{
		return false;
	}
	ReactiveSource->SetActorLocation(ContactLocation);
	FFragmentSourceMask ReactiveMask;
	ReactiveMask.Width = 3;
	ReactiveMask.Height = 3;
	ReactiveMask.CellSize = 20.0f;
	ReactiveMask.MinFragmentAreaPixels = 1;
	ReactiveMask.MaxFragmentsPerBreak = 4;
	ReactiveMask.SolidMask.Init(1, 9);
	if (!TestTrue(
		TEXT("Reactive source initializes"),
		ReactiveSource->InitializeFromProceduralMask(
			ReactiveMask,
			FGuid::NewDeterministicGuid(TEXT("MaterialImpactSource"), 1),
			FLinearColor::White,
			TEXT("wood"))))
	{
		return false;
	}
	const int32 FireCellsBefore =
		MaterialWorld->GetSimulatedMaterialCount(TEXT("fire"));

	FMatterFluxWandCastPlan Plan;
	FMatterFluxMagicProjectilePlan& ProjectilePlan =
		Plan.Projectiles.AddDefaulted_GetRef();
	ProjectilePlan.SpellId = TEXT("spell.material_impact_test");
	ProjectilePlan.Speed = 800.0f;
	ProjectilePlan.Lifetime = 1.0f;
	ProjectilePlan.Radius = 20.0f;
	ProjectilePlan.Damage = 12.0f;
	ProjectilePlan.BodyMaterial = TEXT("fire");
	ProjectilePlan.MaterialAmount = 1;
	if (!TestTrue(TEXT("Material projectile spawns"),
		UGA_CastWand::SpawnCastPlan(*Avatar, Plan, 502)))
	{
		return false;
	}
	AMatterFluxMagicProjectile* Projectile = nullptr;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		if (It->GetPresentation().SpellId
			== TEXT("spell.material_impact_test"))
		{
			Projectile = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("Material projectile exists"), Projectile))
	{
		return false;
	}
	FHitResult Hit;
	Hit.ImpactPoint = ContactLocation;
	const int32 SourceRevisionBeforeImpact = ReactiveSource->Revision;
	TestTrue(TEXT("Projectile resolves its material impact"),
		Projectile->ResolveImpactAuthority(Hit));
	TestEqual(
		TEXT("A conventional damage projectile cannot directly cut world state"),
		ReactiveSource->Revision,
		SourceRevisionBeforeImpact);
	TestTrue(TEXT("Impact material enters the falling-sand world"),
		MaterialWorld->GetSimulatedMaterialCount(TEXT("fire"))
			> FireCellsBefore);
	TestFalse(
		TEXT("Projectile impact does not mutate a reactive source synchronously"),
		ReactiveSource->IsReacting());
	MaterialWorld->Tick(0.06f);
	TestTrue(
		TEXT("The material interaction step applies the deposited stimulus"),
		ReactiveSource->IsReacting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicInventoryEditRulesTest,
	"MatterFlux.Magic.InventoryEditsPreserveOwnershipAndRejectInvalidInput",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicInventoryEditRulesTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	Runtime.ReloadDefaultContentPack(Error);
	const FMatterFluxContentRegistryPtr Registry =
		Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Default magic registry exists"), Registry.IsValid()))
	{
		return false;
	}

	TArray<FMatterFluxOwnedSpell> Spells;
	FMatterFluxOwnedSpell& Stack = Spells.AddDefaulted_GetRef();
	Stack.SpellId = TEXT("spell.spark_bolt");
	Stack.Quantity = 2;
	TArray<FMatterFluxOwnedWand> Wands;
	FMatterFluxOwnedWand& Wand = Wands.AddDefaulted_GetRef();
	Wand.InstanceId = FGuid::NewDeterministicGuid(
		TEXT("MagicInventoryRules"),
		1);
	Wand.DefinitionId = TEXT("wand.apprentice");
	Wand.SpellSlots.Init(NAME_None, 8);
	TArray<FGuid> Equipped;
	Equipped.Init(FGuid(), 4);
	int32 ActiveSlot = 0;

	FMatterFluxMagicEdit Assign;
	Assign.Type = EMatterFluxMagicEditType::AssignSpell;
	Assign.WandId = Wand.InstanceId;
	Assign.SpellId = Stack.SpellId;
	Assign.ToSpellSlot = 0;
	TestTrue(
		TEXT("Owned spell can be assigned"),
		FMatterFluxMagicInventoryRules::ApplyEdit(
			*Registry,
			Spells,
			Wands,
			Equipped,
			ActiveSlot,
			Assign,
			Error));
	TestEqual(TEXT("Assignment consumes exactly one copy"),
		Spells[0].Quantity,
		1);
	TestEqual(TEXT("Assignment updates the requested slot"),
		Wands[0].SpellSlots[0],
		FName(TEXT("spell.spark_bolt")));

	const TArray<FMatterFluxOwnedSpell> BeforeInvalidSpells = Spells;
	const TArray<FMatterFluxOwnedWand> BeforeInvalidWands = Wands;
	FMatterFluxMagicEdit Invalid = Assign;
	Invalid.SpellId = TEXT("spell.not_owned");
	Invalid.ToSpellSlot = 1;
	Error.Reset();
	TestFalse(
		TEXT("Unowned spell is rejected"),
		FMatterFluxMagicInventoryRules::ApplyEdit(
			*Registry,
			Spells,
			Wands,
			Equipped,
			ActiveSlot,
			Invalid,
			Error));
	TestEqual(TEXT("Rejected edit preserves stack count"),
		Spells[0].Quantity,
		BeforeInvalidSpells[0].Quantity);
	TestEqual(TEXT("Rejected edit preserves every wand slot"),
		Wands[0].SpellSlots,
		BeforeInvalidWands[0].SpellSlots);

	FMatterFluxMagicEdit Remove;
	Remove.Type = EMatterFluxMagicEditType::RemoveSpell;
	Remove.WandId = Wand.InstanceId;
	Remove.FromSpellSlot = 0;
	TestTrue(
		TEXT("Right-click style removal succeeds"),
		FMatterFluxMagicInventoryRules::ApplyEdit(
			*Registry,
			Spells,
			Wands,
			Equipped,
			ActiveSlot,
			Remove,
			Error));
	TestEqual(TEXT("Removal returns the spell copy"),
		Spells[0].Quantity,
		2);
	TestTrue(TEXT("Removal empties the deck slot"),
		Wands[0].SpellSlots[0].IsNone());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicLuaAndEvaluationTest,
	"MatterFlux.Magic.LuaDefinitionsProduceDeterministicCastPlan",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicLuaAndEvaluationTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(
		TEXT("Magic content pack loads"),
		Runtime.LoadContentPackFromSource(
			MatterFluxMagicTests::BasicPack,
			TEXT("MagicLuaAndEvaluation"),
			Error)))
	{
		AddError(Error);
		MatterFluxMagicTests::RestoreDefault(Runtime);
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry =
		Runtime.GetActiveRegistry();
	if (!TestTrue(TEXT("Magic registry exists"), Registry.IsValid()))
	{
		MatterFluxMagicTests::RestoreDefault(Runtime);
		return false;
	}
	TestEqual(TEXT("One spell registered"), Registry->Spells.Num(), 1);
	TestEqual(TEXT("One wand registered"), Registry->Wands.Num(), 1);

	FMatterFluxWandProgramState State;
	State.Mana = 100.0f;
	State.DeckCursor = 0;
	State.CastSerial = 7;
	TArray<FName> Slots;
	Slots.Init(NAME_None, 8);
	Slots[0] = TEXT("spell.spark_bolt");
	FMatterFluxWandCastPlan First;
	FMatterFluxWandCastPlan Second;
	FMatterFluxWandCastPlan Consecutive;
	TestTrue(
		TEXT("First cast evaluates"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			TEXT("wand.apprentice"),
			Slots,
			State,
			1337,
			First,
			Error));
	Error.Reset();
	TestTrue(
		TEXT("Second cast evaluates"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			TEXT("wand.apprentice"),
			Slots,
			State,
			1337,
			Second,
			Error));
	Error.Reset();
	TestTrue(
		TEXT("The same spell evaluates again after the deck wraps past empty capacity"),
		FMatterFluxWandProgram::Evaluate(
			*Registry,
			TEXT("wand.apprentice"),
			Slots,
			First.NextState,
			1338,
			Consecutive,
			Error));

	TestEqual(TEXT("One projectile emitted"), First.Projectiles.Num(), 1);
	if (First.Projectiles.Num() == 1)
	{
		TestEqual(TEXT("Projectile spell id"),
			First.Projectiles[0].SpellId,
			FName(TEXT("spell.spark_bolt")));
		TestEqual(TEXT("Projectile damage"),
			First.Projectiles[0].Damage,
			12.0f);
		TestEqual(TEXT("Projectile speed"),
			First.Projectiles[0].Speed,
			1200.0f);
	}
	TestEqual(TEXT("Mana is spent transactionally"),
		First.NextState.Mana,
		92.0f);
	TestEqual(TEXT("Deck cursor advances"),
		First.NextState.DeckCursor,
		1);
	TestEqual(TEXT("Cast serial advances"),
		First.NextState.CastSerial,
		8);
	TestEqual(TEXT("Cast delay includes spell modifier"),
		First.CastDelay,
		0.20f,
		0.001f);
	TestTrue(TEXT("Plans are deterministic"), First == Second);
	TestEqual(
		TEXT("Empty capacity slots do not suppress the next cast"),
		Consecutive.Projectiles.Num(),
		1);
	TestEqual(
		TEXT("Repeated casts continue advancing the logical deck"),
		Consecutive.NextState.DeckCursor,
		2);

	MatterFluxMagicTests::RestoreDefault(Runtime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicProjectileSpawnTest,
	"MatterFlux.Magic.CastPlanSpawnsReplicatedProjectiles",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicProjectileSpawnTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AActor* Avatar = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("Authority test avatar spawns"), Avatar))
	{
		return false;
	}

	FMatterFluxWandCastPlan EmptyPlan;
	TestFalse(
		TEXT("An empty plan is rejected"),
		UGA_CastWand::SpawnCastPlan(*Avatar, EmptyPlan, 77));

	FMatterFluxWandCastPlan InvalidNestedPlan;
	FMatterFluxMagicProjectilePlan& InvalidCarrier =
		InvalidNestedPlan.Projectiles.AddDefaulted_GetRef();
	InvalidCarrier.SpellId = TEXT("spell.valid_carrier");
	InvalidCarrier.Speed = 700.0f;
	InvalidCarrier.Lifetime = 1.0f;
	InvalidCarrier.Radius = 8.0f;
	FMatterFluxMagicProjectilePlan& InvalidPayload =
		InvalidCarrier.OnImpactProjectiles.AddDefaulted_GetRef();
	InvalidPayload.SpellId = TEXT("spell.invalid_payload");
	InvalidPayload.Speed = 0.0f;
	InvalidPayload.Lifetime = 1.0f;
	InvalidPayload.Radius = 8.0f;
	TestFalse(TEXT("Invalid nested payload rejects the whole plan"),
		UGA_CastWand::SpawnCastPlan(*Avatar, InvalidNestedPlan, 77));

	FMatterFluxWandCastPlan Plan;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FMatterFluxMagicProjectilePlan& Projectile =
			Plan.Projectiles.AddDefaulted_GetRef();
		Projectile.SpellId = TEXT("spell.spark_bolt");
		Projectile.Speed = 700.0f + 100.0f * Index;
		Projectile.Lifetime = 5.0f;
		Projectile.Radius = 8.0f;
		Projectile.SpawnAngleDegrees = Index == 0 ? -5.0f : 5.0f;
	}
	TestTrue(
		TEXT("A valid compiled plan spawns atomically"),
		UGA_CastWand::SpawnCastPlan(*Avatar, Plan, 77));

	TArray<AMatterFluxMagicProjectile*> Spawned;
	for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
	{
		Spawned.Add(*It);
	}
	TestEqual(TEXT("Both root projectiles spawn"), Spawned.Num(), 2);
	for (AMatterFluxMagicProjectile* Projectile : Spawned)
	{
		TestTrue(TEXT("Projectile replicates"), Projectile->GetIsReplicated());
		TestNotNull(
			TEXT("Projectile has movement"),
			Projectile->ProjectileMovement.Get());
	}
	return true;
}
