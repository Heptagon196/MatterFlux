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

	virtual void StartPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(
		AController* NewPlayer,
		const FTransform& SpawnTransform) override;

	UPROPERTY(EditDefaultsOnly, Category = "Playable World")
	TSubclassOf<AMatterFluxPlayableWorldActor> PlayableWorldClass;

	/** Spacing used when several players share a map with one PlayerStart. */
	UPROPERTY(EditDefaultsOnly, Category = "Multiplayer", meta = (ClampMin = "80.0"))
	float MultiplayerSpawnSpacing = 180.0f;

private:
	void RefreshConfiguredCreatureSpawns();
	bool ShouldSpawnCreature(FName SpawnQuestId) const;

	FTimerHandle CreatureSpawnTimer;
	TSet<FName> SpawnedCreatureDefinitions;
};
