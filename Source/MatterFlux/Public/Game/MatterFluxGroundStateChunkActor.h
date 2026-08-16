#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Material/MatterFluxGroundStateChunk.h"
#include "MatterFluxGroundStateChunkActor.generated.h"

class AMatterFluxPlayableWorldActor;

UCLASS(NotBlueprintable)
class MATTERFLUX_API AMatterFluxGroundStateChunkActor : public AActor
{
	GENERATED_BODY()

public:
	AMatterFluxGroundStateChunkActor();
	virtual void PostNetInit() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	bool InitializeState(
		AMatterFluxPlayableWorldActor& InWorldActor,
		const FMatterFluxGroundStateChunk& InState);
	int32 GetPayloadByteCount() const { return State.StateBytes.Num(); }
	FIntPoint GetChunkCoordinate() const { return State.ChunkCoordinate; }

private:
	UFUNCTION()
	void OnRep_State();
	void ApplyStateToOwner();

	UPROPERTY(ReplicatedUsing = OnRep_State)
	FMatterFluxGroundStateChunk State;
};
