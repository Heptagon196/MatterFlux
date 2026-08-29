#include "FragmentTestActors.h"

#include "Components/InstancedStaticMeshComponent.h"

#include "Components/PointLightComponent.h"
#include "ProceduralMeshComponent.h"

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

void AMatterFluxReactionLightTestSourceActor::
	EnsureReactionVisualComponentsForTest()
{
	EnsureReactionVisualComponents();
}

void AMatterFluxReactionLightTestSourceActor::
	RebuildMaterialVisualizationForTest()
{
	RebuildMaterialVisualization();
}

bool AMatterFluxReactionLightTestSourceActor::
	IsReactionFireLightVisibleForTest() const
{
	return FireLight && FireLight->IsVisible();
}

bool AMatterFluxReactionLightTestSourceActor::
	AreReactionFlamesVisibleForTest() const
{
	return FlameInstances && FlameInstances->IsVisible();
}

int32 AMatterFluxReactionLightTestSourceActor::
	GetReactionFlameInstanceCountForTest() const
{
	return FlameInstances ? FlameInstances->GetInstanceCount() : 0;
}

bool AMatterFluxReactionLightTestSourceActor::
	IsAnySourceMeshSectionVisibleForTest() const
{
	if (!MeshComponent)
	{
		return false;
	}
	for (int32 SectionIndex = 0;
		SectionIndex < MeshComponent->GetNumSections();
		++SectionIndex)
	{
		if (MeshComponent->IsMeshSectionVisible(SectionIndex))
		{
			return true;
		}
	}
	return false;
}
