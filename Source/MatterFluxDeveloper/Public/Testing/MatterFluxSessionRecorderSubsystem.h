#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Testing/MatterFluxReplayRuntime.h"
#include "Testing/MatterFluxSessionRecordingTypes.h"
#include "MatterFluxSessionRecorderSubsystem.generated.h"

class AMatterFluxCharacter;

UCLASS()
class MATTERFLUXDEVELOPER_API UMatterFluxSessionRecorderSubsystem final
	: public UGameInstanceSubsystem
	, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	virtual UWorld* GetTickableGameObjectWorld() const override;

	bool IsRecording() const { return Options.bRecord && !Options.bReplay; }
	bool IsReplaying() const { return Options.bReplay; }
	bool IsActive() const { return IsRecording() || IsReplaying(); }
	bool HasSessionStarted() const { return bSessionStarted; }
	const FMatterFluxSessionRecording& GetRecording() const
	{
		return Recording;
	}
	const FString& GetOutputFilePath() const { return OutputFilePath; }
	bool DidReplayPass() const { return bReplayPassed; }

	bool RecordPlayerOperation(
		AMatterFluxCharacter* Character,
		EMatterFluxPlayerOperation Operation,
		const FVector2D& Value = FVector2D::ZeroVector,
		int32 IntegerValue = 0);
	void RequestScreenshot(const FString& Label);
	bool FlushRecording(FString* OutError = nullptr);

private:
	void HandlePlayerOperation(
		AMatterFluxCharacter& Character,
		EMatterFluxPlayerOperation Operation,
		FVector2D Value,
		int32 IntegerValue,
		bool bRelayedFromClient);
	void HandlePreExit();
	bool BeginSession(UWorld& World);
	void TickRecording(UWorld& World, double ElapsedSeconds);
	void TickReplay(UWorld& World, double ElapsedSeconds);
	void SamplePlayerStates(UWorld& World, double ElapsedSeconds);
	void CaptureDueScreenshots(UWorld& World, double ElapsedSeconds);
	void CaptureScreenshot(
		UWorld& World,
		const FMatterFluxRecordedScreenshot& Screenshot,
		bool bReplayCapture);
	TOptional<int32> ResolvePlayerId(AMatterFluxCharacter& Character);
	AMatterFluxCharacter* FindPlayerById(UWorld& World, int32 PlayerId) const;
	void ApplyReplayOperation(
		UWorld& World,
		const FMatterFluxRecordedOperation& Operation);
	void VerifyReplayExpectedState(
		UWorld& World,
		const MatterFlux::Recording::FReplayExpectedState& Expected);
	void FinishReplay();
	FString BuildOutputFilePath() const;

	FMatterFluxRecordingLaunchOptions Options;
	FMatterFluxSessionRecording Recording;
	MatterFlux::Recording::FReplayRuntime ReplayRuntime;
	double SessionWorldStartSeconds = 0.0;
	double NextStateSampleSeconds = 0.0;
	int32 NextRecordingScreenshotIndex = 0;
	bool bSessionStarted = false;
	bool bFlushed = false;
	bool bReplayPassed = true;
	bool bReplayFinished = false;
	FString OutputFilePath;
	TMap<int32, FVector2D> LastRecordedMovementByPlayer;
	TMap<int32, TWeakObjectPtr<AMatterFluxCharacter>> ReplayPlayers;
	FDelegateHandle PlayerOperationHandle;
	FDelegateHandle PreExitHandle;
};
