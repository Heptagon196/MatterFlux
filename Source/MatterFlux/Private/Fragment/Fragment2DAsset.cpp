#include "Fragment/Fragment2DAsset.h"

void UFragment2DAsset::BuildInitialMask(TArray<uint8>& OutMask) const
{
	const int32 Width = GetClampedWidth();
	const int32 Height = GetClampedHeight();
	const int32 ExpectedNum = Width * Height;

	OutMask.Reset(ExpectedNum);
	OutMask.SetNum(ExpectedNum);

	if (SolidMask.Num() == ExpectedNum)
	{
		for (int32 Index = 0; Index < ExpectedNum; ++Index)
		{
			OutMask[Index] = SolidMask[Index] != 0 ? 1 : 0;
		}
		return;
	}

	for (uint8& Cell : OutMask)
	{
		Cell = 1;
	}
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 2D0774436990047B */ int32 UFragment2DAsset::GetClampedWidth() const
{
	return FMath::Clamp(MaskWidth, 1, 256);
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE FECE7216CB6C84BC */ int32 UFragment2DAsset::GetClampedHeight() const
{
	return FMath::Clamp(MaskHeight, 1, 256);
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE EE7D2ECAB0D592CE */ float UFragment2DAsset::GetClampedCellSize() const
{
	return FMath::IsFinite(CellSize)
		? FMath::Max(CellSize, 1.0f)
		: 1.0f;
}
