#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MatterFluxContentTypes.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "MatterFluxProgressionComponent.generated.h"

class UMatterFluxMagicInventoryComponent;
class UMatterFluxProgressionComponent;
struct FMatterFluxProgressionSaveState;

UENUM(BlueprintType)
enum class EMatterFluxQuestRuntimeStatus : uint8
{
	Hidden,
	Active,
	Completed,
	Failed
};

UENUM(BlueprintType)
enum class EMatterFluxQuestEventType : uint8
{
	Refresh,
	EnemyKilled,
	ItemChanged,
	WorkbenchClosed
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxQuestEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
	EMatterFluxQuestEventType Type = EMatterFluxQuestEventType::Refresh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
	FName SubjectId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
	int32 Amount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
	int32 SubjectLevel = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
	int32 PreviousItemCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
	int32 NewItemCount = 0;
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxItemStack : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Inventory")
	FName ItemId;

	UPROPERTY(BlueprintReadOnly, Category = "Inventory")
	int32 Quantity = 0;

	void PostReplicatedAdd(const struct FMatterFluxItemStackList& List);
	void PostReplicatedChange(const struct FMatterFluxItemStackList& List);
	void PreReplicatedRemove(const struct FMatterFluxItemStackList& List);
};

USTRUCT(BlueprintType)
struct MATTERFLUX_API FMatterFluxQuestState : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Quest")
	FName QuestId;

	UPROPERTY(BlueprintReadOnly, Category = "Quest")
	EMatterFluxQuestRuntimeStatus Status = EMatterFluxQuestRuntimeStatus::Hidden;

	UPROPERTY(BlueprintReadOnly, Category = "Quest")
	int32 Progress = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Quest")
	bool bActivationRewardsGranted = false;

	UPROPERTY(BlueprintReadOnly, Category = "Quest")
	bool bCompletionRewardsGranted = false;

	void PostReplicatedAdd(const struct FMatterFluxQuestStateList& List);
	void PostReplicatedChange(const struct FMatterFluxQuestStateList& List);
	void PreReplicatedRemove(const struct FMatterFluxQuestStateList& List);
};

USTRUCT()
struct MATTERFLUX_API FMatterFluxItemStackList : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FMatterFluxItemStack> Items;

	TWeakObjectPtr<UMatterFluxProgressionComponent> Owner;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParameters)
	{
		return FastArrayDeltaSerialize<
			FMatterFluxItemStack,
			FMatterFluxItemStackList>(Items, DeltaParameters, *this);
	}
};

template<>
struct TStructOpsTypeTraits<FMatterFluxItemStackList>
	: public TStructOpsTypeTraitsBase2<FMatterFluxItemStackList>
{
	enum { WithNetDeltaSerializer = true };
};

USTRUCT()
struct MATTERFLUX_API FMatterFluxQuestStateList : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FMatterFluxQuestState> Items;

	TWeakObjectPtr<UMatterFluxProgressionComponent> Owner;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParameters)
	{
		return FastArrayDeltaSerialize<
			FMatterFluxQuestState,
			FMatterFluxQuestStateList>(Items, DeltaParameters, *this);
	}
};

template<>
struct TStructOpsTypeTraits<FMatterFluxQuestStateList>
	: public TStructOpsTypeTraitsBase2<FMatterFluxQuestStateList>
{
	enum { WithNetDeltaSerializer = true };
};

struct MATTERFLUX_API FMatterFluxProgressionEvaluationContext
{
	TArray<FName> EquippedWands;
	TArray<TSet<FName>> EquippedSpellsBySlot;
};

struct MATTERFLUX_API FMatterFluxProgressionEffects
{
	TArray<FMatterFluxQuestRewardDefinition> Rewards;
	EMatterFluxItemUseAction ItemUseAction = EMatterFluxItemUseAction::None;
	float ItemUseMagnitude = 0.0f;
	FName GameplayEventTag;

	void Reset()
	{
		Rewards.Reset();
		ItemUseAction = EMatterFluxItemUseAction::None;
		ItemUseMagnitude = 0.0f;
		GameplayEventTag = NAME_None;
	}
};

/** Pure deterministic state machine used by authority code and tests. */
class MATTERFLUX_API FMatterFluxProgressionRules
{
public:
	static bool BuildStarterState(
		const FMatterFluxContentRegistry& Registry,
		const FMatterFluxProgressionEvaluationContext& Context,
		TArray<FMatterFluxItemStack>& Items,
		TArray<FMatterFluxQuestState>& Quests,
		FName& SelectedQuest,
		FMatterFluxProgressionEffects& OutEffects,
		FString& OutError);

	static bool AddItem(
		const FMatterFluxContentRegistry& Registry,
		TArray<FMatterFluxItemStack>& Items,
		FName ItemId,
		int32 Delta,
		int32& OutPreviousQuantity,
		int32& OutNewQuantity,
		FString& OutError);

	static bool UseItem(
		const FMatterFluxContentRegistry& Registry,
		TArray<FMatterFluxItemStack>& Items,
		FName ItemId,
		FMatterFluxProgressionEffects& OutEffects,
		FString& OutError);

	static bool NotifyEvent(
		const FMatterFluxContentRegistry& Registry,
		const FMatterFluxProgressionEvaluationContext& Context,
		TArray<FMatterFluxItemStack>& Items,
		TArray<FMatterFluxQuestState>& Quests,
		FName& SelectedQuest,
		const FMatterFluxQuestEvent& Event,
		FMatterFluxProgressionEffects& OutEffects,
		FString& OutError);
};

DECLARE_MULTICAST_DELEGATE(FOnMatterFluxProgressionChanged);

UCLASS(ClassGroup = (MatterFlux), meta = (BlueprintSpawnableComponent))
class MATTERFLUX_API UMatterFluxProgressionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMatterFluxProgressionComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	const TArray<FMatterFluxItemStack>& GetItems() const { return ItemStacks.Items; }
	const TArray<FMatterFluxQuestState>& GetQuests() const { return QuestStates.Items; }
	FName GetSelectedQuest() const { return SelectedQuest; }
	int32 GetRevision() const { return Revision; }
	int32 GetItemQuantity(FName ItemId) const;
	const FMatterFluxQuestState* FindQuestState(FName QuestId) const;

	UFUNCTION(BlueprintCallable, Category = "Progression")
	void RequestUseItem(FName ItemId);

	UFUNCTION(BlueprintCallable, Category = "Progression")
	void RequestSelectQuest(FName QuestId);

	bool AddItemAuthority(FName ItemId, int32 Delta, FString& OutError);
	bool PurchaseOfferAuthority(
		const FMatterFluxShopOfferDefinition& Offer,
		FName OfferKey,
		int32 ExpectedRevision,
		int32& OutRemainingPurchases,
		FString& OutError);
	bool UseItemAuthority(FName ItemId, int32 ExpectedRevision, FString& OutError);
	bool SelectQuestAuthority(FName QuestId, FString& OutError);
	bool NotifyQuestEventAuthority(
		const FMatterFluxQuestEvent& Event,
		FString& OutError);
	bool ResetToStarterStateAuthority(FString& OutError);
	bool CaptureSaveState(
		FMatterFluxProgressionSaveState& OutState,
		FString& OutError) const;
	bool RestoreSaveStateAuthority(
		const FMatterFluxProgressionSaveState& State,
		FString& OutError);

	FOnMatterFluxProgressionChanged& OnProgressionChanged()
	{
		return ProgressionChanged;
	}
	void HandleReplicatedProgressionChanged();

private:
	FMatterFluxProgressionEvaluationContext BuildEvaluationContext() const;
	UMatterFluxMagicInventoryComponent* ResolveMagicInventory() const;
	bool ApplyEffectsAuthority(
		const FMatterFluxProgressionEffects& Effects,
		FString& OutError);
	void HandleMagicInventoryChanged();
	void HandleContentReloaded(FMatterFluxContentRegistryPtr Registry);
	void MarkAllDirty();

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerUseItem(FName ItemId, int32 ExpectedRevision);
	bool ServerUseItem_Validate(FName ItemId, int32 ExpectedRevision);
	void ServerUseItem_Implementation(FName ItemId, int32 ExpectedRevision);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSelectQuest(FName QuestId);
	bool ServerSelectQuest_Validate(FName QuestId);
	void ServerSelectQuest_Implementation(FName QuestId);

	UFUNCTION()
	void OnRep_ProgressionMetadata();

	UPROPERTY(Replicated)
	FMatterFluxItemStackList ItemStacks;

	UPROPERTY(Replicated)
	FMatterFluxQuestStateList QuestStates;

	UPROPERTY(ReplicatedUsing = OnRep_ProgressionMetadata)
	FName SelectedQuest;

	UPROPERTY(ReplicatedUsing = OnRep_ProgressionMetadata)
	int32 Revision = 0;

	TWeakObjectPtr<UMatterFluxMagicInventoryComponent> BoundMagicInventory;
	FDelegateHandle MagicInventoryChangedHandle;
	FDelegateHandle ContentReloadedHandle;
	FOnMatterFluxProgressionChanged ProgressionChanged;
	TMap<FName, int32> ShopPurchaseCounts;
	int32 LastEvaluatedMagicInventoryRevision = INDEX_NONE;
	bool bMagicInventoryRefreshQueued = false;
	bool bApplyingEffects = false;
};
