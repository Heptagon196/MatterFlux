// Developer-only deterministic replay timeline implementation.
#include "Testing/MatterFluxReplayRuntime.h"

namespace
{
	template<typename ItemType>
	bool IsStableTimeline(const TArray<ItemType>& Items)
	{
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (!FMath::IsFinite(Items[Index].TimeSeconds)
				|| Items[Index].TimeSeconds < 0.0
				|| (Index > 0
					&& Items[Index - 1].TimeSeconds
						> Items[Index].TimeSeconds))
			{
				return false;
			}
		}
		return true;
	}
}

namespace MatterFlux::Recording
{
	void FReplayFrame::Reset()
	{
		Operations.Reset();
		Movements.Reset();
		ExpectedStates.Reset();
		Screenshots.Reset();
		bComplete = false;
	}

	bool FReplayRuntime::Initialize(
		const FMatterFluxSessionRecording& InRecording,
		const FReplayRuntimeSettings& InSettings,
		FString& OutError)
	{
		OutError.Reset();
		if (!FMath::IsFinite(InRecording.DurationSeconds)
			|| InRecording.DurationSeconds < 0.0
			|| !FMath::IsFinite(InSettings.CompletionGraceSeconds)
			|| InSettings.CompletionGraceSeconds < 0.0
			|| !IsStableTimeline(InRecording.Operations)
			|| !IsStableTimeline(InRecording.States)
			|| !IsStableTimeline(InRecording.Screenshots))
		{
			OutError = TEXT("Replay timeline or settings are invalid.");
			return false;
		}

		Recording = &InRecording;
		Settings = InSettings;
		NextOperationIndex = 0;
		NextStateIndex = Settings.bVerifyStates
			? 0
			: Recording->States.Num();
		NextScreenshotIndex = 0;
		LastElapsedSeconds = 0.0;
		bCompletionReported = false;
		MovementByPlayer.Reset();
		return true;
	}

	bool FReplayRuntime::Advance(
		const double ElapsedSeconds,
		FReplayFrame& OutFrame,
		FString& OutError)
	{
		OutFrame.Reset();
		OutError.Reset();
		if (!Recording
			|| !FMath::IsFinite(ElapsedSeconds)
			|| ElapsedSeconds < 0.0
			|| ElapsedSeconds + UE_KINDA_SMALL_NUMBER
				< LastElapsedSeconds)
		{
			OutError = TEXT("Replay time is invalid or moved backwards.");
			return false;
		}

		int32 CandidateOperationIndex = NextOperationIndex;
		int32 CandidateStateIndex = NextStateIndex;
		int32 CandidateScreenshotIndex = NextScreenshotIndex;
		TMap<int32, FVector2D> CandidateMovements = MovementByPlayer;
		FReplayFrame CandidateFrame;
		while (CandidateOperationIndex < Recording->Operations.Num()
			&& Recording->Operations[CandidateOperationIndex].TimeSeconds
				<= ElapsedSeconds + KINDA_SMALL_NUMBER)
		{
			const FMatterFluxRecordedOperation& Operation =
				Recording->Operations[CandidateOperationIndex++];
			if (Operation.Operation == EMatterFluxRecordedOperation::Move)
			{
				CandidateMovements.Add(Operation.PlayerId, Operation.Value);
			}
			else
			{
				CandidateFrame.Operations.Add(Operation);
			}
		}

		TArray<int32> MovementPlayerIds;
		CandidateMovements.GenerateKeyArray(MovementPlayerIds);
		MovementPlayerIds.Sort();
		for (const int32 PlayerId : MovementPlayerIds)
		{
			FReplayMovement& Movement =
				CandidateFrame.Movements.AddDefaulted_GetRef();
			Movement.PlayerId = PlayerId;
			Movement.Value = CandidateMovements.FindChecked(PlayerId);
		}

		if (Settings.bVerifyStates)
		{
			TMap<int32, int32> LatestStateIndexByPlayer;
			while (CandidateStateIndex < Recording->States.Num()
				&& Recording->States[CandidateStateIndex].TimeSeconds
					<= ElapsedSeconds + KINDA_SMALL_NUMBER)
			{
				LatestStateIndexByPlayer.Add(
					Recording->States[CandidateStateIndex].PlayerId,
					CandidateStateIndex);
				++CandidateStateIndex;
			}
			TArray<int32> StatePlayerIds;
			LatestStateIndexByPlayer.GenerateKeyArray(StatePlayerIds);
			StatePlayerIds.Sort();
			for (const int32 PlayerId : StatePlayerIds)
			{
				const FMatterFluxRecordedPlayerState& Expected =
					Recording->States[
						LatestStateIndexByPlayer.FindChecked(PlayerId)];
				FVector ExpectedLocation = Expected.Location;
				for (int32 Index = CandidateStateIndex;
					Index < Recording->States.Num();
					++Index)
				{
					const FMatterFluxRecordedPlayerState& Next =
						Recording->States[Index];
					if (Next.PlayerId != PlayerId)
					{
						continue;
					}
					if (Next.TimeSeconds > Expected.TimeSeconds)
					{
						const double Alpha = FMath::Clamp(
							(ElapsedSeconds - Expected.TimeSeconds)
								/ (Next.TimeSeconds
									- Expected.TimeSeconds),
							0.0,
							1.0);
						ExpectedLocation = FMath::Lerp(
							Expected.Location,
							Next.Location,
							Alpha);
					}
					break;
				}
				FReplayExpectedState& State =
					CandidateFrame.ExpectedStates.AddDefaulted_GetRef();
				State.TimeSeconds = Expected.TimeSeconds;
				State.PlayerId = PlayerId;
				State.Location = ExpectedLocation;
			}
		}

		while (CandidateScreenshotIndex < Recording->Screenshots.Num()
			&& Recording->Screenshots[CandidateScreenshotIndex].TimeSeconds
				<= ElapsedSeconds + KINDA_SMALL_NUMBER)
		{
			CandidateFrame.Screenshots.Add(
				Recording->Screenshots[CandidateScreenshotIndex++]);
		}
		const bool bReachedCompletion =
			ElapsedSeconds
				>= Recording->DurationSeconds
					+ Settings.CompletionGraceSeconds
			&& CandidateOperationIndex >= Recording->Operations.Num()
			&& CandidateStateIndex >= Recording->States.Num()
			&& CandidateScreenshotIndex >= Recording->Screenshots.Num();
		CandidateFrame.bComplete =
			bReachedCompletion && !bCompletionReported;

		NextOperationIndex = CandidateOperationIndex;
		NextStateIndex = CandidateStateIndex;
		NextScreenshotIndex = CandidateScreenshotIndex;
		LastElapsedSeconds = ElapsedSeconds;
		bCompletionReported |= bReachedCompletion;
		MovementByPlayer = MoveTemp(CandidateMovements);
		OutFrame = MoveTemp(CandidateFrame);
		return true;
	}
}
