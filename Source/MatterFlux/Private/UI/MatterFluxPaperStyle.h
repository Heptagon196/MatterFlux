#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"

class SWidget;
struct FButtonStyle;

namespace MatterFlux::UI::Paper
{
	inline const FLinearColor Ink = FLinearColor::Black;
	inline const FLinearColor Surface = FLinearColor::White;
	inline const FLinearColor Muted =
		FLinearColor(0.28f, 0.28f, 0.28f, 1.0f);
	inline constexpr float Keyline = 2.0f;

	FSlateFontInfo Font(int32 Size, bool bBold = false);
	const FButtonStyle& FlatButtonStyle();

	/** One canonical white surface with the project's black keyline. */
	TSharedRef<SWidget> Outline(
		const TSharedRef<SWidget>& Content,
		const FMargin& Padding = FMargin(0.0f),
		const FLinearColor& Fill = Surface);
}
