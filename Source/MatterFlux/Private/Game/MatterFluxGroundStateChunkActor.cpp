#include "Game/MatterFluxGroundStateChunkActor.h"

#include "Game/MatterFluxPlayableWorldActor.h"
#include "Net/UnrealNetwork.h"

AMatterFluxGroundStateChunkActor::AMatterFluxGroundStateChunkActor()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = false;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(10.0f);
}

void AMatterFluxGroundStateChunkActor::PostNetInit()
{
	Super::PostNetInit();
	ApplyStateToOwner();
}

void AMatterFluxGroundStateChunkActor::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMatterFluxGroundStateChunkActor, State);
}

bool AMatterFluxGroundStateChunkActor::InitializeState(
	AMatterFluxPlayableWorldActor& InWorldActor,
	const FMatterFluxGroundStateChunk& InState)
{
	if (InState.StateBytes.IsEmpty())
	{
		return false;
	}
	if (State.StateHash == InState.StateHash
		&& State.Revision == InState.Revision
		&& !State.StateBytes.IsEmpty())
	{
		return true;
	}
	SetOwner(&InWorldActor);
	State = InState;
	FlushNetDormancy();
	ForceNetUpdate();
	return true;
}

void AMatterFluxGroundStateChunkActor::OnRep_State()
{
	ApplyStateToOwner();
}

void AMatterFluxGroundStateChunkActor::ApplyStateToOwner()
{
	if (HasAuthority())
	{
		return;
	}
	if (AMatterFluxPlayableWorldActor* WorldActor =
		Cast<AMatterFluxPlayableWorldActor>(GetOwner()))
	{
		WorldActor->ApplyReplicatedGroundStateChunk(State);
	}
}
