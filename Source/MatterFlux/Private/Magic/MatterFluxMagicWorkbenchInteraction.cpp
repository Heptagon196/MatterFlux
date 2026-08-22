#include "Magic/MatterFluxMagicWorkbenchInteraction.h"

void FMatterFluxMagicWorkbenchInteraction::BuildSpellDropTargets(
	const FMatterFluxOwnedWand& Wand,
	TArray<int32>& OutSlotIndices)
{
	OutSlotIndices.Reset(Wand.SpellSlots.Num());
	for (int32 SlotIndex = 0;
		SlotIndex < Wand.SpellSlots.Num(); ++SlotIndex)
	{
		OutSlotIndices.Add(SlotIndex);
	}
}

bool FMatterFluxMagicWorkbenchInteraction::ResolveSpellDrop(
	const FMatterFluxMagicDragPayload& Payload,
	const FGuid& TargetWandId,
	const int32 TargetSpellSlot,
	FMatterFluxMagicEdit& OutEdit)
{
	OutEdit = FMatterFluxMagicEdit();
	if (!TargetWandId.IsValid() || TargetSpellSlot < 0)
	{
		return false;
	}
	OutEdit.WandId = TargetWandId;
	OutEdit.ToSpellSlot = TargetSpellSlot;
	switch (Payload.Source)
	{
	case EMatterFluxMagicDragSource::SpellInventory:
		if (Payload.SpellId.IsNone())
		{
			return false;
		}
		OutEdit.Type = EMatterFluxMagicEditType::AssignSpell;
		OutEdit.SpellId = Payload.SpellId;
		return true;

	case EMatterFluxMagicDragSource::WandSpellSlot:
		if (Payload.WandId != TargetWandId
			|| Payload.SpellSlot < 0
			|| Payload.SpellSlot == TargetSpellSlot)
		{
			return false;
		}
		OutEdit.Type = EMatterFluxMagicEditType::SwapSpellSlots;
		OutEdit.FromSpellSlot = Payload.SpellSlot;
		return true;

	default:
		return false;
	}
}
