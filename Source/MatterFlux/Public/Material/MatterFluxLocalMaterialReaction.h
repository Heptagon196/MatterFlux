#pragma once

#include "CoreMinimal.h"
#include "Volume/MatterFluxMaterialVolume.h"

struct FMatterFluxContentRegistry;

enum class EMaterialElementAddressKind : uint8
{
	WorldCell,
	VolumeCell,
	AirborneParticle
};

/** Stable identity for every material-bearing element. Array indices are never identities. */
struct MATTERFLUX_API FMaterialElementAddress
{
	EMaterialElementAddressKind Kind = EMaterialElementAddressKind::WorldCell;
	FGuid OwnerId;
	FIntVector Cell = FIntVector::ZeroValue;

	static FMaterialElementAddress MakeWorldCell(const FIntVector& WorldCell);
	static FMaterialElementAddress MakeVolumeCell(
		const FGuid& InstanceId,
		const FIntVector& LocalCell);
	static FMaterialElementAddress MakeAirborneParticle(const FGuid& ParticleId);

	bool IsValid() const;
	bool operator==(const FMaterialElementAddress& Other) const = default;
};

MATTERFLUX_API uint32 GetTypeHash(const FMaterialElementAddress& Address);
MATTERFLUX_API bool MaterialElementAddressLess(
	const FMaterialElementAddress& Left,
	const FMaterialElementAddress& Right);

struct MATTERFLUX_API FMaterialElementState
{
	uint16 MaterialIndex = 0;
	uint16 Amount = 0;
	uint16 Energy = 0;
	uint16 RemainingLifetime = 0;

	bool IsValid() const;
	int64 GetTotalEnergy() const
	{
		return static_cast<int64>(Amount) * Energy;
	}
	bool operator==(const FMaterialElementState& Other) const = default;
};

struct MATTERFLUX_API FMaterialContact
{
	FMaterialElementAddress ElementA;
	FMaterialElementAddress ElementB;
	uint16 ContactUnits = 0;
	uint64 DeterministicSortKey = 0;

	FMaterialContact() = default;
	FMaterialContact(
		const FMaterialElementAddress& InElementA,
		const FMaterialElementAddress& InElementB,
		uint16 InContactUnits);

	bool IsValid() const;
};

struct MATTERFLUX_API FMaterialThermalDefinition
{
	uint16 MaterialIndex = 0;
	uint16 DefaultEnergy = 0;
	uint16 DefaultLifetime = 0;
	uint16 ConductivityPermille = 0;
	uint16 CoolingPerStep = 0;
	uint16 IgnitionThreshold = 0;
	uint16 IgnitionProductMaterialIndex = 0;
	/** Specific energy assigned after ignition; 0 preserves the triggering energy. */
	uint16 IgnitionProductEnergy = 0;
	uint16 IgnitionEmissionMaterialIndex = 0;
	uint16 IgnitionEmissionAmount = 0;
	uint16 IgnitionSecondaryEmissionMaterialIndex = 0;
	uint16 IgnitionSecondaryEmissionAmount = 0;

	bool IsValid() const;
};

enum class EMaterialEmissionSourceSide : uint8
{
	A,
	B
};

struct MATTERFLUX_API FMaterialEmissionDefinition
{
	uint16 MaterialIndex = 0;
	uint16 Amount = 0;
	uint16 Energy = 0;
	EMaterialEmissionSourceSide SourceSide = EMaterialEmissionSourceSide::A;

	bool IsValid() const { return MaterialIndex != 0 && Amount != 0; }
};

/** Contact-only reaction rule. There is deliberately no propagation or duration state. */
struct MATTERFLUX_API FLocalMaterialContactRule
{
	FName RuleId = NAME_None;
	uint16 InputA = 0;
	uint16 InputB = 0;
	uint16 OutputA = 0;
	uint16 OutputB = 0;
	int32 ChancePermille = 1000;
	int32 EnergyDeltaA = 0;
	int32 EnergyDeltaB = 0;
	TArray<FMaterialEmissionDefinition, TInlineAllocator<2>> Emissions;

	bool IsValid() const;
};

struct MATTERFLUX_API FMaterialElementDelta
{
	FMaterialElementAddress Address;
	FMaterialElementState ExpectedBefore;
	FMaterialElementState After;
};

struct MATTERFLUX_API FMaterialParticleEmission
{
	FGuid ParticleId;
	FMaterialElementAddress Emitter;
	uint16 MaterialIndex = 0;
	uint16 Amount = 0;
	uint16 Energy = 0;
	uint16 EmissionOrdinal = 0;
};

struct MATTERFLUX_API FMaterialRevisionTarget
{
	FGuid InstanceId;
	int32 BaseTopologyRevision = 0;
	int32 TargetTopologyRevision = 0;
	int32 BaseFieldRevision = 0;
	int32 TargetFieldRevision = 0;
};

/** Fully validated before commit; no caller may apply individual members directly. */
struct MATTERFLUX_API FMaterialDeltaBatch
{
	int32 BaseStoreRevision = 0;
	int32 TargetStoreRevision = 0;
	/** Authored amount source (>0) or sink (<0), including empty outputs. */
	int64 ExplicitAmountDelta = 0;
	int64 ExplicitEnergyDelta = 0;
	TArray<FMaterialElementDelta> ElementDeltas;
	TArray<FMaterialParticleEmission> ParticleEmissions;
	TArray<FMaterialRevisionTarget> VolumeRevisions;

	int64 ComputeExpectedAmountBefore() const;
	int64 ComputeExpectedAmountAfterIncludingEmissions() const;
	int64 ComputeExpectedAmountAfterIncludingExplicitSources() const;
	int64 ComputeExpectedEnergyBefore() const;
	int64 ComputeExpectedEnergyAfterIncludingExplicitSources() const;
	uint64 ComputeDeterministicHash() const;
	bool IsValid(FString& OutError) const;
};

class MATTERFLUX_API IMaterialElementView
{
public:
	virtual ~IMaterialElementView() = default;
	virtual int32 GetStoreRevision() const = 0;
	virtual bool TryGetState(
		const FMaterialElementAddress& Address,
		FMaterialElementState& OutState) const = 0;
};

/** Pure test/runtime adapter demonstrating atomic validation and commit semantics. */
class MATTERFLUX_API FMaterialElementStore final : public IMaterialElementView
{
public:
	virtual int32 GetStoreRevision() const override { return StoreRevision; }
	virtual bool TryGetState(
		const FMaterialElementAddress& Address,
		FMaterialElementState& OutState) const override;
	bool SetInitialState(
		const FMaterialElementAddress& Address,
		const FMaterialElementState& State);
	bool ApplyBatch(
		const FMaterialDeltaBatch& Batch,
		TArray<FMaterialParticleEmission>& OutEmissions,
		FString& OutError);
	uint64 ComputeDeterministicHash() const;

private:
	TMap<FMaterialElementAddress, FMaterialElementState> States;
	int32 StoreRevision = 0;
};

struct MATTERFLUX_API FLocalMaterialReactionContext
{
	uint32 Seed = 0;
	int32 LogicalStep = 0;
	int32 MaxContacts = 0;
	int32 MaxElementDeltas = 0;
	int32 MaxEmissions = 0;
	bool bApplyCooling = true;
	/** Non-environment elements that must cool even when they have no neighbour. */
	TArray<FMaterialElementAddress> CoolingElements;
};

/** The only reaction entry point: immutable view + contacts + fixed-step context -> delta batch. */
class MATTERFLUX_API FLocalMaterialReactionKernel
{
public:
	static bool Evaluate(
		const IMaterialElementView& View,
		TConstArrayView<FMaterialThermalDefinition> Materials,
		TConstArrayView<FLocalMaterialContactRule> Rules,
		TConstArrayView<FMaterialContact> Contacts,
		const FLocalMaterialReactionContext& Context,
		FMaterialDeltaBatch& OutBatch,
		FString& OutError);
};

/**
 * Immutable compiled view of authored materials and contact rules.
 *
 * This is the single adapter from content names to the kernel's compact integer
 * vocabulary. World cells, Volume cells and airborne particles all evaluate the
 * same program; callers never reinterpret reaction definitions themselves.
 */
class MATTERFLUX_API FLocalMaterialReactionProgram
{
public:
	/**
	 * Derives the visible-flame threshold from material facts only. A hot
	 * combustion product remains visibly burning at its precursor's ignition
	 * threshold; no object-level burning state or actor-specific rule is kept.
	 */
	static uint16 ResolveVisibleFlameThreshold(
		const FMatterFluxContentRegistry& Registry,
		FName CurrentMaterialId,
		FName PrecursorHint = NAME_None);
	bool Compile(
		const FMatterFluxContentRegistry& Registry,
		FString& OutError);
	bool IsCompiled() const { return bCompiled; }
	bool TryGetMaterialIndex(FName MaterialId, uint16& OutIndex) const;
	bool TryGetMaterialId(uint16 MaterialIndex, FName& OutId) const;
	bool MakeState(
		FName MaterialId,
		uint16 Amount,
		TOptional<uint16> Energy,
		FMaterialElementState& OutState) const;
	bool EvaluatePair(
		const FMaterialElementAddress& AddressA,
		const FMaterialElementState& StateA,
		const FMaterialElementAddress& AddressB,
		const FMaterialElementState& StateB,
		int32 StoreRevision,
		const FLocalMaterialReactionContext& Context,
		FMaterialDeltaBatch& OutBatch,
		FString& OutError) const;

	TConstArrayView<FMaterialThermalDefinition> GetThermalDefinitions() const
	{
		return ThermalDefinitions;
	}
	TConstArrayView<FLocalMaterialContactRule> GetContactRules() const
	{
		return ContactRules;
	}

private:
	TMap<FName, uint16> MaterialIndices;
	TArray<FName> MaterialIds;
	TArray<FMaterialThermalDefinition> ThermalDefinitions;
	TArray<FLocalMaterialContactRule> ContactRules;
	bool bCompiled = false;
};
