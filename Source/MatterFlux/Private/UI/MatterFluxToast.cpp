#include "UI/MatterFluxToast.h"

#include "UI/MatterFluxPaperStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

using namespace MatterFlux::UI::Paper;

void SMatterFluxToast::Construct(const FArguments& InArgs)
{
	SetVisibility(EVisibility::Collapsed);
	ChildSlot
	[
		SNew(SBox)
		.MaxDesiredWidth(480.0f)
		[
			Outline(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SAssignNew(BadgeText, STextBlock)
					.Font(Font(18, true))
					.ColorAndOpacity(Ink)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(MessageText, STextBlock)
					.Font(Font(16, true))
					.ColorAndOpacity(Ink)
					.AutoWrapText(true)
				],
				FMargin(18.0f, 12.0f))
		]
	];
}

void SMatterFluxToast::Show(
	const FText& Message,
	const EMatterFluxToastTone Tone,
	const float DisplaySeconds)
{
	constexpr float FadeInSeconds = 0.16f;
	constexpr float FadeOutSeconds = 0.28f;
	CurrentMessage = Message;
	CurrentTone = Tone;
	ElapsedSeconds = 0.0f;
	LifetimeSeconds = FMath::Max(
		DisplaySeconds,
		FadeInSeconds + FadeOutSeconds + 0.1f);
	bShowing = !Message.IsEmpty();
	if (!bShowing)
	{
		Dismiss();
		return;
	}

	if (MessageText.IsValid())
	{
		MessageText->SetText(Message);
	}
	if (BadgeText.IsValid())
	{
		BadgeText->SetText(FText::FromString(
			Tone == EMatterFluxToastTone::Success
				? TEXT("✓")
				: Tone == EMatterFluxToastTone::Error
					? TEXT("!")
					: TEXT("i")));
	}
	SetRenderOpacity(0.0f);
	SetVisibility(EVisibility::HitTestInvisible);
	if (!ActiveTimerHandle.IsValid())
	{
		ActiveTimerHandle = RegisterActiveTimer(
			0.0f,
			FWidgetActiveTimerDelegate::CreateSP(
				this,
				&SMatterFluxToast::UpdateToast));
	}
}

void SMatterFluxToast::Dismiss()
{
	bShowing = false;
	ElapsedSeconds = 0.0f;
	SetRenderOpacity(0.0f);
	SetVisibility(EVisibility::Collapsed);
}

EActiveTimerReturnType SMatterFluxToast::UpdateToast(
	const double CurrentTime,
	const float DeltaTime)
{
	(void)CurrentTime;
	if (!bShowing)
	{
		ActiveTimerHandle.Reset();
		return EActiveTimerReturnType::Stop;
	}

	ElapsedSeconds += DeltaTime;
	if (ElapsedSeconds >= LifetimeSeconds)
	{
		Dismiss();
		ActiveTimerHandle.Reset();
		return EActiveTimerReturnType::Stop;
	}
	SetRenderOpacity(CalculateOpacity());
	return EActiveTimerReturnType::Continue;
}

float SMatterFluxToast::CalculateOpacity() const
{
	constexpr float FadeInSeconds = 0.16f;
	constexpr float FadeOutSeconds = 0.28f;
	const float FadeInOpacity = FMath::Clamp(
		ElapsedSeconds / FadeInSeconds,
		0.0f,
		1.0f);
	const float FadeOutOpacity = FMath::Clamp(
		(LifetimeSeconds - ElapsedSeconds) / FadeOutSeconds,
		0.0f,
		1.0f);
	return FMath::Min(FadeInOpacity, FadeOutOpacity);
}
