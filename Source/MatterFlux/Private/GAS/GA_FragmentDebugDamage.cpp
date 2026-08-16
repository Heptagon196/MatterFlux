#include "GAS/GA_FragmentDebugDamage.h"

#include "EngineUtils.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Fragment/FragmentTypes.h"
#include "MatterFluxGameplayTags.h"
#include "MatterFluxLog.h"

UGA_FragmentDebugDamage::UGA_FragmentDebugDamage()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;

	FGameplayTagContainer Tags;
	Tags.AddTag(TAG_Ability_Fragment_DebugDamage);
	SetAssetTags(Tags);
}

void UGA_FragmentDebugDamage::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!ActorInfo
		|| !ActorInfo->AvatarActor.IsValid()
		|| !ActorInfo->AvatarActor->HasAuthority()
		|| !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	UWorld* World = ActorInfo->AvatarActor->GetWorld();
	if (!World || !World->IsGameWorld())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	AFragment2DSourceActor* Source = FindDebugSourceActor(World);
	if (!Source)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("GameplayEvent.FragmentDamageRequested failed: no available Fragment2DSourceActor."));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	FFragmentDamageShape Shape;
	Shape.Type = EFragmentDamageShapeType::Line;
	Shape.WorldTransform = Source->GetActorTransform();
	Shape.Extents.X = LineLength;
	Shape.Thickness = LineThickness;

	FFragmentDamageEvent Event;
	Event.SourceId = Source->SourceId;
	Event.BaseRevision = Source->Revision;
	Event.DamageShape = Shape;
	Event.DamagePower = DamagePower;
	Event.EventSeed = 1337 + FMath::Min(
		Source->Revision,
		MAX_int32 - 1337);

	UE_LOG(LogMatterFlux, Log, TEXT("GameplayEvent.FragmentDamageRequested source=%s revision=%d seed=%d"), *Source->GetName(), Event.BaseRevision, Event.EventSeed);

	UFragmentSimulationSubsystem* Subsystem = World->GetSubsystem<UFragmentSimulationSubsystem>();
	const bool bRequested = Subsystem
		? Subsystem->RequestFragmentDamage(Source, Event)
		: UFragmentSimulationSubsystem::ExecuteFragmentDamage(Source, Event);

	EndAbility(Handle, ActorInfo, ActivationInfo, true, !bRequested);
}

AFragment2DSourceActor* UGA_FragmentDebugDamage::FindDebugSourceActor(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}

	AFragment2DSourceActor* FirstAvailableSource = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		AFragment2DSourceActor* Source = *It;
		if (!Source || Source->IsActorBeingDestroyed() || Source->IsHidden())
		{
			continue;
		}

		if (!FirstAvailableSource)
		{
			FirstAvailableSource = Source;
		}

		if (Source->ActorHasTag(TEXT("MF_Source_Test")))
		{
			return Source;
		}

#if WITH_EDITOR
		if (Source->GetActorLabel().Equals(TEXT("MF_Source_Test"), ESearchCase::IgnoreCase))
		{
			return Source;
		}
#endif
	}

	return FirstAvailableSource;
}
