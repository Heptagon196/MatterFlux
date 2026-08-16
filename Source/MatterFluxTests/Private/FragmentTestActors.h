#pragma once

#include "CoreMinimal.h"
#include "Fragment/Fragment2DActor.h"
#include "FragmentTestActors.generated.h"

UCLASS()
class AMatterFluxFailingFragmentActor : public AFragment2DActor
{
	GENERATED_BODY()

public:
	virtual bool InitializeFromPayload(const FFragmentSpawnPayload& Payload) override;
};

UCLASS()
class AMatterFluxNetworkTestFragmentActor : public AFragment2DActor
{
	GENERATED_BODY()

public:
	AMatterFluxNetworkTestFragmentActor();
};
