#pragma once

#include "CoreMinimal.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
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

UCLASS()
class AMatterFluxReactionLightTestSourceActor : public AFragment2DSourceActor
{
	GENERATED_BODY()

public:
	void EnsureReactionVisualComponentsForTest();
	void RebuildMaterialVisualizationForTest();
	bool IsReactionFireLightVisibleForTest() const;
	bool AreReactionFlamesVisibleForTest() const;
	int32 GetReactionFlameInstanceCountForTest() const;
	bool IsAnySourceMeshSectionVisibleForTest() const;
};
