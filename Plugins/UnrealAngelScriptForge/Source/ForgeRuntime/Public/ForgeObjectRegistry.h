#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeRWLock.h"

using FForgeObjectDestroy = void (*)(void* Address);

class FORGERUNTIME_API FForgeOwnedObjectAllocation
{
public:
    FForgeOwnedObjectAllocation(void* InAddress, FForgeObjectDestroy InDestroy);
    ~FForgeOwnedObjectAllocation();

    void* GetAddress() const { return Address; }

private:
    void* Address = nullptr;
    FForgeObjectDestroy Destroy = nullptr;
};

struct FORGERUNTIME_API FForgeObjectHandle
{
public:
    FForgeObjectHandle() = default;

    bool IsSet() const
    {
        return Slot != MAX_uint32 && Generation != 0 &&
            !TypeId.IsNone() && !TypeName.IsNone();
    }

    uint32 GetSlot() const { return Slot; }
    uint32 GetGeneration() const { return Generation; }
    FName GetTypeId() const { return TypeId; }
    FName GetTypeName() const { return TypeName; }
    bool IsReadOnly() const { return bReadOnly; }
    bool IsOwned() const { return OwnedAllocation.IsValid(); }

    friend bool operator==(const FForgeObjectHandle& Left, const FForgeObjectHandle& Right)
    {
        return Left.Slot == Right.Slot &&
            Left.Generation == Right.Generation &&
            Left.TypeId == Right.TypeId &&
            Left.TypeName == Right.TypeName &&
            Left.bReadOnly == Right.bReadOnly;
    }

private:
    friend class FForgeObjectRegistry;

    FForgeObjectHandle(
        uint32 InSlot,
        uint32 InGeneration,
        FName InTypeId,
        FName InTypeName,
        bool bInReadOnly,
        TSharedPtr<FForgeOwnedObjectAllocation, ESPMode::ThreadSafe> InOwnedAllocation = nullptr)
        : Slot(InSlot)
        , Generation(InGeneration)
        , TypeId(InTypeId)
        , TypeName(InTypeName)
        , bReadOnly(bInReadOnly)
        , OwnedAllocation(MoveTemp(InOwnedAllocation))
    {
    }

    uint32 Slot = MAX_uint32;
    uint32 Generation = 0;
    FName TypeId;
    FName TypeName;
    bool bReadOnly = true;
    TSharedPtr<FForgeOwnedObjectAllocation, ESPMode::ThreadSafe> OwnedAllocation;
};

struct FORGERUNTIME_API FForgeObjectRegistrationResult
{
    bool bSuccess = false;
    FString Error;
    FForgeObjectHandle Handle;

    static FForgeObjectRegistrationResult Success(FForgeObjectHandle Handle);
    static FForgeObjectRegistrationResult Failure(FString Message);
};

struct FORGERUNTIME_API FForgeObjectOperationResult
{
    bool bSuccess = false;
    FString Error;

    static FForgeObjectOperationResult Success();
    static FForgeObjectOperationResult Failure(FString Message);
};

struct FORGERUNTIME_API FForgeObjectResolveResult
{
    bool bSuccess = false;
    FString Error;
    void* Address = nullptr;

    static FForgeObjectResolveResult Success(void* Address);
    static FForgeObjectResolveResult Failure(FString Message);
};

class FORGERUNTIME_API FForgeObjectLease
{
public:
    FForgeObjectLease() = default;
    FForgeObjectLease(FForgeObjectLease&&) = default;
    FForgeObjectLease& operator=(FForgeObjectLease&&) = default;
    FForgeObjectLease(const FForgeObjectLease&) = delete;
    FForgeObjectLease& operator=(const FForgeObjectLease&) = delete;

    void* GetAddress() const { return Address; }
    bool IsSet() const { return Address != nullptr && ReadScopeLock.IsValid(); }

private:
    friend class FForgeObjectRegistry;

    FForgeObjectLease(
        void* InAddress,
        TUniquePtr<FReadScopeLock> InReadScopeLock,
        TSharedPtr<FForgeOwnedObjectAllocation, ESPMode::ThreadSafe> InOwnedAllocation = nullptr)
        : Address(InAddress)
        , ReadScopeLock(MoveTemp(InReadScopeLock))
        , OwnedAllocation(MoveTemp(InOwnedAllocation))
    {
    }

    void* Address = nullptr;
    TUniquePtr<FReadScopeLock> ReadScopeLock;
    TSharedPtr<FForgeOwnedObjectAllocation, ESPMode::ThreadSafe> OwnedAllocation;
};

struct FORGERUNTIME_API FForgeObjectLeaseResult
{
    bool bSuccess = false;
    FString Error;
    FForgeObjectLease Lease;

    static FForgeObjectLeaseResult Success(FForgeObjectLease Lease);
    static FForgeObjectLeaseResult Failure(FString Message);
};

class FORGERUNTIME_API IForgeObjectRegistry
{
public:
    virtual ~IForgeObjectRegistry() = default;

    virtual FForgeObjectRegistrationResult RegisterBorrowed(
        void* Address,
        FName TypeId,
        FName TypeName,
        bool bReadOnly) = 0;
    // Ownership transfers to the registry only when registration succeeds.
    virtual FForgeObjectRegistrationResult RegisterOwned(
        void* Address,
        FName TypeId,
        FName TypeName,
        FForgeObjectDestroy Destroy) = 0;
    virtual FForgeObjectOperationResult Invalidate(
        const FForgeObjectHandle& Handle) = 0;
    virtual FForgeObjectResolveResult Resolve(
        const FForgeObjectHandle& Handle,
        FName ExpectedTypeName,
        bool bRequireMutable) const = 0;
    virtual FForgeObjectLeaseResult Acquire(
        const FForgeObjectHandle& Handle,
        FName ExpectedTypeId,
        FName ExpectedTypeName,
        bool bRequireMutable) const = 0;
};
