#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Fragment/FragmentTypes.h"
#include "Fragment2DAsset.generated.h"

UCLASS(BlueprintType)
class MATTERFLUX_API UFragment2DAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment|Mask", meta = (ClampMin = "1", ClampMax = "256"))
	int32 MaskWidth = 128;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment|Mask", meta = (ClampMin = "1", ClampMax = "256"))
	int32 MaskHeight = 128;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment|Mask", meta = (ClampMin = "1.0"))
	float CellSize = 10.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment|Mask", meta = (ClampMin = "1"))
	int32 MinFragmentAreaPixels = 16;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment|Mask", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxFragmentsPerBreak = 16;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment|Mask")
	EFragmentSupportMode SupportMode = EFragmentSupportMode::Bottom;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment|Mask")
	TArray<uint8> SolidMask;

	void BuildInitialMask(TArray<uint8>& OutMask) const;
	int32 GetClampedWidth() const;
	int32 GetClampedHeight() const;
	float GetClampedCellSize() const;
};
