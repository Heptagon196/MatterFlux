#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;

namespace MatterFlux::Rendering
{
	enum class EVoxelMaterialFaceRole : uint8
	{
		Primary,
		Side
	};

	/**
	 * Canonical visual projection for one simulated material.
	 *
	 * Source actors, chunk proxies, whole-object batches and detached rigid
	 * bodies must resolve the same values.  A representation may change the
	 * mesh, but it must not invent a second material appearance.
	 */
	struct FVoxelMaterialStyle
	{
		float FaceContrast = 0.70f;
		float ColorVariation = 0.03f;
		float Roughness = 0.82f;
		float ShadowLift = 0.18f;
		float SideBrightness = 0.78f;
	};

	/**
	 * Immutable render input resolved from the authoritative material fact.
	 * Mesh components and rigid-body actors may own different MIDs, but they
	 * must build those MIDs from this value instead of inventing defaults.
	 */
	struct FVoxelMaterialProjection
	{
		FName MaterialId = NAME_None;
		FLinearColor BaseColor = FLinearColor::White;
		FLinearColor ResolvedColor = FLinearColor::White;
		float CellSize = 1.0f;
		EVoxelMaterialFaceRole FaceRole =
			EVoxelMaterialFaceRole::Primary;
		FVoxelMaterialStyle Style;
	};

	MATTERFLUX_API FVoxelMaterialStyle ResolveVoxelMaterialStyle(
		FName MaterialId);

	/**
	 * Returns a legal parent for a new dynamic projection. Unreal does not
	 * support MID-to-MID inheritance, so a stale render projection must be
	 * collapsed back to its underlying material before rebuilding it.
	 */
	MATTERFLUX_API UMaterialInterface* ResolveDynamicMaterialParent(
		UMaterialInterface* Candidate);

	MATTERFLUX_API void ApplyVoxelMaterialStyle(
		UMaterialInstanceDynamic& Material,
		FName MaterialId,
		float CellSize);

	MATTERFLUX_API FLinearColor ResolveVoxelMaterialColor(
		const FLinearColor& BaseColor,
		FName MaterialId,
		bool bSide);

	MATTERFLUX_API FVoxelMaterialProjection ResolveVoxelMaterialProjection(
		const FLinearColor& BaseColor,
		FName MaterialId,
		float CellSize,
		EVoxelMaterialFaceRole FaceRole);

	MATTERFLUX_API void ApplyVoxelMaterialProjection(
		UMaterialInstanceDynamic& Material,
		const FVoxelMaterialProjection& Projection,
		float Opacity = 1.0f);
}
