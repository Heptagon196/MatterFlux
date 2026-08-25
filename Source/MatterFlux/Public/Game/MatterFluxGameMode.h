#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MatterFluxGameMode.generated.h"

class AMatterFluxPlayableWorldActor;

UCLASS()
class MATTERFLUX_API AMatterFluxGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMatterFluxGameMode();
	/** Stable seed used only while the title-screen background is visible. */
	static constexpr int32 TitleBackgroundMapSeed = 1337;
	static int32 ResolveInitialPlayableWorldSeed(const bool bShowTitleScreen)
	{
		return bShowTitleScreen ? TitleBackgroundMapSeed : 0;
	}

	virtual void StartPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void HandleStartingNewPlayer_Implementation(
		APlayerController* NewPlayer) override;
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(
		AController* NewPlayer,
		const FTransform& SpawnTransform) override;
	/** Closes the creature/player entry gate for a newly generated world. */
	void PrepareForInitialWorldEntry();
	/** Reopens configured spawns after an existing pawn's loaded world is ready. */
	bool CompleteExistingPlayerWorldLoad();

	UPROPERTY(EditDefaultsOnly, Category = "Playable World")
	TSubclassOf<AMatterFluxPlayableWorldActor> PlayableWorldClass;

	/** Spacing used when several players share a map with one PlayerStart. */
	UPROPERTY(EditDefaultsOnly, Category = "Multiplayer", meta = (ClampMin = "80.0"))
	float MultiplayerSpawnSpacing = 180.0f;

private:
	friend class FMatterFluxInitialEntryOrderingTest;
	friend class FMatterFluxStoryMapPlayerEntryTest;
	friend class FMatterFluxStoryBossSpawnTest;

	struct FPendingPlayerEntry
	{
		TWeakObjectPtr<APlayerController> Controller;
		TWeakObjectPtr<AActor> StartSpot;
		FTransform SpawnTransform = FTransform::Identity;
		FIntPoint TerrainChunk = FIntPoint::ZeroValue;
		bool bSpawnRegionRequested = false;
		bool bUseSpawnTransform = false;
	};

	AMatterFluxPlayableWorldActor* FindPlayableWorld() const;
	void AdvancePendingPlayerEntries();
	void RefreshConfiguredCreatureSpawns();
	void ReleaseConfiguredCreatureTerrainPins(
		AMatterFluxPlayableWorldActor& PlayableWorld);
	bool ShouldActivateQuestCreatureSpawns(FName QuestId) const;

	FTimerHandle PlayerEntryTimer;
	FTimerHandle CreatureSpawnTimer;
	TArray<FPendingPlayerEntry> PendingPlayerEntries;
	TSet<FName> SpawnedCreatureWaves;
	/**
	 * Authored creatures are real physics actors even before a player reaches
	 * their region. Keep each marker's collision chunk resident so gravity
	 * cannot move a waiting quest creature through an unloaded floor.
	 */
	TMap<FName, FIntPoint> ConfiguredCreatureTerrainPins;
	FName SpawnedForCustomMapId = NAME_None;
	int32 SpawnedForCustomMapLoadSerial = INDEX_NONE;
	bool bCreatureSpawnModeInitialized = false;
	bool bCreatureSpawnGateOpen = false;
	bool bInitialCreaturePassComplete = false;
};
