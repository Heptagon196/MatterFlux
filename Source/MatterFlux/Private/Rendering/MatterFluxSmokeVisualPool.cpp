#include "Rendering/MatterFluxSmokeVisualPool.h"

namespace MatterFlux::Rendering
{
	namespace
	{
		uint32 MixSmokeHash(uint32 Value)
		{
			Value ^= Value >> 16u;
			Value *= 0x7feb352du;
			Value ^= Value >> 15u;
			Value *= 0x846ca68bu;
			Value ^= Value >> 16u;
			return Value;
		}

		float HashUnitFloat(const uint32 Value)
		{
			return static_cast<float>(MixSmokeHash(Value) & 0xffffu)
				/ 65535.0f;
		}
	}

	FSmokeEmissionAnchor::FSmokeEmissionAnchor() = default;
	FSmokeVisualSettings::FSmokeVisualSettings() = default;
	FSmokeVisualPool::FSmokeVisualPool() = default;
	FSmokeVisualPool::~FSmokeVisualPool() = default;

	bool FSmokeEmissionAnchor::IsValid() const
	{
		return !WorldPosition.ContainsNaN()
			&& FMath::IsFinite(CellSize)
			&& CellSize > 0.0f
			&& FMath::IsFinite(EmissionProbability)
			&& EmissionProbability >= 0.0f
			&& EmissionProbability <= 1.0f;
	}

	bool FSmokeVisualSettings::IsValid() const
	{
		return FMath::IsFinite(SpawnIntervalSeconds)
			&& SpawnIntervalSeconds > 0.0f
			&& FMath::IsFinite(MinimumLifetimeSeconds)
			&& MinimumLifetimeSeconds > SpawnIntervalSeconds
			&& FMath::IsFinite(MaximumLifetimeSeconds)
			&& MaximumLifetimeSeconds >= MinimumLifetimeSeconds
			&& MaximumParticles > 0
			&& MaximumParticles <= 8192
			&& MaximumNewParticlesPerStep > 0
			&& MaximumNewParticlesPerStep <= MaximumParticles
			&& VoxelsPerParticle >= 3
			&& VoxelsPerParticle <= 8;
	}

	bool FSmokeVisualPool::Configure(
		const FSmokeVisualSettings& InSettings)
	{
		if (!InSettings.IsValid())
		{
			return false;
		}
		Settings = InSettings;
		Reset();
		return true;
	}

	void FSmokeVisualPool::Reset()
	{
		Anchors.Reset();
		Particles.Reset();
		SpawnAccumulator = 0.0f;
		SpawnSequence = 0;
	}

	void FSmokeVisualPool::SetEmissionAnchors(
		const TConstArrayView<FSmokeEmissionAnchor> InAnchors)
	{
		Anchors.Reset(InAnchors.Num());
		for (const FSmokeEmissionAnchor& Anchor : InAnchors)
		{
			if (Anchor.IsValid() && Anchor.EmissionProbability > 0.0f)
			{
				Anchors.Add(Anchor);
			}
		}
		Anchors.Sort([](
			const FSmokeEmissionAnchor& Left,
			const FSmokeEmissionAnchor& Right)
		{
			if (Left.Seed != Right.Seed)
			{
				return Left.Seed < Right.Seed;
			}
			if (!FMath::IsNearlyEqual(
				Left.WorldPosition.Z,
				Right.WorldPosition.Z))
			{
				return Left.WorldPosition.Z < Right.WorldPosition.Z;
			}
			if (!FMath::IsNearlyEqual(
				Left.WorldPosition.X,
				Right.WorldPosition.X))
			{
				return Left.WorldPosition.X < Right.WorldPosition.X;
			}
			return Left.WorldPosition.Y < Right.WorldPosition.Y;
		});
	}

	void FSmokeVisualPool::Advance(const float DeltaSeconds)
	{
		if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f)
		{
			return;
		}
		const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
		for (int32 Index = Particles.Num() - 1; Index >= 0; --Index)
		{
			FParticle& Particle = Particles[Index];
			Particle.AgeSeconds += ClampedDelta;
			if (Particle.AgeSeconds >= Particle.LifetimeSeconds)
			{
				Particles.RemoveAtSwap(Index, 1, EAllowShrinking::No);
				continue;
			}
			Particle.Position += Particle.Velocity * ClampedDelta;
			const float Buoyancy = 5.0f + Particle.BaseSize * 0.25f;
			Particle.Velocity.Z += Buoyancy * ClampedDelta;
			Particle.Velocity.X *= FMath::Pow(0.94f, ClampedDelta);
			Particle.Velocity.Y *= FMath::Pow(0.94f, ClampedDelta);
		}

		SpawnAccumulator += ClampedDelta;
		int32 SpawnSteps = 0;
		while (SpawnAccumulator + UE_SMALL_NUMBER
				>= Settings.SpawnIntervalSeconds
			&& SpawnSteps < 3)
		{
			SpawnAccumulator -= Settings.SpawnIntervalSeconds;
			SpawnStep();
			++SpawnSteps;
		}
		if (SpawnSteps == 3
			&& SpawnAccumulator >= Settings.SpawnIntervalSeconds)
		{
			SpawnAccumulator = FMath::Fmod(
				SpawnAccumulator,
				Settings.SpawnIntervalSeconds);
		}
	}

	void FSmokeVisualPool::SpawnStep()
	{
		++SpawnSequence;
		int32 SpawnedThisStep = 0;
		for (int32 AnchorIndex = 0;
			AnchorIndex < Anchors.Num()
				&& SpawnedThisStep < Settings.MaximumNewParticlesPerStep
				&& Particles.Num() < Settings.MaximumParticles;
			++AnchorIndex)
		{
			const FSmokeEmissionAnchor& Anchor = Anchors[AnchorIndex];
			const uint32 Hash = MixSmokeHash(
				Anchor.Seed
					^ SpawnSequence * 0x9e3779b9u
					^ static_cast<uint32>(AnchorIndex) * 0x85ebca6bu);
			if (HashUnitFloat(Hash) > Anchor.EmissionProbability)
			{
				continue;
			}

			FParticle& Particle = Particles.AddDefaulted_GetRef();
			const float JitterX = HashUnitFloat(Hash ^ 0x18c253u) * 2.0f - 1.0f;
			const float JitterY = HashUnitFloat(Hash ^ 0x72de91u) * 2.0f - 1.0f;
			Particle.Position = Anchor.WorldPosition + FVector(
				JitterX * Anchor.CellSize * 0.25f,
				JitterY * Anchor.CellSize * 0.25f,
				Anchor.CellSize * 0.18f);
			Particle.Velocity = FVector(
				JitterX * FMath::Lerp(5.0f, 13.0f, HashUnitFloat(Hash ^ 0xa31u)),
				JitterY * FMath::Lerp(5.0f, 13.0f, HashUnitFloat(Hash ^ 0xb47u)),
				FMath::Lerp(34.0f, 56.0f, HashUnitFloat(Hash ^ 0xc59u)));
			Particle.AgeSeconds = 0.0f;
			Particle.LifetimeSeconds = FMath::Lerp(
				Settings.MinimumLifetimeSeconds,
				Settings.MaximumLifetimeSeconds,
				HashUnitFloat(Hash ^ 0xdf12u));
			Particle.BaseSize = Anchor.CellSize * FMath::Lerp(
				0.26f,
				0.38f,
				HashUnitFloat(Hash ^ 0xe281u));
			Particle.Seed = Hash;
			++SpawnedThisStep;
		}
	}

	void FSmokeVisualPool::BuildInstanceTransforms(
		TArray<FTransform>& OutTransforms) const
	{
		OutTransforms.Reset(
			Particles.Num() * Settings.VoxelsPerParticle);
		static const FVector ClusterOffsets[] = {
			FVector(0.00f, 0.00f, 0.00f),
			FVector(0.48f, 0.08f, 0.22f),
			FVector(-0.42f, 0.18f, 0.35f),
			FVector(0.10f, -0.46f, 0.48f),
			FVector(-0.18f, 0.38f, 0.66f),
			FVector(0.36f, -0.28f, 0.78f),
			FVector(-0.36f, -0.22f, 0.88f),
			FVector(0.08f, 0.30f, 1.00f)
		};
		for (const FParticle& Particle : Particles)
		{
			const float NormalizedAge = FMath::Clamp(
				Particle.AgeSeconds / Particle.LifetimeSeconds,
				0.0f,
				1.0f);
			const float Expansion = FMath::Lerp(
				0.62f,
				1.75f,
				FMath::Min(NormalizedAge / 0.65f, 1.0f));
			const float ExitScale = NormalizedAge <= 0.78f
				? 1.0f
				: FMath::Clamp(
					(1.0f - NormalizedAge) / 0.22f,
					0.0f,
					1.0f);
			const float RotationRadians =
				HashUnitFloat(Particle.Seed ^ 0x641au) * 2.0f * PI;
			const float SinRotation = FMath::Sin(RotationRadians);
			const float CosRotation = FMath::Cos(RotationRadians);
			for (int32 LobeIndex = 0;
				LobeIndex < Settings.VoxelsPerParticle;
				++LobeIndex)
			{
				const FVector Offset = ClusterOffsets[LobeIndex];
				const FVector RotatedOffset(
					Offset.X * CosRotation - Offset.Y * SinRotation,
					Offset.X * SinRotation + Offset.Y * CosRotation,
					Offset.Z);
				const float LobeVariation = FMath::Lerp(
					0.72f,
					1.12f,
					HashUnitFloat(
						Particle.Seed
							^ static_cast<uint32>(LobeIndex) * 0x9e37u));
				const float LobeSize = Particle.BaseSize
					* Expansion
					* ExitScale
					* LobeVariation;
				OutTransforms.Emplace(
					FRotator::ZeroRotator,
					Particle.Position
						+ RotatedOffset * Particle.BaseSize * Expansion,
					FVector(LobeSize / 100.0f));
			}
		}
	}

	const FVector& FSmokeVisualPool::GetParticlePosition(
		const int32 Index) const
	{
		static const FVector Zero = FVector::ZeroVector;
		return Particles.IsValidIndex(Index)
			? Particles[Index].Position
			: Zero;
	}
}
