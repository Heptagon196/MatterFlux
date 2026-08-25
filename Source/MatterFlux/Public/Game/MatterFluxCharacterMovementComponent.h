#pragma once

#include "GameFramework/CharacterMovementComponent.h"
#include "MatterFluxCharacterMovementComponent.generated.h"

/**
 * Character movement policy shared by the player and creatures.
 *
 * MatterFlux characters may walk on the same lightweight simulated bodies
 * they can push from the side. UE's stock movement component treats a landing
 * as a generic physics impact, which launches those bodies before based
 * movement starts following them.
 */
UCLASS()
class MATTERFLUX_API UMatterFluxCharacterMovementComponent final
	: public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	/** Weighted liquid/powder resistance sampled before this movement tick. */
	void SetMaterialMovementResistance(float NewResistance);
	float GetMaterialMovementResistance() const
	{
		return MaterialMovementResistance;
	}

	virtual float GetMaxSpeed() const override;
	virtual float GetMaxAcceleration() const override;
	virtual void ApplyImpactPhysicsForces(
		const FHitResult& Impact,
		const FVector& ImpactAcceleration,
		const FVector& ImpactVelocity) override;
	virtual void UpdateBasedMovement(float DeltaSeconds) override;

private:
	float MaterialMovementResistance = 0.0f;
};
