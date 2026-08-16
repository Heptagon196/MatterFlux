#pragma once

#include "Components/InstancedStaticMeshComponent.h"

namespace MatterFlux::Rendering
{
	inline void SynchronizeInstancesWithoutClearing(
		UInstancedStaticMeshComponent& Instances,
		const TArray<FTransform>& Transforms)
	{
		const int32 PreviousCount = Instances.GetInstanceCount();
		const int32 NextCount = Transforms.Num();
		const int32 SharedCount = FMath::Min(PreviousCount, NextCount);
		if (SharedCount > 0)
		{
			Instances.BatchUpdateInstancesTransforms(
				0,
				MakeArrayView(Transforms).Left(SharedCount),
				false,
				PreviousCount == NextCount,
				true);
		}

		if (NextCount > PreviousCount)
		{
			TArray<FTransform> AddedTransforms;
			AddedTransforms.Append(
				Transforms.GetData() + PreviousCount,
				NextCount - PreviousCount);
			Instances.AddInstances(
				AddedTransforms,
				false,
				false,
				false);
		}
		else if (NextCount < PreviousCount)
		{
			TArray<int32> RemovedIndices;
			RemovedIndices.Reserve(PreviousCount - NextCount);
			for (int32 Index = PreviousCount - 1;
				Index >= NextCount;
				--Index)
			{
				RemovedIndices.Add(Index);
			}
			Instances.RemoveInstances(RemovedIndices, true);
		}
	}
}
