#pragma once

#include "CoreMinimal.h"
#include "ForgePatch.h"

struct FForgeCreatedHook
{
    void* TargetAddress = nullptr;
    void* OriginalAddress = nullptr;
};

class FForgeHookBackend
{
public:
    FForgePatchResult Initialize();
    void Shutdown();
    FForgePatchResult Create(
        void* TargetAddress,
        void* ReplacementAddress,
        FForgeCreatedHook& OutHook);
    FForgePatchResult Enable(const FForgeCreatedHook& Hook);
    void Remove(const FForgeCreatedHook& Hook);

private:
    bool bInitialized = false;
};
