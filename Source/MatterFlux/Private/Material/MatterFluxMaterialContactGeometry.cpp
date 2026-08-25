#include "Material/MatterFluxMaterialContactGeometry.h"

namespace MatterFlux::Material
{
	bool FMaterialContactGeometry::IsValid() const
	{
		return FMath::IsFinite(ColumnHeight)
			&& FMath::IsFinite(CenterOffsetZ)
			&& !HalfExtent.ContainsNaN()
			&& FMath::IsFinite(RadialContactRadius)
			&& FMath::IsFinite(GroundVerticalTolerance)
			&& ColumnHeight > 0.0f
			&& CenterOffsetZ > 0.0f
			&& HalfExtent.GetMin() > 0.0f
			&& RadialContactRadius > 0.0f
			&& GroundVerticalTolerance > 0.0f;
	}

	FMaterialContactGeometry BuildMaterialContactGeometry(
		const EMatterFluxMaterialPhase Phase,
		const float CellSize,
		const float LiquidColumnHeight)
	{
		if (!FMath::IsFinite(CellSize)
			|| !FMath::IsFinite(LiquidColumnHeight)
			|| CellSize <= 0.0f
			|| LiquidColumnHeight <= 0.0f)
		{
			return {};
		}

		const bool bUsesLiquidColumn =
			Phase == EMatterFluxMaterialPhase::Liquid;
		FMaterialContactGeometry Result;
		Result.ColumnHeight = bUsesLiquidColumn
			? LiquidColumnHeight
			: CellSize;
		Result.CenterOffsetZ = Result.ColumnHeight * 0.5f;
		Result.HalfExtent = FVector(
			CellSize * 0.52f,
			CellSize * 0.52f,
			Result.ColumnHeight * 0.5f);
		Result.RadialContactRadius = FMath::Max(
			CellSize * 0.52f,
			Result.ColumnHeight * 0.5f);
		Result.GroundVerticalTolerance = bUsesLiquidColumn
			? FMath::Max(CellSize, LiquidColumnHeight)
			: CellSize;
		return Result;
	}
}
