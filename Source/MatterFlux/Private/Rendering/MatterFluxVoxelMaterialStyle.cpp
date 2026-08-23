#include "Rendering/MatterFluxVoxelMaterialStyle.h"

#include "Materials/MaterialInstanceDynamic.h"

namespace MatterFlux::Rendering
{
	UMaterialInterface* ResolveDynamicMaterialParent(
		UMaterialInterface* Candidate)
	{
		if (UMaterialInstanceDynamic* Dynamic =
			Cast<UMaterialInstanceDynamic>(Candidate))
		{
			return Dynamic->GetMaterial();
		}
		return Candidate;
	}

	FVoxelMaterialStyle ResolveVoxelMaterialStyle(const FName MaterialId)
	{
		FVoxelMaterialStyle Style;
		if (MaterialId == TEXT("wood"))
		{
			Style.FaceContrast = 0.68f;
			Style.ShadowLift = 0.34f;
			Style.SideBrightness = 1.0f;
		}
		else if (MaterialId == TEXT("fabric"))
		{
			Style.FaceContrast = 0.58f;
			Style.ColorVariation = 0.018f;
			Style.Roughness = 0.94f;
			Style.ShadowLift = 0.38f;
			Style.SideBrightness = 0.90f;
		}
		else if (MaterialId == TEXT("leaf"))
		{
			Style.FaceContrast = 0.50f;
			Style.ColorVariation = 0.025f;
			Style.Roughness = 0.88f;
			Style.ShadowLift = 0.46f;
			Style.SideBrightness = 0.86f;
		}
		else if (MaterialId == TEXT("grass"))
		{
			Style.FaceContrast = 0.52f;
			Style.ShadowLift = 0.28f;
			Style.SideBrightness = 0.88f;
		}
		else if (MaterialId == TEXT("flower_pink")
			|| MaterialId == TEXT("flower_gold")
			|| MaterialId == TEXT("flower_blue"))
		{
			Style.FaceContrast = 0.42f;
			Style.ColorVariation = 0.012f;
			Style.ShadowLift = 0.38f;
			Style.SideBrightness = 0.88f;
		}
		else if (MaterialId == TEXT("stone"))
		{
			Style.FaceContrast = 0.72f;
			Style.Roughness = 0.96f;
			Style.ShadowLift = 0.12f;
		}
		else if (MaterialId == TEXT("roof"))
		{
			Style.FaceContrast = 0.90f;
			Style.ColorVariation = 0.008f;
			Style.ShadowLift = 0.30f;
			Style.SideBrightness = 0.94f;
		}
		return Style;
	}

	void ApplyVoxelMaterialStyle(
		UMaterialInstanceDynamic& Material,
		const FName MaterialId,
		const float CellSize)
	{
		const FVoxelMaterialStyle Style =
			ResolveVoxelMaterialStyle(MaterialId);
		Material.SetScalarParameterValue(
			TEXT("FaceContrast"), Style.FaceContrast);
		Material.SetScalarParameterValue(
			TEXT("ColorVariation"), Style.ColorVariation);
		Material.SetScalarParameterValue(
			TEXT("PixelSize"), FMath::Max(CellSize, 4.0f));
		Material.SetScalarParameterValue(TEXT("Roughness"), Style.Roughness);
		Material.SetScalarParameterValue(TEXT("ShadowLift"), Style.ShadowLift);
	}

	FLinearColor ResolveVoxelMaterialColor(
		const FLinearColor& BaseColor,
		const FName MaterialId,
		const bool bSide)
	{
		if (!bSide)
		{
			return BaseColor;
		}
		const float Brightness =
			ResolveVoxelMaterialStyle(MaterialId).SideBrightness;
		return FLinearColor(
			BaseColor.R * Brightness,
			BaseColor.G * Brightness,
			BaseColor.B * Brightness,
			BaseColor.A);
	}

	FVoxelMaterialProjection ResolveVoxelMaterialProjection(
		const FLinearColor& BaseColor,
		const FName MaterialId,
		const float CellSize,
		const EVoxelMaterialFaceRole FaceRole)
	{
		FVoxelMaterialProjection Projection;
		Projection.MaterialId = MaterialId;
		Projection.BaseColor = BaseColor;
		Projection.CellSize = FMath::Max(CellSize, 1.0f);
		Projection.FaceRole = FaceRole;
		Projection.Style = ResolveVoxelMaterialStyle(MaterialId);
		Projection.ResolvedColor = ResolveVoxelMaterialColor(
			BaseColor,
			MaterialId,
			FaceRole == EVoxelMaterialFaceRole::Side);
		return Projection;
	}

	void ApplyVoxelMaterialProjection(
		UMaterialInstanceDynamic& Material,
		const FVoxelMaterialProjection& Projection,
		const float Opacity)
	{
		const float SafeOpacity = FMath::Clamp(Opacity, 0.0f, 1.0f);
		FLinearColor Color = Projection.ResolvedColor;
		Color.A *= SafeOpacity;
		Material.SetVectorParameterValue(TEXT("Color"), Color);
		Material.SetScalarParameterValue(TEXT("Opacity"), SafeOpacity);
		Material.SetScalarParameterValue(
			TEXT("FaceContrast"), Projection.Style.FaceContrast);
		Material.SetScalarParameterValue(
			TEXT("ColorVariation"), Projection.Style.ColorVariation);
		Material.SetScalarParameterValue(
			TEXT("PixelSize"), FMath::Max(Projection.CellSize, 4.0f));
		Material.SetScalarParameterValue(
			TEXT("Roughness"), Projection.Style.Roughness);
		Material.SetScalarParameterValue(
			TEXT("ShadowLift"), Projection.Style.ShadowLift);
	}
}
