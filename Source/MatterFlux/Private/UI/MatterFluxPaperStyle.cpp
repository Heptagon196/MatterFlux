#include "UI/MatterFluxPaperStyle.h"

#include "Brushes/SlateColorBrush.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"

namespace MatterFlux::UI::Paper
{
	FSlateFontInfo Font(const int32 Size, const bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(
			bBold ? TEXT("Bold") : TEXT("Regular"),
			Size);
	}

	const FButtonStyle& FlatButtonStyle()
	{
		static FSlateColorBrush NormalBrush(Surface);
		static FSlateColorBrush HoverBrush(
			FLinearColor(0.88f, 0.88f, 0.88f, 1.0f));
		static FSlateColorBrush PressedBrush(
			FLinearColor(0.72f, 0.72f, 0.72f, 1.0f));
		static FSlateColorBrush DisabledBrush(
			FLinearColor(0.94f, 0.94f, 0.94f, 1.0f));
		static FButtonStyle Style = FButtonStyle()
			.SetNormal(NormalBrush)
			.SetHovered(HoverBrush)
			.SetPressed(PressedBrush)
			.SetDisabled(DisabledBrush)
			.SetNormalPadding(FMargin(0.0f))
			.SetPressedPadding(FMargin(0.0f));
		return Style;
	}

	TSharedRef<SWidget> Outline(
		const TSharedRef<SWidget>& Content,
		const FMargin& Padding,
		const FLinearColor& Fill)
	{
		return SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
			.BorderBackgroundColor(Ink)
			.Padding(Keyline)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
				.BorderBackgroundColor(Fill)
				.Padding(Padding)
				[Content]
			];
	}
}
