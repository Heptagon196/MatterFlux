#include "FragmentTestActors.h"

bool AMatterFluxFailingFragmentActor::InitializeFromPayload(const FFragmentSpawnPayload& Payload)
{
	return false;
}

AMatterFluxNetworkTestFragmentActor::AMatterFluxNetworkTestFragmentActor()
{
	InitialLifeSpan = 0.0f;
	// Exact payload/movement replication is the purpose of this synthetic
	// actor. Production distance relevancy is exercised by the scale matrix.
	bAlwaysRelevant = true;
}
