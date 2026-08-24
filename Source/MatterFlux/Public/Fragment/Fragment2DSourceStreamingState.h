#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxSourceReactionRuntime.h"

struct FFragment2DSourceStreamingState
	: public MatterFlux::Reaction::FSourceRuntimeSnapshot
{
	int32 Revision = 0;
	bool bHasReactionState = false;

	bool CaptureReactionState(
		const MatterFlux::Reaction::FSourceReactionRuntime& Runtime)
	{
		if (!Runtime.CaptureState(*this))
		{
			return false;
		}
		bHasReactionState = true;
		StandaloneRuntimeMask.Reset();
		return true;
	}

	void SetRuntimeMask(TArray<uint8> InRuntimeMask)
	{
		if (bHasReactionState)
		{
			ReactionState.InputMask = MoveTemp(InRuntimeMask);
			StandaloneRuntimeMask.Reset();
		}
		else
		{
			StandaloneRuntimeMask = MoveTemp(InRuntimeMask);
		}
	}

	const TArray<uint8>& GetRuntimeMask() const
	{
		return bHasReactionState
			? ReactionState.InputMask
			: StandaloneRuntimeMask;
	}

	int32 GetStoredMaskValueCount() const
	{
		return StandaloneRuntimeMask.Num()
			+ ReactionState.InputMask.Num()
			+ ReactionState.OutputMask.Num()
			+ ReactionState.ActiveMask.Num();
	}

	bool HasPersistentChanges() const
	{
		return Revision > 0 || bHasReactionState;
	}

private:
	TArray<uint8> StandaloneRuntimeMask;
};
