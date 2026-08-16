#include "ForgePatchManager.h"

FForgePatchManager::FForgePatchManager(
    IForgeBindingRegistry& InRegistry,
    IForgeObjectRegistry& InObjectRegistry)
    : Registry(InRegistry)
    , ObjectRegistry(InObjectRegistry)
{
}

FForgePatchResult FForgePatchManager::Initialize()
{
    const FForgePatchResult HookResult = HookBackend.Initialize();
    if (!HookResult.bSuccess)
    {
        return HookResult;
    }
    return ScriptEngine.Initialize(Registry, ObjectRegistry);
}

void FForgePatchManager::Shutdown()
{
    for (TPair<FString, FActivePatch>& Pair : ActivePatches)
    {
        HookBackend.Remove(Pair.Value.Hook);
        ScriptEngine.Release(Pair.Value.Script);
    }
    ActivePatches.Reset();
    ScriptEngine.Shutdown();
    HookBackend.Shutdown();
}

FForgePatchResult FForgePatchManager::ApplyPatch(
    const FString& SymbolId,
    const FString& ScriptSource,
    const FString& EntryFunction,
    EForgePatchMode Mode,
    const FString& BuildFingerprint)
{
    check(IsInGameThread());
    const FForgeFunctionBinding* Binding = Registry.FindFunction(SymbolId);
    if (Binding == nullptr)
    {
        return FForgePatchResult::Failure(TEXT("Patch target SymbolId is not registered"));
    }
    if (!Binding->bPatchable || !Binding->bOriginalCallable)
    {
        return FForgePatchResult::Failure(TEXT("Patch target is not Patchable"));
    }
    if (BuildFingerprint.IsEmpty() || Binding->BuildFingerprint != BuildFingerprint)
    {
        return FForgePatchResult::Failure(TEXT("Patch Build Fingerprint does not match the loaded binding"));
    }

    FForgeCompiledPatch CompiledPatch;
    const FForgePatchResult CompileResult = ScriptEngine.Compile(
        ScriptSource,
        EntryFunction,
        Mode,
        *Binding,
        CompiledPatch);
    if (!CompileResult.bSuccess)
    {
        return CompileResult;
    }

    if (FActivePatch* Existing = ActivePatches.Find(SymbolId))
    {
        FForgeCompiledPatch PreviousScript = MoveTemp(Existing->Script);
        Existing->Script = MoveTemp(CompiledPatch);
        ScriptEngine.Release(PreviousScript);
        return FForgePatchResult::Success();
    }

    FForgeCreatedHook Hook;
    const FForgePatchResult CreateResult = HookBackend.Create(
        Binding->TargetAddress,
        Binding->ReplacementAddress,
        Hook);
    if (!CreateResult.bSuccess)
    {
        ScriptEngine.Release(CompiledPatch);
        return CreateResult;
    }
    ActivePatches.Add(SymbolId, FActivePatch{ Binding, Hook, CompiledPatch });
    const FForgePatchResult EnableResult = HookBackend.Enable(Hook);
    if (!EnableResult.bSuccess)
    {
        FForgeCompiledPatch FailedScript = MoveTemp(ActivePatches[SymbolId].Script);
        ActivePatches.Remove(SymbolId);
        HookBackend.Remove(Hook);
        ScriptEngine.Release(FailedScript);
        return EnableResult;
    }
    return FForgePatchResult::Success();
}

void FForgePatchManager::DisablePatch(const FString& SymbolId)
{
    check(IsInGameThread());
    if (FActivePatch* Existing = ActivePatches.Find(SymbolId))
    {
        FForgeCreatedHook Hook = Existing->Hook;
        FForgeCompiledPatch Script = MoveTemp(Existing->Script);
        ActivePatches.Remove(SymbolId);
        HookBackend.Remove(Hook);
        ScriptEngine.Release(Script);
    }
}

FForgeInvokeResult FForgePatchManager::ExecuteScript(
    const FString& ScriptSource,
    const FString& EntryFunction,
    const FForgeTypeRef& ReturnType,
    TConstArrayView<FForgeTypeRef> ParameterTypes,
    TConstArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    if (!IsInGameThread())
    {
        return FForgeInvokeResult::Failure(
            TEXT("Standalone Forge scripts must execute on the game thread"));
    }
    return ScriptEngine.ExecuteStandalone(
        ScriptSource,
        EntryFunction,
        ReturnType,
        ParameterTypes,
        Arguments,
        OutReturn);
}

FForgeInvokeResult FForgePatchManager::Invoke(
    const FString& SymbolId,
    void* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    FActivePatch* Patch = ActivePatches.Find(SymbolId);
    if (Patch == nullptr)
    {
        return FForgeInvokeResult::Failure(TEXT("No active Patch for SymbolId"));
    }
    if (!IsInGameThread())
    {
        return Patch->Binding->Invoker(
            Patch->Hook.OriginalAddress,
            Instance,
            Arguments,
            OutReturn);
    }
    const FForgeInvokeResult Result = ScriptEngine.Execute(
        Patch->Script,
        *Patch->Binding,
        Patch->Hook.OriginalAddress,
        Instance,
        Arguments,
        OutReturn);
    return Result.bSuccess
        ? Result
        : Patch->Binding->Invoker(
            Patch->Hook.OriginalAddress,
            Instance,
            Arguments,
            OutReturn);
}

FForgePatchResult FForgePatchResult::Success()
{
    return { true, FString{} };
}

FForgePatchResult FForgePatchResult::Failure(FString Message)
{
    return { false, MoveTemp(Message) };
}
