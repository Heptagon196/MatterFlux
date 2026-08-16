#pragma once

#include "CoreMinimal.h"
#include "FragmentTypes.generated.h"

class UPackageMap;

UENUM(BlueprintType)
enum class EFragmentDamageShapeType : uint8
{
	Circle,
	Box,
	Line
};

UENUM(BlueprintType)
enum class EFragmentSupportMode : uint8
{
	None,
	Bottom
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FFragmentDamageShape
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	EFragmentDamageShapeType Type = EFragmentDamageShapeType::Circle;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FTransform WorldTransform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FVector2D Extents = FVector2D(100.0, 100.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	float Radius = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	float Thickness = 20.0f;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FFragmentDamageEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FGuid SourceId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 BaseRevision = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FFragmentDamageShape DamageShape;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	float DamagePower = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 EventSeed = 1337;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FFragmentWorldCutRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FFragmentDamageShape CutShape;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	float DamagePower = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 EventSeed = 1337;

	// Extra broad-phase reach around the exact cut shape. This does not change
	// the cells removed from a source.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	float TargetPadding = 0.0f;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FFragmentContour
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	TArray<FVector2D> Vertices;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FFragmentSourceMask
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 Width = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 Height = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	float CellSize = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 MinFragmentAreaPixels = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 MaxFragmentsPerBreak = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	EFragmentSupportMode SupportMode =
		EFragmentSupportMode::Bottom;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	TArray<uint8> SolidMask;

	bool NetSerialize(
		FArchive& Ar,
		UPackageMap* Map,
		bool& bOutSuccess);

	bool HasValidLayout() const
	{
		if (Width <= 0 || Width > 256
			|| Height <= 0 || Height > 256
			|| !FMath::IsFinite(CellSize) || CellSize <= 0.0f
			|| MinFragmentAreaPixels <= 0
			|| MaxFragmentsPerBreak <= 0 || MaxFragmentsPerBreak > 16
			|| (SupportMode != EFragmentSupportMode::None
				&& SupportMode != EFragmentSupportMode::Bottom)
			|| static_cast<int64>(SolidMask.Num())
				!= static_cast<int64>(Width) * static_cast<int64>(Height))
		{
			return false;
		}

		for (const uint8 Cell : SolidMask)
		{
			if (Cell > 1)
			{
				return false;
			}
		}
		return true;
	}

	bool IsValid() const
	{
		return HasValidLayout() && SolidMask.Contains(1);
	}
};

template<>
struct TStructOpsTypeTraits<FFragmentSourceMask>
	: public TStructOpsTypeTraitsBase2<FFragmentSourceMask>
{
	enum
	{
		WithNetSerializer = true
	};
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FFragmentSpawnPayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FGuid FragmentId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	int32 Revision = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	TArray<FFragmentContour> OuterContours;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	TArray<FFragmentContour> HoleContours;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	TArray<int32> TriangleIndices;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	TArray<FVector2D> Vertices2D;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	TArray<FFragmentContour> CollisionContours;

	/**
	 * Preserves the source's collision policy after detachment. Decorative
	 * sources can still produce visible debris without synchronously cooking a
	 * Chaos body or unexpectedly becoming gameplay blockers.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	bool bEnableCollision = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	float Thickness = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FTransform InitialTransform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FVector InitialLinearVelocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	FVector InitialAngularVelocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment")
	float Mass = 1.0f;
};
