#include "Save/MatterFluxSaveGame.h"

#include "MatterFluxContentTypes.h"

namespace MatterFluxSaveValidation
{
	constexpr int32 MaximumInventoryEntries = 256;
	constexpr int32 MaximumProgressionEntries = 512;
	constexpr int32 MaximumFragmentStates = 20000;
	constexpr int32 MaximumMaskCells = 1024 * 1024;
	constexpr int32 MaximumMaterialStateBytes = 1024 * 1024;
	constexpr int32 EquipmentSlotCount =
		MatterFlux::Magic::EquipmentSlotCount;

	bool IsBinaryMask(const TArray<uint8>& Mask)
	{
		return Mask.Num() <= MaximumMaskCells
			&& !Mask.ContainsByPredicate(
				[](const uint8 Value) { return Value > 1; });
	}

	bool IsValidReactionState(
		const FMatterFluxSavedReactionState& State)
	{
		const int64 CellCount = static_cast<int64>(State.Width)
			* static_cast<int64>(State.Height);
		if (State.RuleId.IsNone()
			|| State.Width <= 0
			|| State.Height <= 0
			|| CellCount <= 0
			|| CellCount > MaximumMaskCells
			|| State.InputMask.Num() != CellCount
			|| State.OutputMask.Num() != CellCount
			|| State.ActiveMask.Num() != CellCount
			|| !IsBinaryMask(State.InputMask)
			|| !IsBinaryMask(State.OutputMask))
		{
			return false;
		}
		for (int32 Index = 0; Index < State.InputMask.Num(); ++Index)
		{
			const uint8 Input = State.InputMask[Index];
			const uint8 Output = State.OutputMask[Index];
			// This is a countdown, not a binary occupancy mask. Its rule-specific
			// upper bound is checked when the reaction runtime restores the state.
			const uint8 ActiveStepsRemaining = State.ActiveMask[Index];
			if ((Input != 0 && Output != 0)
				|| (ActiveStepsRemaining != 0 && Input == 0))
			{
				return false;
			}
		}
		return true;
	}
}

void UMatterFluxSaveGame::InitializeNew(const int32 InMapSeed)
{
	SaveVersion = CurrentVersion;
	SavedAtUtc = FDateTime::UtcNow();
	MapSeed = FMath::Max(InMapSeed, 1);
	CustomMapId = NAME_None;
	PlayerTransform = FTransform::Identity;
	MagicInventory = FMatterFluxMagicInventorySaveState();
	MagicInventory.EquippedWands.SetNum(
		MatterFluxSaveValidation::EquipmentSlotCount);
	Progression = FMatterFluxProgressionSaveState();
	WorldState = FMatterFluxWorldSaveState();
}

bool UMatterFluxSaveGame::ValidateAndMigrate(FString& OutError)
{
	using namespace MatterFluxSaveValidation;
	OutError.Reset();
	const int32 SourceVersion = SaveVersion;
	if (SaveVersion < 0 || SaveVersion > CurrentVersion)
	{
		OutError = FString::Printf(
			TEXT("unsupported save version %d"), SaveVersion);
		return false;
	}
	const bool bHasSourceReactionState =
		WorldState.FragmentSources.ContainsByPredicate(
			[](const FMatterFluxSavedFragmentSourceState& State)
			{
				return State.bHasReactionState;
			});
	if (bHasSourceReactionState || WorldState.bHasGroundReactionState)
	{
		OutError = SourceVersion == 5
			? TEXT("Version 5 save contains incompatible object-level ReactionState; the world was not modified")
			: TEXT("save contains deprecated object-level ReactionState");
		return false;
	}
	if (SourceVersion <= 1)
	{
		// Versions 0-1 predate item/quest persistence. Revision zero tells the
		// authoritative component to rebuild Lua-defined starter progression.
		Progression = FMatterFluxProgressionSaveState();
		Progression.Revision = 0;
	}
	if (SourceVersion <= 3)
	{
		// Version 4 turns Space from a hard-coded jump input into the fifth
		// equipment key. Older valid inventories gain an empty slot; their owned
		// wands and spells remain untouched.
		if (MagicInventory.EquippedWands.Num() == 4)
		{
			MagicInventory.EquippedWands.SetNum(EquipmentSlotCount);
		}
	}
	if (SourceVersion <= 4)
	{
		// Version 5 records whether a save belongs to the procedural free mode
		// or to a Lua-authored story map. Older saves are all free-mode worlds.
		CustomMapId = NAME_None;
	}
	if (MapSeed <= 0 || !PlayerTransform.IsValid())
	{
		OutError = TEXT("save has an invalid map seed or player transform");
		return false;
	}
	if (MagicInventory.Spells.Num() > MaximumInventoryEntries
		|| MagicInventory.Wands.Num() > MaximumInventoryEntries
		|| MagicInventory.EquippedWands.Num() != EquipmentSlotCount
		|| MagicInventory.InventoryRevision < 0
		|| MagicInventory.ActiveEquipmentSlot < 0
		|| MagicInventory.ActiveEquipmentSlot >= EquipmentSlotCount)
	{
		OutError = TEXT("save has invalid magic inventory metadata");
		return false;
	}
	TSet<FName> SeenSpellIds;
	for (const FMatterFluxSavedSpell& Spell : MagicInventory.Spells)
	{
		if (Spell.SpellId.IsNone()
			|| Spell.Quantity <= 0
			|| SeenSpellIds.Contains(Spell.SpellId))
		{
			OutError = TEXT("save contains an invalid spell stack");
			return false;
		}
		SeenSpellIds.Add(Spell.SpellId);
	}
	TSet<FGuid> SeenWandIds;
	for (const FMatterFluxSavedWand& Wand : MagicInventory.Wands)
	{
		if (!Wand.InstanceId.IsValid()
			|| Wand.DefinitionId.IsNone()
			|| SeenWandIds.Contains(Wand.InstanceId)
			|| Wand.SpellSlots.Num() > MaximumInventoryEntries
			|| !FMath::IsFinite(Wand.Mana)
			|| Wand.Mana < 0.0f
			|| Wand.DeckCursor < 0
			|| Wand.CastSerial < 0
			|| !FMath::IsFinite(Wand.CastCooldownRemaining)
			|| Wand.CastCooldownRemaining < 0.0f
			|| Wand.CastCooldownRemaining > 3600.0f)
		{
			OutError = TEXT("save contains an invalid wand");
			return false;
		}
		SeenWandIds.Add(Wand.InstanceId);
	}
	TSet<FGuid> SeenEquippedWands;
	for (const FGuid EquippedWand : MagicInventory.EquippedWands)
	{
		if (EquippedWand.IsValid()
			&& (!SeenWandIds.Contains(EquippedWand)
				|| SeenEquippedWands.Contains(EquippedWand)))
		{
			OutError = TEXT("save contains an invalid equipped wand reference");
			return false;
		}
		if (EquippedWand.IsValid())
		{
			SeenEquippedWands.Add(EquippedWand);
		}
	}
	if (Progression.Items.Num() > MaximumProgressionEntries
		|| Progression.Quests.Num() > MaximumProgressionEntries
		|| Progression.ShopPurchases.Num() > MaximumProgressionEntries
		|| Progression.Revision < 0
		|| (Progression.Revision == 0
			&& (!Progression.Items.IsEmpty() || !Progression.Quests.IsEmpty()
				|| !Progression.ShopPurchases.IsEmpty()
				|| !Progression.SelectedQuest.IsNone())))
	{
		OutError = TEXT("save has invalid progression metadata");
		return false;
	}
	TSet<FName> SeenItemIds;
	for (const FMatterFluxSavedItemStack& Item : Progression.Items)
	{
		if (Item.ItemId.IsNone() || Item.Quantity <= 0
			|| SeenItemIds.Contains(Item.ItemId))
		{
			OutError = TEXT("save contains an invalid item stack");
			return false;
		}
		SeenItemIds.Add(Item.ItemId);
	}
	TSet<FName> SeenQuestIds;
	for (const FMatterFluxSavedQuestState& Quest : Progression.Quests)
	{
		if (Quest.QuestId.IsNone() || Quest.Status > 3 || Quest.Progress < 0
			|| SeenQuestIds.Contains(Quest.QuestId))
		{
			OutError = TEXT("save contains an invalid quest state");
			return false;
		}
		SeenQuestIds.Add(Quest.QuestId);
	}
	TSet<FName> SeenShopOfferKeys;
	for (const FMatterFluxSavedShopPurchase& Purchase
		: Progression.ShopPurchases)
	{
		if (Purchase.OfferKey.IsNone() || Purchase.PurchaseCount <= 0
			|| SeenShopOfferKeys.Contains(Purchase.OfferKey))
		{
			OutError = TEXT("save contains an invalid shop purchase");
			return false;
		}
		SeenShopOfferKeys.Add(Purchase.OfferKey);
	}
	if (!Progression.SelectedQuest.IsNone()
		&& !SeenQuestIds.Contains(Progression.SelectedQuest))
	{
		OutError = TEXT("save selects a quest that is not present");
		return false;
	}
	if (WorldState.MaterialActiveState.Num() > MaximumMaterialStateBytes
		|| WorldState.TerrainHeightOverrides.Num() > MaximumMaskCells
		|| WorldState.TerrainSpanOverrides.Num() > MaximumMaskCells
		|| WorldState.FragmentSources.Num() > MaximumFragmentStates
		|| WorldState.RemovedFragmentSourceIds.Num() > MaximumFragmentStates
		|| !FMath::IsFinite(WorldState.GroundReactionAccumulator)
		|| WorldState.GroundReactionAccumulator < 0.0f
		|| WorldState.GroundReactionRevision < 0)
	{
		OutError = TEXT("save exceeds the supported world-state budget");
		return false;
	}
	TSet<FIntPoint> SeenTerrainSpanCells;
	int64 TerrainSpanCount = 0;
	for (const FMatterFluxTerrainSpanOverride& Column
		: WorldState.TerrainSpanOverrides)
	{
		if (SeenTerrainSpanCells.Contains(Column.WorldCell))
		{
			OutError = TEXT("save contains duplicate terrain span columns");
			return false;
		}
		int32 PreviousEndN = MIN_int32;
		FName PreviousMaterial = NAME_None;
		for (const FMatterFluxTerrainSpanState& Span : Column.Spans)
		{
			++TerrainSpanCount;
			if (TerrainSpanCount > MaximumMaskCells
				|| Span.BeginN >= Span.EndNExclusive
				|| Span.BeginN < PreviousEndN
				|| Span.MaterialId.IsNone()
				|| (Span.BeginN == PreviousEndN
					&& Span.MaterialId == PreviousMaterial))
			{
				OutError = TEXT("save contains an invalid terrain span column");
				return false;
			}
			PreviousEndN = Span.EndNExclusive;
			PreviousMaterial = Span.MaterialId;
		}
		TSet<int32> SeenEnergyCells;
		for (const FMatterFluxTerrainEnergyState& Energy
			: Column.EnergyOverrides)
		{
			++TerrainSpanCount;
			if (TerrainSpanCount > MaximumMaskCells
				|| Energy.N < 0
				|| SeenEnergyCells.Contains(Energy.N))
			{
				OutError = TEXT("save contains an invalid terrain energy field");
				return false;
			}
			SeenEnergyCells.Add(Energy.N);
		}
		if (Column.bHasSettledSurface
			&& (Column.SettledSurfaceN < 0
				|| Column.SettledSurfaceFace > 5))
		{
			OutError = TEXT("save contains an invalid settled terrain surface");
			return false;
		}
		SeenTerrainSpanCells.Add(Column.WorldCell);
	}
	TSet<FGuid> SeenSourceIds;
	for (const FMatterFluxSavedFragmentSourceState& State
		: WorldState.FragmentSources)
	{
		if (!State.SourceId.IsValid()
			|| SeenSourceIds.Contains(State.SourceId)
			|| State.Revision < 0
			|| State.VolumeTopologyRevision < 0
			|| State.VolumeFieldRevision < 0
			|| State.VolumeCellStates.Num() > MaximumMaskCells
			|| !IsBinaryMask(State.RuntimeMask)
			|| !FMath::IsFinite(State.ReactionAccumulator)
			|| State.ReactionAccumulator < 0.0f
			|| State.TotalMaterialEmissionCount < 0
			|| !State.ActorTransform.IsValid()
			|| (State.bHasReactionState
				&& !IsValidReactionState(State.ReactionState)))
		{
			OutError = TEXT("save contains an invalid fragment-source state");
			return false;
		}
		TSet<FIntVector> SeenVolumeCells;
		for (const FMatterFluxSavedVolumeCellState& Cell
			: State.VolumeCellStates)
		{
			if (Cell.Cell.X < 0 || Cell.Cell.Y < 0 || Cell.Cell.Z != 0
				|| Cell.Cell.X >= MaximumMaskCells
				|| Cell.Cell.Y >= MaximumMaskCells
				|| Cell.MaterialId.IsNone()
				|| Cell.MaterialId == TEXT("empty")
				|| SeenVolumeCells.Contains(Cell.Cell))
			{
				OutError = TEXT("save contains an invalid fragment Volume cell state");
				return false;
			}
			SeenVolumeCells.Add(Cell.Cell);
		}
		SeenSourceIds.Add(State.SourceId);
	}
	TSet<FGuid> SeenRemovedSourceIds;
	for (const FGuid RemovedSourceId
		: WorldState.RemovedFragmentSourceIds)
	{
		if (!RemovedSourceId.IsValid()
			|| SeenRemovedSourceIds.Contains(RemovedSourceId)
			|| SeenSourceIds.Contains(RemovedSourceId))
		{
			OutError = TEXT("save contains an invalid removed-source reference");
			return false;
		}
		SeenRemovedSourceIds.Add(RemovedSourceId);
	}
	if (WorldState.bHasGroundReactionState
		&& !IsValidReactionState(WorldState.GroundReactionState))
	{
		OutError = TEXT("save contains an invalid ground-reaction state");
		return false;
	}
	TSet<FGuid> SeenIgnitionSourceIds;
	for (const FGuid SourceId : WorldState.SourcesThatActivatedGround)
	{
		if (!SourceId.IsValid()
			|| SeenIgnitionSourceIds.Contains(SourceId))
		{
			OutError = TEXT("save contains an invalid ignition-source reference");
			return false;
		}
		SeenIgnitionSourceIds.Add(SourceId);
	}
	if (SourceVersion <= 5)
	{
		// Deprecated fields survive one reader version only to detect incompatible
		// ReactionState above. A reaction-free V5 save carries no runtime reaction
		// fact into V6.
		for (FMatterFluxSavedFragmentSourceState& State
			: WorldState.FragmentSources)
		{
			State.ReactionState = FMatterFluxSavedReactionState();
			State.ReactionAccumulator = 0.0f;
			State.TotalMaterialEmissionCount = 0;
		}
		WorldState.GroundReactionState = FMatterFluxSavedReactionState();
		WorldState.GroundReactionAccumulator = 0.0f;
		WorldState.GroundReactionRevision = 0;
		WorldState.SourcesThatActivatedGround.Reset();
	}
	SaveVersion = CurrentVersion;
	return true;
}

void UMatterFluxSaveMetadata::Initialize()
{
	MetadataVersion = CurrentVersion;
	Slots.Reset();
}

bool UMatterFluxSaveMetadata::ValidateAndRepair()
{
	bool bWasValid = MetadataVersion == CurrentVersion;
	MetadataVersion = CurrentVersion;

	TSet<int32> SeenSlotIndices;
	TArray<FMatterFluxSaveSlotInfo> RepairedSlots;
	RepairedSlots.Reserve(Slots.Num());
	for (const FMatterFluxSaveSlotInfo& Slot : Slots)
	{
		const FString NormalizedName = NormalizeDisplayName(Slot.DisplayName);
		const bool bOccupiedSlotValid = Slot.bOccupied
			&& Slot.SlotIndex >= 0
			&& Slot.MapSeed > 0
			&& Slot.SavedAtUtc.GetTicks() > 0
			&& !SeenSlotIndices.Contains(Slot.SlotIndex);
		if (bOccupiedSlotValid)
		{
			SeenSlotIndices.Add(Slot.SlotIndex);
			FMatterFluxSaveSlotInfo& Repaired = RepairedSlots.Add_GetRef(Slot);
			Repaired.DisplayName = NormalizedName;
			bWasValid = bWasValid && NormalizedName == Slot.DisplayName;
		}
		else
		{
			bWasValid = false;
		}
	}
	RepairedSlots.Sort([](
		const FMatterFluxSaveSlotInfo& Left,
		const FMatterFluxSaveSlotInfo& Right)
	{
		return Left.SlotIndex < Right.SlotIndex;
	});
	if (bWasValid)
	{
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			if (Slots[Index].SlotIndex != RepairedSlots[Index].SlotIndex)
			{
				bWasValid = false;
				break;
			}
		}
	}
	Slots = MoveTemp(RepairedSlots);
	return bWasValid;
}

int32 UMatterFluxSaveMetadata::GetNextAvailableSlotIndex() const
{
	TSet<int32> UsedSlotIndices;
	for (const FMatterFluxSaveSlotInfo& Slot : Slots)
	{
		if (Slot.bOccupied && Slot.SlotIndex >= 0)
		{
			UsedSlotIndices.Add(Slot.SlotIndex);
		}
	}
	for (int32 Candidate = 0; Candidate < MAX_int32; ++Candidate)
	{
		if (!UsedSlotIndices.Contains(Candidate))
		{
			return Candidate;
		}
	}
	return INDEX_NONE;
}

FString UMatterFluxSaveMetadata::NormalizeDisplayName(
	const FString& InDisplayName)
{
	FString Result = InDisplayName.TrimStartAndEnd();
	Result.ReplaceInline(TEXT("\r"), TEXT(" "));
	Result.ReplaceInline(TEXT("\n"), TEXT(" "));
	Result.ReplaceInline(TEXT("\t"), TEXT(" "));
	if (Result.Len() > 32)
	{
		Result.LeftInline(32, EAllowShrinking::No);
	}
	return Result;
}
