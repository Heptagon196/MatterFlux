#include "UI/MatterFluxPlayerStatusWidget.h"

#include "AbilitySystemComponent.h"
#include "Game/MatterFluxPlayerState.h"
#include "GameFramework/PlayerController.h"
#include "GAS/MatterFluxPlayerAttributeSet.h"
#include "IMatterFluxScriptRuntime.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Styling/CoreStyle.h"
#include "UI/MatterFluxPaperStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr float StatusWidth = 320.0f;
	constexpr float StatusRowHeight = 30.0f;
	constexpr float StatusRowGap = 6.0f;

	FString EquipmentSlotLabel(const int32 SlotIndex)
	{
		switch (SlotIndex)
		{
		case 0: return TEXT("左键");
		case 1: return TEXT("右键");
		case 2: return TEXT("Q");
		case 3: return TEXT("E");
		case 4: return TEXT("空格");
		default: return TEXT("?");
		}
	}

	float SafePercent(const float Value, const float Maximum)
	{
		return Maximum > UE_KINDA_SMALL_NUMBER
			? FMath::Clamp(Value / Maximum, 0.0f, 1.0f)
			: 0.0f;
	}
}

namespace MatterFluxPlayerStatusUI
{
	using MatterFlux::UI::Paper::Font;

	class SMatterFluxStatusRow final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SMatterFluxStatusRow)
			: _FillColor(FLinearColor::Black)
		{}
			SLATE_ARGUMENT(FLinearColor, FillColor)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args)
		{
			const FSlateBrush* SolidBrush =
				FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox"));
			ChildSlot
			[
				SNew(SBox)
				.WidthOverride(StatusWidth)
				.HeightOverride(StatusRowHeight)
				[
					SNew(SBorder)
					.BorderImage(SolidBrush)
					.BorderBackgroundColor(FLinearColor::Black)
					.Padding(2.0f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							SAssignNew(Progress, SProgressBar)
							.Percent(0.0f)
							.BarFillStyle(EProgressBarFillStyle::Scale)
							.BorderPadding(FVector2D::ZeroVector)
							.BackgroundImage(SolidBrush)
							.FillImage(SolidBrush)
							.FillColorAndOpacity(Args._FillColor)
						]
						+ SOverlay::Slot()
						.VAlign(VAlign_Center)
						.Padding(8.0f, 0.0f)
						[
							SAssignNew(Label, STextBlock)
							.Font(Font(13, true))
							.ColorAndOpacity(FLinearColor::Black)
						]
					]
				]
			];
		}

		void SetState(
			const FString& Name,
			const float Value,
			const float Maximum,
			const bool bShowValue)
		{
			if (Progress.IsValid())
			{
				Progress->SetPercent(TAttribute<TOptional<float>>(
					TOptional<float>(SafePercent(Value, Maximum))));
			}
			if (Label.IsValid())
			{
				Label->SetText(FText::FromString(bShowValue
					? FString::Printf(
						TEXT("%s    %.0f / %.0f"),
						*Name,
						Value,
						Maximum)
					: Name));
			}
		}

	private:
		TSharedPtr<SProgressBar> Progress;
		TSharedPtr<STextBlock> Label;
	};

	class SMatterFluxPlayerStatus final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SMatterFluxPlayerStatus) {}
			SLATE_ARGUMENT(
				TWeakObjectPtr<UMatterFluxPlayerStatusWidget>,
				Owner)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args)
		{
			Owner = Args._Owner;
			TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
			Rows->AddSlot().AutoHeight()
			[
				SAssignNew(HealthRow, SMatterFluxStatusRow)
				.FillColor(FLinearColor(0.88f, 0.20f, 0.18f, 1.0f))
			];
			for (int32 SlotIndex = 0;
				SlotIndex < UMatterFluxPlayerStatusWidget::WandSlotCount;
				++SlotIndex)
			{
				TSharedPtr<SMatterFluxStatusRow> Row;
				Rows->AddSlot()
				.AutoHeight()
				.Padding(0.0f, StatusRowGap, 0.0f, 0.0f)
				[
					SAssignNew(Row, SMatterFluxStatusRow)
					.FillColor(FLinearColor(0.53f, 0.81f, 0.98f, 1.0f))
				];
				WandRows.Add(Row);
			}

			ChildSlot
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				.HAlign(HAlign_Left)
				.VAlign(VAlign_Bottom)
				.Padding(24.0f)
				[
					MatterFlux::UI::Paper::Outline(
						Rows,
						FMargin(10.0f),
						FLinearColor(1.0f, 1.0f, 1.0f, 0.92f))
				]
			];
			Refresh();
		}

		void Refresh()
		{
			const UMatterFluxPlayerStatusWidget* Widget = Owner.Get();
			const APlayerController* Controller = Widget
				? Widget->GetOwningPlayer()
				: nullptr;
			const FMatterFluxPlayerStatusSnapshot Snapshot =
				UMatterFluxPlayerStatusWidget::BuildStatusSnapshot(
					Controller
						? Controller->GetPlayerState<AMatterFluxPlayerState>()
						: nullptr);
			if (HealthRow.IsValid())
			{
				HealthRow->SetState(
					TEXT("生命"),
					Snapshot.Health,
					Snapshot.MaxHealth,
					true);
			}
			for (int32 SlotIndex = 0;
				SlotIndex < WandRows.Num();
				++SlotIndex)
			{
				if (!Snapshot.Wands.IsValidIndex(SlotIndex)
					|| !WandRows[SlotIndex].IsValid())
				{
					continue;
				}
				const FMatterFluxPlayerStatusWandView& Wand =
					Snapshot.Wands[SlotIndex];
				WandRows[SlotIndex]->SetState(
					Wand.Label.ToString(),
					Wand.Mana,
					Wand.MaxMana,
					false);
			}
		}

	private:
		TWeakObjectPtr<UMatterFluxPlayerStatusWidget> Owner;
		TSharedPtr<SMatterFluxStatusRow> HealthRow;
		TArray<TSharedPtr<SMatterFluxStatusRow>> WandRows;
	};
}

FMatterFluxPlayerStatusSnapshot
UMatterFluxPlayerStatusWidget::BuildStatusSnapshot(
	const AMatterFluxPlayerState* PlayerState)
{
	FMatterFluxPlayerStatusSnapshot Snapshot;
	Snapshot.Wands.SetNum(WandSlotCount);
	const UMatterFluxPlayerAttributeSet* Attributes = PlayerState
		? PlayerState->GetPlayerAttributes()
		: nullptr;
	if (Attributes)
	{
		Snapshot.MaxHealth = FMath::Max(0.0f, Attributes->GetMaxHealth());
		Snapshot.Health = FMath::Clamp(
			Attributes->GetHealth(),
			0.0f,
			Snapshot.MaxHealth);
	}

	const UMatterFluxMagicInventoryComponent* Inventory = PlayerState
		? PlayerState->GetMagicInventory()
		: nullptr;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	for (int32 SlotIndex = 0; SlotIndex < WandSlotCount; ++SlotIndex)
	{
		FMatterFluxPlayerStatusWandView& View = Snapshot.Wands[SlotIndex];
		const FString InputLabel = EquipmentSlotLabel(SlotIndex);
		const FGuid WandId = Inventory
			? Inventory->GetEquippedWandId(SlotIndex)
			: FGuid();
		const FMatterFluxOwnedWand* Wand = Inventory && WandId.IsValid()
			? Inventory->FindWand(WandId)
			: nullptr;
		const FMatterFluxWandDefinition* Definition =
			Wand && Registry.IsValid()
				? Registry->Wands.Find(Wand->DefinitionId)
				: nullptr;
		View.bEquipped = Wand != nullptr;
		View.bActive = View.bEquipped
			&& Inventory->GetActiveEquipmentSlot() == SlotIndex;
		View.Mana = Wand ? FMath::Max(0.0f, Wand->Mana) : 0.0f;
		View.MaxMana = Definition
			? FMath::Max(0.0f, Definition->ManaMax)
			: 0.0f;
		View.Mana = FMath::Min(View.Mana, View.MaxMana);
		View.Label = FText::FromString(InputLabel);
	}
	return Snapshot;
}

void UMatterFluxPlayerStatusWidget::InitializeForPlayer(
	APlayerController* InPlayerController)
{
	PlayerController = InPlayerController;
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	BindSources();
	RefreshStatus();
}

void UMatterFluxPlayerStatusWidget::SetSuppressedByFrontEnd(
	const bool bSuppressed)
{
	SetVisibility(bSuppressed
		? ESlateVisibility::Collapsed
		: ESlateVisibility::SelfHitTestInvisible);
}

void UMatterFluxPlayerStatusWidget::RefreshStatus()
{
	BindSources();
	if (StatusView.IsValid())
	{
		StatusView->Refresh();
	}
}

void UMatterFluxPlayerStatusWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (!ContentReloadedHandle.IsValid())
	{
		ContentReloadedHandle =
			IMatterFluxScriptRuntime::Get().OnContentReloaded().AddWeakLambda(
				this,
				[this](const FMatterFluxContentRegistryPtr)
				{
					RefreshStatus();
				});
	}
	BindSources();
	RefreshStatus();
}

void UMatterFluxPlayerStatusWidget::NativeDestruct()
{
	UnbindSources();
	if (ContentReloadedHandle.IsValid()
		&& IMatterFluxScriptRuntime::IsAvailable())
	{
		IMatterFluxScriptRuntime::Get().OnContentReloaded().Remove(
			ContentReloadedHandle);
	}
	ContentReloadedHandle.Reset();
	Super::NativeDestruct();
}

TSharedRef<SWidget> UMatterFluxPlayerStatusWidget::RebuildWidget()
{
	StatusView = SNew(MatterFluxPlayerStatusUI::SMatterFluxPlayerStatus)
		.Owner(this);
	return StatusView.ToSharedRef();
}

void UMatterFluxPlayerStatusWidget::ReleaseSlateResources(
	const bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	StatusView.Reset();
}

void UMatterFluxPlayerStatusWidget::BindSources()
{
	const AMatterFluxPlayerState* PlayerState = PlayerController
		? PlayerController->GetPlayerState<AMatterFluxPlayerState>()
		: nullptr;
	UAbilitySystemComponent* AbilitySystem = PlayerState
		? PlayerState->GetAbilitySystemComponent()
		: nullptr;
	UMatterFluxMagicInventoryComponent* Inventory = PlayerState
		? PlayerState->GetMagicInventory()
		: nullptr;
	if (BoundAbilitySystem.Get() == AbilitySystem
		&& BoundInventory.Get() == Inventory)
	{
		return;
	}
	UnbindSources();
	BoundAbilitySystem = AbilitySystem;
	BoundInventory = Inventory;
	if (AbilitySystem)
	{
		HealthChangedHandle = AbilitySystem
			->GetGameplayAttributeValueChangeDelegate(
				UMatterFluxPlayerAttributeSet::GetHealthAttribute())
			.AddUObject(
				this,
				&UMatterFluxPlayerStatusWidget::HandleAttributeChanged);
		MaxHealthChangedHandle = AbilitySystem
			->GetGameplayAttributeValueChangeDelegate(
				UMatterFluxPlayerAttributeSet::GetMaxHealthAttribute())
			.AddUObject(
				this,
				&UMatterFluxPlayerStatusWidget::HandleAttributeChanged);
	}
	if (Inventory)
	{
		InventoryChangedHandle = Inventory->OnInventoryChanged().AddUObject(
			this,
			&UMatterFluxPlayerStatusWidget::HandleInventoryChanged);
	}
}

void UMatterFluxPlayerStatusWidget::UnbindSources()
{
	if (UAbilitySystemComponent* AbilitySystem = BoundAbilitySystem.Get())
	{
		if (HealthChangedHandle.IsValid())
		{
			AbilitySystem->GetGameplayAttributeValueChangeDelegate(
				UMatterFluxPlayerAttributeSet::GetHealthAttribute())
				.Remove(HealthChangedHandle);
		}
		if (MaxHealthChangedHandle.IsValid())
		{
			AbilitySystem->GetGameplayAttributeValueChangeDelegate(
				UMatterFluxPlayerAttributeSet::GetMaxHealthAttribute())
				.Remove(MaxHealthChangedHandle);
		}
	}
	if (UMatterFluxMagicInventoryComponent* Inventory = BoundInventory.Get();
		Inventory && InventoryChangedHandle.IsValid())
	{
		Inventory->OnInventoryChanged().Remove(InventoryChangedHandle);
	}
	HealthChangedHandle.Reset();
	MaxHealthChangedHandle.Reset();
	InventoryChangedHandle.Reset();
	BoundAbilitySystem.Reset();
	BoundInventory.Reset();
}

void UMatterFluxPlayerStatusWidget::HandleAttributeChanged(
	const FOnAttributeChangeData& ChangeData)
{
	(void)ChangeData;
	RefreshStatus();
}

void UMatterFluxPlayerStatusWidget::HandleInventoryChanged()
{
	RefreshStatus();
}
