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
	}
}
