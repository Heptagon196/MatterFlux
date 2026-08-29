#pragma once

#include "CoreMinimal.h"

struct FFragmentSourceMask;
namespace MatterFlux::PlayableLevel
{
	struct FLevelTerrain;
}

/** Logical storage adapter. It never changes the meaning or hash of a chunk. */
enum class EMaterialVolumeStorageAdapter : uint8
{
	Span,
	Dense
};

/** One material run along the local N axis, expressed as [BeginN, EndNExclusive). */
struct MATTERFLUX_API FMaterialSpan
{
	int32 BeginN = 0;
	int32 EndNExclusive = 0;
	uint16 MaterialIndex = 0;
	uint16 Flags = 0;

	FMaterialSpan() = default;
	FMaterialSpan(
		const int32 InBeginN,
		const int32 InEndNExclusive,
		const uint16 InMaterialIndex,
		const uint16 InFlags = 0)
		: BeginN(InBeginN)
		, EndNExclusive(InEndNExclusive)
		, MaterialIndex(InMaterialIndex)
		, Flags(InFlags)
	{
	}

	bool IsValid() const
	{
		return BeginN < EndNExclusive && MaterialIndex != 0;
	}

	bool operator==(const FMaterialSpan& Other) const = default;
};

struct MATTERFLUX_API FMaterialVolumeColumnHeader
{
	int32 SpanOffset = 0;
	int32 SpanCount = 0;
};

struct MATTERFLUX_API FMaterialDenseCell
{
	uint16 MaterialIndex = 0;
	uint16 Flags = 0;

	bool IsOccupied() const { return MaterialIndex != 0; }
};

/** Immutable-by-convention chunk snapshot produced only by FMaterialVolumeChunkBuilder. */
struct MATTERFLUX_API FMaterialVolumeChunkSnapshot
{
	FIntPoint ChunkCoord = FIntPoint::ZeroValue;
	int32 ChunkSize = 16;
	EMaterialVolumeStorageAdapter Adapter = EMaterialVolumeStorageAdapter::Span;
	TArray<FMaterialVolumeColumnHeader> Columns;
	TArray<FMaterialSpan> SpanPool;
	int32 DenseBeginN = 0;
	int32 DenseEndNExclusive = 0;
	TArray<FMaterialDenseCell> DenseCells;

	bool TryGetColumnSpans(
		const FIntPoint& LocalColumn,
		TArray<FMaterialSpan>& OutSpans) const;
	bool IsValid(FString* OutError = nullptr) const;
};

/** Transactional builder: Build never mutates a previously committed snapshot. */
class MATTERFLUX_API FMaterialVolumeChunkBuilder
{
public:
	explicit FMaterialVolumeChunkBuilder(
		FIntPoint InChunkCoord,
		int32 InChunkSize = 16);
	explicit FMaterialVolumeChunkBuilder(
		const FMaterialVolumeChunkSnapshot& Source);

	bool SetColumnSpans(
		const FIntPoint& LocalColumn,
		TConstArrayView<FMaterialSpan> Spans);
	bool Build(
		EMaterialVolumeStorageAdapter Adapter,
		FMaterialVolumeChunkSnapshot& OutSnapshot,
		FString& OutError) const;

private:
	FIntPoint ChunkCoord = FIntPoint::ZeroValue;
	int32 ChunkSize = 16;
	TArray<TArray<FMaterialSpan>> PendingColumns;
};

struct MATTERFLUX_API FMaterialGridFrame
{
	FVector3d Origin = FVector3d::ZeroVector;
	FVector3d BasisU = FVector3d::ForwardVector;
	FVector3d BasisV = FVector3d::RightVector;
	FVector3d BasisN = FVector3d::UpVector;
	double CellSize = 10.0;

	bool IsValid() const;
};

/** A sparse explicit break between two otherwise six-neighbour-connected cells. */
struct MATTERFLUX_API FStructuralSeam
{
	FIntVector CellA = FIntVector::ZeroValue;
	FIntVector CellB = FIntVector::ZeroValue;

	FStructuralSeam() = default;
	FStructuralSeam(const FIntVector& InCellA, const FIntVector& InCellB);

	bool IsValid() const;
	bool operator==(const FStructuralSeam& Other) const = default;
};

MATTERFLUX_API uint32 GetTypeHash(const FStructuralSeam& Seam);

/** Occupancy, material and structural connectivity only. World pose is deliberately absent. */
struct MATTERFLUX_API FMaterialVolumeTopology
{
	FName DefinitionId = NAME_None;
	FMaterialGridFrame GridFrame;
	int32 TopologyRevision = 0;
	TMap<FIntPoint, FMaterialVolumeChunkSnapshot> Chunks;
	TSet<FIntVector> StructuralAnchors;
	TSet<FStructuralSeam> StructuralSeams;

	bool IsValid(FString* OutError = nullptr) const;
};

/** Sparse non-environment energy values. Field-only edits never touch topology revision. */
struct MATTERFLUX_API FMaterialVolumeFields
{
	int32 FieldRevision = 0;
	uint16 EnvironmentEnergy = 0;
	TMap<FIntVector, uint16> EnergyOverrides;

	uint16 GetEnergy(const FIntVector& Cell) const;
	bool SetEnergy(const FIntVector& Cell, uint16 Energy);
	bool IsValid() const { return FieldRevision >= 0; }
};

/** Runtime identity and movement. These fields never participate in topology hashing. */
struct MATTERFLUX_API FMaterialVolumeInstance
{
	FGuid InstanceId;
	FGuid ParentInstanceId;
	FTransform WorldTransform = FTransform::Identity;
	FVector LinearVelocity = FVector::ZeroVector;
	FVector AngularVelocityRadians = FVector::ZeroVector;
	FMaterialVolumeTopology Topology;
	FMaterialVolumeFields Fields;
};

struct MATTERFLUX_API FMaterialVolumeChunkChange
{
	FIntPoint ChunkCoord = FIntPoint::ZeroValue;
	bool bRemoved = false;
	FMaterialVolumeChunkSnapshot Snapshot;
};

struct MATTERFLUX_API FMaterialVolumeFieldChange
{
	FIntVector Cell = FIntVector::ZeroValue;
	bool bRemoved = false;
	uint16 Energy = 0;
};

/** Atomic topology+field transaction. Runtime pose is deliberately absent. */
struct MATTERFLUX_API FMaterialVolumeDelta
{
	FGuid InstanceId;
	int32 BaseTopologyRevision = 0;
	int32 TargetTopologyRevision = 0;
	int32 BaseFieldRevision = 0;
	int32 TargetFieldRevision = 0;
	FName TargetDefinitionId = NAME_None;
	FMaterialGridFrame TargetGridFrame;
	TSet<FIntVector> TargetStructuralAnchors;
	TSet<FStructuralSeam> TargetStructuralSeams;
	uint16 TargetEnvironmentEnergy = 0;
	TArray<FMaterialVolumeChunkChange> ChunkChanges;
	TArray<FMaterialVolumeFieldChange> FieldChanges;
	uint64 ResultTopologyHash = 0;
};

enum class EMaterialVolumeDeltaApplyResult : uint8
{
	Applied,
	SnapshotRequired,
	Invalid
};

struct MATTERFLUX_API FMaterialVolumeComponent
{
	FIntVector MinimumCell = FIntVector::ZeroValue;
	int64 CellCount = 0;
	TArray<FIntVector> SpanStarts;
};

enum class EMaterialVolumeCutShape : uint8
{
	Sphere,
	OrientedBox,
	Capsule,
	PlaneSlab,
	SweptBlade
};

/** Definition-local quantized cutter; WorldTransform never enters this data. */
struct MATTERFLUX_API FMaterialVolumeCut
{
	EMaterialVolumeCutShape Shape = EMaterialVolumeCutShape::Sphere;
	FTransform StartTransform = FTransform::Identity;
	FTransform EndTransform = FTransform::Identity;
	FVector HalfExtent = FVector::ZeroVector;
	FVector SegmentStart = FVector::ZeroVector;
	FVector SegmentEnd = FVector::ZeroVector;
	FVector PlaneNormal = FVector::UpVector;
	double Radius = 0.0;
	double HalfThickness = 0.0;

	static FMaterialVolumeCut MakeSphere(FVector Center, double Radius);
	static FMaterialVolumeCut MakeOrientedBox(
		const FTransform& Transform,
		FVector HalfExtent);
	static FMaterialVolumeCut MakeCapsule(
		FVector SegmentStart,
		FVector SegmentEnd,
		double Radius);
	static FMaterialVolumeCut MakePlaneSlab(
		FVector Origin,
		FVector Normal,
		double HalfThickness);
	static FMaterialVolumeCut MakeSweptBlade(
		const FTransform& StartTransform,
		const FTransform& EndTransform,
		FVector HalfExtent);
	bool IsValid() const;
};

struct MATTERFLUX_API FMaterialVolumeMassProperties
{
	int64 CellCount = 0;
	double Mass = 0.0;
	FVector3d CenterOfMass = FVector3d::ZeroVector;
	FVector3d InertiaDiagonal = FVector3d::ZeroVector;
	/** XY, XZ and YZ off-diagonal tensor terms. */
	FVector3d InertiaProducts = FVector3d::ZeroVector;
};

enum class EMaterialSurfaceFace : uint8
{
	NegativeU,
	PositiveU,
	NegativeV,
	PositiveV,
	NegativeN,
	PositiveN
};

/** Stable address for any exposed terrain face, including cave ceilings/walls. */
struct MATTERFLUX_API FMaterialSurfaceKey
{
	FIntPoint WorldColumn = FIntPoint::ZeroValue;
	int32 SurfaceN = 0;
	EMaterialSurfaceFace Face = EMaterialSurfaceFace::PositiveN;

	bool operator==(const FMaterialSurfaceKey& Other) const = default;
};

MATTERFLUX_API uint32 GetTypeHash(const FMaterialSurfaceKey& Key);

/**
 * Sparse authoritative terrain edits. The procedural baseline stays implicit;
 * callers supply its column when reading or committing one edited column.
 */
struct MATTERFLUX_API FMaterialTerrainSpanOverlay
{
	int32 Revision = 0;
	TMap<FIntPoint, TArray<FMaterialSpan>> ColumnOverrides;

	bool ResolveColumn(
		const FIntPoint& WorldColumn,
		TConstArrayView<FMaterialSpan> BaselineSpans,
		TArray<FMaterialSpan>& OutSpans,
		FString& OutError) const;
	/** Restoring a column to its normalized baseline immediately removes it. */
	bool CommitColumn(
		const FIntPoint& WorldColumn,
		TConstArrayView<FMaterialSpan> BaselineSpans,
		TConstArrayView<FMaterialSpan> NewSpans,
		FString& OutError);
	bool TryGetHighestSurface(
		const FIntPoint& WorldColumn,
		TConstArrayView<FMaterialSpan> BaselineSpans,
		FMaterialSurfaceKey& OutSurface,
		uint16& OutMaterialIndex,
		FString& OutError) const;
	bool TryGetSolidAtHeight(
		const FIntPoint& WorldColumn,
		int32 N,
		TConstArrayView<FMaterialSpan> BaselineSpans,
		uint16& OutMaterialIndex,
		FString& OutError) const;
};

class MATTERFLUX_API FMaterialTerrainSurfaceAlgorithms
{
public:
	/**
	 * Gathers every exposed face in one resolved column. Four neighbour columns
	 * are supplied explicitly so no whole-world scan or hidden baseline lookup
	 * can occur inside this module.
	 */
	static bool GatherExposedFaces(
		const FIntPoint& WorldColumn,
		TConstArrayView<FMaterialSpan> Center,
		TConstArrayView<FMaterialSpan> NegativeU,
		TConstArrayView<FMaterialSpan> PositiveU,
		TConstArrayView<FMaterialSpan> NegativeV,
		TConstArrayView<FMaterialSpan> PositiveV,
		TArray<FMaterialSurfaceKey>& OutFaces,
		FString& OutError);
};

/** Bounded six-neighbour support analysis for edited terrain columns. */
class MATTERFLUX_API FMaterialTerrainSupportAlgorithms
{
public:
	/**
	 * Finds supported occupied cells inside DirtyBounds without scanning the
	 * world. RegionColumns must contain the dirty columns plus any available
	 * one-column halo. Seeds are occupied bottom cells and dirty-boundary cells
	 * connected to occupied halo cells.
	 */
	static bool GatherSupportedCellsInDirtyRegion(
		const TMap<FIntPoint, TArray<FMaterialSpan>>& RegionColumns,
		const FIntRect& DirtyBounds,
		int32 WorldBottomN,
		int32 MaximumVisitedCells,
		TSet<FIntVector>& OutSupportedCells,
		FString& OutError);
};

class MATTERFLUX_API FMaterialSpanAlgorithms
{
public:
	static bool Normalize(TArray<FMaterialSpan>& InOutSpans, FString& OutError);
	static TArray<FMaterialSpan> SubtractInterval(
		TConstArrayView<FMaterialSpan> Spans,
		int32 BeginN,
		int32 EndNExclusive);
};

class MATTERFLUX_API FMaterialVolumeAlgorithms
{
public:
	static uint64 ComputeChunkLogicalHash(
		const FMaterialVolumeChunkSnapshot& Chunk);
	static uint64 ComputeLogicalHash(const FMaterialVolumeTopology& Topology);
	static bool GatherComponents(
		const FMaterialVolumeTopology& Topology,
		TArray<FMaterialVolumeComponent>& OutComponents,
		FString& OutError);
	static bool TryGetSingleSpan(
		const FMaterialVolumeTopology& Topology,
		const FIntPoint& GlobalColumn,
		FMaterialSpan& OutSpan);
	static bool TryGetColumnSpans(
		const FMaterialVolumeTopology& Topology,
		const FIntPoint& GlobalColumn,
		TArray<FMaterialSpan>& OutSpans);
	static bool TryGetCellMaterial(
		const FMaterialVolumeTopology& Topology,
		const FIntVector& Cell,
		uint16& OutMaterialIndex);
	/** Atomically changes one occupied cell's material and advances topology once. */
	static bool SetCellMaterial(
		const FMaterialVolumeTopology& Source,
		const FIntVector& Cell,
		uint16 MaterialIndex,
		FMaterialVolumeTopology& OutTopology,
		FString& OutError);
	/** Atomically rebuilds changed chunks; an unchanged repeated cut keeps revision. */
	static bool Subtract(
		const FMaterialVolumeTopology& Source,
		const FMaterialVolumeCut& Cut,
		FMaterialVolumeTopology& OutTopology,
		FString& OutError);
	static bool ComputeMassProperties(
		const FMaterialVolumeTopology& Topology,
		const TMap<uint16, double>& DensityByMaterial,
		FMaterialVolumeMassProperties& OutProperties,
		FString& OutError);
	static FVector ComputeChildLinearVelocity(
		const FVector& ParentLinearVelocity,
		const FVector& ParentAngularVelocityRadians,
		const FVector& ParentCenterOfMass,
		const FVector& ChildCenterOfMass);
	static bool BuildDelta(
		const FGuid& InstanceId,
		const FMaterialVolumeTopology& BaseTopology,
		const FMaterialVolumeFields& BaseFields,
		const FMaterialVolumeTopology& TargetTopology,
		const FMaterialVolumeFields& TargetFields,
		FMaterialVolumeDelta& OutDelta,
		FString& OutError);
	static EMaterialVolumeDeltaApplyResult ApplyDelta(
		const FMaterialVolumeDelta& Delta,
		FMaterialVolumeTopology& InOutTopology,
		FMaterialVolumeFields& InOutFields,
		FString& OutError);
};

class MATTERFLUX_API FMaterialVolumeConverters
{
public:
	/** Converts legacy local X/Z mask cells into Volume U/V and extrusion Y into N. */
	static bool FromLegacyMaskXZY(
		const FFragmentSourceMask& Mask,
		int32 BeginY,
		int32 EndYExclusive,
		FMaterialVolumeTopology& OutTopology,
		FString& OutError,
		uint16 MaterialIndex = 1);
	/** Converts the cached legacy height field using world X/Y columns and Z/N. */
	static bool FromLegacyTerrainXYZ(
		const MatterFlux::PlayableLevel::FLevelTerrain& Terrain,
		FMaterialVolumeTopology& OutTopology,
		FString& OutError,
		uint16 MaterialIndex = 1);
};
