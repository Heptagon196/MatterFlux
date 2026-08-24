#include "Material/MatterFluxSourceReactionRuntime.h"

namespace MatterFlux::Reaction
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
			&& MaxStepsPerAdvance <= 64
			&& MaxActivationsPerStep > 0
			&& MaxActivationsPerStep <= 64;
	}

	FSourceReactionRuntime::FSourceReactionRuntime() = default;
	FSourceReactionRuntime::~FSourceReactionRuntime() = default;

	bool FSourceReactionRuntime::Initialize(
		const FSourceRuntimeSettings& Settings,
		const FFragmentSourceMask& SourceMask,
		const FMatterFluxReactionDefinition& Rule,
		const int32 Seed,
		FString& OutError)
	{
		OutError.Reset();
		if (!Settings.IsValid())
		{
			OutError = TEXT("source reaction runtime settings are invalid");
			return false;
		}
		TUniquePtr<FMaskReaction> Candidate = MakeUnique<FMaskReaction>();
		if (!Candidate->Initialize(SourceMask, Rule, Seed))
		{
			OutError = TEXT("source reaction mask could not be initialized");
			return false;
		}
		Reset();
		RuntimeSettings = Settings;
		Width = SourceMask.Width;
		Height = SourceMask.Height;
		Simulation = MoveTemp(Candidate);
		return true;
	}

	bool FSourceReactionRuntime::RestoreState(
		const FSourceRuntimeSettings& Settings,
		const FSourceRuntimeSnapshot& State,
		const FMatterFluxReactionDefinition& Rule,
		FString& OutError)
	{
		OutError.Reset();
		if (!Settings.IsValid()
			|| !FMath::IsFinite(State.ReactionAccumulator)
			|| State.ReactionAccumulator < 0.0f
			|| State.ReactionAccumulator >= Settings.StepSeconds
			|| State.TotalMaterialEmissionCount < 0)
		{
			OutError = TEXT("source reaction snapshot metadata is invalid");
			return false;
		}
		TUniquePtr<FMaskReaction> Candidate = MakeUnique<FMaskReaction>();
		if (!Candidate->RestoreState(State.ReactionState, Rule, OutError))
		{
			return false;
		}
		Reset();
		RuntimeSettings = Settings;
		Width = State.ReactionState.Width;
		Height = State.ReactionState.Height;
		StepAccumulator = State.ReactionAccumulator;
		TotalMaterialEmissionCount = State.TotalMaterialEmissionCount;
		Simulation = MoveTemp(Candidate);
		return true;
	}

	bool FSourceReactionRuntime::CaptureState(
		FSourceRuntimeSnapshot& OutState) const
	{
		if (!Simulation
			|| !Simulation->CaptureState(OutState.ReactionState))
		{
			return false;
		}
		OutState.ReactionAccumulator = StepAccumulator;
		OutState.TotalMaterialEmissionCount = TotalMaterialEmissionCount;
		return true;
	}

	void FSourceReactionRuntime::Reset()
	{
		Simulation.Reset();
		RuntimeSettings = FSourceRuntimeSettings();
		Width = 0;
		Height = 0;
		StepAccumulator = 0.0f;
		TotalMaterialEmissionCount = 0;
	}

	bool FSourceReactionRuntime::ActivateNearest(
		const FIntPoint RequestedCell,
		const FName StimulusMaterial)
	{
		if (!Simulation)
		{
			return false;
		}
		if (Simulation->Activate(RequestedCell, StimulusMaterial))
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
			&& Simulation->Activate(SearchCenter, StimulusMaterial))
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
						if (Simulation->Activate(FIntPoint(X, Y), StimulusMaterial))
						{
							return true;
						}
					}
					continue;
				}
				if (Simulation->Activate(FIntPoint(MinimumX, Y), StimulusMaterial)
					|| (MaximumX != MinimumX
						&& Simulation->Activate(
							FIntPoint(MaximumX, Y),
							StimulusMaterial)))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool FSourceReactionRuntime::ConstrainInputMask(
		const TArray<uint8>& AllowedInputMask)
	{
		return Simulation
			&& Simulation->ConstrainInputMask(AllowedInputMask);
	}

	FSourceAdvanceResult FSourceReactionRuntime::AdvanceAuthority(
		const float DeltaSeconds)
	{
		FSourceAdvanceResult Result;
		if (!Simulation
			|| !Simulation->IsActive()
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
			const FStepStats Stats = Simulation->Step(
				RuntimeSettings.MaxActivationsPerStep);
			Result.bStateChanged |= !Stats.ChangedCellIndices.IsEmpty();
			Result.bGeometryChanged |= Stats.ConsumedInputCells > 0;
			Result.MaterialEmissionCells.Append(Stats.MaterialEmissionCells);
			Result.ChangedCellIndices.Append(Stats.ChangedCellIndices);
			TotalMaterialEmissionCount += Stats.MaterialEmissionCells.Num();
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

	const TArray<uint8>& FSourceReactionRuntime::GetInputMask() const
	{
		return Simulation ? Simulation->GetInputMask() : EmptyMask();
	}

	const TArray<uint8>& FSourceReactionRuntime::GetOutputMask() const
	{
		return Simulation ? Simulation->GetOutputMask() : EmptyMask();
	}

	const TArray<uint8>& FSourceReactionRuntime::GetActiveMask() const
	{
		return Simulation ? Simulation->GetActiveMask() : EmptyMask();
	}
}
