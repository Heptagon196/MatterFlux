#include "UI/MatterFluxInteractionWidget.h"

#include "Creatures/MatterFluxCreatureActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "IMatterFluxScriptRuntime.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "UI/MatterFluxPaperStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

using namespace MatterFlux::UI::Paper;

void UMatterFluxInteractionWidget::InitializeForPlayer(
	AMatterFluxPlayerController* InController)
{
	PlayerController = InController;
	SetIsFocusable(true);
}

void UMatterFluxInteractionWidget::OpenInteraction(
	AMatterFluxCreatureActor* InCreature,
	const FName InDialogueId)
{
	Creature = InCreature;
	DialogueId = InDialogueId;
	ActiveShopId = NAME_None;
	StatusMessage.Reset();
	KnownRemainingPurchases.Reset();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxDialogueDefinition* Dialogue = Registry.IsValid()
		? Registry->Dialogues.Find(DialogueId) : nullptr;
	CurrentNodeId = Dialogue ? Dialogue->StartNodeId : NAME_None;
	SetVisibility(ESlateVisibility::Visible);
	RefreshContent();
}

void UMatterFluxInteractionWidget::HandlePurchaseResult(
	const bool bSuccess,
	const int32 OfferIndex,
	const int32 RemainingPurchases,
	const FString& Message)
{
	if (RemainingPurchases != INDEX_NONE)
	{
		KnownRemainingPurchases.Add(OfferIndex, RemainingPurchases);
	}
	StatusMessage = bSuccess
		? TEXT("购买成功，物品已放入背包。")
		: FString::Printf(TEXT("购买失败：%s"), *Message);
	RefreshContent();
}

bool UMatterFluxInteractionWidget::IsInteractionOpen() const
{
	return GetVisibility() != ESlateVisibility::Collapsed;
}

TSharedRef<SWidget> UMatterFluxInteractionWidget::RebuildWidget()
{
	SAssignNew(ContentBox, SVerticalBox);
	return SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(620.0f)
			.MaxDesiredHeight(680.0f)
			[
				Outline(ContentBox.ToSharedRef(), FMargin(26.0f))
			]
		];
}

void UMatterFluxInteractionWidget::ReleaseSlateResources(
	const bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	ContentBox.Reset();
}

void UMatterFluxInteractionWidget::ShowDialogueNode(const FName NodeId)
{
	CurrentNodeId = NodeId;
	ActiveShopId = NAME_None;
	StatusMessage.Reset();
	RefreshContent();
}

void UMatterFluxInteractionWidget::ShowShop(const FName ShopId)
{
	ActiveShopId = ShopId;
	StatusMessage.Reset();
	RefreshContent();
}

FString UMatterFluxInteractionWidget::ResolveContentName(
	const FName ContentId) const
{
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid()) return ContentId.ToString();
	if (const FMatterFluxItemDefinition* Item = Registry->Items.Find(ContentId))
		return Item->DisplayName;
	if (const FMatterFluxSpellDefinition* Spell = Registry->Spells.Find(ContentId))
		return Spell->DisplayName;
	if (const FMatterFluxWandDefinition* Wand = Registry->Wands.Find(ContentId))
		return Wand->DisplayName;
	return ContentId.ToString();
}

int32 UMatterFluxInteractionWidget::ResolveProgressionRevision() const
{
	const AMatterFluxPlayerState* State = PlayerController
		? PlayerController->GetPlayerState<AMatterFluxPlayerState>() : nullptr;
	const UMatterFluxProgressionComponent* Progression = State
		? State->GetProgression() : nullptr;
	return Progression ? Progression->GetRevision() : INDEX_NONE;
}

void UMatterFluxInteractionWidget::RefreshContent()
{
	if (!ContentBox.IsValid()) return;
	ContentBox->ClearChildren();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxDialogueDefinition* Dialogue = Registry.IsValid()
		? Registry->Dialogues.Find(DialogueId) : nullptr;
	if (!Dialogue)
	{
		ContentBox->AddSlot().AutoHeight()[
			SNew(STextBlock).Text(FText::FromString(TEXT("对话内容不可用")))
			.Font(Font(20, true)).ColorAndOpacity(Ink)];
		return;
	}

	ContentBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 18.0f)[
		SNew(STextBlock).Text(FText::FromString(Dialogue->DisplayName))
		.Font(Font(28, true)).ColorAndOpacity(Ink)];

	if (!ActiveShopId.IsNone())
	{
		const FMatterFluxShopDefinition* Shop = Registry->Shops.Find(ActiveShopId);
		if (Shop)
		{
			ContentBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)[
				SNew(STextBlock).Text(FText::FromString(Shop->DisplayName))
				.Font(Font(20, true)).ColorAndOpacity(Ink)];
			TSharedRef<SVerticalBox> OfferRows = SNew(SVerticalBox);
			for (int32 Index = 0; Index < Shop->Offers.Num(); ++Index)
			{
				const FMatterFluxShopOfferDefinition& Offer = Shop->Offers[Index];
				const int32 Remaining = KnownRemainingPurchases.Contains(Index)
					? KnownRemainingPurchases[Index] : Offer.PurchaseLimit;
				FString Label = FString::Printf(
					TEXT("%s ×%d    %s ×%d"),
					*ResolveContentName(Offer.ProductId), Offer.ProductCount,
					*ResolveContentName(Offer.CostItemId), Offer.CostCount);
				if (Remaining >= 0)
				{
					Label += FString::Printf(TEXT("    剩余 %d"), Remaining);
				}
				OfferRows->AddSlot().AutoHeight().Padding(0.0f, 3.0f)[
					Outline(
						SNew(SButton)
						.ButtonStyle(&FlatButtonStyle())
						.IsEnabled(Remaining != 0)
						.ContentPadding(FMargin(14.0f, 9.0f))
						.OnClicked_Lambda([this, Index]()
						{
							if (PlayerController && Creature.IsValid())
							{
								PlayerController->RequestCreaturePurchase(
									Creature.Get(), Index, ResolveProgressionRevision());
							}
							return FReply::Handled();
						})
						[
							SNew(STextBlock).Text(FText::FromString(Label))
							.Font(Font(16)).ColorAndOpacity(Ink)
						], FMargin(0.0f))];
			}
			ContentBox->AddSlot().AutoHeight()[
				SNew(SBox).MaxDesiredHeight(430.0f)[
					SNew(SScrollBox) + SScrollBox::Slot()[OfferRows]]];
		}
	}
	else
	{
		const FMatterFluxDialogueNodeDefinition* Node = Dialogue->Nodes.FindByPredicate(
			[this](const FMatterFluxDialogueNodeDefinition& Candidate)
			{
				return Candidate.Id == CurrentNodeId;
			});
		if (Node)
		{
			ContentBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 18.0f)[
				SNew(STextBlock).Text(FText::FromString(Node->Text))
				.Font(Font(18)).ColorAndOpacity(Ink).AutoWrapText(true)];
			for (const FMatterFluxDialogueOptionDefinition& Option : Node->Options)
			{
				ContentBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)[
					Outline(
						SNew(SButton).ButtonStyle(&FlatButtonStyle())
						.ContentPadding(FMargin(14.0f, 9.0f))
						.OnClicked_Lambda([this, Option]()
						{
							if (Option.bClose)
								PlayerController->CloseCreatureInteraction();
							else if (!Option.ShopId.IsNone()) ShowShop(Option.ShopId);
							else ShowDialogueNode(Option.NextNodeId);
							return FReply::Handled();
						})[
							SNew(STextBlock).Text(FText::FromString(Option.Text))
							.Font(Font(16)).ColorAndOpacity(Ink)], FMargin(0.0f))];
			}
			if (Node->Options.IsEmpty() && !Node->ShopId.IsNone())
			{
				ShowShop(Node->ShopId);
				return;
			}
		}
	}

	if (!StatusMessage.IsEmpty())
	{
		ContentBox->AddSlot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)[
			SNew(STextBlock).Text(FText::FromString(StatusMessage))
			.Font(Font(15)).ColorAndOpacity(Ink).AutoWrapText(true)];
	}
	ContentBox->AddSlot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[
		Outline(
			SNew(SButton).ButtonStyle(&FlatButtonStyle())
			.ContentPadding(FMargin(14.0f, 8.0f))
			.OnClicked_Lambda([this]()
			{
				if (PlayerController) PlayerController->CloseCreatureInteraction();
				return FReply::Handled();
			})[
				SNew(STextBlock).Text(FText::FromString(TEXT("离开")))
				.Font(Font(16, true)).Justification(ETextJustify::Center)
				.ColorAndOpacity(Ink)], FMargin(0.0f))];
}
