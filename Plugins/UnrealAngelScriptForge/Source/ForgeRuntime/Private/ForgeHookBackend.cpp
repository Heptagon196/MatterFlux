#include "ForgeHookBackend.h"

#include "MinHook.h"

FForgePatchResult FForgeHookBackend::Initialize()
{
    if (bInitialized)
    {
        return FForgePatchResult::Success();
    }
    const MH_STATUS Status = MH_Initialize();
    if (Status != MH_OK && Status != MH_ERROR_ALREADY_INITIALIZED)
    {
        return FForgePatchResult::Failure(
            FString::Printf(TEXT("MinHook initialization failed: %hs"), MH_StatusToString(Status)));
    }
    bInitialized = true;
    return FForgePatchResult::Success();
}

void FForgeHookBackend::Shutdown()
{
    if (bInitialized)
    {
        MH_Uninitialize();
        bInitialized = false;
    }
}

FForgePatchResult FForgeHookBackend::Create(
    void* TargetAddress,
    void* ReplacementAddress,
    FForgeCreatedHook& OutHook)
{
    check(IsInGameThread());
    if (!bInitialized || TargetAddress == nullptr || ReplacementAddress == nullptr)
    {
        return FForgePatchResult::Failure(TEXT("Invalid Hook creation request"));
    }
    void* OriginalAddress = nullptr;
    const MH_STATUS Status = MH_CreateHook(
        TargetAddress,
        ReplacementAddress,
        &OriginalAddress);
    if (Status != MH_OK)
    {
        return FForgePatchResult::Failure(
            FString::Printf(TEXT("MinHook create failed: %hs"), MH_StatusToString(Status)));
    }
    OutHook = { TargetAddress, OriginalAddress };
    return FForgePatchResult::Success();
}

FForgePatchResult FForgeHookBackend::Enable(const FForgeCreatedHook& Hook)
{
    check(IsInGameThread());
    const MH_STATUS Status = MH_EnableHook(Hook.TargetAddress);
    return Status == MH_OK
        ? FForgePatchResult::Success()
        : FForgePatchResult::Failure(
            FString::Printf(TEXT("MinHook enable failed: %hs"), MH_StatusToString(Status)));
}

void FForgeHookBackend::Remove(const FForgeCreatedHook& Hook)
{
    check(IsInGameThread());
    if (Hook.TargetAddress != nullptr)
    {
        MH_DisableHook(Hook.TargetAddress);
        MH_RemoveHook(Hook.TargetAddress);
    }
}
