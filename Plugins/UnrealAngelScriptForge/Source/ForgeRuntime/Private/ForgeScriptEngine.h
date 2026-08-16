#pragma once

#include "CoreMinimal.h"
#include "ForgeBindingRegistry.h"
#include "ForgeObjectRegistry.h"
#include "ForgePatch.h"

class asIScriptEngine;
class asIScriptFunction;
class asIStringFactory;

struct FForgeDelegateSubscription
{
    int32 Token = 0;
    FString ModuleName;
    FForgeDelegateBinding Binding;
    FForgeObjectHandle Owner;
    TWeakObjectPtr<UObject> UObjectOwner;
    FForgeDelegateHandle NativeHandle;
};

struct FForgeDelegateRuntimeState
{
    asIScriptEngine* Engine = nullptr;
    IForgeBindingRegistry* Registry = nullptr;
    IForgeObjectRegistry* ObjectRegistry = nullptr;
    int32 NextToken = 1;
    TMap<int32, FForgeDelegateSubscription> Subscriptions;
};

struct FForgeCompiledPatch
{
    FString ModuleName;
    asIScriptFunction* Function = nullptr;
    EForgePatchMode Mode = EForgePatchMode::Replace;
};

struct FForgeOwnedConstructorMetadata
{
    FForgeTypeBinding Type;
    FForgeOwnedConstructorBinding Constructor;
};

class FForgeScriptEngine
{
public:
    ~FForgeScriptEngine();
    FForgePatchResult Initialize(
        IForgeBindingRegistry& InRegistry,
        IForgeObjectRegistry& InObjectRegistry);
    void Shutdown();
    FForgePatchResult Compile(
        const FString& ScriptSource,
        const FString& EntryFunction,
        EForgePatchMode Mode,
        const FForgeFunctionBinding& Binding,
        FForgeCompiledPatch& OutPatch);
    void Release(FForgeCompiledPatch& Patch);
    FForgeInvokeResult ExecuteStandalone(
        const FString& ScriptSource,
        const FString& EntryFunction,
        const FForgeTypeRef& ReturnType,
        TConstArrayView<FForgeTypeRef> ParameterTypes,
        TConstArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn);
    FForgeInvokeResult Execute(
        const FForgeCompiledPatch& Patch,
        const FForgeFunctionBinding& Binding,
        void* OriginalAddress,
        void* Instance,
        TArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn);

private:
    asIScriptEngine* Engine = nullptr;
    asIStringFactory* StringFactory = nullptr;
    FString CompileMessages;
    uint64 NextModuleId = 1;
    IForgeBindingRegistry* Registry = nullptr;
    IForgeObjectRegistry* ObjectRegistry = nullptr;
    TMap<FString, TUniquePtr<FForgeFunctionBinding>> ScriptMethodBindings;
    TMap<FString, TUniquePtr<FForgeFieldBinding>> ScriptFieldBindings;
    TMap<FString, TUniquePtr<FForgeTypeBinding>> ScriptOwnedTypeBindings;
    TMap<FString, TUniquePtr<FForgeOwnedConstructorMetadata>> ScriptOwnedConstructorBindings;
    TMap<FString, TUniquePtr<FForgeDelegateBinding>> ScriptDelegateBindings;
    TUniquePtr<FForgeDelegateRuntimeState> DelegateState;
};
