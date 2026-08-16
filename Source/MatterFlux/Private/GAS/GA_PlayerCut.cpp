#include "GAS/GA_PlayerCut.h"

#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxCharacter.h"
#include "MatterFluxGameplayTags.h"
#include "MatterFluxLog.h"

UGA_PlayerCut::UGA_PlayerCut()
{
	InstancingPolicy =
		EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy =
		EGameplayAbilityNetExecutionPolicy::ServerOnly;
	FGameplayTagContainer Tags;
	Tags.AddTag(TAG_Ability_Player_Cut);
	SetAssetTags(Tags);
}

void UGA_PlayerCut::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	UE_LOG(
		LogMatterFlux,
		Verbose,
		TEXT("PlayerCut activation entered: avatar=%s authority=%d handle=%s."),
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
	if (ActivationSerial > MAX_int32 - 31000)
	{
		ActivationSerial = 0;
	}
	const int32 EventSeed = 31000 + ActivationSerial;
	++ActivationSerial;
	const int32 DamagedTargets = ExecuteForwardCut(
		Avatar,
		Range,
		TargetRadius,
		CutThickness,
		DamagePower,
		EventSeed);
	UE_LOG(
		LogMatterFlux,
		Verbose,
		TEXT("PlayerCut authoritative execution: avatar=%s damaged=%d seed=%d."),
		*Avatar.GetName(),
		DamagedTargets,
		EventSeed);
	if (AMatterFluxCharacter* Character =
		Cast<AMatterFluxCharacter>(&Avatar))
	{
		Character->BroadcastAbilityEffect(
			EMatterFluxPlayerAbilityEffect::Cut);
	}
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

int32 UGA_PlayerCut::ExecuteForwardCut(
	AActor& Avatar,
	const float Range,
	const float TargetRadius,
	const float CutThickness,
	const float DamagePower,
	const int32 EventSeed)
{
	UWorld* World = Avatar.GetWorld();
	if (!World
		|| (World->IsGameWorld() && !Avatar.HasAuthority())
		|| !FMath::IsFinite(Range)
		|| !FMath::IsFinite(TargetRadius)
		|| !FMath::IsFinite(CutThickness)
		|| !FMath::IsFinite(DamagePower)
		|| Range <= 0.0f
		|| TargetRadius <= 0.0f
		|| CutThickness <= 0.0f
		|| DamagePower < 0.0f)
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
	const FVector End = Start + Direction * Range;
	FFragmentDamageShape Shape;
	Shape.Type = EFragmentDamageShapeType::Line;
	Shape.WorldTransform = FTransform(
		Direction.Rotation(),
		(Start + End) * 0.5f);
	Shape.Extents.X = Range;
	Shape.Thickness = CutThickness;

	FFragmentWorldCutRequest Request;
	Request.CutShape = Shape;
	Request.DamagePower = DamagePower;
	Request.EventSeed = EventSeed;
	Request.TargetPadding = TargetRadius;
	if (UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>())
	{
		return Subsystem->RequestWorldCut(Request);
	}
	return UFragmentSimulationSubsystem::ExecuteWorldCut(
		World,
		Request);
}
