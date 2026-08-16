#include "Material/MatterFluxGroundCombustionRuntime.h"

namespace MatterFlux::Combustion
{
	namespace
	{
		bool IsNewerRevision(const int32 Candidate, const int32 Applied)
		{
			if (Candidate < 0 || Applied < 0 || Candidate == Applied)
			{
				return false;
			}
			constexpr uint32 RevisionMask = 0x7fffffffu;
			constexpr uint32 HalfRange = 0x40000000u;
			const uint32 Distance =
				(static_cast<uint32>(Candidate)
					- static_cast<uint32>(Applied))
				& RevisionMask;
			return Distance != 0 && Distance < HalfRange;
		}
	}

	bool FGroundRuntimeSettings::IsValid() const
	{
		const int64 CellCount =
			static_cast<int64>(Width) * static_cast<int64>(Height);
		return Width > 0
			&& Height > 0
			&& CellCount > 0
			&& CellCount <= 1'048'576
			&& ChunkSize == 64
			&& Width % ChunkSize == 0
			&& Height % ChunkSize == 0
			&& FMath::IsFinite(StepSeconds)
			&& StepSeconds > 0.0f
			&& MaxStepsPerAdvance > 0
			&& MaxStepsPerAdvance <= 64;
	}

	FGroundCombustionRuntime::FGroundCombustionRuntime() = default;
	FGroundCombustionRuntime::~FGroundCombustionRuntime() = default;

	bool FGroundCombustionRuntime::Initialize(
		const FGroundRuntimeSettings& Settings,
		const FFragmentSourceMask& GroundMask,
		const FMatterFluxCombustionDefinition& Rule,
		const int32 Seed,
		FString& OutError)
	{
		Reset();
		OutError.Reset();
		if (!Settings.IsValid()
			|| GroundMask.Width != Settings.Width
			|| GroundMask.Height != Settings.Height)
		{
			OutError = TEXT("ground combustion runtime settings are invalid");
			return false;
		}
		TUniquePtr<FMaskCombustion> Candidate = MakeUnique<FMaskCombustion>();
		if (!Candidate->Initialize(GroundMask, Rule, Seed))
		{
			OutError = TEXT("ground combustion mask could not be initialized");
			return false;
		}
		RuntimeSettings = Settings;
		VisibleResidueMask = Candidate->GetResidueMask();
		VisibleBurningMask = Candidate->GetBurningMask();
		RebuildVisibleCellIndices();
		Simulation = MoveTemp(Candidate);
		return true;
	}

	void FGroundCombustionRuntime::Reset()
	{
		Simulation.Reset();
		RuntimeSettings = FGroundRuntimeSettings();
		VisibleResidueMask.Reset();
		VisibleBurningMask.Reset();
		BurningCellIndicesByChunk.Reset();
		ResidueCellIndicesByChunk.Reset();
		DirtyChunks.Reset();
		AppliedChunkRevisions.Reset();
		StepAccumulator = 0.0f;
		Revision = 0;
	}

	void FGroundCombustionRuntime::SetResidueCellIndexState(
		const int32 CellIndex,
		const bool bResidue)
	{
		if (!RuntimeSettings.IsValid()
			|| CellIndex < 0
			|| CellIndex >= RuntimeSettings.Width * RuntimeSettings.Height)
		{
			return;
		}
		const int32 CellX = CellIndex % RuntimeSettings.Width;
		const int32 CellY = CellIndex / RuntimeSettings.Width;
		const FIntPoint ChunkCoordinate(
			CellX / RuntimeSettings.ChunkSize,
			CellY / RuntimeSettings.ChunkSize);
		TSet<int32>* ChunkCells =
			ResidueCellIndicesByChunk.Find(ChunkCoordinate);
		const bool bWasResidue = ChunkCells && ChunkCells->Contains(CellIndex);
		if (bWasResidue == bResidue)
		{
			return;
		}
		if (bResidue)
		{
			ResidueCellIndicesByChunk.FindOrAdd(ChunkCoordinate).Add(CellIndex);
		}
		else if (ChunkCells)
		{
			ChunkCells->Remove(CellIndex);
			if (ChunkCells->IsEmpty())
			{
				ResidueCellIndicesByChunk.Remove(ChunkCoordinate);
			}
		}
	}

	void FGroundCombustionRuntime::SetBurningCellIndexState(
		const int32 CellIndex,
		const bool bBurning)
	{
		if (!RuntimeSettings.IsValid()
			|| CellIndex < 0
			|| CellIndex >= RuntimeSettings.Width * RuntimeSettings.Height)
		{
			return;
		}
		const int32 CellX = CellIndex % RuntimeSettings.Width;
		const int32 CellY = CellIndex / RuntimeSettings.Width;
		const FIntPoint ChunkCoordinate(
			CellX / RuntimeSettings.ChunkSize,
			CellY / RuntimeSettings.ChunkSize);
		TSet<int32>* ChunkCells =
			BurningCellIndicesByChunk.Find(ChunkCoordinate);
		const bool bWasBurning = ChunkCells && ChunkCells->Contains(CellIndex);
		if (bWasBurning == bBurning)
		{
			return;
		}
		if (bBurning)
		{
			BurningCellIndicesByChunk.FindOrAdd(ChunkCoordinate).Add(CellIndex);
			return;
		}
		if (ChunkCells)
		{
			ChunkCells->Remove(CellIndex);
			if (ChunkCells->IsEmpty())
			{
				BurningCellIndicesByChunk.Remove(ChunkCoordinate);
			}
		}
	}

	void FGroundCombustionRuntime::MarkCellDirty(const int32 CellIndex)
	{
		if (!Simulation || CellIndex < 0
			|| CellIndex >= RuntimeSettings.Width * RuntimeSettings.Height)
		{
			return;
		}
		const int32 CellX = CellIndex % RuntimeSettings.Width;
		const int32 CellY = CellIndex / RuntimeSettings.Width;
		DirtyChunks.Add(FIntPoint(
			CellX / RuntimeSettings.ChunkSize,
			CellY / RuntimeSettings.ChunkSize));
	}

	void FGroundCombustionRuntime::RefreshVisibleCellIndicesForChunk(
		const FIntPoint ChunkCoordinate)
	{
		if (!RuntimeSettings.IsValid())
		{
			return;
		}
		const int32 StartX = ChunkCoordinate.X * RuntimeSettings.ChunkSize;
		const int32 StartY = ChunkCoordinate.Y * RuntimeSettings.ChunkSize;
		const int32 EndX = FMath::Min(
			StartX + RuntimeSettings.ChunkSize,
			RuntimeSettings.Width);
		const int32 EndY = FMath::Min(
			StartY + RuntimeSettings.ChunkSize,
			RuntimeSettings.Height);
		if (StartX < 0 || StartY < 0
			|| StartX >= EndX || StartY >= EndY)
		{
			return;
		}
		for (int32 Y = StartY; Y < EndY; ++Y)
		{
			for (int32 X = StartX; X < EndX; ++X)
			{
				const int32 CellIndex = Y * RuntimeSettings.Width + X;
				SetBurningCellIndexState(
					CellIndex,
					VisibleBurningMask.IsValidIndex(CellIndex)
						&& VisibleBurningMask[CellIndex] != 0);
				SetResidueCellIndexState(
					CellIndex,
					VisibleResidueMask.IsValidIndex(CellIndex)
						&& VisibleResidueMask[CellIndex] != 0);
			}
		}
	}

	void FGroundCombustionRuntime::RebuildVisibleCellIndices()
	{
		BurningCellIndicesByChunk.Reset();
		ResidueCellIndicesByChunk.Reset();
		const int32 CellCount = FMath::Max(
			VisibleBurningMask.Num(),
			VisibleResidueMask.Num());
		for (int32 CellIndex = 0;
			CellIndex < CellCount;
			++CellIndex)
		{
			SetBurningCellIndexState(
				CellIndex,
				VisibleBurningMask.IsValidIndex(CellIndex)
					&& VisibleBurningMask[CellIndex] != 0);
			SetResidueCellIndexState(
				CellIndex,
				VisibleResidueMask.IsValidIndex(CellIndex)
					&& VisibleResidueMask[CellIndex] != 0);
		}
	}

	bool FGroundCombustionRuntime::Ignite(
		const FIntPoint Cell,
		const FName IgnitionMaterial)
	{
		if (!Simulation
			|| Cell.X < 0
			|| Cell.X >= RuntimeSettings.Width
			|| Cell.Y < 0
			|| Cell.Y >= RuntimeSettings.Height)
		{
			return false;
		}
		const int32 CellIndex = Cell.Y * RuntimeSettings.Width + Cell.X;
		const TArray<uint8>& SimulationBurningMask =
			Simulation->GetBurningMask();
		if (!VisibleBurningMask.IsValidIndex(CellIndex)
			|| !SimulationBurningMask.IsValidIndex(CellIndex)
			|| !Simulation->Ignite(Cell, IgnitionMaterial))
		{
			return false;
		}
		VisibleBurningMask[CellIndex] =
			SimulationBurningMask[CellIndex];
		SetBurningCellIndexState(CellIndex, true);
		MarkCellDirty(CellIndex);
		return true;
	}

	FGroundAdvanceResult FGroundCombustionRuntime::AdvanceAuthority(
		const float DeltaSeconds)
	{
		FGroundAdvanceResult Result;
		if (!Simulation
			|| !Simulation->IsBurning()
			|| !FMath::IsFinite(DeltaSeconds))
		{
			return Result;
		}
		StepAccumulator += FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
		TArray<int32> ChangedCellIndices;
		while (StepAccumulator >= RuntimeSettings.StepSeconds
			&& Result.Steps < RuntimeSettings.MaxStepsPerAdvance)
		{
			StepAccumulator -= RuntimeSettings.StepSeconds;
			const FStepStats Stats = Simulation->Step();
			Result.bStateChanged |= !Stats.ChangedCellIndices.IsEmpty();
			for (const int32 CellIndex : Stats.ChangedCellIndices)
			{
				ChangedCellIndices.Add(CellIndex);
			}
			++Result.Steps;
		}
		if (Result.Steps == RuntimeSettings.MaxStepsPerAdvance
			&& StepAccumulator >= RuntimeSettings.StepSeconds)
		{
			StepAccumulator = FMath::Fmod(
				StepAccumulator,
				RuntimeSettings.StepSeconds);
		}
		if (Result.bStateChanged)
		{
			ChangedCellIndices.Sort();
			int32 UniqueCount = 0;
			for (const int32 CellIndex : ChangedCellIndices)
			{
				if (UniqueCount == 0
					|| ChangedCellIndices[UniqueCount - 1] != CellIndex)
				{
					ChangedCellIndices[UniqueCount++] = CellIndex;
				}
			}
			ChangedCellIndices.SetNum(UniqueCount, EAllowShrinking::No);
			Result.ChangedCellIndices = MoveTemp(ChangedCellIndices);
			const TArray<uint8>& SimulationResidueMask =
				Simulation->GetResidueMask();
			const TArray<uint8>& SimulationBurningMask =
				Simulation->GetBurningMask();
			for (const int32 CellIndex : Result.ChangedCellIndices)
			{
				if (!VisibleResidueMask.IsValidIndex(CellIndex)
					|| !VisibleBurningMask.IsValidIndex(CellIndex)
					|| !SimulationResidueMask.IsValidIndex(CellIndex)
					|| !SimulationBurningMask.IsValidIndex(CellIndex))
				{
					continue;
				}
				const bool bWasBurning =
					VisibleBurningMask[CellIndex] != 0;
				const bool bWasResidue =
					VisibleResidueMask[CellIndex] != 0;
				const uint8 NewResidue =
					SimulationResidueMask[CellIndex];
				const uint8 NewBurning =
					SimulationBurningMask[CellIndex];
				VisibleResidueMask[CellIndex] = NewResidue;
				VisibleBurningMask[CellIndex] = NewBurning;
				if (bWasBurning != (NewBurning != 0))
				{
					SetBurningCellIndexState(
						CellIndex,
						NewBurning != 0);
				}
				if (bWasResidue != (NewResidue != 0))
				{
					SetResidueCellIndexState(
						CellIndex,
						NewResidue != 0);
				}
				MarkCellDirty(CellIndex);
			}
		}
		return Result;
	}

	bool FGroundCombustionRuntime::BuildReplicationForCoordinates(
		const TConstArrayView<FIntPoint> Coordinates,
		const int32 TargetRevision,
		TArray<FMatterFluxGroundStateChunk>& OutChunks,
		FString& OutError) const
	{
		OutChunks.Reset();
		OutError.Reset();
		if (!Simulation || Coordinates.IsEmpty())
		{
			OutError = TEXT("ground combustion has no chunks to publish");
			return false;
		}
		OutChunks.Reserve(Coordinates.Num());
		for (const FIntPoint Coordinate : Coordinates)
		{
			FMatterFluxGroundStateChunk Chunk;
			if (!Chunk.Encode(
				Coordinate,
				TargetRevision,
				VisibleResidueMask,
				VisibleBurningMask,
				RuntimeSettings.Width,
				RuntimeSettings.Height,
				OutError))
			{
				OutChunks.Reset();
				return false;
			}
			OutChunks.Add(MoveTemp(Chunk));
		}
		return true;
	}

	bool FGroundCombustionRuntime::BuildInitialReplication(
		TArray<FMatterFluxGroundStateChunk>& OutChunks,
		FString& OutError) const
	{
		TArray<FIntPoint> Coordinates;
		if (Simulation)
		{
			const int32 ChunkCountX =
				RuntimeSettings.Width / RuntimeSettings.ChunkSize;
			const int32 ChunkCountY =
				RuntimeSettings.Height / RuntimeSettings.ChunkSize;
			Coordinates.Reserve(ChunkCountX * ChunkCountY);
			for (int32 Y = 0; Y < ChunkCountY; ++Y)
			{
				for (int32 X = 0; X < ChunkCountX; ++X)
				{
					Coordinates.Add(FIntPoint(X, Y));
				}
			}
		}
		return BuildReplicationForCoordinates(
			Coordinates,
			Revision,
			OutChunks,
			OutError);
	}

	bool FGroundCombustionRuntime::BuildPendingReplication(
		TArray<FMatterFluxGroundStateChunk>& OutChunks,
		FString& OutError)
	{
		TArray<FIntPoint> Coordinates = DirtyChunks.Array();
		Coordinates.Sort([](const FIntPoint& A, const FIntPoint& B)
		{
			return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
		});
		const int32 CandidateRevision = Revision == MAX_int32
			? 0
			: Revision + 1;
		if (!BuildReplicationForCoordinates(
			Coordinates,
			CandidateRevision,
			OutChunks,
			OutError))
		{
			return false;
		}
		Revision = CandidateRevision;
		DirtyChunks.Reset();
		return true;
	}

	EGroundChunkApplyResult FGroundCombustionRuntime::ApplyReplicatedChunk(
		const FMatterFluxGroundStateChunk& State,
		FString& OutError)
	{
		OutError.Reset();
		if (!Simulation)
		{
			OutError = TEXT("ground combustion runtime is not initialized");
			return EGroundChunkApplyResult::Rejected;
		}
		if (const int32* Applied =
			AppliedChunkRevisions.Find(State.ChunkCoordinate))
		{
			if (!IsNewerRevision(State.Revision, *Applied))
			{
				return EGroundChunkApplyResult::NoChange;
			}
		}
		if (!State.DecodeInto(
			VisibleResidueMask,
			VisibleBurningMask,
			RuntimeSettings.Width,
			RuntimeSettings.Height,
			OutError))
		{
			return EGroundChunkApplyResult::Rejected;
		}
		RefreshVisibleCellIndicesForChunk(State.ChunkCoordinate);
		AppliedChunkRevisions.Add(State.ChunkCoordinate, State.Revision);
		return EGroundChunkApplyResult::Applied;
	}

	bool FGroundCombustionRuntime::CaptureState(
		FGroundRuntimeSnapshot& OutState) const
	{
		if (!Simulation || !Simulation->CaptureState(OutState.CombustionState))
		{
			return false;
		}
		OutState.StepAccumulator = StepAccumulator;
		OutState.Revision = Revision;
		return true;
	}

	bool FGroundCombustionRuntime::RestoreState(
		const FGroundRuntimeSettings& Settings,
		const FGroundRuntimeSnapshot& State,
		const FMatterFluxCombustionDefinition& Rule,
		FString& OutError)
	{
		OutError.Reset();
		if (!Settings.IsValid()
			|| State.CombustionState.Width != Settings.Width
			|| State.CombustionState.Height != Settings.Height
			|| !FMath::IsFinite(State.StepAccumulator)
			|| State.StepAccumulator < 0.0f
			|| State.StepAccumulator >= Settings.StepSeconds
			|| State.Revision < 0)
		{
			OutError = TEXT("saved ground combustion runtime state is invalid");
			return false;
		}
		TUniquePtr<FMaskCombustion> Candidate = MakeUnique<FMaskCombustion>();
		if (!Candidate->RestoreState(State.CombustionState, Rule, OutError))
		{
			return false;
		}
		Reset();
		RuntimeSettings = Settings;
		StepAccumulator = State.StepAccumulator;
		Revision = State.Revision;
		VisibleResidueMask = Candidate->GetResidueMask();
		VisibleBurningMask = Candidate->GetBurningMask();
		RebuildVisibleCellIndices();
		Simulation = MoveTemp(Candidate);
		return true;
	}

	int32 FGroundCombustionRuntime::CountResidueCells() const
	{
		int32 Count = 0;
		for (const TPair<FIntPoint, TSet<int32>>& Pair
			: ResidueCellIndicesByChunk)
		{
			Count += Pair.Value.Num();
		}
		return Count;
	}

	void FGroundCombustionRuntime::GatherBurningCellIndices(
		TArray<int32>& OutCellIndices) const
	{
		OutCellIndices.Reset();
		for (const TPair<FIntPoint, TSet<int32>>& Pair
			: BurningCellIndicesByChunk)
		{
			OutCellIndices.Append(Pair.Value.Array());
		}
		OutCellIndices.Sort();
	}

	void FGroundCombustionRuntime::GatherResidueCellIndices(
		TArray<int32>& OutCellIndices) const
	{
		OutCellIndices.Reset();
		for (const TPair<FIntPoint, TSet<int32>>& Pair
			: ResidueCellIndicesByChunk)
		{
			OutCellIndices.Append(Pair.Value.Array());
		}
		OutCellIndices.Sort();
	}

	void FGroundCombustionRuntime::GatherBurningChunkCoordinates(
		TArray<FIntPoint>& OutChunkCoordinates) const
	{
		BurningCellIndicesByChunk.GetKeys(OutChunkCoordinates);
		OutChunkCoordinates.Sort(
			[](const FIntPoint A, const FIntPoint B)
			{
				return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
			});
	}

	void FGroundCombustionRuntime::GatherVisibleCellIndicesForChunks(
		const TConstArrayView<FIntPoint> ChunkCoordinates,
		TArray<int32>& OutResidueCellIndices,
		TArray<int32>& OutBurningCellIndices) const
	{
		TSet<int32> ResidueCells;
		TSet<int32> BurningCells;
		for (const FIntPoint ChunkCoordinate : ChunkCoordinates)
		{
			if (const TSet<int32>* ChunkResidue =
				ResidueCellIndicesByChunk.Find(ChunkCoordinate))
			{
				ResidueCells.Append(*ChunkResidue);
			}
			if (const TSet<int32>* ChunkBurning =
				BurningCellIndicesByChunk.Find(ChunkCoordinate))
			{
				BurningCells.Append(*ChunkBurning);
			}
		}
		OutResidueCellIndices = ResidueCells.Array();
		OutBurningCellIndices = BurningCells.Array();
		OutResidueCellIndices.Sort();
		OutBurningCellIndices.Sort();
	}
}
