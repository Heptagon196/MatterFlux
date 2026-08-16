#pragma once

#include "CoreMinimal.h"

class SWidget;
class UMatterFluxMagicWorkbenchWidget;

namespace MatterFluxMagicUI
{
	/** Builds the private Slate implementation behind the UMG adapter. */
	TSharedRef<SWidget> CreateWorkbench(
		TWeakObjectPtr<UMatterFluxMagicWorkbenchWidget> OwnerWidget);

	/** Refreshes a workbench previously returned by CreateWorkbench. */
	void RefreshWorkbench(const TSharedPtr<SWidget>& Workbench);
}
