#include "Magic/MatterFluxWandProgram.h"
#include "Magic/MatterFluxSpellProgramLayout.h"

#include "Misc/Crc.h"

namespace MatterFluxWandProgram
{
	constexpr int32 MaximumInstructionsPerCast = 128;
	constexpr int32 MaximumProjectilesPerCast = 32;
	constexpr int32 MaximumCasterEffectsPerCast = 32;
	constexpr int32 MaximumTriggerDepth = 4;

	struct FPendingModifiers
	{
		float DamageAdd = 0.0f;
		float DamageMultiplier = 1.0f;
		float SpeedMultiplier = 1.0f;
		float LifetimeMultiplier = 1.0f;
		float SpreadDegrees = 0.0f;
		bool bOverrideColor = false;
		FLinearColor Color = FLinearColor::White;
		float OrbitRadius = 0.0f;
	};

	struct FEvaluationContext
	{
		const FMatterFluxContentRegistry& Registry;
		const FMatterFluxWandDefinition& Wand;
		TConstArrayView<FName> Slots;
		TArray<int32> DrawOrder;
		FMatterFluxWandCastPlan Plan;
		FRandomStream Random;
		int32 InstructionCount = 0;
		int32 ProjectileCount = 0;
		int32 CasterEffectCount = 0;
		int32 RemainingSlotsBeforeWrap = 0;
		FString Error;

		FEvaluationContext(
			const FMatterFluxContentRegistry& InRegistry,
			const FMatterFluxWandDefinition& InWand,
			const TConstArrayView<FName> InSlots,
			const FMatterFluxWandProgramState& InState,
			const int32 Seed)
			: Registry(InRegistry)
			, Wand(InWand)
			, Slots(InSlots)
			, Random(Seed)
		{
			Plan.NextState = InState;
			Plan.CastDelay = InWand.CastDelay;
			Plan.RechargeTime = InWand.RechargeTime;

			// The workbench stores a compact prefix tree. Empty slots that are
			// children of a modifier, multicast, or trigger are executable blank
			// branches; only disconnected reserve capacity must be skipped. Build
			// the draw order from the same structural model used by the editor so
			// presentation and execution cannot disagree.
			FMatterFluxSpellProgramLayout Layout;
			if (!FMatterFluxSpellProgramLayoutBuilder::Build(
				InRegistry,
				InSlots,
				Layout,
				Error))
			{
				return;
			}
			DrawOrder.Reserve(InSlots.Num());
			for (const FMatterFluxSpellProgramColumn& Column : Layout.Columns)
			{
				for (const FMatterFluxSpellProgramNode& Node : Column.Nodes)
				{
					DrawOrder.Add(Node.SlotIndex);
				}
			}
			DrawOrder.Sort();
			if (InWand.bShuffle)
			{
				for (int32 Index = DrawOrder.Num() - 1;
					Index > 0;
					--Index)
				{
					const int32 SwapIndex = Random.RandRange(0, Index);
					DrawOrder.Swap(Index, SwapIndex);
				}
			}
			if (!DrawOrder.IsEmpty())
			{
				const int32 StartIndex =
					InState.DeckCursor % DrawOrder.Num();
				RemainingSlotsBeforeWrap = DrawOrder.Num() - StartIndex;
			}
		}

		bool DrawSpell(const FMatterFluxSpellDefinition*& OutSpell)
		{
			OutSpell = nullptr;
			if (DrawOrder.IsEmpty())
			{
				Error = TEXT("wand has no spell slots to draw");
				return false;
			}
			if (++InstructionCount > MaximumInstructionsPerCast)
			{
				Error = TEXT("wand program exceeded its instruction budget");
				return false;
			}
			// A child draw may reach the physical end of the prefix program. It
			// is an empty branch, not permission to wrap into the next cast and
			// recursively execute the root again.
			if (RemainingSlotsBeforeWrap <= 0)
			{
				return true;
			}
			--RemainingSlotsBeforeWrap;
			const int32 OrderedIndex =
				Plan.NextState.DeckCursor % DrawOrder.Num();
			const int32 SlotIndex = DrawOrder[OrderedIndex];
			++Plan.NextState.DeckCursor;
			const FName SpellId = Slots[SlotIndex];
			if (SpellId.IsNone())
			{
				return true;
			}
			OutSpell = Registry.Spells.Find(SpellId);
			if (!OutSpell)
			{
				Error = FString::Printf(
					TEXT("wand slot references unknown spell '%s'"),
					*SpellId.ToString());
				return false;
			}
			if (Plan.NextState.Mana + KINDA_SMALL_NUMBER
				< OutSpell->ManaCost)
			{
				Error = FString::Printf(
					TEXT("insufficient mana for spell '%s'"),
					*SpellId.ToString());
				return false;
			}
			Plan.NextState.Mana -= OutSpell->ManaCost;
			Plan.ManaSpent += OutSpell->ManaCost;
			Plan.CastDelay += OutSpell->CastDelayDelta;
			Plan.RechargeTime += OutSpell->RechargeTimeDelta;
			return true;
		}

		bool CompileSequence(
			int32 DrawCount,
			const int32 TriggerDepth,
			TArray<FMatterFluxMagicProjectilePlan>& OutProjectiles,
			TArray<FMatterFluxMagicCasterEffectPlan>& OutCasterEffects)
		{
			if (TriggerDepth > MaximumTriggerDepth)
			{
				Error = TEXT("wand program exceeded its trigger depth budget");
				return false;
			}
			FPendingModifiers Pending;
			Pending.SpreadDegrees = Wand.Spread;
			float SequenceSpreadDelta = 0.0f;
			while (DrawCount > 0)
			{
				--DrawCount;
				const FMatterFluxSpellDefinition* Spell = nullptr;
				if (!DrawSpell(Spell))
				{
					return false;
				}
				if (!Spell)
				{
					continue;
				}

				switch (Spell->Kind)
				{
				case EMatterFluxSpellKind::Modifier:
					Pending.DamageAdd += Spell->DamageAdd;
					Pending.DamageMultiplier *= Spell->DamageMultiplier;
					Pending.SpeedMultiplier *= Spell->SpeedMultiplier;
					Pending.LifetimeMultiplier *=
						Spell->LifetimeMultiplier;
					Pending.SpreadDegrees += Spell->SpreadDelta;
					if (Spell->bOverrideColor)
					{
						Pending.bOverrideColor = true;
						Pending.Color = Spell->Color;
					}
					if (Spell->OrbitRadius > 0.0f)
					{
						Pending.OrbitRadius = Spell->OrbitRadius;
					}
					DrawCount += Spell->DrawCount;
					break;

				case EMatterFluxSpellKind::Multicast:
					SequenceSpreadDelta += Spell->SpreadDelta;
					Pending.SpreadDegrees += Spell->SpreadDelta;
					DrawCount += Spell->DrawCount;
					break;

				case EMatterFluxSpellKind::TriggerModifier:
				{
					TArray<FMatterFluxMagicProjectilePlan> CarrierProjectiles;
					TArray<FMatterFluxMagicCasterEffectPlan> CarrierCasterEffects;
					TArray<FMatterFluxMagicProjectilePlan> PayloadProjectiles;
					TArray<FMatterFluxMagicCasterEffectPlan> PayloadCasterEffects;
					if (!CompileSequence(
						1,
						TriggerDepth + 1,
						CarrierProjectiles,
						CarrierCasterEffects)
						|| !CompileSequence(
							1,
							TriggerDepth + 1,
							PayloadProjectiles,
							PayloadCasterEffects))
					{
						return false;
					}
					if (CarrierProjectiles.IsEmpty()
						|| !CarrierCasterEffects.IsEmpty()
						|| !PayloadCasterEffects.IsEmpty())
					{
						Error = TEXT("trigger modifiers require projectile carrier and payload children");
						return false;
					}
					for (FMatterFluxMagicProjectilePlan& Carrier
						: CarrierProjectiles)
					{
						Carrier.Damage = FMath::Max(
							0.0f,
							(Carrier.Damage + Pending.DamageAdd)
								* Pending.DamageMultiplier);
						Carrier.Speed *= Pending.SpeedMultiplier;
						Carrier.Lifetime *= Pending.LifetimeMultiplier;
						Carrier.SpreadDegrees += Pending.SpreadDegrees - Wand.Spread;
						if (Pending.bOverrideColor)
						{
							Carrier.bOverrideColor = true;
							Carrier.Color = Pending.Color;
						}
						if (Pending.OrbitRadius > 0.0f)
						{
							Carrier.OrbitRadius = Pending.OrbitRadius;
						}
						if (Spell->CarrierLifetimeOverride > 0.0f)
						{
							Carrier.Lifetime = Spell->CarrierLifetimeOverride;
						}
						Carrier.bTriggerRandomDirection =
							Spell->bTriggerRandomDirection;
						if (Spell->TriggerEvent ==
							EMatterFluxSpellTriggerEvent::Expired)
						{
							Carrier.OnExpireProjectiles = PayloadProjectiles;
						}
						else
						{
							Carrier.OnImpactProjectiles = PayloadProjectiles;
						}
					}
					OutProjectiles.Append(MoveTemp(CarrierProjectiles));
					Pending = FPendingModifiers();
					Pending.SpreadDegrees = Wand.Spread + SequenceSpreadDelta;
					break;
				}

				case EMatterFluxSpellKind::Projectile:
				case EMatterFluxSpellKind::Trigger:
				{
					if (++ProjectileCount > MaximumProjectilesPerCast)
					{
						Error = TEXT("wand program exceeded its projectile budget");
						return false;
					}
					FMatterFluxMagicProjectilePlan Projectile;
					Projectile.SpellId = Spell->Id;
					Projectile.Damage = FMath::Max(
						0.0f,
						(Spell->Damage + Pending.DamageAdd)
							* Pending.DamageMultiplier);
					Projectile.Speed =
						Spell->Speed * Pending.SpeedMultiplier;
					Projectile.Lifetime =
						Spell->Lifetime * Pending.LifetimeMultiplier;
					Projectile.Radius = Spell->Radius;
					Projectile.GravityScale = Spell->GravityScale;
					Projectile.SpreadDegrees = Pending.SpreadDegrees;
					Projectile.SpawnAngleDegrees =
						FMath::IsNearlyZero(Pending.SpreadDegrees)
							? 0.0f
							: Random.FRandRange(
								-Pending.SpreadDegrees,
								Pending.SpreadDegrees);
					Projectile.BodyMaterial = Spell->BodyMaterial;
					Projectile.MaterialAmount = Spell->MaterialAmount;
					Projectile.bUsePlaneVisual = Spell->bUsePlaneVisual;
					Projectile.bUseVerticalPlaneVisual =
						Spell->bUseVerticalPlaneVisual;
					Projectile.bOverrideColor = Pending.bOverrideColor;
					Projectile.Color = Pending.Color;
					Projectile.OrbitRadius = Pending.OrbitRadius;
					TArray<FMatterFluxMagicCasterEffectPlan> TriggerCasterEffects;
					if (Spell->Kind == EMatterFluxSpellKind::Trigger
						&& (!CompileSequence(
							Spell->TriggerDrawCount,
							TriggerDepth + 1,
							Projectile.OnImpactProjectiles,
							TriggerCasterEffects)
							|| !TriggerCasterEffects.IsEmpty()))
					{
						if (Error.IsEmpty())
						{
							Error = TEXT("avatar actions cannot be trigger payloads");
						}
						return false;
					}
					OutProjectiles.Add(MoveTemp(Projectile));
					Pending = FPendingModifiers();
					Pending.SpreadDegrees = Wand.Spread + SequenceSpreadDelta;
					DrawCount += Spell->DrawCount;
					break;
				}

				case EMatterFluxSpellKind::Jump:
				{
					if (++CasterEffectCount > MaximumCasterEffectsPerCast)
					{
						Error = TEXT("wand program exceeded its caster effect budget");
						return false;
					}
					FMatterFluxMagicCasterEffectPlan Effect;
					Effect.SpellId = Spell->Id;
					Effect.Type = EMatterFluxMagicCasterEffectType::Jump;
					Effect.VerticalImpulse = Spell->VerticalImpulse;
					OutCasterEffects.Add(MoveTemp(Effect));
					Pending = FPendingModifiers();
					Pending.SpreadDegrees = Wand.Spread + SequenceSpreadDelta;
					break;
				}
				default:
					Error = TEXT("wand program contains an unsupported spell kind");
					return false;
				}
			}
			return true;
		}
	};
}

bool FMatterFluxWandProgram::Evaluate(
	const FMatterFluxContentRegistry& Registry,
	const FName WandDefinitionId,
	const TConstArrayView<FName> SpellSlots,
	const FMatterFluxWandProgramState& CurrentState,
	const int32 EventSeed,
	FMatterFluxWandCastPlan& OutPlan,
	FString& OutError)
{
	using namespace MatterFluxWandProgram;
	OutPlan = FMatterFluxWandCastPlan();
	OutError.Reset();
	const FMatterFluxWandDefinition* Wand =
		Registry.Wands.Find(WandDefinitionId);
	if (!Wand)
	{
		OutError = FString::Printf(
			TEXT("unknown wand definition '%s'"),
			*WandDefinitionId.ToString());
		return false;
	}
	if (SpellSlots.IsEmpty()
		|| SpellSlots.Num() > Wand->Capacity
		|| !FMath::IsFinite(CurrentState.Mana)
		|| CurrentState.Mana < 0.0f
		|| CurrentState.Mana > Wand->ManaMax + KINDA_SMALL_NUMBER
		|| CurrentState.DeckCursor < 0
		|| CurrentState.CastSerial < 0
		|| CurrentState.CastSerial == MAX_int32)
	{
		OutError = TEXT("wand program state or slot layout is invalid");
		return false;
	}

	// FName's comparison index is process-local. Hash the constrained ASCII
	// content id instead so equal packs produce the same shuffle on every peer.
	const uint32 WandIdHash = FCrc::StrCrc32(
		*WandDefinitionId.ToString());
	const uint32 DeterministicSeed = HashCombineFast(
		GetTypeHash(EventSeed),
		HashCombineFast(
			WandIdHash,
			GetTypeHash(CurrentState.CastSerial)));
	FEvaluationContext Context(
		Registry,
		*Wand,
		SpellSlots,
		CurrentState,
		static_cast<int32>(DeterministicSeed));
	if (!Context.Error.IsEmpty())
	{
		OutError = Context.Error;
		return false;
	}
	if (!Context.CompileSequence(
		Wand->DrawCount,
		0,
		Context.Plan.Projectiles,
		Context.Plan.CasterEffects))
	{
		OutError = Context.Error;
		return false;
	}
	if (Context.Plan.Projectiles.IsEmpty()
		&& Context.Plan.CasterEffects.IsEmpty())
	{
		OutError = TEXT("wand program emitted no cast actions");
		return false;
	}

	Context.Plan.CastDelay = FMath::Max(0.0f, Context.Plan.CastDelay);
	Context.Plan.RechargeTime =
		FMath::Max(0.0f, Context.Plan.RechargeTime);
	Context.Plan.bDeckExhausted =
		!Context.DrawOrder.IsEmpty()
		&& CurrentState.DeckCursor / Context.DrawOrder.Num()
			!= Context.Plan.NextState.DeckCursor / Context.DrawOrder.Num();
	++Context.Plan.NextState.CastSerial;
	OutPlan = MoveTemp(Context.Plan);
	return true;
}
