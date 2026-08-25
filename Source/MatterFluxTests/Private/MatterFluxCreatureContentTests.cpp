#include "Misc/AutomationTest.h"

#include "Creatures/MatterFluxCreatureActor.h"
#include "Creatures/MatterFluxCreatureBehaviorTree.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxContentTypes.h"
#include "Creatures/MatterFluxCreatureAIController.h"
#include "Creatures/MatterFluxCreatureCastProgram.h"
#include "EngineUtils.h"
#include "Magic/MatterFluxMagicProjectile.h"
#include "Misc/FileHelper.h"
#include "Tests/AutomationEditorCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCreatureContentRegistrationTest,
	"MatterFlux.Creatures.LuaRegistersDialogueShopAndServerAiProgram",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCreatureContentRegistrationTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	const FMatterFluxContentRegistryPtr Baseline = Runtime.GetActiveRegistry();
	const FString Source = TEXT(R"LUA(
content.set_manifest("creature.test", 1, 2)
content.register_item({ id="std.coin", name="金币", max_stack=999999, use_action="none", consume_count=0 })
content.register_spell({ id="std.default", name="火花弹", kind="projectile", mana_cost=1, damage=4, speed=600, lifetime=2, radius=8 })
content.register_quest({ id="std.init_quest.kill_enemy", description="击败敌人", category="objective", objective="never", target_count=1, activation_creature_spawns={ { creature_id="std.slime", marker_id="test.slime.0" }, { creature_id="std.slime", marker_id="test.slime.1" } } })
content.register_creature({ id="std.merchant_base", name="商人", faction="friendly", level="normal", ai="passive", health=100, width=70, height=160, move_speed=0, dialogue_id="dialogue.merchant", shop_id="std.template_merchant", color_r=0.2, color_g=0.8, color_b=0.3, color_a=1 })
content.register_creature({ id="std.slime", name="史莱姆", faction="hostile", level="elite", ai="skirmisher", health=15, width=80, height=90, density=0.35, move_speed=200, perception_range=700, attack_range=300, retreat_range=0, target_memory=5, patrol_turn=3, patrol_pause=1, attack_cooldown=2, attack_spell="std.default", attack_projectiles=2, attack_spread=10, drop_item="std.coin", drop_count=5000, color_r=0.4, color_g=0.9, color_b=0.2, color_a=1 })
content.register_dialogue({ id="dialogue.merchant", name="商人", start="hello", nodes={ { id="hello", text="这是一个商店。", options={ { text="看看商品", shop_id="std.template_merchant" }, { text="再见", close=true } } } } })
content.register_shop({ id="std.template_merchant", name="营地商店", offers={ { kind="item", product_id="std.coin", product_count=1, cost_item="std.coin", cost_count=100, limit=10 } } })
)LUA");

	FString Error;
	if (!TestTrue(TEXT("Creature content loads"),
		Runtime.LoadContentPackFromSource(Source, TEXT("CreatureContent"), Error)))
	{
		AddError(Error);
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (TestTrue(TEXT("Registry exists"), Registry.IsValid()))
	{
		TestEqual(TEXT("Creature count"), Registry->Creatures.Num(), 2);
		TestEqual(TEXT("Dialogue count"), Registry->Dialogues.Num(), 1);
		TestEqual(TEXT("Shop count"), Registry->Shops.Num(), 1);
		const FMatterFluxCreatureDefinition& Slime =
			Registry->Creatures.FindChecked(TEXT("std.slime"));
		TestTrue(TEXT("Lua selects server skirmisher interpreter"),
			Slime.AiMode == EMatterFluxCreatureAiMode::Skirmisher);
		TestFalse(TEXT("Legacy raw creature receives a compiled behavior tree"),
			Slime.BehaviorProgram.IsEmpty());
		const FMatterFluxQuestDefinition& Quest =
			Registry->Quests.FindChecked(TEXT("std.init_quest.kill_enemy"));
		TestEqual(TEXT("Lua preserves task-owned creature spawn count"),
			Quest.ActivationCreatureSpawns.Num(), 2);
		if (Quest.ActivationCreatureSpawns.Num() == 2)
		{
			TestEqual(TEXT("Quest spawn preserves creature id"),
				Quest.ActivationCreatureSpawns[0].CreatureId,
				FName(TEXT("std.slime")));
			TestEqual(TEXT("Quest spawn preserves marker id"),
				Quest.ActivationCreatureSpawns[1].MarkerId,
				FName(TEXT("test.slime.1")));
		}
		TestEqual(TEXT("Lua preserves drop count"), Slime.DropItemCount, 5000);
		TestEqual(TEXT("Lua preserves buoyancy density"),
			Slime.Density, 0.35f);
		TestEqual(TEXT("Lua preserves attack projectile count"),
			Slime.AttackProgram.ProjectileCount, 2);
		TestEqual(TEXT("Lua preserves attack spread"),
			Slime.AttackProgram.SpreadDegrees, 10.0f);
		const FMatterFluxDialogueDefinition& Dialogue =
			Registry->Dialogues.FindChecked(TEXT("dialogue.merchant"));
		TestEqual(TEXT("Dialogue options"), Dialogue.Nodes[0].Options.Num(), 2);
		const FMatterFluxShopDefinition& Shop =
			Registry->Shops.FindChecked(TEXT("std.template_merchant"));
		TestEqual(TEXT("Shop purchase limit"), Shop.Offers[0].PurchaseLimit, 10);
	}

	if (Baseline.IsValid())
	{
		Runtime.ReloadDefaultContentPack(Error);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxShopCategoryDslTest,
	"MatterFlux.Creatures.LuaShopCustomCategoryTabsCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxShopCategoryDslTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString EngineSource;
	if (!TestTrue(TEXT("Shop DSL source is readable"),
		FFileHelper::LoadFileToString(
			EngineSource,
			*Runtime.GetDefaultEngineConfigPath())))
	{
		return false;
	}
	const FString Source = EngineSource + TEXT(R"LUA(
content.set_manifest("shop.category.test", 1, 2)
item.define({ id="std.coin", name="金币", max_stack=999999 })
spell.define({ id="spell.test", name="测试飞弹", icon="paper/default", mana_cost=1 }, function(api)
	api.projectile({ damage=1, speed=100, lifetime=1, radius=4 })
end)
shop.define({ id="shop.category", name="分类商店" }, function(s)
	s.category({ id="projectile", name="投射物" })
	s.category({ id="utility", name="功能" })
	s.offer({ kind="spell", product_id="spell.test", product_count=1,
		cost_item="std.coin", cost_count=10, limit=3, category="projectile" })
end)
)LUA");

	FString Error;
	if (!TestTrue(TEXT("Shop category DSL compiles"),
		Runtime.LoadContentPackFromSource(Source, TEXT("ShopCategoryDsl"), Error)))
	{
		AddError(Error);
		Runtime.ReloadDefaultContentPack(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const FMatterFluxShopDefinition* Shop = Registry.IsValid()
		? Registry->Shops.Find(TEXT("shop.category"))
		: nullptr;
	if (TestNotNull(TEXT("Custom category shop exists"), Shop))
	{
		TestEqual(TEXT("Authored category order is preserved"),
			Shop->Categories.Num(), 2);
		if (Shop->Categories.Num() == 2)
		{
			TestEqual(TEXT("First category id reaches the registry"),
				Shop->Categories[0].Id, FName(TEXT("projectile")));
			TestEqual(TEXT("Second category label reaches the registry"),
				Shop->Categories[1].DisplayName, FString(TEXT("功能")));
		}
		if (Shop->Offers.Num() == 1)
		{
			TestEqual(TEXT("Offer retains its authored category"),
				Shop->Offers[0].CategoryId, FName(TEXT("projectile")));
		}
	}
	Runtime.ReloadDefaultContentPack(Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCreatureBehaviorTreeLuaTest,
	"MatterFlux.Creatures.LuaBehaviorTreeCompilesAndRejectsInvalidPrograms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCreatureBehaviorTreeLuaTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString EngineSource;
	if (!TestTrue(TEXT("Creature DSL source is readable"),
		FFileHelper::LoadFileToString(
			EngineSource,
			*Runtime.GetDefaultEngineConfigPath())))
	{
		return false;
	}
	const FString ValidSource = EngineSource + TEXT(R"LUA(
content.set_manifest("behavior.tree.test", 1, 2)
creature.define({
    id="enemy.tree", name="Tree Enemy", faction="hostile", level="normal",
    health=20, width=60, height=120, wait_for_first_sight=true
}, function(ai)
    ai.tree({
      sight={ range=900, memory_seconds=5 },
      locomotion={ speed=250 },
      root=ai.selector({
        ai.sequence({
            ai.condition("target_too_close", { distance=150 }),
            ai.action("retreat")
        }),
        ai.sequence({
            ai.condition("target_in_attack_range", { distance=600 }),
            ai.condition("attack_ready"),
            ai.action("attack", { cooldown=2 })
        }),
        ai.sequence({
            ai.condition("has_target"),
            ai.action("chase")
        }),
        ai.action("patrol", { turn_seconds=3, pause_seconds=0.5 })
      })
    })
end)
)LUA");

	FString Error;
	if (!TestTrue(TEXT("Declarative Lua behavior tree loads"),
		Runtime.LoadContentPackFromSource(
			ValidSource, TEXT("ValidCreatureBehaviorTree"), Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (TestTrue(TEXT("Behavior registry exists"), Registry.IsValid()))
	{
		const FMatterFluxCreatureDefinition& Creature =
			Registry->Creatures.FindChecked(TEXT("enemy.tree"));
		TestEqual(TEXT("Lua selects behavior-tree mode"),
			Creature.AiMode, EMatterFluxCreatureAiMode::BehaviorTree);
		TestEqual(TEXT("Compiled tree has four selector branches"),
			Creature.BehaviorProgram
				.Nodes[Creature.BehaviorProgram.RootNodeIndex]
				.Children.Num(),
			4);
		TestEqual(TEXT("Sight range is compiled from the tree"),
			Creature.PerceptionRange, 900.0f);
		TestEqual(TEXT("Movement speed is compiled from the tree"),
			Creature.MoveSpeed, 250.0f);
		TestTrue(TEXT("First-sight waiting is compiled from creature metadata"),
			Creature.bWaitForFirstSight);
		TestEqual(TEXT("Attack distance is owned by its condition"),
			Creature.AttackRange, 600.0f);
		TestEqual(TEXT("Attack cooldown is owned by its action"),
			Creature.AttackCooldown, 2.0f);
	}

	const FMatterFluxContentRegistryPtr Baseline = Runtime.GetActiveRegistry();
	const FString InvalidSource = EngineSource + TEXT(R"LUA(
content.set_manifest("behavior.tree.invalid", 1, 2)
creature.define({
    id="enemy.invalid", name="Invalid", faction="hostile", level="normal"
}, function(ai)
    ai.tree({ root=ai.sequence({
        ai.action("attack", { cooldown=2 }),
        ai.condition("has_target")
    }) })
end)
)LUA");
	Error.Reset();
	TestFalse(TEXT("Action-before-condition tree is rejected"),
		Runtime.LoadContentPackFromSource(
			InvalidSource, TEXT("InvalidCreatureBehaviorTree"), Error));
	TestTrue(TEXT("Tree shape failure is explicit"),
		Error.Contains(TEXT("sequence")));
	TestTrue(TEXT("Rejected tree keeps the active registry"),
		Runtime.GetActiveRegistry() == Baseline);

	const FString DeprecatedConfigureSource = EngineSource + TEXT(R"LUA(
content.set_manifest("behavior.tree.deprecated", 1, 2)
creature.define({
    id="enemy.deprecated", name="Deprecated", faction="hostile", level="normal"
}, function(ai)
    ai.configure({ perception_range=900 })
    ai.tree({ root=ai.action("passive") })
end)
)LUA");
	Error.Reset();
	TestFalse(TEXT("Deprecated ai.configure is no longer callable"),
		Runtime.LoadContentPackFromSource(
			DeprecatedConfigureSource,
			TEXT("DeprecatedCreatureConfigure"),
			Error));
	TestTrue(TEXT("Deprecated interface failure names configure"),
		Error.Contains(TEXT("configure")));
	TestTrue(TEXT("Deprecated interface failure keeps the active registry"),
		Runtime.GetActiveRegistry() == Baseline);
	Runtime.ReloadDefaultContentPack(Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCreatureCastProgramTest,
	"MatterFlux.Creatures.CastProgramBuildsDeterministicTimedVolleys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCreatureCastProgramTest::RunTest(const FString& Parameters)
{
	FMatterFluxCreatureCastProgramDefinition Attack;
	Attack.ProjectileCount = 2;
	Attack.SpreadDegrees = 10.0f;
	TArray<FMatterFluxCreatureCastShot> FirstAttack;
	TArray<FMatterFluxCreatureCastShot> SecondAttack;
	FString Error;
	TestTrue(TEXT("Double attack plan builds"),
		FMatterFluxCreatureCastPlanner::Build(
			Attack, FVector::ForwardVector, 41, FirstAttack, Error));
	TestTrue(TEXT("Same attack plan builds again"),
		FMatterFluxCreatureCastPlanner::Build(
			Attack, FVector::ForwardVector, 41, SecondAttack, Error));
	TestEqual(TEXT("Double attack emits two shots"), FirstAttack.Num(), 2);
	TestTrue(TEXT("Same seed produces identical shots"),
		FirstAttack == SecondAttack);
	for (const FMatterFluxCreatureCastShot& Shot : FirstAttack)
	{
		const float Angle = FMath::RadiansToDegrees(
			FMath::Atan2(Shot.Direction.Y, Shot.Direction.X));
		TestTrue(TEXT("Attack spread remains inside PaperMagic range"),
			FMath::Abs(Angle) <= 10.0f + UE_KINDA_SMALL_NUMBER);
		TestEqual(TEXT("Attack volley is immediate"), Shot.DelaySeconds, 0.0f);
	}

	FMatterFluxCreatureCastProgramDefinition Skill;
	Skill.ProjectileCount = 12;
	Skill.ProjectileInterval = 0.2f;
	Skill.bRadial = true;
	TArray<FMatterFluxCreatureCastShot> SkillShots;
	TestTrue(TEXT("Timed radial skill plan builds"),
		FMatterFluxCreatureCastPlanner::Build(
			Skill, FVector::ForwardVector, 99, SkillShots, Error));
	TestEqual(TEXT("Radial skill emits twelve unique directions"),
		SkillShots.Num(), 12);
	for (int32 Index = 0; Index < SkillShots.Num(); ++Index)
	{
		TestEqual(TEXT("Skill delay follows authored cadence"),
			SkillShots[Index].DelaySeconds, 0.2f * Index);
		TestTrue(TEXT("Radial shot direction is normalized"),
			SkillShots[Index].Direction.IsNormalized());
		if (Index > 0)
		{
			TestFalse(TEXT("Adjacent radial directions are distinct"),
				SkillShots[Index].Direction.Equals(
					SkillShots[Index - 1].Direction, UE_KINDA_SMALL_NUMBER));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCreatureTimedCastRuntimeTest,
	"MatterFlux.Creatures.TimedCastRunsThroughActorTimerLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCreatureTimedCastRuntimeTest::RunTest(
	const FString& Parameters)
{
	FString Error;
	if (!TestTrue(TEXT("Default content pack reloads"),
		IMatterFluxScriptRuntime::Get().ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxCreatureActor* Boss = World
		? World->SpawnActor<AMatterFluxCreatureActor>(
			FVector::ZeroVector, FRotator::ZeroRotator) : nullptr;
	AActor* Target = World
		? World->SpawnActor<AActor>(
			FVector(1000.0f, 0.0f, 0.0f), FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("Boss actor spawns"), Boss)
		|| !TestNotNull(TEXT("Target actor spawns"), Target))
	{
		return false;
	}
	// Prime the editor world's timer manager before scheduling. In normal play
	// the engine has already ticked it earlier in the frame.
	++GFrameCounter;
	World->Tick(LEVELTICK_All, 0.0f);
	Boss->InitializeCreature(TEXT("std.test_boss"));
	TestTrue(TEXT("Boss begins its configured skill"),
		Boss->CastConfiguredSpellAuthority(*Target, true, 77));

	const auto CountOwnedProjectiles = [World, Boss]()
	{
		int32 Count = 0;
		for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetOwner() == Boss) ++Count;
		}
		return Count;
	};
	const auto AdvanceWorld = [World](const float DeltaSeconds)
	{
		++GFrameCounter;
		World->Tick(LEVELTICK_All, DeltaSeconds);
	};
	TestEqual(TEXT("First ring projectile is immediate"),
		CountOwnedProjectiles(), 1);
	AdvanceWorld(0.19f);
	TestEqual(TEXT("Second projectile does not fire early"),
		CountOwnedProjectiles(), 1);
	AdvanceWorld(0.02f);
	TestEqual(TEXT("Second projectile fires at the authored cadence"),
		CountOwnedProjectiles(), 2);
	for (int32 Step = 0; Step < 20; ++Step)
	{
		AdvanceWorld(0.10f);
	}
	TestEqual(TEXT("All twelve ring projectiles are emitted"),
		CountOwnedProjectiles(), 12);
	TestTrue(TEXT("Boss remains skill-locked during recovery"),
		Boss->IsCastSequenceActive());
	for (int32 Step = 0; Step < 13; ++Step)
	{
		AdvanceWorld(0.10f);
	}
	TestFalse(TEXT("Boss exits the cast sequence after recovery"),
		Boss->IsCastSequenceActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCreatureAIDecisionTest,
	"MatterFlux.Creatures.LuaAiProgramChoosesStableServerStates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCreatureAIDecisionTest::RunTest(const FString& Parameters)
{
	FMatterFluxCreatureBehaviorProgramDefinition Program;
	const auto AddCondition = [&Program](
		const EMatterFluxCreatureBehaviorCondition Condition)
	{
		FMatterFluxCreatureBehaviorNodeDefinition& Node =
			Program.Nodes.AddDefaulted_GetRef();
		Node.Kind = EMatterFluxCreatureBehaviorNodeKind::Condition;
		Node.Condition = Condition;
		return Program.Nodes.Num() - 1;
	};
	const auto AddAction = [&Program](
		const EMatterFluxCreatureBehaviorAction Action)
	{
		FMatterFluxCreatureBehaviorNodeDefinition& Node =
			Program.Nodes.AddDefaulted_GetRef();
		Node.Kind = EMatterFluxCreatureBehaviorNodeKind::Action;
		Node.Action = Action;
		return Program.Nodes.Num() - 1;
	};
	const auto AddComposite = [&Program](
		const EMatterFluxCreatureBehaviorNodeKind Kind,
		TArray<int32>&& Children)
	{
		FMatterFluxCreatureBehaviorNodeDefinition& Node =
			Program.Nodes.AddDefaulted_GetRef();
		Node.Kind = Kind;
		Node.Children = MoveTemp(Children);
		return Program.Nodes.Num() - 1;
	};
	TArray<int32> Branches;
	Branches.Add(AddComposite(
		EMatterFluxCreatureBehaviorNodeKind::Sequence,
		{ AddCondition(EMatterFluxCreatureBehaviorCondition::TargetTooClose),
			AddAction(EMatterFluxCreatureBehaviorAction::Retreat) }));
	Branches.Add(AddComposite(
		EMatterFluxCreatureBehaviorNodeKind::Sequence,
		{ AddCondition(EMatterFluxCreatureBehaviorCondition::TargetInAttackRange),
			AddCondition(EMatterFluxCreatureBehaviorCondition::SkillReady),
			AddAction(EMatterFluxCreatureBehaviorAction::Skill) }));
	Branches.Add(AddComposite(
		EMatterFluxCreatureBehaviorNodeKind::Sequence,
		{ AddCondition(EMatterFluxCreatureBehaviorCondition::TargetInAttackRange),
			AddCondition(EMatterFluxCreatureBehaviorCondition::AttackReady),
			AddAction(EMatterFluxCreatureBehaviorAction::Attack) }));
	Branches.Add(AddComposite(
		EMatterFluxCreatureBehaviorNodeKind::Sequence,
		{ AddCondition(EMatterFluxCreatureBehaviorCondition::HasTarget),
			AddAction(EMatterFluxCreatureBehaviorAction::Chase) }));
	Branches.Add(AddAction(EMatterFluxCreatureBehaviorAction::Patrol));
	Program.RootNodeIndex = AddComposite(
		EMatterFluxCreatureBehaviorNodeKind::Selector,
		MoveTemp(Branches));

	FMatterFluxCreatureAIDecisionContext Context;
	EMatterFluxCreatureRuntimeState State =
		EMatterFluxCreatureRuntimeState::Passive;
	FString Error;
	TestTrue(TEXT("Behavior tree evaluates without a target"),
		FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
			Program, Context, State, Error));
	TestEqual(TEXT("Fallback action patrols"),
		State, EMatterFluxCreatureRuntimeState::Patrol);
	Context.bHasVisibleTarget = true;
	Context.TargetDistance = 100.0f;
	Context.RetreatRange = 150.0f;
	Context.AttackRange = 600.0f;
	Context.bAttackReady = true;
	TestTrue(TEXT("Close-target tree evaluates"),
		FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
			Program, Context, State, Error));
	TestEqual(TEXT("Selector gives retreat the highest priority"),
		State, EMatterFluxCreatureRuntimeState::Retreat);
	Context.TargetDistance = 400.0f;
	TestTrue(TEXT("Attack tree evaluates"),
		FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
			Program, Context, State, Error));
	TestEqual(TEXT("Ready ranged target selects attack"),
		State, EMatterFluxCreatureRuntimeState::Attack);
	Context.bAttackReady = false;
	TestTrue(TEXT("Cooling-down attack tree evaluates"),
		FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
			Program, Context, State, Error));
	TestEqual(TEXT("Cooling-down ranged target falls through to chase"),
		State, EMatterFluxCreatureRuntimeState::Chase);
	TestTrue(TEXT("Chase input holds inside the attack band"),
		MatterFlux::Creatures::ShouldHoldCombatPosition(
			State,
			Context.bHasVisibleTarget,
			Context.TargetDistance,
			Context.AttackRange));
	TestFalse(TEXT("Remembered-only targets are still chased"),
		MatterFlux::Creatures::ShouldHoldCombatPosition(
			State,
			false,
			Context.TargetDistance,
			Context.AttackRange));
	TestTrue(TEXT("Configured creature waits before its first visible target"),
		MatterFlux::Creatures::ShouldWaitForFirstSight(true, false, false));
	TestFalse(TEXT("First visible target wakes the configured creature"),
		MatterFlux::Creatures::ShouldWaitForFirstSight(true, false, true));
	TestFalse(TEXT("Creature stays awake after losing its first target"),
		MatterFlux::Creatures::ShouldWaitForFirstSight(true, true, false));
	TestFalse(TEXT("Ordinary creatures keep their authored patrol behavior"),
		MatterFlux::Creatures::ShouldWaitForFirstSight(false, false, false));
	Context.TargetDistance = 100.0f;
	TestTrue(TEXT("Too-close cooldown tree evaluates"),
		FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
			Program, Context, State, Error));
	TestEqual(TEXT("Too-close target still selects retreat"),
		State, EMatterFluxCreatureRuntimeState::Retreat);
	TestFalse(TEXT("Retreat is never replaced by combat hold"),
		MatterFlux::Creatures::ShouldHoldCombatPosition(
			State,
			Context.bHasVisibleTarget,
			Context.TargetDistance,
			Context.AttackRange));
	Context.TargetDistance = 400.0f;
	Context.bSkillReady = true;
	TestTrue(TEXT("Skill tree evaluates"),
		FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
			Program, Context, State, Error));
	TestEqual(TEXT("Authored selector puts skill before attack"),
		State, EMatterFluxCreatureRuntimeState::Skill);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPaperMagicCreatureCatalogTest,
	"MatterFlux.Creatures.DefaultPackMigratesPaperMagicCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPaperMagicCreatureCatalogTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default pack reloads"),
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
	TestEqual(TEXT("All ten PaperMagic quest scripts are represented"),
		Registry->Quests.Num(), 10);
	for (const FName Id : {
		FName(TEXT("std.merchant_base")),
		FName(TEXT("std.patrol")),
		FName(TEXT("std.slime")),
		FName(TEXT("std.elite_patrol")),
		FName(TEXT("std.test_boss")) })
	{
		TestTrue(*FString::Printf(TEXT("Creature %s migrated"), *Id.ToString()),
			Registry->Creatures.Contains(Id));
	}
	const FMatterFluxShopDefinition& Shop =
		Registry->Shops.FindChecked(TEXT("std.template_merchant"));
	const auto ExpectedCategoryForSpellKind = [](
		const EMatterFluxSpellKind Kind) -> FName
	{
		switch (Kind)
		{
		case EMatterFluxSpellKind::Projectile:
			return TEXT("projectile");
		case EMatterFluxSpellKind::Modifier:
			return TEXT("modifier");
		case EMatterFluxSpellKind::Multicast:
			return TEXT("multicast");
		case EMatterFluxSpellKind::Trigger:
			return TEXT("trigger");
		case EMatterFluxSpellKind::TriggerModifier:
			return TEXT("trigger_modifier");
		case EMatterFluxSpellKind::Jump:
			return TEXT("jump");
		default:
			return NAME_None;
		}
	};
	TSet<FName> OfferedSpellIds;
	for (const FMatterFluxShopOfferDefinition& Offer : Shop.Offers)
	{
		if (Offer.ProductKind == EMatterFluxShopProductKind::Spell)
		{
			OfferedSpellIds.Add(Offer.ProductId);
			const FMatterFluxSpellDefinition& Spell =
				Registry->Spells.FindChecked(Offer.ProductId);
			TestEqual(*FString::Printf(
				TEXT("Story merchant categorizes %s by spell kind"),
				*Offer.ProductId.ToString()),
				Offer.CategoryId,
				ExpectedCategoryForSpellKind(Spell.Kind));
		}
	}
	TestEqual(TEXT("Story merchant carries every registered spell exactly once"),
		OfferedSpellIds.Num(), Registry->Spells.Num());
	for (const TPair<FName, FMatterFluxSpellDefinition>& Pair : Registry->Spells)
	{
		TestTrue(*FString::Printf(
			TEXT("Story merchant sells registered spell %s"),
			*Pair.Key.ToString()),
			OfferedSpellIds.Contains(Pair.Key));
	}
	TestEqual(TEXT("Story merchant keeps one supply and one wand offer"),
		Shop.Offers.Num(), Registry->Spells.Num() + 2);
	TestEqual(TEXT("PaperMagic merchant preserves the advanced wand product id"),
		Shop.Offers.Last().ProductId, FName(TEXT("std.advanced_wand")));
	const FMatterFluxQuestDefinition& TutorialCombatQuest =
		Registry->Quests.FindChecked(TEXT("std.init_quest.kill_enemy"));
	TestEqual(TEXT("Tutorial task owns exactly three creature spawns"),
		TutorialCombatQuest.ActivationCreatureSpawns.Num(), 3);
	if (TutorialCombatQuest.ActivationCreatureSpawns.Num() == 3)
	{
		TestEqual(TEXT("Tutorial first slime uses its authored marker"),
			TutorialCombatQuest.ActivationCreatureSpawns[0].MarkerId,
			FName(TEXT("creature.std.slime.0")));
		TestEqual(TEXT("Tutorial second slime uses its authored marker"),
			TutorialCombatQuest.ActivationCreatureSpawns[1].MarkerId,
			FName(TEXT("creature.std.slime.1")));
		TestEqual(TEXT("Tutorial elite uses its authored marker"),
			TutorialCombatQuest.ActivationCreatureSpawns[2].MarkerId,
			FName(TEXT("creature.std.elite_patrol.0")));
	}
	const FMatterFluxCreatureDefinition& Slime =
		Registry->Creatures.FindChecked(TEXT("std.slime"));
	TestEqual(TEXT("Tutorial slimes retain their existing health"),
		Slime.MaxHealth, 15.0f);
	TestEqual(TEXT("Story slimes are twice the original width"),
		Slime.Width, 180.0f);
	TestEqual(TEXT("Story slimes are twice the original height"),
		Slime.Height, 170.0f);
	TestEqual(TEXT("Tutorial elite uses the reduced opening-wave health"),
		Registry->Creatures.FindChecked(TEXT("std.elite_patrol")).MaxHealth,
		25.0f);
	const FMatterFluxQuestDefinition& BossQuest =
		Registry->Quests.FindChecked(TEXT("std.side_kill_boss"));
	TestEqual(TEXT("Boss task owns both boss spawns"),
		BossQuest.ActivationCreatureSpawns.Num(), 2);
	if (BossQuest.ActivationCreatureSpawns.Num() == 2)
	{
		TestEqual(TEXT("First boss uses its authored marker"),
			BossQuest.ActivationCreatureSpawns[0].MarkerId,
			FName(TEXT("creature.std.test_boss.0")));
		TestEqual(TEXT("Second boss uses its authored marker"),
			BossQuest.ActivationCreatureSpawns[1].MarkerId,
			FName(TEXT("creature.std.test_boss.1")));
	}
	const FMatterFluxCustomMapDefinition& StoryMap =
		Registry->CustomMaps.FindChecked(TEXT("story.paper_magic"));
	TestEqual(TEXT("Story map owns one non-quest occupant"),
		StoryMap.InitialCreatureSpawns.Num(), 1);
	if (StoryMap.InitialCreatureSpawns.Num() == 1)
	{
		TestEqual(TEXT("Story map initializes the merchant"),
			StoryMap.InitialCreatureSpawns[0].CreatureId,
			FName(TEXT("std.merchant_base")));
		TestEqual(TEXT("Merchant uses the authored map marker"),
			StoryMap.InitialCreatureSpawns[0].MarkerId,
			FName(TEXT("creature.std.merchant_base.0")));
	}
	const FMatterFluxCreatureDefinition& Boss =
		Registry->Creatures.FindChecked(TEXT("std.test_boss"));
	TestEqual(TEXT("Boss uses the enlarged authored width"),
		Boss.Width, 150.0f);
	TestEqual(TEXT("Boss uses the enlarged authored height"),
		Boss.Height, 300.0f);
	TestTrue(TEXT("Boss waits for its first sight of the player"),
		Boss.bWaitForFirstSight);
	TestEqual(TEXT("Boss attack keeps PaperMagic double cast"),
		Boss.AttackProgram.ProjectileCount, 2);
	TestEqual(TEXT("Boss attack keeps PaperMagic random spread"),
		Boss.AttackProgram.SpreadDegrees, 10.0f);
	TestEqual(TEXT("Boss skill keeps twelve-way ring"),
		Boss.SkillProgram.ProjectileCount, 12);
	TestEqual(TEXT("Boss ring keeps 0.2 second cadence"),
		Boss.SkillProgram.ProjectileInterval, 0.2f);
	TestEqual(TEXT("Boss skill keeps one second recovery"),
		Boss.SkillProgram.RecoverySeconds, 1.0f);
	TestEqual(TEXT("Boss skill keeps horizontal launch"),
		Boss.SkillProgram.HorizontalImpulse, 500.0f);
	TestEqual(TEXT("Boss skill keeps vertical launch"),
		Boss.SkillProgram.VerticalImpulse, 1500.0f);
	TestTrue(TEXT("Boss skill projectiles override their color"),
		Boss.SkillProgram.bOverrideColor);
	TestEqual(TEXT("Boss skill projectiles are red"),
		Boss.SkillProgram.Color, FLinearColor::Red);
	return true;
}

#endif
