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

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedShopPurchase
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FName OfferKey = NAME_None;

	UPROPERTY(SaveGame)
	int32 PurchaseCount = 0;
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
	TArray<FMatterFluxSavedShopPurchase> ShopPurchases;

	UPROPERTY(SaveGame)
	FName SelectedQuest = NAME_None;

	UPROPERTY(SaveGame)
	int32 Revision = 1;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedReactionState
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
	TArray<uint8> InputMask;

	UPROPERTY(SaveGame)
	TArray<uint8> OutputMask;

	UPROPERTY(SaveGame)
	TArray<uint8> ActiveMask;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxSavedVolumeCellState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FIntVector Cell = FIntVector::ZeroValue;

	UPROPERTY(SaveGame)
	FName MaterialId = NAME_None;

	UPROPERTY(SaveGame)
	uint16 Energy = 0;
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
	int32 VolumeTopologyRevision = 0;

	UPROPERTY(SaveGame)
	int32 VolumeFieldRevision = 0;

	UPROPERTY(SaveGame)
	uint16 VolumeEnvironmentEnergy = 0;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxSavedVolumeCellState> VolumeCellStates;

	UPROPERTY(SaveGame)
	TArray<uint8> RuntimeMask;

	UPROPERTY(SaveGame)
	bool bHasReactionState = false;

	UPROPERTY(SaveGame)
	FMatterFluxSavedReactionState ReactionState;

	UPROPERTY(SaveGame)
	float ReactionAccumulator = 0.0f;

	UPROPERTY(SaveGame)
	int32 TotalMaterialEmissionCount = 0;

	UPROPERTY(SaveGame)
	FTransform ActorTransform = FTransform::Identity;

	UPROPERTY(SaveGame)
	bool bDetachedFromTerrain = false;
};

/** One sparse canonical terrain edit; also used directly for replication. */
USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxTerrainHeightOverride
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FIntPoint WorldCell = FIntPoint::ZeroValue;

	UPROPERTY(SaveGame)
	float Height = 0.0f;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxTerrainSpanState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	int32 BeginN = 0;

	UPROPERTY(SaveGame)
	int32 EndNExclusive = 0;

	UPROPERTY(SaveGame)
	FName MaterialId = TEXT("soil");
};

/** One sparse non-environment terrain-cell energy value. */
USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxTerrainEnergyState
{
	GENERATED_BODY()

	/** N coordinate inside the owning terrain column. */
	UPROPERTY(SaveGame)
	int32 N = 0;

	UPROPERTY(SaveGame)
	uint16 Energy = 0;
};

/**
 * Sparse authoritative terrain column, shared by V6 save and replication.
 * A column can carry only field edits while its procedural topology stays implicit.
 */
USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxTerrainSpanOverride
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FIntPoint WorldCell = FIntPoint::ZeroValue;

	/** Distinguishes an explicitly empty column from an implicit baseline column. */
	UPROPERTY(SaveGame)
	bool bHasTopologyOverride = false;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxTerrainSpanState> Spans;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxTerrainEnergyState> EnergyOverrides;

	/** Stable cave/terrain face chosen by the settled MaterialWorld column. */
	UPROPERTY(SaveGame)
	bool bHasSettledSurface = false;

	UPROPERTY(SaveGame)
	int32 SettledSurfaceN = 0;

	/** Serialized EMaterialSurfaceFace value; kept POD for save compatibility. */
	UPROPERTY(SaveGame)
	uint8 SettledSurfaceFace = 5;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxWorldSaveState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TArray<uint8> MaterialActiveState;

	/** Deterministic fixed step used by the authoritative local-reaction kernel. */
	UPROPERTY(SaveGame)
	uint32 LocalMaterialReactionStep = 0;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxTerrainHeightOverride> TerrainHeightOverrides;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxTerrainSpanOverride> TerrainSpanOverrides;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxSavedFragmentSourceState> FragmentSources;

	UPROPERTY(SaveGame)
	TArray<FGuid> RemovedFragmentSourceIds;

	UPROPERTY(SaveGame)
	bool bHasGroundReactionState = false;

	UPROPERTY(SaveGame)
	FMatterFluxSavedReactionState GroundReactionState;

	UPROPERTY(SaveGame)
	float GroundReactionAccumulator = 0.0f;

	UPROPERTY(SaveGame)
	int32 GroundReactionRevision = 0;

	UPROPERTY(SaveGame)
	TArray<FGuid> SourcesThatActivatedGround;
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
