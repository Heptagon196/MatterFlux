#pragma once

#include "CoreMinimal.h"
#include "Testing/MatterFluxSessionRecordingTypes.h"

namespace MatterFlux::Recording
{
	struct MATTERFLUXDEVELOPER_API FReplayRuntimeSettings
	{
		bool bVerifyStates = true;
		double CompletionGraceSeconds = 1.0;
	};

	struct MATTERFLUXDEVELOPER_API FReplayMovement
	{
		int32 PlayerId = INDEX_NONE;
		FVector2D Value = FVector2D::ZeroVector;
	};

	struct MATTERFLUXDEVELOPER_API FReplayExpectedState
	{
		double TimeSeconds = 0.0;
		int32 PlayerId = INDEX_NONE;
		FVector Location = FVector::ZeroVector;
	};

	struct MATTERFLUXDEVELOPER_API FReplayFrame
	{
		TArray<FMatterFluxRecordedOperation> Operations;
		TArray<FReplayMovement> Movements;
		TArray<FReplayExpectedState> ExpectedStates;
		TArray<FMatterFluxRecordedScreenshot> Screenshots;
		bool bComplete = false;

		void Reset();
	};

	/**
	 * Deterministically consumes an immutable recording timeline. The recording
	 * passed to Initialize must keep the same address and contents until the
	 * runtime is initialized again or destroyed.
	 */
	class MATTERFLUXDEVELOPER_API FReplayRuntime
	{
	public:
		bool Initialize(
			const FMatterFluxSessionRecording& InRecording,
			const FReplayRuntimeSettings& InSettings,
			FString& OutError);
		bool Advance(
			double ElapsedSeconds,
			FReplayFrame& OutFrame,
			FString& OutError);

	private:
		const FMatterFluxSessionRecording* Recording = nullptr;
		FReplayRuntimeSettings Settings;
		int32 NextOperationIndex = 0;
		int32 NextStateIndex = 0;
		int32 NextScreenshotIndex = 0;
		double LastElapsedSeconds = 0.0;
		bool bCompletionReported = false;
		TMap<int32, FVector2D> MovementByPlayer;
	};
}
