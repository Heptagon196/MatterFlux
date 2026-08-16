#include "IForgeRuntimeModule.h"

#include "ForgeBindingRegistry.h"
#include "ForgeObjectRegistry.h"
#include "ForgePatchManager.h"
#include "ForgeWorkspaceWatcher.h"

TUniquePtr<IForgeBindingRegistry> CreateForgeBindingRegistry();
TUniquePtr<IForgeObjectRegistry> CreateForgeObjectRegistry();

namespace
{
IForgeBindingRegistry* GForgeRegistry = nullptr;
FForgePatchManager* GForgePatchManager = nullptr;
TArray<FForgeModuleBindings> GPendingBindings;
}

class FForgeRuntimeModule final : public IForgeRuntimeModule
{
public:
    virtual void StartupModule() override
    {
        Registry = CreateForgeBindingRegistry();
        ObjectRegistry = CreateForgeObjectRegistry();
        GForgeRegistry = Registry.Get();
        PatchManager = MakeUnique<FForgePatchManager>(*Registry, *ObjectRegistry);
        const FForgePatchResult PatchResult = PatchManager->Initialize();
        ensureMsgf(PatchResult.bSuccess, TEXT("Forge PatchManager initialization failed: %s"), *PatchResult.Error);
        GForgePatchManager = PatchManager.Get();
        for (const FForgeModuleBindings& Bindings : GPendingBindings)
        {
            Registry->RegisterModule(Bindings);
        }
        GPendingBindings.Reset();
        WorkspaceWatcher = MakeUnique<FForgeWorkspaceWatcher>(*PatchManager);
        WorkspaceWatcher->Start();
    }

    virtual void ShutdownModule() override
    {
        GForgePatchManager = nullptr;
        WorkspaceWatcher.Reset();
        if (PatchManager.IsValid())
        {
            PatchManager->Shutdown();
            PatchManager.Reset();
        }
        GForgeRegistry = nullptr;
        ObjectRegistry.Reset();
        Registry.Reset();
    }

    virtual IForgeObjectRegistry& GetObjectRegistry() override
    {
        check(ObjectRegistry.IsValid());
        return *ObjectRegistry;
    }

    virtual FForgeObjectRegistrationResult RegisterBorrowedObject(
        void* Address,
        const FString& TypeId,
        bool bReadOnly,
        const FString& ExpectedBuildFingerprint) override
    {
        if (!Registry.IsValid() || !ObjectRegistry.IsValid())
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Forge Runtime registries are unavailable"));
        }
        const FForgeTypeBinding* Type = Registry->FindTypeById(TypeId);
        if (Type == nullptr || Type->Kind != EForgeValueType::NativeObject)
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Forge native class TypeId is not registered"));
        }
        if (ExpectedBuildFingerprint.IsEmpty() ||
            ExpectedBuildFingerprint != Type->BuildFingerprint)
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Forge native class Build Fingerprint does not match"));
        }
        const EForgeInstanceCapability RequiredCapability = bReadOnly
            ? EForgeInstanceCapability::BorrowedRead
            : EForgeInstanceCapability::BorrowedWrite;
        if (!EnumHasAllFlags(Type->InstanceCapabilities, RequiredCapability))
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Forge native class does not support the requested borrowed access"));
        }
        return ObjectRegistry->RegisterBorrowed(
            Address,
            FName(Type->TypeId),
            FName(Type->QualifiedCppName),
            bReadOnly);
    }

    virtual FForgeInvokeResult ExecuteScript(
        const FString& ScriptSource,
        const FString& EntryFunction,
        const FForgeTypeRef& ReturnType,
        TConstArrayView<FForgeTypeRef> ParameterTypes,
        TConstArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn) override
    {
        return PatchManager.IsValid()
            ? PatchManager->ExecuteScript(
                ScriptSource,
                EntryFunction,
                ReturnType,
                ParameterTypes,
                Arguments,
                OutReturn)
            : FForgeInvokeResult::Failure(TEXT("Forge PatchManager is not initialized"));
    }

    virtual IForgeBindingRegistry& GetBindingRegistry() override
    {
        check(Registry.IsValid());
        return *Registry;
    }

    virtual FForgePatchResult ApplyPatch(
        const FString& SymbolId,
        const FString& ScriptSource,
        const FString& EntryFunction,
        EForgePatchMode Mode,
        const FString& BuildFingerprint) override
    {
        return PatchManager.IsValid()
            ? PatchManager->ApplyPatch(SymbolId, ScriptSource, EntryFunction, Mode, BuildFingerprint)
            : FForgePatchResult::Failure(TEXT("Forge PatchManager is not initialized"));
    }

    virtual void DisablePatch(const FString& SymbolId) override
    {
        if (PatchManager.IsValid())
        {
            PatchManager->DisablePatch(SymbolId);
        }
    }

private:
    TUniquePtr<IForgeBindingRegistry> Registry;
    TUniquePtr<IForgeObjectRegistry> ObjectRegistry;
    TUniquePtr<FForgePatchManager> PatchManager;
    TUniquePtr<FForgeWorkspaceWatcher> WorkspaceWatcher;
};

void RegisterForgeModuleBindings(const FForgeModuleBindings& Bindings)
{
    if (GForgeRegistry != nullptr)
    {
        const FForgeRegistrationResult Result = GForgeRegistry->RegisterModule(Bindings);
        ensureMsgf(Result.bSuccess, TEXT("Failed to register Forge bindings: %s"), *Result.Error);
        return;
    }
    GPendingBindings.Add(Bindings);
}

FForgeInvokeResult InvokeForgePatchedFunction(
    const FString& SymbolId,
    void* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    return GForgePatchManager != nullptr
        ? GForgePatchManager->Invoke(SymbolId, Instance, Arguments, OutReturn)
        : FForgeInvokeResult::Failure(TEXT("Forge PatchManager is unavailable"));
}

IMPLEMENT_MODULE(FForgeRuntimeModule, ForgeRuntime)
