#include "UI/MatterFluxSettingsPanel.h"

#include "Framework/Application/SlateApplication.h"
#include "Settings/MatterFluxGameUserSettings.h"
#include "Styling/CoreStyle.h"
#include "UI/MatterFluxPaperStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace MatterFluxSettingsUI
{
	using namespace MatterFlux::UI::Paper;

	FString QualityLabel(const int32 Level)
	{
		switch (Level)
		{
		case 0: return TEXT("低");
		case 1: return TEXT("中");
		case 2: return TEXT("高");
		case 3: return TEXT("最高");
		default: return TEXT("自定义");
		}
	}

	FString WindowLabel(const EWindowMode::Type Mode)
	{
		switch (Mode)
		{
		case EWindowMode::Fullscreen: return TEXT("全屏");
		case EWindowMode::WindowedFullscreen: return TEXT("无边框");
		default: return TEXT("窗口");
		}
	}
}

using namespace MatterFluxSettingsUI;

void SMatterFluxSettingsPanel::Construct(const FArguments& InArgs)
{
	(void)InArgs;
	Refresh();
}

void SMatterFluxSettingsPanel::Refresh()
{
	ChildSlot.AttachWidget(BuildPanel());
	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
}

void SMatterFluxSettingsPanel::ApplyAndSave(
	TFunction<void(UMatterFluxGameUserSettings&)> Edit) const
{
	if (UMatterFluxGameUserSettings* Settings =
		UMatterFluxGameUserSettings::Get())
	{
		Edit(*Settings);
		Settings->ApplyAndSave();
	}
}

TSharedRef<SWidget> SMatterFluxSettingsPanel::OutlinedPanel(
	const TSharedRef<SWidget>& Content,
	const FMargin& Padding,
	const FLinearColor& Fill) const
{
	return MatterFlux::UI::Paper::Outline(Content, Padding, Fill);
}

TSharedRef<SWidget> SMatterFluxSettingsPanel::BuildRow(
	const FString& Label,
	const TSharedRef<SWidget>& Control,
	const FString& ToolTip) const
{
	return OutlinedPanel(
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Label))
			.ToolTipText(FText::FromString(ToolTip))
			.Font(Font(13, true))
			.ColorAndOpacity(Ink)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(430.0f)
			.HAlign(HAlign_Right)
			[Control]
		],
		FMargin(14.0f, 9.0f));
}

TSharedRef<SWidget> SMatterFluxSettingsPanel::BuildActionButton(
	const FString& Caption,
	TFunction<void()> Action,
	const FString& ToolTip) const
{
	return SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), TEXT("NoBorder"))
		.ContentPadding(0.0f)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.ToolTipText(FText::FromString(ToolTip))
		.OnClicked_Lambda([Action = MoveTemp(Action)]() mutable
		{
			Action();
			return FReply::Handled();
		})
		[
			OutlinedPanel(
				SNew(STextBlock)
				.Text(FText::FromString(Caption))
				.Font(Font(13, true))
				.Justification(ETextJustify::Center)
				.ColorAndOpacity(Ink),
				FMargin(11.0f, 6.0f))
		];
}

TSharedRef<SWidget> SMatterFluxSettingsPanel::BuildDropdown(
	const FString& CurrentLabel,
	TArray<TPair<FString, TFunction<void()>>> Options,
	const FString& ToolTip)
{
	TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
	for (TPair<FString, TFunction<void()>>& Option : Options)
	{
		const FString Label = MoveTemp(Option.Key);
		TFunction<void()> Action = MoveTemp(Option.Value);
		Menu->AddSlot().AutoHeight()
		[
			SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), TEXT("NoBorder"))
			.ContentPadding(0.0f)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.OnClicked_Lambda([this, Action = MoveTemp(Action)]() mutable
			{
				Action();
				FSlateApplication::Get().DismissAllMenus();
				Refresh();
				return FReply::Handled();
			})
			[
				SNew(SBox)
				.WidthOverride(180.0f)
				.HeightOverride(36.0f)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.Font(Font(12))
					.ColorAndOpacity(Ink)
					.Justification(ETextJustify::Center)
				]
			]
		];
	}

	return SNew(SBox)
		.WidthOverride(180.0f)
		.HeightOverride(36.0f)
		[
			SNew(SComboButton)
			.ButtonStyle(FCoreStyle::Get(), TEXT("NoBorder"))
			.ContentPadding(0.0f)
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Center)
			.HasDownArrow(false)
			.ToolTipText(FText::FromString(ToolTip))
			.MenuContent()
			[
				OutlinedPanel(Menu, FMargin(2.0f))
			]
			.ButtonContent()
			[
				OutlinedPanel(
					SNew(SOverlay)
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(CurrentLabel))
						.Font(Font(12))
						.ColorAndOpacity(Ink)
						.Justification(ETextJustify::Center)
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("▼")))
						.Font(Font(9, true))
						.ColorAndOpacity(Ink)
					],
					FMargin(12.0f, 7.0f))
			]
		];
}

TSharedRef<SWidget> SMatterFluxSettingsPanel::BuildSlider(
	TFunction<float()> GetValue,
	TFunction<FText()> GetValueText,
	TFunction<void(float)> PreviewValue,
	TFunction<void(float)> CommitStep,
	TFunction<void()> SaveValue,
	const float MinValue,
	const float MaxValue,
	const float Step,
	const FString& ToolTip) const
{
	return SNew(SBox).WidthOverride(430.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			BuildActionButton(TEXT("−"),
				[GetValue, CommitStep, Step]()
				{
					CommitStep(GetValue() - Step);
				}, ToolTip)
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		.Padding(12.0f, 0.0f)
		[
			SNew(SSlider)
			.Value_Lambda([GetValue]() { return GetValue(); })
			.MinValue(MinValue)
			.MaxValue(MaxValue)
			.StepSize(Step)
			.MouseUsesStep(false)
			.SliderBarColor(Ink)
			.SliderHandleColor(Ink)
			.ToolTipText(FText::FromString(ToolTip))
			.OnValueChanged_Lambda(
				[PreviewValue](const float Value) { PreviewValue(Value); })
			.OnMouseCaptureEnd_Lambda([SaveValue]() { SaveValue(); })
			.OnControllerCaptureEnd_Lambda([SaveValue]() { SaveValue(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(64.0f)
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([GetValueText]() { return GetValueText(); })
				.Font(Font(12))
				.ColorAndOpacity(Ink)
				.Justification(ETextJustify::Center)
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			BuildActionButton(TEXT("+"),
				[GetValue, CommitStep, Step]()
				{
					CommitStep(GetValue() + Step);
				}, ToolTip)
		]
	];
}

TSharedRef<SWidget> SMatterFluxSettingsPanel::BuildPanel()
{
	UMatterFluxGameUserSettings* Settings =
		UMatterFluxGameUserSettings::Get();
	if (!Settings)
	{
		return OutlinedPanel(
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("暂时无法读取游戏设置。")))
			.Font(Font(14, true))
			.ColorAndOpacity(Ink),
			FMargin(20.0f));
	}

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
	[
		BuildRow(
			TEXT("画面质量"),
			BuildDropdown(
				QualityLabel(Settings->GetOverallScalabilityLevel()),
				{
					{TEXT("低"), [this]()
						{ ApplyAndSave([](UMatterFluxGameUserSettings& Value)
							{ Value.SetOverallScalabilityLevel(0); }); }},
					{TEXT("中"), [this]()
						{ ApplyAndSave([](UMatterFluxGameUserSettings& Value)
							{ Value.SetOverallScalabilityLevel(1); }); }},
					{TEXT("高"), [this]()
						{ ApplyAndSave([](UMatterFluxGameUserSettings& Value)
							{ Value.SetOverallScalabilityLevel(2); }); }},
					{TEXT("最高"), [this]()
						{ ApplyAndSave([](UMatterFluxGameUserSettings& Value)
							{ Value.SetOverallScalabilityLevel(3); }); }}
				},
				TEXT("整体调整阴影、材质、后处理和视距质量。")),
			TEXT("整体调整阴影、材质、后处理和视距质量。"))
	];
	Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
	[
		BuildRow(
			TEXT("窗口模式"),
			BuildDropdown(
				WindowLabel(Settings->GetFullscreenMode()),
				{
					{TEXT("窗口"), [this]()
						{ ApplyAndSave([](UMatterFluxGameUserSettings& Value)
							{ Value.SetFullscreenMode(EWindowMode::Windowed); }); }},
					{TEXT("无边框"), [this]()
						{ ApplyAndSave([](UMatterFluxGameUserSettings& Value)
							{ Value.SetFullscreenMode(
								EWindowMode::WindowedFullscreen); }); }},
					{TEXT("全屏"), [this]()
						{ ApplyAndSave([](UMatterFluxGameUserSettings& Value)
							{ Value.SetFullscreenMode(EWindowMode::Fullscreen); }); }}
				},
				TEXT("在窗口、无边框和全屏模式间切换。")),
			TEXT("在窗口、无边框和全屏模式间切换。"))
	];
	Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
	[
		BuildRow(
			TEXT("垂直同步"),
			SNew(SBox)
			.HeightOverride(36.0f)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.Style(FCoreStyle::Get(), TEXT("TransparentCheckBox"))
				.IsChecked_Lambda([]()
				{
					const UMatterFluxGameUserSettings* Current =
						UMatterFluxGameUserSettings::Get();
					return Current && Current->IsVSyncEnabled()
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
				{
					ApplyAndSave([State](UMatterFluxGameUserSettings& Value)
					{
						Value.SetVSyncEnabled(
							State == ECheckBoxState::Checked);
					});
				})
				.ToolTipText(FText::FromString(
					TEXT("减少画面撕裂；可能略微增加输入延迟。")))
				[
					OutlinedPanel(
						SNew(STextBlock)
						.Text_Lambda([]()
						{
							const UMatterFluxGameUserSettings* Current =
								UMatterFluxGameUserSettings::Get();
							return FText::FromString(
								Current && Current->IsVSyncEnabled()
									? TEXT("■  启用") : TEXT("□  启用"));
						})
						.Font(Font(12))
						.Justification(ETextJustify::Center)
						.ColorAndOpacity(Ink),
						FMargin(12.0f, 7.0f))
				]
			],
			TEXT("减少画面撕裂；可能略微增加输入延迟。"))
	];
	Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
	[
		BuildRow(
			TEXT("主音量"),
			BuildSlider(
				[]()
				{
					const UMatterFluxGameUserSettings* Value =
						UMatterFluxGameUserSettings::Get();
					return Value ? Value->GetMasterVolume() : 0.0f;
				},
				[]()
				{
					const UMatterFluxGameUserSettings* Value =
						UMatterFluxGameUserSettings::Get();
					return FText::FromString(FString::Printf(TEXT("%d%%"),
						FMath::RoundToInt((Value
							? Value->GetMasterVolume() : 0.0f) * 100.0f)));
				},
				[](const float NewValue)
				{
					if (UMatterFluxGameUserSettings* Value =
						UMatterFluxGameUserSettings::Get())
					{
						Value->SetMasterVolume(NewValue);
						Value->ApplyNonResolutionSettings();
					}
				},
				[this](const float NewValue)
				{
					ApplyAndSave([NewValue](UMatterFluxGameUserSettings& Value)
						{ Value.SetMasterVolume(NewValue); });
				},
				[]()
				{
					if (UMatterFluxGameUserSettings* Value =
						UMatterFluxGameUserSettings::Get())
					{
						Value->SaveSettings();
					}
				},
				0.0f, 1.0f, 0.1f,
				TEXT("调整所有游戏声音的总体音量。")),
			TEXT("调整所有游戏声音的总体音量。"))
	];
	Rows->AddSlot().AutoHeight()
	[
		BuildRow(
			TEXT("界面缩放"),
			BuildSlider(
				[]()
				{
					const UMatterFluxGameUserSettings* Value =
						UMatterFluxGameUserSettings::Get();
					return Value ? Value->GetInterfaceScale() : 1.0f;
				},
				[]()
				{
					const UMatterFluxGameUserSettings* Value =
						UMatterFluxGameUserSettings::Get();
					return FText::FromString(FString::Printf(TEXT("%d%%"),
						FMath::RoundToInt((Value
							? Value->GetInterfaceScale() : 1.0f) * 100.0f)));
				},
				[](const float NewValue)
				{
					if (UMatterFluxGameUserSettings* Value =
						UMatterFluxGameUserSettings::Get())
					{
						Value->SetInterfaceScale(NewValue);
						Value->ApplyNonResolutionSettings();
					}
				},
				[this](const float NewValue)
				{
					ApplyAndSave([NewValue](UMatterFluxGameUserSettings& Value)
						{ Value.SetInterfaceScale(NewValue); });
				},
				[]()
				{
					if (UMatterFluxGameUserSettings* Value =
						UMatterFluxGameUserSettings::Get())
					{
						Value->SaveSettings();
					}
				},
				0.8f, 1.25f, 0.05f,
				TEXT("缩放游戏内界面，不影响编辑器本身。")),
			TEXT("缩放游戏内界面，不影响编辑器本身。"))
	];

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(6.0f, 4.0f, 6.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("设置")))
			.Font(Font(14, true))
			.ColorAndOpacity(Ink)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(6.0f, 4.0f, 6.0f, 16.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(
				TEXT("改动会立即应用并保存。悬停条目可查看说明。")))
			.Font(Font(11))
			.ColorAndOpacity(Muted)
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[Rows]
		];
}
