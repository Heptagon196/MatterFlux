#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::Liquid
{
	/** One vertical liquid column sampled from the material world. */
	struct FLiquidColumn
	{
		FName MaterialId = NAME_None;
		float Density = 0.0f;
		float BottomZ = 0.0f;
		float SurfaceZ = 0.0f;
		FVector FlowVelocity = FVector::ZeroVector;

		bool IsValid() const;
	};

	/** Minimal body data needed by the deterministic buoyancy solver. */
	struct FBodyState
	{
		float Density = 1.0f;
		float BottomZ = 0.0f;
		float TopZ = 0.0f;
		float GravityZ = -980.0f;
		float LinearDrag = 2.4f;
		FVector Velocity = FVector::ZeroVector;

		bool IsValid() const;
	};

	struct FBuoyancyResult
	{
		bool bSubmerged = false;
		float SubmergedFraction = 0.0f;
		float LiquidDensity = 0.0f;
		FVector Acceleration = FVector::ZeroVector;
	};

	/**
	 * Pure Archimedes/drag calculation shared by characters and Chaos bodies.
	 * Gravity itself remains owned by CharacterMovement or Chaos; Acceleration
	 * contains only the additional liquid force.
	 */
	class MATTERFLUX_API FLiquidBuoyancySolver
	{
	public:
		static bool Evaluate(
			const FBodyState& Body,
			const FLiquidColumn& Liquid,
			FBuoyancyResult& OutResult);
	};
}
