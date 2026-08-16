#pragma once

#include "CoreMinimal.h"
#include "ForgeBindingRegistry.h"
#include "UObject/Object.h"

#include "ForgeDynamicDelegateProxy.generated.h"

UCLASS(Abstract, Transient)
class FORGERUNTIME_API UForgeDynamicDelegateProxy : public UObject
{
    GENERATED_BODY()

public:
    void InitializeForgeCallback(const FForgeDelegateCallback& InCallback);
    void ResetForgeCallback();

protected:
    FForgeInvokeResult InvokeForgeCallback(
        TArrayView<FForgeValue> Arguments,
        FForgeValue& OutReturn);

    virtual void BeginDestroy() override;

private:
    FForgeDelegateCallback Callback;
};
