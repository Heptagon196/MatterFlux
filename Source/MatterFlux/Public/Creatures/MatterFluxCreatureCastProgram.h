#pragma once

#include "CoreMinimal.h"
#include "MatterFluxContentTypes.h"

struct MATTERFLUX_API FMatterFluxCreatureCastShot
{
	FVector Direction = FVector::ForwardVector;
	float DelaySeconds = 0.0f;
	int32 EventSeed = 0;

	bool operator==(const FMatterFluxCreatureCastShot& Other) const
	{
		return Direction == Other.Direction
			&& DelaySeconds == Other.DelaySeconds
			&& EventSeed == Other.EventSeed;
	}
};

/** Builds deterministic projectile directions and timing from Lua cast data. */
class MATTERFLUX_API FMatterFluxCreatureCastPlanner
{
public:
	static bool Build(
		const FMatterFluxCreatureCastProgramDefinition& Program,
		const FVector& AimDirection,
		int32 EventSeed,
		TArray<FMatterFluxCreatureCastShot>& OutShots,
		FString& OutError);
};
