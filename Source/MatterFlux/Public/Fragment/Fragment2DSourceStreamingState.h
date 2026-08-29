#pragma once

#include "CoreMinimal.h"

struct FFragment2DMaterialVolumeCellState
{
	FIntVector Cell = FIntVector::ZeroValue;
	FName MaterialId = NAME_None;
	uint16 Energy = 0;

	bool operator==(const FFragment2DMaterialVolumeCellState& Other) const = default;
};

struct FFragment2DSourceStreamingState
{
	int32 Revision = 0;
	int32 VolumeTopologyRevision = 0;
	int32 VolumeFieldRevision = 0;
	uint16 VolumeEnvironmentEnergy = 0;
	TArray<FFragment2DMaterialVolumeCellState> VolumeCellStates;
	void SetRuntimeMask(TArray<uint8> InRuntimeMask)
	{
		RuntimeMask = MoveTemp(InRuntimeMask);
	}

	const TArray<uint8>& GetRuntimeMask() const
	{
		return RuntimeMask;
	}

	int32 GetStoredMaskValueCount() const
	{
		return RuntimeMask.Num()
			+ VolumeCellStates.Num()
				* static_cast<int32>(sizeof(FFragment2DMaterialVolumeCellState));
	}

	bool HasPersistentChanges() const
	{
		return Revision > 0
			|| VolumeTopologyRevision > 0
			|| VolumeFieldRevision > 0
			|| !VolumeCellStates.IsEmpty();
	}

private:
	TArray<uint8> RuntimeMask;
};
