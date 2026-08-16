#pragma once

#include "CoreMinimal.h"
#include "MatterFluxSaveTypes.generated.h"

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedSpell
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FName SpellId = NAME_None;

	UPROPERTY(SaveGame)
	int32 Quantity = 0;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedWand
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FGuid InstanceId;

	UPROPERTY(SaveGame)
	FName DefinitionId = NAME_None;

	UPROPERTY(SaveGame)
	TArray<FName> SpellSlots;

	UPROPERTY(SaveGame)
	float Mana = 0.0f;

	UPROPERTY(SaveGame)
	int32 DeckCursor = 0;

	UPROPERTY(SaveGame)
	int32 CastSerial = 0;

	UPROPERTY(SaveGame)
	float CastCooldownRemaining = 0.0f;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxMagicInventorySaveState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TArray<FMatterFluxSavedSpell> Spells;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxSavedWand> Wands;

	UPROPERTY(SaveGame)
	TArray<FGuid> EquippedWands;

	UPROPERTY(SaveGame)
	int32 ActiveEquipmentSlot = 0;

	UPROPERTY(SaveGame)
	int32 InventoryRevision = 1;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedItemStack
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FName ItemId = NAME_None;

	UPROPERTY(SaveGame)
	int32 Quantity = 0;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedQuestState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FName QuestId = NAME_None;

	UPROPERTY(SaveGame)
	uint8 Status = 0;

	UPROPERTY(SaveGame)
	int32 Progress = 0;

	UPROPERTY(SaveGame)
	bool bActivationRewardsGranted = false;

	UPROPERTY(SaveGame)
	bool bCompletionRewardsGranted = false;
};

/** Versioned player progression snapshot independent of Lua VM state. */
USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxProgressionSaveState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TArray<FMatterFluxSavedItemStack> Items;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxSavedQuestState> Quests;

	UPROPERTY(SaveGame)
	FName SelectedQuest = NAME_None;

	UPROPERTY(SaveGame)
	int32 Revision = 1;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedCombustionState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FName RuleId = NAME_None;

	UPROPERTY(SaveGame)
	int32 Width = 0;

	UPROPERTY(SaveGame)
	int32 Height = 0;

	UPROPERTY(SaveGame)
	int32 Seed = 0;

	UPROPERTY(SaveGame)
	uint32 Tick = 0;

	UPROPERTY(SaveGame)
	TArray<uint8> FuelMask;

	UPROPERTY(SaveGame)
	TArray<uint8> ResidueMask;

	UPROPERTY(SaveGame)
	TArray<uint8> BurningMask;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedFragmentSourceState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FGuid SourceId;

	UPROPERTY(SaveGame)
	int32 Revision = 0;

	UPROPERTY(SaveGame)
	TArray<uint8> RuntimeMask;

	UPROPERTY(SaveGame)
	bool bHasCombustionState = false;

	UPROPERTY(SaveGame)
	FMatterFluxSavedCombustionState CombustionState;

	UPROPERTY(SaveGame)
	float CombustionAccumulator = 0.0f;

	UPROPERTY(SaveGame)
	int32 TotalSmokeEmissionCount = 0;

	UPROPERTY(SaveGame)
	FTransform ActorTransform = FTransform::Identity;

	UPROPERTY(SaveGame)
	bool bDetachedFromTerrain = false;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxWorldSaveState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TArray<uint8> MaterialActiveState;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxSavedFragmentSourceState> FragmentSources;

	UPROPERTY(SaveGame)
	TArray<FGuid> RemovedFragmentSourceIds;

	UPROPERTY(SaveGame)
	bool bHasGroundCombustionState = false;

	UPROPERTY(SaveGame)
	FMatterFluxSavedCombustionState GroundCombustionState;

	UPROPERTY(SaveGame)
	float GroundCombustionAccumulator = 0.0f;

	UPROPERTY(SaveGame)
	int32 GroundCombustionRevision = 0;

	UPROPERTY(SaveGame)
	TArray<FGuid> SourcesThatIgnitedGround;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSaveSlotInfo
{
	GENERATED_BODY()

	UPROPERTY(SaveGame, BlueprintReadOnly)
	int32 SlotIndex = INDEX_NONE;

	UPROPERTY(SaveGame, BlueprintReadOnly)
	bool bOccupied = false;

	UPROPERTY(SaveGame, BlueprintReadOnly)
	int32 MapSeed = 0;

	UPROPERTY(SaveGame, BlueprintReadOnly)
	FDateTime SavedAtUtc;

	UPROPERTY(SaveGame, BlueprintReadOnly)
	FString DisplayName;
};
