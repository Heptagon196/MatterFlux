#pragma once

#include "CoreMinimal.h"
#include "Game/MatterFluxPlayerOperation.h"
#include "MatterFluxSessionRecordingTypes.generated.h"

using EMatterFluxRecordedOperation = EMatterFluxPlayerOperation;

USTRUCT()
struct MATTERFLUXDEVELOPER_API FMatterFluxRecordedPlayer
{
	GENERATED_BODY()

	UPROPERTY()
	int32 PlayerId = INDEX_NONE;

	UPROPERTY()
	FString PlayerName;
};

USTRUCT()
struct MATTERFLUXDEVELOPER_API FMatterFluxRecordedOperation
{
	GENERATED_BODY()

	UPROPERTY()
	double TimeSeconds = 0.0;

	UPROPERTY()
	int32 PlayerId = INDEX_NONE;

	UPROPERTY()
	EMatterFluxPlayerOperation Operation =
		EMatterFluxPlayerOperation::Move;

	UPROPERTY()
	FVector2D Value = FVector2D::ZeroVector;

	UPROPERTY()
	int32 IntegerValue = 0;
};

USTRUCT()
struct MATTERFLUXDEVELOPER_API FMatterFluxRecordedPlayerState
{
	GENERATED_BODY()

	UPROPERTY()
	double TimeSeconds = 0.0;

	UPROPERTY()
	int32 PlayerId = INDEX_NONE;

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY()
	uint8 MovementMode = 0;
};

USTRUCT()
struct MATTERFLUXDEVELOPER_API FMatterFluxRecordedScreenshot
{
	GENERATED_BODY()

	UPROPERTY()
	double TimeSeconds = 0.0;

	UPROPERTY()
	FString Label;
};

USTRUCT()
struct MATTERFLUXDEVELOPER_API FMatterFluxSessionRecording
{
	GENERATED_BODY()

	static constexpr int32 LatestVersion = 1;

	UPROPERTY()
	int32 Version = LatestVersion;

	UPROPERTY()
	FString CreatedUtc;

	UPROPERTY()
	FString EngineVersion;

	UPROPERTY()
	FString MapName;

	UPROPERTY()
	int32 WorldSeed = 0;

	UPROPERTY()
	double DurationSeconds = 0.0;

	UPROPERTY()
	double StateIntervalSeconds = 0.1;

	UPROPERTY()
	TArray<FMatterFluxRecordedPlayer> Players;

	UPROPERTY()
	TArray<FMatterFluxRecordedOperation> Operations;

	UPROPERTY()
	TArray<FMatterFluxRecordedPlayerState> States;

	UPROPERTY()
	TArray<FMatterFluxRecordedScreenshot> Screenshots;
};

struct MATTERFLUXDEVELOPER_API FMatterFluxRecordingLaunchOptions
{
	bool bRecord = false;
	bool bReplay = false;
	bool bQuitAfterRecord = false;
	bool bQuitAfterReplay = false;
	bool bVerifyReplayStates = true;
	FString RecordDirectory;
	FString RecordName;
	FString ReplayFile;
	FString ReplayOutputDirectory;
	double StateIntervalSeconds = 0.1;
	double ReplayLocationTolerance = 30.0;
	double RecordDurationSeconds = 0.0;
	int32 RequestedSeed = 0;
	TArray<FMatterFluxRecordedScreenshot> ScheduledScreenshots;
};

namespace MatterFlux::Recording
{
	MATTERFLUXDEVELOPER_API FString OperationToString(
		EMatterFluxRecordedOperation Operation);
	MATTERFLUXDEVELOPER_API bool TryParseOperation(
		const FString& Text,
		EMatterFluxRecordedOperation& OutOperation);
	MATTERFLUXDEVELOPER_API bool ParseLaunchOptions(
		const TCHAR* CommandLine,
		FMatterFluxRecordingLaunchOptions& OutOptions,
		FString& OutError);
	MATTERFLUXDEVELOPER_API bool SaveToJson(
		const FMatterFluxSessionRecording& Recording,
		FString& OutJson,
		FString& OutError);
	MATTERFLUXDEVELOPER_API bool LoadFromJson(
		const FString& Json,
		FMatterFluxSessionRecording& OutRecording,
		FString& OutError);
}
