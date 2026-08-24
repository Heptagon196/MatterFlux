#include "GAS/GA_CastWand.h"

#include "AbilitySystemComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Game/MatterFluxPlayerState.h"
#include "Game/MatterFluxCharacter.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Magic/MatterFluxMagicProjectile.h"
#include "MatterFluxGameplayTags.h"
#include "MatterFluxLog.h"

namespace MatterFluxCastWand
{
	constexpr int32 MaximumProjectileDepth = 4;
	constexpr int32 MaximumTotalProjectiles = 32;

	bool ValidateProjectile(
		const FMatterFluxMagicProjectilePlan& Projectile,
		const int32 Depth,
		int32& InOutCount)
	{
		if (Depth > MaximumProjectileDepth
			|| ++InOutCount > MaximumTotalProjectiles
			|| Projectile.SpellId.IsNone()
			|| !FMath::IsFinite(Projectile.Damage)
			|| !FMath::IsFinite(Projectile.Speed)
			|| !FMath::IsFinite(Projectile.Lifetime)
			|| !FMath::IsFinite(Projectile.Radius)
			|| !FMath::IsFinite(Projectile.GravityScale)
			|| !FMath::IsFinite(Projectile.SpreadDegrees)
			|| !FMath::IsFinite(Projectile.SpawnAngleDegrees)
			|| !FMath::IsFinite(Projectile.OrbitRadius)
			|| !FMath::IsFinite(Projectile.Color.R)
			|| !FMath::IsFinite(Projectile.Color.G)
			|| !FMath::IsFinite(Projectile.Color.B)
			|| !FMath::IsFinite(Projectile.Color.A)
			|| Projectile.Damage < 0.0f
			|| Projectile.Speed <= 0.0f
			|| Projectile.Lifetime <= 0.0f
			|| Projectile.Lifetime > 30.0f
			|| Projectile.Radius <= 0.0f
			|| Projectile.Radius > 100.0f
			|| Projectile.GravityScale < 0.0f
			|| Projectile.GravityScale > 4.0f
			|| Projectile.MaterialAmount < 1
			|| Projectile.MaterialAmount > 64
			|| Projectile.OrbitRadius < 0.0f
			|| Projectile.OrbitRadius > 10000.0f)
		{
			return false;
		}
		for (const FMatterFluxMagicProjectilePlan& Child
			: Projectile.OnImpactProjectiles)
		{
			if (!ValidateProjectile(Child, Depth + 1, InOutCount))
			{
				return false;
			}
		}
		for (const FMatterFluxMagicProjectilePlan& Child
			: Projectile.OnExpireProjectiles)
		{
			if (!ValidateProjectile(Child, Depth + 1, InOutCount))
			{
				return false;
			}
		}
		return true;
	}
}

UGA_CastWand::UGA_CastWand()
{
	InstancingPolicy =
		EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy =
		EGameplayAbilityNetExecutionPolicy::ServerOnly;
	FGameplayTagContainer Tags;
	Tags.AddTag(TAG_Ability_Player_CastWand);
	SetAssetTags(Tags);
}

void UGA_CastWand::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	AActor* Avatar = ActorInfo && ActorInfo->AvatarActor.IsValid()
		? ActorInfo->AvatarActor.Get()
		: nullptr;
	AMatterFluxPlayerState* PlayerState =
		ActorInfo && ActorInfo->OwnerActor.IsValid()
			? Cast<AMatterFluxPlayerState>(ActorInfo->OwnerActor.Get())
			: nullptr;
	UMatterFluxMagicInventoryComponent* Inventory = PlayerState
		? PlayerState->GetMagicInventory()
		: nullptr;
	if (!Avatar
		|| !Avatar->HasAuthority()
		|| !Inventory
		|| !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	const FGameplayAbilitySpec* AbilitySpec =
		ActorInfo->AbilitySystemComponent.IsValid()
			? ActorInfo->AbilitySystemComponent->FindAbilitySpecFromHandle(
				Handle)
			: nullptr;
	const int32 EquipmentSlot = AbilitySpec
		? AbilitySpec->InputID
		: INDEX_NONE;
	if (EquipmentSlot < 0
		|| EquipmentSlot >= EquipmentSlotCount)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	if (ActivationSerial == MAX_int32)
	{
		ActivationSerial = 0;
	}
	const int32 EventSeed = static_cast<int32>(HashCombineFast(
		GetTypeHash(PlayerState->GetPlayerId()),
		HashCombineFast(
			GetTypeHash(EquipmentSlot),
			GetTypeHash(ActivationSerial++))));
	FMatterFluxWandCastPlan Plan;
	FGuid WandId;
	FString Error;
	const bool bExecuted = Inventory->ExecuteCastAuthority(
		EquipmentSlot,
		EventSeed,
		[Avatar, EventSeed](const FMatterFluxWandCastPlan& Candidate)
		{
			return SpawnCastPlan(*Avatar, Candidate, EventSeed);
		},
		Plan,
		WandId,
		Error);
	if (!bExecuted)
	{
		UE_LOG(LogMatterFlux, Verbose,
			TEXT("Wand cast rejected for %s: %s"),
			*Avatar->GetName(),
			*Error);
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	UE_LOG(LogMatterFlux, Verbose,
		TEXT("Wand %s in slot %d cast %d projectile(s) and %d caster effect(s), mana spent %.1f"),
		*WandId.ToString(EGuidFormats::Digits),
		EquipmentSlot,
		Plan.Projectiles.Num(),
		Plan.CasterEffects.Num(),
		Plan.ManaSpent);
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

bool UGA_CastWand::SpawnCastPlan(
	AActor& Avatar,
	const FMatterFluxWandCastPlan& Plan,
	const int32 EventSeed)
{
	UWorld* World = Avatar.GetWorld();
	if (!World
		|| !Avatar.HasAuthority()
		|| (Plan.Projectiles.IsEmpty() && Plan.CasterEffects.IsEmpty())
		|| Plan.Projectiles.Num() > 32
		|| Plan.CasterEffects.Num() > 32)
	{
		return false;
	}
	int32 TotalProjectileCount = 0;
	for (const FMatterFluxMagicProjectilePlan& Projectile : Plan.Projectiles)
	{
		if (!MatterFluxCastWand::ValidateProjectile(
			Projectile,
			0,
			TotalProjectileCount))
		{
			return false;
		}
	}
	FVector Forward = Avatar.GetActorForwardVector();
	Forward.Z = 0.0f;
	if (!Forward.Normalize())
	{
		return false;
	}
	for (const FMatterFluxMagicCasterEffectPlan& Effect : Plan.CasterEffects)
	{
		const bool bCommonValid = !Effect.SpellId.IsNone();
		const bool bTypeValid =
			Effect.Type == EMatterFluxMagicCasterEffectType::Jump
				&& FMath::IsFinite(Effect.VerticalImpulse)
				&& Effect.VerticalImpulse > 0.0f
				&& Effect.VerticalImpulse <= 5000.0f
				&& Cast<AMatterFluxCharacter>(&Avatar) != nullptr;
		if (!bCommonValid || !bTypeValid)
		{
			return false;
		}
	}

	struct FDeferredProjectile
	{
		TObjectPtr<AMatterFluxMagicProjectile> Actor;
		FTransform Transform;
	};
	TArray<FDeferredProjectile> Deferred;
	Deferred.Reserve(Plan.Projectiles.Num());
	FVector CasterBoundsExtent = FVector::ZeroVector;
	FVector CasterBoundsOrigin = Avatar.GetActorLocation();
	if (const UPrimitiveComponent* RootPrimitive =
		Cast<UPrimitiveComponent>(Avatar.GetRootComponent()))
	{
		const FBoxSphereBounds& Bounds = RootPrimitive->Bounds;
		CasterBoundsOrigin = Bounds.Origin;
		CasterBoundsExtent = Bounds.BoxExtent;
	}
	else
	{
		Avatar.GetActorBounds(
			true,
			CasterBoundsOrigin,
			CasterBoundsExtent);
	}
	for (int32 Index = 0; Index < Plan.Projectiles.Num(); ++Index)
	{
		const FMatterFluxMagicProjectilePlan& Projectile =
			Plan.Projectiles[Index];
		const FVector Direction = Forward.RotateAngleAxis(
			Projectile.SpawnAngleDegrees,
			FVector::UpVector);
		const float DirectionalCasterExtent =
			FMath::Abs(Direction.X) * CasterBoundsExtent.X
			+ FMath::Abs(Direction.Y) * CasterBoundsExtent.Y
			+ FMath::Abs(Direction.Z) * CasterBoundsExtent.Z;
		const float SpawnDistance = FMath::Max(
			80.0f,
			DirectionalCasterExtent + Projectile.Radius + 8.0f);
		const FTransform Transform(
			Direction.Rotation(),
			CasterBoundsOrigin
				+ FVector(0.0f, 0.0f, 25.0f)
				+ Direction * SpawnDistance);
		AMatterFluxMagicProjectile* Spawned =
			World->SpawnActorDeferred<AMatterFluxMagicProjectile>(
				AMatterFluxMagicProjectile::StaticClass(),
				Transform,
				&Avatar,
				Cast<APawn>(&Avatar),
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Spawned)
		{
			for (FDeferredProjectile& Existing : Deferred)
			{
				if (Existing.Actor)
				{
					Existing.Actor->Destroy();
				}
			}
			return false;
		}
		Spawned->InitializeProjectile(
			Projectile,
			EventSeed ^ (Index * 0x1f1f + 0x713));
		FDeferredProjectile& Entry = Deferred.AddDefaulted_GetRef();
		Entry.Actor = Spawned;
		Entry.Transform = Transform;
	}
	for (FDeferredProjectile& Entry : Deferred)
	{
		UGameplayStatics::FinishSpawningActor(Entry.Actor, Entry.Transform);
	}
	for (int32 Index = 0; Index < Plan.CasterEffects.Num(); ++Index)
	{
		const FMatterFluxMagicCasterEffectPlan& Effect =
			Plan.CasterEffects[Index];
		if (AMatterFluxCharacter* Character =
			Cast<AMatterFluxCharacter>(&Avatar))
		{
			if (UCharacterMovementComponent* Movement =
				Character->GetCharacterMovement())
			{
				Movement->Velocity.Z = Effect.VerticalImpulse;
				Movement->SetMovementMode(MOVE_Falling);
			}
		}
	}
	return true;
}
