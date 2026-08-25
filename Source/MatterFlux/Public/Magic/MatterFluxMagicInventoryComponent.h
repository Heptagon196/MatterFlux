#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Magic/MatterFluxWandProgram.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "MatterFluxMagicInventoryComponent.generated.h"

class UMatterFluxMagicInventoryComponent;
struct FMatterFluxMagicInventorySaveState;

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxOwnedSpell : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	FName SpellId;

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	int32 Quantity = 0;

	void PostReplicatedAdd(const struct FMatterFluxOwnedSpellList& List);
	void PostReplicatedChange(const struct FMatterFluxOwnedSpellList& List);
	void PreReplicatedRemove(const struct FMatterFluxOwnedSpellList& List);
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxOwnedWand : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	FGuid InstanceId;

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	FName DefinitionId;

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	TArray<FName> SpellSlots;

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	float Mana = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	int32 DeckCursor = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	int32 CastSerial = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	double NextCastServerTime = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Magic")
	double LastManaUpdateServerTime = 0.0;

	void PostReplicatedAdd(const struct FMatterFluxOwnedWandList& List);
	void PostReplicatedChange(const struct FMatterFluxOwnedWandList& List);
	void PreReplicatedRemove(const struct FMatterFluxOwnedWandList& List);
};

USTRUCT()
struct MATTERFLUX_API FMatterFluxOwnedSpellList : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FMatterFluxOwnedSpell> Items;

	TWeakObjectPtr<UMatterFluxMagicInventoryComponent> Owner;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParameters)
	{
		return FastArrayDeltaSerialize<
			FMatterFluxOwnedSpell,
			FMatterFluxOwnedSpellList>(
			Items,
			DeltaParameters,
			*this);
	}
};

template<>
struct TStructOpsTypeTraits<FMatterFluxOwnedSpellList>
	: public TStructOpsTypeTraitsBase2<FMatterFluxOwnedSpellList>
{
	enum { WithNetDeltaSerializer = true };
};

USTRUCT()
struct MATTERFLUX_API FMatterFluxOwnedWandList : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FMatterFluxOwnedWand> Items;

	TWeakObjectPtr<UMatterFluxMagicInventoryComponent> Owner;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParameters)
	{
		return FastArrayDeltaSerialize<
			FMatterFluxOwnedWand,
			FMatterFluxOwnedWandList>(
			Items,
			DeltaParameters,
			*this);
	}
};

template<>
struct TStructOpsTypeTraits<FMatterFluxOwnedWandList>
	: public TStructOpsTypeTraitsBase2<FMatterFluxOwnedWandList>
{
	enum { WithNetDeltaSerializer = true };
};

UENUM(BlueprintType)
enum class EMatterFluxMagicEditType : uint8
{
	EquipWand,
	UnequipWand,
	SelectEquipmentSlot,
	AssignSpell,
	RemoveSpell,
	SwapSpellSlots
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxMagicEdit
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Magic")
	EMatterFluxMagicEditType Type =
		EMatterFluxMagicEditType::SelectEquipmentSlot;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Magic")
	int32 ExpectedRevision = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Magic")
	FGuid WandId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Magic")
	FName SpellId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Magic")
	int32 EquipmentSlot = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Magic")
	int32 FromSpellSlot = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Magic")
	int32 ToSpellSlot = INDEX_NONE;
};

class MATTERFLUX_API FMatterFluxMagicInventoryRules
{
public:
	static bool ApplyEdit(
		const FMatterFluxContentRegistry& Registry,
		TArray<FMatterFluxOwnedSpell>& Spells,
		TArray<FMatterFluxOwnedWand>& Wands,
		TArray<FGuid>& EquippedWands,
		int32& ActiveEquipmentSlot,
		const FMatterFluxMagicEdit& Edit,
		FString& OutError);
};

DECLARE_MULTICAST_DELEGATE(FOnMatterFluxMagicInventoryChanged);

UCLASS(ClassGroup = (MatterFlux), meta = (BlueprintSpawnableComponent))
class MATTERFLUX_API UMatterFluxMagicInventoryComponent
	: public UActorComponent
{
	GENERATED_BODY()

public:
	UMatterFluxMagicInventoryComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(
		const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	const TArray<FMatterFluxOwnedSpell>& GetOwnedSpells() const
	{
		return OwnedSpells.Items;
	}
	const TArray<FMatterFluxOwnedWand>& GetOwnedWands() const
	{
		return OwnedWands.Items;
	}
	const TArray<FGuid>& GetEquippedWands() const
	{
		return EquippedWands;
	}
	int32 GetActiveEquipmentSlot() const { return ActiveEquipmentSlot; }
	int32 GetInventoryRevision() const { return InventoryRevision; }
	const FMatterFluxOwnedWand* FindWand(FGuid WandId) const;
	FGuid GetActiveWandId() const;
	FGuid GetEquippedWandId(int32 EquipmentSlot) const;

	UFUNCTION(BlueprintCallable, Category = "Magic")
	void RequestEdit(const FMatterFluxMagicEdit& Edit);

	bool ApplyEditAuthority(
		const FMatterFluxMagicEdit& Edit,
		FString& OutError);
	bool CommitActiveCastAuthority(
		int32 EventSeed,
		FMatterFluxWandCastPlan& OutPlan,
		FGuid& OutWandId,
		FString& OutError);
	bool ExecuteActiveCastAuthority(
		int32 EventSeed,
		TFunctionRef<bool(const FMatterFluxWandCastPlan&)> Executor,
		FMatterFluxWandCastPlan& OutPlan,
		FGuid& OutWandId,
		FString& OutError);
	bool ExecuteCastAuthority(
		int32 EquipmentSlot,
		int32 EventSeed,
		TFunctionRef<bool(const FMatterFluxWandCastPlan&)> Executor,
		FMatterFluxWandCastPlan& OutPlan,
		FGuid& OutWandId,
		FString& OutError);
	bool CaptureSaveState(
		FMatterFluxMagicInventorySaveState& OutState,
		FString& OutError) const;
	bool RestoreSaveStateAuthority(
		const FMatterFluxMagicInventorySaveState& State,
		FString& OutError);
	bool ResetToStarterLoadoutAuthority(FString& OutError);
	/** Clears every owned/equipped spell and wand while retaining valid slots. */
	bool ResetToEmptyLoadoutAuthority(FString& OutError);
	bool ApplyQuestRewardsAuthority(
		const TArray<FMatterFluxQuestRewardDefinition>& Rewards,
		FString& OutError);
	bool RestoreEquippedWandManaAuthority(
		float Amount,
		FString& OutError);
	/**
	 * Stages progression-owned magic rewards and optional mana restoration,
	 * validates the whole batch, then commits it once. Failure never changes
	 * mana, ownership, equipment, revision, or replicated fast arrays.
	 */
	bool ApplyProgressionEffectsAuthority(
		const TArray<FMatterFluxQuestRewardDefinition>& Rewards,
		float WandManaRestoreAmount,
		FString& OutError);

	FOnMatterFluxMagicInventoryChanged& OnInventoryChanged()
	{
		return InventoryChanged;
	}
	void HandleReplicatedInventoryChanged();

private:
	void InitializeStarterLoadout();
	void HandleContentReloaded(FMatterFluxContentRegistryPtr Registry);
	bool RegenerateWandMana(FMatterFluxOwnedWand& Wand, double Now);
	void MarkAllInventoryDirty();

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerApplyEdit(FMatterFluxMagicEdit Edit);
	bool ServerApplyEdit_Validate(FMatterFluxMagicEdit Edit);
	void ServerApplyEdit_Implementation(FMatterFluxMagicEdit Edit);

	UFUNCTION()
	void OnRep_EquipmentState();

	UPROPERTY(Replicated)
	FMatterFluxOwnedSpellList OwnedSpells;

	UPROPERTY(Replicated)
	FMatterFluxOwnedWandList OwnedWands;

	UPROPERTY(ReplicatedUsing = OnRep_EquipmentState)
	TArray<FGuid> EquippedWands;

	UPROPERTY(ReplicatedUsing = OnRep_EquipmentState)
	int32 ActiveEquipmentSlot = 0;

	UPROPERTY(ReplicatedUsing = OnRep_EquipmentState)
	int32 InventoryRevision = 0;

	FOnMatterFluxMagicInventoryChanged InventoryChanged;
	FDelegateHandle ContentReloadedHandle;
};
