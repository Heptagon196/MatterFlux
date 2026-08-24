#pragma once

#include "GameFramework/CharacterMovementComponent.h"

namespace MatterFlux::CharacterPhysics
{
	/**
	 * Player and AI characters must push Chaos bodies through one bounded
	 * profile. UE's defaults are intentionally tuned for heavy 3D props
	 * (750000 continuous force) and launch MatterFlux's light voxel fragments.
	 */
	inline void ConfigurePhysicsInteraction(
		UCharacterMovementComponent& Movement)
	{
		Movement.bEnablePhysicsInteraction = true;
		Movement.bPushForceScaledToMass = true;
		Movement.bScalePushForceToVelocity = true;
		Movement.bPushForceUsingZOffset = true;
		Movement.PushForcePointZOffsetFactor = -0.25f;

		// With mass scaling these values are accelerations in practice: the first
		// contact starts the body gently and sustained walking can bring it up to
		// ordinary character speed without a single-frame launch.
		Movement.InitialPushForceFactor = 100.0f;
		Movement.PushForceFactor = 1500.0f;

		Movement.bTouchForceScaledToMass = true;
		Movement.TouchForceFactor = 0.10f;
		Movement.MinTouchForce = -1.0f;
		Movement.MaxTouchForce = -1.0f;
		Movement.RepulsionForce = 0.0f;

		// CharacterMovement's default standing load applies the character's
		// full weight every frame to its simulated movement base. Cut products
		// can legitimately weigh only 0.5 kg, so that load accelerates the body
		// while the same body moves the character back through based movement,
		// producing a violent feedback loop. Side contact still uses the bounded
		// push/touch forces above; standing must not inject another force path.
		Movement.StandingDownwardForceScale = 0.0f;
	}
}
