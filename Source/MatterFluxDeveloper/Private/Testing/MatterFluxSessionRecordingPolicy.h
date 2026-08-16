#pragma once

// Validation limits shared only by the developer recording adapter.

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/Paths.h"
#include "Testing/MatterFluxSessionRecordingTypes.h"

namespace MatterFlux::Recording::Private
{
	inline constexpr const TCHAR* Schema =
		TEXT("MatterFluxSessionRecording");
	inline constexpr int32 MaxPlayers = 64;
	inline constexpr int32 MaxOperations = 1000000;
	inline constexpr int32 MaxStates = 2000000;
	inline constexpr int32 MaxScreenshots = 10000;
	inline constexpr int64 MaxFileBytes = 256ll * 1024ll * 1024ll;
	inline constexpr int32 MaxJsonCharacters = 256 * 1024 * 1024;
	inline constexpr int32 MaxMetadataCharacters = 1024;
	inline constexpr int32 MaxPlayerNameCharacters = 128;
	inline constexpr int32 MaxScreenshotLabelCharacters = 128;
	inline constexpr double MaxTimeSeconds =
		366.0 * 24.0 * 60.0 * 60.0;

	inline bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	}

	inline bool IsFiniteVector(const FVector2D& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
	}

	inline bool IsValidOperationPayload(
		const FMatterFluxRecordedOperation& Operation)
	{
		return Operation.Operation
				<= EMatterFluxRecordedOperation::CastWand
			&& FMath::IsFinite(Operation.TimeSeconds)
			&& Operation.TimeSeconds >= 0.0
			&& Operation.TimeSeconds <= MaxTimeSeconds
			&& IsFiniteVector(Operation.Value)
			&& FMath::Abs(Operation.Value.X) <= 100.0
			&& FMath::Abs(Operation.Value.Y) <= 100.0
			&& (Operation.Operation
					!= EMatterFluxRecordedOperation::Regenerate
				|| Operation.IntegerValue >= 0)
			&& (Operation.Operation
					!= EMatterFluxRecordedOperation::CastWand
				|| (Operation.IntegerValue >= 0
					&& Operation.IntegerValue < 4));
	}

	inline bool IsValidStatePayload(
		const FMatterFluxRecordedPlayerState& State)
	{
		return FMath::IsFinite(State.TimeSeconds)
			&& State.TimeSeconds >= 0.0
			&& State.TimeSeconds <= MaxTimeSeconds
			&& IsFiniteVector(State.Location)
			&& FMath::IsFinite(State.Rotation.Pitch)
			&& FMath::IsFinite(State.Rotation.Yaw)
			&& FMath::IsFinite(State.Rotation.Roll)
			&& IsFiniteVector(State.Velocity)
			&& State.MovementMode < MOVE_MAX;
	}

	inline FString SanitizeScreenshotLabel(const FString& Label)
	{
		FString Result = Label.IsEmpty()
			? TEXT("Screenshot")
			: Label.Left(MaxScreenshotLabelCharacters);
		Result.ReplaceInline(TEXT(" "), TEXT("_"));
		Result = FPaths::MakeValidFileName(Result);
		return Result.IsEmpty() ? TEXT("Screenshot") : Result;
	}

	template<typename ItemType>
	void SortByTime(TArray<ItemType>& Items)
	{
		Items.StableSort(
			[](const ItemType& A, const ItemType& B)
			{
				return A.TimeSeconds < B.TimeSeconds;
			});
	}
}
