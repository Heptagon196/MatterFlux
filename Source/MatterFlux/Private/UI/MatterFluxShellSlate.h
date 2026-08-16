#pragma once

#include "CoreMinimal.h"

class SWidget;
class UMatterFluxShellWidget;
struct FMatterFluxSaveSlotInfo;

namespace MatterFluxShellUI
{
	/** Builds the private Slate implementation behind the UMG adapter. */
	TSharedRef<SWidget> CreateShell(
		TWeakObjectPtr<UMatterFluxShellWidget> OwnerWidget);

	/** Refreshes a shell previously returned by CreateShell. */
	void RefreshShell(const TSharedPtr<SWidget>& Shell);

	/** Canonical user-facing save-slot name used by view and edit state. */
	FString GetSlotDisplayName(const FMatterFluxSaveSlotInfo& Slot);
}
