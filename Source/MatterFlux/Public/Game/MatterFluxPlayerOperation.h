#pragma once

#include "CoreMinimal.h"
#include "MatterFluxPlayerOperation.generated.h"

class AMatterFluxCharacter;

/** A semantic gameplay operation, independent of physical input and recording. */
UENUM()
enum class EMatterFluxPlayerOperation : uint8
{
	Move,
	JumpStarted,
	JumpCompleted,
	CameraZoom,
	Cut,
	Flame,
	Regenerate,
	CastWand
};

DECLARE_MULTICAST_DELEGATE_FiveParams(
	FMatterFluxPlayerOperationDelegate,
	AMatterFluxCharacter&,
	EMatterFluxPlayerOperation,
	FVector2D,
	int32,
	bool);

namespace MatterFlux::PlayerOperations
{
	/**
	 * Observes operations when they enter the Character gameplay path.
	 * The delegate has no return value and cannot veto gameplay execution.
	 * bRelayedFromClient is true only for a server-side recording relay and
	 * prevents observers from sending the operation back to the server again.
	 */
	MATTERFLUX_API FMatterFluxPlayerOperationDelegate& OnApplied();
}
