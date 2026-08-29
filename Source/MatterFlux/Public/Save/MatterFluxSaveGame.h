#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Save/MatterFluxSaveTypes.h"
#include "MatterFluxSaveGame.generated.h"

UCLASS()
class MATTERFLUX_API UMatterFluxSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr int32 CurrentVersion = 6;

	void InitializeNew(int32 InMapSeed);
	bool ValidateAndMigrate(FString& OutError);

	UPROPERTY(SaveGame)
	int32 SaveVersion = CurrentVersion;

	UPROPERTY(SaveGame)
	FDateTime SavedAtUtc;

	UPROPERTY(SaveGame)
	int32 MapSeed = 0;

	/** Empty for procedural free mode; otherwise the Lua custom-map id. */
	UPROPERTY(SaveGame)
	FName CustomMapId = NAME_None;

	UPROPERTY(SaveGame)
	FTransform PlayerTransform = FTransform::Identity;

	UPROPERTY(SaveGame)
	FMatterFluxMagicInventorySaveState MagicInventory;

	UPROPERTY(SaveGame)
	FMatterFluxProgressionSaveState Progression;

	UPROPERTY(SaveGame)
	FMatterFluxWorldSaveState WorldState;
};

UCLASS()
class MATTERFLUX_API UMatterFluxSaveMetadata : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr int32 CurrentVersion = 2;

	void Initialize();
	bool ValidateAndRepair();
	int32 GetNextAvailableSlotIndex() const;
	static FString NormalizeDisplayName(const FString& InDisplayName);

	UPROPERTY(SaveGame)
	int32 MetadataVersion = CurrentVersion;

	UPROPERTY(SaveGame)
	TArray<FMatterFluxSaveSlotInfo> Slots;
};
