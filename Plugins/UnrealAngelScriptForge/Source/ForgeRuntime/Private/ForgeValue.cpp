#include "ForgeValue.h"

#include "UObject/Class.h"

namespace
{
void CheckType(EForgeValueType Actual, EForgeValueType Expected)
{
    checkf(Actual == Expected, TEXT("Forge value type mismatch"));
}
}

FForgeStructValueAllocation::FForgeStructValueAllocation(
    void* InAddress,
    FForgeValueDestroyThunk InDestroy)
    : Address(InAddress)
    , Destroy(InDestroy)
{
    check(Address != nullptr && Destroy != nullptr);
}

FForgeStructValueAllocation::FForgeStructValueAllocation(
    void* InAddress,
    UScriptStruct* InScriptStruct)
    : Address(InAddress)
    , ScriptStruct(InScriptStruct)
{
    check(Address != nullptr && ScriptStruct.IsValid());
}

FForgeStructValueAllocation::~FForgeStructValueAllocation()
{
    if (Address != nullptr)
    {
        if (ScriptStruct.IsValid())
        {
            ScriptStruct->DestroyStruct(Address);
        }
        else
        {
            check(Destroy != nullptr);
            Destroy(Address);
        }
        FMemory::Free(Address);
        Address = nullptr;
    }
}

FForgeValue FForgeValue::Void()
{
    return FForgeValue{};
}

FForgeValue FForgeValue::FromBool(bool Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::Bool;
    Result.Data.BoolValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromInt32(int32 Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::Int32;
    Result.Data.Int32Value = Value;
    return Result;
}

FForgeValue FForgeValue::FromFloat(float Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::Float;
    Result.Data.FloatValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromDouble(double Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::Double;
    Result.Data.DoubleValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromString(const FString& Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::String;
    Result.StringValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromName(FName Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::Name;
    Result.NameValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromVector(const FVector& Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::Vector;
    Result.VectorValue = Value;
    return Result;
}

#define FORGE_DEFINE_VALUE_FACTORY(Method, EnumValue, Member, NativeType) \
FForgeValue FForgeValue::Method(const NativeType& Value) \
{ \
    FForgeValue Result; \
    Result.Type = EForgeValueType::EnumValue; \
    Result.Member = Value; \
    return Result; \
}

FORGE_DEFINE_VALUE_FACTORY(FromText, Text, TextValue, FText)
FORGE_DEFINE_VALUE_FACTORY(FromRotator, Rotator, RotatorValue, FRotator)
FORGE_DEFINE_VALUE_FACTORY(FromQuat, Quat, QuatValue, FQuat)
FORGE_DEFINE_VALUE_FACTORY(FromTransform, Transform, TransformValue, FTransform)
FORGE_DEFINE_VALUE_FACTORY(FromVector2D, Vector2D, Vector2DValue, FVector2D)
FORGE_DEFINE_VALUE_FACTORY(FromGuid, Guid, GuidValue, FGuid)
FORGE_DEFINE_VALUE_FACTORY(FromColor, Color, ColorValue, FColor)
FORGE_DEFINE_VALUE_FACTORY(FromLinearColor, LinearColor, LinearColorValue, FLinearColor)

#undef FORGE_DEFINE_VALUE_FACTORY

FForgeValue FForgeValue::FromObject(UObject* Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::Object;
    Result.ObjectValue = TStrongObjectPtr<UObject>(Value);
    return Result;
}

FForgeValue FForgeValue::FromWeakObject(UObject* Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::WeakObject;
    Result.WeakObjectValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromSoftObjectPath(const FSoftObjectPath& Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::SoftObject;
    Result.SoftObjectPathValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromSoftClassPath(const FSoftObjectPath& Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::SoftClass;
    Result.SoftClassPathValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromNativeObject(const FForgeObjectHandle& Value)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::NativeObject;
    Result.NativeObjectValue = Value;
    return Result;
}

FForgeValue FForgeValue::FromEnum(int32 Value, const TCHAR* TypeName)
{
    FForgeValue Result;
    Result.Type = EForgeValueType::Enum;
    Result.Data.Int32Value = Value;
    Result.ReflectedTypeName = TypeName;
    return Result;
}

FForgeValue FForgeValue::FromStructBytes(
    const TCHAR* TypeName,
    const void* Value,
    int32 Size)
{
    check(Value != nullptr && Size > 0);
    FForgeValue Result;
    Result.Type = EForgeValueType::Struct;
    Result.ReflectedTypeName = TypeName;
    Result.StructBytes.SetNumUninitialized(Size);
    FMemory::Memcpy(Result.StructBytes.GetData(), Value, Size);
    return Result;
}

FForgeValue FForgeValue::FromManagedStructCopy(
    const TCHAR* TypeName,
    const void* Value,
    int32 Size,
    int32 Alignment,
    FForgeValueCopyConstructThunk CopyConstruct,
    FForgeValueDestroyThunk Destroy)
{
    check(Value != nullptr && Size > 0 && Alignment > 0);
    check(CopyConstruct != nullptr && Destroy != nullptr);
    void* Address = FMemory::Malloc(Size, static_cast<uint32>(Alignment));
    CopyConstruct(Address, Value);
    FForgeValue Result;
    Result.Type = EForgeValueType::Struct;
    Result.ReflectedTypeName = TypeName;
    Result.StructAllocation = MakeShared<FForgeStructValueAllocation, ESPMode::ThreadSafe>(
        Address,
        Destroy);
    return Result;
}

FForgeValue FForgeValue::FromScriptStruct(
    const TCHAR* TypeName,
    UScriptStruct* ScriptStruct,
    const void* Value)
{
    check(TypeName != nullptr && ScriptStruct != nullptr && Value != nullptr);
    void* Address = FMemory::Malloc(
        ScriptStruct->GetStructureSize(),
        static_cast<uint32>(ScriptStruct->GetMinAlignment()));
    ScriptStruct->InitializeStruct(Address);
    ScriptStruct->CopyScriptStruct(Address, Value);
    FForgeValue Result;
    Result.Type = EForgeValueType::Struct;
    Result.ReflectedTypeName = TypeName;
    Result.StructAllocation = MakeShared<FForgeStructValueAllocation, ESPMode::ThreadSafe>(
        Address,
        ScriptStruct);
    return Result;
}

const void* FForgeValue::GetStructData() const
{
    CheckType(Type, EForgeValueType::Struct);
    return StructAllocation.IsValid()
        ? StructAllocation->GetAddress()
        : StructBytes.GetData();
}

void* FForgeValue::GetMutableData()
{
    switch (Type)
    {
    case EForgeValueType::Bool: return &Data.BoolValue;
    case EForgeValueType::Int32:
    case EForgeValueType::Enum: return &Data.Int32Value;
    case EForgeValueType::Float: return &Data.FloatValue;
    case EForgeValueType::Double: return &Data.DoubleValue;
    case EForgeValueType::String: return &StringValue;
    case EForgeValueType::Name: return &NameValue;
    case EForgeValueType::Vector: return &VectorValue;
    case EForgeValueType::Text: return &TextValue;
    case EForgeValueType::Rotator: return &RotatorValue;
    case EForgeValueType::Quat: return &QuatValue;
    case EForgeValueType::Transform: return &TransformValue;
    case EForgeValueType::Vector2D: return &Vector2DValue;
    case EForgeValueType::Guid: return &GuidValue;
    case EForgeValueType::Color: return &ColorValue;
    case EForgeValueType::LinearColor: return &LinearColorValue;
    case EForgeValueType::Object: return &ObjectValue;
    case EForgeValueType::WeakObject: return &WeakObjectValue;
    case EForgeValueType::SoftObject: return &SoftObjectPathValue;
    case EForgeValueType::SoftClass: return &SoftClassPathValue;
    case EForgeValueType::NativeObject: return &NativeObjectValue;
    case EForgeValueType::Struct:
        return StructAllocation.IsValid()
            ? StructAllocation->GetAddress()
            : StructBytes.GetData();
    case EForgeValueType::Array:
    case EForgeValueType::Map:
    case EForgeValueType::Set:
    case EForgeValueType::Optional:
    case EForgeValueType::Variant:
    case EForgeValueType::Pair: return ContainerValue.Get();
    default: return nullptr;
    }
}

FForgeValue FForgeValue::FromContainer(
    EForgeValueType Kind,
    const TCHAR* TypeName,
    TArray<FForgeValue> Values,
    int32 ActiveIndex)
{
    checkf(
        Kind == EForgeValueType::Array ||
            Kind == EForgeValueType::Map ||
            Kind == EForgeValueType::Set ||
            Kind == EForgeValueType::Optional ||
            Kind == EForgeValueType::Variant ||
            Kind == EForgeValueType::Pair,
        TEXT("Forge container kind is invalid"));
    FForgeValue Result;
    Result.Type = Kind;
    Result.ReflectedTypeName = TypeName;
    Result.ContainerValue = MakeShared<FForgeContainer>();
    Result.ContainerValue->Kind = Kind;
    Result.ContainerValue->TypeName = TypeName;
    Result.ContainerValue->Values = MoveTemp(Values);
    Result.ContainerValue->ActiveIndex = ActiveIndex;
    return Result;
}

bool FForgeValue::AsBool() const
{
    CheckType(Type, EForgeValueType::Bool);
    return Data.BoolValue;
}

int32 FForgeValue::AsInt32() const
{
    CheckType(Type, EForgeValueType::Int32);
    return Data.Int32Value;
}

float FForgeValue::AsFloat() const
{
    CheckType(Type, EForgeValueType::Float);
    return Data.FloatValue;
}

double FForgeValue::AsDouble() const
{
    CheckType(Type, EForgeValueType::Double);
    return Data.DoubleValue;
}

const FString& FForgeValue::AsString() const
{
    CheckType(Type, EForgeValueType::String);
    return StringValue;
}

const FName& FForgeValue::AsName() const
{
    CheckType(Type, EForgeValueType::Name);
    return NameValue;
}

const FVector& FForgeValue::AsVector() const
{
    CheckType(Type, EForgeValueType::Vector);
    return VectorValue;
}

#define FORGE_DEFINE_VALUE_ACCESSOR(Method, EnumValue, Member, NativeType) \
const NativeType& FForgeValue::Method() const \
{ \
    CheckType(Type, EForgeValueType::EnumValue); \
    return Member; \
}

FORGE_DEFINE_VALUE_ACCESSOR(AsText, Text, TextValue, FText)
FORGE_DEFINE_VALUE_ACCESSOR(AsRotator, Rotator, RotatorValue, FRotator)
FORGE_DEFINE_VALUE_ACCESSOR(AsQuat, Quat, QuatValue, FQuat)
FORGE_DEFINE_VALUE_ACCESSOR(AsTransform, Transform, TransformValue, FTransform)
FORGE_DEFINE_VALUE_ACCESSOR(AsVector2D, Vector2D, Vector2DValue, FVector2D)
FORGE_DEFINE_VALUE_ACCESSOR(AsGuid, Guid, GuidValue, FGuid)
FORGE_DEFINE_VALUE_ACCESSOR(AsColor, Color, ColorValue, FColor)
FORGE_DEFINE_VALUE_ACCESSOR(AsLinearColor, LinearColor, LinearColorValue, FLinearColor)

#undef FORGE_DEFINE_VALUE_ACCESSOR

UObject* FForgeValue::AsObject() const
{
    CheckType(Type, EForgeValueType::Object);
    return ObjectValue.Get();
}

const TStrongObjectPtr<UObject>& FForgeValue::AsStrongObject() const
{
    CheckType(Type, EForgeValueType::Object);
    return ObjectValue;
}

const TWeakObjectPtr<UObject>& FForgeValue::AsWeakObject() const
{
    CheckType(Type, EForgeValueType::WeakObject);
    return WeakObjectValue;
}

const FSoftObjectPath& FForgeValue::AsSoftObjectPath() const
{
    CheckType(Type, EForgeValueType::SoftObject);
    return SoftObjectPathValue;
}

const FSoftObjectPath& FForgeValue::AsSoftClassPath() const
{
    CheckType(Type, EForgeValueType::SoftClass);
    return SoftClassPathValue;
}

const FForgeObjectHandle& FForgeValue::AsNativeObject() const
{
    CheckType(Type, EForgeValueType::NativeObject);
    return NativeObjectValue;
}

int32 FForgeValue::AsEnum(const TCHAR* ExpectedTypeName) const
{
    CheckType(Type, EForgeValueType::Enum);
    checkf(ReflectedTypeName == ExpectedTypeName, TEXT("Forge reflected enum type mismatch"));
    return Data.Int32Value;
}

const FForgeContainer& FForgeValue::AsContainer(
    EForgeValueType ExpectedKind,
    const TCHAR* ExpectedTypeName) const
{
    checkf(
        Type == ExpectedKind &&
            ReflectedTypeName == ExpectedTypeName &&
            ContainerValue.IsValid() &&
            ContainerValue->Kind == ExpectedKind &&
            ContainerValue->TypeName == ExpectedTypeName,
        TEXT("Forge reflected container type mismatch"));
    return *ContainerValue;
}
