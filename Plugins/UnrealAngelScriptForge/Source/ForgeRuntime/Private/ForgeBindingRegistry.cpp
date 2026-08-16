#include "ForgeBindingRegistry.h"

class FForgeBindingRegistry final : public IForgeBindingRegistry
{
public:
    virtual FForgeRegistrationResult RegisterModule(
        const FForgeModuleBindings& Bindings) override
    {
        if (Bindings.OwnerModule.IsNone())
        {
            return FForgeRegistrationResult::Failure(TEXT("Owner Module is required"));
        }
        if (Bindings.BuildFingerprint.IsEmpty())
        {
            return FForgeRegistrationResult::Failure(TEXT("Build Fingerprint is required"));
        }
        if (Bindings.Register == nullptr)
        {
            return FForgeRegistrationResult::Failure(TEXT("Module register callback is required"));
        }
        if (ModuleFingerprints.Contains(Bindings.OwnerModule))
        {
            return FForgeRegistrationResult::Failure(
                FString::Printf(TEXT("Forge bindings already registered for %s"), *Bindings.OwnerModule.ToString()));
        }

        ActiveRegistrationModule = Bindings.OwnerModule;
        PendingRegistrationFingerprint = Bindings.BuildFingerprint;
        Bindings.Register(*this);
        ActiveRegistrationModule = NAME_None;
        PendingRegistrationFingerprint.Reset();
        ModuleFingerprints.Add(Bindings.OwnerModule, Bindings.BuildFingerprint);
        return FForgeRegistrationResult::Success();
    }

    virtual void RegisterFunction(const FForgeFunctionBinding& Binding) override
    {
        checkf(!ActiveRegistrationModule.IsNone(), TEXT("Functions must be registered by a module callback"));
        checkf(Binding.OwnerModule == ActiveRegistrationModule, TEXT("Function Owner Module mismatch"));
        checkf(!Binding.BuildFingerprint.IsEmpty(), TEXT("Function Build Fingerprint is required"));
        checkf(!Binding.SymbolId.IsEmpty(), TEXT("Function SymbolId is required"));
        checkf(Binding.TargetAddress != nullptr, TEXT("Function target address is required"));
        checkf(Binding.ReplacementAddress != nullptr, TEXT("Function replacement address is required"));
        checkf(Binding.Invoker != nullptr, TEXT("Function invoker is required"));
        checkf(!Functions.Contains(Binding.SymbolId), TEXT("Duplicate Forge SymbolId: %s"), *Binding.SymbolId);
        checkf(
            Binding.BuildFingerprint == PendingRegistrationFingerprint,
            TEXT("Function Build Fingerprint does not match its module"));
        Functions.Add(Binding.SymbolId, Binding);
        PromoteFunction(Binding);
    }

    virtual void RegisterField(const FForgeFieldBinding& Binding) override
    {
        checkf(!ActiveRegistrationModule.IsNone(), TEXT("Fields must be registered by a module callback"));
        checkf(Binding.OwnerModule == ActiveRegistrationModule, TEXT("Field Owner Module mismatch"));
        checkf(!Binding.SymbolId.IsEmpty(), TEXT("Field SymbolId is required"));
        checkf(!Binding.QualifiedName.IsEmpty(), TEXT("Field qualified name is required"));
        checkf(!Binding.BuildFingerprint.IsEmpty(), TEXT("Field Build Fingerprint is required"));
        checkf(Binding.Getter != nullptr, TEXT("Field getter is required"));
        checkf(Binding.bReadOnly == (Binding.Setter == nullptr),
            TEXT("Field read-only metadata must match its setter"));
        checkf(!Fields.Contains(Binding.SymbolId), TEXT("Duplicate Forge field SymbolId: %s"), *Binding.SymbolId);
        checkf(Binding.BuildFingerprint == PendingRegistrationFingerprint,
            TEXT("Field Build Fingerprint does not match its module"));
        Fields.Add(Binding.SymbolId, Binding);
        PromoteField(Binding);
    }

    virtual void RegisterDelegate(const FForgeDelegateBinding& Binding) override
    {
        checkf(!ActiveRegistrationModule.IsNone(), TEXT("Delegates must be registered by a module callback"));
        checkf(Binding.OwnerModule == ActiveRegistrationModule, TEXT("Delegate Owner Module mismatch"));
        checkf(!Binding.SymbolId.IsEmpty(), TEXT("Delegate SymbolId is required"));
        checkf(!Binding.QualifiedName.IsEmpty(), TEXT("Delegate qualified name is required"));
        checkf(Binding.BuildFingerprint == PendingRegistrationFingerprint,
            TEXT("Delegate Build Fingerprint does not match its module"));
        checkf(Binding.Bind != nullptr && Binding.Unbind != nullptr &&
                Binding.IsBound != nullptr && Binding.Trigger != nullptr,
            TEXT("Delegate operation thunks are required"));
        const bool bMulticast = Binding.Kind == EForgeDelegateKind::Multicast ||
            Binding.Kind == EForgeDelegateKind::ThreadSafeMulticast ||
            Binding.Kind == EForgeDelegateKind::DynamicMulticast;
        checkf(!bMulticast || Binding.Clear != nullptr,
            TEXT("Multicast delegates require a clear thunk"));
        checkf(!Delegates.Contains(Binding.SymbolId),
            TEXT("Duplicate Forge delegate SymbolId: %s"), *Binding.SymbolId);
        Delegates.Add(Binding.SymbolId, Binding);
    }

    virtual void RegisterType(const FForgeTypeBinding& Binding) override
    {
        checkf(!ActiveRegistrationModule.IsNone(), TEXT("Types must be registered by a module callback"));
        checkf(Binding.OwnerModule == ActiveRegistrationModule, TEXT("Type Owner Module mismatch"));
        checkf(Binding.ScriptName != nullptr && *Binding.ScriptName != 0, TEXT("Type Script Name is required"));
        checkf(
            Binding.Kind == EForgeValueType::Enum ||
                Binding.Kind == EForgeValueType::Struct ||
                Binding.Kind == EForgeValueType::Array ||
                Binding.Kind == EForgeValueType::Map ||
                Binding.Kind == EForgeValueType::Set ||
                Binding.Kind == EForgeValueType::Optional ||
                Binding.Kind == EForgeValueType::Variant ||
                Binding.Kind == EForgeValueType::Pair ||
                Binding.Kind == EForgeValueType::NativeObject,
            TEXT("Only generated enum, struct, container, and native class types can be registered"));
        checkf(
            (Binding.Kind != EForgeValueType::Array || Binding.TemplateArguments.Num() == 1) &&
                (Binding.Kind != EForgeValueType::Map || Binding.TemplateArguments.Num() == 2) &&
                (Binding.Kind != EForgeValueType::Set || Binding.TemplateArguments.Num() == 1) &&
                (Binding.Kind != EForgeValueType::Optional || Binding.TemplateArguments.Num() == 1) &&
                (Binding.Kind != EForgeValueType::Variant || Binding.TemplateArguments.Num() >= 1) &&
                (Binding.Kind != EForgeValueType::Pair || Binding.TemplateArguments.Num() == 2),
            TEXT("Generated container template argument count is invalid"));
        checkf(!Types.Contains(Binding.ScriptName), TEXT("Duplicate Forge type: %s"), Binding.ScriptName);
        if (Binding.Kind == EForgeValueType::Struct && Binding.bNonTrivialValue)
        {
            checkf(Binding.Size > 0 && Binding.Alignment > 0 && FMath::IsPowerOfTwo(Binding.Alignment),
                TEXT("Non-trivial Forge value size/alignment is invalid"));
            checkf(Binding.ValueDefaultConstruct != nullptr &&
                    Binding.ValueCopyConstruct != nullptr && Binding.ValueMoveConstruct != nullptr &&
                    Binding.ValueAssign != nullptr && Binding.ValueDestroy != nullptr,
                TEXT("Non-trivial Forge value lifecycle thunks are required"));
        }
        if (Binding.Kind == EForgeValueType::NativeObject)
        {
            checkf(Binding.TypeId != nullptr && *Binding.TypeId != 0,
                TEXT("Native class TypeId is required"));
            checkf(Binding.QualifiedCppName != nullptr && *Binding.QualifiedCppName != 0,
                TEXT("Native class qualified C++ name is required"));
            checkf(Binding.BuildFingerprint != nullptr && *Binding.BuildFingerprint != 0,
                TEXT("Native class Build Fingerprint is required"));
            checkf(PendingRegistrationFingerprint == Binding.BuildFingerprint,
                TEXT("Native class Build Fingerprint does not match its module"));
            checkf(Binding.Size > 0 && Binding.Alignment > 0 && FMath::IsPowerOfTwo(Binding.Alignment),
                TEXT("Native class size/alignment is invalid"));
            for (const FForgeBaseTypeBinding& Base : Binding.BaseTypes)
            {
                if (Base.Access == EForgeAccessSpecifier::Public &&
                    Base.TypeId != nullptr && *Base.TypeId != 0)
                {
                    checkf(Base.Cast != nullptr,
                        TEXT("Public reflected base types require an owner-module cast thunk"));
                }
            }
            checkf(!TypeIds.Contains(Binding.TypeId), TEXT("Duplicate Forge TypeId: %s"), Binding.TypeId);
            checkf(!CppTypeNames.Contains(Binding.QualifiedCppName),
                TEXT("Duplicate Forge C++ type: %s"), Binding.QualifiedCppName);
            if (EnumHasAllFlags(
                    Binding.InstanceCapabilities,
                    EForgeInstanceCapability::OwnedLifecycle))
            {
                checkf(Binding.DefaultConstruct != nullptr && Binding.CopyConstruct != nullptr &&
                        Binding.MoveConstruct != nullptr && Binding.Destroy != nullptr,
                    TEXT("Owned native class lifecycle thunks are required"));
                checkf(Binding.ScriptNewName != nullptr && *Binding.ScriptNewName != 0 &&
                        Binding.ScriptCopyName != nullptr && *Binding.ScriptCopyName != 0 &&
                        Binding.ScriptMoveName != nullptr && *Binding.ScriptMoveName != 0,
                    TEXT("Owned native class factory names are required"));
            }
            TypeIds.Add(Binding.TypeId, Binding.ScriptName);
            CppTypeNames.Add(Binding.QualifiedCppName, Binding.ScriptName);
        }
        Types.Add(Binding.ScriptName, Binding);
        PromoteType(Binding);
    }

    virtual void RegisterReflectionMapping(
        const FForgeReflectionMappingBinding& Binding) override
    {
        checkf(!ActiveRegistrationModule.IsNone(), TEXT("Mappings must be registered by a module callback"));
        checkf(Binding.OwnerModule == ActiveRegistrationModule, TEXT("Mapping Owner Module mismatch"));
        checkf(Binding.BuildFingerprint == PendingRegistrationFingerprint,
            TEXT("Mapping Build Fingerprint does not match its module"));
        checkf(!Binding.MappingId.IsEmpty(), TEXT("MappingId is required"));
        checkf(!Binding.CppQualifiedName.IsEmpty(), TEXT("Mapping C++ name is required"));
        checkf(!Binding.UeFieldPath.IsEmpty(), TEXT("Mapping UE FieldPath is required"));
        checkf(!Binding.ForgeDeclarationSymbolId.IsEmpty(),
            TEXT("Mapping Forge declaration SymbolId is required"));

        FForgeUnifiedReflection* Existing = Reflections.Find(Binding.UeFieldPath);
        if (Existing == nullptr)
        {
            FForgeUnifiedReflection Reflection;
            Reflection.StableKey = Binding.UeFieldPath;
            Reflection.Kind = Binding.Kind;
            Reflection.CppQualifiedName = Binding.CppQualifiedName;
            Reflection.UeFieldPath = Binding.UeFieldPath;
            Reflection.OwnerModule = Binding.OwnerModule;
            Reflection.BuildFingerprint = Binding.BuildFingerprint;
            Reflection.Sources = EForgeReflectionSource::Forge | EForgeReflectionSource::Unreal;
            Reflection.Capabilities = EForgeUnifiedCapability::Discoverable;
            Existing = &Reflections.Add(Binding.UeFieldPath, MoveTemp(Reflection));
        }
        else
        {
            checkf(Existing->Kind == Binding.Kind &&
                    Existing->CppQualifiedName == Binding.CppQualifiedName &&
                    Existing->OwnerModule == Binding.OwnerModule &&
                    Existing->BuildFingerprint == Binding.BuildFingerprint,
                TEXT("Conflicting reflection mapping for %s"), *Binding.UeFieldPath);
            Existing->Sources |= EForgeReflectionSource::Forge | EForgeReflectionSource::Unreal;
        }
        Existing->MappingIds.AddUnique(Binding.MappingId);
        Existing->ForgeDeclarationSymbolIds.AddUnique(Binding.ForgeDeclarationSymbolId);
        if (!Binding.ForgeBindingSymbolId.IsEmpty())
        {
            Existing->ForgeBindingSymbolIds.AddUnique(Binding.ForgeBindingSymbolId);
            const FString* ExistingKey = BindingReflectionKeys.Find(Binding.ForgeBindingSymbolId);
            checkf(ExistingKey == nullptr || *ExistingKey == Binding.UeFieldPath,
                TEXT("Forge binding maps to multiple UE fields: %s"), *Binding.ForgeBindingSymbolId);
            BindingReflectionKeys.Add(Binding.ForgeBindingSymbolId, Binding.UeFieldPath);
        }
        TypeReflectionKeys.Add(Binding.CppQualifiedName, Binding.UeFieldPath);
    }

    virtual const FForgeFunctionBinding* FindFunction(
        const FString& SymbolId) const override
    {
        return Functions.Find(SymbolId);
    }

    virtual const FForgeFunctionBinding* FindFunctionByName(
        const FString& QualifiedName) const override
    {
        const FForgeFunctionBinding* Match = nullptr;
        for (const TPair<FString, FForgeFunctionBinding>& Pair : Functions)
        {
            if (Pair.Value.QualifiedName == QualifiedName)
            {
                if (Match != nullptr)
                {
                    return nullptr;
                }
                Match = &Pair.Value;
            }
        }
        return Match;
    }

    virtual const FForgeFieldBinding* FindField(const FString& SymbolId) const override
    {
        return Fields.Find(SymbolId);
    }

    virtual const FForgeFieldBinding* FindFieldByName(
        const FString& QualifiedName) const override
    {
        const FForgeFieldBinding* Match = nullptr;
        for (const TPair<FString, FForgeFieldBinding>& Pair : Fields)
        {
            if (Pair.Value.QualifiedName == QualifiedName)
            {
                if (Match != nullptr)
                {
                    return nullptr;
                }
                Match = &Pair.Value;
            }
        }
        return Match;
    }

    virtual const FForgeDelegateBinding* FindDelegate(
        const FString& SymbolId) const override
    {
        return Delegates.Find(SymbolId);
    }

    virtual const FForgeDelegateBinding* FindDelegateByName(
        const FString& QualifiedName) const override
    {
        const FForgeDelegateBinding* Match = nullptr;
        for (const TPair<FString, FForgeDelegateBinding>& Pair : Delegates)
        {
            if (Pair.Value.QualifiedName == QualifiedName)
            {
                if (Match != nullptr)
                {
                    return nullptr;
                }
                Match = &Pair.Value;
            }
        }
        return Match;
    }

    virtual const FForgeTypeBinding* FindType(const FString& ScriptName) const override
    {
        return Types.Find(ScriptName);
    }

    virtual const FForgeTypeBinding* FindTypeById(const FString& TypeId) const override
    {
        const FString* ScriptName = TypeIds.Find(TypeId);
        return ScriptName != nullptr ? Types.Find(*ScriptName) : nullptr;
    }

    virtual const FForgeTypeBinding* FindTypeByCppName(
        const FString& QualifiedCppName) const override
    {
        const FString* ScriptName = CppTypeNames.Find(QualifiedCppName);
        return ScriptName != nullptr ? Types.Find(*ScriptName) : nullptr;
    }

    virtual FForgeInstanceCastResult CastInstance(
        FName SourceTypeId,
        FName TargetTypeId,
        void* Address) const override
    {
        if (SourceTypeId.IsNone() || TargetTypeId.IsNone() || Address == nullptr)
        {
            return FForgeInstanceCastResult::Failure(
                TEXT("Forge instance cast requires source/target TypeId and an address"));
        }
        if (SourceTypeId == TargetTypeId)
        {
            return FForgeInstanceCastResult::Success(Address);
        }

        const FForgeTypeBinding* Source = FindTypeById(SourceTypeId.ToString());
        const FForgeTypeBinding* Target = FindTypeById(TargetTypeId.ToString());
        if (Source == nullptr || Target == nullptr)
        {
            return FForgeInstanceCastResult::Failure(
                TEXT("Forge object handle owner type mismatch: reflected type is not loaded"));
        }

        struct FPendingCast
        {
            const FForgeTypeBinding* Type = nullptr;
            void* Address = nullptr;
        };
        TArray<FPendingCast, TInlineAllocator<8>> Pending{
            FPendingCast{ Source, Address }
        };
        TArray<void*, TInlineAllocator<2>> Matches;
        for (int32 PendingIndex = 0; PendingIndex < Pending.Num(); ++PendingIndex)
        {
            const FPendingCast Current = Pending[PendingIndex];
            for (const FForgeBaseTypeBinding& Base : Current.Type->BaseTypes)
            {
                if (Base.Access != EForgeAccessSpecifier::Public ||
                    Base.TypeId == nullptr || *Base.TypeId == 0 || Base.Cast == nullptr)
                {
                    continue;
                }
                void* BaseAddress = Base.Cast(Current.Address);
                if (BaseAddress == nullptr)
                {
                    continue;
                }
                const FName BaseTypeId(Base.TypeId);
                if (BaseTypeId == TargetTypeId)
                {
                    Matches.AddUnique(BaseAddress);
                }
                const FForgeTypeBinding* BaseType = FindTypeById(Base.TypeId);
                if (BaseType == nullptr)
                {
                    continue;
                }
                const bool bAlreadyPending = Pending.ContainsByPredicate(
                    [BaseType, BaseAddress](const FPendingCast& Item)
                    {
                        return Item.Type == BaseType && Item.Address == BaseAddress;
                    });
                if (!bAlreadyPending)
                {
                    Pending.Add(FPendingCast{ BaseType, BaseAddress });
                }
            }
        }
        if (Matches.Num() == 1)
        {
            return FForgeInstanceCastResult::Success(Matches[0]);
        }
        if (Matches.Num() > 1)
        {
            return FForgeInstanceCastResult::Failure(
                TEXT("Forge object handle owner type mismatch: base subobject is ambiguous"));
        }
        return FForgeInstanceCastResult::Failure(
            TEXT("Forge object handle owner type mismatch: no public reflected base path"));
    }

    virtual void GetFunctions(TArray<FForgeFunctionBinding>& OutBindings) const override
    {
        OutBindings.Reset();
        TArray<FForgeFunctionBinding> Sorted;
        Functions.GenerateValueArray(Sorted);
        Sorted.Sort([](
            const FForgeFunctionBinding& Left,
            const FForgeFunctionBinding& Right)
        {
            return Left.SymbolId < Right.SymbolId;
        });
        TSet<FString> Seen;
        for (const FForgeFunctionBinding& Binding : Sorted)
        {
            const FString* UnifiedKey = BindingReflectionKeys.Find(Binding.SymbolId);
            const FString& ExposureKey = UnifiedKey != nullptr ? *UnifiedKey : Binding.SymbolId;
            if (!Seen.Contains(ExposureKey))
            {
                Seen.Add(ExposureKey);
                OutBindings.Add(Binding);
            }
        }
    }

    virtual void GetFields(TArray<FForgeFieldBinding>& OutBindings) const override
    {
        OutBindings.Reset();
        TArray<FForgeFieldBinding> Sorted;
        Fields.GenerateValueArray(Sorted);
        Sorted.Sort([](const FForgeFieldBinding& Left, const FForgeFieldBinding& Right)
        {
            return Left.SymbolId < Right.SymbolId;
        });
        TSet<FString> Seen;
        for (const FForgeFieldBinding& Binding : Sorted)
        {
            const FString* UnifiedKey = BindingReflectionKeys.Find(Binding.SymbolId);
            const FString& ExposureKey = UnifiedKey != nullptr ? *UnifiedKey : Binding.SymbolId;
            if (!Seen.Contains(ExposureKey))
            {
                Seen.Add(ExposureKey);
                OutBindings.Add(Binding);
            }
        }
    }

    virtual void GetDelegates(TArray<FForgeDelegateBinding>& OutBindings) const override
    {
        Delegates.GenerateValueArray(OutBindings);
        OutBindings.Sort([](
            const FForgeDelegateBinding& Left,
            const FForgeDelegateBinding& Right)
        {
            return Left.SymbolId < Right.SymbolId;
        });
    }

    virtual void GetTypes(TArray<FForgeTypeBinding>& OutBindings) const override
    {
        OutBindings.Reset();
        TArray<FForgeTypeBinding> Sorted;
        Types.GenerateValueArray(Sorted);
        Sorted.Sort([](const FForgeTypeBinding& Left, const FForgeTypeBinding& Right)
        {
            return FCString::Strcmp(Left.ScriptName, Right.ScriptName) < 0;
        });
        TSet<FString> Seen;
        for (const FForgeTypeBinding& Binding : Sorted)
        {
            const FString CppName = Binding.QualifiedCppName != nullptr
                ? FString(Binding.QualifiedCppName)
                : FString{};
            const FString* UnifiedKey = TypeReflectionKeys.Find(CppName);
            const FString ExposureKey = UnifiedKey != nullptr
                ? *UnifiedKey
                : FString(Binding.ScriptName);
            if (!Seen.Contains(ExposureKey))
            {
                Seen.Add(ExposureKey);
                OutBindings.Add(Binding);
            }
        }
    }

    virtual const FForgeUnifiedReflection* FindReflection(
        const FString& StableKey) const override
    {
        return Reflections.Find(StableKey);
    }

    virtual void GetReflections(
        TArray<FForgeUnifiedReflection>& OutBindings) const override
    {
        Reflections.GenerateValueArray(OutBindings);
        OutBindings.Sort([](
            const FForgeUnifiedReflection& Left,
            const FForgeUnifiedReflection& Right)
        {
            return Left.StableKey < Right.StableKey;
        });
    }

    virtual void UnregisterModule(FName OwnerModule) override
    {
        ModuleFingerprints.Remove(OwnerModule);
        for (auto Iterator = Reflections.CreateIterator(); Iterator; ++Iterator)
        {
            if (Iterator.Value().OwnerModule == OwnerModule)
            {
                for (const FString& SymbolId : Iterator.Value().ForgeBindingSymbolIds)
                {
                    BindingReflectionKeys.Remove(SymbolId);
                }
                TypeReflectionKeys.Remove(Iterator.Value().CppQualifiedName);
                Iterator.RemoveCurrent();
            }
        }
        for (auto Iterator = Functions.CreateIterator(); Iterator; ++Iterator)
        {
            if (Iterator.Value().OwnerModule == OwnerModule)
            {
                Iterator.RemoveCurrent();
            }
        }
        for (auto Iterator = Fields.CreateIterator(); Iterator; ++Iterator)
        {
            if (Iterator.Value().OwnerModule == OwnerModule)
            {
                Iterator.RemoveCurrent();
            }
        }
        for (auto Iterator = Delegates.CreateIterator(); Iterator; ++Iterator)
        {
            if (Iterator.Value().OwnerModule == OwnerModule)
            {
                Iterator.RemoveCurrent();
            }
        }
        for (auto Iterator = Types.CreateIterator(); Iterator; ++Iterator)
        {
            if (Iterator.Value().OwnerModule == OwnerModule)
            {
                if (Iterator.Value().Kind == EForgeValueType::NativeObject)
                {
                    TypeIds.Remove(Iterator.Value().TypeId);
                    CppTypeNames.Remove(Iterator.Value().QualifiedCppName);
                }
                Iterator.RemoveCurrent();
            }
        }
    }

private:
    void PromoteFunction(const FForgeFunctionBinding& Binding)
    {
        if (const FString* Key = BindingReflectionKeys.Find(Binding.SymbolId))
        {
            if (FForgeUnifiedReflection* Reflection = Reflections.Find(*Key))
            {
                Reflection->Capabilities |= EForgeUnifiedCapability::Callable;
                if (Binding.bPatchable) Reflection->Capabilities |= EForgeUnifiedCapability::Patchable;
                if (Binding.bOriginalCallable) Reflection->Capabilities |= EForgeUnifiedCapability::OriginalCallable;
            }
        }
    }

    void PromoteField(const FForgeFieldBinding& Binding)
    {
        if (const FString* Key = BindingReflectionKeys.Find(Binding.SymbolId))
        {
            if (FForgeUnifiedReflection* Reflection = Reflections.Find(*Key))
            {
                Reflection->Capabilities |= EForgeUnifiedCapability::Readable;
                if (!Binding.bReadOnly && Binding.Setter != nullptr)
                {
                    Reflection->Capabilities |= EForgeUnifiedCapability::Writable;
                }
            }
        }
    }

    void PromoteType(const FForgeTypeBinding& Binding)
    {
        if (Binding.QualifiedCppName == nullptr) return;
        if (const FString* Key = TypeReflectionKeys.Find(Binding.QualifiedCppName))
        {
            if (FForgeUnifiedReflection* Reflection = Reflections.Find(*Key))
            {
                Reflection->Capabilities |= EForgeUnifiedCapability::Discoverable;
            }
        }
    }

    TMap<FString, FForgeFunctionBinding> Functions;
    TMap<FString, FForgeFieldBinding> Fields;
    TMap<FString, FForgeDelegateBinding> Delegates;
    TMap<FString, FForgeTypeBinding> Types;
    TMap<FString, FString> TypeIds;
    TMap<FString, FString> CppTypeNames;
    TMap<FName, FString> ModuleFingerprints;
    TMap<FString, FForgeUnifiedReflection> Reflections;
    TMap<FString, FString> BindingReflectionKeys;
    TMap<FString, FString> TypeReflectionKeys;
    FName ActiveRegistrationModule;
    FString PendingRegistrationFingerprint;
};

TUniquePtr<IForgeBindingRegistry> CreateForgeBindingRegistry()
{
    return MakeUnique<FForgeBindingRegistry>();
}

FForgeInvokeResult FForgeInvokeResult::Success()
{
    return { true, FString{} };
}

FForgeInvokeResult FForgeInvokeResult::ArgumentCountMismatch(int32 Expected, int32 Actual)
{
    return Failure(FString::Printf(
        TEXT("Expected %d arguments, received %d"), Expected, Actual));
}

FForgeInvokeResult FForgeInvokeResult::Failure(FString Message)
{
    return { false, MoveTemp(Message) };
}

FForgeRegistrationResult FForgeRegistrationResult::Success()
{
    return { true, FString{} };
}

FForgeRegistrationResult FForgeRegistrationResult::Failure(FString Message)
{
    return { false, MoveTemp(Message) };
}

FForgeInstanceCastResult FForgeInstanceCastResult::Success(void* Address)
{
    return { true, FString{}, Address };
}

FForgeInstanceCastResult FForgeInstanceCastResult::Failure(FString Message)
{
    return { false, MoveTemp(Message), nullptr };
}

FForgeAutoModuleRegistrar::FForgeAutoModuleRegistrar(FForgeModuleBindings Bindings)
{
    RegisterForgeModuleBindings(Bindings);
}
