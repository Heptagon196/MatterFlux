#pragma once

#include "CoreMinimal.h"
#include "MatterFluxContentTypes.h"

enum class EMatterFluxCreatureRuntimeState : uint8;

/** Snapshot of server knowledge consumed by one fixed-rate tree evaluation. */
struct MATTERFLUX_API FMatterFluxCreatureAIDecisionContext
{
	bool bHasVisibleTarget = false;
	bool bRemembersTarget = false;
	float TargetDistance = TNumericLimits<float>::Max();
	float AttackRange = 0.0f;
	float RetreatRange = 0.0f;
	bool bAttackReady = false;
	bool bSkillReady = false;
};

/** Pure interpreter seam shared by the server AIController and tests. */
class MATTERFLUX_API FMatterFluxCreatureBehaviorTreeEvaluator
{
public:
	static bool Evaluate(
		const FMatterFluxCreatureBehaviorProgramDefinition& Program,
		const FMatterFluxCreatureAIDecisionContext& Context,
		EMatterFluxCreatureRuntimeState& OutState,
		FString& OutError);
};
