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
		/** Physical height represented by Amount=255 in surface-topology worlds. */
		int32 LiquidFullColumnHeight = 255;

		bool IsValid() const;
	};

	struct FStepStats
	{
		int32 MovedCells = 0;
		int32 ReactedPairs = 0;
		int32 CulledCells = 0;
		int32 VisitedCells = 0;
		/** Cells examined by the sparse body-wake restitution pass. */
		int32 RestitutionVisitedCells = 0;
	};

	struct FCellSnapshot
	{
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		FName MaterialId = NAME_None;
		int32 SupportHeight = 0;
		/** 物质格占用量；液体使用 1..255，其他相态保持 255。 */
		uint8 Amount = 255;
	};

	struct FSeedCell
	{
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		FName MaterialId = NAME_None;
		int32 SupportHeight = 0;
		/** Initial conserved occupancy for liquids; empty/static cells remain full. */
		uint8 Amount = 255;
	};

	/**
	 * Idempotent transient-body constraint for one liquid column. The solver
	 * moves only the amount above MaximumRemainingAmount, so submitting the
	 * same occupied volume every frame cannot progressively drain the column.
	 */
	struct FLiquidDisplacementConstraint
	{
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		uint8 MaximumRemainingAmount = 0;
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
		/**
		 * Atomically moves liquid out of transiently occupied surface cells.
		 * Destinations are deterministic, empty, and outside the full occupied
		 * footprint, so material identity and amount are conserved exactly.
		 */
		int32 DisplaceLiquids(
			TConstArrayView<FIntPoint> OccupiedCells,
			int32 MaxSearchRadius = 64);
		int32 DisplaceLiquids(
			TConstArrayView<FLiquidDisplacementConstraint> Constraints,
			int32 MaxSearchRadius = 64);
		FName GetMaterialAt(const FIntPoint& WorldCell) const;
		/** 查询单格的材质和承托高度；空格或未初始化世界返回 false。 */
		bool TryGetCellSnapshot(
			const FIntPoint& WorldCell,
			FCellSnapshot& OutSnapshot) const;
		int32 CountMaterial(FName MaterialId) const;
		int64 SumMaterialAmount(FName MaterialId) const;
		int32 GetResidentChunkCount() const;
		int32 GetArchivedChunkCount() const;
		int32 GetSimulationFocusCount() const;
		void GetActiveCells(TArray<FCellSnapshot>& OutCells) const;
		/** Enumerates resident and archived facts for complete visual projection. */
		void GetAllCells(TArray<FCellSnapshot>& OutCells) const;
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
