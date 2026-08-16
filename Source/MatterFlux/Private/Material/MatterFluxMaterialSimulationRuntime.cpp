#include "Material/MatterFluxMaterialSimulationRuntime.h"

#include "Algo/Unique.h"

namespace MatterFlux::Material
{
	bool FRuntimeSettings::IsValid() const
	{
		return World.IsValid()
			&& FMath::IsFinite(StepSeconds)
			&& StepSeconds > 0.0f
			&& MaxStepsPerAdvance > 0
			&& MaxStepsPerAdvance <= 64;
	}

	FSimulationRuntime::FSimulationRuntime() = default;
	FSimulationRuntime::~FSimulationRuntime() = default;

	bool FSimulationRuntime::NormalizeFocuses(
		const TConstArrayView<FIntPoint> Focuses,
		TArray<FIntPoint>& OutFocuses)
	{
		OutFocuses.Reset();
		OutFocuses.Append(Focuses.GetData(), Focuses.Num());
		OutFocuses.Sort([](const FIntPoint& A, const FIntPoint& B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});
		OutFocuses.SetNum(Algo::Unique(OutFocuses));
		return !OutFocuses.IsEmpty();
	}

	bool FSimulationRuntime::Initialize(
		const FRuntimeSettings& Settings,
		const FMatterFluxContentRegistry& Registry,
		const int32 Seed,
		const TConstArrayView<FIntPoint> InitialFocuses,
		FString& OutError)
	{
		Reset();
		OutError.Reset();
		if (!Settings.IsValid()
			|| !NormalizeFocuses(InitialFocuses, CurrentFocuses))
		{
			OutError = TEXT("material runtime settings or initial focuses are invalid");
			return false;
		}

		TUniquePtr<FChunkedMaterialWorld> Candidate =
			MakeUnique<FChunkedMaterialWorld>();
		if (!Candidate->Initialize(Settings.World, Registry, Seed, OutError))
		{
			CurrentFocuses.Reset();
			return false;
		}
		Candidate->SetSimulationFocuses(CurrentFocuses);
		RuntimeSettings = Settings;
		MaterialWorld = MoveTemp(Candidate);
		bReplicationDirty = true;
		return true;
	}

	void FSimulationRuntime::Reset()
	{
		MaterialWorld.Reset();
		RuntimeSettings = FRuntimeSettings();
		CurrentFocuses.Reset();
		StepAccumulator = 0.0f;
		LogicalStep = 0;
		AppliedStateRevision = INDEX_NONE;
		RejectedStateRevision = INDEX_NONE;
		bReplicationDirty = false;
	}

	FRuntimeAdvanceResult FSimulationRuntime::AdvanceAuthority(
		const float DeltaSeconds,
		const TConstArrayView<FIntPoint> Focuses)
	{
		FRuntimeAdvanceResult Result;
		Result.LogicalStep = LogicalStep;
		if (!MaterialWorld)
		{
			return Result;
		}

		TArray<FIntPoint> NormalizedFocuses;
		if (!NormalizeFocuses(Focuses, NormalizedFocuses))
		{
			return Result;
		}
		if (NormalizedFocuses != CurrentFocuses)
		{
			CurrentFocuses = MoveTemp(NormalizedFocuses);
			MaterialWorld->SetSimulationFocuses(CurrentFocuses);
			Result.bFocusChanged = true;
			bReplicationDirty = true;
		}

		StepAccumulator += FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
		if (Result.bFocusChanged)
		{
			return Result;
		}

		while (StepAccumulator >= RuntimeSettings.StepSeconds
			&& Result.Steps < RuntimeSettings.MaxStepsPerAdvance)
		{
			StepAccumulator -= RuntimeSettings.StepSeconds;
			const FStepStats Stats = MaterialWorld->Step();
			LogicalStep = LogicalStep == MAX_int32 ? 0 : LogicalStep + 1;
			Result.bStateChanged |= Stats.MovedCells > 0
				|| Stats.ReactedPairs > 0
				|| Stats.CulledCells > 0;
			++Result.Steps;
		}
		if (Result.Steps == RuntimeSettings.MaxStepsPerAdvance
			&& StepAccumulator >= RuntimeSettings.StepSeconds)
		{
			StepAccumulator = FMath::Fmod(
				StepAccumulator,
				RuntimeSettings.StepSeconds);
		}
		Result.LogicalStep = LogicalStep;
		bReplicationDirty |= Result.Steps > 0;
		return Result;
	}

	bool FSimulationRuntime::BuildReplicatedState(
		const int32 MapSeed,
		const int32 PreviousRevision,
		FMatterFluxReplicatedMaterialState& OutState,
		FString& OutError)
	{
		OutState = FMatterFluxReplicatedMaterialState();
		OutError.Reset();
		if (!MaterialWorld || MapSeed == 0)
		{
			OutError = TEXT("material runtime is not ready to publish");
			return false;
		}
		TArray<uint8> ActiveState;
		if (!MaterialWorld->ExportActiveState(
			LogicalStep,
			ActiveState,
			OutError))
		{
			return false;
		}
		FMatterFluxReplicatedMaterialState Candidate;
		Candidate.MapSeed = MapSeed;
		Candidate.Revision = PreviousRevision == MAX_int32
			? 0
			: PreviousRevision + 1;
		if (!Candidate.EncodeActiveState(ActiveState, OutError))
		{
			return false;
		}
		OutState = MoveTemp(Candidate);
		AppliedStateRevision = OutState.Revision;
		bReplicationDirty = false;
		return true;
	}

	EReplicatedStateApplyResult FSimulationRuntime::ApplyReplicatedState(
		const int32 ExpectedMapSeed,
		const FMatterFluxReplicatedMaterialState& State,
		FString& OutError)
	{
		OutError.Reset();
		if (!MaterialWorld
			|| ExpectedMapSeed == 0
			|| State.MapSeed != ExpectedMapSeed
			|| !State.HasPayload()
			|| AppliedStateRevision == State.Revision
			|| RejectedStateRevision == State.Revision)
		{
			return EReplicatedStateApplyResult::NoChange;
		}

		TArray<uint8> ActiveState;
		if (!State.DecodeActiveState(ActiveState, OutError))
		{
			RejectedStateRevision = State.Revision;
			return EReplicatedStateApplyResult::Rejected;
		}
		int32 ImportedStep = INDEX_NONE;
		FIntPoint ImportedFocus = FIntPoint::ZeroValue;
		if (!MaterialWorld->ImportActiveState(
			ActiveState,
			ImportedStep,
			ImportedFocus,
			OutError))
		{
			RejectedStateRevision = State.Revision;
			return EReplicatedStateApplyResult::Rejected;
		}
		LogicalStep = ImportedStep;
		CurrentFocuses = { ImportedFocus };
		AppliedStateRevision = State.Revision;
		RejectedStateRevision = INDEX_NONE;
		bReplicationDirty = false;
		return EReplicatedStateApplyResult::Applied;
	}

	bool FSimulationRuntime::ExportActiveState(
		TArray<uint8>& OutState,
		FString& OutError) const
	{
		OutState.Reset();
		return MaterialWorld
			&& MaterialWorld->ExportActiveState(
				LogicalStep,
				OutState,
				OutError);
	}

	bool FSimulationRuntime::ImportActiveState(
		const TArray<uint8>& State,
		int32& OutLogicalStep,
		FIntPoint& OutPrimaryFocus,
		FString& OutError)
	{
		if (!MaterialWorld
			|| !MaterialWorld->ImportActiveState(
				State,
				OutLogicalStep,
				OutPrimaryFocus,
				OutError))
		{
			return false;
		}
		LogicalStep = OutLogicalStep;
		CurrentFocuses = { OutPrimaryFocus };
		StepAccumulator = 0.0f;
		bReplicationDirty = true;
		return true;
	}

	bool FSimulationRuntime::SeedSurface(
		const TArray<FSeedCell>& SeedCells)
	{
		const bool bSeeded = MaterialWorld
			&& MaterialWorld->SeedSurface(SeedCells);
		bReplicationDirty |= bSeeded;
		return bSeeded;
	}

	void FSimulationRuntime::SetFocuses(
		const TConstArrayView<FIntPoint> Focuses)
	{
		TArray<FIntPoint> Normalized;
		if (!MaterialWorld || !NormalizeFocuses(Focuses, Normalized))
		{
			return;
		}
		if (Normalized != CurrentFocuses)
		{
			CurrentFocuses = MoveTemp(Normalized);
			MaterialWorld->SetSimulationFocuses(CurrentFocuses);
			bReplicationDirty = true;
		}
	}

	bool FSimulationRuntime::SetCell(
		const FIntPoint& WorldCell,
		const FName MaterialId)
	{
		const bool bChanged = MaterialWorld
			&& MaterialWorld->SetCell(WorldCell, MaterialId);
		bReplicationDirty |= bChanged;
		return bChanged;
	}

	FName FSimulationRuntime::GetMaterialAt(
		const FIntPoint& WorldCell) const
	{
		return MaterialWorld
			? MaterialWorld->GetMaterialAt(WorldCell)
			: NAME_None;
	}

	int32 FSimulationRuntime::CountMaterial(const FName MaterialId) const
	{
		return MaterialWorld ? MaterialWorld->CountMaterial(MaterialId) : 0;
	}

	int32 FSimulationRuntime::GetResidentChunkCount() const
	{
		return MaterialWorld ? MaterialWorld->GetResidentChunkCount() : 0;
	}

	int32 FSimulationRuntime::GetArchivedChunkCount() const
	{
		return MaterialWorld ? MaterialWorld->GetArchivedChunkCount() : 0;
	}

	int32 FSimulationRuntime::GetSimulationFocusCount() const
	{
		return MaterialWorld ? MaterialWorld->GetSimulationFocusCount() : 0;
	}

	void FSimulationRuntime::GetActiveCells(
		TArray<FCellSnapshot>& OutCells) const
	{
		OutCells.Reset();
		if (MaterialWorld)
		{
			MaterialWorld->GetActiveCells(OutCells);
		}
	}
}
