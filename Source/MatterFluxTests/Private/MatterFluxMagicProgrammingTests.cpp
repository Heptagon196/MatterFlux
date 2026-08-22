#include "IMatterFluxScriptRuntime.h"
#include "Components/SphereComponent.h"
#include "EngineUtils.h"
#include "GAS/GA_CastWand.h"
#include "Game/MatterFluxCharacter.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Magic/MatterFluxWandProgram.h"
#include "Magic/MatterFluxSpellProgramLayout.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
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
		TestNotNull(
			*FString::Printf(TEXT("PaperMagic spell %s is registered"),
				*SpellId.ToString()),
			Registry->Spells.Find(SpellId));
	}
	TestEqual(TEXT("MatterFlux and PaperMagic libraries coexist"),
		Registry->Spells.Num(), 18);

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
		TestEqual(TEXT("Jump emits one world effect"),
			JumpPlan.WorldEffects.Num(), 1);
		TestEqual(TEXT("Jump world effect type"),
			JumpPlan.WorldEffects[0].Type,
			EMatterFluxMagicWorldEffectType::Jump);
		TestEqual(TEXT("Jump impulse"),
			JumpPlan.WorldEffects[0].VerticalImpulse, 600.0f);
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
	FMatterFluxMagicWorldEffectPlan& Jump =
		JumpPlan.WorldEffects.AddDefaulted_GetRef();
	Jump.SpellId = TEXT("std.jump");
	Jump.Type = EMatterFluxMagicWorldEffectType::Jump;
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
	FMatterFluxMagicWandWorldEffectsTest,
	"MatterFlux.Magic.CutAndFlameCompileAsWandWorldEffects",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicWandWorldEffectsTest::RunTest(
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
	if (!TestNotNull(TEXT("Cutting starter wand exists"), CuttingWand)
		|| !TestNotNull(TEXT("Flame starter wand exists"), FlameWand))
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
	TestEqual(TEXT("Cut emits no projectile"),
		CutPlan.Projectiles.Num(), 0);
	TestEqual(TEXT("Cut emits one world effect"),
		CutPlan.WorldEffects.Num(), 1);
	if (CutPlan.WorldEffects.Num() == 1)
	{
		TestEqual(TEXT("Cut effect type is preserved"),
			CutPlan.WorldEffects[0].Type,
			EMatterFluxMagicWorldEffectType::Cut);
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
	TestEqual(TEXT("Flame emits one world effect"),
		FlamePlan.WorldEffects.Num(), 1);
	if (FlamePlan.WorldEffects.Num() == 1)
	{
		TestEqual(TEXT("Flame effect type is preserved"),
			FlamePlan.WorldEffects[0].Type,
			EMatterFluxMagicWorldEffectType::Flame);
		TestEqual(TEXT("Flame material comes from Lua"),
			FlamePlan.WorldEffects[0].Material,
			FName(TEXT("fire")));
	}
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
