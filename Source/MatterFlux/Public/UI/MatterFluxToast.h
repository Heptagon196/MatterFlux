#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;

/** Semantic styling for a transient, non-modal viewport notification. */
enum class EMatterFluxToastTone : uint8
{
	Success,
	Error,
	Info
};

/**
 * Reusable paper-style toast. It lives in an overlay, never participates in
 * the layout of the screen that triggered it, and fades itself out.
 */
class MATTERFLUX_API SMatterFluxToast : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMatterFluxToast) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void Show(
		const FText& Message,
		EMatterFluxToastTone Tone = EMatterFluxToastTone::Info,
		float DisplaySeconds = 2.8f);
	void Dismiss();

	bool IsShowing() const { return bShowing; }
	const FText& GetMessage() const { return CurrentMessage; }
	EMatterFluxToastTone GetTone() const { return CurrentTone; }

private:
	EActiveTimerReturnType UpdateToast(
		double CurrentTime,
		float DeltaTime);
	float CalculateOpacity() const;

	TSharedPtr<STextBlock> BadgeText;
	TSharedPtr<STextBlock> MessageText;
	TWeakPtr<FActiveTimerHandle> ActiveTimerHandle;
	FText CurrentMessage;
	EMatterFluxToastTone CurrentTone = EMatterFluxToastTone::Info;
	float ElapsedSeconds = 0.0f;
	float LifetimeSeconds = 2.8f;
	bool bShowing = false;
};
