#include "Game/MatterFluxCharacterMovementComponent.h"

#include "Components/PrimitiveComponent.h"

void UMatterFluxCharacterMovementComponent::ApplyImpactPhysicsForces(
	const FHitResult& Impact,
	const FVector& ImpactAcceleration,
	const FVector& ImpactVelocity)
{
	// A walkable hit becomes the character's movement base. Pushing that body
	// here and then following it in UpdateBasedMovement creates a feedback loop.
	// Non-walkable (side) hits retain UE's normal bounded push interaction.
	if (IsWalkable(Impact))
	{
		return;
	}

	Super::ApplyImpactPhysicsForces(
		Impact,
		ImpactAcceleration,
		ImpactVelocity);
}

void UMatterFluxCharacterMovementComponent::UpdateBasedMovement(
	const float DeltaSeconds)
{
	const UPrimitiveComponent* MovementBase =
		Cast<UPrimitiveComponent>(GetMovementBaseObject());
	if (MovementBase
		&& MovementBase->IsSimulatingPhysics()
		&& MovementBase->GetMass() < Mass)
	{
		// A body lighter than the character is debris/support, not a vehicle.
		// Following every post-physics transform of such a base transfers Chaos
		// contact jitter directly to the character and its camera. Floor checks
		// still keep the character supported, while heavier moving platforms keep
		// UE's normal based-movement behavior.
		return;
	}

	Super::UpdateBasedMovement(DeltaSeconds);
}
