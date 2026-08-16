#pragma once

#include "Widgets/SCompoundWidget.h"

class UMatterFluxGameUserSettings;

/**
 * The single runtime presentation and interaction seam for game settings.
 *
 * Shell and workbench callers only embed this widget. Reading, previewing,
 * applying, saving, refreshing, control choice, and monochrome styling stay
 * local to the panel so the two entry points cannot drift apart again.
 */
class SMatterFluxSettingsPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMatterFluxSettingsPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	void Refresh();
	void ApplyAndSave(
		TFunction<void(UMatterFluxGameUserSettings&)> Edit) const;

	TSharedRef<SWidget> BuildPanel();
	TSharedRef<SWidget> BuildRow(
		const FString& Label,
		const TSharedRef<SWidget>& Control,
		const FString& ToolTip) const;
	TSharedRef<SWidget> BuildActionButton(
		const FString& Caption,
		TFunction<void()> Action,
		const FString& ToolTip) const;
	TSharedRef<SWidget> BuildDropdown(
		const FString& CurrentLabel,
		TArray<TPair<FString, TFunction<void()>>> Options,
		const FString& ToolTip);
	TSharedRef<SWidget> BuildSlider(
		TFunction<float()> GetValue,
		TFunction<FText()> GetValueText,
		TFunction<void(float)> PreviewValue,
		TFunction<void(float)> CommitStep,
		TFunction<void()> SaveValue,
		float MinValue,
		float MaxValue,
		float Step,
		const FString& ToolTip) const;
	TSharedRef<SWidget> OutlinedPanel(
		const TSharedRef<SWidget>& Content,
		const FMargin& Padding = FMargin(0.0f),
		const FLinearColor& Fill = FLinearColor::White) const;
};
