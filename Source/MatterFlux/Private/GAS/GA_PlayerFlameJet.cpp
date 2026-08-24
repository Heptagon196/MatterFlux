#include "GAS/GA_PlayerFlameJet.h"

#include "GAS/GA_CastWand.h"
#include "Game/MatterFluxCharacter.h"
#include "Magic/MatterFluxWandProgram.h"
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
	const int32 SpawnedParticles = ExecuteFlameJet(
		Avatar,
		Range,
		StartRadius,
		EndRadius,
		StimulusMaterial,
		EventSeed);
	UE_LOG(
		LogMatterFlux,
		Verbose,
		TEXT("FlameJet authoritative execution: avatar=%s particles=%d seed=%d."),
		*Avatar.GetName(),
		SpawnedParticles,
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
	const FName StimulusMaterial,
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
		|| StimulusMaterial.IsNone())
	{
		return 0;
	}

	// This legacy ability is only an alternate producer of the same material
	// projectile used by Lua wands. It never selects targets or executes a
	// reaction itself; the deposited particle is resolved by the material step.
	constexpr float ParticleSpeed = 1200.0f;
	FMatterFluxWandCastPlan Plan;
	FMatterFluxMagicProjectilePlan& Particle =
		Plan.Projectiles.AddDefaulted_GetRef();
	Particle.SpellId = TEXT("spell.legacy_material_jet");
	Particle.Speed = ParticleSpeed;
	Particle.Lifetime = FMath::Clamp(Range / ParticleSpeed, 0.05f, 30.0f);
	Particle.Radius = FMath::Clamp(
		FMath::Max(StartRadius, EndRadius),
		2.0f,
		100.0f);
	Particle.BodyMaterial = StimulusMaterial;
	Particle.MaterialAmount = 1;
	return UGA_CastWand::SpawnCastPlan(Avatar, Plan, EventSeed) ? 1 : 0;
}
