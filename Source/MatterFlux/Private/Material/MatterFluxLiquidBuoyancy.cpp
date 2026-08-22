#include "Material/MatterFluxLiquidBuoyancy.h"

namespace MatterFlux::Liquid
{
	bool FLiquidColumn::IsValid() const
	{
		return !MaterialId.IsNone()
			&& FMath::IsFinite(Density)
			&& Density > 0.0f
			&& FMath::IsFinite(BottomZ)
			&& FMath::IsFinite(SurfaceZ)
			&& SurfaceZ > BottomZ
			&& !FlowVelocity.ContainsNaN();
	}

	bool FBodyState::IsValid() const
	{
		return FMath::IsFinite(Density)
			&& Density > 0.0f
			&& FMath::IsFinite(BottomZ)
			&& FMath::IsFinite(TopZ)
			&& TopZ > BottomZ
			&& FMath::IsFinite(GravityZ)
			&& GravityZ < 0.0f
			&& FMath::IsFinite(LinearDrag)
			&& LinearDrag >= 0.0f
			&& !Velocity.ContainsNaN();
	}

	bool FLiquidBuoyancySolver::Evaluate(
		const FBodyState& Body,
		const FLiquidColumn& Liquid,
		FBuoyancyResult& OutResult)
	{
		OutResult = {};
		if (!Body.IsValid() || !Liquid.IsValid())
		{
			return false;
		}

		const float BodyHeight = Body.TopZ - Body.BottomZ;
		const float SubmergedHeight = FMath::Max(
			0.0f,
			FMath::Min(Body.TopZ, Liquid.SurfaceZ)
				- FMath::Max(Body.BottomZ, Liquid.BottomZ));
		if (SubmergedHeight <= UE_SMALL_NUMBER)
		{
			return true;
		}

		OutResult.bSubmerged = true;
		OutResult.SubmergedFraction = FMath::Clamp(
			SubmergedHeight / BodyHeight,
			0.0f,
			1.0f);
		OutResult.LiquidDensity = Liquid.Density;
		const float BuoyantAcceleration = -Body.GravityZ
			* (Liquid.Density / Body.Density)
			* OutResult.SubmergedFraction;
		const FVector RelativeVelocity =
			Body.Velocity - Liquid.FlowVelocity;
		const FVector DragAcceleration = -RelativeVelocity
			* Body.LinearDrag
			* OutResult.SubmergedFraction;
		OutResult.Acceleration =
			DragAcceleration
				+ FVector(0.0f, 0.0f, BuoyantAcceleration);
		return true;
	}
}
