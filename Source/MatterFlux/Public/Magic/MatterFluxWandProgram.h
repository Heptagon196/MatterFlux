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
	float SpreadDegrees = 0.0f;
	float SpawnAngleDegrees = 0.0f;
	bool bOverrideColor = false;
	FLinearColor Color = FLinearColor::White;
	float OrbitRadius = 0.0f;
	FName ImpactMaterial;
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
			&& SpreadDegrees == Other.SpreadDegrees
			&& SpawnAngleDegrees == Other.SpawnAngleDegrees
			&& bOverrideColor == Other.bOverrideColor
			&& Color == Other.Color
			&& OrbitRadius == Other.OrbitRadius
			&& ImpactMaterial == Other.ImpactMaterial
			&& OnImpactProjectiles == Other.OnImpactProjectiles
			&& OnExpireProjectiles == Other.OnExpireProjectiles
			&& bTriggerRandomDirection == Other.bTriggerRandomDirection;
	}
};

enum class EMatterFluxMagicWorldEffectType : uint8
{
	Cut,
	Flame,
	Jump
};

struct MATTERFLUX_API FMatterFluxMagicWorldEffectPlan
{
	FName SpellId;
	EMatterFluxMagicWorldEffectType Type =
		EMatterFluxMagicWorldEffectType::Cut;
	float Range = 0.0f;
	float StartRadius = 0.0f;
	float EndRadius = 0.0f;
	float Thickness = 0.0f;
	float Power = 0.0f;
	float VerticalImpulse = 0.0f;
	FName Material;

	bool operator==(const FMatterFluxMagicWorldEffectPlan& Other) const
	{
		return SpellId == Other.SpellId
			&& Type == Other.Type
			&& Range == Other.Range
			&& StartRadius == Other.StartRadius
			&& EndRadius == Other.EndRadius
			&& Thickness == Other.Thickness
			&& Power == Other.Power
			&& VerticalImpulse == Other.VerticalImpulse
			&& Material == Other.Material;
	}
};

struct MATTERFLUX_API FMatterFluxWandCastPlan
{
	TArray<FMatterFluxMagicProjectilePlan> Projectiles;
	TArray<FMatterFluxMagicWorldEffectPlan> WorldEffects;
	FMatterFluxWandProgramState NextState;
	float ManaSpent = 0.0f;
	float CastDelay = 0.0f;
	float RechargeTime = 0.0f;
	bool bDeckExhausted = false;

	bool operator==(const FMatterFluxWandCastPlan& Other) const
	{
		return Projectiles == Other.Projectiles
			&& WorldEffects == Other.WorldEffects
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
