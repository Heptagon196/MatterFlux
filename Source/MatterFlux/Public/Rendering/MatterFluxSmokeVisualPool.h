#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::Rendering
{
	/** One currently active surface that may emit cosmetic smoke. */
	struct MATTERFLUX_API FMaterialEmissionAnchor
	{
		FMaterialEmissionAnchor();

		FVector WorldPosition = FVector::ZeroVector;
		float CellSize = 0.0f;
		float EmissionProbability = 0.0f;
		uint32 Seed = 0;

		bool IsValid() const;
	};

	struct MATTERFLUX_API FSmokeVisualSettings
	{
		FSmokeVisualSettings();

		float SpawnIntervalSeconds = 0.16f;
		float MinimumLifetimeSeconds = 3.2f;
		float MaximumLifetimeSeconds = 4.8f;
		int32 MaximumParticles = 768;
		int32 MaximumNewParticlesPerStep = 32;
		int32 VoxelsPerParticle = 5;

		bool IsValid() const;
	};

	/**
	 * Client-only world smoke simulation. Gameplay reaction remains fully
	 * deterministic; this pool converts active surface anchors into a bounded,
	 * shared set of rising voxel smoke clusters.
	 */
	class MATTERFLUX_API FSmokeVisualPool
	{
	public:
		FSmokeVisualPool();
		~FSmokeVisualPool();

		bool Configure(const FSmokeVisualSettings& InSettings);
		void Reset();
		void SetEmissionAnchors(TConstArrayView<FMaterialEmissionAnchor> InAnchors);
		void Advance(float DeltaSeconds);
		void BuildInstanceTransforms(TArray<FTransform>& OutTransforms) const;

		int32 GetParticleCount() const { return Particles.Num(); }
		int32 GetAnchorCount() const { return Anchors.Num(); }
		const FVector& GetParticlePosition(int32 Index) const;

	private:
		struct FParticle
		{
			FVector Position = FVector::ZeroVector;
			FVector Velocity = FVector::ZeroVector;
			float AgeSeconds = 0.0f;
			float LifetimeSeconds = 0.0f;
			float BaseSize = 0.0f;
			uint32 Seed = 0;
		};

		void SpawnStep();

		FSmokeVisualSettings Settings;
		TArray<FMaterialEmissionAnchor> Anchors;
		TArray<FParticle> Particles;
		float SpawnAccumulator = 0.0f;
		uint32 SpawnSequence = 0;
	};
}
