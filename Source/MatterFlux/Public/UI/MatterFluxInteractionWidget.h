#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MatterFluxInteractionWidget.generated.h"

class AMatterFluxCreatureActor;
class AMatterFluxPlayerController;
class SVerticalBox;

/** Local presentation of the replicated, server-authoritative NPC interaction. */
UCLASS()
class MATTERFLUX_API UMatterFluxInteractionWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void InitializeForPlayer(AMatterFluxPlayerController* InController);
	void OpenInteraction(AMatterFluxCreatureActor* InCreature, FName InDialogueId);
	void HandlePurchaseResult(
		bool bSuccess,
		int32 OfferIndex,
		int32 RemainingPurchases,
		const FString& Message);
	bool IsInteractionOpen() const;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	void RefreshContent();
	void ShowDialogueNode(FName NodeId);
	void ShowShop(FName ShopId);
	FString ResolveContentName(FName ContentId) const;
	int32 ResolveProgressionRevision() const;

	UPROPERTY(Transient)
	TObjectPtr<AMatterFluxPlayerController> PlayerController;

	TWeakObjectPtr<AMatterFluxCreatureActor> Creature;
	FName DialogueId;
	FName CurrentNodeId;
	FName ActiveShopId;
	FString StatusMessage;
	TMap<int32, int32> KnownRemainingPurchases;
	TSharedPtr<SVerticalBox> ContentBox;
};
