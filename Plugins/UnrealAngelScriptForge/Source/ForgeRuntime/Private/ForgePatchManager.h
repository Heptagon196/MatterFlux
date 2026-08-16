#pragma once

#include "CoreMinimal.h"
#include "ForgeBindingRegistry.h"
#include "ForgeHookBackend.h"
#include "ForgeObjectRegistry.h"
#include "ForgeScriptEngine.h"

class FForgePatchManager
{
public:
    FForgePatchManager(
        IForgeBindingRegistry& InRegistry,
        IForgeObjectRegistry& InObjectRegistry);
    FForgePatchResult Initialize();
    void Shutdown();
    FForgePatchResult ApplyPatch(
        const FString& SymbolId,
        const FString& ScriptSource,
        const FString& EntryFunction,
        EForgePatchMode Mode,
        const FString& BuildFingerprint);
    void DisablePatch(const FString& SymbolId);
    FForgeInvokeResult ExecuteScript(
        const FString& ScriptSource,
        const FString& EntryFunction,
        const FForgeTypeRef& ReturnType,
        TConstArrayView<FForgeTypeRef> ParameterTypes,
        TConstArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn);
    FForgeInvokeResult Invoke(
        const FString& SymbolId,
        void* Instance,
        TArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn);

private:
    struct FActivePatch
    {
        const FForgeFunctionBinding* Binding = nullptr;
        FForgeCreatedHook Hook;
        FForgeCompiledPatch Script;
    };

    IForgeBindingRegistry& Registry;
    IForgeObjectRegistry& ObjectRegistry;
    FForgeHookBackend HookBackend;
    FForgeScriptEngine ScriptEngine;
    TMap<FString, FActivePatch> ActivePatches;
};
