#pragma once

#include "CoreMinimal.h"

enum class EForgePatchMode : uint8
{
    Replace,
    Wrap
};

struct FORGERUNTIME_API FForgePatchResult
{
    bool bSuccess = false;
    FString Error;

    static FForgePatchResult Success();
    static FForgePatchResult Failure(FString Message);
};
