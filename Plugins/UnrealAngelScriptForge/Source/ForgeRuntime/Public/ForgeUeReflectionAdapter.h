#pragma once

#include "CoreMinimal.h"
#include "ForgeBindingRegistry.h"

class UObject;

// Invokes only UHT fields that were correlated with Forge declarations and
// registered in the unified registry. All calls fail closed off the game thread.
FORGERUNTIME_API FForgeInvokeResult InvokeForgeUeFunction(
    IForgeBindingRegistry& Registry,
    const FString& UeFieldPath,
    UObject* Instance,
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn);

FORGERUNTIME_API FForgeInvokeResult ReadForgeUeProperty(
    IForgeBindingRegistry& Registry,
    const FString& UeFieldPath,
    UObject* Instance,
    FForgeValue& OutValue);

FORGERUNTIME_API FForgeInvokeResult WriteForgeUeProperty(
    IForgeBindingRegistry& Registry,
    const FString& UeFieldPath,
    UObject* Instance,
    const FForgeValue& Value);
