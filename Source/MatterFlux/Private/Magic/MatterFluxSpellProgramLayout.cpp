#include "Magic/MatterFluxSpellProgramLayout.h"

namespace MatterFluxSpellProgramLayout
{
	constexpr int32 MaximumLayoutDepth = 32;

	struct FBuildContext
	{
		const FMatterFluxContentRegistry& Registry;
		TConstArrayView<FName> Slots;
		FMatterFluxSpellProgramLayout& Layout;
		FString& Error;
		int32 NextSlotIndex = 0;
		int32 NextRootIndex = 0;

		bool AddNode(
			const int32 Depth,
			const int32 ParentSlotIndex,
			const int32 RootIndex,
			const int32 NodeChildIndex = INDEX_NONE,
			const int32 SiblingCount = 0)
		{
			if (NextSlotIndex >= Slots.Num())
			{
				return true;
			}
			if (Depth >= MaximumLayoutDepth)
			{
				Error = TEXT("spell program layout exceeded its depth budget");
				return false;
			}

			const int32 SlotIndex = NextSlotIndex++;
			if (Layout.Columns.Num() <= Depth)
			{
				Layout.Columns.SetNum(Depth + 1);
			}
			FMatterFluxSpellProgramNode& Node =
				Layout.Columns[Depth].Nodes.AddDefaulted_GetRef();
			Node.SlotIndex = SlotIndex;
			Node.ParentSlotIndex = ParentSlotIndex;
			Node.RootIndex = RootIndex;
			Node.ChildIndex = NodeChildIndex;
			Node.SiblingCount = SiblingCount;

			const FName SpellId = Slots[SlotIndex];
			if (SpellId.IsNone())
			{
				return true;
			}
			const FMatterFluxSpellDefinition* Spell =
				Registry.Spells.Find(SpellId);
			if (!Spell)
			{
				Error = FString::Printf(
					TEXT("spell slot %d references unknown spell '%s'"),
					SlotIndex,
					*SpellId.ToString());
				return false;
			}

			const int32 ChildCount =
				FMatterFluxSpellProgramLayoutBuilder::GetChildCount(*Spell);
			for (int32 ChildIndex = 0;
				ChildIndex < ChildCount && NextSlotIndex < Slots.Num();
				++ChildIndex)
			{
				if (!AddNode(
					Depth + 1,
					SlotIndex,
					RootIndex,
					ChildIndex,
					ChildCount))
				{
					return false;
				}
			}
			return true;
		}
	};
}

int32 FMatterFluxSpellProgramLayout::GetAccountedSlotCount() const
{
	int32 Count = ReserveSlotIndices.Num();
	for (const FMatterFluxSpellProgramColumn& Column : Columns)
	{
		Count += Column.Nodes.Num();
	}
	return Count;
}

int32 FMatterFluxSpellProgramLayoutBuilder::GetChildCount(
	const FMatterFluxSpellDefinition& Spell)
{
	switch (Spell.Kind)
	{
	case EMatterFluxSpellKind::Modifier:
	case EMatterFluxSpellKind::Multicast:
	case EMatterFluxSpellKind::TriggerModifier:
		return FMath::Max(0, Spell.DrawCount);
	case EMatterFluxSpellKind::Trigger:
		return FMath::Max(0, Spell.TriggerDrawCount);
	default:
		return 0;
	}
}

bool FMatterFluxSpellProgramLayoutBuilder::Build(
	const FMatterFluxContentRegistry& Registry,
	const TConstArrayView<FName> SpellSlots,
	FMatterFluxSpellProgramLayout& OutLayout,
	FString& OutError)
{
	OutLayout = FMatterFluxSpellProgramLayout();
	OutError.Reset();
	if (SpellSlots.IsEmpty())
	{
		OutError = TEXT("spell program layout requires at least one slot");
		return false;
	}

	MatterFluxSpellProgramLayout::FBuildContext Context{
		Registry,
		SpellSlots,
		OutLayout,
		OutError
	};
	while (Context.NextSlotIndex < SpellSlots.Num())
	{
		if (SpellSlots[Context.NextSlotIndex].IsNone())
		{
			OutLayout.ReserveSlotIndices.Add(Context.NextSlotIndex++);
			continue;
		}
		if (!Context.AddNode(
			0,
			INDEX_NONE,
			Context.NextRootIndex++))
		{
			OutLayout = FMatterFluxSpellProgramLayout();
			return false;
		}
	}
	return true;
}
