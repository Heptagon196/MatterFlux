#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::GhostFade
{
	/** Keeps item occluders readable instead of making them nearly disappear. */
	inline constexpr float MinimumOpacity = 0.24f;
	inline constexpr float DefaultItemOpacity = 0.30f;
	inline constexpr float FadeToGhostSpeed = 2.4f;
	inline constexpr float FadeToSolidSpeed = 3.0f;

	/** Houses author lower targets because several translucent walls overlap. */
	inline float ResolveStructureTargetOpacity(const float RequestedOpacity)
	{
		return FMath::Clamp(RequestedOpacity, 0.0f, 1.0f);
	}

	inline float AdvanceOpacity(
		const float CurrentOpacity,
		const float TargetOpacity,
		const float DeltaSeconds,
		const float FadeSpeed)
	{
		return FMath::FInterpConstantTo(
			FMath::Clamp(CurrentOpacity, 0.0f, 1.0f),
			FMath::Clamp(TargetOpacity, 0.0f, 1.0f),
			FMath::Max(DeltaSeconds, 0.0f),
			FMath::Max(FadeSpeed, 0.0f));
	}

	inline float AdvanceItemOpacity(
		const float CurrentOpacity,
		const bool bGhostDesired,
		const float DeltaSeconds)
	{
		return AdvanceOpacity(
			CurrentOpacity,
			bGhostDesired ? DefaultItemOpacity : 1.0f,
			DeltaSeconds,
			bGhostDesired ? FadeToGhostSpeed : FadeToSolidSpeed);
	}
}
