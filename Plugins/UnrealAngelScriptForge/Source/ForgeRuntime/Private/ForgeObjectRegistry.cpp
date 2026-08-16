#include "ForgeObjectRegistry.h"

#include "Misc/ScopeRWLock.h"

namespace
{
uint32 NextGeneration(uint32 Generation)
{
    ++Generation;
    return Generation == 0 ? 1 : Generation;
}
}

FForgeOwnedObjectAllocation::FForgeOwnedObjectAllocation(
    void* InAddress,
    FForgeObjectDestroy InDestroy)
    : Address(InAddress)
    , Destroy(InDestroy)
{
}

FForgeOwnedObjectAllocation::~FForgeOwnedObjectAllocation()
{
    if (Address != nullptr && Destroy != nullptr)
    {
        Destroy(Address);
    }
}

class FForgeObjectRegistry final : public IForgeObjectRegistry
{
public:
    virtual FForgeObjectRegistrationResult RegisterBorrowed(
        void* Address,
        FName TypeId,
        FName TypeName,
        bool bReadOnly) override
    {
        if (Address == nullptr)
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Borrowed Forge object address is required"));
        }
        if (TypeId.IsNone() || TypeName.IsNone())
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Borrowed Forge object type id and name are required"));
        }

        FWriteScopeLock ScopeLock(Lock);
        ReclaimExpiredOwnedSlots();
        if (ActiveAddresses.Contains(Address))
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Borrowed Forge object address is already registered"));
        }
        uint32 SlotIndex = MAX_uint32;
        if (!FreeSlots.IsEmpty())
        {
            SlotIndex = FreeSlots.Pop(EAllowShrinking::No);
        }
        else
        {
            SlotIndex = static_cast<uint32>(Slots.AddDefaulted());
        }

        FSlot& Slot = Slots[SlotIndex];
        Slot.Address = Address;
        Slot.TypeId = TypeId;
        Slot.TypeName = TypeName;
        Slot.bReadOnly = bReadOnly;
        Slot.bActive = true;
        Slot.bOwned = false;
        Slot.OwnedAllocation.Reset();
        ActiveAddresses.Add(Address, SlotIndex);
        return FForgeObjectRegistrationResult::Success(FForgeObjectHandle(
            SlotIndex,
            Slot.Generation,
            TypeId,
            TypeName,
            bReadOnly));
    }

    virtual FForgeObjectRegistrationResult RegisterOwned(
        void* Address,
        FName TypeId,
        FName TypeName,
        FForgeObjectDestroy Destroy) override
    {
        if (Address == nullptr || Destroy == nullptr)
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Owned Forge object address and destroy thunk are required"));
        }
        if (TypeId.IsNone() || TypeName.IsNone())
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Owned Forge object type id and name are required"));
        }

        FWriteScopeLock ScopeLock(Lock);
        ReclaimExpiredOwnedSlots();
        if (ActiveAddresses.Contains(Address))
        {
            return FForgeObjectRegistrationResult::Failure(
                TEXT("Owned Forge object address is already registered"));
        }
        TSharedPtr<FForgeOwnedObjectAllocation, ESPMode::ThreadSafe> Allocation =
            MakeShared<FForgeOwnedObjectAllocation, ESPMode::ThreadSafe>(
                Address,
                Destroy);
        const uint32 SlotIndex = AllocateSlot();
        FSlot& Slot = Slots[SlotIndex];
        Slot.Address = Address;
        Slot.TypeId = TypeId;
        Slot.TypeName = TypeName;
        Slot.bReadOnly = false;
        Slot.bActive = true;
        Slot.bOwned = true;
        Slot.OwnedAllocation = Allocation;
        ActiveAddresses.Add(Address, SlotIndex);
        return FForgeObjectRegistrationResult::Success(FForgeObjectHandle(
            SlotIndex,
            Slot.Generation,
            TypeId,
            TypeName,
            false,
            MoveTemp(Allocation)));
    }

    virtual FForgeObjectOperationResult Invalidate(
        const FForgeObjectHandle& Handle) override
    {
        if (!Handle.IsSet())
        {
            return FForgeObjectOperationResult::Failure(
                TEXT("Forge object handle is not set"));
        }
        if (Handle.IsOwned())
        {
            return FForgeObjectOperationResult::Failure(
                TEXT("Owned Forge objects are released by handle lifetime"));
        }

        FWriteScopeLock ScopeLock(Lock);
        if (!Slots.IsValidIndex(static_cast<int32>(Handle.GetSlot())))
        {
            return FForgeObjectOperationResult::Failure(
                TEXT("Forge object handle slot is invalid"));
        }
        FSlot& Slot = Slots[Handle.GetSlot()];
        if (!Slot.bActive ||
            Slot.Generation != Handle.GetGeneration() ||
            Slot.TypeId != Handle.GetTypeId() ||
            Slot.TypeName != Handle.GetTypeName() ||
            Slot.bReadOnly != Handle.IsReadOnly())
        {
            return FForgeObjectOperationResult::Failure(
                TEXT("Forge object handle is stale or inactive"));
        }
        ActiveAddresses.Remove(Slot.Address);
        Slot.Address = nullptr;
        Slot.TypeId = NAME_None;
        Slot.TypeName = NAME_None;
        Slot.bReadOnly = true;
        Slot.bActive = false;
        Slot.bOwned = false;
        Slot.OwnedAllocation.Reset();
        Slot.Generation = NextGeneration(Slot.Generation);
        FreeSlots.Add(Handle.GetSlot());
        return FForgeObjectOperationResult::Success();
    }

    virtual FForgeObjectResolveResult Resolve(
        const FForgeObjectHandle& Handle,
        FName ExpectedTypeName,
        bool bRequireMutable) const override
    {
        if (!Handle.IsSet())
        {
            return FForgeObjectResolveResult::Failure(
                TEXT("Forge object handle is not set"));
        }
        if (!ExpectedTypeName.IsNone() && ExpectedTypeName != Handle.GetTypeName())
        {
            return FForgeObjectResolveResult::Failure(
                TEXT("Forge object handle type mismatch"));
        }
        if (bRequireMutable && Handle.IsReadOnly())
        {
            return FForgeObjectResolveResult::Failure(
                TEXT("Forge object handle is read-only"));
        }

        FReadScopeLock ScopeLock(Lock);
        if (!Slots.IsValidIndex(static_cast<int32>(Handle.GetSlot())))
        {
            return FForgeObjectResolveResult::Failure(
                TEXT("Forge object handle slot is invalid"));
        }
        const FSlot& Slot = Slots[Handle.GetSlot()];
        if (!Slot.bActive ||
            Slot.Address == nullptr ||
            Slot.Generation != Handle.GetGeneration() ||
            Slot.TypeId != Handle.GetTypeId() ||
            Slot.TypeName != Handle.GetTypeName() ||
            Slot.bReadOnly != Handle.IsReadOnly())
        {
            return FForgeObjectResolveResult::Failure(
                TEXT("Forge object handle is stale or inactive"));
        }
        if (Slot.bOwned && !Slot.OwnedAllocation.IsValid())
        {
            return FForgeObjectResolveResult::Failure(
                TEXT("Forge owned object has expired"));
        }
        return FForgeObjectResolveResult::Success(Slot.Address);
    }

    virtual FForgeObjectLeaseResult Acquire(
        const FForgeObjectHandle& Handle,
        FName ExpectedTypeId,
        FName ExpectedTypeName,
        bool bRequireMutable) const override
    {
        if (!Handle.IsSet())
        {
            return FForgeObjectLeaseResult::Failure(
                TEXT("Forge object handle is not set"));
        }
        if ((!ExpectedTypeId.IsNone() && ExpectedTypeId != Handle.GetTypeId()) ||
            (!ExpectedTypeName.IsNone() && ExpectedTypeName != Handle.GetTypeName()))
        {
            return FForgeObjectLeaseResult::Failure(
                TEXT("Forge object handle owner type mismatch"));
        }
        if (bRequireMutable && Handle.IsReadOnly())
        {
            return FForgeObjectLeaseResult::Failure(
                TEXT("Forge object handle is read-only"));
        }

        TUniquePtr<FReadScopeLock> ScopeLock = MakeUnique<FReadScopeLock>(Lock);
        if (!Slots.IsValidIndex(static_cast<int32>(Handle.GetSlot())))
        {
            return FForgeObjectLeaseResult::Failure(
                TEXT("Forge object handle slot is invalid"));
        }
        const FSlot& Slot = Slots[Handle.GetSlot()];
        if (!Slot.bActive ||
            Slot.Address == nullptr ||
            Slot.Generation != Handle.GetGeneration() ||
            Slot.TypeId != Handle.GetTypeId() ||
            Slot.TypeName != Handle.GetTypeName() ||
            Slot.bReadOnly != Handle.IsReadOnly())
        {
            return FForgeObjectLeaseResult::Failure(
                TEXT("Forge object handle is stale or inactive"));
        }
        TSharedPtr<FForgeOwnedObjectAllocation, ESPMode::ThreadSafe> OwnedAllocation;
        if (Slot.bOwned)
        {
            OwnedAllocation = Slot.OwnedAllocation.Pin();
            if (!OwnedAllocation.IsValid())
            {
                return FForgeObjectLeaseResult::Failure(
                    TEXT("Forge owned object has expired"));
            }
        }
        return FForgeObjectLeaseResult::Success(FForgeObjectLease(
            Slot.Address,
            MoveTemp(ScopeLock),
            MoveTemp(OwnedAllocation)));
    }

private:
    struct FSlot
    {
        void* Address = nullptr;
        FName TypeId;
        FName TypeName;
        uint32 Generation = 1;
        bool bReadOnly = true;
        bool bActive = false;
        bool bOwned = false;
        TWeakPtr<FForgeOwnedObjectAllocation, ESPMode::ThreadSafe> OwnedAllocation;
    };

    uint32 AllocateSlot()
    {
        return !FreeSlots.IsEmpty()
            ? FreeSlots.Pop(EAllowShrinking::No)
            : static_cast<uint32>(Slots.AddDefaulted());
    }

    void ReclaimExpiredOwnedSlots()
    {
        for (int32 Index = 0; Index < Slots.Num(); ++Index)
        {
            FSlot& Slot = Slots[Index];
            if (!Slot.bActive || !Slot.bOwned || Slot.OwnedAllocation.IsValid())
            {
                continue;
            }
            ActiveAddresses.Remove(Slot.Address);
            Slot.Address = nullptr;
            Slot.TypeId = NAME_None;
            Slot.TypeName = NAME_None;
            Slot.bReadOnly = true;
            Slot.bActive = false;
            Slot.bOwned = false;
            Slot.OwnedAllocation.Reset();
            Slot.Generation = NextGeneration(Slot.Generation);
            FreeSlots.Add(static_cast<uint32>(Index));
        }
    }

    mutable FRWLock Lock;
    TArray<FSlot> Slots;
    TArray<uint32> FreeSlots;
    TMap<void*, uint32> ActiveAddresses;
};

TUniquePtr<IForgeObjectRegistry> CreateForgeObjectRegistry()
{
    return MakeUnique<FForgeObjectRegistry>();
}

FForgeObjectRegistrationResult FForgeObjectRegistrationResult::Success(
    FForgeObjectHandle Handle)
{
    return { true, FString{}, MoveTemp(Handle) };
}

FForgeObjectRegistrationResult FForgeObjectRegistrationResult::Failure(FString Message)
{
    return { false, MoveTemp(Message), FForgeObjectHandle{} };
}

FForgeObjectOperationResult FForgeObjectOperationResult::Success()
{
    return { true, FString{} };
}

FForgeObjectOperationResult FForgeObjectOperationResult::Failure(FString Message)
{
    return { false, MoveTemp(Message) };
}

FForgeObjectResolveResult FForgeObjectResolveResult::Success(void* Address)
{
    return { true, FString{}, Address };
}

FForgeObjectResolveResult FForgeObjectResolveResult::Failure(FString Message)
{
    return { false, MoveTemp(Message), nullptr };
}

FForgeObjectLeaseResult FForgeObjectLeaseResult::Success(FForgeObjectLease Lease)
{
    return { true, FString{}, MoveTemp(Lease) };
}

FForgeObjectLeaseResult FForgeObjectLeaseResult::Failure(FString Message)
{
    return { false, MoveTemp(Message), FForgeObjectLease{} };
}
