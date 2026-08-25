#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Game/MatterFluxPlayerState.h"
#include "GAS/GA_CastWand.h"
#include "GAS/MatterFluxPlayerAttributeSet.h"
#include "IMatterFluxScriptRuntime.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "Save/MatterFluxSaveTypes.h"
#include "Tests/AutomationEditorCommon.h"
#include "UI/MatterFluxShellWidget.h"

namespace MatterFluxProgressionTests
{
	FMatterFluxContentRegistry BuildRegistry()
	{
		FMatterFluxContentRegistry Registry;
		FMatterFluxItemDefinition Coin;
		Coin.Id = TEXT("item.coin");
		Coin.DisplayName = TEXT("Coin");
		Coin.MaxStack = 999;
		Registry.Items.Add(Coin.Id, Coin);

		FMatterFluxItemDefinition Potion;
		Potion.Id = TEXT("item.potion");
		Potion.DisplayName = TEXT("Potion");
		Potion.Category = EMatterFluxItemCategory::Consumable;
		Potion.MaxStack = 10;
		Potion.StarterCount = 2;
		Potion.UseAction = EMatterFluxItemUseAction::RestoreHealth;
		Potion.UseMagnitude = 30.0f;
		Potion.ConsumeCount = 1;
		Registry.Items.Add(Potion.Id, Potion);

		FMatterFluxSpellDefinition Spell;
		Spell.Id = TEXT("spell.bolt");
		Spell.DisplayName = TEXT("Bolt");
		Registry.Spells.Add(Spell.Id, Spell);
		FMatterFluxSpellDefinition RewardSpell;
		RewardSpell.Id = TEXT("spell.jump");
		RewardSpell.DisplayName = TEXT("Jump");
		Registry.Spells.Add(RewardSpell.Id, RewardSpell);
		FMatterFluxWandDefinition Wand;
		Wand.Id = TEXT("wand.test");
		Wand.DisplayName = TEXT("Wand");
		Registry.Wands.Add(Wand.Id, Wand);

		FMatterFluxQuestDefinition EquipWand;
		EquipWand.Id = TEXT("quest.equip_wand");
		EquipWand.Description = TEXT("Equip wand");
		EquipWand.Category = EMatterFluxQuestCategory::Objective;
		EquipWand.Objective = EMatterFluxQuestObjectiveKind::EquipWand;
		Registry.Quests.Add(EquipWand.Id, EquipWand);

		FMatterFluxQuestDefinition EquipSpell;
		EquipSpell.Id = TEXT("quest.equip_spell");
		EquipSpell.Description = TEXT("Equip spell");
		EquipSpell.Category = EMatterFluxQuestCategory::Objective;
		EquipSpell.Objective = EMatterFluxQuestObjectiveKind::EquipSpell;
		EquipSpell.Prerequisites.Add(EquipWand.Id);
		Registry.Quests.Add(EquipSpell.Id, EquipSpell);

		FMatterFluxQuestDefinition Kill;
		Kill.Id = TEXT("quest.kill");
		Kill.Description = TEXT("Kill enemies");
		Kill.Category = EMatterFluxQuestCategory::Objective;
		Kill.Objective = EMatterFluxQuestObjectiveKind::KillEnemies;
		Kill.TargetCount = 3;
		Kill.Prerequisites.Add(EquipSpell.Id);
		FMatterFluxQuestRewardDefinition JumpReward;
		JumpReward.Kind = EMatterFluxQuestRewardKind::Spell;
		JumpReward.ContentId = RewardSpell.Id;
		Kill.CompletionRewards.Add(JumpReward);
		Registry.Quests.Add(Kill.Id, Kill);

		FMatterFluxQuestDefinition Tutorial;
		Tutorial.Id = TEXT("quest.tutorial");
		Tutorial.DisplayName = TEXT("Tutorial");
		Tutorial.Description = TEXT("Learn");
		Tutorial.Category = EMatterFluxQuestCategory::Main;
		Tutorial.Objective = EMatterFluxQuestObjectiveKind::CompleteChildren;
		Tutorial.bStarter = true;
		Tutorial.bFocusOnActivate = true;
		Tutorial.Subquests = { EquipWand.Id, EquipSpell.Id, Kill.Id };
		Registry.Quests.Add(Tutorial.Id, Tutorial);

		FMatterFluxQuestDefinition Spend;
		Spend.Id = TEXT("quest.spend");
		Spend.DisplayName = TEXT("Spend");
		Spend.Description = TEXT("Spend coins");
		Spend.Category = EMatterFluxQuestCategory::Main;
		Spend.Objective = EMatterFluxQuestObjectiveKind::SpendItem;
		Spend.TargetId = Coin.Id;
		Spend.TargetCount = 5;
		Spend.bFocusOnActivate = true;
		Spend.Prerequisites.Add(Tutorial.Id);
		Registry.Quests.Add(Spend.Id, Spend);
		return Registry;
	}

	const FMatterFluxQuestState* FindQuest(
		const TArray<FMatterFluxQuestState>& Quests,
		const FName Id)
	{
		return Quests.FindByPredicate([Id](const FMatterFluxQuestState& State)
		{
			return State.QuestId == Id;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFreeModeProgressionTest,
	"MatterFlux.Progression.FreeModeStartsWithoutStoryQuests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxFreeModeProgressionTest::RunTest(const FString& Parameters)
{
	using namespace MatterFluxProgressionTests;
	(void)Parameters;
	const FMatterFluxContentRegistry Registry = BuildRegistry();
	FMatterFluxProgressionEvaluationContext Context;
	Context.EquippedWands.SetNum(4);
	Context.EquippedSpellsBySlot.SetNum(4);
	TArray<FMatterFluxItemStack> Items;
	TArray<FMatterFluxQuestState> Quests;
	FName SelectedQuest;
	FMatterFluxProgressionEffects Effects;
	FString Error;
	if (!TestTrue(TEXT("Free-mode progression builds"),
		FMatterFluxProgressionRules::BuildFreeModeState(
			Registry, Context, Items, Quests, SelectedQuest, Effects, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Free mode keeps starter items"), Items.Num(), 1);
	TestTrue(TEXT("Free mode has no story quests"), Quests.IsEmpty());
	TestTrue(TEXT("Free mode selects no story quest"), SelectedQuest.IsNone());
	TestTrue(TEXT("Free mode grants no quest activation rewards"),
		Effects.Rewards.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProgressionQuestFlowTest,
	"MatterFlux.Progression.ReferenceQuestFlowIsDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxProgressionQuestFlowTest::RunTest(const FString& Parameters)
{
	using namespace MatterFluxProgressionTests;
	const FMatterFluxContentRegistry Registry = BuildRegistry();
	FMatterFluxProgressionEvaluationContext Context;
	Context.EquippedWands.SetNum(4);
	Context.EquippedSpellsBySlot.SetNum(4);
	TArray<FMatterFluxItemStack> Items;
	TArray<FMatterFluxQuestState> Quests;
	FName SelectedQuest;
	FMatterFluxProgressionEffects Effects;
	FString Error;
	if (!TestTrue(TEXT("Starter progression builds"),
		FMatterFluxProgressionRules::BuildStarterState(
			Registry, Context, Items, Quests, SelectedQuest, Effects, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Starter potion stack exists"), Items.Num(), 1);
	TestEqual(TEXT("Starter potion quantity"), Items[0].Quantity, 2);
	TestEqual(TEXT("Tutorial is selected"), SelectedQuest,
		FName(TEXT("quest.tutorial")));
	TestEqual(TEXT("Equip wand starts active"),
		FindQuest(Quests, TEXT("quest.equip_wand"))->Status,
		EMatterFluxQuestRuntimeStatus::Active);
	TestEqual(TEXT("Equip spell is hidden behind prerequisite"),
		FindQuest(Quests, TEXT("quest.equip_spell"))->Status,
		EMatterFluxQuestRuntimeStatus::Hidden);

	Context.EquippedWands[0] = TEXT("wand.test");
	FMatterFluxQuestEvent Refresh;
	Refresh.Type = EMatterFluxQuestEventType::Refresh;
	TestTrue(TEXT("Equipping a wand advances quests"),
		FMatterFluxProgressionRules::NotifyEvent(
			Registry, Context, Items, Quests, SelectedQuest,
			Refresh, Effects, Error));
	TestEqual(TEXT("Equip wand completes"),
		FindQuest(Quests, TEXT("quest.equip_wand"))->Status,
		EMatterFluxQuestRuntimeStatus::Completed);
	TestEqual(TEXT("Equip spell unlocks"),
		FindQuest(Quests, TEXT("quest.equip_spell"))->Status,
		EMatterFluxQuestRuntimeStatus::Active);

	Context.EquippedSpellsBySlot[0].Add(TEXT("spell.bolt"));
	TestTrue(TEXT("Equipping a spell advances quests"),
		FMatterFluxProgressionRules::NotifyEvent(
			Registry, Context, Items, Quests, SelectedQuest,
			Refresh, Effects, Error));
	TestEqual(TEXT("Equip spell completes"),
		FindQuest(Quests, TEXT("quest.equip_spell"))->Status,
		EMatterFluxQuestRuntimeStatus::Completed);

	FMatterFluxQuestEvent Kill;
	Kill.Type = EMatterFluxQuestEventType::EnemyKilled;
	Kill.Amount = 2;
	TestTrue(TEXT("First kill batch applies"),
		FMatterFluxProgressionRules::NotifyEvent(
			Registry, Context, Items, Quests, SelectedQuest,
			Kill, Effects, Error));
	TestEqual(TEXT("Kill progress is two"),
		FindQuest(Quests, TEXT("quest.kill"))->Progress, 2);
	Kill.Amount = 1;
	TestTrue(TEXT("Final kill applies"),
		FMatterFluxProgressionRules::NotifyEvent(
			Registry, Context, Items, Quests, SelectedQuest,
			Kill, Effects, Error));
	TestEqual(TEXT("Kill quest completes"),
		FindQuest(Quests, TEXT("quest.kill"))->Status,
		EMatterFluxQuestRuntimeStatus::Completed);
	TestEqual(TEXT("Jump reward emitted exactly once"), Effects.Rewards.Num(), 1);
	TestEqual(TEXT("Tutorial completes"),
		FindQuest(Quests, TEXT("quest.tutorial"))->Status,
		EMatterFluxQuestRuntimeStatus::Completed);
	TestEqual(TEXT("Spend quest auto-activates and focuses"), SelectedQuest,
		FName(TEXT("quest.spend")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProgressionItemTransactionTest,
	"MatterFlux.Progression.ItemUseAndSpendAreTransactional",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxProgressionItemTransactionTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxProgressionTests;
	const FMatterFluxContentRegistry Registry = BuildRegistry();
	TArray<FMatterFluxItemStack> Items;
	int32 Previous = 0;
	int32 Current = 0;
	FString Error;
	if (!TestTrue(TEXT("Potion can be granted"),
		FMatterFluxProgressionRules::AddItem(
			Registry, Items, TEXT("item.potion"), 1,
			Previous, Current, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<FMatterFluxItemStack> BeforeUse = Items;
	FMatterFluxProgressionEffects Effects;
	TestTrue(TEXT("Potion use succeeds"),
		FMatterFluxProgressionRules::UseItem(
			Registry, Items, TEXT("item.potion"), Effects, Error));
	TestEqual(TEXT("Potion is consumed"), Items.Num(), 0);
	TestEqual(TEXT("Health capability is emitted"), Effects.ItemUseAction,
		EMatterFluxItemUseAction::RestoreHealth);
	TestEqual(TEXT("Health magnitude is emitted"), Effects.ItemUseMagnitude, 30.0f);
	const TArray<FMatterFluxItemStack> EmptyBeforeFailure = Items;
	TestFalse(TEXT("Using an absent potion fails"),
		FMatterFluxProgressionRules::UseItem(
			Registry, Items, TEXT("item.potion"), Effects, Error));
	TestEqual(TEXT("Failed use does not mutate stacks"),
		Items.Num(), EmptyBeforeFailure.Num());
	TestTrue(TEXT("Failure explains insufficient quantity"),
		Error.Contains(TEXT("quantity")));
	TestFalse(TEXT("A zero item delta is rejected"),
		FMatterFluxProgressionRules::AddItem(
			Registry, Items, TEXT("item.potion"), 0,
			Previous, Current, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProgressionHotReloadGraphTest,
	"MatterFlux.Progression.HotReloadAddsStarterAndChildNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxProgressionHotReloadGraphTest::RunTest(
	const FString& Parameters)
{
	using namespace MatterFluxProgressionTests;
	FMatterFluxContentRegistry Registry = BuildRegistry();
	FMatterFluxProgressionEvaluationContext Context;
	Context.EquippedWands.SetNum(1);
	Context.EquippedSpellsBySlot.SetNum(1);
	TArray<FMatterFluxItemStack> Items;
	TArray<FMatterFluxQuestState> Quests;
	FName Selected;
	FMatterFluxProgressionEffects Effects;
	FString Error;
	if (!TestTrue(TEXT("Initial graph builds"),
		FMatterFluxProgressionRules::BuildStarterState(
			Registry, Context, Items, Quests, Selected, Effects, Error)))
	{
		return false;
	}

	FMatterFluxQuestDefinition AddedRoot;
	AddedRoot.Id = TEXT("quest.hot_root");
	AddedRoot.DisplayName = TEXT("Hot root");
	AddedRoot.Description = TEXT("Hot root");
	AddedRoot.bStarter = true;
	AddedRoot.Objective = EMatterFluxQuestObjectiveKind::CompleteChildren;
	AddedRoot.Subquests.Add(TEXT("quest.hot_child"));
	Registry.Quests.Add(AddedRoot.Id, AddedRoot);
	FMatterFluxQuestDefinition AddedChild;
	AddedChild.Id = TEXT("quest.hot_child");
	AddedChild.Description = TEXT("Hot child");
	AddedChild.Category = EMatterFluxQuestCategory::Objective;
	AddedChild.Objective = EMatterFluxQuestObjectiveKind::Never;
	Registry.Quests.Add(AddedChild.Id, AddedChild);

	FMatterFluxQuestEvent Refresh;
	Refresh.Type = EMatterFluxQuestEventType::Refresh;
	if (!TestTrue(TEXT("Reloaded graph stabilizes"),
		FMatterFluxProgressionRules::NotifyEvent(
			Registry, Context, Items, Quests, Selected,
			Refresh, Effects, Error)))
	{
		AddError(Error);
		return false;
	}
	TestNotNull(TEXT("New starter root is injected"),
		FindQuest(Quests, AddedRoot.Id));
	const FMatterFluxQuestState* Child = FindQuest(Quests, AddedChild.Id);
	TestNotNull(TEXT("New child node is injected"), Child);
	if (Child)
	{
		TestEqual(TEXT("New child is active"), Child->Status,
			EMatterFluxQuestRuntimeStatus::Active);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxShellCoinQuantityTest,
	"MatterFlux.UI.TopBarTracksOwnedCoins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxShellCoinQuantityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString Error;
	if (!TestTrue(TEXT("Default content reloads for top-bar currency"),
		IMatterFluxScriptRuntime::Get().ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>() : nullptr;
	UMatterFluxProgressionComponent* Progression = PlayerState
		? PlayerState->GetProgression() : nullptr;
	if (!TestNotNull(TEXT("Player state exists for top-bar currency"),
		PlayerState)
		|| !TestNotNull(TEXT("Progression exists for top-bar currency"),
			Progression))
	{
		return false;
	}

	int32 ChangeCount = 0;
	const FDelegateHandle ChangeHandle =
		Progression->OnProgressionChanged().AddLambda(
			[&ChangeCount]() { ++ChangeCount; });
	FMatterFluxProgressionSaveState State;
	State.Revision = 1;
	FMatterFluxSavedItemStack& Coin = State.Items.AddDefaulted_GetRef();
	Coin.ItemId = TEXT("std.coin");
	Coin.Quantity = 7;
	if (!TestTrue(TEXT("Seven owned coins restore"),
		Progression->RestoreSaveStateAuthority(State, Error)))
	{
		AddError(Error);
		Progression->OnProgressionChanged().Remove(ChangeHandle);
		return false;
	}
	TestEqual(TEXT("Top bar reads all owned coins"),
		UMatterFluxShellWidget::ResolveOwnedCoinQuantity(PlayerState), 7);

	Coin.Quantity = 3;
	State.Revision = 2;
	TestTrue(TEXT("Spent coin state restores"),
		Progression->RestoreSaveStateAuthority(State, Error));
	TestEqual(TEXT("Top bar follows a lower coin balance"),
		UMatterFluxShellWidget::ResolveOwnedCoinQuantity(PlayerState), 3);

	State.Items.Reset();
	State.Revision = 3;
	TestTrue(TEXT("Empty coin state restores"),
		Progression->RestoreSaveStateAuthority(State, Error));
	TestEqual(TEXT("Top bar shows zero without a coin stack"),
		UMatterFluxShellWidget::ResolveOwnedCoinQuantity(PlayerState), 0);
	TestEqual(TEXT("Each restored balance broadcasts a UI refresh event"),
		ChangeCount, 3);
	Progression->OnProgressionChanged().Remove(ChangeHandle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProgressionAuthorityComponentTest,
	"MatterFlux.Progression.AuthorityComponentItemAndSaveTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxProgressionAuthorityComponentTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>() : nullptr;
	if (!TestNotNull(TEXT("Authority PlayerState spawns"), PlayerState))
	{
		return false;
	}
	UMatterFluxProgressionComponent* Progression = PlayerState->GetProgression();
	UMatterFluxPlayerAttributeSet* Attributes = PlayerState->GetPlayerAttributes();
	UAbilitySystemComponent* ASC = PlayerState->GetAbilitySystemComponent();
	if (!TestNotNull(TEXT("PlayerState owns progression"), Progression)
		|| !TestNotNull(TEXT("PlayerState owns health attributes"), Attributes)
		|| !TestNotNull(TEXT("PlayerState owns ASC"), ASC))
	{
		return false;
	}
	FString Error;
	if (Progression->GetRevision() == 0
		&& !TestTrue(TEXT("Starter progression initializes"),
			Progression->ResetToStarterStateAuthority(Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Lua starter inventory provides two healing potions"),
		Progression->GetItemQuantity(TEXT("std.heal_item")), 2);
	const int32 FullHealthRevision = Progression->GetRevision();
	TestFalse(TEXT("Potion is rejected at full health"),
		Progression->UseItemAuthority(
			TEXT("std.heal_item"), FullHealthRevision, Error));
	TestEqual(TEXT("Rejected use does not consume an item"),
		Progression->GetItemQuantity(TEXT("std.heal_item")), 2);
	TestEqual(TEXT("Rejected use does not advance revision"),
		Progression->GetRevision(), FullHealthRevision);

	ASC->SetNumericAttributeBase(
		UMatterFluxPlayerAttributeSet::GetHealthAttribute(), 50.0f);
	if (!TestTrue(TEXT("Potion use commits on authority"),
		Progression->UseItemAuthority(
			TEXT("std.heal_item"), FullHealthRevision, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Potion restores configured health"),
		Attributes->GetHealth(), 80.0f, 0.01f);
	TestEqual(TEXT("Committed use consumes exactly one"),
		Progression->GetItemQuantity(TEXT("std.heal_item")), 1);

	FMatterFluxProgressionSaveState Snapshot;
	if (!TestTrue(TEXT("Progression captures a save snapshot"),
		Progression->CaptureSaveState(Snapshot, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Temporary coin grant commits"),
		Progression->AddItemAuthority(TEXT("std.coin"), 7, Error));
	TestEqual(TEXT("Temporary grant is visible"),
		Progression->GetItemQuantity(TEXT("std.coin")), 7);
	if (!TestTrue(TEXT("Progression restores transactionally"),
		Progression->RestoreSaveStateAuthority(Snapshot, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Restore removes post-snapshot mutations"),
		Progression->GetItemQuantity(TEXT("std.coin")), 0);
	TestEqual(TEXT("Restore keeps snapshot item quantities"),
		Progression->GetItemQuantity(TEXT("std.heal_item")), 1);

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!TestTrue(TEXT("Shop registry is available"), Registry.IsValid()))
	{
		return false;
	}
	const FMatterFluxShopDefinition& Shop =
		Registry->Shops.FindChecked(TEXT("std.template_merchant"));
	TestTrue(TEXT("Shop test coins commit"),
		Progression->AddItemAuthority(TEXT("std.coin"), 100, Error));
	const int32 PurchaseRevision = Progression->GetRevision();
	int32 RemainingPurchases = INDEX_NONE;
	const int32 PotionCountBeforePurchase =
		Progression->GetItemQuantity(TEXT("std.heal_item"));
	if (!TestTrue(TEXT("Authority shop purchase commits atomically"),
		Progression->PurchaseOfferAuthority(
			Shop.Offers[0], TEXT("std.template_merchant.offer.0"),
			PurchaseRevision, RemainingPurchases, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Purchase deducts configured currency"),
		Progression->GetItemQuantity(TEXT("std.coin")), 0);
	TestEqual(TEXT("Purchase grants configured product"),
		Progression->GetItemQuantity(TEXT("std.heal_item")),
		PotionCountBeforePurchase + 1);
	FMatterFluxProgressionSaveState PurchasedSnapshot;
	TestTrue(TEXT("Purchased offer state is saveable"),
		Progression->CaptureSaveState(PurchasedSnapshot, Error));
	TestEqual(TEXT("Save contains one stable shop offer counter"),
		PurchasedSnapshot.ShopPurchases.Num(), 1);
	if (PurchasedSnapshot.ShopPurchases.Num() == 1)
	{
		TestEqual(TEXT("Saved shop purchase count"),
			PurchasedSnapshot.ShopPurchases[0].PurchaseCount, 1);
	}
	const int32 CommittedPurchaseRevision = Progression->GetRevision();
	TestFalse(TEXT("Stale purchase revision is rejected"),
		Progression->PurchaseOfferAuthority(
			Shop.Offers[0], TEXT("std.template_merchant.offer.0"),
			PurchaseRevision, RemainingPurchases, Error));
	TestEqual(TEXT("Rejected purchase does not mutate revision"),
		Progression->GetRevision(), CommittedPurchaseRevision);
	TestEqual(TEXT("Rejected purchase does not duplicate product"),
		Progression->GetItemQuantity(TEXT("std.heal_item")),
		PotionCountBeforePurchase + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProgressionMagicEffectsTransactionTest,
	"MatterFlux.Progression.MagicEffectsRejectWholeBatchWithoutMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxProgressionMagicEffectsTransactionTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content reloads"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>() : nullptr;
	UMatterFluxMagicInventoryComponent* Inventory = PlayerState
		? PlayerState->GetMagicInventory() : nullptr;
	if (!TestNotNull(TEXT("Authority magic inventory exists"), Inventory))
	{
		return false;
	}
	if (Inventory->GetInventoryRevision() == 0
		&& !TestTrue(TEXT("Starter loadout initializes"),
			Inventory->ResetToStarterLoadoutAuthority(Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Starter loadout includes five bound and one unbound caster"),
		Inventory->GetOwnedWands().Num(), 6);
	TestEqual(TEXT("Starter loadout exposes five equipment keys"),
		Inventory->GetEquippedWands().Num(),
		UGA_CastWand::EquipmentSlotCount);
	TestTrue(TEXT("PaperMagic default wand starts owned"),
		Inventory->GetOwnedWands().ContainsByPredicate(
			[](const FMatterFluxOwnedWand& Wand)
			{
				return Wand.DefinitionId == TEXT("std.default");
			}));
	TestTrue(TEXT("PaperMagic shoe caster starts owned"),
		Inventory->GetOwnedWands().ContainsByPredicate(
			[](const FMatterFluxOwnedWand& Wand)
			{
				return Wand.DefinitionId == TEXT("std.default_shoe");
			}));
	const FGuid SpaceWandId = Inventory->GetEquippedWandId(4);
	const FMatterFluxOwnedWand* SpaceWand = Inventory->FindWand(SpaceWandId);
	if (TestNotNull(TEXT("Space starts with an equipped wand"), SpaceWand))
	{
		TestEqual(TEXT("Space uses the shoe caster"),
			SpaceWand->DefinitionId, FName(TEXT("std.default_shoe")));
		TestEqual(TEXT("Space caster has one slot"),
			SpaceWand->SpellSlots.Num(), 1);
		if (SpaceWand->SpellSlots.Num() == 1)
		{
			TestEqual(TEXT("Space caster starts with jump"),
				SpaceWand->SpellSlots[0], FName(TEXT("std.jump")));
		}
	}

	FMatterFluxMagicInventorySaveState DrainedState;
	if (!TestTrue(TEXT("Magic inventory snapshot captures"),
		Inventory->CaptureSaveState(DrainedState, Error)))
	{
		AddError(Error);
		return false;
	}
	const FGuid ActiveWandId = Inventory->GetActiveWandId();
	FMatterFluxSavedWand* ActiveSavedWand = DrainedState.Wands.FindByPredicate(
		[ActiveWandId](const FMatterFluxSavedWand& Wand)
		{
			return Wand.InstanceId == ActiveWandId;
		});
	if (!TestNotNull(TEXT("Active wand is present in snapshot"), ActiveSavedWand))
	{
		return false;
	}
	ActiveSavedWand->Mana = 0.0f;
	if (!TestTrue(TEXT("Drained wand state restores"),
		Inventory->RestoreSaveStateAuthority(DrainedState, Error)))
	{
		AddError(Error);
		return false;
	}

	const int32 BeforeRevision = Inventory->GetInventoryRevision();
	const int32 BeforeSpellCount = Inventory->GetOwnedSpells().Num();
	const int32 BeforeWandCount = Inventory->GetOwnedWands().Num();
	const FMatterFluxOwnedWand* BeforeWand =
		Inventory->FindWand(ActiveWandId);
	if (!TestNotNull(TEXT("Drained active wand resolves"), BeforeWand))
	{
		return false;
	}
	TestEqual(TEXT("Test precondition drains active wand"),
		BeforeWand->Mana, 0.0f, 0.01f);

	FMatterFluxQuestRewardDefinition InvalidReward;
	InvalidReward.Kind = EMatterFluxQuestRewardKind::Spell;
	InvalidReward.ContentId = TEXT("missing.transaction_spell");
	InvalidReward.Quantity = 1;
	const TArray<FMatterFluxQuestRewardDefinition> Rewards = { InvalidReward };
	TestFalse(TEXT("Invalid combined magic effects are rejected"),
		Inventory->ApplyProgressionEffectsAuthority(
			Rewards, 25.0f, Error));
	TestTrue(TEXT("Failure identifies the invalid reward"),
		Error.Contains(TEXT("unknown spell")));

	const FMatterFluxOwnedWand* AfterWand = Inventory->FindWand(ActiveWandId);
	if (TestNotNull(TEXT("Active wand still resolves after rejection"), AfterWand))
	{
		TestEqual(TEXT("Rejected reward does not partially restore mana"),
			AfterWand->Mana, 0.0f, 0.01f);
	}
	TestEqual(TEXT("Rejected batch does not advance inventory revision"),
		Inventory->GetInventoryRevision(), BeforeRevision);
	TestEqual(TEXT("Rejected batch does not add spells"),
		Inventory->GetOwnedSpells().Num(), BeforeSpellCount);
	TestEqual(TEXT("Rejected batch does not add wands"),
		Inventory->GetOwnedWands().Num(), BeforeWandCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStoryInitialInventoryTest,
	"MatterFlux.Progression.StoryStartsEmptyThenGrantsOnlyTutorialLoadout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxStoryInitialInventoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content reloads for story inventory"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>() : nullptr;
	UMatterFluxMagicInventoryComponent* Inventory = PlayerState
		? PlayerState->GetMagicInventory() : nullptr;
	UMatterFluxProgressionComponent* Progression = PlayerState
		? PlayerState->GetProgression() : nullptr;
	if (!TestNotNull(TEXT("Story inventory exists"), Inventory)
		|| !TestNotNull(TEXT("Story progression exists"), Progression))
	{
		return false;
	}
	if (!TestTrue(TEXT("Story magic inventory clears starter content"),
		Inventory->ResetToEmptyLoadoutAuthority(Error))
		|| !TestTrue(TEXT("Story progression activates from an empty item bag"),
			Progression->ResetToStoryStateAuthority(Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Story starts without ordinary items"),
		Progression->GetItems().IsEmpty());
	TestEqual(TEXT("Tutorial activation grants exactly one wand definition"),
		Inventory->GetOwnedWands().Num(), 1);
	TestEqual(TEXT("Tutorial activation grants exactly one spell stack"),
		Inventory->GetOwnedSpells().Num(), 1);
	if (Inventory->GetOwnedWands().Num() == 1)
	{
		TestEqual(TEXT("Tutorial wand is the default attack wand"),
			Inventory->GetOwnedWands()[0].DefinitionId,
			FName(TEXT("std.default")));
	}
	if (Inventory->GetOwnedSpells().Num() == 1)
	{
		TestEqual(TEXT("Tutorial spell is the default attack spell"),
			Inventory->GetOwnedSpells()[0].SpellId,
			FName(TEXT("std.default")));
		TestEqual(TEXT("Tutorial grants one attack spell"),
			Inventory->GetOwnedSpells()[0].Quantity, 1);
	}
	for (const FGuid Equipped : Inventory->GetEquippedWands())
	{
		TestFalse(TEXT("Tutorial rewards begin in the backpack"),
			Equipped.IsValid());
	}

	FMatterFluxQuestRewardDefinition ShoeReward;
	ShoeReward.Kind = EMatterFluxQuestRewardKind::Wand;
	ShoeReward.ContentId = TEXT("std.default_shoe");
	ShoeReward.Quantity = 1;
	ShoeReward.EquipmentSlot = 4;
	FMatterFluxQuestRewardDefinition JumpReward;
	JumpReward.Kind = EMatterFluxQuestRewardKind::Spell;
	JumpReward.ContentId = TEXT("std.jump");
	JumpReward.Quantity = 1;
	const TArray<FMatterFluxQuestRewardDefinition> CombatRewards = {
		ShoeReward, JumpReward };
	if (!TestTrue(TEXT("Combat rewards apply atomically"),
		Inventory->ApplyQuestRewardsAuthority(CombatRewards, Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxOwnedWand* Shoe = Inventory->FindWand(
		Inventory->GetEquippedWandId(4));
	if (TestNotNull(TEXT("Combat reward equips the leg wand"), Shoe))
	{
		TestEqual(TEXT("Leg reward uses the shoe caster"),
			Shoe->DefinitionId, FName(TEXT("std.default_shoe")));
		TestEqual(TEXT("Leg wand is immediately usable for jumping"),
			Shoe->SpellSlots[0], FName(TEXT("std.jump")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRetiredEmberSpellMigrationTest,
	"MatterFlux.Progression.RetiredEmberSpellMigratesToFlameProjectile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxRetiredEmberSpellMigrationTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Default content reloads for spell migration"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>() : nullptr;
	UMatterFluxMagicInventoryComponent* Inventory = PlayerState
		? PlayerState->GetMagicInventory() : nullptr;
	if (!TestNotNull(TEXT("Magic inventory exists for migration"), Inventory)
		|| !TestTrue(TEXT("Starter inventory initializes for migration"),
			Inventory->ResetToStarterLoadoutAuthority(Error)))
	{
		AddError(Error);
		return false;
	}

	FMatterFluxMagicInventorySaveState LegacyState;
	if (!TestTrue(TEXT("Legacy migration snapshot captures"),
		Inventory->CaptureSaveState(LegacyState, Error)))
	{
		AddError(Error);
		return false;
	}
	LegacyState.Spells.RemoveAll([](const FMatterFluxSavedSpell& Spell)
	{
		return Spell.SpellId == TEXT("spell.ember_bolt")
			|| Spell.SpellId == TEXT("spell.flame_jet");
	});
	FMatterFluxSavedSpell& LegacyEmber = LegacyState.Spells.AddDefaulted_GetRef();
	LegacyEmber.SpellId = TEXT("spell.ember_bolt");
	LegacyEmber.Quantity = 3;
	FMatterFluxSavedWand* LegacyWand = LegacyState.Wands.FindByPredicate(
		[](const FMatterFluxSavedWand& Wand)
		{
			return !Wand.SpellSlots.IsEmpty();
		});
	if (!TestNotNull(TEXT("Legacy snapshot owns a programmable wand"), LegacyWand))
	{
		return false;
	}
	LegacyWand->SpellSlots[0] = TEXT("spell.ember_bolt");
	const FGuid LegacyWandId = LegacyWand->InstanceId;

	if (!TestTrue(TEXT("Legacy ember inventory restores"),
		Inventory->RestoreSaveStateAuthority(LegacyState, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("Retired ember spell does not survive inventory restore"),
		Inventory->GetOwnedSpells().ContainsByPredicate(
			[](const FMatterFluxOwnedSpell& Spell)
			{
				return Spell.SpellId == TEXT("spell.ember_bolt");
			}));
	const FMatterFluxOwnedSpell* MigratedFlame =
		Inventory->GetOwnedSpells().FindByPredicate(
			[](const FMatterFluxOwnedSpell& Spell)
			{
				return Spell.SpellId == TEXT("spell.flame_jet");
			});
	if (TestNotNull(TEXT("Ember stack becomes flame projectile stack"),
		MigratedFlame))
	{
		TestEqual(TEXT("Retired spell quantity is preserved"),
			MigratedFlame->Quantity, 3);
	}
	const FMatterFluxOwnedWand* MigratedWand = Inventory->FindWand(LegacyWandId);
	if (TestNotNull(TEXT("Wand survives retired spell migration"), MigratedWand))
	{
		TestEqual(TEXT("Wand slot now references the flame projectile"),
			MigratedWand->SpellSlots[0], FName(TEXT("spell.flame_jet")));
	}
	return true;
}
