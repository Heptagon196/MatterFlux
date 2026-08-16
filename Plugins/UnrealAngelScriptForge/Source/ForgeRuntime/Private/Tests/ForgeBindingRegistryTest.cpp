#include "ForgeBindingRegistry.h"
#include "IForgeRuntimeModule.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
constexpr TCHAR TestSymbolId[] = TEXT("sha256:forge-runtime-registry-test");
constexpr TCHAR DuplicateTestSymbolId[] = TEXT("sha256:forge-runtime-registry-test-duplicate");
constexpr TCHAR TestFieldSymbolId[] = TEXT("sha256:forge-runtime-field-registry-test");
constexpr TCHAR TestDelegateSymbolId[] = TEXT("sha256:forge-runtime-delegate-registry-test");
constexpr TCHAR TestUeFieldPath[] = TEXT("/Script/ForgeRuntimeTests.RegistryObject:AddOne");
int32 TestFieldValue = 3;

struct FTestLeftBase
{
    int32 Left = 1;
};

struct FTestRightBase
{
    int32 Right = 2;
};

struct FTestMultipleDerived : FTestLeftBase, FTestRightBase
{
    int32 Derived = 3;
};

struct FTestVirtualBase
{
    int32 Base = 4;
};

struct FTestVirtualDerived : virtual FTestVirtualBase
{
    int32 Derived = 5;
};

void* CastMultipleToLeft(void* Address)
{
    return static_cast<FTestLeftBase*>(static_cast<FTestMultipleDerived*>(Address));
}

void* CastMultipleToRight(void* Address)
{
    return static_cast<FTestRightBase*>(static_cast<FTestMultipleDerived*>(Address));
}

void* CastVirtualToBase(void* Address)
{
    return static_cast<FTestVirtualBase*>(static_cast<FTestVirtualDerived*>(Address));
}

float AddOne(float Value)
{
    return Value + 1.0f;
}

FForgeInvokeResult InvokeAddOne(
    void* FunctionAddress,
    void* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    if (Arguments.Num() != 1)
    {
        return FForgeInvokeResult::ArgumentCountMismatch(1, Arguments.Num());
    }
    const auto Function = reinterpret_cast<float (*)(float)>(FunctionAddress);
    OutReturn = FForgeValue::FromFloat(Function(Arguments[0].AsFloat()));
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult GetTestField(void* Instance, FForgeValue& OutValue)
{
    (void)Instance;
    OutValue = FForgeValue::FromInt32(TestFieldValue);
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult SetTestField(void* Instance, const FForgeValue& Value)
{
    (void)Instance;
    TestFieldValue = Value.AsInt32();
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult BindTestDelegate(
    void* Instance,
    const FForgeDelegateCallback& Callback,
    FForgeDelegateHandle& OutHandle)
{
    (void)Instance;
    if (!Callback.IsValid())
    {
        return FForgeInvokeResult::Failure(TEXT("Callback is required"));
    }
    OutHandle.NativeHandle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult UnbindTestDelegate(
    void* Instance,
    const FForgeDelegateHandle& Handle)
{
    (void)Instance;
    (void)Handle;
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult ClearTestDelegate(void* Instance)
{
    (void)Instance;
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult IsTestDelegateBound(void* Instance, bool& OutIsBound)
{
    (void)Instance;
    OutIsBound = true;
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult TriggerTestDelegate(
    void* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    (void)Instance;
    if (Arguments.Num() != 1)
    {
        return FForgeInvokeResult::ArgumentCountMismatch(1, Arguments.Num());
    }
    OutReturn = FForgeValue::FromInt32(Arguments[0].AsInt32() + 10);
    return FForgeInvokeResult::Success();
}

void RegisterTestFunction(IForgeBindingRegistry& Registry)
{
    static constexpr FForgeTypeRef ParameterTypes[] = {
        FForgeTypeRef{ EForgeValueType::Float, TEXT("float") }
    };
    Registry.RegisterReflectionMapping(FForgeReflectionMappingBinding{
        TEXT("sha256:forge-runtime-reflection-map-a"),
        EForgeReflectionKind::Function,
        TEXT("ForgeRuntimeTests::AddOne"),
        TestUeFieldPath,
        TEXT("sha256:forge-runtime-declaration-a"),
        TestSymbolId,
        FName(TEXT("ForgeRuntimeTests")),
        TEXT("sha256:test-build")
    });
    Registry.RegisterReflectionMapping(FForgeReflectionMappingBinding{
        TEXT("sha256:forge-runtime-reflection-map-b"),
        EForgeReflectionKind::Function,
        TEXT("ForgeRuntimeTests::AddOne"),
        TestUeFieldPath,
        TEXT("sha256:forge-runtime-declaration-b"),
        DuplicateTestSymbolId,
        FName(TEXT("ForgeRuntimeTests")),
        TEXT("sha256:test-build")
    });
    const FForgeFunctionBinding FunctionBinding{
        TestSymbolId,
        TEXT("ForgeRuntimeTests::AddOne"),
        FName(TEXT("ForgeRuntimeTests")),
        TEXT("sha256:test-build"),
        reinterpret_cast<void*>(&AddOne),
        reinterpret_cast<void*>(&AddOne),
        &InvokeAddOne,
        FForgeTypeRef{ EForgeValueType::Float, TEXT("float") },
        TConstArrayView<FForgeTypeRef>(ParameterTypes),
        false,
        TEXT(""),
        true,
        true
    };
    Registry.RegisterFunction(FunctionBinding);
    FForgeFunctionBinding DuplicateBinding = FunctionBinding;
    DuplicateBinding.SymbolId = DuplicateTestSymbolId;
    Registry.RegisterFunction(DuplicateBinding);
    Registry.RegisterField(FForgeFieldBinding{
        TestFieldSymbolId,
        TEXT("ForgeRuntimeTests::TestFieldValue"),
        FName(TEXT("ForgeRuntimeTests")),
        TEXT("sha256:test-build"),
        FForgeTypeRef{ EForgeValueType::Int32, TEXT("int") },
        EForgeAccessSpecifier::Private,
        TEXT(""),
        TEXT(""),
        true,
        false,
        TEXT("Get_TestFieldValue"),
        TEXT("Set_TestFieldValue"),
        &GetTestField,
        &SetTestField
    });
    static constexpr FForgeTypeRef DelegateParameterTypes[] = {
        FForgeTypeRef{ EForgeValueType::Int32, TEXT("int") }
    };
    Registry.RegisterDelegate(FForgeDelegateBinding{
        TestDelegateSymbolId,
        TEXT("ForgeRuntimeTests::OnValue"),
        FName(TEXT("ForgeRuntimeTests")),
        TEXT("sha256:test-build"),
        EForgeDelegateKind::Singlecast,
        FForgeTypeRef{ EForgeValueType::Int32, TEXT("int") },
        TConstArrayView<FForgeTypeRef>(DelegateParameterTypes),
        EForgeAccessSpecifier::Public,
        TEXT(""),
        TEXT(""),
        true,
        false,
        TEXT("Bind_OnValue"),
        TEXT("Unbind_OnValue"),
        TEXT("IsBound_OnValue"),
        TEXT("Execute_OnValue"),
        TEXT(""),
        &BindTestDelegate,
        &UnbindTestDelegate,
        nullptr,
        &IsTestDelegateBound,
        &TriggerTestDelegate
    });

    static constexpr FForgeBaseTypeBinding MultipleBases[] = {
        FForgeBaseTypeBinding{
            TEXT("sha256:test-left-type"), TEXT("FTestLeftBase"),
            EForgeAccessSpecifier::Public, false, &CastMultipleToLeft },
        FForgeBaseTypeBinding{
            TEXT("sha256:test-right-type"), TEXT("FTestRightBase"),
            EForgeAccessSpecifier::Public, false, &CastMultipleToRight }
    };
    static constexpr FForgeBaseTypeBinding VirtualBases[] = {
        FForgeBaseTypeBinding{
            TEXT("sha256:test-virtual-base-type"), TEXT("FTestVirtualBase"),
            EForgeAccessSpecifier::Public, true, &CastVirtualToBase }
    };
    auto RegisterNativeType = [&Registry](
        const TCHAR* ScriptName,
        const TCHAR* TypeId,
        int32 Size,
        int32 Alignment,
        TConstArrayView<FForgeBaseTypeBinding> Bases)
    {
        FForgeTypeBinding Binding;
        Binding.Kind = EForgeValueType::NativeObject;
        Binding.ScriptName = ScriptName;
        Binding.QualifiedCppName = ScriptName;
        Binding.OwnerModule = FName(TEXT("ForgeRuntimeTests"));
        Binding.Size = Size;
        Binding.TypeId = TypeId;
        Binding.BuildFingerprint = TEXT("sha256:test-build");
        Binding.Alignment = Alignment;
        Binding.BaseTypes = Bases;
        Binding.InstanceCapabilities =
            EForgeInstanceCapability::BorrowedRead |
            EForgeInstanceCapability::BorrowedWrite;
        Registry.RegisterType(Binding);
    };
    RegisterNativeType(
        TEXT("FTestLeftBase"), TEXT("sha256:test-left-type"),
        sizeof(FTestLeftBase), alignof(FTestLeftBase), {});
    RegisterNativeType(
        TEXT("FTestRightBase"), TEXT("sha256:test-right-type"),
        sizeof(FTestRightBase), alignof(FTestRightBase), {});
    RegisterNativeType(
        TEXT("FTestMultipleDerived"), TEXT("sha256:test-multiple-type"),
        sizeof(FTestMultipleDerived), alignof(FTestMultipleDerived), MultipleBases);
    RegisterNativeType(
        TEXT("FTestVirtualBase"), TEXT("sha256:test-virtual-base-type"),
        sizeof(FTestVirtualBase), alignof(FTestVirtualBase), {});
    RegisterNativeType(
        TEXT("FTestVirtualDerived"), TEXT("sha256:test-virtual-type"),
        sizeof(FTestVirtualDerived), alignof(FTestVirtualDerived), VirtualBases);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FForgeBindingRegistryTest,
    "Forge.Runtime.BindingRegistry.RegistersAndInvokesOwnerBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FForgeBindingRegistryTest::RunTest(const FString& Parameters)
{
    IForgeBindingRegistry& Registry = IForgeRuntimeModule::Get().GetBindingRegistry();
    Registry.UnregisterModule(FName(TEXT("ForgeRuntimeTests")));
    const FForgeRegistrationResult Registration = Registry.RegisterModule(FForgeModuleBindings{
        FName(TEXT("ForgeRuntimeTests")),
        TEXT("sha256:test-build"),
        &RegisterTestFunction
    });
    TestTrue(TEXT("Module registration succeeds"), Registration.bSuccess);

    const FForgeFunctionBinding* Binding = Registry.FindFunction(TestSymbolId);
    TestNotNull(TEXT("Function is queryable by SymbolId"), Binding);
    TestNull(
        TEXT("Ambiguous C++ names still require SymbolId for native lookup"),
        Registry.FindFunctionByName(TEXT("ForgeRuntimeTests::AddOne")));
    TestNull(
        TEXT("Unknown qualified C++ name is not resolved"),
        Registry.FindFunctionByName(TEXT("ForgeRuntimeTests::Missing")));
    TArray<FForgeFunctionBinding> ExposedFunctions;
    Registry.GetFunctions(ExposedFunctions);
    TestEqual(
        TEXT("Two Forge bindings mapped to one UE FieldPath expose once"),
        ExposedFunctions.FilterByPredicate([](const FForgeFunctionBinding& Candidate)
        {
            return Candidate.QualifiedName == TEXT("ForgeRuntimeTests::AddOne");
        }).Num(),
        1);
    const FForgeUnifiedReflection* Unified = Registry.FindReflection(TestUeFieldPath);
    TestNotNull(TEXT("Unified reflection is queryable by UE FieldPath"), Unified);
    if (Unified != nullptr)
    {
        TestTrue(TEXT("Unified reflection preserves UE source"),
            EnumHasAllFlags(Unified->Sources, EForgeReflectionSource::Unreal));
        TestTrue(TEXT("Unified reflection preserves Forge source"),
            EnumHasAllFlags(Unified->Sources, EForgeReflectionSource::Forge));
        TestTrue(TEXT("Unified reflection merges callable capability"),
            EnumHasAllFlags(Unified->Capabilities, EForgeUnifiedCapability::Callable));
        TestTrue(TEXT("Unified reflection merges patch capability"),
            EnumHasAllFlags(Unified->Capabilities, EForgeUnifiedCapability::Patchable));
        TestEqual(TEXT("Unified reflection preserves both Forge binding ids"),
            Unified->ForgeBindingSymbolIds.Num(), 2);
    }
    const FForgeFieldBinding* Field = Registry.FindField(TestFieldSymbolId);
    TestNotNull(TEXT("Field is queryable by SymbolId"), Field);
    TestTrue(TEXT("Field is queryable by qualified C++ name"),
        Registry.FindFieldByName(TEXT("ForgeRuntimeTests::TestFieldValue")) == Field);
    if (Field != nullptr)
    {
        FForgeValue FieldValue;
        TestTrue(TEXT("Field getter succeeds"), Field->Getter(nullptr, FieldValue).bSuccess);
        TestEqual(TEXT("Field getter returns typed native value"), FieldValue.AsInt32(), 3);
        TestTrue(TEXT("Field setter succeeds"),
            Field->Setter(nullptr, FForgeValue::FromInt32(9)).bSuccess);
        TestEqual(TEXT("Field setter mutates native storage"), TestFieldValue, 9);
    }
    const FForgeDelegateBinding* Delegate = Registry.FindDelegate(TestDelegateSymbolId);
    TestNotNull(TEXT("Delegate is queryable by SymbolId"), Delegate);
    TestTrue(TEXT("Delegate is queryable by qualified C++ name"),
        Registry.FindDelegateByName(TEXT("ForgeRuntimeTests::OnValue")) == Delegate);
    if (Delegate != nullptr)
    {
        bool bBound = false;
        TestTrue(TEXT("Delegate IsBound thunk succeeds"),
            Delegate->IsBound(nullptr, bBound).bSuccess);
        TestTrue(TEXT("Delegate IsBound returns native state"), bBound);
        FForgeValue DelegateArguments[] = { FForgeValue::FromInt32(32) };
        FForgeValue DelegateReturn;
        TestTrue(TEXT("Delegate trigger thunk succeeds"),
            Delegate->Trigger(nullptr, DelegateArguments, DelegateReturn).bSuccess);
        TestEqual(TEXT("Delegate trigger returns typed native value"),
            DelegateReturn.AsInt32(), 42);
    }
    if (Binding == nullptr)
    {
        return false;
    }

    FForgeValue Arguments[] = { FForgeValue::FromFloat(41.0f) };
    FForgeValue ReturnValue;
    const FForgeInvokeResult InvokeResult = Binding->Invoker(
        Binding->TargetAddress,
        nullptr,
        Arguments,
        ReturnValue);
    TestTrue(TEXT("Invocation succeeds"), InvokeResult.bSuccess);
    TestEqual(TEXT("Typed invoker returns native result"), ReturnValue.AsFloat(), 42.0f);

    FTestMultipleDerived Multiple;
    const FForgeInstanceCastResult RightCast = Registry.CastInstance(
        FName(TEXT("sha256:test-multiple-type")),
        FName(TEXT("sha256:test-right-type")),
        &Multiple);
    TestTrue(TEXT("Multiple inheritance cast succeeds"), RightCast.bSuccess);
    TestTrue(TEXT("Multiple inheritance cast uses the C++ adjusted address"),
        RightCast.Address == static_cast<FTestRightBase*>(&Multiple));
    const FForgeInstanceCastResult UnrelatedCast = Registry.CastInstance(
        FName(TEXT("sha256:test-multiple-type")),
        FName(TEXT("sha256:test-virtual-base-type")),
        &Multiple);
    TestFalse(TEXT("Unrelated reflected types fail closed"), UnrelatedCast.bSuccess);

    FTestVirtualDerived Virtual;
    const FForgeInstanceCastResult VirtualCast = Registry.CastInstance(
        FName(TEXT("sha256:test-virtual-type")),
        FName(TEXT("sha256:test-virtual-base-type")),
        &Virtual);
    TestTrue(TEXT("Virtual inheritance cast succeeds"), VirtualCast.bSuccess);
    TestTrue(TEXT("Virtual inheritance cast uses the C++ adjusted address"),
        VirtualCast.Address == static_cast<FTestVirtualBase*>(&Virtual));
    Registry.UnregisterModule(FName(TEXT("ForgeRuntimeTests")));
    TestNull(TEXT("Module unload removes its field"), Registry.FindField(TestFieldSymbolId));
    TestNull(TEXT("Module unload removes its delegate"),
        Registry.FindDelegate(TestDelegateSymbolId));
    return true;
}

#endif
