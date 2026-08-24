#include "Rendering/MatterFluxItemOcclusion.h"

namespace
{
	bool SegmentIntersectsBox(
		const FVector& Start,
		const FVector& End,
		const FBox& Box)
	{
		const FVector Direction = End - Start;
		float MinimumTime = 0.0f;
		float MaximumTime = 1.0f;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (FMath::IsNearlyZero(Direction[Axis]))
			{
				if (Start[Axis] < Box.Min[Axis]
					|| Start[Axis] > Box.Max[Axis])
				{
					return false;
				}
				continue;
			}

			float EntryTime = (Box.Min[Axis] - Start[Axis])
				/ Direction[Axis];
			float ExitTime = (Box.Max[Axis] - Start[Axis])
				/ Direction[Axis];
			if (EntryTime > ExitTime)
			{
				Swap(EntryTime, ExitTime);
			}
			MinimumTime = FMath::Max(MinimumTime, EntryTime);
			MaximumTime = FMath::Min(MaximumTime, ExitTime);
			if (MinimumTime > MaximumTime)
			{
				return false;
			}
		}
		return MaximumTime >= 0.0f && MinimumTime <= 0.96f;
	}

	void BuildViewerProbePoints(
		const FVector& CameraLocation,
		const FBox& ViewerBounds,
		TArray<FVector, TInlineAllocator<5>>& OutPoints)
	{
		const FVector Center = ViewerBounds.GetCenter();
		const FVector Extent = ViewerBounds.GetExtent();
		OutPoints.Add(Center);
		OutPoints.Add(Center + FVector(0.0f, 0.0f, Extent.Z * 0.68f));
		OutPoints.Add(Center - FVector(0.0f, 0.0f, Extent.Z * 0.62f));

		FVector ScreenRight = FVector::CrossProduct(
			(Center - CameraLocation).GetSafeNormal(),
			FVector::UpVector).GetSafeNormal();
		if (ScreenRight.IsNearlyZero())
		{
			ScreenRight = FVector::RightVector;
		}
		const float SideOffset = FMath::Min(Extent.X, Extent.Y) * 0.55f;
		OutPoints.Add(Center + ScreenRight * SideOffset);
		OutPoints.Add(Center - ScreenRight * SideOffset);
	}
}

namespace MatterFlux::ItemOcclusion
{
	bool Resolve(
		const FVector& CameraLocation,
		const FBox& ViewerBounds,
		const TConstArrayView<FItem> Items,
		const FPolicy& Policy,
		FResult& OutResult)
	{
		OutResult = FResult();
		if (CameraLocation.ContainsNaN() || !ViewerBounds.IsValid)
		{
			return false;
		}

		TArray<FVector, TInlineAllocator<5>> ProbePoints;
		BuildViewerProbePoints(CameraLocation, ViewerBounds, ProbePoints);
		TSet<FGuid> OccludingConnectionIds;
		for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
		{
			const FItem& Item = Items[ItemIndex];
			if (!Item.ItemId.IsValid() || !Item.WorldBounds.IsValid)
			{
				continue;
			}
			const FBox ProbeBounds = Item.WorldBounds.ExpandBy(
				FMath::Max(Policy.ProbeRadiusCentimeters, 0.0f));
			const bool bBlocksViewer = ProbePoints.ContainsByPredicate(
				[&CameraLocation, &ProbeBounds](const FVector& ProbePoint)
				{
					return SegmentIntersectsBox(
						CameraLocation, ProbePoint, ProbeBounds);
				});
			if (bBlocksViewer)
			{
				OutResult.GhostItemIds.Add(Item.ItemId);
				if (Item.ConnectionId.IsValid())
				{
					OccludingConnectionIds.Add(Item.ConnectionId);
				}
			}
		}

		if (!OccludingConnectionIds.IsEmpty())
		{
			for (const FItem& Candidate : Items)
			{
				if (Candidate.ItemId.IsValid()
					&& Candidate.ConnectionId.IsValid()
					&& OccludingConnectionIds.Contains(Candidate.ConnectionId))
				{
					OutResult.GhostItemIds.Add(Candidate.ItemId);
				}
			}
		}
		return !OutResult.IsEmpty();
	}
}
