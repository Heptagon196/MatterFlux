#pragma once

#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"

/**
 * MatterFlux's shared monochrome progress indicator.
 *
 * A real percentage draws a square black fill on a white track. An unset
 * percentage uses Slate's marquee brush for operations such as disk IO that
 * do not expose measurable progress. The explicit keyline keeps both modes
 * consistent with the rest of the shell and magic workbench.
 */
class SMatterFluxProgressBar final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMatterFluxProgressBar)
		: _Percent(TOptional<float>())
		, _Height(18.0f)
	{}
		SLATE_ATTRIBUTE(TOptional<float>, Percent)
		SLATE_ARGUMENT(float, Height)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		const FSlateBrush* SolidBrush =
			FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox"));
		ChildSlot
		[
			SNew(SBox)
			.HeightOverride(FMath::Max(InArgs._Height, 6.0f))
			[
				SNew(SBorder)
				.BorderImage(SolidBrush)
				.BorderBackgroundColor(FLinearColor::Black)
				.Padding(2.0f)
				[
					SNew(SProgressBar)
					.Percent(InArgs._Percent)
					.BarFillStyle(EProgressBarFillStyle::Scale)
					.BorderPadding(FVector2D::ZeroVector)
					.BackgroundImage(SolidBrush)
					.FillImage(SolidBrush)
					.FillColorAndOpacity(FLinearColor::Black)
				]
			]
		];
	}
};
