#include "Volume/MatterFluxMaterialVolume.h"

#include "Fragment/FragmentTypes.h"
#include "Game/MatterFluxPlayableLevel.h"

uint16 FMaterialVolumeFields::GetEnergy(const FIntVector& Cell) const
{
	if (const uint16* Found = EnergyOverrides.Find(Cell))
	{
		return *Found;
	}
	return EnvironmentEnergy;
}

bool FMaterialVolumeFields::SetEnergy(
	const FIntVector& Cell,
	const uint16 Energy)
{
	const uint16 Before = GetEnergy(Cell);
	if (Before == Energy)
	{
		return false;
	}
	if (Energy == EnvironmentEnergy)
	{
		EnergyOverrides.Remove(Cell);
	}
	else
	{
		EnergyOverrides.Add(Cell, Energy);
	}
	++FieldRevision;
	return true;
}

namespace MatterFluxMaterialVolumePrivate
{
	constexpr uint64 FnvOffset = 14695981039346656037ull;
	constexpr uint64 FnvPrime = 1099511628211ull;

	bool CellLess(const FIntVector& Left, const FIntVector& Right)
	{
		if (Left.X != Right.X) return Left.X < Right.X;
		if (Left.Y != Right.Y) return Left.Y < Right.Y;
		return Left.Z < Right.Z;
	}

	bool PointLess(const FIntPoint& Left, const FIntPoint& Right)
	{
		return Left.X != Right.X ? Left.X < Right.X : Left.Y < Right.Y;
	}

	void HashByte(uint64& Hash, const uint8 Value)
	{
		Hash ^= Value;
		Hash *= FnvPrime;
	}

	template <typename IntegerType>
	void HashInteger(uint64& Hash, const IntegerType Value)
	{
		using UnsignedType = std::make_unsigned_t<IntegerType>;
		const UnsignedType Bits = static_cast<UnsignedType>(Value);
		for (uint32 ByteIndex = 0; ByteIndex < sizeof(IntegerType); ++ByteIndex)
		{
			HashByte(Hash, static_cast<uint8>(Bits >> (ByteIndex * 8)));
		}
	}

	void HashDouble(uint64& Hash, const double Value)
	{
		uint64 Bits = 0;
		static_assert(sizeof(Bits) == sizeof(Value));
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		HashInteger(Hash, Bits);
	}

	void HashName(uint64& Hash, const FName Name)
	{
		const FTCHARToUTF8 Utf8(*Name.ToString());
		HashInteger(Hash, Utf8.Length());
		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			HashByte(Hash, static_cast<uint8>(Utf8.Get()[Index]));
		}
	}

	int32 FloorDivide(const int32 Value, const int32 Divisor)
	{
		const int32 Quotient = Value / Divisor;
		const int32 Remainder = Value % Divisor;
		return Remainder < 0 ? Quotient - 1 : Quotient;
	}

	struct FSpanNode
	{
		FIntPoint Column = FIntPoint::ZeroValue;
		FMaterialSpan Span;
	};

	bool HasUnseamedConnection(
		const FSpanNode& Left,
		const FSpanNode& Right,
		const TSet<FStructuralSeam>& Seams)
	{
		const int32 DeltaU = FMath::Abs(Left.Column.X - Right.Column.X);
		const int32 DeltaV = FMath::Abs(Left.Column.Y - Right.Column.Y);
		if (DeltaU + DeltaV == 0)
		{
			const FSpanNode* Lower = &Left;
			const FSpanNode* Upper = &Right;
			if (Lower->Span.BeginN > Upper->Span.BeginN)
			{
				Swap(Lower, Upper);
			}
			if (Lower->Span.EndNExclusive != Upper->Span.BeginN)
			{
				return false;
			}
			return !Seams.Contains(FStructuralSeam(
				FIntVector(Left.Column.X, Left.Column.Y, Lower->Span.EndNExclusive - 1),
				FIntVector(Left.Column.X, Left.Column.Y, Upper->Span.BeginN)));
		}
		if (DeltaU + DeltaV != 1)
		{
			return false;
		}
		const int32 Begin = FMath::Max(Left.Span.BeginN, Right.Span.BeginN);
		const int32 End = FMath::Min(
			Left.Span.EndNExclusive,
			Right.Span.EndNExclusive);
		for (int32 N = Begin; N < End; ++N)
		{
			if (!Seams.Contains(FStructuralSeam(
				FIntVector(Left.Column.X, Left.Column.Y, N),
				FIntVector(Right.Column.X, Right.Column.Y, N))))
			{
				return true;
			}
		}
		return false;
	}

	FVector3d CellCenter(
		const FMaterialGridFrame& Frame,
		const FIntVector& Cell)
	{
		return Frame.Origin
			+ Frame.BasisU * ((static_cast<double>(Cell.X) + 0.5) * Frame.CellSize)
			+ Frame.BasisV * ((static_cast<double>(Cell.Y) + 0.5) * Frame.CellSize)
			+ Frame.BasisN * ((static_cast<double>(Cell.Z) + 0.5) * Frame.CellSize);
	}

	double PointSegmentDistanceSquared(
		const FVector3d& Point,
		const FVector3d& Start,
		const FVector3d& End)
	{
		const FVector3d Segment = End - Start;
		const double LengthSquared = Segment.SquaredLength();
		if (LengthSquared <= UE_DOUBLE_SMALL_NUMBER)
		{
			return FVector3d::DistSquared(Point, Start);
		}
		const double T = FMath::Clamp(
			FVector3d::DotProduct(Point - Start, Segment) / LengthSquared,
			0.0,
			1.0);
		return FVector3d::DistSquared(Point, Start + Segment * T);
	}

	bool PointInsideBox(
		const FVector3d& Point,
		const FTransform& Transform,
		const FVector& HalfExtent)
	{
		const FVector Local = Transform.InverseTransformPosition(Point);
		return FMath::Abs(Local.X) <= HalfExtent.X
			&& FMath::Abs(Local.Y) <= HalfExtent.Y
			&& FMath::Abs(Local.Z) <= HalfExtent.Z;
	}

	bool PointInsideCut(
		const FVector3d& Point,
		const FMaterialVolumeCut& Cut)
	{
		switch (Cut.Shape)
		{
		case EMaterialVolumeCutShape::Sphere:
			return FVector3d::DistSquared(Point, Cut.StartTransform.GetLocation())
				<= Cut.Radius * Cut.Radius;
		case EMaterialVolumeCutShape::OrientedBox:
			return PointInsideBox(Point, Cut.StartTransform, Cut.HalfExtent);
		case EMaterialVolumeCutShape::Capsule:
			return PointSegmentDistanceSquared(
				Point, Cut.SegmentStart, Cut.SegmentEnd)
				<= Cut.Radius * Cut.Radius;
		case EMaterialVolumeCutShape::PlaneSlab:
			return FMath::Abs(FVector3d::DotProduct(
				Point - Cut.StartTransform.GetLocation(),
				FVector3d(Cut.PlaneNormal))) <= Cut.HalfThickness;
		case EMaterialVolumeCutShape::SweptBlade:
		{
			const double Travel = FVector::Distance(
				Cut.StartTransform.GetLocation(), Cut.EndTransform.GetLocation());
			const double MinimumExtent = FMath::Max(
				FMath::Min3(Cut.HalfExtent.X, Cut.HalfExtent.Y, Cut.HalfExtent.Z),
				1.e-3);
			const int32 SampleCount = FMath::Clamp(
				FMath::CeilToInt(Travel / MinimumExtent) + 1, 2, 64);
			for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
			{
				const double Alpha = static_cast<double>(SampleIndex)
					/ static_cast<double>(SampleCount - 1);
				const FQuat Rotation = FQuat::Slerp(
					Cut.StartTransform.GetRotation(),
					Cut.EndTransform.GetRotation(),
					Alpha).GetNormalized();
				const FVector Location = FMath::Lerp(
					Cut.StartTransform.GetLocation(),
					Cut.EndTransform.GetLocation(),
					Alpha);
				if (PointInsideBox(
						Point,
						FTransform(Rotation, Location, FVector::OneVector),
						Cut.HalfExtent))
				{
					return true;
				}
			}
			return false;
		}
		}
		return false;
	}

	bool IsCellOccupied(
		const FMaterialVolumeTopology& Topology,
		const FIntVector& Cell)
	{
		TArray<FMaterialSpan> Spans;
		if (!FMaterialVolumeAlgorithms::TryGetColumnSpans(
				Topology, FIntPoint(Cell.X, Cell.Y), Spans))
		{
			return false;
		}
		return Spans.ContainsByPredicate([&Cell](const FMaterialSpan& Span)
		{
			return Cell.Z >= Span.BeginN && Cell.Z < Span.EndNExclusive;
		});
	}
}

FMaterialVolumeCut FMaterialVolumeCut::MakeSphere(
	const FVector Center,
	const double InRadius)
{
	FMaterialVolumeCut Result;
	Result.Shape = EMaterialVolumeCutShape::Sphere;
	Result.StartTransform.SetLocation(Center);
	Result.Radius = InRadius;
	return Result;
}

FMaterialVolumeCut FMaterialVolumeCut::MakeOrientedBox(
	const FTransform& Transform,
	const FVector InHalfExtent)
{
	FMaterialVolumeCut Result;
	Result.Shape = EMaterialVolumeCutShape::OrientedBox;
	Result.StartTransform = Transform;
	Result.StartTransform.SetScale3D(FVector::OneVector);
	Result.HalfExtent = InHalfExtent;
	return Result;
}

FMaterialVolumeCut FMaterialVolumeCut::MakeCapsule(
	const FVector InSegmentStart,
	const FVector InSegmentEnd,
	const double InRadius)
{
	FMaterialVolumeCut Result;
	Result.Shape = EMaterialVolumeCutShape::Capsule;
	Result.SegmentStart = InSegmentStart;
	Result.SegmentEnd = InSegmentEnd;
	Result.Radius = InRadius;
	return Result;
}

FMaterialVolumeCut FMaterialVolumeCut::MakePlaneSlab(
	const FVector Origin,
	const FVector Normal,
	const double InHalfThickness)
{
	FMaterialVolumeCut Result;
	Result.Shape = EMaterialVolumeCutShape::PlaneSlab;
	Result.StartTransform.SetLocation(Origin);
	Result.PlaneNormal = Normal.GetSafeNormal();
	Result.HalfThickness = InHalfThickness;
	return Result;
}

FMaterialVolumeCut FMaterialVolumeCut::MakeSweptBlade(
	const FTransform& InStartTransform,
	const FTransform& InEndTransform,
	const FVector InHalfExtent)
{
	FMaterialVolumeCut Result;
	Result.Shape = EMaterialVolumeCutShape::SweptBlade;
	Result.StartTransform = InStartTransform;
	Result.EndTransform = InEndTransform;
	Result.StartTransform.SetScale3D(FVector::OneVector);
	Result.EndTransform.SetScale3D(FVector::OneVector);
	Result.HalfExtent = InHalfExtent;
	return Result;
}

bool FMaterialVolumeCut::IsValid() const
{
	if (!StartTransform.IsValid() || !EndTransform.IsValid())
	{
		return false;
	}
	switch (Shape)
	{
	case EMaterialVolumeCutShape::Sphere:
		return FMath::IsFinite(Radius) && Radius > 0.0;
	case EMaterialVolumeCutShape::OrientedBox:
	case EMaterialVolumeCutShape::SweptBlade:
		return !HalfExtent.ContainsNaN() && HalfExtent.GetMin() > 0.0;
	case EMaterialVolumeCutShape::Capsule:
		return !SegmentStart.ContainsNaN() && !SegmentEnd.ContainsNaN()
			&& FMath::IsFinite(Radius) && Radius > 0.0;
	case EMaterialVolumeCutShape::PlaneSlab:
		return !PlaneNormal.ContainsNaN() && !PlaneNormal.IsNearlyZero()
			&& FMath::IsFinite(HalfThickness) && HalfThickness > 0.0;
	}
	return false;
}

bool FMaterialSpanAlgorithms::Normalize(
	TArray<FMaterialSpan>& InOutSpans,
	FString& OutError)
{
	OutError.Reset();
	for (const FMaterialSpan& Span : InOutSpans)
	{
		if (!Span.IsValid())
		{
			OutError = TEXT("material span must be non-empty and use a non-zero material index");
			return false;
		}
	}
	InOutSpans.Sort([](const FMaterialSpan& Left, const FMaterialSpan& Right)
	{
		if (Left.BeginN != Right.BeginN) return Left.BeginN < Right.BeginN;
		if (Left.EndNExclusive != Right.EndNExclusive)
		{
			return Left.EndNExclusive < Right.EndNExclusive;
		}
		if (Left.MaterialIndex != Right.MaterialIndex)
		{
			return Left.MaterialIndex < Right.MaterialIndex;
		}
		return Left.Flags < Right.Flags;
	});

	TArray<FMaterialSpan> Normalized;
	Normalized.Reserve(InOutSpans.Num());
	for (const FMaterialSpan& Span : InOutSpans)
	{
		if (Normalized.IsEmpty())
		{
			Normalized.Add(Span);
			continue;
		}
		FMaterialSpan& Previous = Normalized.Last();
		if (Span.BeginN < Previous.EndNExclusive)
		{
			OutError = TEXT("material spans overlap");
			return false;
		}
		if (Span.BeginN == Previous.EndNExclusive
			&& Span.MaterialIndex == Previous.MaterialIndex
			&& Span.Flags == Previous.Flags)
		{
			Previous.EndNExclusive = Span.EndNExclusive;
		}
		else
		{
			Normalized.Add(Span);
		}
	}
	InOutSpans = MoveTemp(Normalized);
	return true;
}

TArray<FMaterialSpan> FMaterialSpanAlgorithms::SubtractInterval(
	const TConstArrayView<FMaterialSpan> Spans,
	const int32 BeginN,
	const int32 EndNExclusive)
{
	TArray<FMaterialSpan> Result;
	if (BeginN >= EndNExclusive)
	{
		Result.Append(Spans);
		return Result;
	}
	Result.Reserve(Spans.Num() + 1);
	for (const FMaterialSpan& Span : Spans)
	{
		if (Span.EndNExclusive <= BeginN || Span.BeginN >= EndNExclusive)
		{
			Result.Add(Span);
			continue;
		}
		if (Span.BeginN < BeginN)
		{
			Result.Emplace(
				Span.BeginN,
				FMath::Min(BeginN, Span.EndNExclusive),
				Span.MaterialIndex,
				Span.Flags);
		}
		if (Span.EndNExclusive > EndNExclusive)
		{
			Result.Emplace(
				FMath::Max(EndNExclusive, Span.BeginN),
				Span.EndNExclusive,
				Span.MaterialIndex,
				Span.Flags);
		}
	}
	return Result;
}

bool FMaterialVolumeChunkSnapshot::TryGetColumnSpans(
	const FIntPoint& LocalColumn,
	TArray<FMaterialSpan>& OutSpans) const
{
	OutSpans.Reset();
	if (LocalColumn.X < 0 || LocalColumn.Y < 0
		|| LocalColumn.X >= ChunkSize || LocalColumn.Y >= ChunkSize)
	{
		return false;
	}
	const int32 ColumnIndex = LocalColumn.Y * ChunkSize + LocalColumn.X;
	if (Adapter == EMaterialVolumeStorageAdapter::Span)
	{
		if (!Columns.IsValidIndex(ColumnIndex)) return false;
		const FMaterialVolumeColumnHeader& Header = Columns[ColumnIndex];
		if (Header.SpanOffset < 0 || Header.SpanCount < 0
			|| Header.SpanOffset + Header.SpanCount > SpanPool.Num())
		{
			return false;
		}
		OutSpans.Append(SpanPool.GetData() + Header.SpanOffset, Header.SpanCount);
		return true;
	}

	const int32 Depth = DenseEndNExclusive - DenseBeginN;
	if (Depth < 0 || DenseCells.Num() != ChunkSize * ChunkSize * Depth)
	{
		return false;
	}
	FMaterialSpan Active;
	for (int32 DepthIndex = 0; DepthIndex < Depth; ++DepthIndex)
	{
		const FMaterialDenseCell& Cell = DenseCells[ColumnIndex * Depth + DepthIndex];
		const int32 N = DenseBeginN + DepthIndex;
		if (!Cell.IsOccupied())
		{
			if (Active.IsValid())
			{
				OutSpans.Add(Active);
				Active = FMaterialSpan();
			}
			continue;
		}
		if (Active.IsValid()
			&& Active.MaterialIndex == Cell.MaterialIndex
			&& Active.Flags == Cell.Flags)
		{
			Active.EndNExclusive = N + 1;
		}
		else
		{
			if (Active.IsValid()) OutSpans.Add(Active);
			Active = FMaterialSpan(N, N + 1, Cell.MaterialIndex, Cell.Flags);
		}
	}
	if (Active.IsValid()) OutSpans.Add(Active);
	return true;
}

bool FMaterialVolumeChunkSnapshot::IsValid(FString* OutError) const
{
	FString LocalError;
	auto Fail = [&](const TCHAR* Message)
	{
		if (OutError) *OutError = Message;
		return false;
	};
	if (ChunkSize <= 0 || ChunkSize > 64) return Fail(TEXT("invalid chunk size"));
	if (Adapter == EMaterialVolumeStorageAdapter::Span
		&& Columns.Num() != ChunkSize * ChunkSize)
	{
		return Fail(TEXT("span chunk header count does not match dimensions"));
	}
	for (int32 Y = 0; Y < ChunkSize; ++Y)
	{
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			TArray<FMaterialSpan> Spans;
			if (!TryGetColumnSpans(FIntPoint(X, Y), Spans)
				|| !FMaterialSpanAlgorithms::Normalize(Spans, LocalError))
			{
				return Fail(TEXT("chunk contains an invalid material column"));
			}
		}
	}
	if (OutError) OutError->Reset();
	return true;
}

FMaterialVolumeChunkBuilder::FMaterialVolumeChunkBuilder(
	const FIntPoint InChunkCoord,
	const int32 InChunkSize)
	: ChunkCoord(InChunkCoord)
	, ChunkSize(InChunkSize)
{
	if (ChunkSize > 0 && ChunkSize <= 64)
	{
		PendingColumns.SetNum(ChunkSize * ChunkSize);
	}
}

FMaterialVolumeChunkBuilder::FMaterialVolumeChunkBuilder(
	const FMaterialVolumeChunkSnapshot& Source)
	: FMaterialVolumeChunkBuilder(Source.ChunkCoord, Source.ChunkSize)
{
	for (int32 Y = 0; Y < ChunkSize; ++Y)
	{
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			Source.TryGetColumnSpans(
				FIntPoint(X, Y),
				PendingColumns[Y * ChunkSize + X]);
		}
	}
}

bool FMaterialVolumeChunkBuilder::SetColumnSpans(
	const FIntPoint& LocalColumn,
	const TConstArrayView<FMaterialSpan> Spans)
{
	if (LocalColumn.X < 0 || LocalColumn.Y < 0
		|| LocalColumn.X >= ChunkSize || LocalColumn.Y >= ChunkSize)
	{
		return false;
	}
	TArray<FMaterialSpan>& Target =
		PendingColumns[LocalColumn.Y * ChunkSize + LocalColumn.X];
	Target.Reset(Spans.Num());
	Target.Append(Spans);
	return true;
}

bool FMaterialVolumeChunkBuilder::Build(
	const EMaterialVolumeStorageAdapter InAdapter,
	FMaterialVolumeChunkSnapshot& OutSnapshot,
	FString& OutError) const
{
	OutError.Reset();
	if (ChunkSize <= 0 || ChunkSize > 64
		|| PendingColumns.Num() != ChunkSize * ChunkSize)
	{
		OutError = TEXT("chunk builder dimensions are invalid");
		return false;
	}
	TArray<TArray<FMaterialSpan>> Normalized = PendingColumns;
	for (TArray<FMaterialSpan>& Column : Normalized)
	{
		if (!FMaterialSpanAlgorithms::Normalize(Column, OutError))
		{
			return false;
		}
	}

	FMaterialVolumeChunkSnapshot Candidate;
	Candidate.ChunkCoord = ChunkCoord;
	Candidate.ChunkSize = ChunkSize;
	Candidate.Adapter = InAdapter;
	if (InAdapter == EMaterialVolumeStorageAdapter::Span)
	{
		Candidate.Columns.SetNum(ChunkSize * ChunkSize);
		for (int32 ColumnIndex = 0; ColumnIndex < Normalized.Num(); ++ColumnIndex)
		{
			FMaterialVolumeColumnHeader& Header = Candidate.Columns[ColumnIndex];
			Header.SpanOffset = Candidate.SpanPool.Num();
			Header.SpanCount = Normalized[ColumnIndex].Num();
			Candidate.SpanPool.Append(Normalized[ColumnIndex]);
		}
	}
	else
	{
		bool bHasCells = false;
		int32 MinimumN = MAX_int32;
		int32 MaximumN = MIN_int32;
		for (const TArray<FMaterialSpan>& Column : Normalized)
		{
			for (const FMaterialSpan& Span : Column)
			{
				bHasCells = true;
				MinimumN = FMath::Min(MinimumN, Span.BeginN);
				MaximumN = FMath::Max(MaximumN, Span.EndNExclusive);
			}
		}
		Candidate.DenseBeginN = bHasCells ? MinimumN : 0;
		Candidate.DenseEndNExclusive = bHasCells ? MaximumN : 0;
		const int32 Depth = Candidate.DenseEndNExclusive - Candidate.DenseBeginN;
		Candidate.DenseCells.SetNumZeroed(ChunkSize * ChunkSize * Depth);
		for (int32 ColumnIndex = 0; ColumnIndex < Normalized.Num(); ++ColumnIndex)
		{
			for (const FMaterialSpan& Span : Normalized[ColumnIndex])
			{
				for (int32 N = Span.BeginN; N < Span.EndNExclusive; ++N)
				{
					FMaterialDenseCell& Cell = Candidate.DenseCells[
						ColumnIndex * Depth + N - Candidate.DenseBeginN];
					Cell.MaterialIndex = Span.MaterialIndex;
					Cell.Flags = Span.Flags;
				}
			}
		}
	}
	if (!Candidate.IsValid(&OutError)) return false;
	OutSnapshot = MoveTemp(Candidate);
	return true;
}

bool FMaterialGridFrame::IsValid() const
{
	return FMath::IsFinite(CellSize) && CellSize > 0.0
		&& !BasisU.IsNearlyZero() && !BasisV.IsNearlyZero() && !BasisN.IsNearlyZero()
		&& FMath::IsNearlyZero(FVector3d::DotProduct(BasisU, BasisV), 1.e-6)
		&& FMath::IsNearlyZero(FVector3d::DotProduct(BasisU, BasisN), 1.e-6)
		&& FMath::IsNearlyZero(FVector3d::DotProduct(BasisV, BasisN), 1.e-6);
}

FStructuralSeam::FStructuralSeam(
	const FIntVector& InCellA,
	const FIntVector& InCellB)
	: CellA(InCellA)
	, CellB(InCellB)
{
	if (MatterFluxMaterialVolumePrivate::CellLess(CellB, CellA))
	{
		Swap(CellA, CellB);
	}
}

bool FStructuralSeam::IsValid() const
{
	return FMath::Abs(CellA.X - CellB.X)
		+ FMath::Abs(CellA.Y - CellB.Y)
		+ FMath::Abs(CellA.Z - CellB.Z) == 1;
}

uint32 GetTypeHash(const FStructuralSeam& Seam)
{
	return HashCombine(GetTypeHash(Seam.CellA), GetTypeHash(Seam.CellB));
}

bool FMaterialVolumeTopology::IsValid(FString* OutError) const
{
	if (TopologyRevision < 0 || !GridFrame.IsValid())
	{
		if (OutError) *OutError = TEXT("volume topology metadata is invalid");
		return false;
	}
	for (const TPair<FIntPoint, FMaterialVolumeChunkSnapshot>& Pair : Chunks)
	{
		if (Pair.Key != Pair.Value.ChunkCoord || !Pair.Value.IsValid(OutError))
		{
			if (OutError && OutError->IsEmpty())
			{
				*OutError = TEXT("volume chunk key does not match snapshot coordinate");
			}
			return false;
		}
	}
	for (const FStructuralSeam& Seam : StructuralSeams)
	{
		if (!Seam.IsValid())
		{
			if (OutError) *OutError = TEXT("volume contains a non-adjacent structural seam");
			return false;
		}
	}
	if (OutError) OutError->Reset();
	return true;
}

uint64 FMaterialVolumeAlgorithms::ComputeChunkLogicalHash(
	const FMaterialVolumeChunkSnapshot& Chunk)
{
	using namespace MatterFluxMaterialVolumePrivate;
	uint64 Hash = FnvOffset;
	HashInteger(Hash, Chunk.ChunkCoord.X);
	HashInteger(Hash, Chunk.ChunkCoord.Y);
	HashInteger(Hash, Chunk.ChunkSize);
	for (int32 Y = 0; Y < Chunk.ChunkSize; ++Y)
	{
		for (int32 X = 0; X < Chunk.ChunkSize; ++X)
		{
			TArray<FMaterialSpan> Spans;
			if (!Chunk.TryGetColumnSpans(FIntPoint(X, Y), Spans))
			{
				HashByte(Hash, 0xff);
				continue;
			}
			HashInteger(Hash, Spans.Num());
			for (const FMaterialSpan& Span : Spans)
			{
				HashInteger(Hash, Span.BeginN);
				HashInteger(Hash, Span.EndNExclusive);
				HashInteger(Hash, Span.MaterialIndex);
				HashInteger(Hash, Span.Flags);
			}
		}
	}
	return Hash;
}

uint64 FMaterialVolumeAlgorithms::ComputeLogicalHash(
	const FMaterialVolumeTopology& Topology)
{
	using namespace MatterFluxMaterialVolumePrivate;
	uint64 Hash = FnvOffset;
	HashName(Hash, Topology.DefinitionId);
	HashDouble(Hash, Topology.GridFrame.Origin.X);
	HashDouble(Hash, Topology.GridFrame.Origin.Y);
	HashDouble(Hash, Topology.GridFrame.Origin.Z);
	for (const FVector3d Axis : {
		Topology.GridFrame.BasisU,
		Topology.GridFrame.BasisV,
		Topology.GridFrame.BasisN })
	{
		HashDouble(Hash, Axis.X);
		HashDouble(Hash, Axis.Y);
		HashDouble(Hash, Axis.Z);
	}
	HashDouble(Hash, Topology.GridFrame.CellSize);

	TArray<FIntPoint> ChunkCoords;
	Topology.Chunks.GetKeys(ChunkCoords);
	ChunkCoords.Sort(PointLess);
	HashInteger(Hash, ChunkCoords.Num());
	for (const FIntPoint Coord : ChunkCoords)
	{
		HashInteger(Hash, ComputeChunkLogicalHash(Topology.Chunks.FindChecked(Coord)));
	}

	TArray<FIntVector> Anchors = Topology.StructuralAnchors.Array();
	Anchors.Sort(CellLess);
	HashInteger(Hash, Anchors.Num());
	for (const FIntVector Anchor : Anchors)
	{
		HashInteger(Hash, Anchor.X);
		HashInteger(Hash, Anchor.Y);
		HashInteger(Hash, Anchor.Z);
	}
	TArray<FStructuralSeam> Seams = Topology.StructuralSeams.Array();
	Seams.Sort([](const FStructuralSeam& Left, const FStructuralSeam& Right)
	{
		if (Left.CellA != Right.CellA) return CellLess(Left.CellA, Right.CellA);
		return CellLess(Left.CellB, Right.CellB);
	});
	HashInteger(Hash, Seams.Num());
	for (const FStructuralSeam& Seam : Seams)
	{
		for (const FIntVector Cell : { Seam.CellA, Seam.CellB })
		{
			HashInteger(Hash, Cell.X);
			HashInteger(Hash, Cell.Y);
			HashInteger(Hash, Cell.Z);
		}
	}
	return Hash;
}

bool FMaterialVolumeAlgorithms::TryGetColumnSpans(
	const FMaterialVolumeTopology& Topology,
	const FIntPoint& GlobalColumn,
	TArray<FMaterialSpan>& OutSpans)
{
	OutSpans.Reset();
	for (const TPair<FIntPoint, FMaterialVolumeChunkSnapshot>& Pair : Topology.Chunks)
	{
		const FMaterialVolumeChunkSnapshot& Chunk = Pair.Value;
		const FIntPoint Local(
			GlobalColumn.X - Chunk.ChunkCoord.X * Chunk.ChunkSize,
			GlobalColumn.Y - Chunk.ChunkCoord.Y * Chunk.ChunkSize);
		if (Local.X >= 0 && Local.Y >= 0
			&& Local.X < Chunk.ChunkSize && Local.Y < Chunk.ChunkSize)
		{
			return Chunk.TryGetColumnSpans(Local, OutSpans);
		}
	}
	return false;
}

bool FMaterialVolumeAlgorithms::TryGetSingleSpan(
	const FMaterialVolumeTopology& Topology,
	const FIntPoint& GlobalColumn,
	FMaterialSpan& OutSpan)
{
	TArray<FMaterialSpan> Spans;
	if (!TryGetColumnSpans(Topology, GlobalColumn, Spans) || Spans.Num() != 1)
	{
		return false;
	}
	OutSpan = Spans[0];
	return true;
}

bool FMaterialVolumeAlgorithms::TryGetCellMaterial(
	const FMaterialVolumeTopology& Topology,
	const FIntVector& Cell,
	uint16& OutMaterialIndex)
{
	OutMaterialIndex = 0;
	TArray<FMaterialSpan> Spans;
	if (!TryGetColumnSpans(Topology, FIntPoint(Cell.X, Cell.Y), Spans))
	{
		return false;
	}
	for (const FMaterialSpan& Span : Spans)
	{
		if (Cell.Z >= Span.BeginN && Cell.Z < Span.EndNExclusive)
		{
			OutMaterialIndex = Span.MaterialIndex;
			return true;
		}
	}
	return false;
}

bool FMaterialVolumeAlgorithms::SetCellMaterial(
	const FMaterialVolumeTopology& Source,
	const FIntVector& Cell,
	const uint16 MaterialIndex,
	FMaterialVolumeTopology& OutTopology,
	FString& OutError)
{
	OutError.Reset();
	if (MaterialIndex == 0 || !Source.IsValid(&OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Volume cell material must be non-empty");
		}
		return false;
	}
	constexpr int32 ChunkSize = 16;
	const FIntPoint ChunkCoord(
		MatterFluxMaterialVolumePrivate::FloorDivide(Cell.X, ChunkSize),
		MatterFluxMaterialVolumePrivate::FloorDivide(Cell.Y, ChunkSize));
	const FMaterialVolumeChunkSnapshot* Chunk = Source.Chunks.Find(ChunkCoord);
	if (!Chunk)
	{
		OutError = TEXT("Volume material edit references an empty cell");
		return false;
	}
	const FIntPoint LocalColumn(
		Cell.X - ChunkCoord.X * Chunk->ChunkSize,
		Cell.Y - ChunkCoord.Y * Chunk->ChunkSize);
	TArray<FMaterialSpan> Spans;
	if (!Chunk->TryGetColumnSpans(LocalColumn, Spans))
	{
		OutError = TEXT("Volume material edit references an empty column");
		return false;
	}
	int32 SpanIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Spans.Num(); ++Index)
	{
		if (Cell.Z >= Spans[Index].BeginN
			&& Cell.Z < Spans[Index].EndNExclusive)
		{
			SpanIndex = Index;
			break;
		}
	}
	if (SpanIndex == INDEX_NONE)
	{
		OutError = TEXT("Volume material edit references an empty cell");
		return false;
	}
	if (Spans[SpanIndex].MaterialIndex == MaterialIndex)
	{
		OutTopology = Source;
		return true;
	}
	const FMaterialSpan Original = Spans[SpanIndex];
	Spans.RemoveAt(SpanIndex);
	if (Original.BeginN < Cell.Z)
	{
		Spans.Add(FMaterialSpan(
			Original.BeginN, Cell.Z, Original.MaterialIndex, Original.Flags));
	}
	Spans.Add(FMaterialSpan(
		Cell.Z, Cell.Z + 1, MaterialIndex, Original.Flags));
	if (Cell.Z + 1 < Original.EndNExclusive)
	{
		Spans.Add(FMaterialSpan(
			Cell.Z + 1,
			Original.EndNExclusive,
			Original.MaterialIndex,
			Original.Flags));
	}
	if (!FMaterialSpanAlgorithms::Normalize(Spans, OutError))
	{
		return false;
	}
	FMaterialVolumeChunkBuilder Builder(*Chunk);
	if (!Builder.SetColumnSpans(LocalColumn, Spans))
	{
		OutError = TEXT("Volume material edit could not stage its column");
		return false;
	}
	FMaterialVolumeChunkSnapshot Rebuilt;
	if (!Builder.Build(Chunk->Adapter, Rebuilt, OutError))
	{
		return false;
	}
	FMaterialVolumeTopology Candidate = Source;
	Candidate.Chunks.Add(ChunkCoord, MoveTemp(Rebuilt));
	++Candidate.TopologyRevision;
	if (!Candidate.IsValid(&OutError))
	{
		return false;
	}
	OutTopology = MoveTemp(Candidate);
	return true;
}

bool FMaterialVolumeAlgorithms::GatherComponents(
	const FMaterialVolumeTopology& Topology,
	TArray<FMaterialVolumeComponent>& OutComponents,
	FString& OutError)
{
	using namespace MatterFluxMaterialVolumePrivate;
	OutComponents.Reset();
	if (!Topology.IsValid(&OutError)) return false;

	TArray<FSpanNode> Nodes;
	for (const TPair<FIntPoint, FMaterialVolumeChunkSnapshot>& Pair : Topology.Chunks)
	{
		const FMaterialVolumeChunkSnapshot& Chunk = Pair.Value;
		for (int32 Y = 0; Y < Chunk.ChunkSize; ++Y)
		{
			for (int32 X = 0; X < Chunk.ChunkSize; ++X)
			{
				TArray<FMaterialSpan> Spans;
				Chunk.TryGetColumnSpans(FIntPoint(X, Y), Spans);
				for (const FMaterialSpan& Span : Spans)
				{
					Nodes.Add({
						FIntPoint(
							Chunk.ChunkCoord.X * Chunk.ChunkSize + X,
							Chunk.ChunkCoord.Y * Chunk.ChunkSize + Y),
						Span });
				}
			}
		}
	}
	Nodes.Sort([](const FSpanNode& Left, const FSpanNode& Right)
	{
		if (Left.Column != Right.Column) return PointLess(Left.Column, Right.Column);
		return Left.Span.BeginN < Right.Span.BeginN;
	});

	TArray<TArray<int32>> Neighbours;
	Neighbours.SetNum(Nodes.Num());
	for (int32 LeftIndex = 0; LeftIndex < Nodes.Num(); ++LeftIndex)
	{
		for (int32 RightIndex = LeftIndex + 1; RightIndex < Nodes.Num(); ++RightIndex)
		{
			if (HasUnseamedConnection(
				Nodes[LeftIndex], Nodes[RightIndex], Topology.StructuralSeams))
			{
				Neighbours[LeftIndex].Add(RightIndex);
				Neighbours[RightIndex].Add(LeftIndex);
			}
		}
	}

	TBitArray<> Visited(false, Nodes.Num());
	for (int32 Root = 0; Root < Nodes.Num(); ++Root)
	{
		if (Visited[Root]) continue;
		FMaterialVolumeComponent Component;
		Component.MinimumCell = FIntVector(
			Nodes[Root].Column.X,
			Nodes[Root].Column.Y,
			Nodes[Root].Span.BeginN);
		TArray<int32> Queue = { Root };
		Visited[Root] = true;
		for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
		{
			const int32 NodeIndex = Queue[QueueIndex];
			const FSpanNode& Node = Nodes[NodeIndex];
			const FIntVector Start(
				Node.Column.X, Node.Column.Y, Node.Span.BeginN);
			Component.SpanStarts.Add(Start);
			Component.CellCount +=
				static_cast<int64>(Node.Span.EndNExclusive) - Node.Span.BeginN;
			if (CellLess(Start, Component.MinimumCell)) Component.MinimumCell = Start;
			for (const int32 Neighbour : Neighbours[NodeIndex])
			{
				if (!Visited[Neighbour])
				{
					Visited[Neighbour] = true;
					Queue.Add(Neighbour);
				}
			}
		}
		Component.SpanStarts.Sort(CellLess);
		OutComponents.Add(MoveTemp(Component));
	}
	OutComponents.Sort([](
		const FMaterialVolumeComponent& Left,
		const FMaterialVolumeComponent& Right)
	{
		return CellLess(Left.MinimumCell, Right.MinimumCell);
	});
	return true;
}

bool FMaterialVolumeAlgorithms::Subtract(
	const FMaterialVolumeTopology& Source,
	const FMaterialVolumeCut& Cut,
	FMaterialVolumeTopology& OutTopology,
	FString& OutError)
{
	using namespace MatterFluxMaterialVolumePrivate;
	OutError.Reset();
	if (!Source.IsValid(&OutError) || !Cut.IsValid())
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("material volume cut is invalid");
		}
		return false;
	}

	FMaterialVolumeTopology Candidate = Source;
	bool bChanged = false;
	TArray<FIntPoint> ChunkCoords;
	Source.Chunks.GetKeys(ChunkCoords);
	ChunkCoords.Sort(PointLess);
	for (const FIntPoint ChunkCoord : ChunkCoords)
	{
		const FMaterialVolumeChunkSnapshot& SourceChunk =
			Source.Chunks.FindChecked(ChunkCoord);
		FMaterialVolumeChunkBuilder Builder(SourceChunk);
		bool bChunkChanged = false;
		bool bChunkOccupied = false;
		for (int32 V = 0; V < SourceChunk.ChunkSize; ++V)
		{
			for (int32 U = 0; U < SourceChunk.ChunkSize; ++U)
			{
				const FIntPoint LocalColumn(U, V);
				TArray<FMaterialSpan> SourceSpans;
				SourceChunk.TryGetColumnSpans(LocalColumn, SourceSpans);
				TArray<FMaterialSpan> ResultSpans;
				for (const FMaterialSpan& Span : SourceSpans)
				{
					FMaterialSpan Active;
					for (int32 N = Span.BeginN; N < Span.EndNExclusive; ++N)
					{
						const FIntVector Cell(
							ChunkCoord.X * SourceChunk.ChunkSize + U,
							ChunkCoord.Y * SourceChunk.ChunkSize + V,
							N);
						if (PointInsideCut(CellCenter(Source.GridFrame, Cell), Cut))
						{
							bChunkChanged = true;
							if (Active.IsValid())
							{
								ResultSpans.Add(Active);
								Active = FMaterialSpan();
							}
							continue;
						}
						bChunkOccupied = true;
						if (Active.IsValid())
						{
							Active.EndNExclusive = N + 1;
						}
						else
						{
							Active = FMaterialSpan(
								N, N + 1, Span.MaterialIndex, Span.Flags);
						}
					}
					if (Active.IsValid())
					{
						ResultSpans.Add(Active);
					}
				}
				if (bChunkChanged)
				{
					Builder.SetColumnSpans(LocalColumn, ResultSpans);
				}
			}
		}
		if (!bChunkChanged)
		{
			continue;
		}
		bChanged = true;
		if (!bChunkOccupied)
		{
			Candidate.Chunks.Remove(ChunkCoord);
			continue;
		}
		FMaterialVolumeChunkSnapshot Rebuilt;
		if (!Builder.Build(SourceChunk.Adapter, Rebuilt, OutError))
		{
			return false;
		}
		Candidate.Chunks.FindChecked(ChunkCoord) = MoveTemp(Rebuilt);
	}

	if (bChanged)
	{
		Candidate.TopologyRevision = Source.TopologyRevision == MAX_int32
			? 0
			: Source.TopologyRevision + 1;
		for (const FIntVector Cell : Candidate.StructuralAnchors.Array())
		{
			if (!IsCellOccupied(Candidate, Cell))
			{
				Candidate.StructuralAnchors.Remove(Cell);
			}
		}
		for (const FStructuralSeam Seam : Candidate.StructuralSeams.Array())
		{
			if (!IsCellOccupied(Candidate, Seam.CellA)
				|| !IsCellOccupied(Candidate, Seam.CellB))
			{
				Candidate.StructuralSeams.Remove(Seam);
			}
		}
	}
	if (!Candidate.IsValid(&OutError))
	{
		return false;
	}
	OutTopology = MoveTemp(Candidate);
	return true;
}

bool FMaterialVolumeAlgorithms::ComputeMassProperties(
	const FMaterialVolumeTopology& Topology,
	const TMap<uint16, double>& DensityByMaterial,
	FMaterialVolumeMassProperties& OutProperties,
	FString& OutError)
{
	using namespace MatterFluxMaterialVolumePrivate;
	OutProperties = FMaterialVolumeMassProperties();
	OutError.Reset();
	if (!Topology.IsValid(&OutError))
	{
		return false;
	}
	const double CellVolume = FMath::Pow(Topology.GridFrame.CellSize, 3.0);
	FVector3d WeightedCenter = FVector3d::ZeroVector;
	struct FMassCell
	{
		FVector3d Center;
		double Mass = 0.0;
	};
	TArray<FMassCell> Cells;
	for (const TPair<FIntPoint, FMaterialVolumeChunkSnapshot>& Pair
		: Topology.Chunks)
	{
		const FMaterialVolumeChunkSnapshot& Chunk = Pair.Value;
		for (int32 V = 0; V < Chunk.ChunkSize; ++V)
		{
			for (int32 U = 0; U < Chunk.ChunkSize; ++U)
			{
				TArray<FMaterialSpan> Spans;
				Chunk.TryGetColumnSpans(FIntPoint(U, V), Spans);
				for (const FMaterialSpan& Span : Spans)
				{
					const double* Density = DensityByMaterial.Find(Span.MaterialIndex);
					if (!Density || !FMath::IsFinite(*Density) || *Density <= 0.0)
					{
						OutError = FString::Printf(
							TEXT("material %u has no positive finite density"),
							Span.MaterialIndex);
						return false;
					}
					const double CellMass = *Density * CellVolume;
					for (int32 N = Span.BeginN; N < Span.EndNExclusive; ++N)
					{
						const FIntVector Coordinate(
							Pair.Key.X * Chunk.ChunkSize + U,
							Pair.Key.Y * Chunk.ChunkSize + V,
							N);
						const FVector3d Center = CellCenter(
							Topology.GridFrame, Coordinate);
						Cells.Add({ Center, CellMass });
						OutProperties.Mass += CellMass;
						WeightedCenter += Center * CellMass;
						++OutProperties.CellCount;
					}
				}
			}
		}
	}
	if (OutProperties.Mass <= 0.0)
	{
		return true;
	}
	OutProperties.CenterOfMass = WeightedCenter / OutProperties.Mass;
	const double Intrinsic = FMath::Square(Topology.GridFrame.CellSize) / 6.0;
	for (const FMassCell& Cell : Cells)
	{
		const FVector3d Delta = Cell.Center - OutProperties.CenterOfMass;
		OutProperties.InertiaDiagonal.X += Cell.Mass
			* (Intrinsic + Delta.Y * Delta.Y + Delta.Z * Delta.Z);
		OutProperties.InertiaDiagonal.Y += Cell.Mass
			* (Intrinsic + Delta.X * Delta.X + Delta.Z * Delta.Z);
		OutProperties.InertiaDiagonal.Z += Cell.Mass
			* (Intrinsic + Delta.X * Delta.X + Delta.Y * Delta.Y);
		OutProperties.InertiaProducts.X -= Cell.Mass * Delta.X * Delta.Y;
		OutProperties.InertiaProducts.Y -= Cell.Mass * Delta.X * Delta.Z;
		OutProperties.InertiaProducts.Z -= Cell.Mass * Delta.Y * Delta.Z;
	}
	return true;
}

FVector FMaterialVolumeAlgorithms::ComputeChildLinearVelocity(
	const FVector& ParentLinearVelocity,
	const FVector& ParentAngularVelocityRadians,
	const FVector& ParentCenterOfMass,
	const FVector& ChildCenterOfMass)
{
	return ParentLinearVelocity + FVector::CrossProduct(
		ParentAngularVelocityRadians,
		ChildCenterOfMass - ParentCenterOfMass);
}

bool FMaterialVolumeAlgorithms::BuildDelta(
	const FGuid& InstanceId,
	const FMaterialVolumeTopology& BaseTopology,
	const FMaterialVolumeFields& BaseFields,
	const FMaterialVolumeTopology& TargetTopology,
	const FMaterialVolumeFields& TargetFields,
	FMaterialVolumeDelta& OutDelta,
	FString& OutError)
{
	OutError.Reset();
	if (!InstanceId.IsValid()
		|| !BaseTopology.IsValid(&OutError)
		|| !TargetTopology.IsValid(&OutError)
		|| !BaseFields.IsValid()
		|| !TargetFields.IsValid()
		|| TargetTopology.TopologyRevision < BaseTopology.TopologyRevision
		|| TargetFields.FieldRevision < BaseFields.FieldRevision)
	{
		if (OutError.IsEmpty()) OutError = TEXT("Volume delta endpoints are invalid");
		return false;
	}
	const uint64 BaseHash = ComputeLogicalHash(BaseTopology);
	const uint64 TargetHash = ComputeLogicalHash(TargetTopology);
	bool bFieldsEqual =
		TargetFields.EnergyOverrides.Num() == BaseFields.EnergyOverrides.Num();
	if (bFieldsEqual)
	{
		for (const TPair<FIntVector, uint16>& Pair
			: TargetFields.EnergyOverrides)
		{
			const uint16* BaseValue =
				BaseFields.EnergyOverrides.Find(Pair.Key);
			if (!BaseValue || *BaseValue != Pair.Value)
			{
				bFieldsEqual = false;
				break;
			}
		}
	}
	if ((TargetTopology.TopologyRevision == BaseTopology.TopologyRevision
			&& TargetHash != BaseHash)
		|| (TargetFields.FieldRevision == BaseFields.FieldRevision
			&& (TargetFields.EnvironmentEnergy != BaseFields.EnvironmentEnergy
				|| !bFieldsEqual)))
	{
		OutError = TEXT("Volume facts changed without advancing their revision");
		return false;
	}
	FMaterialVolumeDelta Candidate;
	Candidate.InstanceId = InstanceId;
	Candidate.BaseTopologyRevision = BaseTopology.TopologyRevision;
	Candidate.TargetTopologyRevision = TargetTopology.TopologyRevision;
	Candidate.BaseFieldRevision = BaseFields.FieldRevision;
	Candidate.TargetFieldRevision = TargetFields.FieldRevision;
	Candidate.TargetDefinitionId = TargetTopology.DefinitionId;
	Candidate.TargetGridFrame = TargetTopology.GridFrame;
	Candidate.TargetStructuralAnchors = TargetTopology.StructuralAnchors;
	Candidate.TargetStructuralSeams = TargetTopology.StructuralSeams;
	Candidate.TargetEnvironmentEnergy = TargetFields.EnvironmentEnergy;

	TSet<FIntPoint> AllChunkCoords;
	for (const TPair<FIntPoint, FMaterialVolumeChunkSnapshot>& Pair
		: BaseTopology.Chunks) AllChunkCoords.Add(Pair.Key);
	for (const TPair<FIntPoint, FMaterialVolumeChunkSnapshot>& Pair
		: TargetTopology.Chunks) AllChunkCoords.Add(Pair.Key);
	TArray<FIntPoint> OrderedChunkCoords = AllChunkCoords.Array();
	OrderedChunkCoords.Sort([](const FIntPoint& A, const FIntPoint& B)
	{
		return A.X != B.X ? A.X < B.X : A.Y < B.Y;
	});
	for (const FIntPoint Coord : OrderedChunkCoords)
	{
		const FMaterialVolumeChunkSnapshot* Base = BaseTopology.Chunks.Find(Coord);
		const FMaterialVolumeChunkSnapshot* Target = TargetTopology.Chunks.Find(Coord);
		if (Base && Target
			&& ComputeChunkLogicalHash(*Base) == ComputeChunkLogicalHash(*Target))
		{
			continue;
		}
		FMaterialVolumeChunkChange& Change =
			Candidate.ChunkChanges.AddDefaulted_GetRef();
		Change.ChunkCoord = Coord;
		Change.bRemoved = Target == nullptr;
		if (Target) Change.Snapshot = *Target;
	}

	TSet<FIntVector> AllFieldCells;
	for (const TPair<FIntVector, uint16>& Pair : BaseFields.EnergyOverrides)
		AllFieldCells.Add(Pair.Key);
	for (const TPair<FIntVector, uint16>& Pair : TargetFields.EnergyOverrides)
		AllFieldCells.Add(Pair.Key);
	TArray<FIntVector> OrderedFieldCells = AllFieldCells.Array();
	OrderedFieldCells.Sort([](const FIntVector& A, const FIntVector& B)
	{
		if (A.X != B.X) return A.X < B.X;
		if (A.Y != B.Y) return A.Y < B.Y;
		return A.Z < B.Z;
	});
	for (const FIntVector Cell : OrderedFieldCells)
	{
		const uint16* Base = BaseFields.EnergyOverrides.Find(Cell);
		const uint16* Target = TargetFields.EnergyOverrides.Find(Cell);
		if (Base && Target && *Base == *Target) continue;
		FMaterialVolumeFieldChange& Change =
			Candidate.FieldChanges.AddDefaulted_GetRef();
		Change.Cell = Cell;
		Change.bRemoved = Target == nullptr;
		Change.Energy = Target ? *Target : 0;
	}
	Candidate.ResultTopologyHash = TargetHash;
	OutDelta = MoveTemp(Candidate);
	return true;
}

EMaterialVolumeDeltaApplyResult FMaterialVolumeAlgorithms::ApplyDelta(
	const FMaterialVolumeDelta& Delta,
	FMaterialVolumeTopology& InOutTopology,
	FMaterialVolumeFields& InOutFields,
	FString& OutError)
{
	OutError.Reset();
	if (!Delta.InstanceId.IsValid())
	{
		OutError = TEXT("Volume delta has no stable instance id");
		return EMaterialVolumeDeltaApplyResult::Invalid;
	}
	if (InOutTopology.TopologyRevision != Delta.BaseTopologyRevision
		|| InOutFields.FieldRevision != Delta.BaseFieldRevision)
	{
		OutError = TEXT("Volume delta base revision does not match; snapshot required");
		return EMaterialVolumeDeltaApplyResult::SnapshotRequired;
	}
	if (Delta.TargetTopologyRevision < Delta.BaseTopologyRevision
		|| Delta.TargetFieldRevision < Delta.BaseFieldRevision
		|| !Delta.TargetGridFrame.IsValid())
	{
		OutError = TEXT("Volume delta target revisions or frame are invalid");
		return EMaterialVolumeDeltaApplyResult::Invalid;
	}
	FMaterialVolumeTopology CandidateTopology = InOutTopology;
	FMaterialVolumeFields CandidateFields = InOutFields;
	CandidateTopology.DefinitionId = Delta.TargetDefinitionId;
	CandidateTopology.GridFrame = Delta.TargetGridFrame;
	CandidateTopology.StructuralAnchors = Delta.TargetStructuralAnchors;
	CandidateTopology.StructuralSeams = Delta.TargetStructuralSeams;
	TSet<FIntPoint> SeenChunks;
	for (const FMaterialVolumeChunkChange& Change : Delta.ChunkChanges)
	{
		if (SeenChunks.Contains(Change.ChunkCoord)
			|| (!Change.bRemoved
				&& (Change.Snapshot.ChunkCoord != Change.ChunkCoord
					|| !Change.Snapshot.IsValid(&OutError))))
		{
			if (OutError.IsEmpty()) OutError = TEXT("Volume delta has invalid chunk changes");
			return EMaterialVolumeDeltaApplyResult::Invalid;
		}
		SeenChunks.Add(Change.ChunkCoord);
		if (Change.bRemoved) CandidateTopology.Chunks.Remove(Change.ChunkCoord);
		else CandidateTopology.Chunks.Add(Change.ChunkCoord, Change.Snapshot);
	}
	CandidateTopology.TopologyRevision = Delta.TargetTopologyRevision;
	CandidateFields.EnvironmentEnergy = Delta.TargetEnvironmentEnergy;
	TSet<FIntVector> SeenFields;
	for (const FMaterialVolumeFieldChange& Change : Delta.FieldChanges)
	{
		if (SeenFields.Contains(Change.Cell))
		{
			OutError = TEXT("Volume delta has duplicate field changes");
			return EMaterialVolumeDeltaApplyResult::Invalid;
		}
		SeenFields.Add(Change.Cell);
		if (Change.bRemoved) CandidateFields.EnergyOverrides.Remove(Change.Cell);
		else if (Change.Energy == CandidateFields.EnvironmentEnergy)
			CandidateFields.EnergyOverrides.Remove(Change.Cell);
		else CandidateFields.EnergyOverrides.Add(Change.Cell, Change.Energy);
	}
	CandidateFields.FieldRevision = Delta.TargetFieldRevision;
	if (!CandidateTopology.IsValid(&OutError)
		|| !CandidateFields.IsValid()
		|| ComputeLogicalHash(CandidateTopology) != Delta.ResultTopologyHash)
	{
		if (OutError.IsEmpty()) OutError = TEXT("Volume delta result hash does not match");
		return EMaterialVolumeDeltaApplyResult::Invalid;
	}
	InOutTopology = MoveTemp(CandidateTopology);
	InOutFields = MoveTemp(CandidateFields);
	return EMaterialVolumeDeltaApplyResult::Applied;
}

uint32 GetTypeHash(const FMaterialSurfaceKey& Key)
{
	uint32 Hash = GetTypeHash(Key.WorldColumn);
	Hash = HashCombineFast(Hash, GetTypeHash(Key.SurfaceN));
	return HashCombineFast(Hash, GetTypeHash(static_cast<uint8>(Key.Face)));
}

bool FMaterialTerrainSpanOverlay::ResolveColumn(
	const FIntPoint& WorldColumn,
	const TConstArrayView<FMaterialSpan> BaselineSpans,
	TArray<FMaterialSpan>& OutSpans,
	FString& OutError) const
{
	OutError.Reset();
	OutSpans = ColumnOverrides.Contains(WorldColumn)
		? ColumnOverrides.FindChecked(WorldColumn)
		: TArray<FMaterialSpan>(BaselineSpans);
	return FMaterialSpanAlgorithms::Normalize(OutSpans, OutError);
}

bool FMaterialTerrainSpanOverlay::CommitColumn(
	const FIntPoint& WorldColumn,
	const TConstArrayView<FMaterialSpan> BaselineSpans,
	const TConstArrayView<FMaterialSpan> NewSpans,
	FString& OutError)
{
	OutError.Reset();
	TArray<FMaterialSpan> NormalizedBaseline(BaselineSpans);
	TArray<FMaterialSpan> NormalizedNew(NewSpans);
	if (!FMaterialSpanAlgorithms::Normalize(NormalizedBaseline, OutError)
		|| !FMaterialSpanAlgorithms::Normalize(NormalizedNew, OutError))
	{
		return false;
	}
	const TArray<FMaterialSpan>* Existing = ColumnOverrides.Find(WorldColumn);
	const bool bRestoresBaseline = NormalizedNew == NormalizedBaseline;
	if (bRestoresBaseline)
	{
		if (Existing)
		{
			ColumnOverrides.Remove(WorldColumn);
			++Revision;
		}
		return true;
	}
	if (Existing && *Existing == NormalizedNew)
	{
		return true;
	}
	ColumnOverrides.Add(WorldColumn, MoveTemp(NormalizedNew));
	++Revision;
	return true;
}

bool FMaterialTerrainSpanOverlay::TryGetHighestSurface(
	const FIntPoint& WorldColumn,
	const TConstArrayView<FMaterialSpan> BaselineSpans,
	FMaterialSurfaceKey& OutSurface,
	uint16& OutMaterialIndex,
	FString& OutError) const
{
	TArray<FMaterialSpan> Spans;
	if (!ResolveColumn(WorldColumn, BaselineSpans, Spans, OutError)
		|| Spans.IsEmpty())
	{
		return false;
	}
	const FMaterialSpan& Highest = Spans.Last();
	OutSurface = {
		WorldColumn,
		Highest.EndNExclusive,
		EMaterialSurfaceFace::PositiveN };
	OutMaterialIndex = Highest.MaterialIndex;
	return true;
}

bool FMaterialTerrainSpanOverlay::TryGetSolidAtHeight(
	const FIntPoint& WorldColumn,
	const int32 N,
	const TConstArrayView<FMaterialSpan> BaselineSpans,
	uint16& OutMaterialIndex,
	FString& OutError) const
{
	TArray<FMaterialSpan> Spans;
	if (!ResolveColumn(WorldColumn, BaselineSpans, Spans, OutError))
	{
		return false;
	}
	for (const FMaterialSpan& Span : Spans)
	{
		if (N >= Span.BeginN && N < Span.EndNExclusive)
		{
			OutMaterialIndex = Span.MaterialIndex;
			return true;
		}
	}
	return false;
}

bool FMaterialTerrainSurfaceAlgorithms::GatherExposedFaces(
	const FIntPoint& WorldColumn,
	const TConstArrayView<FMaterialSpan> Center,
	const TConstArrayView<FMaterialSpan> NegativeU,
	const TConstArrayView<FMaterialSpan> PositiveU,
	const TConstArrayView<FMaterialSpan> NegativeV,
	const TConstArrayView<FMaterialSpan> PositiveV,
	TArray<FMaterialSurfaceKey>& OutFaces,
	FString& OutError)
{
	OutFaces.Reset();
	OutError.Reset();
	TArray<FMaterialSpan> Columns[5] = {
		TArray<FMaterialSpan>(Center),
		TArray<FMaterialSpan>(NegativeU),
		TArray<FMaterialSpan>(PositiveU),
		TArray<FMaterialSpan>(NegativeV),
		TArray<FMaterialSpan>(PositiveV) };
	for (TArray<FMaterialSpan>& Column : Columns)
	{
		if (!FMaterialSpanAlgorithms::Normalize(Column, OutError))
		{
			OutFaces.Reset();
			return false;
		}
	}
	const auto IsOccupied = [](const TArray<FMaterialSpan>& Spans, const int32 N)
	{
		for (const FMaterialSpan& Span : Spans)
		{
			if (N < Span.BeginN) return false;
			if (N < Span.EndNExclusive) return true;
		}
		return false;
	};
	for (const FMaterialSpan& Span : Columns[0])
	{
		OutFaces.Add({ WorldColumn, Span.BeginN,
			EMaterialSurfaceFace::NegativeN });
		OutFaces.Add({ WorldColumn, Span.EndNExclusive,
			EMaterialSurfaceFace::PositiveN });
		for (int32 N = Span.BeginN; N < Span.EndNExclusive; ++N)
		{
			if (!IsOccupied(Columns[1], N))
				OutFaces.Add({ WorldColumn, N, EMaterialSurfaceFace::NegativeU });
			if (!IsOccupied(Columns[2], N))
				OutFaces.Add({ WorldColumn, N, EMaterialSurfaceFace::PositiveU });
			if (!IsOccupied(Columns[3], N))
				OutFaces.Add({ WorldColumn, N, EMaterialSurfaceFace::NegativeV });
			if (!IsOccupied(Columns[4], N))
				OutFaces.Add({ WorldColumn, N, EMaterialSurfaceFace::PositiveV });
		}
	}
	OutFaces.Sort([](const FMaterialSurfaceKey& A, const FMaterialSurfaceKey& B)
	{
		if (A.SurfaceN != B.SurfaceN) return A.SurfaceN < B.SurfaceN;
		return static_cast<uint8>(A.Face) < static_cast<uint8>(B.Face);
	});
	return true;
}

bool FMaterialTerrainSupportAlgorithms::GatherSupportedCellsInDirtyRegion(
	const TMap<FIntPoint, TArray<FMaterialSpan>>& RegionColumns,
	const FIntRect& DirtyBounds,
	const int32 WorldBottomN,
	const int32 MaximumVisitedCells,
	TSet<FIntVector>& OutSupportedCells,
	FString& OutError)
{
	OutSupportedCells.Reset();
	OutError.Reset();
	if (DirtyBounds.Min.X >= DirtyBounds.Max.X
		|| DirtyBounds.Min.Y >= DirtyBounds.Max.Y
		|| MaximumVisitedCells <= 0)
	{
		OutError = TEXT("Terrain support dirty region or visit budget is invalid");
		return false;
	}
	auto IsInsideDirty = [&DirtyBounds](const FIntPoint Column)
	{
		return Column.X >= DirtyBounds.Min.X
			&& Column.X < DirtyBounds.Max.X
			&& Column.Y >= DirtyBounds.Min.Y
			&& Column.Y < DirtyBounds.Max.Y;
	};
	auto IsOccupied = [&RegionColumns](const FIntVector Cell)
	{
		const TArray<FMaterialSpan>* Spans = RegionColumns.Find(
			FIntPoint(Cell.X, Cell.Y));
		return Spans && Spans->ContainsByPredicate(
			[Cell](const FMaterialSpan& Span)
			{
				return Cell.Z >= Span.BeginN
					&& Cell.Z < Span.EndNExclusive;
			});
	};

	TArray<FIntVector> OccupiedCells;
	for (const TPair<FIntPoint, TArray<FMaterialSpan>>& Pair : RegionColumns)
	{
		if (!IsInsideDirty(Pair.Key))
		{
			continue;
		}
		TArray<FMaterialSpan> Normalized = Pair.Value;
		if (!FMaterialSpanAlgorithms::Normalize(Normalized, OutError))
		{
			return false;
		}
		for (const FMaterialSpan& Span : Normalized)
		{
			const int64 SpanLength = static_cast<int64>(Span.EndNExclusive)
				- Span.BeginN;
			if (SpanLength > MaximumVisitedCells
				|| OccupiedCells.Num() + SpanLength > MaximumVisitedCells)
			{
				OutError = TEXT("Terrain support dirty region exceeds visit budget");
				OutSupportedCells.Reset();
				return false;
			}
			for (int32 N = Span.BeginN; N < Span.EndNExclusive; ++N)
			{
				OccupiedCells.Add(FIntVector(Pair.Key.X, Pair.Key.Y, N));
			}
		}
	}
	OccupiedCells.Sort([](const FIntVector& A, const FIntVector& B)
	{
		return A.X != B.X ? A.X < B.X
			: A.Y != B.Y ? A.Y < B.Y
			: A.Z < B.Z;
	});

	static const FIntVector Neighbours[] =
	{
		FIntVector(-1, 0, 0), FIntVector(1, 0, 0),
		FIntVector(0, -1, 0), FIntVector(0, 1, 0),
		FIntVector(0, 0, -1), FIntVector(0, 0, 1)
	};
	TArray<FIntVector> Queue;
	for (const FIntVector Cell : OccupiedCells)
	{
		bool bSeed = Cell.Z == WorldBottomN;
		if (!bSeed)
		{
			for (const FIntVector Offset : Neighbours)
			{
				const FIntVector Neighbour = Cell + Offset;
				if (!IsInsideDirty(FIntPoint(Neighbour.X, Neighbour.Y))
					&& IsOccupied(Neighbour))
				{
					bSeed = true;
					break;
				}
			}
		}
		if (bSeed && !OutSupportedCells.Contains(Cell))
		{
			OutSupportedCells.Add(Cell);
			Queue.Add(Cell);
		}
	}
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const FIntVector Cell = Queue[Head];
		for (const FIntVector Offset : Neighbours)
		{
			const FIntVector Neighbour = Cell + Offset;
			if (!IsInsideDirty(FIntPoint(Neighbour.X, Neighbour.Y))
				|| OutSupportedCells.Contains(Neighbour)
				|| !IsOccupied(Neighbour))
			{
				continue;
			}
			OutSupportedCells.Add(Neighbour);
			Queue.Add(Neighbour);
		}
	}
	return true;
}

bool FMaterialVolumeConverters::FromLegacyMaskXZY(
	const FFragmentSourceMask& Mask,
	const int32 BeginY,
	const int32 EndYExclusive,
	FMaterialVolumeTopology& OutTopology,
	FString& OutError,
	const uint16 MaterialIndex)
{
	OutError.Reset();
	if (!Mask.HasValidLayout() || BeginY >= EndYExclusive || MaterialIndex == 0)
	{
		OutError = TEXT("legacy mask or extrusion interval is invalid");
		return false;
	}
	FMaterialVolumeTopology Candidate;
	Candidate.DefinitionId = TEXT("legacy.mask.xzy");
	Candidate.TopologyRevision = 1;
	Candidate.GridFrame.CellSize = Mask.CellSize;
	Candidate.GridFrame.BasisU = FVector3d::ForwardVector;
	Candidate.GridFrame.BasisV = FVector3d::UpVector;
	Candidate.GridFrame.BasisN = FVector3d::RightVector;
	// Legacy mask vertices are centered around local X/Z and their one-cell Y
	// extrusion is centered on local Y=0. Keep integer topology coordinates while
	// encoding that centering once in the definition-local frame origin.
	Candidate.GridFrame.Origin =
		Candidate.GridFrame.BasisU
			* (-static_cast<double>(Mask.Width) * Mask.CellSize * 0.5)
		+ Candidate.GridFrame.BasisV
			* (-static_cast<double>(Mask.Height) * Mask.CellSize * 0.5)
		+ Candidate.GridFrame.BasisN
			* (-static_cast<double>(BeginY + EndYExclusive)
				* Mask.CellSize * 0.5);

	TMap<FIntPoint, FMaterialVolumeChunkBuilder> Builders;
	constexpr int32 ChunkSize = 16;
	for (int32 Z = 0; Z < Mask.Height; ++Z)
	{
		for (int32 X = 0; X < Mask.Width; ++X)
		{
			if (Mask.SolidMask[Z * Mask.Width + X] == 0) continue;
			const FIntPoint ChunkCoord(
				MatterFluxMaterialVolumePrivate::FloorDivide(X, ChunkSize),
				MatterFluxMaterialVolumePrivate::FloorDivide(Z, ChunkSize));
			FMaterialVolumeChunkBuilder* Builder = Builders.Find(ChunkCoord);
			if (!Builder)
			{
				Builders.Add(ChunkCoord, FMaterialVolumeChunkBuilder(ChunkCoord, ChunkSize));
				Builder = Builders.Find(ChunkCoord);
			}
			Builder->SetColumnSpans(
				FIntPoint(X - ChunkCoord.X * ChunkSize, Z - ChunkCoord.Y * ChunkSize),
				{ FMaterialSpan(BeginY, EndYExclusive, MaterialIndex) });
		}
	}
	for (const TPair<FIntPoint, FMaterialVolumeChunkBuilder>& Pair : Builders)
	{
		FMaterialVolumeChunkSnapshot Chunk;
		if (!Pair.Value.Build(EMaterialVolumeStorageAdapter::Span, Chunk, OutError))
		{
			return false;
		}
		Candidate.Chunks.Add(Pair.Key, MoveTemp(Chunk));
	}
	if (!Candidate.IsValid(&OutError)) return false;
	OutTopology = MoveTemp(Candidate);
	return true;
}

bool FMaterialVolumeConverters::FromLegacyTerrainXYZ(
	const MatterFlux::PlayableLevel::FLevelTerrain& Terrain,
	FMaterialVolumeTopology& OutTopology,
	FString& OutError,
	const uint16 MaterialIndex)
{
	OutError.Reset();
	if (!Terrain.IsValid() || MaterialIndex == 0)
	{
		OutError = TEXT("legacy terrain or material index is invalid");
		return false;
	}
	FMaterialVolumeTopology Candidate;
	Candidate.DefinitionId = TEXT("legacy.terrain.xyz");
	Candidate.TopologyRevision = 1;
	Candidate.GridFrame.CellSize = Terrain.CellSize;
	Candidate.GridFrame.BasisU = FVector3d::ForwardVector;
	Candidate.GridFrame.BasisV = FVector3d::RightVector;
	Candidate.GridFrame.BasisN = FVector3d::UpVector;
	Candidate.GridFrame.Origin = FVector3d(
		Terrain.FirstCellCenter.X - Terrain.CellSize * 0.5,
		Terrain.FirstCellCenter.Y - Terrain.CellSize * 0.5,
		Terrain.BottomZ);

	constexpr int32 ChunkSize = 16;
	TMap<FIntPoint, FMaterialVolumeChunkBuilder> Builders;
	for (int32 Y = 0; Y < Terrain.Height; ++Y)
	{
		for (int32 X = 0; X < Terrain.Width; ++X)
		{
			const float Height = Terrain.HeightAt(X, Y);
			if (!FMath::IsFinite(Height) || Height < Terrain.BottomZ)
			{
				OutError = TEXT("legacy terrain contains an invalid height");
				return false;
			}
			const int32 EndZExclusive = FMath::Max(0,
				FMath::CeilToInt((Height - Terrain.BottomZ) / Terrain.CellSize));
			if (EndZExclusive == 0) continue;
			const FIntPoint ChunkCoord(X / ChunkSize, Y / ChunkSize);
			FMaterialVolumeChunkBuilder* Builder = Builders.Find(ChunkCoord);
			if (!Builder)
			{
				Builders.Add(ChunkCoord,
					FMaterialVolumeChunkBuilder(ChunkCoord, ChunkSize));
				Builder = Builders.Find(ChunkCoord);
			}
			Builder->SetColumnSpans(
				FIntPoint(X % ChunkSize, Y % ChunkSize),
				{ FMaterialSpan(0, EndZExclusive, MaterialIndex) });
		}
	}
	for (const TPair<FIntPoint, FMaterialVolumeChunkBuilder>& Pair : Builders)
	{
		FMaterialVolumeChunkSnapshot Chunk;
		if (!Pair.Value.Build(EMaterialVolumeStorageAdapter::Span, Chunk, OutError))
		{
			return false;
		}
		Candidate.Chunks.Add(Pair.Key, MoveTemp(Chunk));
	}
	if (!Candidate.IsValid(&OutError)) return false;
	OutTopology = MoveTemp(Candidate);
	return true;
}
