#include "Creatures/MatterFluxCreatureBehaviorTree.h"

#include "Creatures/MatterFluxCreatureActor.h"

namespace MatterFluxCreatureBehaviorTree
{
	constexpr int32 MaximumEvaluationDepth = 8;

	static bool TestCondition(
		const EMatterFluxCreatureBehaviorCondition Condition,
		const FMatterFluxCreatureAIDecisionContext& Context)
	{
		switch (Condition)
		{
		case EMatterFluxCreatureBehaviorCondition::HasVisibleTarget:
			return Context.bHasVisibleTarget;
		case EMatterFluxCreatureBehaviorCondition::HasTarget:
			return Context.bHasVisibleTarget || Context.bRemembersTarget;
		case EMatterFluxCreatureBehaviorCondition::TargetTooClose:
			return Context.bHasVisibleTarget
				&& Context.RetreatRange > 0.0f
				&& Context.TargetDistance <= Context.RetreatRange;
		case EMatterFluxCreatureBehaviorCondition::TargetInAttackRange:
			return Context.bHasVisibleTarget
				&& Context.TargetDistance <= Context.AttackRange;
		case EMatterFluxCreatureBehaviorCondition::AttackReady:
			return Context.bAttackReady;
		case EMatterFluxCreatureBehaviorCondition::SkillReady:
			return Context.bSkillReady;
		default:
			return false;
		}
	}

	static EMatterFluxCreatureRuntimeState ResolveAction(
		const EMatterFluxCreatureBehaviorAction Action)
	{
		switch (Action)
		{
		case EMatterFluxCreatureBehaviorAction::Passive:
			return EMatterFluxCreatureRuntimeState::Passive;
		case EMatterFluxCreatureBehaviorAction::Patrol:
			return EMatterFluxCreatureRuntimeState::Patrol;
		case EMatterFluxCreatureBehaviorAction::Chase:
			return EMatterFluxCreatureRuntimeState::Chase;
		case EMatterFluxCreatureBehaviorAction::Retreat:
			return EMatterFluxCreatureRuntimeState::Retreat;
		case EMatterFluxCreatureBehaviorAction::Attack:
			return EMatterFluxCreatureRuntimeState::Attack;
		case EMatterFluxCreatureBehaviorAction::Skill:
			return EMatterFluxCreatureRuntimeState::Skill;
		default:
			return EMatterFluxCreatureRuntimeState::Passive;
		}
	}

	static bool EvaluateActionNode(
		const FMatterFluxCreatureBehaviorProgramDefinition& Program,
		const int32 NodeIndex,
		const int32 Depth,
		const FMatterFluxCreatureAIDecisionContext& Context,
		EMatterFluxCreatureRuntimeState& OutState,
		bool& bOutMatched,
		FString& OutError)
	{
		bOutMatched = false;
		if (Depth > MaximumEvaluationDepth
			|| !Program.Nodes.IsValidIndex(NodeIndex))
		{
			OutError = TEXT("creature behavior tree contains an invalid node reference");
			return false;
		}

		const FMatterFluxCreatureBehaviorNodeDefinition& Node =
			Program.Nodes[NodeIndex];
		switch (Node.Kind)
		{
		case EMatterFluxCreatureBehaviorNodeKind::Action:
			OutState = ResolveAction(Node.Action);
			bOutMatched = true;
			return true;

		case EMatterFluxCreatureBehaviorNodeKind::Sequence:
			if (Node.Children.IsEmpty())
			{
				OutError = TEXT("creature behavior sequence has no children");
				return false;
			}
			for (int32 ChildOffset = 0;
				ChildOffset + 1 < Node.Children.Num(); ++ChildOffset)
			{
				const int32 ConditionIndex = Node.Children[ChildOffset];
				if (!Program.Nodes.IsValidIndex(ConditionIndex)
					|| Program.Nodes[ConditionIndex].Kind
						!= EMatterFluxCreatureBehaviorNodeKind::Condition)
				{
					OutError = TEXT("creature behavior sequence predicate is invalid");
					return false;
				}
				if (!TestCondition(
					Program.Nodes[ConditionIndex].Condition, Context))
				{
					return true;
				}
			}
			return EvaluateActionNode(
				Program,
				Node.Children.Last(),
				Depth + 1,
				Context,
				OutState,
				bOutMatched,
				OutError);

		case EMatterFluxCreatureBehaviorNodeKind::Selector:
			for (const int32 ChildIndex : Node.Children)
			{
				bool bChildMatched = false;
				if (!EvaluateActionNode(
					Program,
					ChildIndex,
					Depth + 1,
					Context,
					OutState,
					bChildMatched,
					OutError))
				{
					return false;
				}
				if (bChildMatched)
				{
					bOutMatched = true;
					return true;
				}
			}
			return true;

		default:
			OutError = TEXT("creature behavior tree expected an action subtree");
			return false;
		}
	}
}

bool FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
	const FMatterFluxCreatureBehaviorProgramDefinition& Program,
	const FMatterFluxCreatureAIDecisionContext& Context,
	EMatterFluxCreatureRuntimeState& OutState,
	FString& OutError)
{
	OutState = EMatterFluxCreatureRuntimeState::Passive;
	OutError.Reset();
	bool bMatched = false;
	if (!MatterFluxCreatureBehaviorTree::EvaluateActionNode(
		Program,
		Program.RootNodeIndex,
		0,
		Context,
		OutState,
		bMatched,
		OutError))
	{
		return false;
	}
	if (!bMatched)
	{
		OutError = TEXT("creature behavior tree did not select an action");
		return false;
	}
	return true;
}
