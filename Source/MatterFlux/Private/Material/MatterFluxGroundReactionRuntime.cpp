#include "Material/MatterFluxGroundReactionRuntime.h"

namespace MatterFlux::Reaction
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

	FGroundReactionRuntime::FGroundReactionRuntime() = default;
	FGroundReactionRuntime::~FGroundReactionRuntime() = default;

	bool FGroundReactionRuntime::Initialize(
		const FGroundRuntimeSettings& Settings,
		const FFragmentSourceMask& GroundMask,
		const FMatterFluxReactionDefinition& Rule,
		const int32 Seed,
		FString& OutError)
	{
		Reset();
		OutError.Reset();
		if (!Settings.IsValid()
			|| GroundMask.Width != Settings.Width
			|| GroundMask.Height != Settings.Height)
		{
			OutError = TEXT("ground reaction runtime settings are invalid");
			return false;
		}
		TUniquePtr<FMaskReaction> Candidate = MakeUnique<FMaskReaction>();
		if (!Candidate->Initialize(GroundMask, Rule, Seed))
		{
			OutError = TEXT("ground reaction mask could not be initialized");
			return false;
		}
		RuntimeSettings = Settings;
		VisibleOutputMask = Candidate->GetOutputMask();
		VisibleActiveMask = Candidate->GetActiveMask();
		RebuildVisibleCellIndices();
		Simulation = MoveTemp(Candidate);
		return true;
	}

	void FGroundReactionRuntime::Reset()
	{
		Simulation.Reset();
		RuntimeSettings = FGroundRuntimeSettings();
		VisibleOutputMask.Reset();
		VisibleActiveMask.Reset();
		ActiveCellIndicesByChunk.Reset();
		OutputCellIndicesByChunk.Reset();
		DirtyChunks.Reset();
		AppliedChunkRevisions.Reset();
		StepAccumulator = 0.0f;
		Revision = 0;
	}

	void FGroundReactionRuntime::SetOutputCellIndexState(
		const int32 CellIndex,
		const bool bOutput)
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
			OutputCellIndicesByChunk.Find(ChunkCoordinate);
		const bool bWasOutput = ChunkCells && ChunkCells->Contains(CellIndex);
		if (bWasOutput == bOutput)
		{
			return;
		}
		if (bOutput)
		{
			OutputCellIndicesByChunk.FindOrAdd(ChunkCoordinate).Add(CellIndex);
		}
		else if (ChunkCells)
		{
			ChunkCells->Remove(CellIndex);
			if (ChunkCells->IsEmpty())
			{
				OutputCellIndicesByChunk.Remove(ChunkCoordinate);
			}
		}
	}

	void FGroundReactionRuntime::SetActiveCellIndexState(
		const int32 CellIndex,
		const bool bActive)
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
			ActiveCellIndicesByChunk.Find(ChunkCoordinate);
		const bool bWasActive = ChunkCells && ChunkCells->Contains(CellIndex);
		if (bWasActive == bActive)
		{
			return;
		}
		if (bActive)
		{
			ActiveCellIndicesByChunk.FindOrAdd(ChunkCoordinate).Add(CellIndex);
			return;
		}
		if (ChunkCells)
		{
			ChunkCells->Remove(CellIndex);
			if (ChunkCells->IsEmpty())
			{
				ActiveCellIndicesByChunk.Remove(ChunkCoordinate);
			}
		}
	}

	void FGroundReactionRuntime::MarkCellDirty(const int32 CellIndex)
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

	void FGroundReactionRuntime::RefreshVisibleCellIndicesForChunk(
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
				SetActiveCellIndexState(
					CellIndex,
					VisibleActiveMask.IsValidIndex(CellIndex)
						&& VisibleActiveMask[CellIndex] != 0);
				SetOutputCellIndexState(
					CellIndex,
					VisibleOutputMask.IsValidIndex(CellIndex)
						&& VisibleOutputMask[CellIndex] != 0);
			}
		}
	}

	void FGroundReactionRuntime::RebuildVisibleCellIndices()
	{
		ActiveCellIndicesByChunk.Reset();
		OutputCellIndicesByChunk.Reset();
		const int32 CellCount = FMath::Max(
			VisibleActiveMask.Num(),
			VisibleOutputMask.Num());
		for (int32 CellIndex = 0;
			CellIndex < CellCount;
			++CellIndex)
		{
			SetActiveCellIndexState(
				CellIndex,
				VisibleActiveMask.IsValidIndex(CellIndex)
					&& VisibleActiveMask[CellIndex] != 0);
			SetOutputCellIndexState(
				CellIndex,
				VisibleOutputMask.IsValidIndex(CellIndex)
					&& VisibleOutputMask[CellIndex] != 0);
		}
	}

	bool FGroundReactionRuntime::Activate(
		const FIntPoint Cell,
		const FName StimulusMaterial)
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
		const TArray<uint8>& SimulationActiveMask =
			Simulation->GetActiveMask();
		if (!VisibleActiveMask.IsValidIndex(CellIndex)
			|| !SimulationActiveMask.IsValidIndex(CellIndex)
			|| !Simulation->Activate(Cell, StimulusMaterial))
		{
			return false;
		}
		VisibleActiveMask[CellIndex] =
			SimulationActiveMask[CellIndex];
		SetActiveCellIndexState(CellIndex, true);
		MarkCellDirty(CellIndex);
		return true;
	}

	FGroundAdvanceResult FGroundReactionRuntime::AdvanceAuthority(
		const float DeltaSeconds)
	{
		FGroundAdvanceResult Result;
		if (!Simulation
			|| !Simulation->IsActive()
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
			const TArray<uint8>& SimulationOutputMask =
				Simulation->GetOutputMask();
			const TArray<uint8>& SimulationActiveMask =
				Simulation->GetActiveMask();
			for (const int32 CellIndex : Result.ChangedCellIndices)
			{
				if (!VisibleOutputMask.IsValidIndex(CellIndex)
					|| !VisibleActiveMask.IsValidIndex(CellIndex)
					|| !SimulationOutputMask.IsValidIndex(CellIndex)
					|| !SimulationActiveMask.IsValidIndex(CellIndex))
				{
					continue;
				}
				const bool bWasActive =
					VisibleActiveMask[CellIndex] != 0;
				const bool bWasOutput =
					VisibleOutputMask[CellIndex] != 0;
				const uint8 NewOutput =
					SimulationOutputMask[CellIndex];
				const uint8 NewActive =
					SimulationActiveMask[CellIndex];
				VisibleOutputMask[CellIndex] = NewOutput;
				VisibleActiveMask[CellIndex] = NewActive;
				if (bWasActive != (NewActive != 0))
				{
					SetActiveCellIndexState(
						CellIndex,
						NewActive != 0);
				}
				if (bWasOutput != (NewOutput != 0))
				{
					SetOutputCellIndexState(
						CellIndex,
						NewOutput != 0);
				}
				MarkCellDirty(CellIndex);
			}
		}
		return Result;
	}

	bool FGroundReactionRuntime::BuildReplicationForCoordinates(
		const TConstArrayView<FIntPoint> Coordinates,
		const int32 TargetRevision,
		TArray<FMatterFluxGroundStateChunk>& OutChunks,
		FString& OutError) const
	{
		OutChunks.Reset();
		OutError.Reset();
		if (!Simulation || Coordinates.IsEmpty())
		{
			OutError = TEXT("ground reaction has no chunks to publish");
			return false;
		}
		OutChunks.Reserve(Coordinates.Num());
		for (const FIntPoint Coordinate : Coordinates)
		{
			FMatterFluxGroundStateChunk Chunk;
			if (!Chunk.Encode(
				Coordinate,
				TargetRevision,
				VisibleOutputMask,
				VisibleActiveMask,
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

	bool FGroundReactionRuntime::BuildInitialReplication(
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

	bool FGroundReactionRuntime::BuildPendingReplication(
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

	EGroundChunkApplyResult FGroundReactionRuntime::ApplyReplicatedChunk(
		const FMatterFluxGroundStateChunk& State,
		FString& OutError)
	{
		OutError.Reset();
		if (!Simulation)
		{
			OutError = TEXT("ground reaction runtime is not initialized");
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
			VisibleOutputMask,
			VisibleActiveMask,
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

	bool FGroundReactionRuntime::CaptureState(
		FGroundRuntimeSnapshot& OutState) const
	{
		if (!Simulation || !Simulation->CaptureState(OutState.ReactionState))
		{
			return false;
		}
		OutState.StepAccumulator = StepAccumulator;
		OutState.Revision = Revision;
		return true;
	}

	bool FGroundReactionRuntime::RestoreState(
		const FGroundRuntimeSettings& Settings,
		const FGroundRuntimeSnapshot& State,
		const FMatterFluxReactionDefinition& Rule,
		FString& OutError)
	{
		OutError.Reset();
		if (!Settings.IsValid()
			|| State.ReactionState.Width != Settings.Width
			|| State.ReactionState.Height != Settings.Height
			|| !FMath::IsFinite(State.StepAccumulator)
			|| State.StepAccumulator < 0.0f
			|| State.StepAccumulator >= Settings.StepSeconds
			|| State.Revision < 0)
		{
			OutError = TEXT("saved ground reaction runtime state is invalid");
			return false;
		}
		TUniquePtr<FMaskReaction> Candidate = MakeUnique<FMaskReaction>();
		if (!Candidate->RestoreState(State.ReactionState, Rule, OutError))
		{
			return false;
		}
		Reset();
		RuntimeSettings = Settings;
		StepAccumulator = State.StepAccumulator;
		Revision = State.Revision;
		VisibleOutputMask = Candidate->GetOutputMask();
		VisibleActiveMask = Candidate->GetActiveMask();
		RebuildVisibleCellIndices();
		Simulation = MoveTemp(Candidate);
		return true;
	}

	int32 FGroundReactionRuntime::CountOutputCells() const
	{
		int32 Count = 0;
		for (const TPair<FIntPoint, TSet<int32>>& Pair
			: OutputCellIndicesByChunk)
		{
			Count += Pair.Value.Num();
		}
		return Count;
	}

	void FGroundReactionRuntime::GatherActiveCellIndices(
		TArray<int32>& OutCellIndices) const
	{
		OutCellIndices.Reset();
		for (const TPair<FIntPoint, TSet<int32>>& Pair
			: ActiveCellIndicesByChunk)
		{
			OutCellIndices.Append(Pair.Value.Array());
		}
		OutCellIndices.Sort();
	}

	void FGroundReactionRuntime::GatherOutputCellIndices(
		TArray<int32>& OutCellIndices) const
	{
		OutCellIndices.Reset();
		for (const TPair<FIntPoint, TSet<int32>>& Pair
			: OutputCellIndicesByChunk)
		{
			OutCellIndices.Append(Pair.Value.Array());
		}
		OutCellIndices.Sort();
	}

	void FGroundReactionRuntime::GatherActiveChunkCoordinates(
		TArray<FIntPoint>& OutChunkCoordinates) const
	{
		ActiveCellIndicesByChunk.GetKeys(OutChunkCoordinates);
		OutChunkCoordinates.Sort(
			[](const FIntPoint A, const FIntPoint B)
			{
				return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
			});
	}

	void FGroundReactionRuntime::GatherVisibleCellIndicesForChunks(
		const TConstArrayView<FIntPoint> ChunkCoordinates,
		TArray<int32>& OutOutputCellIndices,
		TArray<int32>& OutActiveCellIndices) const
	{
		TSet<int32> OutputCells;
		TSet<int32> ActiveCells;
		for (const FIntPoint ChunkCoordinate : ChunkCoordinates)
		{
			if (const TSet<int32>* ChunkOutput =
				OutputCellIndicesByChunk.Find(ChunkCoordinate))
			{
				OutputCells.Append(*ChunkOutput);
			}
			if (const TSet<int32>* ChunkActive =
				ActiveCellIndicesByChunk.Find(ChunkCoordinate))
			{
				ActiveCells.Append(*ChunkActive);
			}
		}
		OutOutputCellIndices = OutputCells.Array();
		OutActiveCellIndices = ActiveCells.Array();
		OutOutputCellIndices.Sort();
		OutActiveCellIndices.Sort();
	}
}
