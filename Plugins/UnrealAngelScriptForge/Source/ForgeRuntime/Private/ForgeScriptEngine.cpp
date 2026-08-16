#include "ForgeScriptEngine.h"

#include "ForgeDynamicDelegateProxy.h"
#include "angelscript.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
struct FForgeInvocationContext
{
    const FForgeFunctionBinding* Binding = nullptr;
    void* OriginalAddress = nullptr;
    void* Instance = nullptr;
    IForgeBindingRegistry* Registry = nullptr;
    IForgeObjectRegistry* ObjectRegistry = nullptr;
    FForgeDelegateRuntimeState* DelegateState = nullptr;
    EForgePatchMode Mode = EForgePatchMode::Replace;
};

thread_local FForgeInvocationContext* GInvocationContext = nullptr;
constexpr asDWORD ReplaceAccessMask = 1u << 0;
constexpr asDWORD WrapAccessMask = 1u << 1;

class FForgeStringFactory final : public asIStringFactory
{
public:
    virtual const void* GetStringConstant(const char* Data, asUINT Length) override
    {
        const FUTF8ToTCHAR Converted(Data, static_cast<int32>(Length));
        const FString Value(Converted.Length(), Converted.Get());
        if (const FString* const* Existing = Values.Find(Value))
        {
            ++References.FindChecked(*Existing);
            return *Existing;
        }
        TUniquePtr<FString> Stored = MakeUnique<FString>(Value);
        const FString* Address = Stored.Get();
        Storage.Add(Address, MoveTemp(Stored));
        Values.Add(Value, Address);
        References.Add(Address, 1);
        return Address;
    }

    virtual int ReleaseStringConstant(const void* String) override
    {
        const FString* Address = static_cast<const FString*>(String);
        int32* Count = References.Find(Address);
        if (Count == nullptr)
        {
            return asERROR;
        }
        if (--*Count == 0)
        {
            Values.Remove(*Address);
            References.Remove(Address);
            Storage.Remove(Address);
        }
        return asSUCCESS;
    }

    virtual int GetRawStringData(
        const void* String,
        char* Data,
        asUINT* Length) const override
    {
        if (String == nullptr || Length == nullptr)
        {
            return asINVALID_ARG;
        }
        const FTCHARToUTF8 Converted(*static_cast<const FString*>(String));
        *Length = static_cast<asUINT>(Converted.Length());
        if (Data != nullptr && Converted.Length() > 0)
        {
            FMemory::Memcpy(Data, Converted.Get(), Converted.Length());
        }
        return asSUCCESS;
    }

private:
    TMap<FString, const FString*> Values;
    TMap<const FString*, int32> References;
    TMap<const FString*, TUniquePtr<FString>> Storage;
};

const TCHAR* ScriptType(const FForgeTypeRef& Type)
{
    switch (Type.Kind)
    {
    case EForgeValueType::Void: return TEXT("void");
    case EForgeValueType::Bool: return TEXT("bool");
    case EForgeValueType::Int32: return TEXT("int");
    case EForgeValueType::Float: return TEXT("float");
    case EForgeValueType::Double: return TEXT("double");
    case EForgeValueType::String: return TEXT("FString");
    case EForgeValueType::Name: return TEXT("FName");
    case EForgeValueType::Vector: return TEXT("FVector");
    case EForgeValueType::Text: return TEXT("FText");
    case EForgeValueType::Rotator: return TEXT("FRotator");
    case EForgeValueType::Quat: return TEXT("FQuat");
    case EForgeValueType::Transform: return TEXT("FTransform");
    case EForgeValueType::Vector2D: return TEXT("FVector2D");
    case EForgeValueType::Guid: return TEXT("FGuid");
    case EForgeValueType::Color: return TEXT("FColor");
    case EForgeValueType::LinearColor: return TEXT("FLinearColor");
    case EForgeValueType::Object: return TEXT("UObjectHandle");
    case EForgeValueType::WeakObject: return TEXT("UObjectWeakHandle");
    case EForgeValueType::SoftObject: return TEXT("UObjectSoftHandle");
    case EForgeValueType::SoftClass: return TEXT("UClassSoftHandle");
    case EForgeValueType::NativeObject: return TEXT("ForgeObjectHandle");
    case EForgeValueType::Enum:
    case EForgeValueType::Struct:
    case EForgeValueType::Array:
    case EForgeValueType::Map:
    case EForgeValueType::Set:
    case EForgeValueType::Optional:
    case EForgeValueType::Variant:
    case EForgeValueType::Pair:
        return Type.Name;
    default: return TEXT("");
    }
}

bool IsScriptObjectValue(EForgeValueType Kind)
{
    return Kind == EForgeValueType::String ||
        Kind == EForgeValueType::Name ||
        Kind == EForgeValueType::Vector ||
        Kind == EForgeValueType::Text ||
        Kind == EForgeValueType::Rotator ||
        Kind == EForgeValueType::Quat ||
        Kind == EForgeValueType::Transform ||
        Kind == EForgeValueType::Vector2D ||
        Kind == EForgeValueType::Guid ||
        Kind == EForgeValueType::Color ||
        Kind == EForgeValueType::LinearColor ||
        Kind == EForgeValueType::WeakObject ||
        Kind == EForgeValueType::SoftObject ||
        Kind == EForgeValueType::SoftClass ||
        Kind == EForgeValueType::Struct ||
        Kind == EForgeValueType::Array ||
        Kind == EForgeValueType::Map ||
        Kind == EForgeValueType::Set ||
        Kind == EForgeValueType::Optional ||
        Kind == EForgeValueType::Variant ||
        Kind == EForgeValueType::Pair ||
        Kind == EForgeValueType::Object ||
        Kind == EForgeValueType::NativeObject;
}

FString ScriptParameter(const FForgeTypeRef& Type)
{
    if (Type.PassingMode == FForgeTypeRef::EPassingMode::InOutReference)
    {
        return FString::Printf(TEXT("%s &inout"), ScriptType(Type));
    }
    return IsScriptObjectValue(Type.Kind)
        ? FString::Printf(TEXT("const %s &in"), ScriptType(Type))
        : ScriptType(Type);
}

bool CloneInvocationValue(
    const FForgeValue& Value,
    const FForgeTypeRef& Type,
    IForgeBindingRegistry& Registry,
    FForgeValue& OutValue)
{
    if (Value.GetType() != Type.Kind)
    {
        return false;
    }
    switch (Type.Kind)
    {
    case EForgeValueType::Void:
        OutValue = FForgeValue::Void(); return true;
    case EForgeValueType::Bool:
        OutValue = FForgeValue::FromBool(Value.AsBool()); return true;
    case EForgeValueType::Int32:
        OutValue = FForgeValue::FromInt32(Value.AsInt32()); return true;
    case EForgeValueType::Float:
        OutValue = FForgeValue::FromFloat(Value.AsFloat()); return true;
    case EForgeValueType::Double:
        OutValue = FForgeValue::FromDouble(Value.AsDouble()); return true;
    case EForgeValueType::String:
        OutValue = FForgeValue::FromString(Value.AsString()); return true;
    case EForgeValueType::Name:
        OutValue = FForgeValue::FromName(Value.AsName()); return true;
    case EForgeValueType::Vector:
        OutValue = FForgeValue::FromVector(Value.AsVector()); return true;
    case EForgeValueType::Text:
        OutValue = FForgeValue::FromText(Value.AsText()); return true;
    case EForgeValueType::Rotator:
        OutValue = FForgeValue::FromRotator(Value.AsRotator()); return true;
    case EForgeValueType::Quat:
        OutValue = FForgeValue::FromQuat(Value.AsQuat()); return true;
    case EForgeValueType::Transform:
        OutValue = FForgeValue::FromTransform(Value.AsTransform()); return true;
    case EForgeValueType::Vector2D:
        OutValue = FForgeValue::FromVector2D(Value.AsVector2D()); return true;
    case EForgeValueType::Guid:
        OutValue = FForgeValue::FromGuid(Value.AsGuid()); return true;
    case EForgeValueType::Color:
        OutValue = FForgeValue::FromColor(Value.AsColor()); return true;
    case EForgeValueType::LinearColor:
        OutValue = FForgeValue::FromLinearColor(Value.AsLinearColor()); return true;
    case EForgeValueType::Object:
        OutValue = FForgeValue::FromObject(Value.AsObject()); return true;
    case EForgeValueType::WeakObject:
        OutValue = FForgeValue::FromWeakObject(Value.AsWeakObject().Get()); return true;
    case EForgeValueType::SoftObject:
        OutValue = FForgeValue::FromSoftObjectPath(Value.AsSoftObjectPath()); return true;
    case EForgeValueType::SoftClass:
        OutValue = FForgeValue::FromSoftClassPath(Value.AsSoftClassPath()); return true;
    case EForgeValueType::NativeObject:
        OutValue = FForgeValue::FromNativeObject(Value.AsNativeObject()); return true;
    case EForgeValueType::Enum:
        OutValue = FForgeValue::FromEnum(Value.AsEnum(Type.Name), Type.Name); return true;
    case EForgeValueType::Struct:
        if (const FForgeTypeBinding* Binding = Registry.FindType(Type.Name))
        {
            OutValue = Binding->bNonTrivialValue
                ? FForgeValue::FromManagedStructCopy(
                    Type.Name,
                    Value.GetStructData(),
                    Binding->Size,
                    Binding->Alignment,
                    Binding->ValueCopyConstruct,
                    Binding->ValueDestroy)
                : FForgeValue::FromStructBytes(
                    Type.Name,
                    Value.GetStructData(),
                    Binding->Size);
            return true;
        }
        return false;
    case EForgeValueType::Array:
    case EForgeValueType::Map:
    case EForgeValueType::Set:
    case EForgeValueType::Optional:
    case EForgeValueType::Variant:
    case EForgeValueType::Pair:
        if (const FForgeTypeBinding* Binding = Registry.FindType(Type.Name))
        {
            const FForgeContainer& Container = Value.AsContainer(Type.Kind, Type.Name);
            const int32 ExpectedTypeCount =
                Type.Kind == EForgeValueType::Map || Type.Kind == EForgeValueType::Pair
                    ? 2
                    : Type.Kind == EForgeValueType::Variant
                        ? Binding->TemplateArguments.Num()
                        : 1;
            if (Binding->TemplateArguments.Num() != ExpectedTypeCount ||
                (Type.Kind == EForgeValueType::Map && Container.Values.Num() % 2 != 0) ||
                (Type.Kind == EForgeValueType::Pair && Container.Values.Num() != 2) ||
                (Type.Kind == EForgeValueType::Optional && Container.Values.Num() > 1) ||
                (Type.Kind == EForgeValueType::Variant &&
                    (Container.Values.Num() != 1 ||
                     !Binding->TemplateArguments.IsValidIndex(Container.ActiveIndex))))
            {
                return false;
            }
            TArray<FForgeValue> ClonedValues;
            ClonedValues.Reserve(Container.Values.Num());
            for (int32 Index = 0; Index < Container.Values.Num(); ++Index)
            {
                const int32 TypeIndex = Type.Kind == EForgeValueType::Map || Type.Kind == EForgeValueType::Pair
                    ? Index % 2
                    : Type.Kind == EForgeValueType::Variant
                        ? Container.ActiveIndex
                        : 0;
                FForgeValue ClonedValue;
                if (!CloneInvocationValue(
                        Container.Values[Index],
                        Binding->TemplateArguments[TypeIndex],
                        Registry,
                        ClonedValue))
                {
                    return false;
                }
                ClonedValues.Add(MoveTemp(ClonedValue));
            }
            OutValue = FForgeValue::FromContainer(
                Type.Kind,
                Type.Name,
                MoveTemp(ClonedValues),
                Container.ActiveIndex);
            return true;
        }
        return false;
    default:
        return false;
    }
}

FString FunctionDeclaration(const FString& Name, const FForgeFunctionBinding& Binding)
{
    TArray<FString> Parameters;
    Parameters.Reserve(Binding.ParameterTypes.Num());
    for (const FForgeTypeRef& Type : Binding.ParameterTypes)
    {
        Parameters.Add(ScriptParameter(Type));
    }
    return FString::Printf(
        TEXT("%s %s(%s)"),
        ScriptType(Binding.ReturnType),
        *Name,
        *FString::Join(Parameters, TEXT(", ")));
}

FForgeValue ReadGenericArgument(
    asIScriptGeneric& Generic,
    int32 Index,
    const FForgeTypeRef& Type,
    IForgeBindingRegistry& Registry)
{
    if (Type.PassingMode == FForgeTypeRef::EPassingMode::InOutReference)
    {
        void* Address = Generic.GetArgAddress(Index);
        if (Address == nullptr) return FForgeValue::Void();
        switch (Type.Kind)
        {
        case EForgeValueType::Bool:
            return FForgeValue::FromBool(*static_cast<const bool*>(Address));
        case EForgeValueType::Int32:
            return FForgeValue::FromInt32(*static_cast<const int32*>(Address));
        case EForgeValueType::Float:
            return FForgeValue::FromFloat(*static_cast<const float*>(Address));
        case EForgeValueType::Double:
            return FForgeValue::FromDouble(*static_cast<const double*>(Address));
        case EForgeValueType::String:
            return FForgeValue::FromString(*static_cast<const FString*>(Address));
        case EForgeValueType::Name:
            return FForgeValue::FromName(*static_cast<const FName*>(Address));
        case EForgeValueType::Vector:
            return FForgeValue::FromVector(*static_cast<const FVector*>(Address));
        case EForgeValueType::Text:
            return FForgeValue::FromText(*static_cast<const FText*>(Address));
        case EForgeValueType::Rotator:
            return FForgeValue::FromRotator(*static_cast<const FRotator*>(Address));
        case EForgeValueType::Quat:
            return FForgeValue::FromQuat(*static_cast<const FQuat*>(Address));
        case EForgeValueType::Transform:
            return FForgeValue::FromTransform(*static_cast<const FTransform*>(Address));
        case EForgeValueType::Vector2D:
            return FForgeValue::FromVector2D(*static_cast<const FVector2D*>(Address));
        case EForgeValueType::Guid:
            return FForgeValue::FromGuid(*static_cast<const FGuid*>(Address));
        case EForgeValueType::Color:
            return FForgeValue::FromColor(*static_cast<const FColor*>(Address));
        case EForgeValueType::LinearColor:
            return FForgeValue::FromLinearColor(*static_cast<const FLinearColor*>(Address));
        case EForgeValueType::Object:
            return FForgeValue::FromObject(static_cast<const TStrongObjectPtr<UObject>*>(Address)->Get());
        case EForgeValueType::WeakObject:
            return FForgeValue::FromWeakObject(static_cast<const TWeakObjectPtr<UObject>*>(Address)->Get());
        case EForgeValueType::SoftObject:
            return FForgeValue::FromSoftObjectPath(*static_cast<const FSoftObjectPath*>(Address));
        case EForgeValueType::SoftClass:
            return FForgeValue::FromSoftClassPath(*static_cast<const FSoftObjectPath*>(Address));
        case EForgeValueType::Enum:
            return FForgeValue::FromEnum(*static_cast<const int32*>(Address), Type.Name);
        case EForgeValueType::Struct:
            if (const FForgeTypeBinding* Binding = Registry.FindType(Type.Name))
            {
                return Binding->bNonTrivialValue
                    ? FForgeValue::FromManagedStructCopy(Type.Name, Address,
                        Binding->Size, Binding->Alignment,
                        Binding->ValueCopyConstruct, Binding->ValueDestroy)
                    : FForgeValue::FromStructBytes(Type.Name, Address, Binding->Size);
            }
            return FForgeValue::Void();
        case EForgeValueType::Array:
        case EForgeValueType::Map:
        case EForgeValueType::Set:
        case EForgeValueType::Optional:
        case EForgeValueType::Variant:
        case EForgeValueType::Pair:
            return FForgeValue::FromContainer(Type.Kind, Type.Name,
                static_cast<const FForgeContainer*>(Address)->Values,
                static_cast<const FForgeContainer*>(Address)->ActiveIndex);
        default: return FForgeValue::Void();
        }
    }
    switch (Type.Kind)
    {
    case EForgeValueType::Bool: return FForgeValue::FromBool(Generic.GetArgByte(Index) != 0);
    case EForgeValueType::Int32: return FForgeValue::FromInt32(static_cast<int32>(Generic.GetArgDWord(Index)));
    case EForgeValueType::Float: return FForgeValue::FromFloat(Generic.GetArgFloat(Index));
    case EForgeValueType::Double: return FForgeValue::FromDouble(Generic.GetArgDouble(Index));
    case EForgeValueType::String:
        return FForgeValue::FromString(*static_cast<const FString*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Name:
        return FForgeValue::FromName(*static_cast<const FName*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Vector:
        return FForgeValue::FromVector(*static_cast<const FVector*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Text:
        return FForgeValue::FromText(*static_cast<const FText*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Rotator:
        return FForgeValue::FromRotator(*static_cast<const FRotator*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Quat:
        return FForgeValue::FromQuat(*static_cast<const FQuat*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Transform:
        return FForgeValue::FromTransform(*static_cast<const FTransform*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Vector2D:
        return FForgeValue::FromVector2D(*static_cast<const FVector2D*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Guid:
        return FForgeValue::FromGuid(*static_cast<const FGuid*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Color:
        return FForgeValue::FromColor(*static_cast<const FColor*>(Generic.GetArgObject(Index)));
    case EForgeValueType::LinearColor:
        return FForgeValue::FromLinearColor(*static_cast<const FLinearColor*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Object:
        return FForgeValue::FromObject(
            static_cast<const TStrongObjectPtr<UObject>*>(Generic.GetArgObject(Index))->Get());
    case EForgeValueType::WeakObject:
        return FForgeValue::FromWeakObject(
            static_cast<const TWeakObjectPtr<UObject>*>(Generic.GetArgObject(Index))->Get());
    case EForgeValueType::SoftObject:
        return FForgeValue::FromSoftObjectPath(
            *static_cast<const FSoftObjectPath*>(Generic.GetArgObject(Index)));
    case EForgeValueType::SoftClass:
        return FForgeValue::FromSoftClassPath(
            *static_cast<const FSoftObjectPath*>(Generic.GetArgObject(Index)));
    case EForgeValueType::NativeObject:
        return FForgeValue::FromNativeObject(
            *static_cast<const FForgeObjectHandle*>(Generic.GetArgObject(Index)));
    case EForgeValueType::Enum:
        return FForgeValue::FromEnum(
            static_cast<int32>(Generic.GetArgDWord(Index)), Type.Name);
    case EForgeValueType::Struct:
        if (const FForgeTypeBinding* Binding = Registry.FindType(Type.Name))
        {
            if (Binding->bNonTrivialValue)
            {
                return FForgeValue::FromManagedStructCopy(
                    Type.Name,
                    Generic.GetArgObject(Index),
                    Binding->Size,
                    Binding->Alignment,
                    Binding->ValueCopyConstruct,
                    Binding->ValueDestroy);
            }
            return FForgeValue::FromStructBytes(
                Type.Name, Generic.GetArgObject(Index), Binding->Size);
        }
        return FForgeValue::Void();
    case EForgeValueType::Array:
    case EForgeValueType::Map:
    case EForgeValueType::Set:
    case EForgeValueType::Optional:
    case EForgeValueType::Variant:
    case EForgeValueType::Pair:
        if (const FForgeContainer* Container =
                static_cast<const FForgeContainer*>(Generic.GetArgObject(Index)))
        {
            return FForgeValue::FromContainer(
                Type.Kind, Type.Name, Container->Values, Container->ActiveIndex);
        }
        return FForgeValue::Void();
    default: return FForgeValue::Void();
    }
}

bool WriteGenericInOutArgument(
    asIScriptGeneric& Generic,
    int32 Index,
    const FForgeTypeRef& Type,
    const FForgeValue& Value,
    IForgeBindingRegistry& Registry)
{
    if (Type.PassingMode != FForgeTypeRef::EPassingMode::InOutReference)
    {
        return true;
    }
    void* Address = Generic.GetArgAddress(Index);
    if (Address == nullptr) return false;
    switch (Type.Kind)
    {
    case EForgeValueType::Bool:
        *static_cast<bool*>(Address) = Value.AsBool(); return true;
    case EForgeValueType::Int32:
        *static_cast<int32*>(Address) = Value.AsInt32(); return true;
    case EForgeValueType::Float:
        *static_cast<float*>(Address) = Value.AsFloat(); return true;
    case EForgeValueType::Double:
        *static_cast<double*>(Address) = Value.AsDouble(); return true;
    case EForgeValueType::String:
        *static_cast<FString*>(Address) = Value.AsString(); return true;
    case EForgeValueType::Name:
        *static_cast<FName*>(Address) = Value.AsName(); return true;
    case EForgeValueType::Vector:
        *static_cast<FVector*>(Address) = Value.AsVector(); return true;
    case EForgeValueType::Text:
        *static_cast<FText*>(Address) = Value.AsText(); return true;
    case EForgeValueType::Rotator:
        *static_cast<FRotator*>(Address) = Value.AsRotator(); return true;
    case EForgeValueType::Quat:
        *static_cast<FQuat*>(Address) = Value.AsQuat(); return true;
    case EForgeValueType::Transform:
        *static_cast<FTransform*>(Address) = Value.AsTransform(); return true;
    case EForgeValueType::Vector2D:
        *static_cast<FVector2D*>(Address) = Value.AsVector2D(); return true;
    case EForgeValueType::Guid:
        *static_cast<FGuid*>(Address) = Value.AsGuid(); return true;
    case EForgeValueType::Color:
        *static_cast<FColor*>(Address) = Value.AsColor(); return true;
    case EForgeValueType::LinearColor:
        *static_cast<FLinearColor*>(Address) = Value.AsLinearColor(); return true;
    case EForgeValueType::Object:
        *static_cast<TStrongObjectPtr<UObject>*>(Address) = Value.AsStrongObject(); return true;
    case EForgeValueType::WeakObject:
        *static_cast<TWeakObjectPtr<UObject>*>(Address) = Value.AsWeakObject(); return true;
    case EForgeValueType::SoftObject:
        *static_cast<FSoftObjectPath*>(Address) = Value.AsSoftObjectPath(); return true;
    case EForgeValueType::SoftClass:
        *static_cast<FSoftObjectPath*>(Address) = Value.AsSoftClassPath(); return true;
    case EForgeValueType::Enum:
        *static_cast<int32*>(Address) = Value.AsEnum(Type.Name); return true;
    case EForgeValueType::Struct:
        if (const FForgeTypeBinding* Binding = Registry.FindType(Type.Name))
        {
            if (Binding->bNonTrivialValue)
            {
                Binding->ValueAssign(Address, Value.GetStructData());
            }
            else
            {
                FMemory::Memcpy(Address, Value.GetStructData(), Binding->Size);
            }
            return true;
        }
        return false;
    case EForgeValueType::Array:
    case EForgeValueType::Map:
    case EForgeValueType::Set:
    case EForgeValueType::Optional:
    case EForgeValueType::Variant:
    case EForgeValueType::Pair:
        *static_cast<FForgeContainer*>(Address) =
            Value.AsContainer(Type.Kind, Type.Name);
        return true;
    default: return false;
    }
}

void WriteGenericReturn(asIScriptGeneric& Generic, const FForgeValue& Value)
{
    switch (Value.GetType())
    {
    case EForgeValueType::Void: break;
    case EForgeValueType::Bool: Generic.SetReturnByte(Value.AsBool() ? 1 : 0); break;
    case EForgeValueType::Int32: Generic.SetReturnDWord(static_cast<asDWORD>(Value.AsInt32())); break;
    case EForgeValueType::Float: Generic.SetReturnFloat(Value.AsFloat()); break;
    case EForgeValueType::Double: Generic.SetReturnDouble(Value.AsDouble()); break;
    case EForgeValueType::String:
        new (Generic.GetAddressOfReturnLocation()) FString(Value.AsString());
        break;
    case EForgeValueType::Object:
        new (Generic.GetAddressOfReturnLocation()) TStrongObjectPtr<UObject>(Value.AsStrongObject());
        break;
    case EForgeValueType::WeakObject:
        new (Generic.GetAddressOfReturnLocation()) TWeakObjectPtr<UObject>(Value.AsWeakObject());
        break;
    case EForgeValueType::SoftObject:
        new (Generic.GetAddressOfReturnLocation()) FSoftObjectPath(Value.AsSoftObjectPath());
        break;
    case EForgeValueType::SoftClass:
        new (Generic.GetAddressOfReturnLocation()) FSoftObjectPath(Value.AsSoftClassPath());
        break;
    case EForgeValueType::NativeObject:
        new (Generic.GetAddressOfReturnLocation()) FForgeObjectHandle(Value.AsNativeObject());
        break;
    case EForgeValueType::Name:
        new (Generic.GetAddressOfReturnLocation()) FName(Value.AsName());
        break;
    case EForgeValueType::Vector:
        new (Generic.GetAddressOfReturnLocation()) FVector(Value.AsVector());
        break;
    case EForgeValueType::Text:
        new (Generic.GetAddressOfReturnLocation()) FText(Value.AsText()); break;
    case EForgeValueType::Rotator:
        new (Generic.GetAddressOfReturnLocation()) FRotator(Value.AsRotator()); break;
    case EForgeValueType::Quat:
        new (Generic.GetAddressOfReturnLocation()) FQuat(Value.AsQuat()); break;
    case EForgeValueType::Transform:
        new (Generic.GetAddressOfReturnLocation()) FTransform(Value.AsTransform()); break;
    case EForgeValueType::Vector2D:
        new (Generic.GetAddressOfReturnLocation()) FVector2D(Value.AsVector2D()); break;
    case EForgeValueType::Guid:
        new (Generic.GetAddressOfReturnLocation()) FGuid(Value.AsGuid()); break;
    case EForgeValueType::Color:
        new (Generic.GetAddressOfReturnLocation()) FColor(Value.AsColor()); break;
    case EForgeValueType::LinearColor:
        new (Generic.GetAddressOfReturnLocation()) FLinearColor(Value.AsLinearColor()); break;
    case EForgeValueType::Enum:
        Generic.SetReturnDWord(static_cast<asDWORD>(Value.AsEnum(*Value.GetReflectedTypeName())));
        break;
    case EForgeValueType::Struct:
        if (GInvocationContext != nullptr && GInvocationContext->Registry != nullptr)
        {
            if (const FForgeTypeBinding* Binding =
                    GInvocationContext->Registry->FindType(Value.GetReflectedTypeName()))
            {
                if (Binding->bNonTrivialValue)
                {
                    Binding->ValueCopyConstruct(
                        Generic.GetAddressOfReturnLocation(),
                        Value.GetStructData());
                    break;
                }
            }
        }
        FMemory::Memcpy(Generic.GetAddressOfReturnLocation(), Value.GetStructData(),
            Value.GetStructBytes().Num());
        break;
    case EForgeValueType::Array:
    case EForgeValueType::Map:
    case EForgeValueType::Set:
    case EForgeValueType::Optional:
    case EForgeValueType::Variant:
    case EForgeValueType::Pair:
        new (Generic.GetAddressOfReturnLocation()) FForgeContainer(
            Value.AsContainer(Value.GetType(), *Value.GetReflectedTypeName()));
        break;
    }
}

bool SetContextArgument(
    asIScriptContext& Context,
    int32 Index,
    FForgeValue& Value,
    const FForgeTypeRef& Type)
{
    if (Type.PassingMode == FForgeTypeRef::EPassingMode::InOutReference)
    {
        return Context.SetArgAddress(Index, Value.GetMutableData()) >= 0;
    }
    switch (Value.GetType())
    {
    case EForgeValueType::Bool: return Context.SetArgByte(Index, Value.AsBool() ? 1 : 0) >= 0;
    case EForgeValueType::Int32: return Context.SetArgDWord(Index, static_cast<asDWORD>(Value.AsInt32())) >= 0;
    case EForgeValueType::Float: return Context.SetArgFloat(Index, Value.AsFloat()) >= 0;
    case EForgeValueType::Double: return Context.SetArgDouble(Index, Value.AsDouble()) >= 0;
    case EForgeValueType::String:
        return Context.SetArgObject(Index, const_cast<FString*>(&Value.AsString())) >= 0;
    case EForgeValueType::Name:
        return Context.SetArgObject(Index, const_cast<FName*>(&Value.AsName())) >= 0;
    case EForgeValueType::Vector:
        return Context.SetArgObject(Index, const_cast<FVector*>(&Value.AsVector())) >= 0;
    case EForgeValueType::Text:
        return Context.SetArgObject(Index, const_cast<FText*>(&Value.AsText())) >= 0;
    case EForgeValueType::Rotator:
        return Context.SetArgObject(Index, const_cast<FRotator*>(&Value.AsRotator())) >= 0;
    case EForgeValueType::Quat:
        return Context.SetArgObject(Index, const_cast<FQuat*>(&Value.AsQuat())) >= 0;
    case EForgeValueType::Transform:
        return Context.SetArgObject(Index, const_cast<FTransform*>(&Value.AsTransform())) >= 0;
    case EForgeValueType::Vector2D:
        return Context.SetArgObject(Index, const_cast<FVector2D*>(&Value.AsVector2D())) >= 0;
    case EForgeValueType::Guid:
        return Context.SetArgObject(Index, const_cast<FGuid*>(&Value.AsGuid())) >= 0;
    case EForgeValueType::Color:
        return Context.SetArgObject(Index, const_cast<FColor*>(&Value.AsColor())) >= 0;
    case EForgeValueType::LinearColor:
        return Context.SetArgObject(Index, const_cast<FLinearColor*>(&Value.AsLinearColor())) >= 0;
    case EForgeValueType::Enum:
        return Context.SetArgDWord(Index, static_cast<asDWORD>(Value.AsEnum(*Value.GetReflectedTypeName()))) >= 0;
    case EForgeValueType::Struct:
        return Context.SetArgObject(
            Index,
            const_cast<void*>(Value.GetStructData())) >= 0;
    case EForgeValueType::Array:
    case EForgeValueType::Map:
    case EForgeValueType::Set:
    case EForgeValueType::Optional:
    case EForgeValueType::Variant:
    case EForgeValueType::Pair:
        return Context.SetArgObject(
            Index,
            const_cast<FForgeContainer*>(&Value.AsContainer(
                Value.GetType(), *Value.GetReflectedTypeName()))) >= 0;
    case EForgeValueType::Object:
        return Context.SetArgObject(
            Index,
            const_cast<TStrongObjectPtr<UObject>*>(&Value.AsStrongObject())) >= 0;
    case EForgeValueType::WeakObject:
        return Context.SetArgObject(
            Index,
            const_cast<TWeakObjectPtr<UObject>*>(&Value.AsWeakObject())) >= 0;
    case EForgeValueType::SoftObject:
        return Context.SetArgObject(
            Index,
            const_cast<FSoftObjectPath*>(&Value.AsSoftObjectPath())) >= 0;
    case EForgeValueType::SoftClass:
        return Context.SetArgObject(
            Index,
            const_cast<FSoftObjectPath*>(&Value.AsSoftClassPath())) >= 0;
    case EForgeValueType::NativeObject:
        return Context.SetArgObject(
            Index,
            const_cast<FForgeObjectHandle*>(&Value.AsNativeObject())) >= 0;
    default: return false;
    }
}

FForgeValue ReadContextReturn(
    asIScriptContext& Context,
    const FForgeTypeRef& Type,
    IForgeBindingRegistry& Registry)
{
    switch (Type.Kind)
    {
    case EForgeValueType::Void: return FForgeValue::Void();
    case EForgeValueType::Bool: return FForgeValue::FromBool(Context.GetReturnByte() != 0);
    case EForgeValueType::Int32: return FForgeValue::FromInt32(static_cast<int32>(Context.GetReturnDWord()));
    case EForgeValueType::Float: return FForgeValue::FromFloat(Context.GetReturnFloat());
    case EForgeValueType::Double: return FForgeValue::FromDouble(Context.GetReturnDouble());
    case EForgeValueType::String:
        return FForgeValue::FromString(*static_cast<const FString*>(Context.GetReturnObject()));
    case EForgeValueType::Name:
        return FForgeValue::FromName(*static_cast<const FName*>(Context.GetReturnObject()));
    case EForgeValueType::Vector:
        return FForgeValue::FromVector(*static_cast<const FVector*>(Context.GetReturnObject()));
    case EForgeValueType::Text:
        return FForgeValue::FromText(*static_cast<const FText*>(Context.GetReturnObject()));
    case EForgeValueType::Rotator:
        return FForgeValue::FromRotator(*static_cast<const FRotator*>(Context.GetReturnObject()));
    case EForgeValueType::Quat:
        return FForgeValue::FromQuat(*static_cast<const FQuat*>(Context.GetReturnObject()));
    case EForgeValueType::Transform:
        return FForgeValue::FromTransform(*static_cast<const FTransform*>(Context.GetReturnObject()));
    case EForgeValueType::Vector2D:
        return FForgeValue::FromVector2D(*static_cast<const FVector2D*>(Context.GetReturnObject()));
    case EForgeValueType::Guid:
        return FForgeValue::FromGuid(*static_cast<const FGuid*>(Context.GetReturnObject()));
    case EForgeValueType::Color:
        return FForgeValue::FromColor(*static_cast<const FColor*>(Context.GetReturnObject()));
    case EForgeValueType::LinearColor:
        return FForgeValue::FromLinearColor(*static_cast<const FLinearColor*>(Context.GetReturnObject()));
    case EForgeValueType::Enum:
        return FForgeValue::FromEnum(static_cast<int32>(Context.GetReturnDWord()), Type.Name);
    case EForgeValueType::Struct:
        if (const FForgeTypeBinding* Binding = Registry.FindType(Type.Name))
        {
            if (Binding->bNonTrivialValue)
            {
                return FForgeValue::FromManagedStructCopy(
                    Type.Name,
                    Context.GetReturnObject(),
                    Binding->Size,
                    Binding->Alignment,
                    Binding->ValueCopyConstruct,
                    Binding->ValueDestroy);
            }
            return FForgeValue::FromStructBytes(
                Type.Name, Context.GetReturnObject(), Binding->Size);
        }
        return FForgeValue::Void();
    case EForgeValueType::Array:
    case EForgeValueType::Map:
    case EForgeValueType::Set:
    case EForgeValueType::Optional:
    case EForgeValueType::Variant:
    case EForgeValueType::Pair:
        if (const FForgeContainer* Container =
                static_cast<const FForgeContainer*>(Context.GetReturnObject()))
        {
            return FForgeValue::FromContainer(
                Type.Kind, Type.Name, Container->Values, Container->ActiveIndex);
        }
        return FForgeValue::Void();
    case EForgeValueType::Object:
        return FForgeValue::FromObject(
            static_cast<const TStrongObjectPtr<UObject>*>(Context.GetReturnObject())->Get());
    case EForgeValueType::WeakObject:
        return FForgeValue::FromWeakObject(
            static_cast<const TWeakObjectPtr<UObject>*>(Context.GetReturnObject())->Get());
    case EForgeValueType::SoftObject:
        return FForgeValue::FromSoftObjectPath(
            *static_cast<const FSoftObjectPath*>(Context.GetReturnObject()));
    case EForgeValueType::SoftClass:
        return FForgeValue::FromSoftClassPath(
            *static_cast<const FSoftObjectPath*>(Context.GetReturnObject()));
    case EForgeValueType::NativeObject:
        return FForgeValue::FromNativeObject(
            *static_cast<const FForgeObjectHandle*>(Context.GetReturnObject()));
    default: return FForgeValue::Void();
    }
}

class FForgeScriptDelegateCallback final : public IForgeDelegateCallback
{
public:
    FForgeScriptDelegateCallback(
        FForgeDelegateRuntimeState& InState,
        asIScriptFunction& InFunction,
        const FForgeDelegateBinding& InBinding)
        : State(&InState)
        , Function(&InFunction)
        , ReturnType(InBinding.ReturnType)
        , ParameterTypes(InBinding.ParameterTypes)
    {
        Function->AddRef();
    }

    virtual ~FForgeScriptDelegateCallback() override
    {
        if (Function != nullptr)
        {
            Function->Release();
            Function = nullptr;
        }
    }

    virtual FForgeInvokeResult Invoke(
        TArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn) override
    {
        if (!IsInGameThread())
        {
            return FForgeInvokeResult::Failure(
                TEXT("AngelScript delegate callbacks require the game thread"));
        }
        if (State == nullptr || State->Engine == nullptr || State->Registry == nullptr ||
            State->ObjectRegistry == nullptr || Function == nullptr ||
            Arguments.Num() != ParameterTypes.Num())
        {
            return FForgeInvokeResult::Failure(TEXT("Forge delegate callback is stale"));
        }
        asIScriptContext* Context = State->Engine->CreateContext();
        if (Context == nullptr || Context->Prepare(Function) < 0)
        {
            if (Context != nullptr) Context->Release();
            return FForgeInvokeResult::Failure(
                TEXT("Failed to prepare AngelScript delegate callback"));
        }
        TArray<FForgeValue, TInlineAllocator<8>> WorkingArguments;
        WorkingArguments.Reserve(Arguments.Num());
        for (int32 Index = 0; Index < Arguments.Num(); ++Index)
        {
            FForgeValue Copy;
            if (!CloneInvocationValue(
                    Arguments[Index], ParameterTypes[Index], *State->Registry, Copy))
            {
                Context->Release();
                return FForgeInvokeResult::Failure(
                    TEXT("Forge delegate callback argument conversion failed"));
            }
            WorkingArguments.Add(MoveTemp(Copy));
            if (!SetContextArgument(
                    *Context, Index, WorkingArguments[Index], ParameterTypes[Index]))
            {
                Context->Release();
                return FForgeInvokeResult::Failure(
                    TEXT("Forge delegate callback argument setup failed"));
            }
        }
        FForgeInvocationContext Invocation{
            nullptr,
            nullptr,
            nullptr,
            State->Registry,
            State->ObjectRegistry,
            State,
            EForgePatchMode::Replace };
        FForgeInvocationContext* PreviousInvocation = GInvocationContext;
        GInvocationContext = &Invocation;
        const int ExecutionResult = Context->Execute();
        GInvocationContext = PreviousInvocation;
        if (ExecutionResult != asEXECUTION_FINISHED)
        {
            const FString Error = Context->GetExceptionString()
                ? UTF8_TO_TCHAR(Context->GetExceptionString())
                : TEXT("AngelScript delegate callback failed");
            Context->Release();
            return FForgeInvokeResult::Failure(Error);
        }
        OutReturn = ReadContextReturn(*Context, ReturnType, *State->Registry);
        Context->Release();
        for (int32 Index = 0; Index < Arguments.Num(); ++Index)
        {
            if (ParameterTypes[Index].PassingMode ==
                FForgeTypeRef::EPassingMode::InOutReference)
            {
                Arguments[Index] = MoveTemp(WorkingArguments[Index]);
            }
        }
        return FForgeInvokeResult::Success();
    }

private:
    FForgeDelegateRuntimeState* State = nullptr;
    asIScriptFunction* Function = nullptr;
    FForgeTypeRef ReturnType;
    TArray<FForgeTypeRef> ParameterTypes;
};

template <typename T>
void DefaultConstructGeneric(asIScriptGeneric* Generic)
{
    new (Generic->GetObject()) T();
}

template <typename T>
void CopyConstructGeneric(asIScriptGeneric* Generic)
{
    new (Generic->GetObject()) T(*static_cast<const T*>(Generic->GetArgObject(0)));
}

template <typename T>
void DestructGeneric(asIScriptGeneric* Generic)
{
    static_cast<T*>(Generic->GetObject())->~T();
}

template <typename T>
void AssignGeneric(asIScriptGeneric* Generic)
{
    T* Self = static_cast<T*>(Generic->GetObject());
    *Self = *static_cast<const T*>(Generic->GetArgObject(0));
    Generic->SetReturnAddress(Self);
}

void FStringDefaultConstruct(asIScriptGeneric* Generic) { DefaultConstructGeneric<FString>(Generic); }
void FStringCopyConstruct(asIScriptGeneric* Generic) { CopyConstructGeneric<FString>(Generic); }
void FStringDestruct(asIScriptGeneric* Generic) { DestructGeneric<FString>(Generic); }
void FStringAssign(asIScriptGeneric* Generic) { AssignGeneric<FString>(Generic); }
void FStringToUpper(asIScriptGeneric* Generic)
{
    new (Generic->GetAddressOfReturnLocation()) FString(
        static_cast<const FString*>(Generic->GetObject())->ToUpper());
}

void FNameDefaultConstruct(asIScriptGeneric* Generic) { DefaultConstructGeneric<FName>(Generic); }
void FNameCopyConstruct(asIScriptGeneric* Generic) { CopyConstructGeneric<FName>(Generic); }
void FNameDestruct(asIScriptGeneric* Generic) { DestructGeneric<FName>(Generic); }
void FNameAssign(asIScriptGeneric* Generic) { AssignGeneric<FName>(Generic); }
void FNameToString(asIScriptGeneric* Generic)
{
    new (Generic->GetAddressOfReturnLocation()) FString(
        static_cast<const FName*>(Generic->GetObject())->ToString());
}

void FVectorDefaultConstruct(asIScriptGeneric* Generic) { DefaultConstructGeneric<FVector>(Generic); }
void FVectorCopyConstruct(asIScriptGeneric* Generic) { CopyConstructGeneric<FVector>(Generic); }
void FVectorDestruct(asIScriptGeneric* Generic) { DestructGeneric<FVector>(Generic); }
void FVectorAssign(asIScriptGeneric* Generic) { AssignGeneric<FVector>(Generic); }
void FVectorSizeSquared(asIScriptGeneric* Generic)
{
    Generic->SetReturnDouble(static_cast<const FVector*>(Generic->GetObject())->SizeSquared());
}

#define FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(Prefix, NativeType) \
void Prefix##DefaultConstruct(asIScriptGeneric* Generic) { DefaultConstructGeneric<NativeType>(Generic); } \
void Prefix##CopyConstruct(asIScriptGeneric* Generic) { CopyConstructGeneric<NativeType>(Generic); } \
void Prefix##Destruct(asIScriptGeneric* Generic) { DestructGeneric<NativeType>(Generic); } \
void Prefix##Assign(asIScriptGeneric* Generic) { AssignGeneric<NativeType>(Generic); }

FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(FText, FText)
FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(FRotator, FRotator)
FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(FQuat, FQuat)
FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(FTransform, FTransform)
FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(FVector2D, FVector2D)
FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(FGuid, FGuid)
FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(FColor, FColor)
FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS(FLinearColor, FLinearColor)

#undef FORGE_DEFINE_SCRIPT_VALUE_BEHAVIOURS

void FTextToString(asIScriptGeneric* Generic)
{
    new (Generic->GetAddressOfReturnLocation()) FString(
        static_cast<const FText*>(Generic->GetObject())->ToString());
}

void FTransformGetTranslation(asIScriptGeneric* Generic)
{
    new (Generic->GetAddressOfReturnLocation()) FVector(
        static_cast<const FTransform*>(Generic->GetObject())->GetTranslation());
}

void FTransformSetTranslation(asIScriptGeneric* Generic)
{
    static_cast<FTransform*>(Generic->GetObject())->SetTranslation(
        *static_cast<const FVector*>(Generic->GetArgObject(0)));
}

using FForgeStrongObjectHandle = TStrongObjectPtr<UObject>;
using FForgeWeakObjectHandle = TWeakObjectPtr<UObject>;
using FForgeSoftObjectHandle = FSoftObjectPath;
using FForgeSoftClassHandle = FSoftObjectPath;

void UObjectHandleDefaultConstruct(asIScriptGeneric* Generic) { DefaultConstructGeneric<FForgeStrongObjectHandle>(Generic); }
void UObjectHandleCopyConstruct(asIScriptGeneric* Generic) { CopyConstructGeneric<FForgeStrongObjectHandle>(Generic); }
void UObjectHandleDestruct(asIScriptGeneric* Generic) { DestructGeneric<FForgeStrongObjectHandle>(Generic); }
void UObjectHandleAssign(asIScriptGeneric* Generic) { AssignGeneric<FForgeStrongObjectHandle>(Generic); }
void UObjectHandleIsValid(asIScriptGeneric* Generic)
{
    Generic->SetReturnByte(
        static_cast<const FForgeStrongObjectHandle*>(Generic->GetObject())->IsValid() ? 1 : 0);
}
void UObjectHandleGetName(asIScriptGeneric* Generic)
{
    const UObject* Object = static_cast<const FForgeStrongObjectHandle*>(Generic->GetObject())->Get();
    new (Generic->GetAddressOfReturnLocation()) FString(
        Object != nullptr ? Object->GetName() : FString{});
}
void UObjectHandleGetClassName(asIScriptGeneric* Generic)
{
    const UObject* Object = static_cast<const FForgeStrongObjectHandle*>(Generic->GetObject())->Get();
    new (Generic->GetAddressOfReturnLocation()) FString(
        Object != nullptr ? Object->GetClass()->GetName() : FString{});
}
void UObjectHandleCallFloat(asIScriptGeneric* Generic)
{
    UObject* Object = static_cast<const FForgeStrongObjectHandle*>(Generic->GetObject())->Get();
    const FName FunctionName = *static_cast<const FName*>(Generic->GetArgObject(0));
    const float Argument = Generic->GetArgFloat(1);
    UFunction* Function = Object != nullptr ? Object->FindFunction(FunctionName) : nullptr;
    FFloatProperty* InputProperty = nullptr;
    FFloatProperty* ReturnProperty = nullptr;
    if (Function != nullptr)
    {
        for (TFieldIterator<FProperty> Iterator(Function); Iterator; ++Iterator)
        {
            FProperty* Property = *Iterator;
            if (!Property->HasAnyPropertyFlags(CPF_Parm))
            {
                continue;
            }
            FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property);
            if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                ReturnProperty = FloatProperty;
            }
            else if (InputProperty == nullptr)
            {
                InputProperty = FloatProperty;
            }
            else
            {
                InputProperty = nullptr;
                break;
            }
        }
    }
    if (Object == nullptr || Function == nullptr || InputProperty == nullptr || ReturnProperty == nullptr)
    {
        if (asIScriptContext* Context = asGetActiveContext())
        {
            Context->SetException("UE Reflection adapter requires one float input and one float return");
        }
        return;
    }
    FStructOnScope Parameters(Function);
    InputProperty->SetPropertyValue_InContainer(Parameters.GetStructMemory(), Argument);
    Object->ProcessEvent(Function, Parameters.GetStructMemory());
    Generic->SetReturnFloat(
        ReturnProperty->GetPropertyValue_InContainer(Parameters.GetStructMemory()));
}

void UObjectWeakHandleDefaultConstruct(asIScriptGeneric* Generic) { DefaultConstructGeneric<FForgeWeakObjectHandle>(Generic); }
void UObjectWeakHandleCopyConstruct(asIScriptGeneric* Generic) { CopyConstructGeneric<FForgeWeakObjectHandle>(Generic); }
void UObjectWeakHandleDestruct(asIScriptGeneric* Generic) { DestructGeneric<FForgeWeakObjectHandle>(Generic); }
void UObjectWeakHandleAssign(asIScriptGeneric* Generic) { AssignGeneric<FForgeWeakObjectHandle>(Generic); }
void UObjectWeakHandleIsValid(asIScriptGeneric* Generic)
{
    Generic->SetReturnByte(
        static_cast<const FForgeWeakObjectHandle*>(Generic->GetObject())->IsValid() ? 1 : 0);
}
void UObjectWeakHandlePin(asIScriptGeneric* Generic)
{
    new (Generic->GetAddressOfReturnLocation()) FForgeStrongObjectHandle(
        static_cast<const FForgeWeakObjectHandle*>(Generic->GetObject())->Get());
}

template <typename TSoftHandle>
void SoftHandleDefaultConstruct(asIScriptGeneric* Generic) { DefaultConstructGeneric<TSoftHandle>(Generic); }
template <typename TSoftHandle>
void SoftHandleCopyConstruct(asIScriptGeneric* Generic) { CopyConstructGeneric<TSoftHandle>(Generic); }
template <typename TSoftHandle>
void SoftHandleDestruct(asIScriptGeneric* Generic) { DestructGeneric<TSoftHandle>(Generic); }
template <typename TSoftHandle>
void SoftHandleAssign(asIScriptGeneric* Generic) { AssignGeneric<TSoftHandle>(Generic); }
template <typename TSoftHandle>
void SoftHandleIsNull(asIScriptGeneric* Generic)
{
    Generic->SetReturnByte(
        static_cast<const TSoftHandle*>(Generic->GetObject())->IsNull() ? 1 : 0);
}
template <typename TSoftHandle>
void SoftHandleGetPath(asIScriptGeneric* Generic)
{
    new (Generic->GetAddressOfReturnLocation()) FString(
        static_cast<const TSoftHandle*>(Generic->GetObject())->ToString());
}
template <typename TSoftHandle>
UObject* ResolveSoftHandle(asIScriptGeneric* Generic)
{
    return static_cast<const TSoftHandle*>(Generic->GetObject())->ResolveObject();
}
void UObjectSoftHandleIsLoaded(asIScriptGeneric* Generic)
{
    Generic->SetReturnByte(ResolveSoftHandle<FForgeSoftObjectHandle>(Generic) != nullptr ? 1 : 0);
}
void UObjectSoftHandleResolve(asIScriptGeneric* Generic)
{
    new (Generic->GetAddressOfReturnLocation()) FForgeStrongObjectHandle(
        ResolveSoftHandle<FForgeSoftObjectHandle>(Generic));
}
void UClassSoftHandleIsLoaded(asIScriptGeneric* Generic)
{
    Generic->SetReturnByte(
        Cast<UClass>(ResolveSoftHandle<FForgeSoftClassHandle>(Generic)) != nullptr ? 1 : 0);
}
void UClassSoftHandleResolve(asIScriptGeneric* Generic)
{
    new (Generic->GetAddressOfReturnLocation()) FForgeStrongObjectHandle(
        Cast<UClass>(ResolveSoftHandle<FForgeSoftClassHandle>(Generic)));
}

void ForgeObjectHandleDefaultConstruct(asIScriptGeneric* Generic)
{
    DefaultConstructGeneric<FForgeObjectHandle>(Generic);
}

void ForgeObjectHandleCopyConstruct(asIScriptGeneric* Generic)
{
    CopyConstructGeneric<FForgeObjectHandle>(Generic);
}

void ForgeObjectHandleDestruct(asIScriptGeneric* Generic)
{
    DestructGeneric<FForgeObjectHandle>(Generic);
}

void ForgeObjectHandleAssign(asIScriptGeneric* Generic)
{
    AssignGeneric<FForgeObjectHandle>(Generic);
}

void ForgeObjectHandleIsValid(asIScriptGeneric* Generic)
{
    const FForgeObjectHandle& Handle =
        *static_cast<const FForgeObjectHandle*>(Generic->GetObject());
    const bool bValid = GInvocationContext != nullptr &&
        GInvocationContext->ObjectRegistry != nullptr &&
        GInvocationContext->ObjectRegistry->Resolve(
            Handle,
            Handle.GetTypeName(),
            false).bSuccess;
    Generic->SetReturnByte(bValid ? 1 : 0);
}

void ForgeObjectHandleGetTypeName(asIScriptGeneric* Generic)
{
    const FForgeObjectHandle& Handle =
        *static_cast<const FForgeObjectHandle*>(Generic->GetObject());
    new (Generic->GetAddressOfReturnLocation()) FString(
        Handle.GetTypeName().ToString());
}

void ForgeObjectHandleGetTypeId(asIScriptGeneric* Generic)
{
    const FForgeObjectHandle& Handle =
        *static_cast<const FForgeObjectHandle*>(Generic->GetObject());
    new (Generic->GetAddressOfReturnLocation()) FString(
        Handle.GetTypeId().ToString());
}

void ForgeObjectHandleIsReadOnly(asIScriptGeneric* Generic)
{
    const FForgeObjectHandle& Handle =
        *static_cast<const FForgeObjectHandle*>(Generic->GetObject());
    Generic->SetReturnByte(Handle.IsReadOnly() ? 1 : 0);
}

void ForgeObjectHandleIsOwned(asIScriptGeneric* Generic)
{
    const FForgeObjectHandle& Handle =
        *static_cast<const FForgeObjectHandle*>(Generic->GetObject());
    Generic->SetReturnByte(Handle.IsOwned() ? 1 : 0);
}

const FForgeTypeBinding* ContainerBinding(asIScriptGeneric* Generic)
{
    asITypeInfo* TypeInfo = Generic->GetEngine()->GetTypeInfoById(Generic->GetObjectTypeId());
    return TypeInfo != nullptr
        ? static_cast<const FForgeTypeBinding*>(TypeInfo->GetUserData())
        : nullptr;
}

void CleanupContainerTypeMetadata(asITypeInfo* TypeInfo)
{
    delete static_cast<FForgeTypeBinding*>(TypeInfo->GetUserData());
    TypeInfo->SetUserData(nullptr);
}

void SetScriptException(const char* Message)
{
    if (asIScriptContext* Context = asGetActiveContext())
    {
        Context->SetException(Message);
    }
}

void SetScriptException(const FString& Message)
{
    FTCHARToUTF8 MessageUtf8(*Message);
    SetScriptException(MessageUtf8.Get());
}

const FForgeTypeBinding* ReflectedValueBinding(asIScriptGeneric* Generic)
{
    asITypeInfo* TypeInfo = Generic->GetEngine()->GetTypeInfoById(Generic->GetObjectTypeId());
    return TypeInfo != nullptr
        ? static_cast<const FForgeTypeBinding*>(TypeInfo->GetUserData())
        : nullptr;
}

void ReflectedValueDefaultConstruct(asIScriptGeneric* Generic)
{
    if (const FForgeTypeBinding* Binding = ReflectedValueBinding(Generic))
    {
        Binding->ValueDefaultConstruct(Generic->GetObject());
    }
}

void ReflectedValueCopyConstruct(asIScriptGeneric* Generic)
{
    if (const FForgeTypeBinding* Binding = ReflectedValueBinding(Generic))
    {
        Binding->ValueCopyConstruct(Generic->GetObject(), Generic->GetArgObject(0));
    }
}

void ReflectedValueDestruct(asIScriptGeneric* Generic)
{
    if (const FForgeTypeBinding* Binding = ReflectedValueBinding(Generic))
    {
        Binding->ValueDestroy(Generic->GetObject());
    }
}

void ReflectedValueAssign(asIScriptGeneric* Generic)
{
    if (const FForgeTypeBinding* Binding = ReflectedValueBinding(Generic))
    {
        Binding->ValueAssign(Generic->GetObject(), Generic->GetArgObject(0));
        Generic->SetReturnAddress(Generic->GetObject());
    }
}

void ContainerDefaultConstruct(asIScriptGeneric* Generic)
{
    FForgeContainer* Container = new (Generic->GetObject()) FForgeContainer();
    if (const FForgeTypeBinding* Binding = ContainerBinding(Generic))
    {
        Container->Kind = Binding->Kind;
        Container->TypeName = Binding->ScriptName;
    }
}

void ContainerCopyConstruct(asIScriptGeneric* Generic) { CopyConstructGeneric<FForgeContainer>(Generic); }
void ContainerDestruct(asIScriptGeneric* Generic) { DestructGeneric<FForgeContainer>(Generic); }
void ContainerAssign(asIScriptGeneric* Generic) { AssignGeneric<FForgeContainer>(Generic); }

bool ResolveContainerCall(
    asIScriptGeneric* Generic,
    FForgeContainer*& OutContainer,
    const FForgeTypeBinding*& OutBinding,
    IForgeBindingRegistry*& OutRegistry)
{
    OutContainer = static_cast<FForgeContainer*>(Generic->GetObject());
    OutBinding = ContainerBinding(Generic);
    OutRegistry = GInvocationContext != nullptr ? GInvocationContext->Registry : nullptr;
    if (OutContainer == nullptr || OutBinding == nullptr || OutRegistry == nullptr)
    {
        SetScriptException("Forge container operation is outside an active Patch invocation");
        return false;
    }
    return true;
}

void ContainerNum(asIScriptGeneric* Generic)
{
    const FForgeContainer* Container = static_cast<const FForgeContainer*>(Generic->GetObject());
    const FForgeTypeBinding* Binding = ContainerBinding(Generic);
    if (Container == nullptr || Binding == nullptr)
    {
        SetScriptException("Forge container metadata is unavailable");
        return;
    }
    Generic->SetReturnDWord(static_cast<asDWORD>(
        Binding->Kind == EForgeValueType::Map
            ? Container->Values.Num() / 2
            : Container->Values.Num()));
}

bool ValidateContainerIndex(
    const FForgeContainer& Container,
    const FForgeTypeBinding& Binding,
    int32 Index)
{
    const int32 Count = Binding.Kind == EForgeValueType::Map
        ? Container.Values.Num() / 2
        : Container.Values.Num();
    if (Index < 0 || Index >= Count)
    {
        SetScriptException("Forge container index is out of range");
        return false;
    }
    return true;
}

void ContainerAt(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    const int32 Index = static_cast<int32>(Generic->GetArgDWord(0));
    if (!ValidateContainerIndex(*Container, *Binding, Index)) return;
    (void)Registry;
    WriteGenericReturn(*Generic, Container->Values[Index]);
}

void ContainerSet(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    const int32 Index = static_cast<int32>(Generic->GetArgDWord(0));
    if (!ValidateContainerIndex(*Container, *Binding, Index)) return;
    Container->Values[Index] = ReadGenericArgument(
        *Generic, 1, Binding->TemplateArguments[0], *Registry);
}

void ContainerAdd(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    Container->Values.Add(ReadGenericArgument(
        *Generic, 0, Binding->TemplateArguments[0], *Registry));
    if (Binding->Kind == EForgeValueType::Map)
    {
        Container->Values.Add(ReadGenericArgument(
            *Generic, 1, Binding->TemplateArguments[1], *Registry));
    }
}

void ContainerMapKeyAt(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    const int32 Index = static_cast<int32>(Generic->GetArgDWord(0));
    if (!ValidateContainerIndex(*Container, *Binding, Index)) return;
    (void)Registry;
    WriteGenericReturn(*Generic, Container->Values[Index * 2]);
}

void ContainerMapValueAt(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    const int32 Index = static_cast<int32>(Generic->GetArgDWord(0));
    if (!ValidateContainerIndex(*Container, *Binding, Index)) return;
    (void)Registry;
    WriteGenericReturn(*Generic, Container->Values[Index * 2 + 1]);
}

void OptionalHasValue(asIScriptGeneric* Generic)
{
    const FForgeContainer* Container = static_cast<const FForgeContainer*>(Generic->GetObject());
    Generic->SetReturnByte(Container != nullptr && Container->Values.Num() == 1 ? 1 : 0);
}

void OptionalGetValue(asIScriptGeneric* Generic)
{
    const FForgeContainer* Container = static_cast<const FForgeContainer*>(Generic->GetObject());
    if (Container == nullptr || Container->Values.Num() != 1)
    {
        SetScriptException("Forge optional has no value");
        return;
    }
    WriteGenericReturn(*Generic, Container->Values[0]);
}

void OptionalSetValue(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    Container->Values.Reset(1);
    Container->Values.Add(ReadGenericArgument(*Generic, 0, Binding->TemplateArguments[0], *Registry));
    Container->ActiveIndex = 0;
}

void OptionalReset(asIScriptGeneric* Generic)
{
    FForgeContainer* Container = static_cast<FForgeContainer*>(Generic->GetObject());
    if (Container != nullptr)
    {
        Container->Values.Reset();
        Container->ActiveIndex = INDEX_NONE;
    }
}

void PairGetFirst(asIScriptGeneric* Generic)
{
    const FForgeContainer* Container = static_cast<const FForgeContainer*>(Generic->GetObject());
    if (Container == nullptr || Container->Values.Num() != 2)
    {
        SetScriptException("Forge pair storage is corrupt");
        return;
    }
    WriteGenericReturn(*Generic, Container->Values[0]);
}

void PairGetSecond(asIScriptGeneric* Generic)
{
    const FForgeContainer* Container = static_cast<const FForgeContainer*>(Generic->GetObject());
    if (Container == nullptr || Container->Values.Num() != 2)
    {
        SetScriptException("Forge pair storage is corrupt");
        return;
    }
    WriteGenericReturn(*Generic, Container->Values[1]);
}

void PairSetFirst(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    if (Container->Values.Num() != 2)
    {
        SetScriptException("Forge pair must be initialized with Set before setting one element");
        return;
    }
    Container->Values[0] = ReadGenericArgument(*Generic, 0, Binding->TemplateArguments[0], *Registry);
}

void PairSetSecond(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    if (Container->Values.Num() != 2)
    {
        SetScriptException("Forge pair must be initialized with Set before setting one element");
        return;
    }
    Container->Values[1] = ReadGenericArgument(*Generic, 0, Binding->TemplateArguments[1], *Registry);
}

void PairSet(asIScriptGeneric* Generic)
{
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry)) return;
    Container->Values.Reset(2);
    Container->Values.Add(ReadGenericArgument(
        *Generic, 0, Binding->TemplateArguments[0], *Registry));
    Container->Values.Add(ReadGenericArgument(
        *Generic, 1, Binding->TemplateArguments[1], *Registry));
}

int32 VariantAlternativeIndex(asIScriptGeneric* Generic, const char* Prefix)
{
    const char* Name = Generic->GetFunction() != nullptr ? Generic->GetFunction()->GetName() : nullptr;
    if (Name == nullptr || FCStringAnsi::Strncmp(Name, Prefix, FCStringAnsi::Strlen(Prefix)) != 0)
    {
        return INDEX_NONE;
    }
    return FCStringAnsi::Atoi(Name + FCStringAnsi::Strlen(Prefix));
}

void VariantGetIndex(asIScriptGeneric* Generic)
{
    const FForgeContainer* Container = static_cast<const FForgeContainer*>(Generic->GetObject());
    Generic->SetReturnDWord(static_cast<asDWORD>(
        Container != nullptr ? Container->ActiveIndex : INDEX_NONE));
}

void VariantIsIndex(asIScriptGeneric* Generic)
{
    const FForgeContainer* Container = static_cast<const FForgeContainer*>(Generic->GetObject());
    Generic->SetReturnByte(Container != nullptr &&
        Container->ActiveIndex == static_cast<int32>(Generic->GetArgDWord(0)) ? 1 : 0);
}

void VariantGet(asIScriptGeneric* Generic)
{
    const int32 Index = VariantAlternativeIndex(Generic, "Get");
    const FForgeContainer* Container = static_cast<const FForgeContainer*>(Generic->GetObject());
    if (Container == nullptr || Container->Values.Num() != 1 || Container->ActiveIndex != Index)
    {
        SetScriptException("Forge variant alternative is not active");
        return;
    }
    WriteGenericReturn(*Generic, Container->Values[0]);
}

void VariantSet(asIScriptGeneric* Generic)
{
    const int32 Index = VariantAlternativeIndex(Generic, "Set");
    FForgeContainer* Container;
    const FForgeTypeBinding* Binding;
    IForgeBindingRegistry* Registry;
    if (!ResolveContainerCall(Generic, Container, Binding, Registry) ||
        !Binding->TemplateArguments.IsValidIndex(Index)) return;
    Container->Values.Reset(1);
    Container->Values.Add(ReadGenericArgument(*Generic, 0, Binding->TemplateArguments[Index], *Registry));
    Container->ActiveIndex = Index;
}

bool RegisterValueType(
    asIScriptEngine& Engine,
    const char* Name,
    int32 Size,
    asDWORD Flags,
    asGENFUNC_t DefaultConstruct,
    asGENFUNC_t CopyConstruct,
    asGENFUNC_t Destruct,
    asGENFUNC_t Assign)
{
    if (Engine.RegisterObjectType(Name, Size, asOBJ_VALUE | Flags) < 0)
    {
        return false;
    }
    const FString CopyDeclaration = FString::Printf(TEXT("void f(const %hs &in)"), Name);
    const FString AssignDeclaration = FString::Printf(TEXT("%hs &opAssign(const %hs &in)"), Name, Name);
    FTCHARToUTF8 CopyUtf8(*CopyDeclaration);
    FTCHARToUTF8 AssignUtf8(*AssignDeclaration);
    return
        Engine.RegisterObjectBehaviour(Name, asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(DefaultConstruct), asCALL_GENERIC) >= 0 &&
        Engine.RegisterObjectBehaviour(Name, asBEHAVE_CONSTRUCT, CopyUtf8.Get(), asFUNCTION(CopyConstruct), asCALL_GENERIC) >= 0 &&
        Engine.RegisterObjectBehaviour(Name, asBEHAVE_DESTRUCT, "void f()", asFUNCTION(Destruct), asCALL_GENERIC) >= 0 &&
        Engine.RegisterObjectMethod(Name, AssignUtf8.Get(), asFUNCTION(Assign), asCALL_GENERIC) >= 0;
}

bool RegisterForgeValueTypes(asIScriptEngine& Engine)
{
    if (!RegisterValueType(
            Engine, "FString", sizeof(FString), asGetTypeTraits<FString>(),
            FStringDefaultConstruct, FStringCopyConstruct, FStringDestruct, FStringAssign) ||
        Engine.RegisterObjectMethod("FString", "FString ToUpper() const", asFUNCTION(FStringToUpper), asCALL_GENERIC) < 0 ||
        !RegisterValueType(
            Engine, "FName", sizeof(FName), asGetTypeTraits<FName>(),
            FNameDefaultConstruct, FNameCopyConstruct, FNameDestruct, FNameAssign) ||
        Engine.RegisterObjectMethod("FName", "FString ToString() const", asFUNCTION(FNameToString), asCALL_GENERIC) < 0 ||
        !RegisterValueType(
            Engine, "FVector", sizeof(FVector), asGetTypeTraits<FVector>(),
            FVectorDefaultConstruct, FVectorCopyConstruct, FVectorDestruct, FVectorAssign) ||
        Engine.RegisterObjectProperty("FVector", "double X", asOFFSET(FVector, X)) < 0 ||
        Engine.RegisterObjectProperty("FVector", "double Y", asOFFSET(FVector, Y)) < 0 ||
        Engine.RegisterObjectProperty("FVector", "double Z", asOFFSET(FVector, Z)) < 0 ||
        Engine.RegisterObjectMethod("FVector", "double SizeSquared() const", asFUNCTION(FVectorSizeSquared), asCALL_GENERIC) < 0 ||
        !RegisterValueType(
            Engine, "FText", sizeof(FText), asGetTypeTraits<FText>(),
            FTextDefaultConstruct, FTextCopyConstruct, FTextDestruct, FTextAssign) ||
        Engine.RegisterObjectMethod("FText", "FString ToString() const", asFUNCTION(FTextToString), asCALL_GENERIC) < 0 ||
        !RegisterValueType(
            Engine, "FRotator", sizeof(FRotator), asGetTypeTraits<FRotator>(),
            FRotatorDefaultConstruct, FRotatorCopyConstruct, FRotatorDestruct, FRotatorAssign) ||
        Engine.RegisterObjectProperty("FRotator", "double Pitch", asOFFSET(FRotator, Pitch)) < 0 ||
        Engine.RegisterObjectProperty("FRotator", "double Yaw", asOFFSET(FRotator, Yaw)) < 0 ||
        Engine.RegisterObjectProperty("FRotator", "double Roll", asOFFSET(FRotator, Roll)) < 0 ||
        !RegisterValueType(
            Engine, "FQuat", sizeof(FQuat), asGetTypeTraits<FQuat>(),
            FQuatDefaultConstruct, FQuatCopyConstruct, FQuatDestruct, FQuatAssign) ||
        Engine.RegisterObjectProperty("FQuat", "double X", asOFFSET(FQuat, X)) < 0 ||
        Engine.RegisterObjectProperty("FQuat", "double Y", asOFFSET(FQuat, Y)) < 0 ||
        Engine.RegisterObjectProperty("FQuat", "double Z", asOFFSET(FQuat, Z)) < 0 ||
        Engine.RegisterObjectProperty("FQuat", "double W", asOFFSET(FQuat, W)) < 0 ||
        !RegisterValueType(
            Engine, "FTransform", sizeof(FTransform), asGetTypeTraits<FTransform>(),
            FTransformDefaultConstruct, FTransformCopyConstruct, FTransformDestruct, FTransformAssign) ||
        Engine.RegisterObjectMethod(
            "FTransform", "FVector GetTranslation() const",
            asFUNCTION(FTransformGetTranslation), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "FTransform", "void SetTranslation(const FVector &in)",
            asFUNCTION(FTransformSetTranslation), asCALL_GENERIC) < 0 ||
        !RegisterValueType(
            Engine, "FVector2D", sizeof(FVector2D), asGetTypeTraits<FVector2D>(),
            FVector2DDefaultConstruct, FVector2DCopyConstruct, FVector2DDestruct, FVector2DAssign) ||
        Engine.RegisterObjectProperty("FVector2D", "double X", asOFFSET(FVector2D, X)) < 0 ||
        Engine.RegisterObjectProperty("FVector2D", "double Y", asOFFSET(FVector2D, Y)) < 0 ||
        !RegisterValueType(
            Engine, "FGuid", sizeof(FGuid), asGetTypeTraits<FGuid>(),
            FGuidDefaultConstruct, FGuidCopyConstruct, FGuidDestruct, FGuidAssign) ||
        Engine.RegisterObjectProperty("FGuid", "uint A", asOFFSET(FGuid, A)) < 0 ||
        Engine.RegisterObjectProperty("FGuid", "uint B", asOFFSET(FGuid, B)) < 0 ||
        Engine.RegisterObjectProperty("FGuid", "uint C", asOFFSET(FGuid, C)) < 0 ||
        Engine.RegisterObjectProperty("FGuid", "uint D", asOFFSET(FGuid, D)) < 0 ||
        !RegisterValueType(
            Engine, "FColor", sizeof(FColor), asGetTypeTraits<FColor>(),
            FColorDefaultConstruct, FColorCopyConstruct, FColorDestruct, FColorAssign) ||
        Engine.RegisterObjectProperty("FColor", "uint8 R", asOFFSET(FColor, R)) < 0 ||
        Engine.RegisterObjectProperty("FColor", "uint8 G", asOFFSET(FColor, G)) < 0 ||
        Engine.RegisterObjectProperty("FColor", "uint8 B", asOFFSET(FColor, B)) < 0 ||
        Engine.RegisterObjectProperty("FColor", "uint8 A", asOFFSET(FColor, A)) < 0 ||
        !RegisterValueType(
            Engine, "FLinearColor", sizeof(FLinearColor), asGetTypeTraits<FLinearColor>(),
            FLinearColorDefaultConstruct, FLinearColorCopyConstruct,
            FLinearColorDestruct, FLinearColorAssign) ||
        Engine.RegisterObjectProperty("FLinearColor", "float R", asOFFSET(FLinearColor, R)) < 0 ||
        Engine.RegisterObjectProperty("FLinearColor", "float G", asOFFSET(FLinearColor, G)) < 0 ||
        Engine.RegisterObjectProperty("FLinearColor", "float B", asOFFSET(FLinearColor, B)) < 0 ||
        Engine.RegisterObjectProperty("FLinearColor", "float A", asOFFSET(FLinearColor, A)) < 0)
    {
        return false;
    }
    if (!RegisterValueType(
            Engine,
            "UObjectHandle",
            sizeof(FForgeStrongObjectHandle),
            asGetTypeTraits<FForgeStrongObjectHandle>(),
            UObjectHandleDefaultConstruct,
            UObjectHandleCopyConstruct,
            UObjectHandleDestruct,
            UObjectHandleAssign) ||
        Engine.RegisterObjectMethod(
            "UObjectHandle", "bool IsValid() const", asFUNCTION(UObjectHandleIsValid), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UObjectHandle", "FString GetName() const", asFUNCTION(UObjectHandleGetName), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UObjectHandle", "FString GetClassName() const", asFUNCTION(UObjectHandleGetClassName), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UObjectHandle",
            "float CallFloat(const FName &in, float) const",
            asFUNCTION(UObjectHandleCallFloat),
            asCALL_GENERIC) < 0)
    {
        return false;
    }
    if (!RegisterValueType(
            Engine,
            "UObjectWeakHandle",
            sizeof(FForgeWeakObjectHandle),
            asGetTypeTraits<FForgeWeakObjectHandle>(),
            UObjectWeakHandleDefaultConstruct,
            UObjectWeakHandleCopyConstruct,
            UObjectWeakHandleDestruct,
            UObjectWeakHandleAssign) ||
        Engine.RegisterObjectMethod(
            "UObjectWeakHandle", "bool IsValid() const",
            asFUNCTION(UObjectWeakHandleIsValid), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UObjectWeakHandle", "UObjectHandle Pin() const",
            asFUNCTION(UObjectWeakHandlePin), asCALL_GENERIC) < 0)
    {
        return false;
    }
    if (!RegisterValueType(
            Engine,
            "UObjectSoftHandle",
            sizeof(FForgeSoftObjectHandle),
            asGetTypeTraits<FForgeSoftObjectHandle>(),
            SoftHandleDefaultConstruct<FForgeSoftObjectHandle>,
            SoftHandleCopyConstruct<FForgeSoftObjectHandle>,
            SoftHandleDestruct<FForgeSoftObjectHandle>,
            SoftHandleAssign<FForgeSoftObjectHandle>) ||
        Engine.RegisterObjectMethod(
            "UObjectSoftHandle", "bool IsNull() const",
            asFUNCTION(SoftHandleIsNull<FForgeSoftObjectHandle>), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UObjectSoftHandle", "bool IsLoaded() const",
            asFUNCTION(UObjectSoftHandleIsLoaded), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UObjectSoftHandle", "FString GetPath() const",
            asFUNCTION(SoftHandleGetPath<FForgeSoftObjectHandle>), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UObjectSoftHandle", "UObjectHandle Resolve() const",
            asFUNCTION(UObjectSoftHandleResolve), asCALL_GENERIC) < 0)
    {
        return false;
    }
    if (!RegisterValueType(
            Engine,
            "UClassSoftHandle",
            sizeof(FForgeSoftClassHandle),
            asGetTypeTraits<FForgeSoftClassHandle>(),
            SoftHandleDefaultConstruct<FForgeSoftClassHandle>,
            SoftHandleCopyConstruct<FForgeSoftClassHandle>,
            SoftHandleDestruct<FForgeSoftClassHandle>,
            SoftHandleAssign<FForgeSoftClassHandle>) ||
        Engine.RegisterObjectMethod(
            "UClassSoftHandle", "bool IsNull() const",
            asFUNCTION(SoftHandleIsNull<FForgeSoftClassHandle>), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UClassSoftHandle", "bool IsLoaded() const",
            asFUNCTION(UClassSoftHandleIsLoaded), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UClassSoftHandle", "FString GetPath() const",
            asFUNCTION(SoftHandleGetPath<FForgeSoftClassHandle>), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "UClassSoftHandle", "UObjectHandle Resolve() const",
            asFUNCTION(UClassSoftHandleResolve), asCALL_GENERIC) < 0)
    {
        return false;
    }
    if (!RegisterValueType(
            Engine,
            "ForgeObjectHandle",
            sizeof(FForgeObjectHandle),
            asGetTypeTraits<FForgeObjectHandle>(),
            ForgeObjectHandleDefaultConstruct,
            ForgeObjectHandleCopyConstruct,
            ForgeObjectHandleDestruct,
            ForgeObjectHandleAssign) ||
        Engine.RegisterObjectMethod(
            "ForgeObjectHandle",
            "bool IsValid() const",
            asFUNCTION(ForgeObjectHandleIsValid),
            asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "ForgeObjectHandle",
            "FString GetTypeName() const",
            asFUNCTION(ForgeObjectHandleGetTypeName),
            asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "ForgeObjectHandle",
            "FString GetTypeId() const",
            asFUNCTION(ForgeObjectHandleGetTypeId),
            asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "ForgeObjectHandle",
            "bool IsReadOnly() const",
            asFUNCTION(ForgeObjectHandleIsReadOnly),
            asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(
            "ForgeObjectHandle",
            "bool IsOwned() const",
            asFUNCTION(ForgeObjectHandleIsOwned),
            asCALL_GENERIC) < 0)
    {
        return false;
    }
    return true;
}

bool RegisterContainerType(
    asIScriptEngine& Engine,
    const FForgeTypeBinding& Binding,
    FString& OutError)
{
    FTCHARToUTF8 TypeNameUtf8(Binding.ScriptName);
    if (!RegisterValueType(
            Engine,
            TypeNameUtf8.Get(),
            sizeof(FForgeContainer),
            asGetTypeTraits<FForgeContainer>(),
            ContainerDefaultConstruct,
            ContainerCopyConstruct,
            ContainerDestruct,
            ContainerAssign))
    {
        OutError = FString::Printf(
            TEXT("Failed to register AngelScript container %s"), Binding.ScriptName);
        return false;
    }
    asITypeInfo* TypeInfo = Engine.GetTypeInfoByDecl(TypeNameUtf8.Get());
    if (TypeInfo == nullptr)
    {
        OutError = FString::Printf(
            TEXT("AngelScript container type metadata is unavailable: %s"), Binding.ScriptName);
        return false;
    }
    TypeInfo->SetUserData(new FForgeTypeBinding(Binding));

    const FString NumDeclaration = TEXT("uint Num() const");
    if (Engine.RegisterObjectMethod(
            TypeNameUtf8.Get(), TCHAR_TO_UTF8(*NumDeclaration),
            asFUNCTION(ContainerNum), asCALL_GENERIC) < 0)
    {
        OutError = FString::Printf(TEXT("Failed to register %s.Num"), Binding.ScriptName);
        return false;
    }

    const FForgeTypeRef& First = Binding.TemplateArguments[0];
    if (Binding.Kind == EForgeValueType::Optional)
    {
        const FString GetValue = FString::Printf(TEXT("%s GetValue() const"), ScriptType(First));
        const FString SetValue = FString::Printf(TEXT("void SetValue(%s)"), *ScriptParameter(First));
        FTCHARToUTF8 GetValueUtf8(*GetValue);
        FTCHARToUTF8 SetValueUtf8(*SetValue);
        if (Engine.RegisterObjectMethod(TypeNameUtf8.Get(), "bool HasValue() const",
                asFUNCTION(OptionalHasValue), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), GetValueUtf8.Get(),
                asFUNCTION(OptionalGetValue), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), SetValueUtf8.Get(),
                asFUNCTION(OptionalSetValue), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), "void Reset()",
                asFUNCTION(OptionalReset), asCALL_GENERIC) < 0)
        {
            OutError = FString::Printf(TEXT("Failed to register %s optional methods"), Binding.ScriptName);
            return false;
        }
        return true;
    }
    if (Binding.Kind == EForgeValueType::Pair)
    {
        const FForgeTypeRef& Second = Binding.TemplateArguments[1];
        const FString GetFirst = FString::Printf(TEXT("%s GetFirst() const"), ScriptType(First));
        const FString GetSecond = FString::Printf(TEXT("%s GetSecond() const"), ScriptType(Second));
        const FString SetFirst = FString::Printf(TEXT("void SetFirst(%s)"), *ScriptParameter(First));
        const FString SetSecond = FString::Printf(TEXT("void SetSecond(%s)"), *ScriptParameter(Second));
        const FString Set = FString::Printf(
            TEXT("void Set(%s, %s)"), *ScriptParameter(First), *ScriptParameter(Second));
        FTCHARToUTF8 GetFirstUtf8(*GetFirst);
        FTCHARToUTF8 GetSecondUtf8(*GetSecond);
        FTCHARToUTF8 SetFirstUtf8(*SetFirst);
        FTCHARToUTF8 SetSecondUtf8(*SetSecond);
        FTCHARToUTF8 SetUtf8(*Set);
        if (Engine.RegisterObjectMethod(TypeNameUtf8.Get(), GetFirstUtf8.Get(),
                asFUNCTION(PairGetFirst), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), GetSecondUtf8.Get(),
                asFUNCTION(PairGetSecond), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), SetFirstUtf8.Get(),
                asFUNCTION(PairSetFirst), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), SetSecondUtf8.Get(),
                asFUNCTION(PairSetSecond), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), SetUtf8.Get(),
                asFUNCTION(PairSet), asCALL_GENERIC) < 0)
        {
            OutError = FString::Printf(TEXT("Failed to register %s pair methods"), Binding.ScriptName);
            return false;
        }
        return true;
    }
    if (Binding.Kind == EForgeValueType::Variant)
    {
        if (Engine.RegisterObjectMethod(TypeNameUtf8.Get(), "uint GetIndex() const",
                asFUNCTION(VariantGetIndex), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), "bool IsIndex(uint) const",
                asFUNCTION(VariantIsIndex), asCALL_GENERIC) < 0)
        {
            OutError = FString::Printf(TEXT("Failed to register %s variant index methods"), Binding.ScriptName);
            return false;
        }
        for (int32 Index = 0; Index < Binding.TemplateArguments.Num(); ++Index)
        {
            const FForgeTypeRef& Alternative = Binding.TemplateArguments[Index];
            const FString Get = FString::Printf(
                TEXT("%s Get%d() const"), ScriptType(Alternative), Index);
            const FString Set = FString::Printf(
                TEXT("void Set%d(%s)"), Index, *ScriptParameter(Alternative));
            FTCHARToUTF8 GetUtf8(*Get);
            FTCHARToUTF8 SetUtf8(*Set);
            if (Engine.RegisterObjectMethod(TypeNameUtf8.Get(), GetUtf8.Get(),
                    asFUNCTION(VariantGet), asCALL_GENERIC) < 0 ||
                Engine.RegisterObjectMethod(TypeNameUtf8.Get(), SetUtf8.Get(),
                    asFUNCTION(VariantSet), asCALL_GENERIC) < 0)
            {
                OutError = FString::Printf(
                    TEXT("Failed to register %s variant alternative %d"), Binding.ScriptName, Index);
                return false;
            }
        }
        return true;
    }
    if (Binding.Kind == EForgeValueType::Map)
    {
        const FForgeTypeRef& Second = Binding.TemplateArguments[1];
        const FString KeyAt = FString::Printf(TEXT("%s KeyAt(uint) const"), ScriptType(First));
        const FString ValueAt = FString::Printf(TEXT("%s ValueAt(uint) const"), ScriptType(Second));
        const FString Add = FString::Printf(
            TEXT("void Add(%s, %s)"), *ScriptParameter(First), *ScriptParameter(Second));
        FTCHARToUTF8 KeyAtUtf8(*KeyAt);
        FTCHARToUTF8 ValueAtUtf8(*ValueAt);
        FTCHARToUTF8 AddUtf8(*Add);
        if (Engine.RegisterObjectMethod(TypeNameUtf8.Get(), KeyAtUtf8.Get(),
                asFUNCTION(ContainerMapKeyAt), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), ValueAtUtf8.Get(),
                asFUNCTION(ContainerMapValueAt), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), AddUtf8.Get(),
                asFUNCTION(ContainerAdd), asCALL_GENERIC) < 0)
        {
            OutError = FString::Printf(TEXT("Failed to register %s map methods"), Binding.ScriptName);
            return false;
        }
        return true;
    }

    const FString At = FString::Printf(TEXT("%s At(uint) const"), ScriptType(First));
    const FString Add = FString::Printf(TEXT("void Add(%s)"), *ScriptParameter(First));
    FTCHARToUTF8 AtUtf8(*At);
    FTCHARToUTF8 AddUtf8(*Add);
    if (Engine.RegisterObjectMethod(TypeNameUtf8.Get(), AtUtf8.Get(),
            asFUNCTION(ContainerAt), asCALL_GENERIC) < 0 ||
        Engine.RegisterObjectMethod(TypeNameUtf8.Get(), AddUtf8.Get(),
            asFUNCTION(ContainerAdd), asCALL_GENERIC) < 0)
    {
        OutError = FString::Printf(TEXT("Failed to register %s container methods"), Binding.ScriptName);
        return false;
    }
    if (Binding.Kind == EForgeValueType::Array)
    {
        const FString Set = FString::Printf(
            TEXT("void Set(uint, %s)"), *ScriptParameter(First));
        FTCHARToUTF8 SetUtf8(*Set);
        if (Engine.RegisterObjectMethod(TypeNameUtf8.Get(), SetUtf8.Get(),
                asFUNCTION(ContainerSet), asCALL_GENERIC) < 0)
        {
            OutError = FString::Printf(TEXT("Failed to register %s.Set"), Binding.ScriptName);
            return false;
        }
    }
    return true;
}

bool EnsureReflectedTypeRegistered(
    asIScriptEngine& Engine,
    IForgeBindingRegistry& Registry,
    const FForgeTypeRef& Type,
    FString& OutError)
{
    if (Type.Kind != EForgeValueType::Enum &&
        Type.Kind != EForgeValueType::Struct &&
        Type.Kind != EForgeValueType::Array &&
        Type.Kind != EForgeValueType::Map &&
        Type.Kind != EForgeValueType::Set &&
        Type.Kind != EForgeValueType::Optional &&
        Type.Kind != EForgeValueType::Variant &&
        Type.Kind != EForgeValueType::Pair)
    {
        return true;
    }
    FTCHARToUTF8 TypeNameUtf8(Type.Name);
    if (Engine.GetTypeIdByDecl(TypeNameUtf8.Get()) >= 0)
    {
        return true;
    }
    const FForgeTypeBinding* Binding = Registry.FindType(Type.Name);
    if (Binding == nullptr || Binding->Kind != Type.Kind)
    {
        OutError = FString::Printf(TEXT("Generated Forge type is not registered: %s"), Type.Name);
        return false;
    }
    if (Type.Kind == EForgeValueType::Enum)
    {
        if (Engine.RegisterEnum(TypeNameUtf8.Get()) < 0)
        {
            OutError = FString::Printf(TEXT("Failed to register AngelScript enum %s"), Type.Name);
            return false;
        }
        for (const FForgeEnumValueBinding& Value : Binding->EnumValues)
        {
            FTCHARToUTF8 ValueNameUtf8(Value.Name);
            if (Engine.RegisterEnumValue(TypeNameUtf8.Get(), ValueNameUtf8.Get(), Value.Value) < 0)
            {
                OutError = FString::Printf(
                    TEXT("Failed to register enum value %s::%s"), Type.Name, Value.Name);
                return false;
            }
        }
        return true;
    }

    if (Type.Kind == EForgeValueType::Array ||
        Type.Kind == EForgeValueType::Map ||
        Type.Kind == EForgeValueType::Set ||
        Type.Kind == EForgeValueType::Optional ||
        Type.Kind == EForgeValueType::Variant ||
        Type.Kind == EForgeValueType::Pair)
    {
        for (const FForgeTypeRef& Argument : Binding->TemplateArguments)
        {
            if (!EnsureReflectedTypeRegistered(Engine, Registry, Argument, OutError))
            {
                return false;
            }
        }
        return RegisterContainerType(Engine, *Binding, OutError);
    }

    for (const FForgeTypeFieldBinding& Field : Binding->Fields)
    {
        if (!EnsureReflectedTypeRegistered(Engine, Registry, Field.Type, OutError))
        {
            return false;
        }
    }
    const asDWORD StructFlags = Binding->bNonTrivialValue
        ? asOBJ_VALUE | asOBJ_APP_CLASS_CDAK |
            (Binding->Alignment >= 16 ? asOBJ_APP_ALIGN16 : 0)
        : asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS | asOBJ_APP_CLASS_ALLINTS;
    const int32 RegisterTypeResult = Engine.RegisterObjectType(
            TypeNameUtf8.Get(), Binding->Size, StructFlags);
    if (RegisterTypeResult < 0)
    {
        OutError = FString::Printf(
            TEXT("Failed to register AngelScript struct %s (%d)"),
            Type.Name,
            RegisterTypeResult);
        return false;
    }
    if (Binding->bNonTrivialValue)
    {
        asITypeInfo* TypeInfo = Engine.GetTypeInfoByDecl(TypeNameUtf8.Get());
        if (TypeInfo == nullptr)
        {
            OutError = FString::Printf(
                TEXT("AngelScript value metadata is unavailable: %s"), Type.Name);
            return false;
        }
        TypeInfo->SetUserData(new FForgeTypeBinding(*Binding));
        const FString CopyDeclaration = FString::Printf(
            TEXT("void f(const %s &in)"), Type.Name);
        const FString AssignDeclaration = FString::Printf(
            TEXT("%s &opAssign(const %s &in)"), Type.Name, Type.Name);
        FTCHARToUTF8 CopyUtf8(*CopyDeclaration);
        FTCHARToUTF8 AssignUtf8(*AssignDeclaration);
        if (Engine.RegisterObjectBehaviour(TypeNameUtf8.Get(), asBEHAVE_CONSTRUCT,
                "void f()", asFUNCTION(ReflectedValueDefaultConstruct), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectBehaviour(TypeNameUtf8.Get(), asBEHAVE_CONSTRUCT,
                CopyUtf8.Get(), asFUNCTION(ReflectedValueCopyConstruct), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectBehaviour(TypeNameUtf8.Get(), asBEHAVE_DESTRUCT,
                "void f()", asFUNCTION(ReflectedValueDestruct), asCALL_GENERIC) < 0 ||
            Engine.RegisterObjectMethod(TypeNameUtf8.Get(), AssignUtf8.Get(),
                asFUNCTION(ReflectedValueAssign), asCALL_GENERIC) < 0)
        {
            OutError = FString::Printf(
                TEXT("Failed to register non-trivial value lifecycle for %s"), Type.Name);
            return false;
        }
    }
    for (const FForgeTypeFieldBinding& Field : Binding->Fields)
    {
        const FString Declaration = FString::Printf(
            TEXT("%s %s"), ScriptType(Field.Type), Field.Name);
        FTCHARToUTF8 DeclarationUtf8(*Declaration);
        if (Engine.RegisterObjectProperty(
                TypeNameUtf8.Get(), DeclarationUtf8.Get(), Field.Offset) < 0)
        {
            OutError = FString::Printf(
                TEXT("Failed to register struct field %s.%s"), Type.Name, Field.Name);
            return false;
        }
    }
    return true;
}

bool EnsureBindingTypesRegistered(
    asIScriptEngine& Engine,
    IForgeBindingRegistry& Registry,
    const FForgeFunctionBinding& Binding,
    FString& OutError)
{
    if (!EnsureReflectedTypeRegistered(Engine, Registry, Binding.ReturnType, OutError))
    {
        return false;
    }
    for (const FForgeTypeRef& Type : Binding.ParameterTypes)
    {
        if (!EnsureReflectedTypeRegistered(Engine, Registry, Type, OutError))
        {
            return false;
        }
    }
    return true;
}

bool AcquireForgeInstance(
    const FForgeObjectHandle& Handle,
    const FString& ExpectedTypeId,
    bool bRequireMutable,
    IForgeBindingRegistry& Registry,
    IForgeObjectRegistry& ObjectRegistry,
    FForgeObjectLeaseResult& OutLease,
    void*& OutInstance,
    FString& OutError)
{
    OutLease = ObjectRegistry.Acquire(
        Handle,
        Handle.GetTypeId(),
        Handle.GetTypeName(),
        bRequireMutable);
    if (!OutLease.bSuccess)
    {
        OutError = OutLease.Error;
        return false;
    }
    const FForgeInstanceCastResult CastResult = Registry.CastInstance(
        Handle.GetTypeId(),
        FName(*ExpectedTypeId),
        OutLease.Lease.GetAddress());
    if (!CastResult.bSuccess)
    {
        OutError = CastResult.Error;
        return false;
    }
    OutInstance = CastResult.Address;
    return true;
}

bool ValidateLoadedDelegate(const FForgeDelegateBinding& Binding, FString& OutError)
{
    if (GInvocationContext == nullptr || GInvocationContext->Registry == nullptr ||
        GInvocationContext->DelegateState == nullptr)
    {
        OutError = TEXT("Forge delegate operation called outside an active script invocation");
        return false;
    }
    const FForgeDelegateBinding* Loaded =
        GInvocationContext->Registry->FindDelegate(Binding.SymbolId);
    if (Loaded == nullptr || Loaded->BuildFingerprint != Binding.BuildFingerprint ||
        Loaded->Bind != Binding.Bind || Loaded->Unbind != Binding.Unbind ||
        Loaded->Trigger != Binding.Trigger)
    {
        OutError = TEXT("Forge delegate binding is stale or unloaded");
        return false;
    }
    return true;
}

bool AcquireDelegateOwner(
    asIScriptGeneric& Generic,
    const FForgeDelegateBinding& Binding,
    FForgeObjectLeaseResult& OutLease,
    FForgeObjectHandle& OutHandle,
    TWeakObjectPtr<UObject>& OutUObject,
    void*& OutInstance,
    FString& OutError)
{
    OutInstance = nullptr;
    if (Binding.bStatic)
    {
        return true;
    }
    if (Binding.bUObjectOwner)
    {
        const TStrongObjectPtr<UObject>* Handle =
            static_cast<const TStrongObjectPtr<UObject>*>(Generic.GetObject());
        UObject* Object = Handle != nullptr ? Handle->Get() : nullptr;
        if (Object == nullptr)
        {
            OutError = TEXT("Forge dynamic delegate owner is no longer valid");
            return false;
        }
        OutUObject = Object;
        OutInstance = Object;
        return true;
    }
    const FForgeObjectHandle* Handle =
        static_cast<const FForgeObjectHandle*>(Generic.GetObject());
    if (Handle == nullptr || GInvocationContext == nullptr ||
        GInvocationContext->Registry == nullptr ||
        GInvocationContext->ObjectRegistry == nullptr)
    {
        OutError = TEXT("Forge delegate property requires an object registry");
        return false;
    }
    OutHandle = *Handle;
    return AcquireForgeInstance(
        *Handle,
        Binding.ObjectTypeId,
        true,
        *GInvocationContext->Registry,
        *GInvocationContext->ObjectRegistry,
        OutLease,
        OutInstance,
        OutError);
}

bool DelegateOwnerMatches(
    const FForgeDelegateSubscription& Subscription,
    const FForgeDelegateBinding& Binding,
    const FForgeObjectHandle& Owner,
    const TWeakObjectPtr<UObject>& UObjectOwner)
{
    if (Binding.bStatic) return true;
    return Binding.bUObjectOwner
        ? Subscription.UObjectOwner == UObjectOwner
        : Subscription.Owner == Owner;
}

void ReleaseDynamicDelegateProxy(FForgeDelegateHandle& Handle)
{
    if (auto* Proxy = Cast<UForgeDynamicDelegateProxy>(Handle.DynamicProxy.Get()))
    {
        Proxy->ResetForgeCallback();
    }
    Handle.DynamicProxy.Reset();
}

bool RemoveDelegateSubscription(
    FForgeDelegateRuntimeState& State,
    int32 Token,
    FString* OutError = nullptr)
{
    FForgeDelegateSubscription* Subscription = State.Subscriptions.Find(Token);
    if (Subscription == nullptr)
    {
        if (OutError != nullptr) *OutError = TEXT("Unknown Forge delegate subscription token");
        return false;
    }
    FForgeObjectLeaseResult Lease;
    void* Instance = nullptr;
    if (!Subscription->Binding.bStatic)
    {
        if (Subscription->Binding.bUObjectOwner)
        {
            Instance = Subscription->UObjectOwner.Get();
            if (Instance == nullptr)
            {
                ReleaseDynamicDelegateProxy(Subscription->NativeHandle);
                State.Subscriptions.Remove(Token);
                return true;
            }
        }
        else
        {
            Lease = State.ObjectRegistry->Acquire(
                Subscription->Owner,
                FName(*Subscription->Binding.ObjectTypeId),
                FName(*Subscription->Binding.ObjectType),
                true);
            if (!Lease.bSuccess)
            {
                // The owner is already invalid, so its native delegate storage no longer needs touching.
                State.Subscriptions.Remove(Token);
                return true;
            }
            const FForgeInstanceCastResult Cast = State.Registry->CastInstance(
                Subscription->Owner.GetTypeId(),
                FName(*Subscription->Binding.ObjectTypeId),
                Lease.Lease.GetAddress());
            if (!Cast.bSuccess)
            {
                if (OutError != nullptr) *OutError = Cast.Error;
                return false;
            }
            Instance = Cast.Address;
        }
    }
    const FForgeInvokeResult Result = Subscription->Binding.Unbind(
        Instance, Subscription->NativeHandle);
    if (!Result.bSuccess)
    {
        if (OutError != nullptr) *OutError = Result.Error;
        return false;
    }
    ReleaseDynamicDelegateProxy(Subscription->NativeHandle);
    State.Subscriptions.Remove(Token);
    return true;
}

void RemoveDelegateSubscriptionsForModule(
    FForgeDelegateRuntimeState& State,
    const FString& ModuleName)
{
    TArray<int32> Tokens;
    for (const TPair<int32, FForgeDelegateSubscription>& Pair : State.Subscriptions)
    {
        if (Pair.Value.ModuleName == ModuleName)
        {
            Tokens.Add(Pair.Key);
        }
    }
    for (const int32 Token : Tokens)
    {
        RemoveDelegateSubscription(State, Token);
    }
}

FForgeDelegateCallback FindDelegateCallback(
    FForgeDelegateRuntimeState& State,
    const FForgeDelegateBinding& Binding,
    const FString& CallbackName,
    FString& OutModuleName,
    FString& OutError)
{
    bool bInvalidCharacter = false;
    for (const TCHAR Character : CallbackName)
    {
        if (!(FChar::IsAlnum(Character) || Character == TEXT('_')))
        {
            bInvalidCharacter = true;
            break;
        }
    }
    if (CallbackName.IsEmpty() ||
        !(FChar::IsAlpha(CallbackName[0]) || CallbackName[0] == TEXT('_')) ||
        bInvalidCharacter)
    {
        OutError = TEXT("Delegate callback name must be an AngelScript identifier");
        return nullptr;
    }
    asIScriptContext* ActiveContext = asGetActiveContext();
    asIScriptFunction* ActiveFunction =
        ActiveContext != nullptr ? ActiveContext->GetFunction() : nullptr;
    asIScriptModule* Module =
        ActiveFunction != nullptr ? ActiveFunction->GetModule() : nullptr;
    if (Module == nullptr)
    {
        OutError = TEXT("Delegate binding requires an active script module");
        return nullptr;
    }
    FForgeFunctionBinding Signature;
    Signature.ReturnType = Binding.ReturnType;
    Signature.ParameterTypes = Binding.ParameterTypes;
    const FString Declaration = FunctionDeclaration(CallbackName, Signature);
    FTCHARToUTF8 DeclarationUtf8(*Declaration);
    asIScriptFunction* Function = Module->GetFunctionByDecl(DeclarationUtf8.Get());
    if (Function == nullptr)
    {
        OutError = FString::Printf(
            TEXT("Delegate callback not found with exact signature: %s"),
            *Declaration);
        return nullptr;
    }
    OutModuleName = UTF8_TO_TCHAR(Function->GetModuleName());
    return MakeShared<FForgeScriptDelegateCallback, ESPMode::ThreadSafe>(
        State, *Function, Binding);
}

void ForgeDelegateBindGeneric(asIScriptGeneric* Generic)
{
    const FForgeDelegateBinding* Binding =
        static_cast<const FForgeDelegateBinding*>(Generic->GetAuxiliary());
    FString Error;
    if (Binding == nullptr || !ValidateLoadedDelegate(*Binding, Error))
    {
        SetScriptException(Error.IsEmpty() ? TEXT("Forge delegate metadata is unavailable") : Error);
        return;
    }
    FForgeObjectLeaseResult Lease;
    FForgeObjectHandle Owner;
    TWeakObjectPtr<UObject> UObjectOwner;
    void* Instance = nullptr;
    if (!AcquireDelegateOwner(
            *Generic, *Binding, Lease, Owner, UObjectOwner, Instance, Error))
    {
        SetScriptException(Error);
        return;
    }
    const FString& CallbackName =
        *static_cast<const FString*>(Generic->GetArgObject(0));
    FString ModuleName;
    FForgeDelegateRuntimeState& State = *GInvocationContext->DelegateState;
    FForgeDelegateCallback Callback = FindDelegateCallback(
        State, *Binding, CallbackName, ModuleName, Error);
    if (!Callback.IsValid())
    {
        SetScriptException(Error);
        return;
    }
    if (Binding->Kind == EForgeDelegateKind::Singlecast ||
        Binding->Kind == EForgeDelegateKind::DynamicSinglecast)
    {
        TArray<int32> ExistingTokens;
        for (const TPair<int32, FForgeDelegateSubscription>& Pair : State.Subscriptions)
        {
            if (Pair.Value.Binding.SymbolId == Binding->SymbolId &&
                DelegateOwnerMatches(Pair.Value, *Binding, Owner, UObjectOwner))
            {
                ExistingTokens.Add(Pair.Key);
            }
        }
        for (const int32 Token : ExistingTokens)
        {
            if (!RemoveDelegateSubscription(State, Token, &Error))
            {
                SetScriptException(Error);
                return;
            }
        }
    }
    FForgeDelegateHandle NativeHandle;
    const FForgeInvokeResult BindResult = Binding->Bind(
        Instance, Callback, NativeHandle);
    if (!BindResult.bSuccess)
    {
        SetScriptException(BindResult.Error);
        return;
    }
    int32 Token = State.NextToken++;
    if (Token <= 0) Token = State.NextToken = 1;
    State.Subscriptions.Add(Token, FForgeDelegateSubscription{
        Token, ModuleName, *Binding, Owner, UObjectOwner, MoveTemp(NativeHandle) });
    Generic->SetReturnDWord(static_cast<asDWORD>(Token));
}

void ForgeDelegateUnbindGeneric(asIScriptGeneric* Generic)
{
    const FForgeDelegateBinding* Binding =
        static_cast<const FForgeDelegateBinding*>(Generic->GetAuxiliary());
    FString Error;
    if (Binding == nullptr || !ValidateLoadedDelegate(*Binding, Error))
    {
        SetScriptException(Error.IsEmpty() ? TEXT("Forge delegate metadata is unavailable") : Error);
        return;
    }
    FForgeDelegateRuntimeState& State = *GInvocationContext->DelegateState;
    if (Binding->Kind != EForgeDelegateKind::Singlecast &&
        Binding->Kind != EForgeDelegateKind::DynamicSinglecast)
    {
        const int32 Token = static_cast<int32>(Generic->GetArgDWord(0));
        FForgeDelegateSubscription* Subscription = State.Subscriptions.Find(Token);
        if (Subscription == nullptr || Subscription->Binding.SymbolId != Binding->SymbolId ||
            !RemoveDelegateSubscription(State, Token, &Error))
        {
            SetScriptException(Error.IsEmpty()
                ? TEXT("Delegate subscription token does not belong to this property")
                : Error);
        }
        return;
    }
    FForgeObjectLeaseResult Lease;
    FForgeObjectHandle Owner;
    TWeakObjectPtr<UObject> UObjectOwner;
    void* Instance = nullptr;
    if (!AcquireDelegateOwner(
            *Generic, *Binding, Lease, Owner, UObjectOwner, Instance, Error))
    {
        SetScriptException(Error);
        return;
    }
    TArray<int32> Tokens;
    for (const TPair<int32, FForgeDelegateSubscription>& Pair : State.Subscriptions)
    {
        if (Pair.Value.Binding.SymbolId == Binding->SymbolId &&
            DelegateOwnerMatches(Pair.Value, *Binding, Owner, UObjectOwner))
        {
            Tokens.Add(Pair.Key);
        }
    }
    if (Tokens.IsEmpty())
    {
        const FForgeInvokeResult Result = Binding->Unbind(Instance, FForgeDelegateHandle{});
        if (!Result.bSuccess) SetScriptException(Result.Error);
        return;
    }
    for (const int32 Token : Tokens)
    {
        if (!RemoveDelegateSubscription(State, Token, &Error))
        {
            SetScriptException(Error);
            return;
        }
    }
}

void ForgeDelegateClearGeneric(asIScriptGeneric* Generic)
{
    const FForgeDelegateBinding* Binding =
        static_cast<const FForgeDelegateBinding*>(Generic->GetAuxiliary());
    FString Error;
    if (Binding == nullptr || Binding->Clear == nullptr ||
        !ValidateLoadedDelegate(*Binding, Error))
    {
        SetScriptException(Error.IsEmpty() ? TEXT("Forge multicast clear is unavailable") : Error);
        return;
    }
    FForgeObjectLeaseResult Lease;
    FForgeObjectHandle Owner;
    TWeakObjectPtr<UObject> UObjectOwner;
    void* Instance = nullptr;
    if (!AcquireDelegateOwner(
            *Generic, *Binding, Lease, Owner, UObjectOwner, Instance, Error))
    {
        SetScriptException(Error);
        return;
    }
    const FForgeInvokeResult Result = Binding->Clear(Instance);
    if (!Result.bSuccess)
    {
        SetScriptException(Result.Error);
        return;
    }
    FForgeDelegateRuntimeState& State = *GInvocationContext->DelegateState;
    for (auto Iterator = State.Subscriptions.CreateIterator(); Iterator; ++Iterator)
    {
        if (Iterator.Value().Binding.SymbolId == Binding->SymbolId &&
            DelegateOwnerMatches(Iterator.Value(), *Binding, Owner, UObjectOwner))
        {
            ReleaseDynamicDelegateProxy(Iterator.Value().NativeHandle);
            Iterator.RemoveCurrent();
        }
    }
}

void ForgeDelegateIsBoundGeneric(asIScriptGeneric* Generic)
{
    const FForgeDelegateBinding* Binding =
        static_cast<const FForgeDelegateBinding*>(Generic->GetAuxiliary());
    FString Error;
    if (Binding == nullptr || !ValidateLoadedDelegate(*Binding, Error))
    {
        SetScriptException(Error.IsEmpty() ? TEXT("Forge delegate metadata is unavailable") : Error);
        return;
    }
    FForgeObjectLeaseResult Lease;
    FForgeObjectHandle Owner;
    TWeakObjectPtr<UObject> UObjectOwner;
    void* Instance = nullptr;
    if (!AcquireDelegateOwner(
            *Generic, *Binding, Lease, Owner, UObjectOwner, Instance, Error))
    {
        SetScriptException(Error);
        return;
    }
    bool bBound = false;
    const FForgeInvokeResult Result = Binding->IsBound(Instance, bBound);
    if (!Result.bSuccess)
    {
        SetScriptException(Result.Error);
        return;
    }
    Generic->SetReturnByte(bBound ? 1 : 0);
}

void ForgeDelegateTriggerGeneric(asIScriptGeneric* Generic)
{
    const FForgeDelegateBinding* Binding =
        static_cast<const FForgeDelegateBinding*>(Generic->GetAuxiliary());
    FString Error;
    if (Binding == nullptr || !ValidateLoadedDelegate(*Binding, Error))
    {
        SetScriptException(Error.IsEmpty() ? TEXT("Forge delegate metadata is unavailable") : Error);
        return;
    }
    FForgeObjectLeaseResult Lease;
    FForgeObjectHandle Owner;
    TWeakObjectPtr<UObject> UObjectOwner;
    void* Instance = nullptr;
    if (!AcquireDelegateOwner(
            *Generic, *Binding, Lease, Owner, UObjectOwner, Instance, Error))
    {
        SetScriptException(Error);
        return;
    }
    TArray<FForgeValue, TInlineAllocator<8>> Arguments;
    for (int32 Index = 0; Index < Binding->ParameterTypes.Num(); ++Index)
    {
        Arguments.Add(ReadGenericArgument(
            *Generic, Index, Binding->ParameterTypes[Index],
            *GInvocationContext->Registry));
    }
    FForgeValue ReturnValue;
    const FForgeInvokeResult Result = Binding->Trigger(
        Instance, Arguments, ReturnValue);
    if (!Result.bSuccess)
    {
        SetScriptException(Result.Error);
        return;
    }
    for (int32 Index = 0; Index < Binding->ParameterTypes.Num(); ++Index)
    {
        if (!WriteGenericInOutArgument(
                *Generic, Index, Binding->ParameterTypes[Index], Arguments[Index],
                *GInvocationContext->Registry))
        {
            SetScriptException("Failed to copy a delegate inout argument back to AngelScript");
            return;
        }
    }
    WriteGenericReturn(*Generic, ReturnValue);
}

bool EnsureDelegatesRegistered(
    asIScriptEngine& Engine,
    IForgeBindingRegistry& Registry,
    TMap<FString, TUniquePtr<FForgeDelegateBinding>>& RegisteredBindings,
    FString& OutError)
{
    TArray<FForgeDelegateBinding> Bindings;
    Registry.GetDelegates(Bindings);
    for (const FForgeDelegateBinding& Binding : Bindings)
    {
        if (RegisteredBindings.Contains(Binding.SymbolId))
        {
            continue;
        }
        if (!EnsureReflectedTypeRegistered(Engine, Registry, Binding.ReturnType, OutError))
        {
            return false;
        }
        for (const FForgeTypeRef& Parameter : Binding.ParameterTypes)
        {
            if (!EnsureReflectedTypeRegistered(Engine, Registry, Parameter, OutError))
            {
                return false;
            }
        }
        TUniquePtr<FForgeDelegateBinding> Metadata =
            MakeUnique<FForgeDelegateBinding>(Binding);
        FForgeDelegateBinding* Address = Metadata.Get();
        const bool bSinglecast = Binding.Kind == EForgeDelegateKind::Singlecast ||
            Binding.Kind == EForgeDelegateKind::DynamicSinglecast;
        const FString BindDeclaration = FString::Printf(
            TEXT("int %s(const FString &in)%s"), *Binding.ScriptBindName,
            Binding.bStatic ? TEXT("") : TEXT(" const"));
        const FString UnbindDeclaration = bSinglecast
            ? FString::Printf(
                TEXT("void %s()%s"),
                *Binding.ScriptUnbindName,
                Binding.bStatic ? TEXT("") : TEXT(" const"))
            : FString::Printf(
                TEXT("void %s(int)%s"),
                *Binding.ScriptUnbindName,
                Binding.bStatic ? TEXT("") : TEXT(" const"));
        const FString IsBoundDeclaration = FString::Printf(
            TEXT("bool %s()%s"), *Binding.ScriptIsBoundName,
            Binding.bStatic ? TEXT("") : TEXT(" const"));
        FForgeFunctionBinding TriggerSignature;
        TriggerSignature.ReturnType = Binding.ReturnType;
        TriggerSignature.ParameterTypes = Binding.ParameterTypes;
        FString TriggerDeclaration = FunctionDeclaration(
            Binding.ScriptTriggerName, TriggerSignature);
        if (!Binding.bStatic) TriggerDeclaration += TEXT(" const");
        auto Register = [&](const FString& Declaration, asSFuncPtr Function) -> bool
        {
            FTCHARToUTF8 Utf8(*Declaration);
            const int Result = Binding.bStatic
                ? Engine.RegisterGlobalFunction(Utf8.Get(), Function, asCALL_GENERIC, Address)
                : Engine.RegisterObjectMethod(
                    Binding.bUObjectOwner ? "UObjectHandle" : "ForgeObjectHandle",
                    Utf8.Get(), Function, asCALL_GENERIC, Address);
            if (Result < 0)
            {
                OutError = FString::Printf(
                    TEXT("Failed to register delegate operation %s (%d)"),
                    *Declaration, Result);
                return false;
            }
            return true;
        };
        if (!Register(BindDeclaration, asFUNCTION(ForgeDelegateBindGeneric)) ||
            !Register(UnbindDeclaration, asFUNCTION(ForgeDelegateUnbindGeneric)) ||
            !Register(IsBoundDeclaration, asFUNCTION(ForgeDelegateIsBoundGeneric)) ||
            !Register(TriggerDeclaration, asFUNCTION(ForgeDelegateTriggerGeneric)))
        {
            return false;
        }
        if (!bSinglecast)
        {
            const FString ClearDeclaration = FString::Printf(
                TEXT("void %s()%s"), *Binding.ScriptClearName,
                Binding.bStatic ? TEXT("") : TEXT(" const"));
            if (!Register(ClearDeclaration, asFUNCTION(ForgeDelegateClearGeneric)))
            {
                return false;
            }
        }
        RegisteredBindings.Add(Binding.SymbolId, MoveTemp(Metadata));
    }
    return true;
}

void ForgeObjectHandleInvokeMethod(asIScriptGeneric* Generic)
{
    const FForgeFunctionBinding* Binding =
        static_cast<const FForgeFunctionBinding*>(Generic->GetAuxiliary());
    const FForgeObjectHandle* Handle =
        static_cast<const FForgeObjectHandle*>(Generic->GetObject());
    if (Binding == nullptr || Handle == nullptr || !Binding->bInstanceMember ||
        Binding->ObjectTypeId.IsEmpty() || Binding->ObjectType.IsEmpty())
    {
        SetScriptException("Forge instance method metadata is unavailable");
        return;
    }
    if (GInvocationContext == nullptr || GInvocationContext->Registry == nullptr ||
        GInvocationContext->ObjectRegistry == nullptr)
    {
        SetScriptException("Forge instance method called outside an active script invocation");
        return;
    }
    const FForgeFunctionBinding* LoadedBinding =
        GInvocationContext->Registry->FindFunction(Binding->SymbolId);
    if (LoadedBinding == nullptr ||
        LoadedBinding->BuildFingerprint != Binding->BuildFingerprint ||
        LoadedBinding->TargetAddress != Binding->TargetAddress)
    {
        SetScriptException("Forge instance method binding is stale or unloaded");
        return;
    }

    FForgeObjectLeaseResult LeaseResult;
    void* Instance = nullptr;
    FString InstanceError;
    if (!AcquireForgeInstance(
            *Handle,
            Binding->ObjectTypeId,
            !Binding->bConstMember,
            *GInvocationContext->Registry,
            *GInvocationContext->ObjectRegistry,
            LeaseResult,
            Instance,
            InstanceError))
    {
        SetScriptException(InstanceError);
        return;
    }

    TArray<FForgeValue, TInlineAllocator<8>> Arguments;
    Arguments.Reserve(Binding->ParameterTypes.Num());
    for (int32 Index = 0; Index < Binding->ParameterTypes.Num(); ++Index)
    {
        Arguments.Add(ReadGenericArgument(
            *Generic,
            Index,
            Binding->ParameterTypes[Index],
            *GInvocationContext->Registry));
    }
    FForgeValue ReturnValue;
    const FForgeInvokeResult Result = Binding->Invoker(
        Binding->TargetAddress,
        Instance,
        Arguments,
        ReturnValue);
    if (!Result.bSuccess)
    {
        SetScriptException(Result.Error);
        return;
    }
    for (int32 Index = 0; Index < Binding->ParameterTypes.Num(); ++Index)
    {
        if (!WriteGenericInOutArgument(
                *Generic,
                Index,
                Binding->ParameterTypes[Index],
                Arguments[Index],
                *GInvocationContext->Registry))
        {
            SetScriptException("Failed to copy an instance-method inout argument back to AngelScript");
            return;
        }
    }
    WriteGenericReturn(*Generic, ReturnValue);
}

bool EnsureInstanceMethodsRegistered(
    asIScriptEngine& Engine,
    IForgeBindingRegistry& Registry,
    TMap<FString, TUniquePtr<FForgeFunctionBinding>>& RegisteredBindings,
    FString& OutError)
{
    TArray<FForgeFunctionBinding> Bindings;
    Registry.GetFunctions(Bindings);
    for (const FForgeFunctionBinding& Binding : Bindings)
    {
        if (!Binding.bInstanceMember || Binding.ObjectTypeId.IsEmpty() ||
            Binding.ScriptMethodName.IsEmpty())
        {
            continue;
        }
        if (RegisteredBindings.Contains(Binding.SymbolId))
        {
            continue;
        }
        if (!EnsureBindingTypesRegistered(Engine, Registry, Binding, OutError))
        {
            return false;
        }

        TUniquePtr<FForgeFunctionBinding> Metadata =
            MakeUnique<FForgeFunctionBinding>(Binding);
        FForgeFunctionBinding* MetadataAddress = Metadata.Get();
        const FString Declaration =
            FunctionDeclaration(Binding.ScriptMethodName, Binding) + TEXT(" const");
        FTCHARToUTF8 DeclarationUtf8(*Declaration);
        const int RegisterResult = Engine.RegisterObjectMethod(
            "ForgeObjectHandle",
            DeclarationUtf8.Get(),
            asFUNCTION(ForgeObjectHandleInvokeMethod),
            asCALL_GENERIC,
            MetadataAddress);
        if (RegisterResult < 0)
        {
            OutError = FString::Printf(
                TEXT("Failed to register active instance method %s (%d)"),
                *Declaration,
                RegisterResult);
            return false;
        }
        RegisteredBindings.Add(Binding.SymbolId, MoveTemp(Metadata));
    }
    return true;
}

bool ValidateLoadedField(const FForgeFieldBinding& Binding, FString& OutError)
{
    if (GInvocationContext == nullptr || GInvocationContext->Registry == nullptr)
    {
        OutError = TEXT("Forge field accessor called outside an active script invocation");
        return false;
    }
    const FForgeFieldBinding* Loaded =
        GInvocationContext->Registry->FindField(Binding.SymbolId);
    if (Loaded == nullptr ||
        Loaded->BuildFingerprint != Binding.BuildFingerprint ||
        Loaded->Getter != Binding.Getter ||
        Loaded->Setter != Binding.Setter)
    {
        OutError = TEXT("Forge field binding is stale or unloaded");
        return false;
    }
    return true;
}

void ForgeFieldGet(asIScriptGeneric* Generic)
{
    const FForgeFieldBinding* Binding =
        static_cast<const FForgeFieldBinding*>(Generic->GetAuxiliary());
    FString Error;
    if (Binding == nullptr || Binding->Getter == nullptr ||
        !ValidateLoadedField(*Binding, Error))
    {
        SetScriptException(Error.IsEmpty() ? TEXT("Forge field getter metadata is unavailable") : Error);
        return;
    }

    FForgeObjectLeaseResult LeaseResult;
    void* Instance = nullptr;
    if (!Binding->bStatic)
    {
        const FForgeObjectHandle* Handle =
            static_cast<const FForgeObjectHandle*>(Generic->GetObject());
        if (Handle == nullptr || GInvocationContext->ObjectRegistry == nullptr)
        {
            SetScriptException("Forge instance field requires an object registry");
            return;
        }
        if (!AcquireForgeInstance(
                *Handle,
                Binding->ObjectTypeId,
                false,
                *GInvocationContext->Registry,
                *GInvocationContext->ObjectRegistry,
                LeaseResult,
                Instance,
                Error))
        {
            SetScriptException(Error);
            return;
        }
    }

    FForgeValue Value;
    const FForgeInvokeResult Result = Binding->Getter(Instance, Value);
    if (!Result.bSuccess)
    {
        SetScriptException(Result.Error);
        return;
    }
    WriteGenericReturn(*Generic, Value);
}

void ForgeFieldSet(asIScriptGeneric* Generic)
{
    const FForgeFieldBinding* Binding =
        static_cast<const FForgeFieldBinding*>(Generic->GetAuxiliary());
    FString Error;
    if (Binding == nullptr || Binding->Setter == nullptr || Binding->bReadOnly ||
        !ValidateLoadedField(*Binding, Error))
    {
        SetScriptException(Error.IsEmpty() ? TEXT("Forge field is read-only") : Error);
        return;
    }

    FForgeObjectLeaseResult LeaseResult;
    void* Instance = nullptr;
    if (!Binding->bStatic)
    {
        const FForgeObjectHandle* Handle =
            static_cast<const FForgeObjectHandle*>(Generic->GetObject());
        if (Handle == nullptr || GInvocationContext->ObjectRegistry == nullptr)
        {
            SetScriptException("Forge instance field requires an object registry");
            return;
        }
        if (!AcquireForgeInstance(
                *Handle,
                Binding->ObjectTypeId,
                true,
                *GInvocationContext->Registry,
                *GInvocationContext->ObjectRegistry,
                LeaseResult,
                Instance,
                Error))
        {
            SetScriptException(Error);
            return;
        }
    }

    const FForgeValue Value = ReadGenericArgument(
        *Generic, 0, Binding->Type, *GInvocationContext->Registry);
    const FForgeInvokeResult Result = Binding->Setter(Instance, Value);
    if (!Result.bSuccess)
    {
        SetScriptException(Result.Error);
    }
}

bool EnsureFieldsRegistered(
    asIScriptEngine& Engine,
    IForgeBindingRegistry& Registry,
    TMap<FString, TUniquePtr<FForgeFieldBinding>>& RegisteredBindings,
    FString& OutError)
{
    TArray<FForgeFieldBinding> Bindings;
    Registry.GetFields(Bindings);
    for (const FForgeFieldBinding& Binding : Bindings)
    {
        if (Binding.ScriptGetterName.IsEmpty() || RegisteredBindings.Contains(Binding.SymbolId))
        {
            continue;
        }
        if (!EnsureReflectedTypeRegistered(Engine, Registry, Binding.Type, OutError))
        {
            return false;
        }
        TUniquePtr<FForgeFieldBinding> Metadata = MakeUnique<FForgeFieldBinding>(Binding);
        FForgeFieldBinding* MetadataAddress = Metadata.Get();
        const FString GetterDeclaration = FString::Printf(
            TEXT("%s %s()%s"),
            ScriptType(Binding.Type),
            *Binding.ScriptGetterName,
            Binding.bStatic ? TEXT("") : TEXT(" const"));
        FTCHARToUTF8 GetterUtf8(*GetterDeclaration);
        const int GetterResult = Binding.bStatic
            ? Engine.RegisterGlobalFunction(
                GetterUtf8.Get(), asFUNCTION(ForgeFieldGet), asCALL_GENERIC, MetadataAddress)
            : Engine.RegisterObjectMethod(
                "ForgeObjectHandle", GetterUtf8.Get(),
                asFUNCTION(ForgeFieldGet), asCALL_GENERIC, MetadataAddress);
        if (GetterResult < 0)
        {
            OutError = FString::Printf(
                TEXT("Failed to register field getter %s (%d)"),
                *GetterDeclaration,
                GetterResult);
            return false;
        }
        if (!Binding.bReadOnly && Binding.Setter != nullptr)
        {
            const FString SetterDeclaration = FString::Printf(
                TEXT("void %s(%s)%s"),
                *Binding.ScriptSetterName,
                *ScriptParameter(Binding.Type),
                Binding.bStatic ? TEXT("") : TEXT(" const"));
            FTCHARToUTF8 SetterUtf8(*SetterDeclaration);
            const int SetterResult = Binding.bStatic
                ? Engine.RegisterGlobalFunction(
                    SetterUtf8.Get(), asFUNCTION(ForgeFieldSet), asCALL_GENERIC, MetadataAddress)
                : Engine.RegisterObjectMethod(
                    "ForgeObjectHandle", SetterUtf8.Get(),
                    asFUNCTION(ForgeFieldSet), asCALL_GENERIC, MetadataAddress);
            if (SetterResult < 0)
            {
                OutError = FString::Printf(
                    TEXT("Failed to register field setter %s (%d)"),
                    *SetterDeclaration,
                    SetterResult);
                return false;
            }
        }
        RegisteredBindings.Add(Binding.SymbolId, MoveTemp(Metadata));
    }
    return true;
}

enum class EForgeOwnedFactoryOperation : uint8
{
    DefaultConstruct,
    CopyConstruct,
    MoveConstruct
};

template <EForgeOwnedFactoryOperation Operation>
void ForgeOwnedFactory(asIScriptGeneric* Generic)
{
    const FForgeTypeBinding* Binding =
        static_cast<const FForgeTypeBinding*>(Generic->GetAuxiliary());
    if (Binding == nullptr || GInvocationContext == nullptr ||
        GInvocationContext->Registry == nullptr ||
        GInvocationContext->ObjectRegistry == nullptr)
    {
        SetScriptException("Forge owned factory called outside an active script invocation");
        return;
    }
    const FForgeTypeBinding* Loaded =
        GInvocationContext->Registry->FindTypeById(Binding->TypeId);
    if (Loaded == nullptr ||
        FCString::Strcmp(Loaded->BuildFingerprint, Binding->BuildFingerprint) != 0 ||
        Loaded->DefaultConstruct != Binding->DefaultConstruct ||
        Loaded->CopyConstruct != Binding->CopyConstruct ||
        Loaded->MoveConstruct != Binding->MoveConstruct ||
        Loaded->Destroy != Binding->Destroy)
    {
        SetScriptException("Forge owned type binding is stale or unloaded");
        return;
    }

    FForgeObjectLeaseResult SourceLease;
    void* Source = nullptr;
    if constexpr (Operation != EForgeOwnedFactoryOperation::DefaultConstruct)
    {
        const FForgeObjectHandle* SourceHandle =
            static_cast<const FForgeObjectHandle*>(Generic->GetArgObject(0));
        if (SourceHandle == nullptr)
        {
            SetScriptException("Forge owned construction requires a source handle");
            return;
        }
        SourceLease = GInvocationContext->ObjectRegistry->Acquire(
            *SourceHandle,
            FName(Binding->TypeId),
            FName(Binding->QualifiedCppName),
            Operation == EForgeOwnedFactoryOperation::MoveConstruct);
        if (!SourceLease.bSuccess)
        {
            SetScriptException(SourceLease.Error);
            return;
        }
        Source = SourceLease.Lease.GetAddress();
    }

    FForgeObjectConstruct Construct = Binding->DefaultConstruct;
    if constexpr (Operation == EForgeOwnedFactoryOperation::CopyConstruct)
    {
        Construct = Binding->CopyConstruct;
    }
    else if constexpr (Operation == EForgeOwnedFactoryOperation::MoveConstruct)
    {
        Construct = Binding->MoveConstruct;
    }
    void* Address = nullptr;
    const FForgeInvokeResult ConstructResult = Construct(Source, Address);
    if (!ConstructResult.bSuccess || Address == nullptr)
    {
        SetScriptException(ConstructResult.bSuccess
            ? TEXT("Forge owned constructor returned no object")
            : ConstructResult.Error);
        return;
    }
    SourceLease.Lease = FForgeObjectLease{};
    FForgeObjectRegistrationResult Registration =
        GInvocationContext->ObjectRegistry->RegisterOwned(
            Address,
            FName(Binding->TypeId),
            FName(Binding->QualifiedCppName),
            Binding->Destroy);
    if (!Registration.bSuccess)
    {
        Binding->Destroy(Address);
        SetScriptException(Registration.Error);
        return;
    }
    new (Generic->GetAddressOfReturnLocation()) FForgeObjectHandle(
        MoveTemp(Registration.Handle));
}

void ForgeConcreteOwnedFactory(asIScriptGeneric* Generic)
{
    const FForgeOwnedConstructorMetadata* Metadata =
        static_cast<const FForgeOwnedConstructorMetadata*>(Generic->GetAuxiliary());
    if (Metadata == nullptr || GInvocationContext == nullptr ||
        GInvocationContext->Registry == nullptr ||
        GInvocationContext->ObjectRegistry == nullptr)
    {
        SetScriptException("Forge concrete constructor called outside an active script invocation");
        return;
    }

    const FForgeTypeBinding* Loaded =
        GInvocationContext->Registry->FindTypeById(Metadata->Type.TypeId);
    const FForgeOwnedConstructorBinding* LoadedConstructor = nullptr;
    if (Loaded != nullptr)
    {
        for (const FForgeOwnedConstructorBinding& Constructor : Loaded->Constructors)
        {
            if (FCString::Strcmp(Constructor.SymbolId, Metadata->Constructor.SymbolId) == 0)
            {
                LoadedConstructor = &Constructor;
                break;
            }
        }
    }
    if (Loaded == nullptr || LoadedConstructor == nullptr ||
        FCString::Strcmp(Loaded->BuildFingerprint, Metadata->Type.BuildFingerprint) != 0 ||
        Loaded->Destroy == nullptr ||
        Loaded->Destroy != Metadata->Type.Destroy ||
        LoadedConstructor->Construct == nullptr ||
        LoadedConstructor->Construct != Metadata->Constructor.Construct ||
        LoadedConstructor->ParameterTypes.Num() != Generic->GetArgCount())
    {
        SetScriptException("Forge concrete constructor binding is stale or unloaded");
        return;
    }

    TArray<FForgeValue, TInlineAllocator<8>> Arguments;
    Arguments.Reserve(LoadedConstructor->ParameterTypes.Num());
    for (int32 Index = 0; Index < LoadedConstructor->ParameterTypes.Num(); ++Index)
    {
        Arguments.Add(ReadGenericArgument(
            *Generic,
            Index,
            LoadedConstructor->ParameterTypes[Index],
            *GInvocationContext->Registry));
    }
    void* Address = nullptr;
    const FForgeInvokeResult ConstructResult =
        LoadedConstructor->Construct(Arguments, Address);
    if (!ConstructResult.bSuccess || Address == nullptr)
    {
        SetScriptException(ConstructResult.bSuccess
            ? TEXT("Forge concrete constructor returned no object")
            : ConstructResult.Error);
        return;
    }
    FForgeObjectRegistrationResult Registration =
        GInvocationContext->ObjectRegistry->RegisterOwned(
            Address,
            FName(Loaded->TypeId),
            FName(Loaded->QualifiedCppName),
            Loaded->Destroy);
    if (!Registration.bSuccess)
    {
        Loaded->Destroy(Address);
        SetScriptException(Registration.Error);
        return;
    }
    new (Generic->GetAddressOfReturnLocation()) FForgeObjectHandle(
        MoveTemp(Registration.Handle));
}

bool EnsureOwnedFactoriesRegistered(
    asIScriptEngine& Engine,
    IForgeBindingRegistry& Registry,
    TMap<FString, TUniquePtr<FForgeTypeBinding>>& RegisteredBindings,
    TMap<FString, TUniquePtr<FForgeOwnedConstructorMetadata>>& RegisteredConstructors,
    FString& OutError)
{
    TArray<FForgeTypeBinding> Types;
    Registry.GetTypes(Types);
    for (const FForgeTypeBinding& Binding : Types)
    {
        if (Binding.Kind != EForgeValueType::NativeObject ||
            !EnumHasAllFlags(
                Binding.InstanceCapabilities,
                EForgeInstanceCapability::OwnedLifecycle))
        {
            continue;
        }
        if (!RegisteredBindings.Contains(Binding.TypeId))
        {
            TUniquePtr<FForgeTypeBinding> Metadata = MakeUnique<FForgeTypeBinding>(Binding);
            FForgeTypeBinding* MetadataAddress = Metadata.Get();
            const auto RegisterFactory = [&](const TCHAR* Name, const asSFuncPtr& Function, bool bHasSource)
            {
                const FString Declaration = bHasSource
                    ? FString::Printf(TEXT("ForgeObjectHandle %s(const ForgeObjectHandle &in)"), Name)
                    : FString::Printf(TEXT("ForgeObjectHandle %s()"), Name);
                FTCHARToUTF8 DeclarationUtf8(*Declaration);
                const int Result = Engine.RegisterGlobalFunction(
                    DeclarationUtf8.Get(), Function, asCALL_GENERIC, MetadataAddress);
                if (Result < 0)
                {
                    OutError = FString::Printf(
                        TEXT("Failed to register owned factory %s (%d)"),
                        *Declaration,
                        Result);
                    return false;
                }
                return true;
            };
            if (!RegisterFactory(
                    Binding.ScriptNewName,
                    asFUNCTION((ForgeOwnedFactory<EForgeOwnedFactoryOperation::DefaultConstruct>)),
                    false) ||
                !RegisterFactory(
                    Binding.ScriptCopyName,
                    asFUNCTION((ForgeOwnedFactory<EForgeOwnedFactoryOperation::CopyConstruct>)),
                    true) ||
                !RegisterFactory(
                    Binding.ScriptMoveName,
                    asFUNCTION((ForgeOwnedFactory<EForgeOwnedFactoryOperation::MoveConstruct>)),
                    true))
            {
                return false;
            }
            RegisteredBindings.Add(Binding.TypeId, MoveTemp(Metadata));
        }

        for (const FForgeOwnedConstructorBinding& Constructor : Binding.Constructors)
        {
            if (RegisteredConstructors.Contains(Constructor.SymbolId))
            {
                continue;
            }
            for (const FForgeTypeRef& ParameterType : Constructor.ParameterTypes)
            {
                if (!EnsureReflectedTypeRegistered(Engine, Registry, ParameterType, OutError))
                {
                    return false;
                }
            }
            TUniquePtr<FForgeOwnedConstructorMetadata> Metadata =
                MakeUnique<FForgeOwnedConstructorMetadata>();
            Metadata->Type = Binding;
            Metadata->Constructor = Constructor;
            TArray<FString> Parameters;
            Parameters.Reserve(Constructor.ParameterTypes.Num());
            for (const FForgeTypeRef& ParameterType : Constructor.ParameterTypes)
            {
                Parameters.Add(ScriptParameter(ParameterType));
            }
            const FString Declaration = FString::Printf(
                TEXT("ForgeObjectHandle %s(%s)"),
                Constructor.ScriptName,
                *FString::Join(Parameters, TEXT(", ")));
            FTCHARToUTF8 DeclarationUtf8(*Declaration);
            const int Result = Engine.RegisterGlobalFunction(
                DeclarationUtf8.Get(),
                asFUNCTION(ForgeConcreteOwnedFactory),
                asCALL_GENERIC,
                Metadata.Get());
            if (Result < 0)
            {
                OutError = FString::Printf(
                    TEXT("Failed to register concrete constructor %s (%d)"),
                    *Declaration,
                    Result);
                return false;
            }
            RegisteredConstructors.Add(Constructor.SymbolId, MoveTemp(Metadata));
        }
    }
    return true;
}

void AngelScriptMessageCallback(const asSMessageInfo* Message, void* UserData)
{
    FString& Messages = *static_cast<FString*>(UserData);
    Messages += FString::Printf(
        TEXT("%hs (%d, %d): %hs\n"),
        Message->section,
        Message->row,
        Message->col,
        Message->message);
}

void OriginalGeneric(asIScriptGeneric* Generic)
{
    if (GInvocationContext == nullptr || GInvocationContext->Binding == nullptr)
    {
        if (asIScriptContext* Context = asGetActiveContext())
        {
            Context->SetException("Original() called outside a Forge invocation");
        }
        return;
    }
    if (GInvocationContext->Mode != EForgePatchMode::Wrap)
    {
        if (asIScriptContext* Context = asGetActiveContext())
        {
            Context->SetException("Original() is only available to Wrap Patches");
        }
        return;
    }
    TArray<FForgeValue, TInlineAllocator<8>> Arguments;
    Arguments.Reserve(GInvocationContext->Binding->ParameterTypes.Num());
    for (int32 Index = 0; Index < GInvocationContext->Binding->ParameterTypes.Num(); ++Index)
    {
        Arguments.Add(ReadGenericArgument(
            *Generic,
            Index,
            GInvocationContext->Binding->ParameterTypes[Index],
            *GInvocationContext->Registry));
    }
    FForgeValue ReturnValue;
    const FForgeInvokeResult Result = GInvocationContext->Binding->Invoker(
        GInvocationContext->OriginalAddress,
        GInvocationContext->Instance,
        Arguments,
        ReturnValue);
    if (!Result.bSuccess)
    {
        if (asIScriptContext* Context = asGetActiveContext())
        {
            FTCHARToUTF8 ErrorUtf8(*Result.Error);
            Context->SetException(ErrorUtf8.Get());
        }
        return;
    }
    for (int32 Index = 0; Index < GInvocationContext->Binding->ParameterTypes.Num(); ++Index)
    {
        if (!WriteGenericInOutArgument(
                *Generic,
                Index,
                GInvocationContext->Binding->ParameterTypes[Index],
                Arguments[Index],
                *GInvocationContext->Registry))
        {
            SetScriptException("Failed to copy an Original() inout argument back to AngelScript");
            return;
        }
    }
    WriteGenericReturn(*Generic, ReturnValue);
}
}

FForgeScriptEngine::~FForgeScriptEngine() = default;

FForgePatchResult FForgeScriptEngine::Initialize(
    IForgeBindingRegistry& InRegistry,
    IForgeObjectRegistry& InObjectRegistry)
{
    Registry = &InRegistry;
    ObjectRegistry = &InObjectRegistry;
    Engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    if (Engine == nullptr)
    {
        return FForgePatchResult::Failure(TEXT("Failed to create AngelScript engine"));
    }
    DelegateState = MakeUnique<FForgeDelegateRuntimeState>();
    DelegateState->Engine = Engine;
    DelegateState->Registry = Registry;
    DelegateState->ObjectRegistry = ObjectRegistry;
    Engine->SetMessageCallback(
        asFUNCTION(AngelScriptMessageCallback),
        &CompileMessages,
        asCALL_CDECL);
    Engine->SetEngineProperty(asEP_ALLOW_UNSAFE_REFERENCES, 1);
    Engine->SetEngineProperty(asEP_REQUIRE_ENUM_SCOPE, 1);
    Engine->SetTypeInfoUserDataCleanupCallback(CleanupContainerTypeMetadata);
    if (!RegisterForgeValueTypes(*Engine))
    {
        Shutdown();
        return FForgePatchResult::Failure(TEXT("Failed to register Forge UE value types"));
    }
    StringFactory = new FForgeStringFactory();
    if (Engine->RegisterStringFactory("FString", StringFactory) < 0)
    {
        Shutdown();
        return FForgePatchResult::Failure(TEXT("Failed to register FString literals"));
    }
    return FForgePatchResult::Success();
}

void FForgeScriptEngine::Shutdown()
{
    if (DelegateState.IsValid())
    {
        TArray<int32> Tokens;
        DelegateState->Subscriptions.GetKeys(Tokens);
        for (const int32 Token : Tokens)
        {
            RemoveDelegateSubscription(*DelegateState, Token);
        }
    }
    if (Engine != nullptr)
    {
        Engine->ShutDownAndRelease();
        Engine = nullptr;
    }
    delete StringFactory;
    StringFactory = nullptr;
    ScriptMethodBindings.Reset();
    ScriptFieldBindings.Reset();
    ScriptOwnedTypeBindings.Reset();
    ScriptOwnedConstructorBindings.Reset();
    ScriptDelegateBindings.Reset();
    DelegateState.Reset();
    Registry = nullptr;
    ObjectRegistry = nullptr;
}

FForgePatchResult FForgeScriptEngine::Compile(
    const FString& ScriptSource,
    const FString& EntryFunction,
    EForgePatchMode Mode,
    const FForgeFunctionBinding& Binding,
    FForgeCompiledPatch& OutPatch)
{
    if (Engine == nullptr)
    {
        return FForgePatchResult::Failure(TEXT("AngelScript engine is not initialized"));
    }
    FString TypeRegistrationError;
    if (Registry == nullptr ||
        !EnsureBindingTypesRegistered(*Engine, *Registry, Binding, TypeRegistrationError) ||
        !EnsureInstanceMethodsRegistered(
            *Engine,
            *Registry,
            ScriptMethodBindings,
            TypeRegistrationError) ||
        !EnsureFieldsRegistered(
            *Engine,
            *Registry,
            ScriptFieldBindings,
            TypeRegistrationError) ||
        !EnsureDelegatesRegistered(
            *Engine,
            *Registry,
            ScriptDelegateBindings,
            TypeRegistrationError) ||
        !EnsureOwnedFactoriesRegistered(
            *Engine,
            *Registry,
            ScriptOwnedTypeBindings,
            ScriptOwnedConstructorBindings,
            TypeRegistrationError))
    {
        return FForgePatchResult::Failure(
            TypeRegistrationError.IsEmpty()
                ? TEXT("Forge type registry is unavailable")
                : MoveTemp(TypeRegistrationError));
    }
    CompileMessages.Reset();
    const uint64 ModuleId = NextModuleId++;
    const FString ModuleName = FString::Printf(TEXT("ForgePatch_%llu"), ModuleId);
    FString EffectiveSource = ScriptSource;
    if (Mode == EForgePatchMode::Wrap)
    {
        const FString OriginalName = FString::Printf(TEXT("__ForgeOriginal_%llu"), ModuleId);
        const FString OriginalDeclaration = FunctionDeclaration(OriginalName, Binding);
        FTCHARToUTF8 OriginalDeclarationUtf8(*OriginalDeclaration);
        Engine->SetDefaultAccessMask(WrapAccessMask);
        const int RegisterResult = Engine->RegisterGlobalFunction(
            OriginalDeclarationUtf8.Get(),
            asFUNCTION(OriginalGeneric),
            asCALL_GENERIC);
        Engine->SetDefaultAccessMask(0xFFFFFFFFu);
        if (RegisterResult < 0)
        {
            return FForgePatchResult::Failure(FString::Printf(
                TEXT("Failed to register typed AngelScript Original() %s (%d)"),
                *OriginalDeclaration,
                RegisterResult));
        }
        EffectiveSource.ReplaceInline(TEXT("Original("), *(OriginalName + TEXT("(")));
        EffectiveSource.ReplaceInline(TEXT("Original ("), *(OriginalName + TEXT(" (")));
    }
    FTCHARToUTF8 ModuleNameUtf8(*ModuleName);
    asIScriptModule* Module = Engine->GetModule(ModuleNameUtf8.Get(), asGM_ALWAYS_CREATE);
    Module->SetAccessMask(Mode == EForgePatchMode::Wrap ? WrapAccessMask : ReplaceAccessMask);
    FTCHARToUTF8 ScriptUtf8(*EffectiveSource);
    Module->AddScriptSection("patch.as", ScriptUtf8.Get(), ScriptUtf8.Length());
    if (Module->Build() < 0)
    {
        Engine->DiscardModule(ModuleNameUtf8.Get());
        return FForgePatchResult::Failure(
            CompileMessages.IsEmpty() ? TEXT("AngelScript compilation failed") : CompileMessages);
    }
    const FString Declaration = FunctionDeclaration(EntryFunction, Binding);
    FTCHARToUTF8 DeclarationUtf8(*Declaration);
    asIScriptFunction* Function = Module->GetFunctionByDecl(DeclarationUtf8.Get());
    if (Function == nullptr)
    {
        Engine->DiscardModule(ModuleNameUtf8.Get());
        return FForgePatchResult::Failure(
            FString::Printf(TEXT("Patch entry function not found: %s"), *Declaration));
    }
    OutPatch = { ModuleName, Function, Mode };
    return FForgePatchResult::Success();
}

void FForgeScriptEngine::Release(FForgeCompiledPatch& Patch)
{
    if (Engine != nullptr && !Patch.ModuleName.IsEmpty())
    {
        if (DelegateState.IsValid())
        {
            RemoveDelegateSubscriptionsForModule(*DelegateState, Patch.ModuleName);
        }
        FTCHARToUTF8 ModuleNameUtf8(*Patch.ModuleName);
        Engine->DiscardModule(ModuleNameUtf8.Get());
    }
    Patch = {};
}

FForgeInvokeResult FForgeScriptEngine::ExecuteStandalone(
    const FString& ScriptSource,
    const FString& EntryFunction,
    const FForgeTypeRef& ReturnType,
    TConstArrayView<FForgeTypeRef> ParameterTypes,
    TConstArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    FForgeFunctionBinding Signature;
    Signature.QualifiedName = TEXT("ForgeStandaloneScript");
    Signature.ReturnType = ReturnType;
    Signature.ParameterTypes = ParameterTypes;

    FForgeCompiledPatch Script;
    const FForgePatchResult CompileResult = Compile(
        ScriptSource,
        EntryFunction,
        EForgePatchMode::Replace,
        Signature,
        Script);
    if (!CompileResult.bSuccess)
    {
        return FForgeInvokeResult::Failure(CompileResult.Error);
    }
    TArray<FForgeValue> MutableArguments(Arguments);
    const FForgeInvokeResult ExecuteResult = Execute(
        Script,
        Signature,
        nullptr,
        nullptr,
        MutableArguments,
        OutReturn);
    Release(Script);
    return ExecuteResult;
}

FForgeInvokeResult FForgeScriptEngine::Execute(
    const FForgeCompiledPatch& Patch,
    const FForgeFunctionBinding& Binding,
    void* OriginalAddress,
    void* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    if (Engine == nullptr || Patch.Function == nullptr || Arguments.Num() != Binding.ParameterTypes.Num())
    {
        return FForgeInvokeResult::Failure(TEXT("Invalid MVP Patch invocation"));
    }
    asIScriptContext* Context = Engine->CreateContext();
    if (Context == nullptr)
    {
        return FForgeInvokeResult::Failure(TEXT("Failed to create AngelScript context"));
    }
    if (Context->Prepare(Patch.Function) < 0)
    {
        Context->Release();
        return FForgeInvokeResult::Failure(TEXT("Failed to prepare AngelScript Patch"));
    }
    TArray<FForgeValue, TInlineAllocator<8>> WorkingArguments;
    WorkingArguments.Reserve(Arguments.Num());
    for (int32 Index = 0; Index < Arguments.Num(); ++Index)
    {
        FForgeValue WorkingValue;
        if (!CloneInvocationValue(
                Arguments[Index],
                Binding.ParameterTypes[Index],
                *Registry,
                WorkingValue))
        {
            Context->Release();
            return FForgeInvokeResult::Failure(TEXT("Forge Patch argument snapshot failed"));
        }
        WorkingArguments.Add(MoveTemp(WorkingValue));
    }
    for (int32 Index = 0; Index < Arguments.Num(); ++Index)
    {
        if ((WorkingArguments[Index].GetType() == EForgeValueType::Enum ||
             WorkingArguments[Index].GetType() == EForgeValueType::Struct ||
             WorkingArguments[Index].GetType() == EForgeValueType::Array ||
             WorkingArguments[Index].GetType() == EForgeValueType::Map ||
             WorkingArguments[Index].GetType() == EForgeValueType::Set ||
             WorkingArguments[Index].GetType() == EForgeValueType::Optional ||
             WorkingArguments[Index].GetType() == EForgeValueType::Variant ||
             WorkingArguments[Index].GetType() == EForgeValueType::Pair) &&
            WorkingArguments[Index].GetReflectedTypeName() != Binding.ParameterTypes[Index].Name ||
            !SetContextArgument(
                *Context,
                Index,
                WorkingArguments[Index],
                Binding.ParameterTypes[Index]))
        {
            Context->Release();
            return FForgeInvokeResult::Failure(TEXT("Forge Patch argument type mismatch"));
        }
    }
    FForgeInvocationContext Invocation{
        &Binding,
        OriginalAddress,
        Instance,
        Registry,
        ObjectRegistry,
        DelegateState.Get(),
        Patch.Mode };
    GInvocationContext = &Invocation;
    const int ExecutionResult = Context->Execute();
    GInvocationContext = nullptr;
    if (ExecutionResult != asEXECUTION_FINISHED)
    {
        const FString Error = Context->GetExceptionString()
            ? UTF8_TO_TCHAR(Context->GetExceptionString())
            : TEXT("AngelScript execution failed");
        Context->Release();
        return FForgeInvokeResult::Failure(Error);
    }
    OutReturn = ReadContextReturn(*Context, Binding.ReturnType, *Registry);
    Context->Release();
    for (int32 Index = 0; Index < Arguments.Num(); ++Index)
    {
        if (Binding.ParameterTypes[Index].PassingMode ==
            FForgeTypeRef::EPassingMode::InOutReference)
        {
            Arguments[Index] = MoveTemp(WorkingArguments[Index]);
        }
    }
    return FForgeInvokeResult::Success();
}
