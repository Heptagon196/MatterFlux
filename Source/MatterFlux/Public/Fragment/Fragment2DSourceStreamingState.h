#pragma once

#include "CoreMinimal.h"
#include "Material/MatterFluxSourceCombustionRuntime.h"

struct FFragment2DSourceStreamingState
	: public MatterFlux::Combustion::FSourceRuntimeSnapshot
{
	int32 Revision = 0;
	bool bHasCombustionState = false;

	bool CaptureCombustionState(
		const MatterFlux::Combustion::FSourceCombustionRuntime& Runtime)
	{
		if (!Runtime.CaptureState(*this))
		{
			return false;
		}
		bHasCombustionState = true;
		StandaloneRuntimeMask.Reset();
		return true;
	}

	void SetRuntimeMask(TArray<uint8> InRuntimeMask)
	{
		if (bHasCombustionState)
		{
			CombustionState.FuelMask = MoveTemp(InRuntimeMask);
			StandaloneRuntimeMask.Reset();
		}
		else
		{
			StandaloneRuntimeMask = MoveTemp(InRuntimeMask);
		}
	}

	const TArray<uint8>& GetRuntimeMask() const
	{
		return bHasCombustionState
			? CombustionState.FuelMask
			: StandaloneRuntimeMask;
	}

	int32 GetStoredMaskValueCount() const
	{
		return StandaloneRuntimeMask.Num()
			+ CombustionState.FuelMask.Num()
			+ CombustionState.ResidueMask.Num()
			+ CombustionState.BurningMask.Num();
	}

	bool HasPersistentChanges() const
	{
		return Revision > 0 || bHasCombustionState;
	}

private:
	TArray<uint8> StandaloneRuntimeMask;
};
