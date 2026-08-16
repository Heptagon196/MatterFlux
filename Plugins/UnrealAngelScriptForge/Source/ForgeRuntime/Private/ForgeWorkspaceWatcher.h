#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class FForgePatchManager;

class FForgeWorkspaceWatcher
{
public:
    explicit FForgeWorkspaceWatcher(FForgePatchManager& InPatchManager);
    ~FForgeWorkspaceWatcher();

    void Start();
    void Stop();

private:
    bool Tick(float DeltaSeconds);
    bool LoadActivePatch();
    void DisableActivePatch();
    void WriteRuntimeStatus(
        const TCHAR* Status,
        const FString& RequestedRevision,
        const FString& ActiveRevisionValue,
        const FString& SymbolId,
        const FString& Error = FString());

    FForgePatchManager& PatchManager;
    FTSTicker::FDelegateHandle TickerHandle;
    FString WorkspaceRoot;
    FString ActivePatchFile;
    FString RuntimeStatusFile;
    FString ActiveRevision;
    FString ActiveSymbolId;
    FString LastObservedContent;
    bool bObservedFile = false;
};
