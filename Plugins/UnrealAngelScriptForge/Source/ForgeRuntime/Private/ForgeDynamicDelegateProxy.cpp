#include "ForgeDynamicDelegateProxy.h"

void UForgeDynamicDelegateProxy::InitializeForgeCallback(
    const FForgeDelegateCallback& InCallback)
{
    check(IsInGameThread());
    Callback = InCallback;
}

void UForgeDynamicDelegateProxy::ResetForgeCallback()
{
    Callback.Reset();
}

FForgeInvokeResult UForgeDynamicDelegateProxy::InvokeForgeCallback(
    TArrayView<FForgeValue> Arguments,
    FForgeValue& OutReturn)
{
    if (!Callback.IsValid())
    {
        return FForgeInvokeResult::Failure(TEXT("Forge dynamic delegate callback is stale"));
    }
    return Callback->Invoke(Arguments, OutReturn);
}

void UForgeDynamicDelegateProxy::BeginDestroy()
{
    ResetForgeCallback();
    Super::BeginDestroy();
}
