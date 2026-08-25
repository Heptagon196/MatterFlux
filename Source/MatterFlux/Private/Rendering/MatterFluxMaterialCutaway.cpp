#include "Rendering/MatterFluxMaterialCutaway.h"

#include "Fragment/Fragment2DSourceActor.h"

namespace
{
	struct FMaterialNode
	{
		AFragment2DSourceActor* Source = nullptr;
		FBox WorldBounds = FBox(ForceInit);
		float CellSize = 1.0f;
	};

	bool BuildLiveMaterialNode(
		AFragment2DSourceActor* Source,
		FMaterialNode& OutNode)
	{
		if (!IsValid(Source)
			|| Source->bBroken
			|| !Source->SourceId.IsValid()
			|| Source->StructuralRole
				== EMatterFluxMaterialStructuralRole::None)
		{
			return false;
		}

		const int32 Width = Source->GetMaskWidth();
		const int32 Height = Source->GetMaskHeight();
		const float CellSize = Source->GetCellSize();
		const TArray<uint8>& Mask = Source->GetRuntimeMask();
		if (Width <= 0 || Height <= 0 || CellSize <= 0.0f
			|| Mask.Num() != Width * Height)
		{
			return false;
		}

		int32 MinX = Width;
		int32 MaxX = INDEX_NONE;
		int32 MinZ = Height;
		int32 MaxZ = INDEX_NONE;
		for (int32 Z = 0; Z < Height; ++Z)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				if (Mask[Z * Width + X] == 0)
				{
					continue;
				}
				MinX = FMath::Min(MinX, X);
				MaxX = FMath::Max(MaxX, X);
				MinZ = FMath::Min(MinZ, Z);
				MaxZ = FMath::Max(MaxZ, Z);
			}
		}
		if (MaxX == INDEX_NONE || MaxZ == INDEX_NONE)
		{
			return false;
		}

		const FVector LocalMinimum(
			(static_cast<float>(MinX) - Width * 0.5f) * CellSize,
			-CellSize * 0.5f,
			(static_cast<float>(MinZ) - Height * 0.5f) * CellSize);
		const FVector LocalMaximum(
			(static_cast<float>(MaxX + 1) - Width * 0.5f) * CellSize,
			CellSize * 0.5f,
			(static_cast<float>(MaxZ + 1) - Height * 0.5f) * CellSize);

		OutNode.Source = Source;
		OutNode.CellSize = CellSize;
		OutNode.WorldBounds =
			FBox(LocalMinimum, LocalMaximum).TransformBy(
				Source->GetActorTransform());
		return OutNode.WorldBounds.IsValid != 0;
	}

	float AxisGap(
		const float MinimumA,
		const float MaximumA,
		const float MinimumB,
		const float MaximumB)
	{
		return FMath::Max(
			FMath::Max(MinimumA - MaximumB, MinimumB - MaximumA),
			0.0f);
	}

	bool AreConnected(
		const FMaterialNode& A,
		const FMaterialNode& B,
		const float ConfiguredTolerance)
	{
		const float ContactTolerance = FMath::Max(
			ConfiguredTolerance,
			FMath::Max(A.CellSize, B.CellSize) * 0.25f);
		return AxisGap(
			A.WorldBounds.Min.X, A.WorldBounds.Max.X,
			B.WorldBounds.Min.X, B.WorldBounds.Max.X) <= ContactTolerance
			&& AxisGap(
				A.WorldBounds.Min.Y, A.WorldBounds.Max.Y,
				B.WorldBounds.Min.Y, B.WorldBounds.Max.Y) <= ContactTolerance
			&& AxisGap(
				A.WorldBounds.Min.Z, A.WorldBounds.Max.Z,
				B.WorldBounds.Min.Z, B.WorldBounds.Max.Z) <= ContactTolerance;
	}

	bool CutawaySegmentIntersectsBox(
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

			float EntryTime = (Box.Min[Axis] - Start[Axis]) / Direction[Axis];
			float ExitTime = (Box.Max[Axis] - Start[Axis]) / Direction[Axis];
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

	bool IsExteriorStructureRole(
		const EMatterFluxMaterialStructuralRole Role)
	{
		return Role == EMatterFluxMaterialStructuralRole::Wall
			|| Role == EMatterFluxMaterialStructuralRole::Floor;
	}

	void BuildCutawayViewerProbePoints(
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

namespace MatterFlux::MaterialCutaway
{
	bool Resolve(
		const FVector& ViewerFeet,
		const TConstArrayView<AFragment2DSourceActor*> Sources,
		const FGuid& PreferredFloorSourceId,
		const FPolicy& Policy,
		FResult& OutResult)
	{
		OutResult = FResult();

		TArray<FMaterialNode, TInlineAllocator<64>> Nodes;
		Nodes.Reserve(Sources.Num());
		for (AFragment2DSourceActor* Source : Sources)
		{
			FMaterialNode& Node = Nodes.AddDefaulted_GetRef();
			if (!BuildLiveMaterialNode(Source, Node))
			{
				Nodes.Pop(EAllowShrinking::No);
			}
		}

		int32 FloorNodeIndex = INDEX_NONE;
		float HighestFloorSurface = -FLT_MAX;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			const FMaterialNode& Node = Nodes[NodeIndex];
			if (Node.Source->StructuralRole
					!= EMatterFluxMaterialStructuralRole::Floor
				|| ViewerFeet.X < Node.WorldBounds.Min.X - 2.0f
				|| ViewerFeet.X > Node.WorldBounds.Max.X + 2.0f
				|| ViewerFeet.Y < Node.WorldBounds.Min.Y - 2.0f
				|| ViewerFeet.Y > Node.WorldBounds.Max.Y + 2.0f)
			{
				continue;
			}

			const float FloorSurface = Node.WorldBounds.Max.Z;
			const float SnapHeight = FMath::Max(
				Policy.FloorSnapHeightCentimeters,
				Node.CellSize * 0.5f);
			if (FMath::Abs(FloorSurface - ViewerFeet.Z) <= SnapHeight
				&& FloorSurface > HighestFloorSurface)
			{
				FloorNodeIndex = NodeIndex;
				HighestFloorSurface = FloorSurface;
			}
		}

		if (FloorNodeIndex == INDEX_NONE && PreferredFloorSourceId.IsValid())
		{
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const FMaterialNode& Node = Nodes[NodeIndex];
				if (Node.Source->SourceId != PreferredFloorSourceId
					|| Node.Source->StructuralRole
						!= EMatterFluxMaterialStructuralRole::Floor)
				{
					continue;
				}
				const float Padding = FMath::Max(
					Policy.PreferredFloorPaddingCentimeters, 0.0f);
				const float VerticalRange = FMath::Max(
					Policy.PreferredFloorVerticalRangeCentimeters, 0.0f);
				if (ViewerFeet.X >= Node.WorldBounds.Min.X - Padding
					&& ViewerFeet.X <= Node.WorldBounds.Max.X + Padding
					&& ViewerFeet.Y >= Node.WorldBounds.Min.Y - Padding
					&& ViewerFeet.Y <= Node.WorldBounds.Max.Y + Padding
					&& FMath::Abs(ViewerFeet.Z - Node.WorldBounds.Max.Z)
						<= VerticalRange)
				{
					FloorNodeIndex = NodeIndex;
					HighestFloorSurface = Node.WorldBounds.Max.Z;
				}
				break;
			}
		}

		if (FloorNodeIndex == INDEX_NONE)
		{
			return false;
		}

		const FMaterialNode& FloorNode = Nodes[FloorNodeIndex];
		OutResult.FloorSourceId = FloorNode.Source->SourceId;
		OutResult.FloorSurfaceZ = HighestFloorSurface;

		TArray<float, TInlineAllocator<16>> FloorSurfaces;
		for (const FMaterialNode& Node : Nodes)
		{
			if (Node.Source->StructuralRole
				== EMatterFluxMaterialStructuralRole::Floor)
			{
				FloorSurfaces.Add(Node.WorldBounds.Max.Z);
			}
		}
		FloorSurfaces.Sort();
		OutResult.FloorOrdinal = 0;
		float PreviousDistinctSurface = 0.0f;
		bool bHasPreviousDistinctSurface = false;
		const float SurfaceTolerance = FMath::Max(
			1.0f, FloorNode.CellSize * 0.5f);
		for (const float Surface : FloorSurfaces)
		{
			if (bHasPreviousDistinctSurface && FMath::IsNearlyEqual(
				Surface, PreviousDistinctSurface, SurfaceTolerance))
			{
				continue;
			}
			if (Surface < HighestFloorSurface - SurfaceTolerance)
			{
				++OutResult.FloorOrdinal;
			}
			PreviousDistinctSurface = Surface;
			bHasPreviousDistinctSurface = true;
		}

		// Connectivity is evaluated exclusively from live material masks. Actor
		// ownership, class names and house bounds are intentionally absent here.
		TArray<int32, TInlineAllocator<64>> ConnectedNodeIndices;
		ConnectedNodeIndices.Add(FloorNodeIndex);
		for (int32 ConnectedIndex = 0;
			ConnectedIndex < ConnectedNodeIndices.Num();
			++ConnectedIndex)
		{
			const FMaterialNode& ConnectedNode =
				Nodes[ConnectedNodeIndices[ConnectedIndex]];
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const FMaterialNode& Node = Nodes[NodeIndex];
				if (Node.Source->StructuralRole
						!= EMatterFluxMaterialStructuralRole::Wall
					|| Node.WorldBounds.Max.Z
						<= HighestFloorSurface + 1.0f
					|| OutResult.GhostSourceIds.Contains(Node.Source->SourceId)
					|| !AreConnected(
						ConnectedNode,
						Node,
						Policy.ContactToleranceCentimeters))
				{
					continue;
				}
				OutResult.GhostSourceIds.Add(Node.Source->SourceId);
				ConnectedNodeIndices.Add(NodeIndex);
			}
		}

		return true;
	}

	bool ResolveOccludingWalls(
		const FVector& CameraLocation,
		const FBox& ViewerBounds,
		const TConstArrayView<AFragment2DSourceActor*> Sources,
		const FPolicy& Policy,
		FResult& OutResult)
	{
		OutResult = FResult();
		if (CameraLocation.ContainsNaN() || !ViewerBounds.IsValid)
		{
			return false;
		}

		TArray<FMaterialNode, TInlineAllocator<64>> Nodes;
		Nodes.Reserve(Sources.Num());
		for (AFragment2DSourceActor* Source : Sources)
		{
			FMaterialNode& Node = Nodes.AddDefaulted_GetRef();
			if (!BuildLiveMaterialNode(Source, Node))
			{
				Nodes.Pop(EAllowShrinking::No);
			}
		}

		TArray<FVector, TInlineAllocator<5>> ProbePoints;
		BuildCutawayViewerProbePoints(CameraLocation, ViewerBounds, ProbePoints);
		TArray<int32, TInlineAllocator<32>> ConnectedNodeIndices;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			const FMaterialNode& Node = Nodes[NodeIndex];
			if (Node.Source->StructuralRole
				!= EMatterFluxMaterialStructuralRole::Wall)
			{
				continue;
			}
			const FBox ProbeBounds = Node.WorldBounds.ExpandBy(
				FMath::Max(Policy.OcclusionProbeRadiusCentimeters, 0.0f));
			const bool bBlocksViewer = ProbePoints.ContainsByPredicate(
				[&CameraLocation, &ProbeBounds](const FVector& ProbePoint)
				{
					return CutawaySegmentIntersectsBox(
						CameraLocation, ProbePoint, ProbeBounds);
				});
			if (bBlocksViewer)
			{
				OutResult.GhostSourceIds.Add(Node.Source->SourceId);
				ConnectedNodeIndices.Add(NodeIndex);
			}
		}

		for (int32 ConnectedIndex = 0;
			ConnectedIndex < ConnectedNodeIndices.Num();
			++ConnectedIndex)
		{
			const FMaterialNode& ConnectedNode =
				Nodes[ConnectedNodeIndices[ConnectedIndex]];
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const FMaterialNode& Node = Nodes[NodeIndex];
				if (!IsExteriorStructureRole(Node.Source->StructuralRole)
					|| OutResult.GhostSourceIds.Contains(Node.Source->SourceId)
					|| !AreConnected(
						ConnectedNode,
						Node,
						Policy.ContactToleranceCentimeters))
				{
					continue;
				}
				OutResult.GhostSourceIds.Add(Node.Source->SourceId);
				ConnectedNodeIndices.Add(NodeIndex);
			}
		}
		return !OutResult.GhostSourceIds.IsEmpty();
	}
}
