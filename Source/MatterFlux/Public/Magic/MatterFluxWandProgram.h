#pragma once

#include "CoreMinimal.h"
#include "MatterFluxContentTypes.h"

struct MATTERFLUX_API FMatterFluxWandProgramState
{
	float Mana = 0.0f;
	int32 DeckCursor = 0;
	int32 CastSerial = 0;

	bool operator==(const FMatterFluxWandProgramState& Other) const
	{
		return Mana == Other.Mana
			&& DeckCursor == Other.DeckCursor
			&& CastSerial == Other.CastSerial;
	}
};

struct MATTERFLUX_API FMatterFluxMagicProjectilePlan
{
	FName SpellId;
	float Damage = 0.0f;
	float Speed = 0.0f;
	float Lifetime = 0.0f;
	float Radius = 0.0f;
	float GravityScale = 0.0f;
	float SpreadDegrees = 0.0f;
	float SpawnAngleDegrees = 0.0f;
	bool bOverrideColor = false;
	FLinearColor Color = FLinearColor::White;
	float OrbitRadius = 0.0f;
	FName BodyMaterial;
	int32 MaterialAmount = 1;
	bool bUsePlaneVisual = false;
	bool bUseVerticalPlaneVisual = false;
	TArray<FMatterFluxMagicProjectilePlan> OnImpactProjectiles;
	TArray<FMatterFluxMagicProjectilePlan> OnExpireProjectiles;
	bool bTriggerRandomDirection = false;

	bool operator==(
		const FMatterFluxMagicProjectilePlan& Other) const
	{
		return SpellId == Other.SpellId
			&& Damage == Other.Damage
			&& Speed == Other.Speed
			&& Lifetime == Other.Lifetime
			&& Radius == Other.Radius
			&& GravityScale == Other.GravityScale
			&& SpreadDegrees == Other.SpreadDegrees
			&& SpawnAngleDegrees == Other.SpawnAngleDegrees
			&& bOverrideColor == Other.bOverrideColor
			&& Color == Other.Color
			&& OrbitRadius == Other.OrbitRadius
			&& BodyMaterial == Other.BodyMaterial
			&& MaterialAmount == Other.MaterialAmount
			&& bUsePlaneVisual == Other.bUsePlaneVisual
			&& bUseVerticalPlaneVisual == Other.bUseVerticalPlaneVisual
			&& OnImpactProjectiles == Other.OnImpactProjectiles
			&& OnExpireProjectiles == Other.OnExpireProjectiles
			&& bTriggerRandomDirection == Other.bTriggerRandomDirection;
	}
};

enum class EMatterFluxMagicCasterEffectType : uint8
{
	Jump
};

struct MATTERFLUX_API FMatterFluxMagicCasterEffectPlan
{
	FName SpellId;
	EMatterFluxMagicCasterEffectType Type =
		EMatterFluxMagicCasterEffectType::Jump;
	float VerticalImpulse = 0.0f;

	bool operator==(const FMatterFluxMagicCasterEffectPlan& Other) const
	{
		return SpellId == Other.SpellId
			&& Type == Other.Type
			&& VerticalImpulse == Other.VerticalImpulse;
	}
};

struct MATTERFLUX_API FMatterFluxWandCastPlan
{
	TArray<FMatterFluxMagicProjectilePlan> Projectiles;
	TArray<FMatterFluxMagicCasterEffectPlan> CasterEffects;
	FMatterFluxWandProgramState NextState;
	float ManaSpent = 0.0f;
	float CastDelay = 0.0f;
	float RechargeTime = 0.0f;
	bool bDeckExhausted = false;

	bool operator==(const FMatterFluxWandCastPlan& Other) const
	{
		return Projectiles == Other.Projectiles
			&& CasterEffects == Other.CasterEffects
			&& NextState == Other.NextState
			&& ManaSpent == Other.ManaSpent
			&& CastDelay == Other.CastDelay
			&& RechargeTime == Other.RechargeTime
			&& bDeckExhausted == Other.bDeckExhausted;
	}
};

/**
 * Compiles one wand cast without side effects. Callers commit NextState only
 * after the returned plan has been accepted by the authoritative game state.
 */
class MATTERFLUX_API FMatterFluxWandProgram
{
public:
	static bool Evaluate(
		const FMatterFluxContentRegistry& Registry,
		FName WandDefinitionId,
		TConstArrayView<FName> SpellSlots,
		const FMatterFluxWandProgramState& CurrentState,
		int32 EventSeed,
		FMatterFluxWandCastPlan& OutPlan,
		FString& OutError);
};
