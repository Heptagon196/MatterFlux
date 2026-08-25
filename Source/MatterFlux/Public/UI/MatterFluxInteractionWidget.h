#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Blueprint/UserWidget.h"
#include "MatterFluxInteractionWidget.generated.h"

class AMatterFluxCreatureActor;
class AMatterFluxPlayerController;
class SMatterFluxToast;
class SVerticalBox;
struct FSlateBrush;

/** Local presentation of the replicated, server-authoritative NPC interaction. */
UCLASS()
class MATTERFLUX_API UMatterFluxInteractionWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void InitializeForPlayer(AMatterFluxPlayerController* InController);
	void OpenInteraction(AMatterFluxCreatureActor* InCreature, FName InDialogueId);
	void CloseInteraction();
	void SetInteractionPromptVisible(bool bVisible);
	/** Opens a known shop directly while preserving the normal purchase path. */
	void OpenShop(
		AMatterFluxCreatureActor* InCreature,
		FName InDialogueId,
		FName InShopId);
	void HandlePurchaseResult(
		bool bSuccess,
		int32 OfferIndex,
		int32 RemainingPurchases,
		const FString& Message);
	bool IsInteractionOpen() const;
#if WITH_DEV_AUTOMATION_TESTS
	TSharedRef<SWidget> RebuildWidgetForTesting();
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	void RefreshContent();
	void ShowDialogueNode(FName NodeId);
	void ShowShop(FName ShopId);
	FString ResolveContentName(FName ContentId) const;
	FString ResolveContentIconKey(FName ContentId) const;
	const FSlateBrush* GetIconBrush(const FString& IconKey);
	int32 ResolveProgressionRevision() const;

	UPROPERTY(Transient)
	TObjectPtr<AMatterFluxPlayerController> PlayerController;

	TWeakObjectPtr<AMatterFluxCreatureActor> Creature;
	FName DialogueId;
	FName CurrentNodeId;
	FName ActiveShopId;
	/** Empty selects the implicit all-offers tab. */
	FName ActiveShopCategoryId;
	TMap<int32, int32> KnownRemainingPurchases;
	TMap<FString, TSharedPtr<FSlateDynamicImageBrush>> IconBrushes;
	TSharedPtr<SVerticalBox> ContentBox;
	TSharedPtr<SMatterFluxToast> Toast;
	bool bInteractionOpen = false;
	bool bInteractionPromptVisible = false;
};
