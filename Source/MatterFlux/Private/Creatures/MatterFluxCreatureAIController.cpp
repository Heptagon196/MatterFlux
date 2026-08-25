#include "Creatures/MatterFluxCreatureAIController.h"

#include "EngineUtils.h"
#include "Components/CapsuleComponent.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxTwoStoreyHouseActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/Crc.h"

namespace
{
	constexpr float BlockedSpeedThreshold = 8.0f;
	constexpr float BlockedRecoveryDelaySeconds = 0.5f;
}

bool MatterFlux::Creatures::ShouldWaitForFirstSight(
	const bool bWaitForFirstSight,
	const bool bHasEverSeenTarget,
	const bool bHasVisibleTarget)
{
	return bWaitForFirstSight
		&& !bHasEverSeenTarget
		&& !bHasVisibleTarget;
}

bool MatterFlux::Creatures::ShouldHoldCombatPosition(
	const EMatterFluxCreatureRuntimeState State,
	const bool bHasVisibleTarget,
	const float TargetDistance,
	const float AttackRange)
{
	if (State != EMatterFluxCreatureRuntimeState::Chase
		|| !bHasVisibleTarget
		|| !FMath::IsFinite(TargetDistance)
		|| AttackRange <= 0.0f)
	{
		return false;
	}

	return TargetDistance <= AttackRange;
}

AMatterFluxCreatureAIController::AMatterFluxCreatureAIController()
{
	PrimaryActorTick.bCanEverTick = true;
	// CMC 需要每帧接收移动输入；昂贵的感知和行为树决策仍由
	// NextDecisionTime 限制为 10 Hz。
	PrimaryActorTick.TickInterval = 0.0f;
	bStartAILogicOnPossess = true;
	bStopAILogicOnUnposses = true;
}

void AMatterFluxCreatureAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	const uint32 Hash = FCrc::StrCrc32(*InPawn->GetName());
	PatrolDirection = (Hash & 1u) == 0u ? 1.0f : -1.0f;
	RecoveryDirection = (Hash & 2u) == 0u ? 1.0f : -1.0f;
	LastPatrolTurnTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	NextDecisionTime = -DBL_MAX;
	bHasEverSeenTarget = false;
	MovementTarget.Reset();
	bMovementTargetVisible = false;
	MovementState = EMatterFluxCreatureRuntimeState::Passive;
	BlockedDurationSeconds = 0.0f;
}

void AMatterFluxCreatureAIController::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	AMatterFluxCreatureActor* Creature = GetCreature();
	if (!Creature
		|| !Creature->HasAuthority()
		|| Creature->GetRuntimeState() == EMatterFluxCreatureRuntimeState::Dead)
	{
		return;
	}
	const FMatterFluxCreatureDefinition* Definition =
		Creature->ResolveDefinition();
	if (!Definition) return;
	const double Now = GetWorld()->GetTimeSeconds();
	if (Creature->IsCastSequenceActive())
	{
		MovementTarget.Reset();
		MovementState = EMatterFluxCreatureRuntimeState::Skill;
		Creature->SetRuntimeStateAuthority(
			MovementState);
		ApplyMovement(
			*Creature, nullptr, MovementState,
			DeltaSeconds);
		return;
	}
	if (Now >= NextDecisionTime)
	{
		UpdateDecision(*Creature, *Definition, Now);
		NextDecisionTime = Now + 0.10;
	}
	ApplyMovement(
		*Creature,
		MovementTarget.Get(),
		MovementState,
		DeltaSeconds);
}

void AMatterFluxCreatureAIController::UpdateDecision(
	AMatterFluxCreatureActor& Creature,
	const FMatterFluxCreatureDefinition& Definition,
	const double Now)
{
	float Distance = TNumericLimits<float>::Max();
	AActor* VisibleTarget = FindNearestPlayer(
		Definition.PerceptionRange, Distance);
	if (VisibleTarget && LineOfSightTo(VisibleTarget))
	{
		RememberedTarget = VisibleTarget;
		LastSeenTime = Now;
	}
	else
	{
		VisibleTarget = nullptr;
	}
	if (MatterFlux::Creatures::ShouldWaitForFirstSight(
		Definition.bWaitForFirstSight,
		bHasEverSeenTarget,
		VisibleTarget != nullptr))
	{
		RememberedTarget.Reset();
		MovementTarget.Reset();
		bMovementTargetVisible = false;
		MovementState = EMatterFluxCreatureRuntimeState::Passive;
		Creature.SetRuntimeStateAuthority(MovementState);
		return;
	}
	bHasEverSeenTarget |= VisibleTarget != nullptr;
	const bool bRemembers = RememberedTarget.IsValid()
		&& Now - LastSeenTime <= Definition.TargetMemorySeconds;
	if (!bRemembers) RememberedTarget.Reset();
	AActor* Target = VisibleTarget
		? VisibleTarget : RememberedTarget.Get();
	if (Target && !VisibleTarget)
	{
		Distance = FVector::Dist(
			Creature.GetActorLocation(), Target->GetActorLocation());
	}

	FMatterFluxCreatureAIDecisionContext Context;
	Context.bHasVisibleTarget = VisibleTarget != nullptr;
	Context.bRemembersTarget = bRemembers;
	Context.TargetDistance = Distance;
	Context.AttackRange = Definition.AttackRange;
	Context.RetreatRange = Definition.RetreatRange;
	Context.bAttackReady = Now - LastAttackTime >= Definition.AttackCooldown;
	Context.bSkillReady = !Definition.SkillProgram.SpellId.IsNone()
		&& Now - LastSkillTime >= Definition.SkillCooldown;
	EMatterFluxCreatureRuntimeState State =
		EMatterFluxCreatureRuntimeState::Passive;
	FString BehaviorError;
	if (!FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
		Definition.BehaviorProgram,
		Context,
		State,
		BehaviorError))
	{
		ensureMsgf(
			false,
			TEXT("Invalid compiled creature behavior for '%s': %s"),
			*Definition.Id.ToString(),
			*BehaviorError);
	}
	MovementTarget = Target;
	bMovementTargetVisible = VisibleTarget != nullptr;
	MovementState = State;
	Creature.SetRuntimeStateAuthority(State);
	if (Target && State == EMatterFluxCreatureRuntimeState::Attack)
	{
		if (Creature.CastConfiguredSpellAuthority(
			*Target, false, ++DecisionSerial))
		{
			LastAttackTime = Now;
		}
	}
	else if (Target && State == EMatterFluxCreatureRuntimeState::Skill)
	{
		if (Creature.CastConfiguredSpellAuthority(
			*Target, true, ++DecisionSerial))
		{
			LastSkillTime = Now;
		}
	}
}

AMatterFluxCreatureActor* AMatterFluxCreatureAIController::GetCreature() const
{
	return Cast<AMatterFluxCreatureActor>(GetPawn());
}

AActor* AMatterFluxCreatureAIController::FindNearestPlayer(
	const float Range,
	float& OutDistance) const
{
	OutDistance = TNumericLimits<float>::Max();
	AActor* Best = nullptr;
	const APawn* Controlled = GetPawn();
	if (!Controlled || !GetWorld()) return nullptr;
	for (TActorIterator<AMatterFluxCharacter> It(GetWorld()); It; ++It)
	{
		if (!IsValid(*It)) continue;
		const float Distance = FVector::Dist(
			Controlled->GetActorLocation(), It->GetActorLocation());
		if (Distance <= Range && Distance < OutDistance)
		{
			OutDistance = Distance;
			Best = *It;
		}
	}
	return Best;
}

void AMatterFluxCreatureAIController::ApplyMovement(
	AMatterFluxCreatureActor& Creature,
	AActor* Target,
	const EMatterFluxCreatureRuntimeState State,
	const float DeltaSeconds)
{
	const FMatterFluxCreatureDefinition* Definition = Creature.ResolveDefinition();
	if (!Definition) return;
	if (State == EMatterFluxCreatureRuntimeState::Skill
		&& Creature.IsCastSequenceActive())
	{
		BlockedDurationSeconds = 0.0f;
		return;
	}
	if (Target
		&& MatterFlux::Creatures::ShouldHoldCombatPosition(
			State,
			bMovementTargetVisible,
			FVector::Dist(
				Creature.GetActorLocation(), Target->GetActorLocation()),
			Definition->AttackRange))
	{
		BlockedDurationSeconds = 0.0f;
		Creature.GetCharacterMovement()->StopMovementImmediately();
		return;
	}
	if (State == EMatterFluxCreatureRuntimeState::Attack
		|| State == EMatterFluxCreatureRuntimeState::Skill
		|| State == EMatterFluxCreatureRuntimeState::Passive)
	{
		BlockedDurationSeconds = 0.0f;
		Creature.GetCharacterMovement()->StopMovementImmediately();
		return;
	}
	FVector Direction = FVector::ZeroVector;
	if (Target && (State == EMatterFluxCreatureRuntimeState::Chase
		|| State == EMatterFluxCreatureRuntimeState::Retreat))
	{
		Direction = Target->GetActorLocation() - Creature.GetActorLocation();
		Direction.Z = 0.0f;
		Direction = Direction.GetSafeNormal();
		if (State == EMatterFluxCreatureRuntimeState::Retreat) Direction *= -1.0f;
	}
	else
	{
		AMatterFluxTwoStoreyHouseActor* House = GetWorld()
			? AMatterFluxTwoStoreyHouseActor::FindContainingHouse(
				*GetWorld(), Creature.GetActorLocation(), 60.0f)
			: nullptr;
		if (House)
		{
			if (PatrolHouse.Get() != House)
			{
				PatrolHouse = House;
				float BestDistance = TNumericLimits<float>::Max();
				for (int32 Index = 0;
					Index < House->GetIndoorPatrolWaypointCount(); ++Index)
				{
					const float Distance = FVector::DistSquared(
						Creature.GetActorLocation(),
						House->GetIndoorPatrolWaypoint(Index));
					if (Distance < BestDistance)
					{
						BestDistance = Distance;
						HousePatrolWaypointIndex = Index;
					}
				}
			}
			FVector TargetPoint = House->GetIndoorPatrolWaypoint(
				HousePatrolWaypointIndex);
			float FeetZ = Creature.GetActorLocation().Z;
			if (const UCapsuleComponent* Capsule = Creature.GetCapsuleComponent())
			{
				FeetZ -= Capsule->GetScaledCapsuleHalfHeight();
			}
			const float HorizontalDistance = FVector::Dist2D(
				Creature.GetActorLocation(), TargetPoint);
			if (HorizontalDistance < 72.0f
				&& FMath::Abs(FeetZ - TargetPoint.Z) < 105.0f)
			{
				HousePatrolWaypointIndex =
					(HousePatrolWaypointIndex + 1)
					% House->GetIndoorPatrolWaypointCount();
				TargetPoint = House->GetIndoorPatrolWaypoint(
					HousePatrolWaypointIndex);
			}
			Direction = TargetPoint - Creature.GetActorLocation();
			Direction.Z = 0.0f;
			Direction = Direction.GetSafeNormal();
		}
		else
		{
			PatrolHouse.Reset();
		const double Now = GetWorld()->GetTimeSeconds();
		if (Now >= PatrolPauseUntil
			&& Now - LastPatrolTurnTime >= Definition->PatrolTurnSeconds)
		{
			PatrolPauseUntil = Now + Definition->PatrolPauseSeconds;
			LastPatrolTurnTime = Now;
			PatrolDirection *= -1.0f;
		}
		if (Now >= PatrolPauseUntil)
		{
			Direction = FVector(PatrolDirection, 0.0f, 0.0f);
		}
		}
	}
	if (!Direction.IsNearlyZero())
	{
		UCharacterMovementComponent* Movement = Creature.GetCharacterMovement();
		if (Movement->Velocity.SizeSquared2D()
			< FMath::Square(BlockedSpeedThreshold))
		{
			BlockedDurationSeconds += FMath::Max(DeltaSeconds, 0.0f);
		}
		else
		{
			BlockedDurationSeconds = 0.0f;
		}
		if (BlockedDurationSeconds >= BlockedRecoveryDelaySeconds)
		{
			if (State == EMatterFluxCreatureRuntimeState::Patrol
				&& PatrolHouse.IsValid())
			{
				// 室内路线的方向由楼梯航点决定，不能套用野外巡逻的
				// X 轴反向逻辑；保留方向并用一次小跳越过门槛。
			}
			else if (State == EMatterFluxCreatureRuntimeState::Patrol)
			{
				PatrolDirection *= -1.0f;
				Direction = FVector(PatrolDirection, 0.0f, 0.0f);
			}
			else
			{
				Direction = Direction.RotateAngleAxis(
					RecoveryDirection * 55.0f, FVector::UpVector);
				RecoveryDirection *= -1.0f;
			}
			if (Movement->IsMovingOnGround() && Creature.CanJump())
			{
				Creature.Jump();
			}
			BlockedDurationSeconds = 0.0f;
		}
		Creature.AddMovementInput(Direction, 1.0f);
	}
	else
	{
		BlockedDurationSeconds = 0.0f;
	}
}
