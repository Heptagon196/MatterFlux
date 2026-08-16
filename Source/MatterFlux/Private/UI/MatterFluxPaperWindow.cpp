#include "UI/MatterFluxPaperWindow.h"

#include "Styling/CoreStyle.h"
#include "UI/MatterFluxPaperStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SMatterFluxPaperWindow::Construct(const FArguments& InArgs)
{
	using namespace MatterFlux::UI::Paper;

	ChildSlot
	[
		Outline(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				InArgs._Header.Widget
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(InArgs._ContentPadding)
			[
				InArgs._Content.Widget
			])
	];
}

void SMatterFluxPaperTab::Construct(const FArguments& InArgs)
{
	using namespace MatterFlux::UI::Paper;

	const FLinearColor Fill = InArgs._bSelected ? Ink : Surface;
	const FLinearColor TextColor = InArgs._bSelected ? Surface : Ink;
	TFunction<FReply()> OnClicked = InArgs._OnClicked;

	ChildSlot
	[
		SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), TEXT("NoBorder"))
		.ContentPadding(0.0f)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.ToolTipText(InArgs._ToolTip)
		.OnClicked_Lambda([OnClicked = MoveTemp(OnClicked)]() mutable
		{
			return OnClicked ? OnClicked() : FReply::Handled();
		})
		[
			Outline(
				SNew(STextBlock)
				.Text(InArgs._Label)
				.Font(Font(14, true))
				.Justification(ETextJustify::Center)
				.ColorAndOpacity(TextColor),
				InArgs._Padding,
				Fill)
		]
	];
}
