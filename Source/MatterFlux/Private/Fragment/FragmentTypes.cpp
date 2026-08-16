#include "Fragment/FragmentTypes.h"

bool FFragmentSourceMask::NetSerialize(
	FArchive& Ar,
	UPackageMap* Map,
	bool& bOutSuccess)
{
	bOutSuccess = false;
	if (Ar.IsSaving() && !HasValidLayout())
	{
		return false;
	}

	Ar << Width;
	Ar << Height;
	Ar << CellSize;
	Ar << MinFragmentAreaPixels;
	Ar << MaxFragmentsPerBreak;
	uint8 SupportModeValue = static_cast<uint8>(SupportMode);
	Ar << SupportModeValue;
	if (Ar.IsLoading())
	{
		SupportMode =
			static_cast<EFragmentSupportMode>(SupportModeValue);
	}

	const bool bMetadataValid =
		Width > 0 && Width <= 256
		&& Height > 0 && Height <= 256
		&& FMath::IsFinite(CellSize) && CellSize > 0.0f
		&& MinFragmentAreaPixels > 0
		&& MaxFragmentsPerBreak > 0
		&& MaxFragmentsPerBreak <= 16
		&& (SupportMode == EFragmentSupportMode::None
			|| SupportMode == EFragmentSupportMode::Bottom);
	if (Ar.IsError() || !bMetadataValid)
	{
		if (Ar.IsLoading())
		{
			*this = FFragmentSourceMask();
		}
		return false;
	}

	const int32 CellCount = Width * Height;
	const int32 PackedByteCount = (CellCount + 7) / 8;
	TArray<uint8> PackedMask;
	PackedMask.SetNumZeroed(PackedByteCount);
	if (Ar.IsSaving())
	{
		for (int32 CellIndex = 0;
			CellIndex < CellCount;
			++CellIndex)
		{
			PackedMask[CellIndex >> 3] |=
				(SolidMask[CellIndex] & 1u)
				<< (CellIndex & 7);
		}
	}

	Ar.SerializeBits(PackedMask.GetData(), CellCount);
	if (Ar.IsLoading() && !Ar.IsError())
	{
		SolidMask.SetNumUninitialized(CellCount);
		for (int32 CellIndex = 0;
			CellIndex < CellCount;
			++CellIndex)
		{
			SolidMask[CellIndex] =
				(PackedMask[CellIndex >> 3]
					>> (CellIndex & 7))
				& 1u;
		}
	}

	bOutSuccess = !Ar.IsError() && HasValidLayout();
	if (!bOutSuccess && Ar.IsLoading())
	{
		*this = FFragmentSourceMask();
	}
	return bOutSuccess;
}
