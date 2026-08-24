#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Save/MatterFluxSaveGame.h"
#include "Save/MatterFluxSaveSubsystem.h"
#include "Settings/MatterFluxGameUserSettings.h"

#include <limits>

namespace MatterFluxSaveTests
{
	UMatterFluxSaveGame* MakeValidSave(const int32 Seed = 12345)
	{
		UMatterFluxSaveGame* Save = NewObject<UMatterFluxSaveGame>();
		Save->InitializeNew(Seed);
		return Save;
	}

	FMatterFluxSavedWand MakeWand()
	{
		FMatterFluxSavedWand Wand;
		Wand.InstanceId = FGuid::NewGuid();
		Wand.DefinitionId = TEXT("wand.test");
		Wand.SpellSlots = { TEXT("spell.test"), NAME_None };
		Wand.Mana = 50.0f;
		Wand.DeckCursor = 1;
		Wand.CastSerial = 2;
		Wand.CastCooldownRemaining = 0.25f;
		return Wand;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSaveInitializationTest,
	"MatterFlux.Save.InitializationAndMigration",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSaveInitializationTest::RunTest(const FString& Parameters)
{
	using namespace MatterFluxSaveTests;
	UMatterFluxSaveGame* Save = MakeValidSave(9876);
	TestNotNull(TEXT("Save object is created"), Save);
	TestEqual(TEXT("New save uses current version"),
		Save->SaveVersion,
		UMatterFluxSaveGame::CurrentVersion);
	TestEqual(TEXT("Seed is retained"), Save->MapSeed, 9876);
	TestEqual(TEXT("All equipment slots are initialized"),
		Save->MagicInventory.EquippedWands.Num(),
		4);

	FString Error;
	TestTrue(TEXT("Fresh save validates"), Save->ValidateAndMigrate(Error));
	TestTrue(TEXT("Fresh save has no validation error"), Error.IsEmpty());

	Save->SaveVersion = 0;
	TestTrue(TEXT("Version zero migrates"), Save->ValidateAndMigrate(Error));
	TestEqual(TEXT("Migration updates version"),
		Save->SaveVersion,
		UMatterFluxSaveGame::CurrentVersion);

	Save->SaveVersion = UMatterFluxSaveGame::CurrentVersion + 1;
	TestFalse(TEXT("Future save version is rejected"),
		Save->ValidateAndMigrate(Error));
	TestFalse(TEXT("Future version reports an error"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSaveCorruptionTest,
	"MatterFlux.Save.CorruptPayloadRejected",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSaveCorruptionTest::RunTest(const FString& Parameters)
{
	using namespace MatterFluxSaveTests;
	FString Error;

	UMatterFluxSaveGame* DuplicateSpellSave = MakeValidSave();
	FMatterFluxSavedSpell Spell;
	Spell.SpellId = TEXT("spell.test");
	Spell.Quantity = 1;
	DuplicateSpellSave->MagicInventory.Spells = { Spell, Spell };
	TestFalse(TEXT("Duplicate spell stacks are rejected"),
		DuplicateSpellSave->ValidateAndMigrate(Error));

	UMatterFluxSaveGame* BadEquipmentSave = MakeValidSave();
	BadEquipmentSave->MagicInventory.EquippedWands[0] = FGuid::NewGuid();
	TestFalse(TEXT("Unknown equipped wand is rejected"),
		BadEquipmentSave->ValidateAndMigrate(Error));

	UMatterFluxSaveGame* AmbiguousProgressionSave = MakeValidSave();
	FMatterFluxSavedItemStack AmbiguousItem;
	AmbiguousItem.ItemId = TEXT("std.coin");
	AmbiguousItem.Quantity = 1;
	AmbiguousProgressionSave->Progression.Items = { AmbiguousItem };
	AmbiguousProgressionSave->Progression.Revision = 0;
	TestFalse(TEXT("Revision-zero progression with payload is rejected"),
		AmbiguousProgressionSave->ValidateAndMigrate(Error));

	UMatterFluxSaveGame* DuplicateSourceSave = MakeValidSave();
	FMatterFluxSavedFragmentSourceState Source;
	Source.SourceId = FGuid::NewGuid();
	Source.RuntimeMask = { 1, 0, 1 };
	DuplicateSourceSave->WorldState.FragmentSources = { Source, Source };
	TestFalse(TEXT("Duplicate fragment source ids are rejected"),
		DuplicateSourceSave->ValidateAndMigrate(Error));

	UMatterFluxSaveGame* NonBinaryMaskSave = MakeValidSave();
	Source.SourceId = FGuid::NewGuid();
	Source.RuntimeMask = { 1, 2, 0 };
	NonBinaryMaskSave->WorldState.FragmentSources = { Source };
	TestFalse(TEXT("Non-binary source masks are rejected"),
		NonBinaryMaskSave->ValidateAndMigrate(Error));

	UMatterFluxSaveGame* ActiveCountdownSave = MakeValidSave();
	Source = FMatterFluxSavedFragmentSourceState();
	Source.SourceId = FGuid::NewGuid();
	Source.RuntimeMask = { 1 };
	Source.bHasReactionState = true;
	Source.ReactionState.RuleId = TEXT("reaction.test");
	Source.ReactionState.Width = 1;
	Source.ReactionState.Height = 1;
	Source.ReactionState.InputMask = { 1 };
	Source.ReactionState.OutputMask = { 0 };
	Source.ReactionState.ActiveMask = { 3 };
	ActiveCountdownSave->WorldState.FragmentSources = { Source };
	TestTrue(TEXT("A multi-step active reaction is saveable"),
		ActiveCountdownSave->ValidateAndMigrate(Error));

	UMatterFluxSaveGame* OrphanedActiveCellSave = MakeValidSave();
	Source.SourceId = FGuid::NewGuid();
	Source.RuntimeMask = { 0 };
	Source.ReactionState.InputMask = { 0 };
	Source.ReactionState.ActiveMask = { 1 };
	OrphanedActiveCellSave->WorldState.FragmentSources = { Source };
	TestFalse(TEXT("An active cell without reaction input is rejected"),
		OrphanedActiveCellSave->ValidateAndMigrate(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSaveMemoryRoundTripTest,
	"MatterFlux.Save.MemoryRoundTrip",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSaveMemoryRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MatterFluxSaveTests;
	UMatterFluxSaveGame* Source = MakeValidSave(24680);
	FMatterFluxSavedSpell& Spell =
		Source->MagicInventory.Spells.AddDefaulted_GetRef();
	Spell.SpellId = TEXT("spell.test");
	Spell.Quantity = 3;
	FMatterFluxSavedWand& Wand =
		Source->MagicInventory.Wands.Add_GetRef(MakeWand());
	Source->MagicInventory.EquippedWands[0] = Wand.InstanceId;
	FMatterFluxSavedItemStack& Item =
		Source->Progression.Items.AddDefaulted_GetRef();
	Item.ItemId = TEXT("std.coin");
	Item.Quantity = 9;
	FMatterFluxSavedQuestState& Quest =
		Source->Progression.Quests.AddDefaulted_GetRef();
	Quest.QuestId = TEXT("std.quest");
	Quest.Status = 1;
	Quest.Progress = 2;
	Source->Progression.SelectedQuest = Quest.QuestId;
	Source->Progression.Revision = 7;
	Source->PlayerTransform.SetLocation(FVector(120.0, 0.0, 340.0));

	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("Save serializes to memory"),
		UGameplayStatics::SaveGameToMemory(Source, Bytes)))
	{
		return false;
	}
	TestTrue(TEXT("Serialized payload is non-empty"), !Bytes.IsEmpty());

	UMatterFluxSaveGame* Loaded = Cast<UMatterFluxSaveGame>(
		UGameplayStatics::LoadGameFromMemory(Bytes));
	if (!TestNotNull(TEXT("Serialized save loads with the right class"), Loaded))
	{
		return false;
	}
	FString Error;
	TestTrue(TEXT("Round-tripped save validates"),
		Loaded->ValidateAndMigrate(Error));
	TestEqual(TEXT("Seed round-trips"), Loaded->MapSeed, 24680);
	TestEqual(TEXT("Spell stack round-trips"),
		Loaded->MagicInventory.Spells[0].Quantity,
		3);
	TestEqual(TEXT("Wand id round-trips"),
		Loaded->MagicInventory.Wands[0].InstanceId,
		Wand.InstanceId);
	TestEqual(TEXT("Item stack round-trips"),
		Loaded->Progression.Items[0].Quantity, 9);
	TestEqual(TEXT("Quest selection round-trips"),
		Loaded->Progression.SelectedQuest, FName(TEXT("std.quest")));
	TestEqual(TEXT("Player location round-trips"),
		Loaded->PlayerTransform.GetLocation(),
		FVector(120.0, 0.0, 340.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSaveMetadataRepairTest,
	"MatterFlux.Save.MetadataRepair",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSaveMetadataRepairTest::RunTest(const FString& Parameters)
{
	UMatterFluxSaveMetadata* Metadata =
		NewObject<UMatterFluxSaveMetadata>();
	Metadata->Initialize();
	TestTrue(TEXT("Fresh metadata is valid"),
		Metadata->ValidateAndRepair());
	TestEqual(TEXT("Fresh metadata has no artificial empty slots"),
		Metadata->Slots.Num(), 0);
	TestEqual(TEXT("The first dynamic slot uses id zero"),
		Metadata->GetNextAvailableSlotIndex(), 0);

	Metadata->MetadataVersion = 1;
	Metadata->Slots.SetNum(3);
	Metadata->Slots[0].SlotIndex = 0;
	Metadata->Slots[0].bOccupied = true;
	Metadata->Slots[0].MapSeed = 101;
	Metadata->Slots[0].SavedAtUtc = FDateTime(2026, 1, 1);
	Metadata->Slots[1].SlotIndex = 1;
	Metadata->Slots[2].SlotIndex = 2;
	Metadata->Slots[2].bOccupied = true;
	Metadata->Slots[2].MapSeed = 303;
	Metadata->Slots[2].SavedAtUtc = FDateTime(2026, 1, 3);
	TestFalse(TEXT("Version-one fixed slots report a migration"),
		Metadata->ValidateAndRepair());
	TestEqual(TEXT("Migration retains only occupied saves"),
		Metadata->Slots.Num(), 2);
	TestEqual(TEXT("First stable slot id is retained"),
		Metadata->Slots[0].SlotIndex, 0);
	TestEqual(TEXT("Sparse stable slot id is retained"),
		Metadata->Slots[1].SlotIndex, 2);
	TestEqual(TEXT("Deleted gap is reused for the next save"),
		Metadata->GetNextAvailableSlotIndex(), 1);
	TestEqual(TEXT("Display names trim surrounding whitespace"),
		UMatterFluxSaveMetadata::NormalizeDisplayName(
			TEXT("  森林世界  ")),
		FString(TEXT("森林世界")));
	TestEqual(TEXT("Display names cannot inject new rows"),
		UMatterFluxSaveMetadata::NormalizeDisplayName(
			TEXT("森林\n副本")),
		FString(TEXT("森林 副本")));
	TestEqual(TEXT("Display names are bounded to 32 characters"),
		UMatterFluxSaveMetadata::NormalizeDisplayName(
			FString::ChrN(40, TCHAR('A'))).Len(),
		32);

	Metadata->Initialize();
	for (int32 SlotIndex = 0; SlotIndex < 128; ++SlotIndex)
	{
		if (SlotIndex == 73)
		{
			continue;
		}
		FMatterFluxSaveSlotInfo& Slot = Metadata->Slots.AddDefaulted_GetRef();
		Slot.SlotIndex = SlotIndex;
		Slot.bOccupied = true;
		Slot.MapSeed = 1000 + SlotIndex;
		Slot.SavedAtUtc = FDateTime(2026, 2, 1)
			+ FTimespan::FromMinutes(SlotIndex);
		Slot.DisplayName = FString::Printf(TEXT("世界 %d"), SlotIndex);
	}
	TestTrue(TEXT("More than three dynamic slots remain valid"),
		Metadata->ValidateAndRepair());
	TestEqual(TEXT("Dynamic metadata keeps all 127 saves"),
		Metadata->Slots.Num(), 127);
	TestEqual(TEXT("The first sparse gap is reused"),
		Metadata->GetNextAvailableSlotIndex(), 73);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxOperationProgressModelTest,
	"MatterFlux.Save.OperationProgressModel",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxOperationProgressModelTest::RunTest(
	const FString& Parameters)
{
	const auto Map = [](const float WorldProgress, const bool bLoading)
	{
		return UMatterFluxSaveSubsystem::MapWorldGenerationProgress(
			WorldProgress,
			bLoading);
	};

	TestEqual(TEXT("New-game generation begins at five percent"),
		Map(0.05f, false), 0.05f, 0.001f);
	TestEqual(TEXT("New-game tree phase matches world progress"),
		Map(0.82f, false), 0.82f, 0.001f);
	TestEqual(TEXT("New-game generation reserves final apply step"),
		Map(1.0f, false), 0.97f, 0.001f);
	TestEqual(TEXT("Load generation begins after disk-read phase"),
		Map(0.05f, true), 0.15f, 0.001f);
	TestEqual(TEXT("Load tree phase remains semantically aligned"),
		Map(0.82f, true), 0.815f, 0.002f);
	TestEqual(TEXT("Load generation reserves final apply step"),
		Map(1.0f, true), 0.97f, 0.001f);
	TestEqual(TEXT("Negative new-game progress clamps to its start"),
		Map(-10.0f, false), 0.05f, 0.001f);
	TestEqual(TEXT("Oversized load progress clamps to its endpoint"),
		Map(10.0f, true), 0.97f, 0.001f);
	TestEqual(TEXT("Non-finite load progress repairs to its start"),
		Map(std::numeric_limits<float>::quiet_NaN(), true),
		0.15f,
		0.001f);
	TestTrue(TEXT("Applying world follows completed generation"),
		UMatterFluxSaveSubsystem::GetApplyingWorldProgress() > 0.97f);

	for (const bool bLoading : { false, true })
	{
		float Previous = 0.0f;
		for (int32 Step = 0; Step <= 100; ++Step)
		{
			const float Progress = Map(Step / 100.0f, bLoading);
			TestTrue(TEXT("Mapped progress is finite"),
				FMath::IsFinite(Progress));
			TestTrue(TEXT("Mapped progress stays in range"),
				Progress >= 0.0f && Progress <= 1.0f);
			TestTrue(TEXT("Mapped progress never decreases"),
				Progress + KINDA_SMALL_NUMBER >= Previous);
			Previous = Progress;
		}
	}

	TestFalse(TEXT("Asynchronous disk save is indeterminate"),
		UMatterFluxSaveSubsystem::IsDeterminateOperation(
			EMatterFluxSaveOperation::Saving));
	TestFalse(TEXT("Asynchronous disk load is indeterminate"),
		UMatterFluxSaveSubsystem::IsDeterminateOperation(
			EMatterFluxSaveOperation::Loading));
	TestTrue(TEXT("World generation reports real progress"),
		UMatterFluxSaveSubsystem::IsDeterminateOperation(
			EMatterFluxSaveOperation::GeneratingWorld));
	TestTrue(TEXT("World application has a bounded final step"),
		UMatterFluxSaveSubsystem::IsDeterminateOperation(
			EMatterFluxSaveOperation::ApplyingWorld));
	TestFalse(TEXT("Idle has no progress indicator"),
		UMatterFluxSaveSubsystem::IsDeterminateOperation(
			EMatterFluxSaveOperation::Idle));
	TestFalse(TEXT("Completed operation has no active progress"),
		UMatterFluxSaveSubsystem::IsDeterminateOperation(
			EMatterFluxSaveOperation::Complete));
	TestFalse(TEXT("Failed operation has no active progress"),
		UMatterFluxSaveSubsystem::IsDeterminateOperation(
			EMatterFluxSaveOperation::Failed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxGameUserSettingsTest,
	"MatterFlux.Settings.DefaultsAndClamps",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxGameUserSettingsTest::RunTest(const FString& Parameters)
{
	UMatterFluxGameUserSettings* Settings =
		NewObject<UMatterFluxGameUserSettings>();
	Settings->SetToDefaults();
	TestEqual(TEXT("Default master volume"),
		Settings->GetMasterVolume(),
		0.8f);
	TestEqual(TEXT("Default interface scale"),
		Settings->GetInterfaceScale(),
		1.0f);
	TestTrue(TEXT("VSync defaults on"), Settings->IsVSyncEnabled());

	Settings->SetMasterVolume(4.0f);
	Settings->SetInterfaceScale(0.1f);
	TestEqual(TEXT("Master volume clamps high"),
		Settings->GetMasterVolume(),
		1.0f);
	TestEqual(TEXT("Interface scale clamps low"),
		Settings->GetInterfaceScale(),
		0.8f);

	Settings->SetMasterVolume(std::numeric_limits<float>::quiet_NaN());
	Settings->SetInterfaceScale(std::numeric_limits<float>::quiet_NaN());
	Settings->ValidateSettings();
	TestEqual(TEXT("Invalid master volume repairs to default"),
		Settings->GetMasterVolume(),
		0.8f);
	TestEqual(TEXT("Invalid interface scale repairs to default"),
		Settings->GetInterfaceScale(),
		1.0f);
	return true;
}
