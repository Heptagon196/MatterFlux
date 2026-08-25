#include "Magic/MatterFluxMagicWorkbenchInteraction.h"

void FMatterFluxMagicWorkbenchInteraction::BuildEquipmentSlotPresentations(
	TArray<FMatterFluxMagicEquipmentSlotPresentation>& OutSlots)
{
	OutSlots.Reset(MatterFlux::Magic::EquipmentSlotCount);
	for (int32 SlotIndex = 0;
		SlotIndex < MatterFlux::Magic::EquipmentSlotCount;
		++SlotIndex)
	{
		FMatterFluxMagicEquipmentSlotPresentation& Slot =
			OutSlots.AddDefaulted_GetRef();
		Slot.SlotIndex = SlotIndex;
		switch (SlotIndex)
		{
		case 0:
			Slot.KeyLabel = TEXT("左键");
			Slot.KeyBadge = TEXT("L");
			break;
		case 1:
			Slot.KeyLabel = TEXT("右键");
			Slot.KeyBadge = TEXT("R");
			break;
		case 2:
			Slot.KeyLabel = TEXT("Q 键");
			Slot.KeyBadge = TEXT("Q");
			break;
		case 3:
			Slot.KeyLabel = TEXT("E 键");
			Slot.KeyBadge = TEXT("E");
			break;
		case 4:
			Slot.KeyLabel = TEXT("空格键");
			Slot.KeyBadge = TEXT("空格");
			break;
		default:
			Slot.KeyLabel = TEXT("未绑定");
			Slot.KeyBadge = TEXT("?");
			break;
		}
	}
}

FVector2D FMatterFluxMagicWorkbenchInteraction::GetSpellSlotSize()
{
	return FVector2D(72.0f, 72.0f);
}

FVector2D FMatterFluxMagicWorkbenchInteraction::GetSpellDragDecoratorSize()
{
	return GetSpellSlotSize();
}

FVector2D FMatterFluxMagicWorkbenchInteraction::CalculateSpellDragDecoratorPosition(
	const FVector2D& CursorScreenPosition)
{
	return CursorScreenPosition - GetSpellDragDecoratorSize() * 0.5f;
}

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
