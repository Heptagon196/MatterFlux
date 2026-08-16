#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ForgeBindingRegistry.h"
#include "ForgePatch.h"
#include "ForgeObjectRegistry.h"

class IForgeBindingRegistry;
class IForgeObjectRegistry;

class FORGERUNTIME_API IForgeRuntimeModule : public IModuleInterface
{
public:
    static IForgeRuntimeModule& Get()
    {
        return FModuleManager::LoadModuleChecked<IForgeRuntimeModule>("ForgeRuntime");
    }

    static bool IsAvailable()
    {
        return FModuleManager::Get().IsModuleLoaded("ForgeRuntime");
    }

    virtual IForgeBindingRegistry& GetBindingRegistry() = 0;
    virtual IForgeObjectRegistry& GetObjectRegistry() = 0;
    virtual FForgeObjectRegistrationResult RegisterBorrowedObject(
        void* Address,
        const FString& TypeId,
        bool bReadOnly,
        const FString& ExpectedBuildFingerprint) = 0;
    virtual FForgeInvokeResult ExecuteScript(
        const FString& ScriptSource,
        const FString& EntryFunction,
        const FForgeTypeRef& ReturnType,
        TConstArrayView<FForgeTypeRef> ParameterTypes,
        TConstArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn) = 0;
    virtual FForgePatchResult ApplyPatch(
        const FString& SymbolId,
        const FString& ScriptSource,
        const FString& EntryFunction,
        EForgePatchMode Mode,
        const FString& BuildFingerprint) = 0;
    virtual void DisablePatch(const FString& SymbolId) = 0;
};
