#pragma once

#include "CoreMinimal.h"
#include "MatterFluxContentTypes.h"

namespace MatterFlux::Material
{
	/** Canonical contact volume represented by one 2.5D material cell. */
	struct MATTERFLUX_API FMaterialContactGeometry
	{
		float ColumnHeight = 0.0f;
		float CenterOffsetZ = 0.0f;
		FVector HalfExtent = FVector::ZeroVector;
		float RadialContactRadius = 0.0f;
		float GroundVerticalTolerance = 0.0f;

		bool IsValid() const;
	};

	MATTERFLUX_API FMaterialContactGeometry BuildMaterialContactGeometry(
		EMatterFluxMaterialPhase Phase,
		float CellSize,
		float LiquidColumnHeight);
}
