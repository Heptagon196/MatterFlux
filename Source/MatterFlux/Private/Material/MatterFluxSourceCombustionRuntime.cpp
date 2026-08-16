#include "Material/MatterFluxSourceCombustionRuntime.h"

namespace MatterFlux::Combustion
{
	namespace
	{
		const TArray<uint8>& EmptyMask()
		{
			static const TArray<uint8> Empty;
			return Empty;
		}
	}

	bool FSourceRuntimeSettings::IsValid() const
	{
		return FMath::IsFinite(StepSeconds)
			&& StepSeconds > 0.0f
			&& MaxStepsPerAdvance > 0
			&& MaxStepsPerAdvance <= 64;
	}

	FSourceCombustionRuntime::FSourceCombustionRuntime() = default;
	FSourceCombustionRuntime::~FSourceCombustionRuntime() = default;

	bool FSourceCombustionRuntime::Initialize(
		const FSourceRuntimeSettings& Settings,
		const FFragmentSourceMask& SourceMask,
		const FMatterFluxCombustionDefinition& Rule,
		const int32 Seed,
		FString& OutError)
	{
		OutError.Reset();
		if (!Settings.IsValid())
		{
			OutError = TEXT("source combustion runtime settings are invalid");
			return false;
		}
		TUniquePtr<FMaskCombustion> Candidate = MakeUnique<FMaskCombustion>();
		if (!Candidate->Initialize(SourceMask, Rule, Seed))
		{
			OutError = TEXT("source combustion mask could not be initialized");
			return false;
		}
		Reset();
		RuntimeSettings = Settings;
		Width = SourceMask.Width;
		Height = SourceMask.Height;
		Simulation = MoveTemp(Candidate);
		return true;
	}

	bool FSourceCombustionRuntime::RestoreState(
		const FSourceRuntimeSettings& Settings,
		const FSourceRuntimeSnapshot& State,
		const FMatterFluxCombustionDefinition& Rule,
		FString& OutError)
	{
		OutError.Reset();
		if (!Settings.IsValid()
			|| !FMath::IsFinite(State.CombustionAccumulator)
			|| State.CombustionAccumulator < 0.0f
			|| State.CombustionAccumulator >= Settings.StepSeconds
			|| State.TotalSmokeEmissionCount < 0)
		{
			OutError = TEXT("source combustion snapshot metadata is invalid");
			return false;
		}
		TUniquePtr<FMaskCombustion> Candidate = MakeUnique<FMaskCombustion>();
		if (!Candidate->RestoreState(State.CombustionState, Rule, OutError))
		{
			return false;
		}
		Reset();
		RuntimeSettings = Settings;
		Width = State.CombustionState.Width;
		Height = State.CombustionState.Height;
		StepAccumulator = State.CombustionAccumulator;
		TotalSmokeEmissionCount = State.TotalSmokeEmissionCount;
		Simulation = MoveTemp(Candidate);
		return true;
	}

	bool FSourceCombustionRuntime::CaptureState(
		FSourceRuntimeSnapshot& OutState) const
	{
		if (!Simulation
			|| !Simulation->CaptureState(OutState.CombustionState))
		{
			return false;
		}
		OutState.CombustionAccumulator = StepAccumulator;
		OutState.TotalSmokeEmissionCount = TotalSmokeEmissionCount;
		return true;
	}

	void FSourceCombustionRuntime::Reset()
	{
		Simulation.Reset();
		RuntimeSettings = FSourceRuntimeSettings();
		Width = 0;
		Height = 0;
		StepAccumulator = 0.0f;
		TotalSmokeEmissionCount = 0;
	}

	bool FSourceCombustionRuntime::IgniteNearest(
		const FIntPoint RequestedCell,
		const FName IgnitionMaterial)
	{
		if (!Simulation)
		{
			return false;
		}
		if (Simulation->Ignite(RequestedCell, IgnitionMaterial))
		{
			return true;
		}

		if (Width <= 0 || Height <= 0)
		{
			return false;
		}
		const FIntPoint SearchCenter(
			FMath::Clamp(RequestedCell.X, 0, Width - 1),
			FMath::Clamp(RequestedCell.Y, 0, Height - 1));
		if (SearchCenter != RequestedCell
			&& Simulation->Ignite(SearchCenter, IgnitionMaterial))
		{
			return true;
		}
		const int32 MaximumRadius = FMath::Max(
			Width,
			Height);
		for (int32 Radius = 1; Radius <= MaximumRadius; ++Radius)
		{
			for (int32 Y = 0; Y < Height; ++Y)
			{
				const int32 DeltaY = FMath::Abs(Y - SearchCenter.Y);
				if (DeltaY > Radius)
				{
					continue;
				}
				const int32 MinimumX = FMath::Max(SearchCenter.X - Radius, 0);
				const int32 MaximumX = FMath::Min(
					SearchCenter.X + Radius,
					Width - 1);
				if (MinimumX > MaximumX)
				{
					continue;
				}
				if (DeltaY == Radius)
				{
					for (int32 X = MinimumX; X <= MaximumX; ++X)
					{
						if (Simulation->Ignite(FIntPoint(X, Y), IgnitionMaterial))
						{
							return true;
						}
					}
					continue;
				}
				if (Simulation->Ignite(FIntPoint(MinimumX, Y), IgnitionMaterial)
					|| (MaximumX != MinimumX
						&& Simulation->Ignite(
							FIntPoint(MaximumX, Y),
							IgnitionMaterial)))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool FSourceCombustionRuntime::ConstrainFuelMask(
		const TArray<uint8>& AllowedFuelMask)
	{
		return Simulation
			&& Simulation->ConstrainFuelMask(AllowedFuelMask);
	}

	FSourceAdvanceResult FSourceCombustionRuntime::AdvanceAuthority(
		const float DeltaSeconds)
	{
		FSourceAdvanceResult Result;
		if (!Simulation
			|| !Simulation->IsBurning()
			|| !FMath::IsFinite(DeltaSeconds))
		{
			return Result;
		}
		StepAccumulator += FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
		while (StepAccumulator + UE_SMALL_NUMBER
				>= RuntimeSettings.StepSeconds
			&& Result.Steps < RuntimeSettings.MaxStepsPerAdvance)
		{
			StepAccumulator = FMath::Max(
				0.0f,
				StepAccumulator - RuntimeSettings.StepSeconds);
			const FStepStats Stats = Simulation->Step();
			Result.bStateChanged |= !Stats.ChangedCellIndices.IsEmpty();
			Result.bGeometryChanged |= Stats.ConsumedFuelCells > 0;
			Result.SmokeEmissionCells.Append(Stats.SmokeEmissionCells);
			Result.ChangedCellIndices.Append(Stats.ChangedCellIndices);
			TotalSmokeEmissionCount += Stats.SmokeEmissionCells.Num();
			++Result.Steps;
		}
		if (Result.Steps == RuntimeSettings.MaxStepsPerAdvance
			&& StepAccumulator >= RuntimeSettings.StepSeconds)
		{
			StepAccumulator = FMath::Fmod(
				StepAccumulator,
				RuntimeSettings.StepSeconds);
		}
		return Result;
	}

	const TArray<uint8>& FSourceCombustionRuntime::GetFuelMask() const
	{
		return Simulation ? Simulation->GetFuelMask() : EmptyMask();
	}

	const TArray<uint8>& FSourceCombustionRuntime::GetResidueMask() const
	{
		return Simulation ? Simulation->GetResidueMask() : EmptyMask();
	}

	const TArray<uint8>& FSourceCombustionRuntime::GetBurningMask() const
	{
		return Simulation ? Simulation->GetBurningMask() : EmptyMask();
	}
}
