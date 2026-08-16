#include "ForgeUeReflectionAdapter.h"

#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
FForgeInvokeResult Fail(const FProperty& Property, const FString& Message)
{
    return FForgeInvokeResult::Failure(FString::Printf(
        TEXT("FProperty %s: %s"),
        *Property.GetPathName(),
        *Message));
}

FString CppTypeName(const FProperty& Property)
{
    return Property.GetCPPType(nullptr, CPPF_None);
}

FString NormalizeTypeName(FString Value)
{
    Value.ReplaceInline(TEXT(" "), TEXT(""));
    return Value;
}

bool SameTypeName(const FString& Left, const FString& Right)
{
    return NormalizeTypeName(Left) == NormalizeTypeName(Right);
}

FForgeInvokeResult ReadPropertyValue(
    const FProperty& Property,
    const void* Address,
    FForgeValue& OutValue);

FForgeInvokeResult WritePropertyValue(
    const FProperty& Property,
    void* Address,
    const FForgeValue& Value);

FForgeInvokeResult ReadStruct(
    const FStructProperty& Property,
    const void* Address,
    FForgeValue& OutValue)
{
    UScriptStruct* Struct = Property.Struct;
    if (Struct == TBaseStructure<FVector>::Get())
    {
        OutValue = FForgeValue::FromVector(*static_cast<const FVector*>(Address));
    }
    else if (Struct == TBaseStructure<FRotator>::Get())
    {
        OutValue = FForgeValue::FromRotator(*static_cast<const FRotator*>(Address));
    }
    else if (Struct == TBaseStructure<FQuat>::Get())
    {
        OutValue = FForgeValue::FromQuat(*static_cast<const FQuat*>(Address));
    }
    else if (Struct == TBaseStructure<FTransform>::Get())
    {
        OutValue = FForgeValue::FromTransform(*static_cast<const FTransform*>(Address));
    }
    else if (Struct == TBaseStructure<FVector2D>::Get())
    {
        OutValue = FForgeValue::FromVector2D(*static_cast<const FVector2D*>(Address));
    }
    else if (Struct == TBaseStructure<FGuid>::Get())
    {
        OutValue = FForgeValue::FromGuid(*static_cast<const FGuid*>(Address));
    }
    else if (Struct == TBaseStructure<FColor>::Get())
    {
        OutValue = FForgeValue::FromColor(*static_cast<const FColor*>(Address));
    }
    else if (Struct == TBaseStructure<FLinearColor>::Get())
    {
        OutValue = FForgeValue::FromLinearColor(*static_cast<const FLinearColor*>(Address));
    }
    else
    {
        OutValue = FForgeValue::FromScriptStruct(
            *CppTypeName(Property),
            Struct,
            Address);
    }
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult WriteStruct(
    const FStructProperty& Property,
    void* Address,
    const FForgeValue& Value)
{
    UScriptStruct* Struct = Property.Struct;
    const void* Source = nullptr;
    if (Struct == TBaseStructure<FVector>::Get() && Value.GetType() == EForgeValueType::Vector)
    {
        Source = &Value.AsVector();
    }
    else if (Struct == TBaseStructure<FRotator>::Get() && Value.GetType() == EForgeValueType::Rotator)
    {
        Source = &Value.AsRotator();
    }
    else if (Struct == TBaseStructure<FQuat>::Get() && Value.GetType() == EForgeValueType::Quat)
    {
        Source = &Value.AsQuat();
    }
    else if (Struct == TBaseStructure<FTransform>::Get() && Value.GetType() == EForgeValueType::Transform)
    {
        Source = &Value.AsTransform();
    }
    else if (Struct == TBaseStructure<FVector2D>::Get() && Value.GetType() == EForgeValueType::Vector2D)
    {
        Source = &Value.AsVector2D();
    }
    else if (Struct == TBaseStructure<FGuid>::Get() && Value.GetType() == EForgeValueType::Guid)
    {
        Source = &Value.AsGuid();
    }
    else if (Struct == TBaseStructure<FColor>::Get() && Value.GetType() == EForgeValueType::Color)
    {
        Source = &Value.AsColor();
    }
    else if (Struct == TBaseStructure<FLinearColor>::Get() && Value.GetType() == EForgeValueType::LinearColor)
    {
        Source = &Value.AsLinearColor();
    }
    else if (Value.GetType() == EForgeValueType::Struct &&
        SameTypeName(Value.GetReflectedTypeName(), CppTypeName(Property)))
    {
        Source = Value.GetStructData();
    }
    if (Source == nullptr)
    {
        return Fail(Property, TEXT("Forge value does not match the reflected struct type"));
    }
    Property.CopyCompleteValue(Address, Source);
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult ReadPropertyValue(
    const FProperty& Property,
    const void* Address,
    FForgeValue& OutValue)
{
    if (Property.ArrayDim != 1)
    {
        return Fail(Property, TEXT("static C++ arrays are not supported by the UE adapter"));
    }
    if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property))
    {
        OutValue = FForgeValue::FromBool(BoolProperty->GetPropertyValue(Address));
    }
    else if (const FIntProperty* IntProperty = CastField<FIntProperty>(&Property))
    {
        OutValue = FForgeValue::FromInt32(IntProperty->GetPropertyValue(Address));
    }
    else if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(&Property))
    {
        OutValue = FForgeValue::FromFloat(FloatProperty->GetPropertyValue(Address));
    }
    else if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(&Property))
    {
        OutValue = FForgeValue::FromDouble(DoubleProperty->GetPropertyValue(Address));
    }
    else if (const FStrProperty* StringProperty = CastField<FStrProperty>(&Property))
    {
        OutValue = FForgeValue::FromString(StringProperty->GetPropertyValue(Address));
    }
    else if (const FNameProperty* NameProperty = CastField<FNameProperty>(&Property))
    {
        OutValue = FForgeValue::FromName(NameProperty->GetPropertyValue(Address));
    }
    else if (const FTextProperty* TextProperty = CastField<FTextProperty>(&Property))
    {
        OutValue = FForgeValue::FromText(TextProperty->GetPropertyValue(Address));
    }
    else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
    {
        const int64 Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Address);
        if (Raw < MIN_int32 || Raw > MAX_int32)
        {
            return Fail(Property, TEXT("enum value does not fit Forge int32 storage"));
        }
        OutValue = FForgeValue::FromEnum(static_cast<int32>(Raw), *CppTypeName(Property));
    }
    else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property); ByteProperty && ByteProperty->Enum)
    {
        OutValue = FForgeValue::FromEnum(ByteProperty->GetPropertyValue(Address), *CppTypeName(Property));
    }
    else if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
    {
        return ReadStruct(*StructProperty, Address, OutValue);
    }
    else if (const FWeakObjectProperty* WeakProperty = CastField<FWeakObjectProperty>(&Property))
    {
        OutValue = FForgeValue::FromWeakObject(WeakProperty->GetObjectPropertyValue(Address));
    }
    else if (CastField<FSoftClassProperty>(&Property) != nullptr)
    {
        OutValue = FForgeValue::FromSoftClassPath(
            static_cast<const FSoftObjectPtr*>(Address)->ToSoftObjectPath());
    }
    else if (CastField<FSoftObjectProperty>(&Property) != nullptr)
    {
        OutValue = FForgeValue::FromSoftObjectPath(
            static_cast<const FSoftObjectPtr*>(Address)->ToSoftObjectPath());
    }
    else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
    {
        OutValue = FForgeValue::FromObject(ObjectProperty->GetObjectPropertyValue(Address));
    }
    else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
    {
        FScriptArrayHelper Helper(ArrayProperty, Address);
        TArray<FForgeValue> Values;
        Values.Reserve(Helper.Num());
        for (int32 Index = 0; Index < Helper.Num(); ++Index)
        {
            FForgeValue Item;
            const FForgeInvokeResult Result = ReadPropertyValue(
                *ArrayProperty->Inner, Helper.GetRawPtr(Index), Item);
            if (!Result.bSuccess) return Result;
            Values.Add(MoveTemp(Item));
        }
        OutValue = FForgeValue::FromContainer(
            EForgeValueType::Array, *CppTypeName(Property), MoveTemp(Values));
    }
    else if (const FSetProperty* SetProperty = CastField<FSetProperty>(&Property))
    {
        FScriptSetHelper Helper(SetProperty, Address);
        TArray<FForgeValue> Values;
        Values.Reserve(Helper.Num());
        for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
        {
            if (!Helper.IsValidIndex(Index)) continue;
            FForgeValue Item;
            const FForgeInvokeResult Result = ReadPropertyValue(
                *SetProperty->ElementProp, Helper.GetElementPtr(Index), Item);
            if (!Result.bSuccess) return Result;
            Values.Add(MoveTemp(Item));
        }
        OutValue = FForgeValue::FromContainer(
            EForgeValueType::Set, *CppTypeName(Property), MoveTemp(Values));
    }
    else if (const FMapProperty* MapProperty = CastField<FMapProperty>(&Property))
    {
        FScriptMapHelper Helper(MapProperty, Address);
        TArray<FForgeValue> Values;
        Values.Reserve(Helper.Num() * 2);
        for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
        {
            if (!Helper.IsValidIndex(Index)) continue;
            FForgeValue Key;
            FForgeValue MapValue;
            FForgeInvokeResult Result = ReadPropertyValue(
                *MapProperty->KeyProp, Helper.GetKeyPtr(Index), Key);
            if (!Result.bSuccess) return Result;
            Result = ReadPropertyValue(
                *MapProperty->ValueProp, Helper.GetValuePtr(Index), MapValue);
            if (!Result.bSuccess) return Result;
            Values.Add(MoveTemp(Key));
            Values.Add(MoveTemp(MapValue));
        }
        OutValue = FForgeValue::FromContainer(
            EForgeValueType::Map, *CppTypeName(Property), MoveTemp(Values));
    }
    else
    {
        return Fail(Property, FString::Printf(
            TEXT("unsupported UE property kind %s"), *Property.GetClass()->GetName()));
    }
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult WritePropertyValue(
    const FProperty& Property,
    void* Address,
    const FForgeValue& Value)
{
    if (Property.ArrayDim != 1)
    {
        return Fail(Property, TEXT("static C++ arrays are not supported by the UE adapter"));
    }
    if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Bool) return Fail(Property, TEXT("expected bool"));
        BoolProperty->SetPropertyValue(Address, Value.AsBool());
    }
    else if (const FIntProperty* IntProperty = CastField<FIntProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Int32) return Fail(Property, TEXT("expected int32"));
        IntProperty->SetPropertyValue(Address, Value.AsInt32());
    }
    else if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Float) return Fail(Property, TEXT("expected float"));
        FloatProperty->SetPropertyValue(Address, Value.AsFloat());
    }
    else if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Double) return Fail(Property, TEXT("expected double"));
        DoubleProperty->SetPropertyValue(Address, Value.AsDouble());
    }
    else if (const FStrProperty* StringProperty = CastField<FStrProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::String) return Fail(Property, TEXT("expected FString"));
        StringProperty->SetPropertyValue(Address, Value.AsString());
    }
    else if (const FNameProperty* NameProperty = CastField<FNameProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Name) return Fail(Property, TEXT("expected FName"));
        NameProperty->SetPropertyValue(Address, Value.AsName());
    }
    else if (const FTextProperty* TextProperty = CastField<FTextProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Text) return Fail(Property, TEXT("expected FText"));
        TextProperty->SetPropertyValue(Address, Value.AsText());
    }
    else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Enum ||
            !SameTypeName(Value.GetReflectedTypeName(), CppTypeName(Property)))
        {
            return Fail(Property, TEXT("expected matching enum"));
        }
        EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(
            Address, static_cast<int64>(Value.AsEnum(*Value.GetReflectedTypeName())));
    }
    else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property); ByteProperty && ByteProperty->Enum)
    {
        if (Value.GetType() != EForgeValueType::Enum ||
            !SameTypeName(Value.GetReflectedTypeName(), CppTypeName(Property)))
        {
            return Fail(Property, TEXT("expected matching byte enum"));
        }
        ByteProperty->SetPropertyValue(Address, static_cast<uint8>(
            Value.AsEnum(*Value.GetReflectedTypeName())));
    }
    else if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
    {
        return WriteStruct(*StructProperty, Address, Value);
    }
    else if (const FWeakObjectProperty* WeakProperty = CastField<FWeakObjectProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::WeakObject) return Fail(Property, TEXT("expected weak UObject handle"));
        UObject* Object = Value.AsWeakObject().Get();
        if (Object != nullptr && !Object->IsA(WeakProperty->PropertyClass)) return Fail(Property, TEXT("weak UObject class mismatch"));
        WeakProperty->SetObjectPropertyValue(Address, Object);
    }
    else if (CastField<FSoftClassProperty>(&Property) != nullptr)
    {
        if (Value.GetType() != EForgeValueType::SoftClass) return Fail(Property, TEXT("expected soft class path"));
        const FSoftObjectPtr Soft(Value.AsSoftClassPath());
        Property.CopyCompleteValue(Address, &Soft);
    }
    else if (CastField<FSoftObjectProperty>(&Property) != nullptr)
    {
        if (Value.GetType() != EForgeValueType::SoftObject) return Fail(Property, TEXT("expected soft object path"));
        const FSoftObjectPtr Soft(Value.AsSoftObjectPath());
        Property.CopyCompleteValue(Address, &Soft);
    }
    else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Object) return Fail(Property, TEXT("expected UObject handle"));
        UObject* Object = Value.AsObject();
        if (Object != nullptr && !Object->IsA(ObjectProperty->PropertyClass)) return Fail(Property, TEXT("UObject class mismatch"));
        ObjectProperty->SetObjectPropertyValue(Address, Object);
    }
    else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Array)
        {
            return Fail(Property, TEXT("expected matching TArray"));
        }
        const FForgeContainer& Container = Value.AsContainer(
            EForgeValueType::Array, *Value.GetReflectedTypeName());
        FScriptArrayHelper Helper(ArrayProperty, Address);
        Helper.Resize(Container.Values.Num());
        for (int32 Index = 0; Index < Container.Values.Num(); ++Index)
        {
            const FForgeInvokeResult Result = WritePropertyValue(
                *ArrayProperty->Inner, Helper.GetRawPtr(Index), Container.Values[Index]);
            if (!Result.bSuccess) return Result;
        }
    }
    else if (const FSetProperty* SetProperty = CastField<FSetProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Set)
        {
            return Fail(Property, TEXT("expected matching TSet"));
        }
        const FForgeContainer& Container = Value.AsContainer(
            EForgeValueType::Set, *Value.GetReflectedTypeName());
        FScriptSetHelper Helper(SetProperty, Address);
        Helper.EmptyElements(Container.Values.Num());
        for (const FForgeValue& Item : Container.Values)
        {
            const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
            const FForgeInvokeResult Result = WritePropertyValue(
                *SetProperty->ElementProp, Helper.GetElementPtr(Index), Item);
            if (!Result.bSuccess) return Result;
        }
        Helper.Rehash();
    }
    else if (const FMapProperty* MapProperty = CastField<FMapProperty>(&Property))
    {
        if (Value.GetType() != EForgeValueType::Map)
        {
            return Fail(Property, TEXT("expected matching TMap"));
        }
        const FForgeContainer& Container = Value.AsContainer(
            EForgeValueType::Map, *Value.GetReflectedTypeName());
        if (Container.Values.Num() % 2 != 0) return Fail(Property, TEXT("Forge map storage is corrupt"));
        FScriptMapHelper Helper(MapProperty, Address);
        Helper.EmptyValues(Container.Values.Num() / 2);
        for (int32 Index = 0; Index < Container.Values.Num(); Index += 2)
        {
            const int32 MapIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
            FForgeInvokeResult Result = WritePropertyValue(
                *MapProperty->KeyProp, Helper.GetKeyPtr(MapIndex), Container.Values[Index]);
            if (!Result.bSuccess) return Result;
            Result = WritePropertyValue(
                *MapProperty->ValueProp, Helper.GetValuePtr(MapIndex), Container.Values[Index + 1]);
            if (!Result.bSuccess) return Result;
        }
        Helper.Rehash();
    }
    else
    {
        return Fail(Property, FString::Printf(
            TEXT("unsupported UE property kind %s"), *Property.GetClass()->GetName()));
    }
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult ResolveMappedOwner(
    IForgeBindingRegistry& Registry,
    const FString& UeFieldPath,
    EForgeReflectionKind ExpectedKind,
    UStruct*& OutOwner,
    FName& OutFieldName)
{
    const FForgeUnifiedReflection* Reflection = Registry.FindReflection(UeFieldPath);
    if (Reflection == nullptr || Reflection->Kind != ExpectedKind ||
        !EnumHasAllFlags(Reflection->Sources, EForgeReflectionSource::Unreal) ||
        !EnumHasAllFlags(Reflection->Sources, EForgeReflectionSource::Forge))
    {
        return FForgeInvokeResult::Failure(TEXT("UE FieldPath is not a mapped unified reflection entry"));
    }
    FString OwnerPath;
    FString FieldName;
    if (!UeFieldPath.Split(TEXT(":"), &OwnerPath, &FieldName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) ||
        OwnerPath.IsEmpty() || FieldName.IsEmpty())
    {
        return FForgeInvokeResult::Failure(TEXT("UE member FieldPath is malformed"));
    }
    OutOwner = FindObject<UStruct>(nullptr, *OwnerPath);
    if (OutOwner == nullptr)
    {
        return FForgeInvokeResult::Failure(FString::Printf(
            TEXT("UE owner is not loaded: %s"), *OwnerPath));
    }
    OutFieldName = FName(*FieldName);
    return FForgeInvokeResult::Success();
}

FForgeInvokeResult ValidateInstance(UStruct& Owner, UObject* Instance)
{
    UClass* OwnerClass = Cast<UClass>(&Owner);
    if (OwnerClass == nullptr || Instance == nullptr || !Instance->IsA(OwnerClass))
    {
        return FForgeInvokeResult::Failure(TEXT("UObject instance does not match the reflected owner class"));
    }
    return FForgeInvokeResult::Success();
}
}

FForgeInvokeResult InvokeForgeUeFunction(
    IForgeBindingRegistry& Registry,
    const FString& UeFieldPath,
    UObject* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    if (!IsInGameThread())
    {
        return FForgeInvokeResult::Failure(TEXT("ProcessEvent adapter requires the game thread"));
    }
    UStruct* Owner = nullptr;
    FName FieldName;
    FForgeInvokeResult Result = ResolveMappedOwner(
        Registry, UeFieldPath, EForgeReflectionKind::Function, Owner, FieldName);
    if (!Result.bSuccess) return Result;
    UFunction* Function = FindObject<UFunction>(Owner, *FieldName.ToString());
    if (Function == nullptr)
    {
        return FForgeInvokeResult::Failure(TEXT("Mapped UFunction is not loaded"));
    }
    UObject* Target = Instance;
    if (Function->HasAnyFunctionFlags(FUNC_Static))
    {
        UClass* OwnerClass = Cast<UClass>(Owner);
        if (OwnerClass == nullptr) return FForgeInvokeResult::Failure(TEXT("Static UFunction owner is not a UClass"));
        Target = OwnerClass->GetDefaultObject();
    }
    Result = ValidateInstance(*Owner, Target);
    if (!Result.bSuccess) return Result;

    TArray<FProperty*> Parameters;
    FProperty* ReturnProperty = nullptr;
    for (TFieldIterator<FProperty> It(Function); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property->HasAnyPropertyFlags(CPF_Parm)) break;
        if (Property->HasAnyPropertyFlags(CPF_ReturnParm)) ReturnProperty = Property;
        else Parameters.Add(Property);
    }
    if (Parameters.Num() != Arguments.Num())
    {
        return FForgeInvokeResult::ArgumentCountMismatch(Parameters.Num(), Arguments.Num());
    }

    FStructOnScope ParameterScope(Function);
    void* ParameterMemory = ParameterScope.GetStructMemory();
    if (ParameterMemory == nullptr)
    {
        return FForgeInvokeResult::Failure(TEXT("UFunction parameter storage could not be initialized"));
    }
    for (int32 Index = 0; Index < Parameters.Num(); ++Index)
    {
        Result = WritePropertyValue(
            *Parameters[Index],
            Parameters[Index]->ContainerPtrToValuePtr<void>(ParameterMemory),
            Arguments[Index]);
        if (!Result.bSuccess)
        {
            return Result;
        }
    }

    Target->ProcessEvent(Function, ParameterMemory);
    for (int32 Index = 0; Index < Parameters.Num(); ++Index)
    {
        if (!Parameters[Index]->HasAnyPropertyFlags(CPF_OutParm)) continue;
        Result = ReadPropertyValue(
            *Parameters[Index],
            Parameters[Index]->ContainerPtrToValuePtr<void>(ParameterMemory),
            Arguments[Index]);
        if (!Result.bSuccess) break;
    }
    if (Result.bSuccess && ReturnProperty != nullptr)
    {
        Result = ReadPropertyValue(
            *ReturnProperty,
            ReturnProperty->ContainerPtrToValuePtr<void>(ParameterMemory),
            OutReturn);
    }
    else if (Result.bSuccess)
    {
        OutReturn = FForgeValue::Void();
    }
    return Result;
}

FForgeInvokeResult ReadForgeUeProperty(
    IForgeBindingRegistry& Registry,
    const FString& UeFieldPath,
    UObject* Instance,
    FForgeValue& OutValue)
{
    if (!IsInGameThread()) return FForgeInvokeResult::Failure(TEXT("FProperty adapter requires the game thread"));
    UStruct* Owner = nullptr;
    FName FieldName;
    FForgeInvokeResult Result = ResolveMappedOwner(
        Registry, UeFieldPath, EForgeReflectionKind::Property, Owner, FieldName);
    if (!Result.bSuccess) return Result;
    Result = ValidateInstance(*Owner, Instance);
    if (!Result.bSuccess) return Result;
    FProperty* Property = FindFProperty<FProperty>(Owner, FieldName);
    if (Property == nullptr) return FForgeInvokeResult::Failure(TEXT("Mapped FProperty is not loaded"));
    return ReadPropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(Instance), OutValue);
}

FForgeInvokeResult WriteForgeUeProperty(
    IForgeBindingRegistry& Registry,
    const FString& UeFieldPath,
    UObject* Instance,
    const FForgeValue& Value)
{
    if (!IsInGameThread()) return FForgeInvokeResult::Failure(TEXT("FProperty adapter requires the game thread"));
    UStruct* Owner = nullptr;
    FName FieldName;
    FForgeInvokeResult Result = ResolveMappedOwner(
        Registry, UeFieldPath, EForgeReflectionKind::Property, Owner, FieldName);
    if (!Result.bSuccess) return Result;
    Result = ValidateInstance(*Owner, Instance);
    if (!Result.bSuccess) return Result;
    FProperty* Property = FindFProperty<FProperty>(Owner, FieldName);
    if (Property == nullptr) return FForgeInvokeResult::Failure(TEXT("Mapped FProperty is not loaded"));
    void* Temporary = Property->AllocateAndInitializeValue();
    Result = WritePropertyValue(*Property, Temporary, Value);
    if (Result.bSuccess)
    {
        Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Instance), Temporary);
    }
    Property->DestroyAndFreeValue(Temporary);
    return Result;
}
