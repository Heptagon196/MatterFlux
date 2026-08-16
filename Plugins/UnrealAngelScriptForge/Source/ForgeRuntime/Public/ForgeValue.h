#pragma once

#include "CoreMinimal.h"
#include "ForgeObjectRegistry.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include <type_traits>
#include <utility>

enum class EForgeValueType : uint8
{
    Void,
    Bool,
    Int32,
    Float,
    Double,
    String,
    Name,
    Vector,
    Text,
    Rotator,
    Quat,
    Transform,
    Vector2D,
    Guid,
    Color,
    LinearColor,
    Object,
    WeakObject,
    SoftObject,
    SoftClass,
    Enum,
    Struct,
    Array,
    Map,
    Set,
    Optional,
    Variant,
    Pair,
    NativeObject
};

struct FForgeContainer;
class UScriptStruct;

using FForgeValueDefaultConstructThunk = void (*)(void* Destination);
using FForgeValueCopyConstructThunk = void (*)(void* Destination, const void* Source);
using FForgeValueMoveConstructThunk = void (*)(void* Destination, void* Source);
using FForgeValueAssignThunk = void (*)(void* Destination, const void* Source);
using FForgeValueDestroyThunk = void (*)(void* Address);

class FORGERUNTIME_API FForgeStructValueAllocation
{
public:
    FForgeStructValueAllocation(void* InAddress, FForgeValueDestroyThunk InDestroy);
    FForgeStructValueAllocation(void* InAddress, UScriptStruct* InScriptStruct);
    ~FForgeStructValueAllocation();

    void* GetAddress() const { return Address; }

private:
    void* Address = nullptr;
    FForgeValueDestroyThunk Destroy = nullptr;
    TStrongObjectPtr<UScriptStruct> ScriptStruct;
};

class FORGERUNTIME_API FForgeValue
{
public:
    static FForgeValue Void();
    static FForgeValue FromBool(bool Value);
    static FForgeValue FromInt32(int32 Value);
    static FForgeValue FromFloat(float Value);
    static FForgeValue FromDouble(double Value);
    static FForgeValue FromString(const FString& Value);
    static FForgeValue FromName(FName Value);
    static FForgeValue FromVector(const FVector& Value);
    static FForgeValue FromText(const FText& Value);
    static FForgeValue FromRotator(const FRotator& Value);
    static FForgeValue FromQuat(const FQuat& Value);
    static FForgeValue FromTransform(const FTransform& Value);
    static FForgeValue FromVector2D(const FVector2D& Value);
    static FForgeValue FromGuid(const FGuid& Value);
    static FForgeValue FromColor(const FColor& Value);
    static FForgeValue FromLinearColor(const FLinearColor& Value);
    static FForgeValue FromObject(UObject* Value);
    static FForgeValue FromWeakObject(UObject* Value);
    static FForgeValue FromSoftObjectPath(const FSoftObjectPath& Value);
    static FForgeValue FromSoftClassPath(const FSoftObjectPath& Value);
    static FForgeValue FromNativeObject(const FForgeObjectHandle& Value);
    static FForgeValue FromEnum(int32 Value, const TCHAR* TypeName);
    static FForgeValue FromStructBytes(
        const TCHAR* TypeName,
        const void* Data,
        int32 Size);
    static FForgeValue FromManagedStructCopy(
        const TCHAR* TypeName,
        const void* Data,
        int32 Size,
        int32 Alignment,
        FForgeValueCopyConstructThunk CopyConstruct,
        FForgeValueDestroyThunk Destroy);
    static FForgeValue FromScriptStruct(
        const TCHAR* TypeName,
        UScriptStruct* ScriptStruct,
        const void* Data);
    static FForgeValue FromContainer(
        EForgeValueType Kind,
        const TCHAR* TypeName,
        TArray<FForgeValue> Values,
        int32 ActiveIndex = INDEX_NONE);

    template <typename T>
    static FForgeValue FromStruct(T&& Value, const TCHAR* TypeName)
    {
        using NativeType = std::remove_cv_t<std::remove_reference_t<T>>;
        static_assert(std::is_copy_constructible_v<NativeType>,
            "Forge struct values must be copy constructible");
        static_assert(std::is_move_constructible_v<NativeType>,
            "Forge struct values must be move constructible");
        static_assert(std::is_destructible_v<NativeType>,
            "Forge struct values must be destructible");
        FForgeValue Result;
        Result.Type = EForgeValueType::Struct;
        Result.ReflectedTypeName = TypeName;
        if constexpr (std::is_trivially_copyable_v<NativeType> &&
            std::is_trivially_default_constructible_v<NativeType>)
        {
            Result.StructBytes.SetNumUninitialized(sizeof(NativeType));
            FMemory::Memcpy(Result.StructBytes.GetData(), &Value, sizeof(NativeType));
        }
        else
        {
            void* Address = FMemory::Malloc(sizeof(NativeType), alignof(NativeType));
            if constexpr (std::is_lvalue_reference_v<T>)
            {
                new (Address) NativeType(Value);
            }
            else
            {
                new (Address) NativeType(std::forward<T>(Value));
            }
            Result.StructAllocation = MakeShared<FForgeStructValueAllocation, ESPMode::ThreadSafe>(
                Address,
                &DestroyManagedStruct<NativeType>);
        }
        return Result;
    }

    EForgeValueType GetType() const { return Type; }
    bool AsBool() const;
    int32 AsInt32() const;
    float AsFloat() const;
    double AsDouble() const;
    const FString& AsString() const;
    const FName& AsName() const;
    const FVector& AsVector() const;
    const FText& AsText() const;
    const FRotator& AsRotator() const;
    const FQuat& AsQuat() const;
    const FTransform& AsTransform() const;
    const FVector2D& AsVector2D() const;
    const FGuid& AsGuid() const;
    const FColor& AsColor() const;
    const FLinearColor& AsLinearColor() const;
    UObject* AsObject() const;
    const TStrongObjectPtr<UObject>& AsStrongObject() const;
    const TWeakObjectPtr<UObject>& AsWeakObject() const;
    const FSoftObjectPath& AsSoftObjectPath() const;
    const FSoftObjectPath& AsSoftClassPath() const;
    const FForgeObjectHandle& AsNativeObject() const;
    int32 AsEnum(const TCHAR* ExpectedTypeName) const;
    const FForgeContainer& AsContainer(
        EForgeValueType ExpectedKind,
        const TCHAR* ExpectedTypeName) const;

    template <typename T>
    T AsStruct(const TCHAR* ExpectedTypeName) const
    {
        static_assert(std::is_copy_constructible_v<T>,
            "Forge struct values must be copy constructible");
        checkf(
            Type == EForgeValueType::Struct &&
                ReflectedTypeName == ExpectedTypeName,
            TEXT("Forge reflected struct type mismatch"));
        if (StructAllocation.IsValid())
        {
            return T(*static_cast<const T*>(StructAllocation->GetAddress()));
        }
        if constexpr (std::is_trivially_copyable_v<T> &&
            std::is_trivially_default_constructible_v<T>)
        {
            checkf(StructBytes.Num() == sizeof(T), TEXT("Forge reflected struct size mismatch"));
            T Result{};
            FMemory::Memcpy(&Result, StructBytes.GetData(), sizeof(T));
            return Result;
        }
        else
        {
            checkf(false, TEXT("Forge non-trivial struct storage is unavailable"));
            static_assert(std::is_default_constructible_v<T>,
                "Forge struct values must be default constructible");
            return T{};
        }
    }

    const FString& GetReflectedTypeName() const { return ReflectedTypeName; }
    TConstArrayView<uint8> GetStructBytes() const { return StructBytes; }
    const void* GetStructData() const;
    void* GetMutableData();

private:
    template <typename T>
    static void DestroyManagedStruct(void* Address)
    {
        static_cast<T*>(Address)->~T();
    }

    EForgeValueType Type = EForgeValueType::Void;
    union
    {
        bool BoolValue;
        int32 Int32Value;
        float FloatValue;
        double DoubleValue;
    } Data{};
    FString StringValue;
    FName NameValue;
    FVector VectorValue = FVector::ZeroVector;
    FText TextValue;
    FRotator RotatorValue = FRotator::ZeroRotator;
    FQuat QuatValue = FQuat::Identity;
    FTransform TransformValue = FTransform::Identity;
    FVector2D Vector2DValue = FVector2D::ZeroVector;
    FGuid GuidValue;
    FColor ColorValue = FColor::Black;
    FLinearColor LinearColorValue = FLinearColor::Black;
    TStrongObjectPtr<UObject> ObjectValue;
    TWeakObjectPtr<UObject> WeakObjectValue;
    FSoftObjectPath SoftObjectPathValue;
    FSoftObjectPath SoftClassPathValue;
    FForgeObjectHandle NativeObjectValue;
    FString ReflectedTypeName;
    TArray<uint8> StructBytes;
    TSharedPtr<FForgeStructValueAllocation, ESPMode::ThreadSafe> StructAllocation;
    TSharedPtr<FForgeContainer> ContainerValue;
};

struct FORGERUNTIME_API FForgeContainer
{
    EForgeValueType Kind = EForgeValueType::Array;
    FString TypeName;
    TArray<FForgeValue> Values;
    int32 ActiveIndex = INDEX_NONE;
};
