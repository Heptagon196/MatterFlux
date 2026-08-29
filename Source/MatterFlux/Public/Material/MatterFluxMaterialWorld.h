#pragma once

#include "CoreMinimal.h"
#include "MatterFluxContentTypes.h"

namespace MatterFlux::Material
{
	/** Physical height represented by Amount=255 for a surface powder pile. */
	inline constexpr int32 SurfacePowderFullColumnHeight = 14;

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
		/** Maximum adjacent powder-height delta, in conserved amount units. */
		int32 PowderMaximumStableSlopeAmount = 96;
		/** Fixed steps after the final body constraint before wake refill begins. */
		int32 BodyWakeRefillDelaySteps = 8;
		/** Target fixed-step duration of the progressive body-wake refill. */
		int32 BodyWakeRefillDurationSteps = 16;

		bool IsValid() const;
	};

	struct FStepStats
	{
		/** Dirty candidates scheduled before phase/material filtering. */
		int32 CandidateCells = 0;
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
		/** Conserved column volume. Liquids use 1..255; surface powders may stack above 255. */
		uint16 Amount = 255;
		/** 剩余存在步数；0 表示不会自行消散。 */
		uint8 RemainingLifetime = 0;
		/** Deterministic specific energy carried by this material amount. */
		uint16 Energy = 0;
	};

	struct FSeedCell
	{
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		FName MaterialId = NAME_None;
		int32 SupportHeight = 0;
		/** Initial conserved occupancy for liquids; empty/static cells remain full. */
		uint16 Amount = 255;
		/** 0 uses the material's authored default energy. */
		uint16 Energy = 0;
	};

	/** A validated kernel emission waiting for its geometry adapter. */
	struct FReactionEmission
	{
		FGuid ParticleId;
		FName MaterialId = NAME_None;
		FIntVector GridCell = FIntVector::ZeroValue;
		uint16 Amount = 0;
		uint16 Energy = 0;
		uint16 RemainingLifetimeSteps = 0;
	};

	/**
	 * Idempotent transient-body constraint for one liquid column. The solver
	 * moves only the amount above MaximumRemainingAmount, so submitting the
	 * same occupied volume every frame cannot progressively drain the column.
	 */
	struct FLiquidDisplacementConstraint
	{
		FIntPoint WorldCell = FIntPoint::ZeroValue;
		uint16 MaximumRemainingAmount = 0;
	};

	/** Deterministic work counters for the most recent body displacement solve. */
	struct FLiquidDisplacementStats
	{
		int32 SourceCells = 0;
		int32 ConnectedFootprints = 0;
		int32 CandidateCellsVisited = 0;
		int32 DestinationCellsChanged = 0;
		int64 TransferredAmount = 0;
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
		/** Sets an exact conserved occupancy; intended for liquid injection. */
		bool SetCellAmount(
			const FIntPoint& WorldCell,
			FName MaterialId,
			uint16 Amount);
		bool SetCellAmount(
			const FIntPoint& WorldCell,
			FName MaterialId,
			uint16 Amount,
			uint16 Energy);
		/**
		 * Adds conserved material to a surface column without discarding another
		 * flowing material already occupying that column. Layers are ordered by
		 * density, so powder can settle below liquid at the same terrain cell.
		 * Returns the amount accepted by the column.
		 */
		int32 AddCellAmount(
			const FIntPoint& WorldCell,
			FName MaterialId,
			uint16 Amount);
		int32 AddCellAmount(
			const FIntPoint& WorldCell,
			FName MaterialId,
			uint16 Amount,
			uint16 Energy);
		/**
		 * Evaluates one stable airborne-particle/surface contact through the same
		 * immutable local reaction kernel used by cell/cell contacts. The particle
		 * values are committed only after the complete delta batch validates.
		 */
		bool ReactAirborneParticleAt(
			const FIntPoint& WorldCell,
			const FGuid& ParticleId,
			FName& InOutMaterialId,
			uint16& InOutAmount,
			uint16& InOutEnergy,
			FString& OutError);
		/**
		 * Atomically rolls an incoming powder packet across the current canonical
		 * surface until adding it preserves the material's angle of repose.
		 * Returns the conserved amount accepted and reports the actual destination.
		 */
		int32 AddPowderAmountAtStableSurface(
			const FIntPoint& ImpactCell,
			FName MaterialId,
			uint16 Amount,
			int32 MaximumTravelCells,
			FIntPoint& OutDestinationCell);
		uint16 GetMaterialAmountAt(
			const FIntPoint& WorldCell,
			FName MaterialId) const;
		bool SetSupportHeight(const FIntPoint& WorldCell, int32 Height);
		/**
		 * Adds a non-serialized physical support supplied by a world object such as
		 * a tree canopy. The canonical terrain support remains untouched.
		 */
		bool SetExternalSupportHeight(const FIntPoint& WorldCell, int32 Height);
		bool ClearExternalSupportHeight(const FIntPoint& WorldCell);
		/**
		 * Seeds a bounded surface batch. Streaming callers may defer baseline
		 * encoding until the last batch so one logical terrain chunk can be
		 * spread across frames without repeatedly serializing the same chunk.
		 */
		bool SeedSurface(const TArray<FSeedCell>& SeedCells);
		bool SeedSurface(
			const TArray<FSeedCell>& SeedCells,
			bool bFinalizeBaseline);
		/**
		 * Temporarily admits surface chunks containing the supplied cells into the
		 * solver without consuming the player-focus chunk budget. Call this before
		 * authoring newly visible generated or projectile material. The chunks stay
		 * resident only while they still have dirty work.
		 */
		void WakeSurfaceCells(TConstArrayView<FIntPoint> WorldCells);
		/**
		 * Atomically moves liquid out of transiently occupied surface cells.
		 * Destinations are deterministic, material-compatible, and outside the
		 * full occupied footprint, so identity and amount are conserved exactly.
		 */
		int32 DisplaceLiquids(
			TConstArrayView<FIntPoint> OccupiedCells,
			int32 MaxSearchRadius = 64);
		int32 DisplaceLiquids(
			TConstArrayView<FLiquidDisplacementConstraint> Constraints,
			int32 MaxSearchRadius = 64);
		/** Conservatively pushes stacked surface powder out of occupied cells. */
		int32 DisplacePowders(
			TConstArrayView<FLiquidDisplacementConstraint> Constraints,
			int32 MaxSearchRadius = 64);
		const FLiquidDisplacementStats& GetLastLiquidDisplacementStats() const;
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
		/** Enumerates resident facts only for local disposable projections. */
		void GetResidentCells(TArray<FCellSnapshot>& OutCells) const;
		/**
		 * Enumerates only the requested material chunks, reading either their live
		 * resident state or compact archived state. This is the bounded query used
		 * by disposable local projections such as the visible liquid surface.
		 */
		void GetCellsInChunks(
			TConstArrayView<FIntPoint> Chunks,
			TArray<FCellSnapshot>& OutCells) const;
		/** Enumerates every fact for persistence and diagnostic inspection. */
		void GetAllCells(TArray<FCellSnapshot>& OutCells) const;
		/**
		 * Moves the render-dirty material chunk set to the caller. Simulation dirty
		 * rectangles remain private; this set tracks only neighborhoods whose facts
		 * changed and therefore require disposable projection rebuilds.
		 */
		void ConsumeProjectionDirtyChunks(TArray<FIntPoint>& OutChunks);
		/** Moves committed reaction emissions to the canonical particle adapter. */
		void ConsumeReactionEmissions(TArray<FReactionEmission>& OutEmissions);
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
