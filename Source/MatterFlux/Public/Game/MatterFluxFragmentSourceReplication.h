#pragma once

#include "CoreMinimal.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "MatterFluxFragmentSourceReplication.generated.h"

class AMatterFluxPlayableWorldActor;

USTRUCT()
struct MATTERFLUX_API FMatterFluxReplicatedVolumeCellState
{
	GENERATED_BODY()

	UPROPERTY()
	FIntVector Cell = FIntVector::ZeroValue;

	UPROPERTY()
	FName MaterialId = NAME_None;

	UPROPERTY()
	uint16 Energy = 0;
};

USTRUCT()
struct MATTERFLUX_API FMatterFluxReplicatedFragmentSourceState
	: public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid SourceId;

	UPROPERTY()
	int32 Revision = 0;

	UPROPERTY()
	int32 VolumeTopologyRevision = 0;

	UPROPERTY()
	int32 VolumeFieldRevision = 0;

	UPROPERTY()
	uint16 VolumeEnvironmentEnergy = 0;

	UPROPERTY()
	TArray<FMatterFluxReplicatedVolumeCellState> VolumeCellStates;

	UPROPERTY()
	TArray<uint8> PackedRuntimeMask;

};

enum class EMatterFluxFragmentSourceStateUpsertResult : uint8
{
	Committed,
	InvalidState,
	ItemBudgetExceeded,
	ByteBudgetExceeded
};

/**
 * A non-owning logical Source update consumed synchronously by the replicated
 * state store. The batch interface validates every update before mutating the
 * Fast Array, so callers never observe a partially committed simulation step.
 */
struct MATTERFLUX_API FMatterFluxFragmentSourceStateBatchUpdate
{
	FGuid SourceId;
	const FFragment2DSourceStreamingState* State = nullptr;
};

/**
 * A synchronous client-side apply plan. Item indices are valid only until the
 * replicated list changes again; callers must consume the plan immediately.
 */
struct MATTERFLUX_API FMatterFluxFragmentSourceClientApplyPlan
{
	bool bFullRebuild = false;
	TArray<int32> UpsertItemIndices;
	TArray<FGuid> RemovedSourceIds;
};

/**
 * Deterministic, failure-atomic Source state replication store. Runtime callers
 * submit logical states; this module owns validation, bit packing, payload
 * budgets, stable ordering and Fast Array dirtiness.
 */
USTRUCT()
struct MATTERFLUX_API FMatterFluxReplicatedFragmentSourceStateList
	: public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FMatterFluxReplicatedFragmentSourceState> Items;

	TWeakObjectPtr<AMatterFluxPlayableWorldActor> Owner;

	EMatterFluxFragmentSourceStateUpsertResult UpsertAuthorityBatch(
		TConstArrayView<FMatterFluxFragmentSourceStateBatchUpdate> Updates,
		int32 MaximumItemCount,
		int32 MaximumPayloadBytes);
	int32 GetAuthorityPayloadByteCount();
	void ResetAuthorityItems();
	void RequestClientFullRebuild();
	void ConsumeClientApplyPlan(
		FMatterFluxFragmentSourceClientApplyPlan& OutPlan);

	void PreReplicatedRemove(
		const TArrayView<int32>& RemovedIndices,
		int32 FinalSize);
	void PostReplicatedAdd(
		const TArrayView<int32>& AddedIndices,
		int32 FinalSize);
	void PostReplicatedChange(
		const TArrayView<int32>& ChangedIndices,
		int32 FinalSize);
	void PostReplicatedReceive(
		const FFastArraySerializer::FPostReplicatedReceiveParameters& Parameters);

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParameters)
	{
		return FastArrayDeltaSerialize<
			FMatterFluxReplicatedFragmentSourceState,
			FMatterFluxReplicatedFragmentSourceStateList>(
				Items,
				DeltaParameters,
				*this);
	}

private:
	bool RebuildAuthorityCache();
	TMap<FGuid, int32> AuthorityIndexBySourceId;
	int32 AuthorityPayloadByteCount = 0;
	int32 AuthorityCachedItemCount = -1;
	TArray<int32> PendingClientUpsertReplicationIds;
	TArray<FGuid> PendingClientRemovedSourceIds;
	bool bClientFullRebuildRequired = true;
};

template<>
struct TStructOpsTypeTraits<
	FMatterFluxReplicatedFragmentSourceStateList>
	: public TStructOpsTypeTraitsBase2<
		FMatterFluxReplicatedFragmentSourceStateList>
{
	enum { WithNetDeltaSerializer = true };
};
