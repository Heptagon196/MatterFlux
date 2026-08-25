#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Fragment/FragmentTypes.h"
#include "Fragment/FragmentSourceSpatialIndex.h"
#include "FragmentSimulationSubsystem.generated.h"

class AFragment2DSourceActor;
class AFragment2DActor;
class UMaterialInterface;

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnFragmentSourcePresenceChanged,
	const FGuid&,
	bool);

UCLASS()
class MATTERFLUX_API UFragmentSimulationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual void Deinitialize() override;

	/** Returns true only when the event was accepted and the source damage state was committed. */
	UFUNCTION(BlueprintCallable, Category = "Fragment")
	bool RequestFragmentDamage(AFragment2DSourceActor* SourceActor, const FFragmentDamageEvent& DamageEvent);

	/**
	 * Accepts one cut for every intersecting mask-backed source. Game worlds
	 * process accepted requests through a bounded FIFO; editor worlds execute
	 * synchronously and return the number of changed sources.
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragment")
	int32 RequestWorldCut(const FFragmentWorldCutRequest& Request);

	static bool ExecuteFragmentDamage(
		AFragment2DSourceActor* SourceActor,
		const FFragmentDamageEvent& DamageEvent);
	static bool ExecuteFragmentDamage(
		AFragment2DSourceActor* SourceActor,
		const FFragmentDamageEvent& DamageEvent,
		AFragment2DActor** OutPrimaryCarrier);
	static int32 ExecuteWorldCut(UWorld* World, const FFragmentWorldCutRequest& Request);
	/** Runtime switch used by cut-path diagnostics. Toggle with mf.Fragment.CutLog 0|1. */
	static bool IsCutLoggingEnabled();

	bool RegisterSourceActor(AFragment2DSourceActor& SourceActor);
	void UnregisterSourceActor(AFragment2DSourceActor& SourceActor);
	void GatherSourcesInBounds(
		const FBox& WorldBounds,
		TArray<AFragment2DSourceActor*>& OutSources);
	/** Returns the stable, de-duplicated union of registered actors intersecting any bound. */
	void GatherSourcesInBoundsMany(
		TConstArrayView<FBox> WorldBounds,
		TArray<AFragment2DSourceActor*>& OutSources);
	FOnFragmentSourcePresenceChanged& OnSourcePresenceChanged()
	{
		return SourcePresenceChanged;
	}

private:
	struct FQueuedFragmentSpawn
	{
		TSubclassOf<AFragment2DActor> ActorClass;
		FFragmentSpawnPayload Payload;
		TWeakObjectPtr<UMaterialInterface> Material;
		FLinearColor Color = FLinearColor::White;
	};

	void EnqueueRenderOnlyFragments(
		TSubclassOf<AFragment2DActor> ActorClass,
		const TArray<FFragmentSpawnPayload>& Payloads,
		UMaterialInterface* Material,
		const FLinearColor& Color);
	bool SpawnQueuedFragment(const FQueuedFragmentSpawn& QueuedSpawn);

	TMap<FGuid, TWeakObjectPtr<AFragment2DSourceActor>> RegisteredSources;
	MatterFlux::Fragment::FSourceSpatialIndex RegisteredSourceIndex;
	FOnFragmentSourcePresenceChanged SourcePresenceChanged;
	TArray<FQueuedFragmentSpawn> QueuedFragmentSpawns;
	int32 NextQueuedFragmentSpawn = 0;
	TArray<FFragmentWorldCutRequest> QueuedWorldCuts;
	int32 NextQueuedWorldCut = 0;
};
