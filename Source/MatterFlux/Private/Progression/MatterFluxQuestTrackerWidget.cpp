#include "Progression/MatterFluxQuestTrackerWidget.h"

#include "Game/MatterFluxPlayerState.h"
#include "IMatterFluxScriptRuntime.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "UI/MatterFluxPaperStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace MatterFluxProgressionUI
{
	using MatterFlux::UI::Paper::Font;

	class SMatterFluxQuestTracker : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SMatterFluxQuestTracker) {}
			SLATE_ARGUMENT(TWeakObjectPtr<UMatterFluxQuestTrackerWidget>, Owner)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args)
		{
			Owner = Args._Owner;
			Refresh();
		}

		void Refresh()
		{
			ChildSlot.AttachWidget(Build());
		}

	private:
		TSharedRef<SWidget> Build()
		{
			UMatterFluxQuestTrackerWidget* Widget = Owner.Get();
			UMatterFluxProgressionComponent* Progression = Widget
				? Widget->ResolveProgression() : nullptr;
			const FMatterFluxContentRegistryPtr Registry =
				IMatterFluxScriptRuntime::Get().GetActiveRegistry();
			const FMatterFluxQuestDefinition* Definition =
				Progression && Registry.IsValid()
				? Registry->Quests.Find(Progression->GetSelectedQuest()) : nullptr;
			const FMatterFluxQuestState* State = Definition && Progression
				? Progression->FindQuestState(Definition->Id) : nullptr;
			if (!Definition || !State
				|| State->Status != EMatterFluxQuestRuntimeStatus::Active)
			{
				return SNew(SOverlay);
			}

			TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);
			Lines->AddSlot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Definition->DisplayName))
				.Font(Font(13, true))
				.ColorAndOpacity(FLinearColor::Black)
			];
			if (Definition->Subquests.IsEmpty())
			{
				const FString Counter = Definition->TargetCount > 1
					? FString::Printf(TEXT("  %d / %d"),
						State->Progress, Definition->TargetCount) : FString();
				Lines->AddSlot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Definition->Description + Counter))
					.Font(Font(10))
					.ColorAndOpacity(FLinearColor::Black)
					.AutoWrapText(true)
				];
			}
			else
			{
				for (const FName ChildId : Definition->Subquests)
				{
					const FMatterFluxQuestDefinition* Child = Registry->Quests.Find(ChildId);
					const FMatterFluxQuestState* ChildState =
						Progression->FindQuestState(ChildId);
					if (!Child || !ChildState
						|| ChildState->Status == EMatterFluxQuestRuntimeStatus::Hidden)
					{
						continue;
					}
					const bool bComplete = ChildState->Status
						== EMatterFluxQuestRuntimeStatus::Completed;
					const FString Counter = Child->TargetCount > 1 && !bComplete
						? FString::Printf(TEXT("  %d / %d"),
							ChildState->Progress, Child->TargetCount) : FString();
					Lines->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(
							TEXT("%s  %s%s"), bComplete ? TEXT("■") : TEXT("□"),
							*Child->Description, *Counter)))
						.Font(Font(10))
						.ColorAndOpacity(bComplete
							? FLinearColor(0.35f, 0.35f, 0.35f) : FLinearColor::Black)
						.AutoWrapText(true)
					];
				}
			}

			return SNew(SOverlay)
			+ SOverlay::Slot()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Top)
			.Padding(24.0f, 76.0f)
			[
				MatterFlux::UI::Paper::Outline(
					SNew(SBox)
					.WidthOverride(310.0f)
					[Lines],
					FMargin(14.0f, 10.0f),
					FLinearColor(1.0f, 1.0f, 1.0f, 0.92f))
			];
		}

		TWeakObjectPtr<UMatterFluxQuestTrackerWidget> Owner;
	};
}

void UMatterFluxQuestTrackerWidget::InitializeForPlayer(
	APlayerController* InPlayerController)
{
	PlayerController = InPlayerController;
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	BindProgression();
	RefreshTracker();
}

void UMatterFluxQuestTrackerWidget::SetSuppressedByFrontEnd(
	const bool bSuppressed)
{
	SetVisibility(bSuppressed
		? ESlateVisibility::Collapsed
		: ESlateVisibility::SelfHitTestInvisible);
}

UMatterFluxProgressionComponent*
UMatterFluxQuestTrackerWidget::ResolveProgression() const
{
	const AMatterFluxPlayerState* PlayerState = PlayerController
		? PlayerController->GetPlayerState<AMatterFluxPlayerState>() : nullptr;
	return PlayerState ? PlayerState->GetProgression() : nullptr;
}

void UMatterFluxQuestTrackerWidget::RefreshTracker()
{
	BindProgression();
	if (Tracker.IsValid()) Tracker->Refresh();
}

void UMatterFluxQuestTrackerWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (!ContentReloadedHandle.IsValid())
	{
		ContentReloadedHandle =
			IMatterFluxScriptRuntime::Get().OnContentReloaded().AddWeakLambda(
				this, [this](const FMatterFluxContentRegistryPtr)
				{
					RefreshTracker();
				});
	}
	BindProgression();
	RefreshTracker();
}

void UMatterFluxQuestTrackerWidget::NativeDestruct()
{
	if (ContentReloadedHandle.IsValid() && IMatterFluxScriptRuntime::IsAvailable())
	{
		IMatterFluxScriptRuntime::Get().OnContentReloaded().Remove(
			ContentReloadedHandle);
	}
	ContentReloadedHandle.Reset();
	if (BoundProgression.IsValid() && ProgressionChangedHandle.IsValid())
	{
		BoundProgression->OnProgressionChanged().Remove(ProgressionChangedHandle);
	}
	ProgressionChangedHandle.Reset();
	BoundProgression.Reset();
	Super::NativeDestruct();
}

TSharedRef<SWidget> UMatterFluxQuestTrackerWidget::RebuildWidget()
{
	Tracker = SNew(MatterFluxProgressionUI::SMatterFluxQuestTracker)
		.Owner(this);
	return Tracker.ToSharedRef();
}

void UMatterFluxQuestTrackerWidget::ReleaseSlateResources(
	const bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	Tracker.Reset();
}

void UMatterFluxQuestTrackerWidget::BindProgression()
{
	UMatterFluxProgressionComponent* Progression = ResolveProgression();
	if (BoundProgression.Get() == Progression) return;
	if (BoundProgression.IsValid() && ProgressionChangedHandle.IsValid())
	{
		BoundProgression->OnProgressionChanged().Remove(ProgressionChangedHandle);
	}
	ProgressionChangedHandle.Reset();
	BoundProgression = Progression;
	if (Progression)
	{
		ProgressionChangedHandle = Progression->OnProgressionChanged().AddUObject(
			this, &UMatterFluxQuestTrackerWidget::RefreshTracker);
	}
}
