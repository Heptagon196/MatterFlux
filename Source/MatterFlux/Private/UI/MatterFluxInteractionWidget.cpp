#include "UI/MatterFluxInteractionWidget.h"

#include "Creatures/MatterFluxCreatureActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "IMatterFluxScriptRuntime.h"
#include "Magic/MatterFluxMagicIconResolver.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "UI/MatterFluxPaperStyle.h"
#include "UI/MatterFluxToast.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
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
	ActiveShopCategoryId = NAME_None;
	if (Toast.IsValid()) Toast->Dismiss();
	KnownRemainingPurchases.Reset();
	bInteractionOpen = true;
	bInteractionPromptVisible = false;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxDialogueDefinition* Dialogue = Registry.IsValid()
		? Registry->Dialogues.Find(DialogueId) : nullptr;
	CurrentNodeId = Dialogue ? Dialogue->StartNodeId : NAME_None;
	SetVisibility(ESlateVisibility::Visible);
	RefreshContent();
}

void UMatterFluxInteractionWidget::CloseInteraction()
{
	bInteractionOpen = false;
	bInteractionPromptVisible = false;
	Creature.Reset();
	if (Toast.IsValid()) Toast->Dismiss();
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UMatterFluxInteractionWidget::SetInteractionPromptVisible(
	const bool bVisible)
{
	bInteractionPromptVisible = bVisible && !bInteractionOpen;
	if (bInteractionPromptVisible
		&& GetVisibility() == ESlateVisibility::Collapsed)
	{
		SetVisibility(ESlateVisibility::HitTestInvisible);
	}
}

void UMatterFluxInteractionWidget::OpenShop(
	AMatterFluxCreatureActor* InCreature,
	const FName InDialogueId,
	const FName InShopId)
{
	OpenInteraction(InCreature, InDialogueId);
	ShowShop(InShopId);
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
	RefreshContent();
	if (Toast.IsValid())
	{
		Toast->Show(
			FText::FromString(bSuccess
				? TEXT("购买成功，物品已放入背包。")
				: FString::Printf(TEXT("购买失败：%s"), *Message)),
			bSuccess
				? EMatterFluxToastTone::Success
				: EMatterFluxToastTone::Error);
	}
}

bool UMatterFluxInteractionWidget::IsInteractionOpen() const
{
	return bInteractionOpen;
}

#if WITH_DEV_AUTOMATION_TESTS
TSharedRef<SWidget> UMatterFluxInteractionWidget::RebuildWidgetForTesting()
{
	return RebuildWidget();
}
#endif

TSharedRef<SWidget> UMatterFluxInteractionWidget::RebuildWidget()
{
	SAssignNew(ContentBox, SVerticalBox);
	return SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return bInteractionOpen
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.WidthOverride(620.0f)
			.MaxDesiredHeight(680.0f)
			[
				Outline(ContentBox.ToSharedRef(), FMargin(26.0f))
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(0.0f, 0.0f, 0.0f, 118.0f)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return bInteractionPromptVisible && !bInteractionOpen
					? EVisibility::HitTestInvisible : EVisibility::Collapsed;
			})
			[
				Outline(
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("E  与商人交互")))
					.Font(Font(18, true))
					.ColorAndOpacity(Ink),
					FMargin(18.0f, 10.0f))
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Top)
		.Padding(24.0f, 28.0f, 24.0f, 0.0f)
		[
			SAssignNew(Toast, SMatterFluxToast)
		];
}

void UMatterFluxInteractionWidget::ReleaseSlateResources(
	const bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	ContentBox.Reset();
	Toast.Reset();
	IconBrushes.Reset();
}

void UMatterFluxInteractionWidget::ShowDialogueNode(const FName NodeId)
{
	CurrentNodeId = NodeId;
	ActiveShopId = NAME_None;
	ActiveShopCategoryId = NAME_None;
	if (Toast.IsValid()) Toast->Dismiss();
	RefreshContent();
}

void UMatterFluxInteractionWidget::ShowShop(const FName ShopId)
{
	ActiveShopId = ShopId;
	ActiveShopCategoryId = NAME_None;
	if (Toast.IsValid()) Toast->Dismiss();
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

FString UMatterFluxInteractionWidget::ResolveContentIconKey(
	const FName ContentId) const
{
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid()) return FString();
	if (const FMatterFluxItemDefinition* Item = Registry->Items.Find(ContentId))
		return Item->Icon;
	if (const FMatterFluxSpellDefinition* Spell = Registry->Spells.Find(ContentId))
		return Spell->Icon;
	if (const FMatterFluxWandDefinition* Wand = Registry->Wands.Find(ContentId))
		return Wand->Icon;
	return FString();
}

const FSlateBrush* UMatterFluxInteractionWidget::GetIconBrush(
	const FString& IconKey)
{
	if (IconKey.IsEmpty()) return nullptr;
	if (const TSharedPtr<FSlateDynamicImageBrush>* Existing =
		IconBrushes.Find(IconKey))
	{
		return Existing->Get();
	}

	FString IconPath;
	if (!FMatterFluxMagicIconResolver::TryResolveIconPath(IconKey, IconPath))
	{
		return nullptr;
	}
	TSharedPtr<FSlateDynamicImageBrush> Brush =
		MakeShared<FSlateDynamicImageBrush>(
			FName(*IconPath), FVector2D(72.0f, 72.0f));
	IconBrushes.Add(IconKey, Brush);
	return Brush.Get();
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
			if (!Shop->Categories.IsEmpty())
			{
				TSharedRef<SHorizontalBox> CategoryTabs = SNew(SHorizontalBox);
				const auto AddCategoryTab = [this, &CategoryTabs](
					const FName CategoryId,
					const FString& Label)
				{
					const bool bSelected = ActiveShopCategoryId == CategoryId;
					CategoryTabs->AddSlot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						Outline(
							SNew(SButton)
							.ButtonStyle(&FlatButtonStyle())
							.IsEnabled(!bSelected)
							.ContentPadding(FMargin(12.0f, 6.0f))
							.OnClicked_Lambda([this, CategoryId]()
							{
								ActiveShopCategoryId = CategoryId;
								RefreshContent();
								return FReply::Handled();
							})
							[
								SNew(STextBlock)
								.Text(FText::FromString(bSelected
									? FString::Printf(TEXT("● %s"), *Label)
									: Label))
								.Font(Font(14, true))
								.ColorAndOpacity(Ink)
							],
							FMargin(0.0f))
					];
				};
				AddCategoryTab(NAME_None, TEXT("全部"));
				for (const FMatterFluxShopCategoryDefinition& Category
					: Shop->Categories)
				{
					AddCategoryTab(Category.Id, Category.DisplayName);
				}
				ContentBox->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 12.0f)
				[
					SNew(SScrollBox)
					.Orientation(Orient_Horizontal)
					+ SScrollBox::Slot()[CategoryTabs]
				];
			}
			TSharedRef<SUniformGridPanel> OfferGrid =
				SNew(SUniformGridPanel)
				.SlotPadding(FMargin(6.0f))
				.MinDesiredSlotWidth(168.0f)
				.MinDesiredSlotHeight(218.0f);
			int32 VisibleOfferIndex = 0;
			for (int32 Index = 0; Index < Shop->Offers.Num(); ++Index)
			{
				const FMatterFluxShopOfferDefinition& Offer = Shop->Offers[Index];
				if (!ActiveShopCategoryId.IsNone()
					&& Offer.CategoryId != ActiveShopCategoryId)
				{
					continue;
				}
				const int32 Remaining = KnownRemainingPurchases.Contains(Index)
					? KnownRemainingPurchases[Index] : Offer.PurchaseLimit;
				FString ProductName = ResolveContentName(Offer.ProductId);
				if (Offer.ProductCount > 1)
				{
					ProductName += FString::Printf(
						TEXT(" ×%d"), Offer.ProductCount);
				}
				const FString PriceLabel = FString::Printf(
					TEXT("%d %s"),
					Offer.CostCount,
					*ResolveContentName(Offer.CostItemId));
				const FString StockLabel = Remaining < 0
					? TEXT("剩余 ∞")
					: Remaining == 0
						? TEXT("剩余 0 · 售罄")
						: FString::Printf(TEXT("剩余 %d"), Remaining);

				const FString DefaultProductIcon =
					Offer.ProductKind == EMatterFluxShopProductKind::Item
						? TEXT("paper/default_item")
						: Offer.ProductKind == EMatterFluxShopProductKind::Spell
							? TEXT("paper/default")
							: TEXT("wand_default");
				const FSlateBrush* ProductIcon = GetIconBrush(
					ResolveContentIconKey(Offer.ProductId));
				if (!ProductIcon)
				{
					ProductIcon = GetIconBrush(DefaultProductIcon);
				}
				const FSlateBrush* CostIcon = GetIconBrush(
					ResolveContentIconKey(Offer.CostItemId));
				if (!CostIcon)
				{
					CostIcon = GetIconBrush(TEXT("paper/coin"));
				}

				TSharedRef<SWidget> ProductIconWidget = ProductIcon
					? StaticCastSharedRef<SWidget>(
						SNew(SImage).Image(ProductIcon))
					: StaticCastSharedRef<SWidget>(
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("?")))
						.Font(Font(32, true))
						.ColorAndOpacity(Ink));
				TSharedRef<SWidget> CostIconWidget = CostIcon
					? StaticCastSharedRef<SWidget>(
						SNew(SImage).Image(CostIcon))
					: StaticCastSharedRef<SWidget>(SNew(SBox));

				OfferGrid->AddSlot(
					VisibleOfferIndex % 3,
					VisibleOfferIndex / 3)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					Outline(
						SNew(SButton)
						.ButtonStyle(&FlatButtonStyle())
						.IsEnabled(Remaining != 0)
						.ContentPadding(FMargin(10.0f, 9.0f))
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
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Center)
							.Padding(0.0f, 1.0f, 0.0f, 8.0f)
							[
								SNew(SBox)
								.WidthOverride(72.0f)
								.HeightOverride(72.0f)
								.HAlign(HAlign_Center)
								.VAlign(VAlign_Center)
								[ProductIconWidget]
							]
							+ SVerticalBox::Slot()
							.FillHeight(1.0f)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.Padding(2.0f, 0.0f, 2.0f, 7.0f)
							[
								SNew(STextBlock)
								.Text(FText::FromString(ProductName))
								.Font(Font(16, true))
								.ColorAndOpacity(Ink)
								.Justification(ETextJustify::Center)
								.AutoWrapText(true)
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Center)
							.Padding(0.0f, 0.0f, 0.0f, 6.0f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								[
									SNew(SBox)
									.WidthOverride(20.0f)
									.HeightOverride(20.0f)
									[CostIconWidget]
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(5.0f, 0.0f, 0.0f, 0.0f)
								[
									SNew(STextBlock)
									.Text(FText::FromString(PriceLabel))
									.Font(Font(14, true))
									.ColorAndOpacity(Ink)
								]
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Text(FText::FromString(StockLabel))
								.Font(Font(13))
								.ColorAndOpacity(Remaining == 0 ? Muted : Ink)
							]
						], FMargin(0.0f))
				];
				++VisibleOfferIndex;
			}
			// The catalog is the flexible region. Header, tabs and the exit button
			// keep their authored height while this slot takes the remaining frame.
			ContentBox->AddSlot().FillHeight(1.0f)[
				SNew(SBox).MaxDesiredHeight(470.0f)[
					SNew(SScrollBox) + SScrollBox::Slot()[OfferGrid]]];
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
