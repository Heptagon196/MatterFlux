#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Creatures/MatterFluxCreatureBehaviorTree.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "MatterFluxCreatureAIController.generated.h"

class AMatterFluxTwoStoreyHouseActor;

namespace MatterFlux::Creatures
{
	/** True while an authored creature has never acquired its first target. */
	MATTERFLUX_API bool ShouldWaitForFirstSight(
		bool bWaitForFirstSight,
		bool bHasEverSeenTarget,
		bool bHasVisibleTarget);

	/** True when chase input should pause inside the configured combat band. */
	MATTERFLUX_API bool ShouldHoldCombatPosition(
		EMatterFluxCreatureRuntimeState State,
		bool bHasVisibleTarget,
		float TargetDistance,
		float AttackRange);
}

/** Fixed-rate server-only interpreter for Lua creature AI programs. */
UCLASS()
class MATTERFLUX_API AMatterFluxCreatureAIController : public AAIController
{
	GENERATED_BODY()

public:
	AMatterFluxCreatureAIController();
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void OnPossess(APawn* InPawn) override;

private:
	AMatterFluxCreatureActor* GetCreature() const;
	AActor* FindNearestPlayer(float Range, float& OutDistance) const;
	void UpdateDecision(
		AMatterFluxCreatureActor& Creature,
		const FMatterFluxCreatureDefinition& Definition,
		double Now);
	void ApplyMovement(
		AMatterFluxCreatureActor& Creature,
		AActor* Target,
		EMatterFluxCreatureRuntimeState State,
		float DeltaSeconds);

	TWeakObjectPtr<AActor> RememberedTarget;
	bool bHasEverSeenTarget = false;
	double LastSeenTime = -DBL_MAX;
	double LastAttackTime = -DBL_MAX;
	double LastSkillTime = -DBL_MAX;
	double LastPatrolTurnTime = -DBL_MAX;
	double PatrolPauseUntil = -DBL_MAX;
	double NextDecisionTime = -DBL_MAX;
	float PatrolDirection = 1.0f;
	float RecoveryDirection = 1.0f;
	TWeakObjectPtr<AMatterFluxTwoStoreyHouseActor> PatrolHouse;
	TWeakObjectPtr<AActor> MovementTarget;
	bool bMovementTargetVisible = false;
	EMatterFluxCreatureRuntimeState MovementState =
		EMatterFluxCreatureRuntimeState::Passive;
	int32 HousePatrolWaypointIndex = 0;
	float BlockedDurationSeconds = 0.0f;
	int32 DecisionSerial = 0;
};
