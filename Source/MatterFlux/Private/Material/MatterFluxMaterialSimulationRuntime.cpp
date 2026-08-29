#include "Material/MatterFluxMaterialSimulationRuntime.h"

#include "Algo/Unique.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace MatterFlux::Material
{
	bool FRuntimeSettings::IsValid() const
	{
		return World.IsValid()
			&& FMath::IsFinite(StepSeconds)
			&& StepSeconds > 0.0f
			&& MaxStepsPerAdvance > 0
			&& MaxStepsPerAdvance <= 64;
	}

	FSimulationRuntime::FSimulationRuntime() = default;
	FSimulationRuntime::~FSimulationRuntime() = default;

	bool FSimulationRuntime::NormalizeFocuses(
		const TConstArrayView<FIntPoint> Focuses,
		TArray<FIntPoint>& OutFocuses)
	{
		OutFocuses.Reset();
		OutFocuses.Append(Focuses.GetData(), Focuses.Num());
		OutFocuses.Sort([](const FIntPoint& A, const FIntPoint& B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});
		OutFocuses.SetNum(Algo::Unique(OutFocuses));
		return !OutFocuses.IsEmpty();
	}

	bool FSimulationRuntime::Initialize(
		const FRuntimeSettings& Settings,
		const FMatterFluxContentRegistry& Registry,
		const int32 Seed,
		const TConstArrayView<FIntPoint> InitialFocuses,
		FString& OutError)
	{
		Reset();
		OutError.Reset();
		if (!Settings.IsValid()
			|| !NormalizeFocuses(InitialFocuses, CurrentFocuses))
		{
			OutError = TEXT("material runtime settings or initial focuses are invalid");
			return false;
		}

		TUniquePtr<FChunkedMaterialWorld> Candidate =
			MakeUnique<FChunkedMaterialWorld>();
		if (!Candidate->Initialize(Settings.World, Registry, Seed, OutError))
		{
			CurrentFocuses.Reset();
			return false;
		}
		Candidate->SetSimulationFocuses(CurrentFocuses);
		TMap<FName, uint16> CandidateDefaultEnergies;
		for (const TPair<FName, FMatterFluxMaterialDefinition>& Pair
			: Registry.Materials)
		{
			CandidateDefaultEnergies.Add(Pair.Key, Pair.Value.DefaultEnergy);
		}
		RuntimeSettings = Settings;
		MaterialWorld = MoveTemp(Candidate);
		DefaultMaterialEnergies = MoveTemp(CandidateDefaultEnergies);
		bReplicationDirty = true;
		return true;
	}

	void FSimulationRuntime::Reset()
	{
		MaterialWorld.Reset();
		RuntimeSettings = FRuntimeSettings();
		CurrentFocuses.Reset();
		AirborneParticles.Reset();
		DefaultMaterialEnergies.Reset();
		StepAccumulator = 0.0f;
		LogicalStep = 0;
		AppliedStateRevision = INDEX_NONE;
		RejectedStateRevision = INDEX_NONE;
		NextAirborneBatchSerial = 1;
		bReplicationDirty = false;
	}

	bool FSimulationRuntime::WillAdvanceStep(const float DeltaSeconds) const
	{
		return MaterialWorld.IsValid()
			&& StepAccumulator
				+ FMath::Clamp(DeltaSeconds, 0.0f, 0.25f)
				>= RuntimeSettings.StepSeconds;
	}

	FRuntimeAdvanceResult FSimulationRuntime::AdvanceAuthority(
		const float DeltaSeconds,
		const TConstArrayView<FIntPoint> Focuses,
		const int32 MaxStepsThisAdvance)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_MaterialRuntime_AdvanceAuthority);
		FRuntimeAdvanceResult Result;
		Result.LogicalStep = LogicalStep;
		if (!MaterialWorld)
		{
			return Result;
		}

		TArray<FIntPoint> NormalizedFocuses;
		if (!NormalizeFocuses(Focuses, NormalizedFocuses))
		{
			return Result;
		}
		if (NormalizedFocuses != CurrentFocuses)
		{
			CurrentFocuses = MoveTemp(NormalizedFocuses);
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(
					MatterFlux_MaterialRuntime_ReconcileFocus);
				MaterialWorld->SetSimulationFocuses(CurrentFocuses);
			}
			Result.bFocusChanged = true;
			bReplicationDirty = true;
		}

		StepAccumulator += FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
		// Focus reconciliation changes which chunks are resident; it must not pause
		// canonical material time. Airborne grains and moving actors can change the
		// focus set every frame for several seconds. Returning here used to starve
		// powder/liquid motion throughout that interval, then make it begin suddenly
		// once the focuses happened to stop changing. The caller already caps normal
		// play to one material step per frame, so continuing remains bounded.

		const int32 EffectiveMaxSteps = FMath::Clamp(
			MaxStepsThisAdvance,
			1,
			RuntimeSettings.MaxStepsPerAdvance);
		while (StepAccumulator >= RuntimeSettings.StepSeconds
			&& Result.Steps < EffectiveMaxSteps)
		{
			StepAccumulator -= RuntimeSettings.StepSeconds;
			FStepStats Stats;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(MatterFlux_MaterialRuntime_Step);
				Stats = MaterialWorld->Step();
			}
			LogicalStep = LogicalStep == MAX_int32 ? 0 : LogicalStep + 1;
			Result.bStateChanged |= Stats.MovedCells > 0
				|| Stats.ReactedPairs > 0
				|| Stats.CulledCells > 0;
			++Result.Steps;
		}
		if (Result.Steps == EffectiveMaxSteps
			&& StepAccumulator >= RuntimeSettings.StepSeconds)
		{
			StepAccumulator = FMath::Fmod(
				StepAccumulator,
				RuntimeSettings.StepSeconds);
		}
		Result.LogicalStep = LogicalStep;
		bReplicationDirty |= Result.Steps > 0;
		return Result;
	}

	bool FSimulationRuntime::BuildReplicatedState(
		const int32 MapSeed,
		const int32 PreviousRevision,
		FMatterFluxReplicatedMaterialState& OutState,
		FString& OutError)
	{
		OutState = FMatterFluxReplicatedMaterialState();
		OutError.Reset();
		if (!MaterialWorld || MapSeed == 0)
		{
			OutError = TEXT("material runtime is not ready to publish");
			return false;
		}
		TArray<uint8> ActiveState;
		if (!MaterialWorld->ExportActiveState(
			LogicalStep,
			ActiveState,
			OutError))
		{
			return false;
		}
		FMatterFluxReplicatedMaterialState Candidate;
		Candidate.MapSeed = MapSeed;
		Candidate.Revision = PreviousRevision == MAX_int32
			? 0
			: PreviousRevision + 1;
		if (!Candidate.EncodeActiveState(ActiveState, OutError))
		{
			return false;
		}
		OutState = MoveTemp(Candidate);
		AppliedStateRevision = OutState.Revision;
		bReplicationDirty = false;
		return true;
	}

	EReplicatedStateApplyResult FSimulationRuntime::ApplyReplicatedState(
		const int32 ExpectedMapSeed,
		const FMatterFluxReplicatedMaterialState& State,
		FString& OutError)
	{
		OutError.Reset();
		if (!MaterialWorld
			|| ExpectedMapSeed == 0
			|| State.MapSeed != ExpectedMapSeed
			|| !State.HasPayload()
			|| AppliedStateRevision == State.Revision
			|| RejectedStateRevision == State.Revision)
		{
			return EReplicatedStateApplyResult::NoChange;
		}

		TArray<uint8> ActiveState;
		if (!State.DecodeActiveState(ActiveState, OutError))
		{
			RejectedStateRevision = State.Revision;
			return EReplicatedStateApplyResult::Rejected;
		}
		int32 ImportedStep = INDEX_NONE;
		FIntPoint ImportedFocus = FIntPoint::ZeroValue;
		if (!MaterialWorld->ImportActiveState(
			ActiveState,
			ImportedStep,
			ImportedFocus,
			OutError))
		{
			RejectedStateRevision = State.Revision;
			return EReplicatedStateApplyResult::Rejected;
		}
		LogicalStep = ImportedStep;
		CurrentFocuses = { ImportedFocus };
		AppliedStateRevision = State.Revision;
		RejectedStateRevision = INDEX_NONE;
		bReplicationDirty = false;
		return EReplicatedStateApplyResult::Applied;
	}

	bool FSimulationRuntime::ExportActiveState(
		TArray<uint8>& OutState,
		FString& OutError) const
	{
		OutState.Reset();
		return MaterialWorld
			&& MaterialWorld->ExportActiveState(
				LogicalStep,
				OutState,
				OutError);
	}

	bool FSimulationRuntime::ImportActiveState(
		const TArray<uint8>& State,
		int32& OutLogicalStep,
		FIntPoint& OutPrimaryFocus,
		FString& OutError)
	{
		if (!MaterialWorld
			|| !MaterialWorld->ImportActiveState(
				State,
				OutLogicalStep,
				OutPrimaryFocus,
				OutError))
		{
			return false;
		}
		LogicalStep = OutLogicalStep;
		CurrentFocuses = { OutPrimaryFocus };
		StepAccumulator = 0.0f;
		bReplicationDirty = true;
		return true;
	}

	bool FSimulationRuntime::SeedSurface(
		const TArray<FSeedCell>& SeedCells)
	{
		return SeedSurface(SeedCells, true);
	}

	bool FSimulationRuntime::SeedSurface(
		const TArray<FSeedCell>& SeedCells,
		const bool bFinalizeBaseline)
	{
		const bool bSeeded = MaterialWorld
			&& MaterialWorld->SeedSurface(
				SeedCells,
				bFinalizeBaseline);
		bReplicationDirty |= bSeeded;
		return bSeeded;
	}

	void FSimulationRuntime::WakeSurfaceCells(
		const TConstArrayView<FIntPoint> WorldCells)
	{
		if (MaterialWorld)
		{
			MaterialWorld->WakeSurfaceCells(WorldCells);
		}
	}

	int32 FSimulationRuntime::DisplaceLiquids(
		const TConstArrayView<FIntPoint> OccupiedCells,
		const int32 MaxSearchRadius)
	{
		const int32 Moved = MaterialWorld
			? MaterialWorld->DisplaceLiquids(
				OccupiedCells, MaxSearchRadius)
			: 0;
		bReplicationDirty |= Moved > 0;
		return Moved;
	}

	int32 FSimulationRuntime::DisplaceLiquids(
		const TConstArrayView<FLiquidDisplacementConstraint> Constraints,
		const int32 MaxSearchRadius)
	{
		const int32 Moved = MaterialWorld
			? MaterialWorld->DisplaceLiquids(
				Constraints, MaxSearchRadius)
			: 0;
		bReplicationDirty |= Moved > 0;
		return Moved;
	}

	int32 FSimulationRuntime::DisplacePowders(
		const TConstArrayView<FLiquidDisplacementConstraint> Constraints,
		const int32 MaxSearchRadius)
	{
		const int32 Moved = MaterialWorld
			? MaterialWorld->DisplacePowders(Constraints, MaxSearchRadius)
			: 0;
		bReplicationDirty |= Moved > 0;
		return Moved;
	}

	void FSimulationRuntime::SetFocuses(
		const TConstArrayView<FIntPoint> Focuses)
	{
		TArray<FIntPoint> Normalized;
		if (!MaterialWorld || !NormalizeFocuses(Focuses, Normalized))
		{
			return;
		}
		if (Normalized != CurrentFocuses)
		{
			CurrentFocuses = MoveTemp(Normalized);
			MaterialWorld->SetSimulationFocuses(CurrentFocuses);
			bReplicationDirty = true;
		}
	}

	FGuid FSimulationRuntime::SpawnAirborneParticles(
		const FName MaterialId,
		const TConstArrayView<FVector> WorldPositions,
		const TConstArrayView<FVector> InitialVelocities,
		const int32 CellCount,
		const int32 ConservedAmountPerCell,
		const float Radius,
		const float GravityScale,
		const float Lifetime,
		const int32 EventSeed,
		const int32 SpecificEnergy)
	{
		if (!MaterialWorld
			|| MaterialId.IsNone()
			|| WorldPositions.IsEmpty()
			|| CellCount <= 0
			|| ConservedAmountPerCell <= 0
			|| !FMath::IsFinite(Radius)
			|| Radius <= 0.0f
			|| !FMath::IsFinite(GravityScale)
			|| !FMath::IsFinite(Lifetime)
			|| Lifetime <= 0.0f
			|| (SpecificEnergy != INDEX_NONE
				&& (SpecificEnergy < 0 || SpecificEnergy > MAX_uint16)))
		{
			return FGuid();
		}

		const uint32 Serial = NextAirborneBatchSerial++;
		const FGuid BatchId(
			static_cast<uint32>(EventSeed),
			Serial,
			static_cast<uint32>(LogicalStep),
			GetTypeHash(MaterialId));
		const int32 ParticleCount = FMath::Min(
			WorldPositions.Num(),
			CellCount);
		AirborneParticles.Reserve(
			AirborneParticles.Num() + ParticleCount);
		const int32 BaseCellsPerParticle = CellCount / ParticleCount;
		const int32 Remainder = CellCount % ParticleCount;
		for (int32 Index = 0; Index < ParticleCount; ++Index)
		{
			if (WorldPositions[Index].ContainsNaN())
			{
				continue;
			}
			FAirborneParticle& Particle =
				AirborneParticles.AddDefaulted_GetRef();
			const uint32 StableOrdinal = static_cast<uint32>(Index) + 1u;
			Particle.ParticleId = FGuid(
				BatchId.A ^ 0x9e3779b9u,
				BatchId.B ^ StableOrdinal * 0x85ebca6bu,
				BatchId.C ^ StableOrdinal * 0xc2b2ae35u,
				BatchId.D ^ StableOrdinal * 0x27d4eb2fu);
			Particle.BatchId = BatchId;
			Particle.MaterialId = MaterialId;
			Particle.WorldPosition = WorldPositions[Index];
			Particle.Velocity = InitialVelocities.IsValidIndex(Index)
				&& !InitialVelocities[Index].ContainsNaN()
				? InitialVelocities[Index]
				: FVector::ZeroVector;
			Particle.Radius = Radius;
			Particle.GravityScale = FMath::Clamp(GravityScale, 0.0f, 4.0f);
			Particle.RemainingLifetime = FMath::Clamp(Lifetime, 0.05f, 30.0f);
			Particle.CellCount = BaseCellsPerParticle
				+ (Index < Remainder ? 1 : 0);
			Particle.ConservedMaterialAmount =
				Particle.CellCount * ConservedAmountPerCell;
			Particle.Energy = SpecificEnergy == INDEX_NONE
				? DefaultMaterialEnergies.FindRef(MaterialId)
				: static_cast<uint16>(SpecificEnergy);
			Particle.EventSeed = EventSeed;
		}
		bReplicationDirty |= HasAirborneParticleBatch(BatchId);
		return HasAirborneParticleBatch(BatchId) ? BatchId : FGuid();
	}

	int32 FSimulationRuntime::AdvanceAirborneParticles(
		const float DeltaSeconds,
		const TFunctionRef<bool(FAirborneParticle&, float)> AdvanceParticle)
	{
		if (AirborneParticles.IsEmpty())
		{
			return 0;
		}
		const float StepSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 0.10f);
		int32 TransferredCount = 0;
		for (int32 Index = AirborneParticles.Num() - 1; Index >= 0; --Index)
		{
			if (!AdvanceParticle(AirborneParticles[Index], StepSeconds))
			{
				continue;
			}
			AirborneParticles.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			++TransferredCount;
		}
		bReplicationDirty |= TransferredCount > 0;
		return TransferredCount;
	}

	void FSimulationRuntime::GetAirborneParticlesForBatch(
		const FGuid& BatchId,
		TArray<FAirborneParticle>& OutParticles) const
	{
		OutParticles.Reset();
		if (!BatchId.IsValid())
		{
			return;
		}
		for (const FAirborneParticle& Particle : AirborneParticles)
		{
			if (Particle.BatchId == BatchId)
			{
				OutParticles.Add(Particle);
			}
		}
	}

	bool FSimulationRuntime::GetAirborneParticleBounds(
		FBox& OutBounds,
		float& OutMaximumRadius,
		float& OutMaximumSpeed,
		float& OutMaximumGravityScale) const
	{
		OutBounds = FBox(ForceInit);
		OutMaximumRadius = 0.0f;
		OutMaximumSpeed = 0.0f;
		OutMaximumGravityScale = 0.0f;
		for (const FAirborneParticle& Particle : AirborneParticles)
		{
			if (Particle.WorldPosition.ContainsNaN())
			{
				continue;
			}
			OutBounds += Particle.WorldPosition;
			OutMaximumRadius = FMath::Max(
				OutMaximumRadius,
				Particle.Radius);
			OutMaximumSpeed = FMath::Max(
				OutMaximumSpeed,
				Particle.Velocity.Size());
			OutMaximumGravityScale = FMath::Max(
				OutMaximumGravityScale,
				FMath::Abs(Particle.GravityScale));
		}
		return OutBounds.IsValid != 0;
	}

	bool FSimulationRuntime::HasAirborneParticleBatch(
		const FGuid& BatchId) const
	{
		return BatchId.IsValid()
			&& AirborneParticles.ContainsByPredicate(
				[&BatchId](const FAirborneParticle& Particle)
				{
					return Particle.BatchId == BatchId;
				});
	}

	int32 FSimulationRuntime::CountAirborneParticles(
		const FName MaterialId) const
	{
		int32 Count = 0;
		for (const FAirborneParticle& Particle : AirborneParticles)
		{
			Count += MaterialId.IsNone()
				|| Particle.MaterialId == MaterialId;
		}
		return Count;
	}

	int64 FSimulationRuntime::SumAirborneMaterialAmount(
		const FName MaterialId) const
	{
		int64 Amount = 0;
		for (const FAirborneParticle& Particle : AirborneParticles)
		{
			if (Particle.MaterialId == MaterialId)
			{
				Amount += Particle.ConservedMaterialAmount;
			}
		}
		return Amount;
	}

	int64 FSimulationRuntime::RemoveAirborneParticles(
		const TFunctionRef<bool(const FAirborneParticle&)> ShouldRemove)
	{
		int64 RemovedAmount = 0;
		for (int32 Index = AirborneParticles.Num() - 1; Index >= 0; --Index)
		{
			const FAirborneParticle& Particle = AirborneParticles[Index];
			if (!ShouldRemove(Particle))
			{
				continue;
			}
			RemovedAmount += Particle.ConservedMaterialAmount;
			AirborneParticles.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		}
		bReplicationDirty |= RemovedAmount > 0;
		return RemovedAmount;
	}

	int32 FSimulationRuntime::MaterializePendingReactionEmissions(
		const TFunctionRef<FVector(const FIntVector&)> ResolveWorldPosition)
	{
		if (!MaterialWorld)
		{
			return 0;
		}
		TArray<FReactionEmission> Emissions;
		MaterialWorld->ConsumeReactionEmissions(Emissions);
		return MaterializeReactionEmissions(Emissions, ResolveWorldPosition);
	}

	int32 FSimulationRuntime::MaterializeReactionEmissions(
		const TConstArrayView<FReactionEmission> Emissions,
		const TFunctionRef<FVector(const FIntVector&)> ResolveWorldPosition)
	{
		int32 MaterializedCount = 0;
		for (const FReactionEmission& Emission : Emissions)
		{
			const FVector WorldPosition = ResolveWorldPosition(Emission.GridCell);
			if (!Emission.ParticleId.IsValid()
				|| Emission.MaterialId.IsNone()
				|| Emission.Amount == 0
				|| WorldPosition.ContainsNaN()
				|| AirborneParticles.ContainsByPredicate(
					[&Emission](const FAirborneParticle& Existing)
					{
						return Existing.ParticleId == Emission.ParticleId;
					}))
			{
				continue;
			}
			FAirborneParticle& Particle = AirborneParticles.AddDefaulted_GetRef();
			Particle.ParticleId = Emission.ParticleId;
			Particle.MaterialId = Emission.MaterialId;
			Particle.WorldPosition = WorldPosition;
			Particle.Radius = 2.0f;
			Particle.RemainingLifetime = Emission.RemainingLifetimeSteps > 0
				? Emission.RemainingLifetimeSteps * RuntimeSettings.StepSeconds
				: 1.0f;
			Particle.CellCount = FMath::Max(
				1,
				FMath::DivideAndRoundUp(
					static_cast<int32>(Emission.Amount), 255));
			Particle.ConservedMaterialAmount = Emission.Amount;
			Particle.Energy = Emission.Energy;
			Particle.EventSeed = static_cast<int32>(
				GetTypeHash(Emission.ParticleId));
			++MaterializedCount;
		}
		bReplicationDirty |= MaterializedCount > 0;
		return MaterializedCount;
	}

	bool FSimulationRuntime::ReactAirborneParticleAt(
		const FIntPoint& WorldCell,
		const FGuid& ParticleId,
		FString& OutError)
	{
		OutError.Reset();
		if (!MaterialWorld || !ParticleId.IsValid())
		{
			OutError = TEXT("material world or airborne particle identity is invalid");
			return false;
		}
		const int32 ParticleIndex = AirborneParticles.IndexOfByPredicate(
			[&ParticleId](const FAirborneParticle& Particle)
			{
				return Particle.ParticleId == ParticleId;
			});
		if (!AirborneParticles.IsValidIndex(ParticleIndex))
		{
			OutError = TEXT("airborne material element does not exist");
			return false;
		}

		FAirborneParticle& Particle = AirborneParticles[ParticleIndex];
		if (!ReactAirborneParticleAt(WorldCell, Particle, OutError))
		{
			return false;
		}
		if (Particle.ConservedMaterialAmount == 0)
		{
			AirborneParticles.RemoveAtSwap(
				ParticleIndex, 1, EAllowShrinking::No);
		}
		bReplicationDirty = true;
		return true;
	}

	bool FSimulationRuntime::ReactAirborneParticleAt(
		const FIntPoint& WorldCell,
		FAirborneParticle& Particle,
		FString& OutError)
	{
		OutError.Reset();
		if (!MaterialWorld || !Particle.ParticleId.IsValid())
		{
			OutError = TEXT("material world or airborne particle identity is invalid");
			return false;
		}
		if (Particle.ConservedMaterialAmount <= 0
			|| Particle.ConservedMaterialAmount > MAX_uint16)
		{
			OutError = TEXT("airborne material amount is outside the local element range");
			return false;
		}
		uint16 PacketAmount = static_cast<uint16>(
			Particle.ConservedMaterialAmount);
		FName PacketMaterial = Particle.MaterialId;
		uint16 PacketEnergy = Particle.Energy;
		if (!MaterialWorld->ReactAirborneParticleAt(
				WorldCell,
				Particle.ParticleId,
				PacketMaterial,
				PacketAmount,
				PacketEnergy,
				OutError))
		{
			return false;
		}
		Particle.MaterialId = PacketMaterial;
		Particle.ConservedMaterialAmount = PacketAmount;
		Particle.Energy = PacketEnergy;
		bReplicationDirty = true;
		return true;
	}

	bool FSimulationRuntime::SetCell(
		const FIntPoint& WorldCell,
		const FName MaterialId)
	{
		const bool bChanged = MaterialWorld
			&& MaterialWorld->SetCell(WorldCell, MaterialId);
		bReplicationDirty |= bChanged;
		return bChanged;
	}

	bool FSimulationRuntime::SetCellAmount(
		const FIntPoint& WorldCell,
		const FName MaterialId,
		const uint16 Amount)
	{
		return SetCellAmount(
			WorldCell,
			MaterialId,
			Amount,
			DefaultMaterialEnergies.FindRef(MaterialId));
	}

	bool FSimulationRuntime::SetCellAmount(
		const FIntPoint& WorldCell,
		const FName MaterialId,
		const uint16 Amount,
		const uint16 Energy)
	{
		const bool bChanged = MaterialWorld
			&& MaterialWorld->SetCellAmount(
				WorldCell,
				MaterialId,
				Amount,
				Energy);
		bReplicationDirty |= bChanged;
		return bChanged;
	}

	int32 FSimulationRuntime::AddCellAmount(
		const FIntPoint& WorldCell,
		const FName MaterialId,
		const uint16 Amount)
	{
		return AddCellAmount(
			WorldCell,
			MaterialId,
			Amount,
			DefaultMaterialEnergies.FindRef(MaterialId));
	}

	int32 FSimulationRuntime::AddCellAmount(
		const FIntPoint& WorldCell,
		const FName MaterialId,
		const uint16 Amount,
		const uint16 Energy)
	{
		const int32 Accepted = MaterialWorld
			? MaterialWorld->AddCellAmount(
				WorldCell, MaterialId, Amount, Energy)
			: 0;
		bReplicationDirty |= Accepted > 0;
		return Accepted;
	}

	int32 FSimulationRuntime::AddPowderAmountAtStableSurface(
		const FIntPoint& ImpactCell,
		const FName MaterialId,
		const uint16 Amount,
		const int32 MaximumTravelCells,
		FIntPoint& OutDestinationCell)
	{
		const int32 Accepted = MaterialWorld
			? MaterialWorld->AddPowderAmountAtStableSurface(
				ImpactCell,
				MaterialId,
				Amount,
				MaximumTravelCells,
				OutDestinationCell)
			: 0;
		bReplicationDirty |= Accepted > 0;
		return Accepted;
	}

	bool FSimulationRuntime::SetExternalSupportHeight(
		const FIntPoint& WorldCell,
		const int32 Height)
	{
		const bool bChanged = MaterialWorld.IsValid()
			&& MaterialWorld->SetExternalSupportHeight(WorldCell, Height);
		bReplicationDirty |= bChanged;
		return bChanged;
	}

	bool FSimulationRuntime::SetSupportHeight(
		const FIntPoint& WorldCell,
		const int32 Height)
	{
		const bool bChanged = MaterialWorld.IsValid()
			&& MaterialWorld->SetSupportHeight(WorldCell, Height);
		bReplicationDirty |= bChanged;
		return bChanged;
	}

	bool FSimulationRuntime::ClearExternalSupportHeight(
		const FIntPoint& WorldCell)
	{
		const bool bChanged = MaterialWorld.IsValid()
			&& MaterialWorld->ClearExternalSupportHeight(WorldCell);
		bReplicationDirty |= bChanged;
		return bChanged;
	}

	FName FSimulationRuntime::GetMaterialAt(
		const FIntPoint& WorldCell) const
	{
		return MaterialWorld
			? MaterialWorld->GetMaterialAt(WorldCell)
			: NAME_None;
	}

	uint16 FSimulationRuntime::GetMaterialAmountAt(
		const FIntPoint& WorldCell,
		const FName MaterialId) const
	{
		return MaterialWorld
			? MaterialWorld->GetMaterialAmountAt(WorldCell, MaterialId)
			: 0;
	}

	bool FSimulationRuntime::TryGetCellSnapshot(
		const FIntPoint& WorldCell,
		FCellSnapshot& OutSnapshot) const
	{
		OutSnapshot = {};
		return MaterialWorld
			&& MaterialWorld->TryGetCellSnapshot(WorldCell, OutSnapshot);
	}

	int32 FSimulationRuntime::CountMaterial(const FName MaterialId) const
	{
		// Preserve the established meaning of this query: occupied settled cells.
		// Airborne facts have an explicit particle-count query, while total amount
		// intentionally spans both states for conservation audits.
		return MaterialWorld ? MaterialWorld->CountMaterial(MaterialId) : 0;
	}

	int64 FSimulationRuntime::SumMaterialAmount(const FName MaterialId) const
	{
		return MaterialWorld
			? MaterialWorld->SumMaterialAmount(MaterialId)
				+ SumAirborneMaterialAmount(MaterialId)
			: 0;
	}

	int32 FSimulationRuntime::GetResidentChunkCount() const
	{
		return MaterialWorld ? MaterialWorld->GetResidentChunkCount() : 0;
	}

	int32 FSimulationRuntime::GetArchivedChunkCount() const
	{
		return MaterialWorld ? MaterialWorld->GetArchivedChunkCount() : 0;
	}

	int32 FSimulationRuntime::GetSimulationFocusCount() const
	{
		return MaterialWorld ? MaterialWorld->GetSimulationFocusCount() : 0;
	}

	void FSimulationRuntime::GetActiveCells(
		TArray<FCellSnapshot>& OutCells) const
	{
		OutCells.Reset();
		if (MaterialWorld)
		{
			MaterialWorld->GetActiveCells(OutCells);
		}
	}

	void FSimulationRuntime::GetAllCells(
		TArray<FCellSnapshot>& OutCells) const
	{
		OutCells.Reset();
		if (MaterialWorld)
		{
			MaterialWorld->GetAllCells(OutCells);
		}
	}

	void FSimulationRuntime::GetResidentCells(
		TArray<FCellSnapshot>& OutCells) const
	{
		OutCells.Reset();
		if (MaterialWorld)
		{
			MaterialWorld->GetResidentCells(OutCells);
		}
	}

	void FSimulationRuntime::GetCellsInChunks(
		const TConstArrayView<FIntPoint> Chunks,
		TArray<FCellSnapshot>& OutCells) const
	{
		OutCells.Reset();
		if (MaterialWorld)
		{
			MaterialWorld->GetCellsInChunks(Chunks, OutCells);
		}
	}

	void FSimulationRuntime::ConsumeProjectionDirtyChunks(
		TArray<FIntPoint>& OutChunks)
	{
		OutChunks.Reset();
		if (MaterialWorld)
		{
			MaterialWorld->ConsumeProjectionDirtyChunks(OutChunks);
		}
	}
}
