#include "Game/MatterFluxPlayerOperation.h"

namespace MatterFlux::PlayerOperations
{
	FMatterFluxPlayerOperationDelegate& OnApplied()
	{
		static FMatterFluxPlayerOperationDelegate Delegate;
		return Delegate;
	}
}
