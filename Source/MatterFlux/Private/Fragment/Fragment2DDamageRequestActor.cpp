#include "Fragment/Fragment2DDamageRequestActor.h"

#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "MatterFluxLog.h"

#if WITH_EDITOR
#include "UObject/UnrealType.h"
#endif

AFragment2DDamageRequestActor::AFragment2DDamageRequestActor()
{
	PrimaryActorTick.bCanEverTick = false;
}

bool AFragment2DDamageRequestActor::ExecuteRequest()
{
	bExecuteRequest = false;

	if (!SourceActor)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Damage request actor %s has no SourceActor."), *GetName());
		return false;
	}

	UWorld* World = GetWorld();
	UFragmentSimulationSubsystem* Subsystem = World ? World->GetSubsystem<UFragmentSimulationSubsystem>() : nullptr;

	FFragmentDamageShape Shape;
	Shape.Type = ShapeType;
	Shape.WorldTransform = GetActorTransform();
	Shape.Radius = Radius;
	Shape.Extents = Extents;
	Shape.Thickness = Thickness;

	FFragmentDamageEvent Event;
	Event.SourceId = SourceActor->SourceId;
	Event.BaseRevision = SourceActor->Revision;
	Event.DamageShape = Shape;
	Event.DamagePower = DamagePower;
	Event.EventSeed = EventSeed;

	const bool bResult = Subsystem
		? Subsystem->RequestFragmentDamage(SourceActor, Event)
		: UFragmentSimulationSubsystem::ExecuteFragmentDamage(SourceActor, Event);
	if (!Subsystem)
	{
		UE_LOG(LogMatterFlux, Log, TEXT("Damage request actor %s used direct fragment execution because the world subsystem was unavailable."), *GetName());
	}
#if WITH_EDITOR
	Modify();
#endif
	return bResult;
}

#if WITH_EDITOR
void AFragment2DDamageRequestActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AFragment2DDamageRequestActor, bExecuteRequest) && bExecuteRequest)
	{
		ExecuteRequest();
	}
}
#endif
