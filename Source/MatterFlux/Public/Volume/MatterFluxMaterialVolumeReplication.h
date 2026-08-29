#pragma once

#include "CoreMinimal.h"
#include "MatterFluxMaterialVolumeReplication.generated.h"

/** One sparse, logical material-cell override in a replicated Volume snapshot. */
USTRUCT()
struct MATTERFLUX_API FMatterFluxReplicatedMaterialVolumeCell
{
	GENERATED_BODY()

	UPROPERTY()
	FIntVector Cell = FIntVector::ZeroValue;

	UPROPERTY()
	FName MaterialId = NAME_None;

	UPROPERTY()
	uint16 Energy = 0;

	bool operator==(
		const FMatterFluxReplicatedMaterialVolumeCell& Other) const = default;
};

/**
 * Full sparse state used when a runtime Volume Actor first becomes relevant.
 * Incremental world-owned Volumes still use the revisioned delta/FastArray path.
 */
USTRUCT()
struct MATTERFLUX_API FMatterFluxReplicatedMaterialVolumeState
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid InstanceId;

	UPROPERTY()
	int32 TopologyRevision = 0;

	UPROPERTY()
	int32 FieldRevision = 0;

	UPROPERTY()
	uint16 EnvironmentEnergy = 0;

	UPROPERTY()
	TArray<FMatterFluxReplicatedMaterialVolumeCell> Cells;

	bool operator==(
		const FMatterFluxReplicatedMaterialVolumeState& Other) const = default;
};
