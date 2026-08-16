#pragma once

#include "CoreMinimal.h"
#include "MatterFluxContentTypes.h"

namespace MatterFlux::Material
{
	struct FWorldSettings
	{
		int32 ChunkSize = 64;
		int32 ActiveChunkRadius = 2;
		int32 MaxActiveChunks = 25;
		int32 MinWorldHeightCells = -1024;
		int32 MaxWorldHeightCells = 1024;
		bool bCullOutsideVerticalBounds = true;
		bool bUseSurfaceTopology = false;
		FIntPoint MinSurfaceCell = FIntPoint(-1024, -1024);
		FIntPoint MaxSurfaceCellExclusive = FIntPoint(1024, 1024);
		bool bCullOutsideSurfaceBounds = true;

		bool IsValid() const;
	};

	struct FStepStats
	{
		int32 MovedCells = 0;
		int32 ReactedPairs = 0;
		int32 CulledCells = 0;
		int32 VisitedCells = 0;
	};

	struct FCellSnapshot
	{
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		FName MaterialId = NAME_None;
		int32 SupportHeight = 0;
	};

	struct FSeedCell
	{
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		FName MaterialId = NAME_None;
		int32 SupportHeight = 0;
	};

	class MATTERFLUX_API FChunkedMaterialWorld
	{
	public:
		FChunkedMaterialWorld();
		~FChunkedMaterialWorld();

		FChunkedMaterialWorld(const FChunkedMaterialWorld&) = delete;
		FChunkedMaterialWorld& operator=(const FChunkedMaterialWorld&) = delete;

		bool Initialize(
			const FWorldSettings& Settings,
			const FMatterFluxContentRegistry& Registry,
			int32 Seed,
			FString& OutError);
		bool SetCell(const FIntPoint& WorldCell, FName MaterialId);
		bool SetSupportHeight(const FIntPoint& WorldCell, int32 Height);
		bool SeedSurface(const TArray<FSeedCell>& SeedCells);
		FName GetMaterialAt(const FIntPoint& WorldCell) const;
		int32 CountMaterial(FName MaterialId) const;
		int32 GetResidentChunkCount() const;
		int32 GetArchivedChunkCount() const;
		int32 GetSimulationFocusCount() const;
		void GetActiveCells(TArray<FCellSnapshot>& OutCells) const;
		bool ExportActiveState(
			int32 LogicalStep,
			TArray<uint8>& OutState,
			FString& OutError) const;
		bool ImportActiveState(
			const TArray<uint8>& State,
			int32& OutLogicalStep,
			FIntPoint& OutFocus,
			FString& OutError);
		void SetSimulationFocus(const FIntPoint& WorldCell);
		/**
		 * Activates the deterministic, budget-capped union around every focus.
		 * Input order does not affect the chosen active chunks.
		 */
		void SetSimulationFocuses(
			TConstArrayView<FIntPoint> WorldCells);
		FStepStats Step();

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
