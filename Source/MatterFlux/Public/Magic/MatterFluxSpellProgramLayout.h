#pragma once

#include "CoreMinimal.h"
#include "MatterFluxContentTypes.h"

struct MATTERFLUX_API FMatterFluxSpellProgramNode
{
	int32 SlotIndex = INDEX_NONE;
	int32 ParentSlotIndex = INDEX_NONE;
	int32 RootIndex = INDEX_NONE;
	int32 ChildIndex = INDEX_NONE;
	int32 SiblingCount = 0;

	bool IsRoot() const
	{
		return ParentSlotIndex == INDEX_NONE;
	}
};

struct MATTERFLUX_API FMatterFluxSpellProgramColumn
{
	TArray<FMatterFluxSpellProgramNode> Nodes;
};

/**
 * Deterministic presentation model for a prefix-encoded wand program.
 *
 * The replicated spell-slot array remains compact. Modifier, multicast and
 * trigger arity is interpreted here once so Slate, tests and future editors
 * agree on independent roots, depth columns and parent/branch connections.
 */
struct MATTERFLUX_API FMatterFluxSpellProgramLayout
{
	TArray<FMatterFluxSpellProgramColumn> Columns;
	TArray<int32> ReserveSlotIndices;

	int32 GetAccountedSlotCount() const;
};

class MATTERFLUX_API FMatterFluxSpellProgramLayoutBuilder
{
public:
	static int32 GetChildCount(const FMatterFluxSpellDefinition& Spell);

	static bool Build(
		const FMatterFluxContentRegistry& Registry,
		TConstArrayView<FName> SpellSlots,
		FMatterFluxSpellProgramLayout& OutLayout,
		FString& OutError);
};
