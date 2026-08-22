#include "GAS/GA_PlayerFlameJet.h"

#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "MatterFluxGameplayTags.h"
#include "MatterFluxLog.h"

UGA_PlayerFlameJet::UGA_PlayerFlameJet()
{
	InstancingPolicy =
		EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy =
		EGameplayAbilityNetExecutionPolicy::ServerOnly;
	FGameplayTagContainer Tags;
	Tags.AddTag(TAG_Ability_Player_FlameJet);
	SetAssetTags(Tags);
}

void UGA_PlayerFlameJet::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	UE_LOG(
		LogMatterFlux,
		Verbose,
		TEXT("FlameJet activation entered: avatar=%s authority=%d handle=%s."),
		ActorInfo && ActorInfo->AvatarActor.IsValid()
			? *ActorInfo->AvatarActor->GetName()
			: TEXT("None"),
		ActorInfo && ActorInfo->AvatarActor.IsValid()
			? ActorInfo->AvatarActor->HasAuthority()
			: false,
		*Handle.ToString());
	if (!ActorInfo
		|| !ActorInfo->AvatarActor.IsValid()
		|| !ActorInfo->AvatarActor->HasAuthority()
		|| !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	AActor& Avatar = *ActorInfo->AvatarActor.Get();
	if (ActivationSerial > MAX_int32 - 41000)
	{
		ActivationSerial = 0;
	}
	const int32 EventSeed = 41000 + ActivationSerial;
	++ActivationSerial;
	const int32 IgnitedTargets = ExecuteFlameJet(
		Avatar,
		Range,
		StartRadius,
		EndRadius,
		FlameMaterial,
		EventSeed);
	UE_LOG(
		LogMatterFlux,
		Verbose,
		TEXT("FlameJet authoritative execution: avatar=%s ignited=%d seed=%d."),
		*Avatar.GetName(),
		IgnitedTargets,
		EventSeed);
	if (AMatterFluxCharacter* Character =
		Cast<AMatterFluxCharacter>(&Avatar))
	{
		Character->BroadcastAbilityEffect(
			EMatterFluxPlayerAbilityEffect::FlameJet);
	}
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

int32 UGA_PlayerFlameJet::ExecuteFlameJet(
	AActor& Avatar,
	const float Range,
	const float StartRadius,
	const float EndRadius,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	UWorld* World = Avatar.GetWorld();
	if (!World
		|| (World->IsGameWorld() && !Avatar.HasAuthority())
		|| !FMath::IsFinite(Range)
		|| !FMath::IsFinite(StartRadius)
		|| !FMath::IsFinite(EndRadius)
		|| Range <= 0.0f
		|| StartRadius <= 0.0f
		|| EndRadius < StartRadius
		|| FlameMaterial.IsNone())
	{
		return 0;
	}

	FVector Direction = Avatar.GetActorForwardVector();
	Direction.Z = 0.0f;
	if (!Direction.Normalize())
	{
		return 0;
	}

	const FVector Start = Avatar.GetActorLocation();
	int32 IgnitedTargets = 0;
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		IgnitedTargets += It->IgniteLogicalFragmentSourcesInCone(
			Start,
			Direction,
			Range,
			StartRadius,
			EndRadius,
			FlameMaterial,
			EventSeed);
	}
	const FVector End = Start + Direction * Range;
	const float MaximumRadius = FMath::Max(StartRadius, EndRadius);
	const FBox FlameBounds = FBox::BuildAABB(
		(Start + End) * 0.5,
		(End - Start).GetAbs() * 0.5 + FVector(MaximumRadius));
	TArray<AFragment2DSourceActor*> CandidateSources;
	if (UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>())
	{
		Subsystem->GatherSourcesInBounds(
			FlameBounds,
			CandidateSources);
	}
	for (AFragment2DSourceActor* Source : CandidateSources)
	{
		if (!IsValid(Source)
			|| Source->IsActorBeingDestroyed()
			|| Source->bBroken
			|| !Source->HasCombustionRule())
		{
			continue;
		}

		const FBox Bounds = Source->GetComponentsBoundingBox(true);
		if (!Bounds.IsValid)
		{
			continue;
		}
		const float Along = FVector::DotProduct(
			Bounds.GetCenter() - Start,
			Direction);
		if (Along < 0.0f || Along > Range)
		{
			continue;
		}
		const FVector CenterlinePoint = Start + Direction * Along;
		const float Radius = FMath::Lerp(
			StartRadius,
			EndRadius,
			Along / Range);
		if (Bounds.ComputeSquaredDistanceToPoint(CenterlinePoint)
			> FMath::Square(Radius))
		{
			continue;
		}

		const FVector IgnitionPoint =
			Bounds.GetClosestPointTo(CenterlinePoint);
		if (Source->IgniteAtWorldLocation(
			IgnitionPoint,
			FlameMaterial,
			EventSeed ^ static_cast<int32>(
				GetTypeHash(Source->SourceId))))
		{
			++IgnitedTargets;
		}
	}

	// Detached pieces are rigid-body carriers rather than Source Actors. They
	// still use the same Lua combustion rules, so normal wand fire must query
	// them through the same cone instead of requiring a debug-only ignition path.
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		AFragment2DActor* Fragment = *It;
		if (!IsValid(Fragment) || Fragment->IsActorBeingDestroyed())
		{
			continue;
		}
		const FBox Bounds = Fragment->GetCombustibleWorldBounds();
		if (!Bounds.IsValid || !Bounds.Intersect(FlameBounds))
		{
			continue;
		}
		if (Fragment->IgniteInCone(
			Start,
			Direction,
			Range,
			StartRadius,
			EndRadius,
			FlameMaterial,
			EventSeed ^ static_cast<int32>(
				GetTypeHash(Fragment->SpawnPayload.FragmentId))))
		{
			++IgnitedTargets;
		}
	}

	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World);
		It;
		++It)
	{
		constexpr int32 GroundSamples = 8;
		bool bIgnitedWorld = false;
		for (int32 Index = 1; Index <= GroundSamples; ++Index)
		{
			const FVector Point = Start
				+ Direction
					* (Range * static_cast<float>(Index)
						/ static_cast<float>(GroundSamples));
			bIgnitedWorld |= It->IgniteGroundAtWorldLocation(
				Point,
				EventSeed ^ Index);
		}
		IgnitedTargets += bIgnitedWorld ? 1 : 0;
	}
	return IgnitedTargets;
}
