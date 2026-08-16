#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "UObject/StrongObjectPtr.h"
#include "ForgeValue.h"

struct FORGERUNTIME_API FForgeInvokeResult
{
    bool bSuccess = false;
    FString Error;

    static FForgeInvokeResult Success();
    static FForgeInvokeResult ArgumentCountMismatch(int32 Expected, int32 Actual);
    static FForgeInvokeResult Failure(FString Message);
};

using FForgeInvoker = FForgeInvokeResult (*)(
    void* FunctionAddress,
    void* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn);

struct FORGERUNTIME_API FForgeTypeRef
{
    EForgeValueType Kind = EForgeValueType::Void;
    const TCHAR* Name = TEXT("");
    enum class EPassingMode : uint8
    {
        Value,
        ConstReference,
        InOutReference,
        NullablePointer
    } PassingMode = EPassingMode::Value;
};

enum class EForgeAccessSpecifier : uint8
{
    Public,
    Protected,
    Private
};

enum class EForgeReflectionSource : uint8
{
    None = 0,
    Forge = 1 << 0,
    Unreal = 1 << 1
};
ENUM_CLASS_FLAGS(EForgeReflectionSource);

enum class EForgeReflectionKind : uint8
{
    Class,
    Struct,
    Enum,
    Function,
    Property
};

enum class EForgeUnifiedCapability : uint8
{
    None = 0,
    Discoverable = 1 << 0,
    Readable = 1 << 1,
    Writable = 1 << 2,
    Callable = 1 << 3,
    Patchable = 1 << 4,
    OriginalCallable = 1 << 5
};
ENUM_CLASS_FLAGS(EForgeUnifiedCapability);

struct FORGERUNTIME_API FForgeReflectionMappingBinding
{
    FString MappingId;
    EForgeReflectionKind Kind = EForgeReflectionKind::Class;
    FString CppQualifiedName;
    FString UeFieldPath;
    FString ForgeDeclarationSymbolId;
    FString ForgeBindingSymbolId;
    FName OwnerModule;
    FString BuildFingerprint;
};

struct FORGERUNTIME_API FForgeUnifiedReflection
{
    FString StableKey;
    EForgeReflectionKind Kind = EForgeReflectionKind::Class;
    FString CppQualifiedName;
    FString UeFieldPath;
    FName OwnerModule;
    FString BuildFingerprint;
    EForgeReflectionSource Sources = EForgeReflectionSource::None;
    EForgeUnifiedCapability Capabilities = EForgeUnifiedCapability::None;
    TArray<FString> MappingIds;
    TArray<FString> ForgeDeclarationSymbolIds;
    TArray<FString> ForgeBindingSymbolIds;
};

using FForgeFieldGetter = FForgeInvokeResult (*)(
    void* Instance,
    FForgeValue& OutValue);

using FForgeFieldSetter = FForgeInvokeResult (*)(
    void* Instance,
    const FForgeValue& Value);

using FForgeObjectConstruct = FForgeInvokeResult (*)(
    void* Source,
    void*& OutAddress);

using FForgeObjectDestroyThunk = void (*)(void* Address);

using FForgeObjectParameterizedConstruct = FForgeInvokeResult (*)(
    TConstArrayView<FForgeValue> Arguments,
    void*& OutAddress);

struct FORGERUNTIME_API FForgeOwnedConstructorBinding
{
    const TCHAR* SymbolId = TEXT("");
    const TCHAR* ScriptName = TEXT("");
    TConstArrayView<FForgeTypeRef> ParameterTypes;
    FForgeObjectParameterizedConstruct Construct = nullptr;
};

struct FORGERUNTIME_API FForgeFieldBinding
{
    FString SymbolId;
    FString QualifiedName;
    FName OwnerModule;
    FString BuildFingerprint;
    FForgeTypeRef Type;
    EForgeAccessSpecifier Access = EForgeAccessSpecifier::Public;
    FString ObjectType;
    FString ObjectTypeId;
    bool bStatic = false;
    bool bReadOnly = false;
    FString ScriptGetterName;
    FString ScriptSetterName;
    FForgeFieldGetter Getter = nullptr;
    FForgeFieldSetter Setter = nullptr;
};

enum class EForgeDelegateKind : uint8
{
    Singlecast,
    Multicast,
    ThreadSafeMulticast,
    DynamicSinglecast,
    DynamicMulticast
};

class FORGERUNTIME_API IForgeDelegateCallback
{
public:
    virtual ~IForgeDelegateCallback() = default;
    virtual FForgeInvokeResult Invoke(
        TArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn) = 0;
};

using FForgeDelegateCallback = TSharedPtr<IForgeDelegateCallback, ESPMode::ThreadSafe>;

struct FORGERUNTIME_API FForgeDelegateHandle
{
    FDelegateHandle NativeHandle;
    TStrongObjectPtr<UObject> DynamicProxy;
};

using FForgeDelegateBind = FForgeInvokeResult (*)(
    void* Instance,
    const FForgeDelegateCallback& Callback,
    FForgeDelegateHandle& OutHandle);

using FForgeDelegateUnbind = FForgeInvokeResult (*)(
    void* Instance,
    const FForgeDelegateHandle& Handle);

using FForgeDelegateClear = FForgeInvokeResult (*)(void* Instance);

using FForgeDelegateIsBound = FForgeInvokeResult (*)(
    void* Instance,
    bool& OutIsBound);

using FForgeDelegateTrigger = FForgeInvokeResult (*)(
    void* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn);

struct FORGERUNTIME_API FForgeDelegateBinding
{
    FString SymbolId;
    FString QualifiedName;
    FName OwnerModule;
    FString BuildFingerprint;
    EForgeDelegateKind Kind = EForgeDelegateKind::Singlecast;
    FForgeTypeRef ReturnType;
    TConstArrayView<FForgeTypeRef> ParameterTypes;
    EForgeAccessSpecifier Access = EForgeAccessSpecifier::Public;
    FString ObjectType;
    FString ObjectTypeId;
    bool bStatic = false;
    bool bUObjectOwner = false;
    FString ScriptBindName;
    FString ScriptUnbindName;
    FString ScriptIsBoundName;
    FString ScriptTriggerName;
    FString ScriptClearName;
    FForgeDelegateBind Bind = nullptr;
    FForgeDelegateUnbind Unbind = nullptr;
    FForgeDelegateClear Clear = nullptr;
    FForgeDelegateIsBound IsBound = nullptr;
    FForgeDelegateTrigger Trigger = nullptr;
};

struct FORGERUNTIME_API FForgeTypeFieldBinding
{
    const TCHAR* Name = TEXT("");
    FForgeTypeRef Type;
    int32 Offset = 0;
};

struct FORGERUNTIME_API FForgeEnumValueBinding
{
    const TCHAR* Name = TEXT("");
    int32 Value = 0;
};

enum class EForgeInstanceCapability : uint8
{
    None = 0,
    BorrowedRead = 1 << 0,
    BorrowedWrite = 1 << 1,
    MethodCall = 1 << 2,
    OwnedLifecycle = 1 << 3
};
ENUM_CLASS_FLAGS(EForgeInstanceCapability);

using FForgeInstanceCast = void* (*)(void* Address);

struct FORGERUNTIME_API FForgeBaseTypeBinding
{
    const TCHAR* TypeId = TEXT("");
    const TCHAR* QualifiedCppName = TEXT("");
    EForgeAccessSpecifier Access = EForgeAccessSpecifier::Public;
    bool bVirtual = false;
    FForgeInstanceCast Cast = nullptr;
};

struct FORGERUNTIME_API FForgeTypeBinding
{
    EForgeValueType Kind = EForgeValueType::Void;
    const TCHAR* ScriptName = TEXT("");
    const TCHAR* QualifiedCppName = TEXT("");
    FName OwnerModule;
    int32 Size = 0;
    TConstArrayView<FForgeTypeFieldBinding> Fields;
    TConstArrayView<FForgeEnumValueBinding> EnumValues;
    TConstArrayView<FForgeTypeRef> TemplateArguments;
    const TCHAR* TypeId = TEXT("");
    const TCHAR* BuildFingerprint = TEXT("");
    int32 Alignment = 0;
    TConstArrayView<FForgeBaseTypeBinding> BaseTypes;
    EForgeInstanceCapability InstanceCapabilities = EForgeInstanceCapability::None;
    FForgeObjectConstruct DefaultConstruct = nullptr;
    FForgeObjectConstruct CopyConstruct = nullptr;
    FForgeObjectConstruct MoveConstruct = nullptr;
    FForgeObjectDestroyThunk Destroy = nullptr;
    const TCHAR* ScriptNewName = TEXT("");
    const TCHAR* ScriptCopyName = TEXT("");
    const TCHAR* ScriptMoveName = TEXT("");
    bool bNonTrivialValue = false;
    FForgeValueDefaultConstructThunk ValueDefaultConstruct = nullptr;
    FForgeValueCopyConstructThunk ValueCopyConstruct = nullptr;
    FForgeValueMoveConstructThunk ValueMoveConstruct = nullptr;
    FForgeValueAssignThunk ValueAssign = nullptr;
    FForgeValueDestroyThunk ValueDestroy = nullptr;
    TConstArrayView<FForgeOwnedConstructorBinding> Constructors;
};

struct FORGERUNTIME_API FForgeFunctionBinding
{
    FString SymbolId;
    FString QualifiedName;
    FName OwnerModule;
    FString BuildFingerprint;
    void* TargetAddress = nullptr;
    void* ReplacementAddress = nullptr;
    FForgeInvoker Invoker = nullptr;
    FForgeTypeRef ReturnType;
    TConstArrayView<FForgeTypeRef> ParameterTypes;
    bool bInstanceMember = false;
    FString ObjectType;
    bool bPatchable = false;
    bool bOriginalCallable = false;
    FString ObjectTypeId;
    FString ScriptMethodName;
    bool bConstMember = false;
};

class IForgeBindingRegistry;
using FForgeRegisterModuleFunction = void (*)(IForgeBindingRegistry& Registry);

struct FORGERUNTIME_API FForgeModuleBindings
{
    FName OwnerModule;
    FString BuildFingerprint;
    FForgeRegisterModuleFunction Register = nullptr;
};

struct FORGERUNTIME_API FForgeRegistrationResult
{
    bool bSuccess = false;
    FString Error;

    static FForgeRegistrationResult Success();
    static FForgeRegistrationResult Failure(FString Message);
};

struct FORGERUNTIME_API FForgeInstanceCastResult
{
    bool bSuccess = false;
    FString Error;
    void* Address = nullptr;

    static FForgeInstanceCastResult Success(void* Address);
    static FForgeInstanceCastResult Failure(FString Message);
};

class FORGERUNTIME_API IForgeBindingRegistry
{
public:
    virtual ~IForgeBindingRegistry() = default;

    virtual FForgeRegistrationResult RegisterModule(
        const FForgeModuleBindings& Bindings) = 0;
    virtual void RegisterFunction(const FForgeFunctionBinding& Binding) = 0;
    virtual void RegisterField(const FForgeFieldBinding& Binding) = 0;
    virtual void RegisterDelegate(const FForgeDelegateBinding& Binding) = 0;
    virtual void RegisterType(const FForgeTypeBinding& Binding) = 0;
    virtual void RegisterReflectionMapping(
        const FForgeReflectionMappingBinding& Binding) = 0;
    virtual const FForgeFunctionBinding* FindFunction(
        const FString& SymbolId) const = 0;
    virtual const FForgeFunctionBinding* FindFunctionByName(
        const FString& QualifiedName) const = 0;
    virtual const FForgeFieldBinding* FindField(
        const FString& SymbolId) const = 0;
    virtual const FForgeFieldBinding* FindFieldByName(
        const FString& QualifiedName) const = 0;
    virtual const FForgeDelegateBinding* FindDelegate(
        const FString& SymbolId) const = 0;
    virtual const FForgeDelegateBinding* FindDelegateByName(
        const FString& QualifiedName) const = 0;
    virtual const FForgeTypeBinding* FindType(const FString& ScriptName) const = 0;
    virtual const FForgeTypeBinding* FindTypeById(const FString& TypeId) const = 0;
    virtual const FForgeTypeBinding* FindTypeByCppName(
        const FString& QualifiedCppName) const = 0;
    virtual FForgeInstanceCastResult CastInstance(
        FName SourceTypeId,
        FName TargetTypeId,
        void* Address) const = 0;
    virtual void GetFunctions(TArray<FForgeFunctionBinding>& OutBindings) const = 0;
    virtual void GetFields(TArray<FForgeFieldBinding>& OutBindings) const = 0;
    virtual void GetDelegates(TArray<FForgeDelegateBinding>& OutBindings) const = 0;
    virtual void GetTypes(TArray<FForgeTypeBinding>& OutBindings) const = 0;
    virtual const FForgeUnifiedReflection* FindReflection(
        const FString& StableKey) const = 0;
    virtual void GetReflections(
        TArray<FForgeUnifiedReflection>& OutBindings) const = 0;
    virtual void UnregisterModule(FName OwnerModule) = 0;
};

FORGERUNTIME_API void RegisterForgeModuleBindings(
    const FForgeModuleBindings& Bindings);

FORGERUNTIME_API FForgeInvokeResult InvokeForgePatchedFunction(
    const FString& SymbolId,
    void* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn);

class FORGERUNTIME_API FForgeAutoModuleRegistrar
{
public:
    explicit FForgeAutoModuleRegistrar(FForgeModuleBindings Bindings);
};
