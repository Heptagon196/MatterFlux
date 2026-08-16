#include "ForgeObjectRegistry.h"
#include "IForgeRuntimeModule.h"

#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
std::atomic<int32> OwnedDestroyCount{ 0 };

void DestroyOwnedInt(void* Address)
{
    ++OwnedDestroyCount;
    delete static_cast<int32*>(Address);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FForgeObjectRegistryTest,
    "Forge.Runtime.ObjectRegistry.BorrowedHandleLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FForgeObjectRegistryTest::RunTest(const FString& Parameters)
{
    IForgeObjectRegistry& Registry = IForgeRuntimeModule::Get().GetObjectRegistry();
    int32 FirstObject = 17;
    int32 SecondObject = 23;
    const FName IntTypeId(TEXT("sha256:forge-tests-native-int"));
    const FName IntType(TEXT("ForgeTests::FNativeInt"));

    const FForgeObjectRegistrationResult InvalidAddress =
        Registry.RegisterBorrowed(nullptr, IntTypeId, IntType, false);
    TestFalse(TEXT("Null borrowed address is rejected"), InvalidAddress.bSuccess);

    const FForgeObjectRegistrationResult First =
        Registry.RegisterBorrowed(&FirstObject, IntTypeId, IntType, false);
    TestTrue(TEXT("Mutable borrowed object registers"), First.bSuccess);
    if (!First.bSuccess)
    {
        return false;
    }

    const FForgeObjectRegistrationResult Duplicate =
        Registry.RegisterBorrowed(&FirstObject, IntTypeId, IntType, false);
    TestFalse(
        TEXT("The same live address cannot acquire a second independent lifetime"),
        Duplicate.bSuccess);

    FForgeObjectResolveResult Resolve = Registry.Resolve(First.Handle, IntType, true);
    TestTrue(TEXT("Mutable handle resolves with matching type"), Resolve.bSuccess);
    TestTrue(TEXT("Resolve returns the registered native address"), Resolve.Address == &FirstObject);

    Resolve = Registry.Resolve(First.Handle, FName(TEXT("ForgeTests::FOther")), false);
    TestFalse(TEXT("Wrong expected type is rejected"), Resolve.bSuccess);

    std::atomic<int32> ConcurrentFailures{ 0 };
    ParallelFor(64, [&Registry, &First, &FirstObject, &ConcurrentFailures, IntType](int32)
    {
        for (int32 Attempt = 0; Attempt < 64; ++Attempt)
        {
            const FForgeObjectResolveResult ConcurrentResolve =
                Registry.Resolve(First.Handle, IntType, false);
            if (!ConcurrentResolve.bSuccess || ConcurrentResolve.Address != &FirstObject)
            {
                ConcurrentFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    TestEqual(
        TEXT("Concurrent readers resolve the same live slot safely"),
        ConcurrentFailures.load(std::memory_order_relaxed),
        0);

    const FForgeObjectOperationResult FirstInvalidation = Registry.Invalidate(First.Handle);
    TestTrue(TEXT("First invalidation succeeds"), FirstInvalidation.bSuccess);
    Resolve = Registry.Resolve(First.Handle, IntType, false);
    TestFalse(TEXT("Invalidated generation no longer resolves"), Resolve.bSuccess);

    const FForgeObjectOperationResult RepeatedInvalidation = Registry.Invalidate(First.Handle);
    TestFalse(TEXT("Repeated invalidation is rejected"), RepeatedInvalidation.bSuccess);
    TestEqual(
        TEXT("Repeated invalidation has a deterministic reason"),
        RepeatedInvalidation.Error,
        FString(TEXT("Forge object handle is stale or inactive")));

    const FForgeObjectRegistrationResult Second =
        Registry.RegisterBorrowed(&SecondObject, IntTypeId, IntType, true);
    TestTrue(TEXT("Read-only borrowed object registers"), Second.bSuccess);
    if (!Second.bSuccess)
    {
        return false;
    }
    TestEqual(TEXT("Freed slot is reused"), Second.Handle.GetSlot(), First.Handle.GetSlot());
    TestTrue(
        TEXT("Reused slot receives a new generation"),
        Second.Handle.GetGeneration() != First.Handle.GetGeneration());

    Resolve = Registry.Resolve(First.Handle, IntType, false);
    TestFalse(TEXT("Old generation cannot alias the reused slot"), Resolve.bSuccess);
    Resolve = Registry.Resolve(Second.Handle, IntType, false);
    TestTrue(TEXT("Current read-only generation resolves for reads"), Resolve.bSuccess);
    TestTrue(TEXT("Current generation resolves to the second address"), Resolve.Address == &SecondObject);
    Resolve = Registry.Resolve(Second.Handle, IntType, true);
    TestFalse(TEXT("Read-only handle rejects mutable access"), Resolve.bSuccess);
    TestEqual(
        TEXT("Mutable access rejection identifies read-only state"),
        Resolve.Error,
        FString(TEXT("Forge object handle is read-only")));

    TestTrue(TEXT("Final invalidation succeeds"), Registry.Invalidate(Second.Handle).bSuccess);

    int32 LeasedObject = 31;
    const FForgeObjectRegistrationResult Leased =
        Registry.RegisterBorrowed(&LeasedObject, IntTypeId, IntType, false);
    TestTrue(TEXT("Lease test object registers"), Leased.bSuccess);
    if (!Leased.bSuccess)
    {
        return false;
    }
    FForgeObjectLeaseResult WrongOwnerLease = Registry.Acquire(
        Leased.Handle,
        FName(TEXT("sha256:forge-tests-wrong-native-int")),
        IntType,
        false);
    TestFalse(TEXT("Lease rejects a wrong owner TypeId"), WrongOwnerLease.bSuccess);

    FForgeObjectLeaseResult Lease = Registry.Acquire(
        Leased.Handle,
        IntTypeId,
        IntType,
        true);
    TestTrue(TEXT("Mutable call lease acquires the native address"), Lease.bSuccess);
    TestTrue(TEXT("Lease exposes the address only to the immediate native caller"),
        Lease.Lease.GetAddress() == &LeasedObject);
    if (!Lease.bSuccess)
    {
        Registry.Invalidate(Leased.Handle);
        return false;
    }

    std::atomic<bool> bInvalidationStarted{ false };
    TFuture<FForgeObjectOperationResult> PendingInvalidation = Async(
        EAsyncExecution::Thread,
        [&Registry, &Leased, &bInvalidationStarted]()
        {
            bInvalidationStarted.store(true, std::memory_order_release);
            return Registry.Invalidate(Leased.Handle);
        });
    while (!bInvalidationStarted.load(std::memory_order_acquire))
    {
        FPlatformProcess::YieldThread();
    }
    FPlatformProcess::Sleep(0.02f);
    TestFalse(TEXT("Invalidation waits while an active call lease holds the read lock"),
        PendingInvalidation.IsReady());
    Lease.Lease = FForgeObjectLease{};
    const FForgeObjectOperationResult LeasedInvalidation = PendingInvalidation.Get();
    TestTrue(TEXT("Invalidation completes after the call lease releases"),
        LeasedInvalidation.bSuccess);

    OwnedDestroyCount.store(0, std::memory_order_relaxed);
    int32* OwnedAddress = new int32(47);
    FForgeObjectRegistrationResult Owned = Registry.RegisterOwned(
        OwnedAddress,
        IntTypeId,
        IntType,
        &DestroyOwnedInt);
    TestTrue(TEXT("Owned native object registers"), Owned.bSuccess);
    TestTrue(TEXT("Owned handle is distinct from borrowed handles"),
        Owned.Handle.IsOwned());
    FForgeObjectHandle OwnedCopy = Owned.Handle;
    FForgeObjectLeaseResult OwnedLease = Registry.Acquire(
        OwnedCopy,
        IntTypeId,
        IntType,
        true);
    TestTrue(TEXT("Owned object supports the same typed call lease"), OwnedLease.bSuccess);
    if (OwnedLease.bSuccess)
    {
        TestEqual(TEXT("Owned lease reaches native storage"),
            *static_cast<int32*>(OwnedLease.Lease.GetAddress()), 47);
    }
    TestFalse(TEXT("Owned object cannot be invalidated as borrowed storage"),
        Registry.Invalidate(Owned.Handle).bSuccess);
    OwnedLease.Lease = FForgeObjectLease{};

    const FForgeObjectRegistrationResult DuplicateOwned =
        Registry.RegisterOwned(
            OwnedAddress,
            IntTypeId,
            IntType,
            &DestroyOwnedInt);
    TestFalse(
        TEXT("The same live owned address cannot be registered twice"),
        DuplicateOwned.bSuccess);
    TestEqual(
        TEXT("Failed owned registration leaves the existing owner alive"),
        OwnedDestroyCount.load(std::memory_order_relaxed),
        0);

    Owned.Handle = FForgeObjectHandle{};
    TestEqual(TEXT("A remaining owned handle copy keeps allocation alive"),
        OwnedDestroyCount.load(std::memory_order_relaxed), 0);
    OwnedCopy = FForgeObjectHandle{};
    TestEqual(TEXT("Last owned handle destroys native allocation exactly once"),
        OwnedDestroyCount.load(std::memory_order_relaxed), 1);
    return true;
}

#endif
