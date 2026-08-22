#include "Rendering/MatterFluxWholeObjectGeometry.h"

namespace MatterFlux::WholeObject
{
	namespace
	{
		constexpr int32 MaximumWholeObjectCells = 1024 * 1024;
		constexpr float GridTolerance = 0.01f;

		struct FVoxel
		{
			int32 MaterialIndex = INDEX_NONE;
			int32 Priority = 0;
			bool bEnableCollision = false;
		};

		struct FFacePlaneKey
		{
			int32 Direction = 0;
			int32 Plane = 0;
			int32 MaterialIndex = INDEX_NONE;
			bool bEnableCollision = false;

			bool operator==(const FFacePlaneKey& Other) const
			{
				return Direction == Other.Direction
					&& Plane == Other.Plane
					&& MaterialIndex == Other.MaterialIndex
					&& bEnableCollision == Other.bEnableCollision;
			}

			friend uint32 GetTypeHash(const FFacePlaneKey& Key)
			{
				uint32 Hash = HashCombineFast(
					GetTypeHash(Key.Direction), GetTypeHash(Key.Plane));
				Hash = HashCombineFast(Hash, GetTypeHash(Key.MaterialIndex));
				return HashCombineFast(Hash, GetTypeHash(Key.bEnableCollision));
			}
		};

		struct FSectionKey
		{
			int32 MaterialIndex = INDEX_NONE;
			EFaceRole FaceRole = EFaceRole::Side;
			bool bEnableCollision = false;

			bool operator==(const FSectionKey& Other) const
			{
				return MaterialIndex == Other.MaterialIndex
					&& FaceRole == Other.FaceRole
					&& bEnableCollision == Other.bEnableCollision;
			}

			friend uint32 GetTypeHash(const FSectionKey& Key)
			{
				return HashCombineFast(
					HashCombineFast(
						GetTypeHash(Key.MaterialIndex),
						GetTypeHash(static_cast<uint8>(Key.FaceRole))),
					GetTypeHash(Key.bEnableCollision));
			}
		};

		const FIntVector Directions[6] = {
			FIntVector(0, 1, 0),
			FIntVector(0, -1, 0),
			FIntVector(1, 0, 0),
			FIntVector(-1, 0, 0),
			FIntVector(0, 0, 1),
			FIntVector(0, 0, -1)
		};

		EFaceRole RoleForDirection(const int32 Direction)
		{
			switch (Direction)
			{
			case 0:
			case 1:
				return EFaceRole::FrontBack;
			case 4:
				return EFaceRole::Top;
			case 5:
				return EFaceRole::Bottom;
			default:
				return EFaceRole::Side;
			}
		}

		void ToPlaneCoordinates(
			const int32 Direction,
			const FIntVector& Cell,
			int32& OutPlane,
			FIntPoint& OutPoint)
		{
			if (Direction <= 1)
			{
				OutPlane = Cell.Y;
				OutPoint = FIntPoint(Cell.X, Cell.Z);
			}
			else if (Direction <= 3)
			{
				OutPlane = Cell.X;
				OutPoint = FIntPoint(Cell.Y, Cell.Z);
			}
			else
			{
				OutPlane = Cell.Z;
				OutPoint = FIntPoint(Cell.X, Cell.Y);
			}
		}

		FIntVector FromPlaneCoordinates(
			const int32 Direction,
			const int32 Plane,
			const int32 U,
			const int32 V)
		{
			if (Direction <= 1)
			{
				return FIntVector(U, Plane, V);
			}
			if (Direction <= 3)
			{
				return FIntVector(Plane, U, V);
			}
			return FIntVector(U, V, Plane);
		}

		bool IsAxisAlignedRotation(const FTransform& Transform)
		{
			for (const FVector Axis : {
				FVector::XAxisVector,
				FVector::YAxisVector,
				FVector::ZAxisVector})
			{
				const FVector Rotated =
					Transform.TransformVectorNoScale(Axis).GetSafeNormal();
				const FVector Absolute = Rotated.GetAbs();
				const float Maximum = FMath::Max3(Absolute.X, Absolute.Y, Absolute.Z);
				if (!FMath::IsNearlyEqual(Maximum, 1.0f, GridTolerance)
					|| Absolute.X + Absolute.Y + Absolute.Z > 1.0f + GridTolerance)
				{
					return false;
				}
			}
			return true;
		}

		uint8 ComputeCornerAO(
			const TMap<FIntVector, FVoxel>& Occupied,
			const FIntVector& FaceCell,
			const FIntVector& Normal,
			const FIntVector& TangentA,
			const FIntVector& TangentB)
		{
			const bool bSideA = Occupied.Contains(
				FaceCell + Normal + TangentA);
			const bool bSideB = Occupied.Contains(
				FaceCell + Normal + TangentB);
			const bool bCorner = Occupied.Contains(
				FaceCell + Normal + TangentA + TangentB);
			const int32 Level = bSideA && bSideB
				? 0
				: 3 - static_cast<int32>(bSideA)
					- static_cast<int32>(bSideB)
					- static_cast<int32>(bCorner);
			// AO 只能提示相邻体素关系，不能把朝下的凸面压成黑洞。
			// 旧曲线最低只有 55% 亮度，再叠加真实光照和材质面差后，
			// 树冠底面会产生强烈的“向内凹”错觉。
			constexpr uint8 Brightness[4] = {198, 216, 236, 255};
			return Brightness[FMath::Clamp(Level, 0, 3)];
		}

		void AddMergedQuad(
			const FFacePlaneKey& PlaneKey,
			const FIntPoint Min,
			const FIntPoint Max,
			const FVector& Anchor,
			const float CellSize,
			const TMap<FIntVector, FVoxel>& Occupied,
			FMeshSection& Section)
		{
			const int32 Direction = PlaneKey.Direction;
			const FIntVector NormalCell = Directions[Direction];
			const FVector Normal(NormalCell);
			const FIntVector CornerCells[4] = {
				FromPlaneCoordinates(Direction, PlaneKey.Plane, Min.X, Min.Y),
				FromPlaneCoordinates(Direction, PlaneKey.Plane, Min.X, Max.Y),
				FromPlaneCoordinates(Direction, PlaneKey.Plane, Max.X, Max.Y),
				FromPlaneCoordinates(Direction, PlaneKey.Plane, Max.X, Min.Y)
			};

			FVector P[4];
			FIntVector TangentA[4];
			FIntVector TangentB[4];
			const float UMin = static_cast<float>(Min.X) - 0.5f;
			const float UMax = static_cast<float>(Max.X) + 0.5f;
			const float VMin = static_cast<float>(Min.Y) - 0.5f;
			const float VMax = static_cast<float>(Max.Y) + 0.5f;
			const float Plane = static_cast<float>(PlaneKey.Plane)
				+ (Direction % 2 == 0 ? 0.5f : -0.5f);

			if (Direction == 0)
			{
				P[0] = FVector(UMin, Plane, VMin);
				P[1] = FVector(UMin, Plane, VMax);
				P[2] = FVector(UMax, Plane, VMax);
				P[3] = FVector(UMax, Plane, VMin);
				TangentA[0] = TangentA[1] = FIntVector(-1, 0, 0);
				TangentA[2] = TangentA[3] = FIntVector(1, 0, 0);
				TangentB[0] = TangentB[3] = FIntVector(0, 0, -1);
				TangentB[1] = TangentB[2] = FIntVector(0, 0, 1);
			}
			else if (Direction == 1)
			{
				P[0] = FVector(UMin, Plane, VMin);
				P[1] = FVector(UMax, Plane, VMin);
				P[2] = FVector(UMax, Plane, VMax);
				P[3] = FVector(UMin, Plane, VMax);
				TangentA[0] = TangentA[3] = FIntVector(-1, 0, 0);
				TangentA[1] = TangentA[2] = FIntVector(1, 0, 0);
				TangentB[0] = TangentB[1] = FIntVector(0, 0, -1);
				TangentB[2] = TangentB[3] = FIntVector(0, 0, 1);
			}
			else if (Direction == 2)
			{
				P[0] = FVector(Plane, UMin, VMin);
				P[1] = FVector(Plane, UMax, VMin);
				P[2] = FVector(Plane, UMax, VMax);
				P[3] = FVector(Plane, UMin, VMax);
				TangentA[0] = TangentA[3] = FIntVector(0, -1, 0);
				TangentA[1] = TangentA[2] = FIntVector(0, 1, 0);
				TangentB[0] = TangentB[1] = FIntVector(0, 0, -1);
				TangentB[2] = TangentB[3] = FIntVector(0, 0, 1);
			}
			else if (Direction == 3)
			{
				P[0] = FVector(Plane, UMax, VMin);
				P[1] = FVector(Plane, UMin, VMin);
				P[2] = FVector(Plane, UMin, VMax);
				P[3] = FVector(Plane, UMax, VMax);
				TangentA[0] = TangentA[3] = FIntVector(0, 1, 0);
				TangentA[1] = TangentA[2] = FIntVector(0, -1, 0);
				TangentB[0] = TangentB[1] = FIntVector(0, 0, -1);
				TangentB[2] = TangentB[3] = FIntVector(0, 0, 1);
			}
			else if (Direction == 4)
			{
				P[0] = FVector(UMin, VMin, Plane);
				P[1] = FVector(UMax, VMin, Plane);
				P[2] = FVector(UMax, VMax, Plane);
				P[3] = FVector(UMin, VMax, Plane);
				TangentA[0] = TangentA[3] = FIntVector(-1, 0, 0);
				TangentA[1] = TangentA[2] = FIntVector(1, 0, 0);
				TangentB[0] = TangentB[1] = FIntVector(0, -1, 0);
				TangentB[2] = TangentB[3] = FIntVector(0, 1, 0);
			}
			else
			{
				P[0] = FVector(UMin, VMax, Plane);
				P[1] = FVector(UMax, VMax, Plane);
				P[2] = FVector(UMax, VMin, Plane);
				P[3] = FVector(UMin, VMin, Plane);
				TangentA[0] = TangentA[3] = FIntVector(-1, 0, 0);
				TangentA[1] = TangentA[2] = FIntVector(1, 0, 0);
				TangentB[0] = TangentB[1] = FIntVector(0, 1, 0);
				TangentB[2] = TangentB[3] = FIntVector(0, -1, 0);
			}

			const int32 BaseVertex = Section.Vertices.Num();
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				Section.Vertices.Add(Anchor + P[Corner] * CellSize);
				Section.Normals.Add(Normal);
				const uint8 AO = ComputeCornerAO(
					Occupied,
					CornerCells[Corner],
					NormalCell,
					TangentA[Corner],
					TangentB[Corner]);
				Section.VertexColors.Add(FColor(AO, AO, AO, 255));
			}
			const float Width = static_cast<float>(Max.X - Min.X + 1);
			const float Height = static_cast<float>(Max.Y - Min.Y + 1);
			Section.UVs.Append({
				FVector2D(0.0f, 0.0f),
				FVector2D(0.0f, Height),
				FVector2D(Width, Height),
				FVector2D(Width, 0.0f)});
			Section.Triangles.Append({
				// ProceduralMesh 使用顺时针顶点作为正面。原绕序会剔除
				// 靠近镜头的立方体面，斜视时只剩远端两面，底边因而
				// 错误地呈现为向内凹的 ^，而不是向下凸的 V。
				BaseVertex, BaseVertex + 2, BaseVertex + 1,
				BaseVertex, BaseVertex + 3, BaseVertex + 2});
		}

		bool SectionKeyLess(const FSectionKey& A, const FSectionKey& B)
		{
			if (A.MaterialIndex != B.MaterialIndex)
			{
				return A.MaterialIndex < B.MaterialIndex;
			}
			if (A.FaceRole != B.FaceRole)
			{
				return static_cast<uint8>(A.FaceRole)
					< static_cast<uint8>(B.FaceRole);
			}
			return static_cast<int32>(A.bEnableCollision)
				< static_cast<int32>(B.bEnableCollision);
		}
	}

	bool FLayer::IsValid() const
	{
		const int64 CellCount = static_cast<int64>(Width) * Height;
		if (MaterialIndex < 0
			|| Width <= 0 || Width > 256
			|| Height <= 0 || Height > 256
			|| CellCount != SolidMask.Num()
			|| !FMath::IsFinite(CellSize) || CellSize <= 0.0f
			|| !LocalTransform.IsValid()
			|| !LocalTransform.GetScale3D().Equals(
				FVector::OneVector, GridTolerance)
			|| !IsAxisAlignedRotation(LocalTransform))
		{
			return false;
		}
		for (const uint8 Cell : SolidMask)
		{
			if (Cell > 1)
			{
				return false;
			}
		}
		return SolidMask.Contains(1);
	}

	void FBuildResult::Reset()
	{
		Sections.Reset();
		LocalBounds = FBox(ForceInit);
		OccupiedCellCount = 0;
		VisibleQuadCount = 0;
	}

	bool BuildMesh(
		const TArray<FLayer>& Layers,
		FBuildResult& OutResult,
		FString* OutError)
	{
		OutResult.Reset();
		if (OutError)
		{
			OutError->Reset();
		}
		const auto Fail = [&OutResult, OutError](const TCHAR* Message)
		{
			OutResult.Reset();
			if (OutError)
			{
				*OutError = Message;
			}
			return false;
		};
		if (Layers.IsEmpty())
		{
			return Fail(TEXT("Whole object has no layers"));
		}

		float CellSize = 0.0f;
		TArray<FVector> CellCenters;
		TArray<TPair<int32, int32>> CenterLayerAndIndex;
		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
		{
			const FLayer& Layer = Layers[LayerIndex];
			if (!Layer.IsValid())
			{
				return Fail(TEXT("Whole object contains an invalid layer"));
			}
			if (CellSize == 0.0f)
			{
				CellSize = Layer.CellSize;
			}
			else if (!FMath::IsNearlyEqual(
				CellSize, Layer.CellSize, KINDA_SMALL_NUMBER))
			{
				return Fail(TEXT("Whole object layers use different cell sizes"));
			}
			for (int32 Y = 0; Y < Layer.Height; ++Y)
			{
				for (int32 X = 0; X < Layer.Width; ++X)
				{
					const int32 MaskIndex = Y * Layer.Width + X;
					if (Layer.SolidMask[MaskIndex] == 0)
					{
						continue;
					}
					const FVector LocalCenter(
						(static_cast<float>(X) + 0.5f
							- static_cast<float>(Layer.Width) * 0.5f) * CellSize,
						0.0f,
						(static_cast<float>(Y) + 0.5f
							- static_cast<float>(Layer.Height) * 0.5f) * CellSize);
					CellCenters.Add(
						Layer.LocalTransform.TransformPosition(LocalCenter));
					CenterLayerAndIndex.Emplace(LayerIndex, MaskIndex);
					if (CellCenters.Num() > MaximumWholeObjectCells)
					{
						return Fail(TEXT("Whole object exceeds the cell budget"));
					}
				}
			}
		}
		if (CellCenters.IsEmpty())
		{
			return Fail(TEXT("Whole object has no occupied cells"));
		}

		FVector Anchor = CellCenters[0];
		for (const FVector& Center : CellCenters)
		{
			Anchor.X = FMath::Min(Anchor.X, Center.X);
			Anchor.Y = FMath::Min(Anchor.Y, Center.Y);
			Anchor.Z = FMath::Min(Anchor.Z, Center.Z);
		}

		TMap<FIntVector, FVoxel> Occupied;
		Occupied.Reserve(CellCenters.Num());
		for (int32 Index = 0; Index < CellCenters.Num(); ++Index)
		{
			const FVector GridPosition = (CellCenters[Index] - Anchor) / CellSize;
			const FIntVector GridCell(
				FMath::RoundToInt(GridPosition.X),
				FMath::RoundToInt(GridPosition.Y),
				FMath::RoundToInt(GridPosition.Z));
			if (!GridPosition.Equals(FVector(GridCell), GridTolerance))
			{
				OutResult.Reset();
				if (OutError)
				{
					*OutError = FString::Printf(
						TEXT("Whole object layer %d cell %d does not share the voxel lattice: grid=%s rounded=(%d,%d,%d) anchor=%s cell=%.3f"),
						CenterLayerAndIndex[Index].Key,
						CenterLayerAndIndex[Index].Value,
						*GridPosition.ToString(),
						GridCell.X,
						GridCell.Y,
						GridCell.Z,
						*Anchor.ToString(),
						CellSize);
				}
				return false;
			}
			const FLayer& Layer = Layers[CenterLayerAndIndex[Index].Key];
			if (FVoxel* Existing = Occupied.Find(GridCell))
			{
				if (Layer.Priority > Existing->Priority
					|| (Layer.Priority == Existing->Priority
						&& Layer.MaterialIndex < Existing->MaterialIndex))
				{
					*Existing = {
						Layer.MaterialIndex,
						Layer.Priority,
						Layer.bEnableCollision};
				}
				else if (Layer.MaterialIndex == Existing->MaterialIndex)
				{
					Existing->bEnableCollision |= Layer.bEnableCollision;
				}
			}
			else
			{
				Occupied.Add(GridCell, {
					Layer.MaterialIndex,
					Layer.Priority,
					Layer.bEnableCollision});
			}
		}

		TMap<FFacePlaneKey, TSet<FIntPoint>> FacesByPlane;
		for (const TPair<FIntVector, FVoxel>& Pair : Occupied)
		{
			for (int32 Direction = 0; Direction < 6; ++Direction)
			{
				if (Occupied.Contains(Pair.Key + Directions[Direction]))
				{
					continue;
				}
				int32 Plane = 0;
				FIntPoint Point;
				ToPlaneCoordinates(Direction, Pair.Key, Plane, Point);
				FacesByPlane.FindOrAdd({
					Direction,
					Plane,
					Pair.Value.MaterialIndex,
					Pair.Value.bEnableCollision}).Add(Point);
			}
		}

		TMap<FSectionKey, FMeshSection> Sections;
		TArray<FFacePlaneKey> PlaneKeys;
		FacesByPlane.GenerateKeyArray(PlaneKeys);
		PlaneKeys.Sort([](const FFacePlaneKey& A, const FFacePlaneKey& B)
		{
			if (A.MaterialIndex != B.MaterialIndex)
			{
				return A.MaterialIndex < B.MaterialIndex;
			}
			if (A.Direction != B.Direction)
			{
				return A.Direction < B.Direction;
			}
			if (A.Plane != B.Plane)
			{
				return A.Plane < B.Plane;
			}
			return static_cast<int32>(A.bEnableCollision)
				< static_cast<int32>(B.bEnableCollision);
		});
		for (const FFacePlaneKey& PlaneKey : PlaneKeys)
		{
			TSet<FIntPoint> Remaining = FacesByPlane.FindChecked(PlaneKey);
			while (!Remaining.IsEmpty())
			{
				TArray<FIntPoint> Ordered = Remaining.Array();
				Ordered.Sort([](const FIntPoint A, const FIntPoint B)
				{
					return A.Y == B.Y ? A.X < B.X : A.Y < B.Y;
				});
				const FIntPoint Min = Ordered[0];
				int32 MaxX = Min.X;
				while (Remaining.Contains(FIntPoint(MaxX + 1, Min.Y)))
				{
					++MaxX;
				}
				int32 MaxY = Min.Y;
				for (;;)
				{
					const int32 CandidateY = MaxY + 1;
					bool bCompleteRow = true;
					for (int32 X = Min.X; X <= MaxX; ++X)
					{
						bCompleteRow &= Remaining.Contains(
							FIntPoint(X, CandidateY));
					}
					if (!bCompleteRow)
					{
						break;
					}
					MaxY = CandidateY;
				}
				for (int32 Y = Min.Y; Y <= MaxY; ++Y)
				{
					for (int32 X = Min.X; X <= MaxX; ++X)
					{
						Remaining.Remove(FIntPoint(X, Y));
					}
				}

				const FSectionKey SectionKey{
					PlaneKey.MaterialIndex,
					RoleForDirection(PlaneKey.Direction),
					PlaneKey.bEnableCollision};
				FMeshSection& Section = Sections.FindOrAdd(SectionKey);
				Section.MaterialIndex = SectionKey.MaterialIndex;
				Section.FaceRole = SectionKey.FaceRole;
				Section.bEnableCollision = SectionKey.bEnableCollision;
				AddMergedQuad(
					PlaneKey,
					Min,
					FIntPoint(MaxX, MaxY),
					Anchor,
					CellSize,
					Occupied,
					Section);
				++OutResult.VisibleQuadCount;
			}
		}

		TArray<FSectionKey> SectionKeys;
		Sections.GenerateKeyArray(SectionKeys);
		SectionKeys.Sort(SectionKeyLess);
		for (const FSectionKey& Key : SectionKeys)
		{
			FMeshSection Section = MoveTemp(Sections.FindChecked(Key));
			if (Section.Vertices.Num() != Section.Normals.Num()
				|| Section.Vertices.Num() != Section.UVs.Num()
				|| Section.Vertices.Num() != Section.VertexColors.Num()
				|| Section.Triangles.IsEmpty())
			{
				return Fail(TEXT("Whole object produced an invalid mesh section"));
			}
			for (const int32 TriangleIndex : Section.Triangles)
			{
				if (!Section.Vertices.IsValidIndex(TriangleIndex))
				{
					return Fail(TEXT("Whole object produced an invalid triangle index"));
				}
			}
			for (const FVector& Vertex : Section.Vertices)
			{
				OutResult.LocalBounds += Vertex;
			}
			OutResult.Sections.Add(MoveTemp(Section));
		}
		OutResult.OccupiedCellCount = Occupied.Num();
		return !OutResult.Sections.IsEmpty();
	}
}
