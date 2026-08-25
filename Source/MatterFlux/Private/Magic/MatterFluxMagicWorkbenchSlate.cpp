#include "Magic/MatterFluxMagicWorkbenchSlate.h"
#include "Magic/MatterFluxMagicIconResolver.h"
#include "Magic/MatterFluxMagicWorkbenchWidget.h"
#include "Magic/MatterFluxMagicWorkbenchInteraction.h"
#include "Magic/MatterFluxSpellProgramLayout.h"
#include "UI/MatterFluxPaperStyle.h"
#include "UI/MatterFluxPaperWindow.h"
#include "UI/MatterFluxProgressBar.h"
#include "UI/MatterFluxSettingsPanel.h"

#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "IMatterFluxScriptRuntime.h"
#include "Framework/Application/SlateApplication.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Input/DragAndDrop.h"
#include "InputCoreTypes.h"
#include "Input/Reply.h"
#include "Rendering/DrawElementTypes.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SToolTip.h"
#include "Widgets/Text/STextBlock.h"

namespace MatterFluxMagicUI
{
	using MatterFlux::UI::Paper::Font;
	using MatterFlux::UI::Paper::Ink;
	using MatterFlux::UI::Paper::Muted;
	using MatterFlux::UI::Paper::Surface;

	FLinearColor SRGB(
		const uint8 Red,
		const uint8 Green,
		const uint8 Blue,
		const uint8 Alpha = 255)
	{
		return FLinearColor::FromSRGBColor(
			FColor(Red, Green, Blue, Alpha));
	}

	// Match PaperMagic's monochrome editor language: every panel and item is
	// white with a black keyline; selection inverts to black with white text.
	const FLinearColor& Paper = Surface;
	const FLinearColor Panel = FLinearColor::White;
	const FLinearColor Slot = FLinearColor::White;
	const FLinearColor Line = FLinearColor::Black;
	const FLinearColor Selected = FLinearColor::Black;
	const FLinearColor SpellProjectile = FLinearColor::Black;
	const FLinearColor SpellMulticast = FLinearColor::Black;
	const FLinearColor SpellTrigger = FLinearColor::Black;
	const FLinearColor ToolTipValue = SRGB(12, 132, 38);
	constexpr float KeylineThickness = 2.0f;
	// One outer size for every spell slot, regardless of whether it is shown in
	// the inventory, a program tree, or the reserve list.  Keeping the size on
	// the slot itself prevents parent layout panels from changing its geometry.
	const FVector2D SpellSlotDimensions =
		FMatterFluxMagicWorkbenchInteraction::GetSpellSlotSize();
	const float SpellSlotWidth = static_cast<float>(SpellSlotDimensions.X);
	const float SpellSlotHeight = static_cast<float>(SpellSlotDimensions.Y);
	constexpr float WorkbenchUiScale = 1.25f;
	constexpr float SpellIconSize = 58.0f;
	const float ProgramNodeWidth = SpellSlotWidth;
	const float ProgramNodeHeight = SpellSlotHeight;
	constexpr float ProgramHorizontalGap = 45.0f;
	constexpr float ProgramVerticalGap = 15.0f;

	FSlateFontInfo WorkbenchFont(
		const int32 BaseSize,
		const bool bBold = false)
	{
		return Font(
			FMath::RoundToInt(BaseSize * WorkbenchUiScale),
			bBold);
	}

	FLinearColor SpellColor(const EMatterFluxSpellKind Kind)
	{
		(void)Kind;
		return FLinearColor::White;
	}

	FText SpellBadge(const EMatterFluxSpellKind Kind)
	{
		switch (Kind)
		{
		case EMatterFluxSpellKind::Modifier:
			return FText::FromString(TEXT("+"));
		case EMatterFluxSpellKind::Multicast:
			return FText::FromString(TEXT("xN"));
		case EMatterFluxSpellKind::Trigger:
		case EMatterFluxSpellKind::TriggerModifier:
			return FText::FromString(TEXT("触"));
		case EMatterFluxSpellKind::Jump:
			return FText::FromString(TEXT("跃"));
		default:
			return FText::FromString(TEXT("弹"));
		}
	}

	TSharedRef<SWidget> ItemVisual(
		const FSlateBrush* IconBrush,
		const FText& FallbackBadge)
	{
		if (IconBrush)
		{
			return SNew(SBox)
				.WidthOverride(SpellIconSize)
				.HeightOverride(SpellIconSize)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(IconBrush)
					.ColorAndOpacity(FLinearColor::White)
				];
		}
		return SNew(STextBlock)
			.Text(FallbackBadge)
			.Font(WorkbenchFont(18, true))
			.Justification(ETextJustify::Center)
			.ColorAndOpacity(Ink);
	}

	TSharedRef<SWidget> BuildSpellItemFrame(
		const FSlateBrush* IconBrush,
		const FText& Badge,
		const FText& Subtitle,
		const FLinearColor& FillColor,
		TSharedPtr<SBorder>* OutFocusBorder = nullptr)
	{
		TSharedRef<SBorder> Keyline = SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
			.BorderBackgroundColor(Line)
			.Padding(KeylineThickness)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
				.BorderBackgroundColor(FillColor)
				.Padding(2.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						ItemVisual(IconBrush, Badge)
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Bottom)
					[
						SNew(STextBlock)
						.Text(Subtitle)
						.Font(WorkbenchFont(9, true))
						.ColorAndOpacity(Ink)
					]
				]
			];
		if (OutFocusBorder)
		{
			*OutFocusBorder = Keyline;
		}
		return SNew(SBox)
			.WidthOverride(SpellSlotWidth)
			.HeightOverride(SpellSlotHeight)
			[Keyline];
	}

	FText SpellKindDisplayName(const EMatterFluxSpellKind Kind)
	{
		switch (Kind)
		{
		case EMatterFluxSpellKind::Modifier:
			return FText::FromString(TEXT("修饰法术"));
		case EMatterFluxSpellKind::Multicast:
			return FText::FromString(TEXT("多重施法"));
		case EMatterFluxSpellKind::Trigger:
			return FText::FromString(TEXT("触发投射物"));
		case EMatterFluxSpellKind::TriggerModifier:
			return FText::FromString(TEXT("触发器"));
		case EMatterFluxSpellKind::Jump:
			return FText::FromString(TEXT("施法者动作"));
		default:
			return FText::FromString(TEXT("投射物"));
		}
	}

	void AddToolTipStat(
		const TSharedRef<SGridPanel>& Stats,
		int32& Row,
		const FString& Label,
		const FText& Value)
	{
		Stats->AddSlot(0, Row)
		.Padding(0.0f, 2.0f, 18.0f, 2.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
				.Text(FText::FromString(Label + TEXT("：")))
				.Font(WorkbenchFont(12))
				.ColorAndOpacity(Ink)
		];
		Stats->AddSlot(1, Row)
		.Padding(0.0f, 2.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
				.Text(Value)
				.Font(WorkbenchFont(12, true))
				.ColorAndOpacity(ToolTipValue)
		];
		++Row;
	}

	TSharedRef<SWidget> BuildToolTipFrame(
		const FString& Title,
		const FString& Description,
		const TSharedRef<SGridPanel>& Stats)
	{
		return SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
			.BorderBackgroundColor(Ink)
			.Padding(1.0f)
			[
				SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
					.BorderBackgroundColor(FLinearColor::White)
					.Padding(FMargin(17.0f, 14.0f, 18.0f, 16.0f))
				[
					SNew(SBox)
						.WidthOverride(310.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
								.Text(FText::FromString(Title))
								.Font(WorkbenchFont(21, true))
								.ColorAndOpacity(Ink)
						]
						+ SVerticalBox::Slot().AutoHeight()
						.Padding(0.0f, 12.0f, 0.0f, 14.0f)
						[
							SNew(STextBlock)
								.Text(FText::FromString(Description))
								.Font(WorkbenchFont(12))
								.ColorAndOpacity(Muted)
								.AutoWrapText(true)
								.WrapTextAt(280.0f)
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							Stats
						]
					]
				]
			];
	}

	TSharedRef<SWidget> BuildSpellToolTipContent(
		const FMatterFluxSpellDefinition& Definition)
	{
		TSharedRef<SGridPanel> Stats = SNew(SGridPanel);
		int32 Row = 0;
		const auto AddStat = [&Stats, &Row](
			const FString& Label,
			const FText& Value)
		{
			AddToolTipStat(Stats, Row, Label, Value);
		};

		AddStat(TEXT("法术类型"), SpellKindDisplayName(Definition.Kind));
		if (Definition.Kind == EMatterFluxSpellKind::Modifier
			&& !FMath::IsNearlyZero(Definition.DamageAdd))
		{
			AddStat(TEXT("增加伤害"), FText::AsNumber(Definition.DamageAdd));
		}
		else if (Definition.Kind == EMatterFluxSpellKind::Projectile
			|| Definition.Kind == EMatterFluxSpellKind::Trigger)
		{
			AddStat(TEXT("造成伤害"), FText::AsNumber(Definition.Damage));
		}
		if (Definition.Kind == EMatterFluxSpellKind::Multicast)
		{
			AddStat(TEXT("分支数量"), FText::AsNumber(Definition.DrawCount));
		}
		else if (Definition.DrawCount > 0)
		{
			AddStat(TEXT("抽取数量"), FText::AsNumber(Definition.DrawCount));
		}
		if (Definition.TriggerDrawCount > 0)
		{
			AddStat(TEXT("载荷数量"),
				FText::AsNumber(Definition.TriggerDrawCount));
		}
		if (Definition.Kind == EMatterFluxSpellKind::Jump)
		{
			AddStat(TEXT("垂直冲量"),
				FText::AsNumber(Definition.VerticalImpulse));
		}
		AddStat(TEXT("消耗法力"), FText::AsNumber(Definition.ManaCost));
		if (!FMath::IsNearlyZero(Definition.CastDelayDelta))
		{
			AddStat(TEXT("施法延迟"), FText::FromString(FString::Printf(
				TEXT("%+.2f 秒"), Definition.CastDelayDelta)));
		}
		if (!FMath::IsNearlyZero(Definition.RechargeTimeDelta))
		{
			AddStat(TEXT("充能时间"), FText::FromString(FString::Printf(
				TEXT("%+.2f 秒"), Definition.RechargeTimeDelta)));
		}

		return BuildToolTipFrame(
			Definition.DisplayName,
			Definition.Description,
			Stats);
	}

	FText ItemCategoryDisplayName(const EMatterFluxItemCategory Category)
	{
		switch (Category)
		{
		case EMatterFluxItemCategory::Quest:
			return FText::FromString(TEXT("任务道具"));
		case EMatterFluxItemCategory::Consumable:
			return FText::FromString(TEXT("消耗品"));
		default:
			return FText::FromString(TEXT("材料"));
		}
	}

	FText ItemUseDisplayName(const EMatterFluxItemUseAction UseAction)
	{
		switch (UseAction)
		{
		case EMatterFluxItemUseAction::RestoreHealth:
			return FText::FromString(TEXT("恢复生命"));
		case EMatterFluxItemUseAction::RestoreWandMana:
			return FText::FromString(TEXT("恢复法杖法力"));
		case EMatterFluxItemUseAction::GameplayEvent:
			return FText::FromString(TEXT("触发特殊效果"));
		default:
			return FText::FromString(TEXT("不可主动使用"));
		}
	}

	TSharedRef<SWidget> BuildItemToolTipContent(
		const FMatterFluxItemDefinition& Definition,
		const int32 Quantity)
	{
		TSharedRef<SGridPanel> Stats = SNew(SGridPanel);
		int32 Row = 0;
		AddToolTipStat(
			Stats, Row, TEXT("物品类型"),
			ItemCategoryDisplayName(Definition.Category));
		AddToolTipStat(
			Stats, Row, TEXT("持有数量"), FText::AsNumber(Quantity));
		AddToolTipStat(
			Stats, Row, TEXT("堆叠上限"), FText::AsNumber(Definition.MaxStack));
		if (Definition.UseAction != EMatterFluxItemUseAction::None)
		{
			AddToolTipStat(
				Stats, Row, TEXT("使用效果"),
				ItemUseDisplayName(Definition.UseAction));
			if (!FMath::IsNearlyZero(Definition.UseMagnitude))
			{
				AddToolTipStat(
					Stats, Row, TEXT("效果强度"),
					FText::AsNumber(Definition.UseMagnitude));
			}
			AddToolTipStat(
				Stats, Row, TEXT("每次消耗"),
				FText::AsNumber(Definition.ConsumeCount));
			AddToolTipStat(
				Stats, Row, TEXT("操作"),
				FText::FromString(TEXT("右键或 Enter 使用")));
		}
		return BuildToolTipFrame(
			Definition.DisplayName,
			Definition.Description,
			Stats);
	}

	TSharedRef<SWidget> BuildWandToolTipContent(
		const FMatterFluxWandDefinition& Definition,
		const FMatterFluxOwnedWand& Wand)
	{
		TSharedRef<SGridPanel> Stats = SNew(SGridPanel);
		int32 Row = 0;
		AddToolTipStat(
			Stats, Row, TEXT("抽取方式"),
			FText::FromString(Definition.bShuffle ? TEXT("乱序") : TEXT("顺序")));
		AddToolTipStat(
			Stats, Row, TEXT("法术上限"), FText::AsNumber(Definition.Capacity));
		AddToolTipStat(
			Stats, Row, TEXT("每次抽取"), FText::AsNumber(Definition.DrawCount));
		AddToolTipStat(
			Stats, Row, TEXT("当前法力"),
			FText::FromString(FString::Printf(
				TEXT("%.0f / %.0f"), Wand.Mana, Definition.ManaMax)));
		AddToolTipStat(
			Stats, Row, TEXT("恢复速度"),
			FText::FromString(FString::Printf(
				TEXT("+%.0f / 秒"), Definition.ManaRechargePerSecond)));
		AddToolTipStat(
			Stats, Row, TEXT("施法间隔"),
			FText::FromString(FString::Printf(
				TEXT("%.2f 秒"), Definition.CastDelay)));
		AddToolTipStat(
			Stats, Row, TEXT("充能时间"),
			FText::FromString(FString::Printf(
				TEXT("%.2f 秒"), Definition.RechargeTime)));
		if (!FMath::IsNearlyZero(Definition.Spread))
		{
			AddToolTipStat(
				Stats, Row, TEXT("散布角度"),
				FText::FromString(FString::Printf(
					TEXT("%.1f°"), Definition.Spread)));
		}
		return BuildToolTipFrame(
			Definition.DisplayName,
			Definition.Description,
			Stats);
	}

	enum class EDragSource : uint8
	{
		None,
		Wand,
		SpellInventory,
		DeckSpell
	};

	struct FDragPayload
	{
		EDragSource Source = EDragSource::None;
		FGuid WandId;
		FName SpellId;
		int32 SpellSlot = INDEX_NONE;
		FText Label;
		FText Badge;
		FText Subtitle;
		const FSlateBrush* IconBrush = nullptr;
		FLinearColor Tint = FLinearColor::White;

		bool IsValid() const
		{
			return Source != EDragSource::None;
		}
	};

	class FMagicDragDropOperation : public FGameDragDropOperation
	{
	public:
		DRAG_DROP_OPERATOR_TYPE(
			FMagicDragDropOperation,
			FGameDragDropOperation)

		FDragPayload Payload;

		static TSharedRef<FMagicDragDropOperation> New(
			const FDragPayload& InPayload,
			const FVector2D& CursorScreenPosition)
		{
			TSharedRef<FMagicDragDropOperation> Operation =
				MakeShared<FMagicDragDropOperation>();
			Operation->Payload = InPayload;
			Operation->DecoratorPosition =
				FMatterFluxMagicWorkbenchInteraction::
					CalculateSpellDragDecoratorPosition(
						CursorScreenPosition);
			Operation->DecoratorWidget = BuildSpellItemFrame(
				InPayload.IconBrush,
				InPayload.Badge.IsEmpty()
					? InPayload.Label : InPayload.Badge,
				InPayload.Subtitle,
				InPayload.Tint);
			Operation->Construct();
			return Operation;
		}

		virtual TSharedPtr<SWidget> GetDefaultDecorator() const override
		{
			return DecoratorWidget;
		}

		virtual void OnDragged(
			const FDragDropEvent& DragDropEvent) override
		{
			DecoratorPosition =
				FMatterFluxMagicWorkbenchInteraction::
					CalculateSpellDragDecoratorPosition(
						DragDropEvent.GetScreenSpacePosition());
		}

	private:
		TSharedPtr<SWidget> DecoratorWidget;
	};

	class SMagicItemSlot : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SMagicItemSlot)
			: _Badge(FText::GetEmpty())
			, _IconBrush(nullptr)
			, _ToolTipContent()
			, _Tint(Slot)
			, _bSelected(false)
		{}
			SLATE_ARGUMENT(FText, Badge)
			SLATE_ARGUMENT(FText, Label)
			SLATE_ARGUMENT(FText, Subtitle)
			SLATE_ARGUMENT(const FSlateBrush*, IconBrush)
			SLATE_ARGUMENT(FText, ToolTip)
			SLATE_ARGUMENT(TSharedPtr<SWidget>, ToolTipContent)
			SLATE_ARGUMENT(FLinearColor, Tint)
			SLATE_ARGUMENT(bool, bSelected)
			SLATE_ARGUMENT(FDragPayload, DragPayload)
			SLATE_ARGUMENT(TFunction<void()>, OnLeftClick)
			SLATE_ARGUMENT(TFunction<void()>, OnRightClick)
			SLATE_ARGUMENT(TFunction<void()>, OnActivate)
			SLATE_ARGUMENT(TFunction<bool(const FDragPayload&)>, CanAcceptPayload)
			SLATE_ARGUMENT(TFunction<bool(const FDragPayload&)>, OnDropPayload)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args)
		{
			DragPayload = Args._DragPayload;
			const FLinearColor VisibleFillColor = Args._bSelected
				? SRGB(234, 234, 234)
				: FLinearColor::White;
			DragPayload.Badge = Args._Badge;
			DragPayload.Subtitle = Args._Subtitle;
			DragPayload.IconBrush = Args._IconBrush;
			DragPayload.Tint = VisibleFillColor;
			OnLeftClick = Args._OnLeftClick;
			OnRightClick = Args._OnRightClick;
			OnActivate = Args._OnActivate;
			CanAcceptPayload = Args._CanAcceptPayload;
			OnDropPayload = Args._OnDropPayload;
			BaseBorderColor = Line;
			if (Args._ToolTipContent.IsValid())
			{
				const TSharedRef<SToolTip> RichToolTip = SNew(SToolTip)
					.TextMargin(FMargin(0.0f))
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("NoBorder")))
					[Args._ToolTipContent.ToSharedRef()];
				SetToolTip(TAttribute<TSharedPtr<IToolTip>>(RichToolTip));
			}
			else
			{
				SetToolTipText(Args._ToolTip);
			}
			ChildSlot
			[
				BuildSpellItemFrame(
					Args._IconBrush,
					Args._Badge,
					Args._Subtitle,
					VisibleFillColor,
					&FocusBorder)
			];
		}

		virtual FReply OnMouseButtonDown(
			const FGeometry& Geometry,
			const FPointerEvent& Event) override
		{
			FSlateApplication::Get().SetKeyboardFocus(
				SharedThis(this),
				EFocusCause::Mouse);
			if (Event.GetEffectingButton() == EKeys::RightMouseButton)
			{
				if (OnRightClick)
				{
					OnRightClick();
					return FReply::Handled();
				}
			}
			if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				return DragPayload.IsValid()
					? FReply::Handled().DetectDrag(
						SharedThis(this),
						EKeys::LeftMouseButton)
					: FReply::Handled();
			}
			return FReply::Unhandled();
		}

		virtual bool SupportsKeyboardFocus() const override
		{
			return true;
		}

		virtual FReply OnFocusReceived(
			const FGeometry& Geometry,
			const FFocusEvent& Event) override
		{
			if (FocusBorder.IsValid())
			{
				FocusBorder->SetBorderBackgroundColor(Selected);
			}
			return FReply::Handled();
		}

		virtual void OnFocusLost(const FFocusEvent& Event) override
		{
			if (FocusBorder.IsValid())
			{
				FocusBorder->SetBorderBackgroundColor(BaseBorderColor);
			}
			SCompoundWidget::OnFocusLost(Event);
		}

		virtual FReply OnKeyDown(
			const FGeometry& Geometry,
			const FKeyEvent& Event) override
		{
			const FKey Key = Event.GetKey();
			if ((Key == EKeys::Enter
					|| Key == EKeys::SpaceBar
					|| Key == EKeys::Gamepad_FaceButton_Bottom)
				&& (OnActivate || OnLeftClick))
			{
				if (OnActivate) OnActivate();
				else OnLeftClick();
				return FReply::Handled();
			}
			if ((Key == EKeys::Delete
					|| Key == EKeys::BackSpace
					|| Key == EKeys::Gamepad_FaceButton_Right)
				&& OnRightClick)
			{
				OnRightClick();
				return FReply::Handled();
			}
			return FReply::Unhandled();
		}

		virtual void OnMouseEnter(
			const FGeometry& Geometry,
			const FPointerEvent& Event) override
		{
			SCompoundWidget::OnMouseEnter(Geometry, Event);
			if (FocusBorder.IsValid())
			{
				FocusBorder->SetBorderBackgroundColor(Selected);
			}
		}

		virtual void OnMouseLeave(
			const FPointerEvent& Event) override
		{
			SCompoundWidget::OnMouseLeave(Event);
			if (FocusBorder.IsValid() && !HasKeyboardFocus())
			{
				FocusBorder->SetBorderBackgroundColor(BaseBorderColor);
			}
		}

		virtual FReply OnMouseButtonUp(
			const FGeometry& Geometry,
			const FPointerEvent& Event) override
		{
			if (Event.GetEffectingButton() == EKeys::LeftMouseButton
				&& OnLeftClick)
			{
				OnLeftClick();
				return FReply::Handled();
			}
			return FReply::Unhandled();
		}

		virtual FReply OnDragDetected(
			const FGeometry& Geometry,
			const FPointerEvent& Event) override
		{
			return DragPayload.IsValid()
				? FReply::Handled().BeginDragDrop(
					FMagicDragDropOperation::New(
						DragPayload,
						Event.GetScreenSpacePosition()))
				: FReply::Unhandled();
		}

		virtual FReply OnDrop(
			const FGeometry& Geometry,
			const FDragDropEvent& Event) override
		{
			const TSharedPtr<FMagicDragDropOperation> Operation =
				Event.GetOperationAs<FMagicDragDropOperation>();
			const bool bAccepted = Operation.IsValid()
				&& OnDropPayload
				&& OnDropPayload(Operation->Payload);
			if (FocusBorder.IsValid())
			{
				FocusBorder->SetBorderBackgroundColor(BaseBorderColor);
			}
			return bAccepted ? FReply::Handled() : FReply::Unhandled();
		}

		virtual void OnDragEnter(
			const FGeometry& Geometry,
			const FDragDropEvent& Event) override
		{
			SCompoundWidget::OnDragEnter(Geometry, Event);
			const TSharedPtr<FMagicDragDropOperation> Operation =
				Event.GetOperationAs<FMagicDragDropOperation>();
			if (Operation.IsValid() && FocusBorder.IsValid())
			{
				const bool bCanAccept = CanAcceptPayload
					? CanAcceptPayload(Operation->Payload)
					: static_cast<bool>(OnDropPayload);
				FocusBorder->SetBorderBackgroundColor(
					bCanAccept ? SRGB(35, 145, 82) : SRGB(188, 44, 44));
			}
		}

		virtual void OnDragLeave(const FDragDropEvent& Event) override
		{
			if (FocusBorder.IsValid())
			{
				FocusBorder->SetBorderBackgroundColor(BaseBorderColor);
			}
			SCompoundWidget::OnDragLeave(Event);
		}

	private:
		FDragPayload DragPayload;
		TFunction<void()> OnLeftClick;
		TFunction<void()> OnRightClick;
		TFunction<void()> OnActivate;
		TFunction<bool(const FDragPayload&)> CanAcceptPayload;
		TFunction<bool(const FDragPayload&)> OnDropPayload;
		TSharedPtr<SBorder> FocusBorder;
		FLinearColor BaseBorderColor = Line;
	};

	struct FMagicTreeLine
	{
		FVector2f Start = FVector2f::ZeroVector;
		FVector2f End = FVector2f::ZeroVector;
	};

	class SMagicTreeCanvas : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SMagicTreeCanvas)
			: _DesiredSize(FVector2D::ZeroVector)
		{}
			SLATE_ARGUMENT(FVector2D, DesiredSize)
			SLATE_ARGUMENT(TArray<FMagicTreeLine>, Lines)
			SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args)
		{
			DesiredSize = Args._DesiredSize;
			Lines = Args._Lines;
			ChildSlot
			[
				SNew(SBox)
				.WidthOverride(DesiredSize.X)
				.HeightOverride(DesiredSize.Y)
				[Args._Content.Widget]
			];
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return DesiredSize;
		}

		virtual int32 OnPaint(
			const FPaintArgs& Args,
			const FGeometry& AllottedGeometry,
			const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements,
			const int32 LayerId,
			const FWidgetStyle& InWidgetStyle,
			const bool bParentEnabled) const override
		{
			for (const FMagicTreeLine& LineToDraw : Lines)
			{
				TArray<FVector2f> Points;
				Points.Reserve(2);
				Points.Add(LineToDraw.Start);
				Points.Add(LineToDraw.End);
				FSlateDrawElement::MakeLines(
					OutDrawElements,
					LayerId,
					AllottedGeometry.ToPaintGeometry(),
					MoveTemp(Points),
					ESlateDrawEffect::None,
					Ink,
					true,
					2.0f);
			}
			return SCompoundWidget::OnPaint(
				Args,
				AllottedGeometry,
				MyCullingRect,
				OutDrawElements,
				LayerId + 1,
				InWidgetStyle,
				bParentEnabled);
		}

	private:
		FVector2D DesiredSize = FVector2D::ZeroVector;
		TArray<FMagicTreeLine> Lines;
	};

}

namespace MatterFluxMagicUI
{
class SMatterFluxMagicWorkbench : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMatterFluxMagicWorkbench) {}
		SLATE_ARGUMENT(
			TWeakObjectPtr<UMatterFluxMagicWorkbenchWidget>,
			OwnerWidget)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args)
	{
		OwnerWidget = Args._OwnerWidget;
		Refresh();
	}

	void Refresh()
	{
		ChildSlot.AttachWidget(BuildWorkbench());
	}

private:
	TSharedRef<SWidget> Heading(const FString& Text) const
	{
		return SNew(STextBlock)
			.Text(FText::FromString(Text))
			.Font(WorkbenchFont(14, true))
			.ColorAndOpacity(Ink);
	}

	TSharedRef<SWidget> OutlinedPanel(
		const TSharedRef<SWidget>& Content,
		const FMargin& Padding = FMargin(0.0f),
		const FLinearColor& Fill = FLinearColor::White) const
	{
		return MatterFlux::UI::Paper::Outline(Content, Padding, Fill);
	}

	const FSlateBrush* GetIconBrush(const FString& IconKey)
	{
		if (IconKey.IsEmpty())
		{
			return nullptr;
		}
		if (const TSharedPtr<FSlateDynamicImageBrush>* Existing =
			IconBrushes.Find(IconKey))
		{
			return Existing->Get();
		}

		FString IconPath;
		if (!FMatterFluxMagicIconResolver::TryResolveIconPath(
			IconKey,
			IconPath))
		{
			return nullptr;
		}

		TSharedPtr<FSlateDynamicImageBrush> Brush =
			MakeShared<FSlateDynamicImageBrush>(
				FName(*IconPath),
				FVector2D(SpellIconSize, SpellIconSize));
		IconBrushes.Add(IconKey, Brush);
		return Brush.Get();
	}

	TSharedRef<SWidget> BuildWorkbench()
	{
		UMatterFluxMagicWorkbenchWidget* Widget = OwnerWidget.Get();
		UMatterFluxMagicInventoryComponent* Inventory = Widget
			? Widget->ResolveInventory()
			: nullptr;
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::Get().GetActiveRegistry();
		if (!Widget || !Inventory || !Registry.IsValid())
		{
			return SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
				.BorderBackgroundColor(Paper)
				.Padding(24.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("正在等待同步魔法背包……")))
					.Font(WorkbenchFont(14, true))
					.ColorAndOpacity(Ink)
				];
		}

		const FMatterFluxOwnedWand* SelectedWand =
			Inventory->FindWand(Widget->GetSelectedWandId());
		const FMatterFluxWandDefinition* SelectedDefinition =
			SelectedWand
				? Registry->Wands.Find(SelectedWand->DefinitionId)
				: nullptr;

		TSharedRef<SWidget> Page = SNullWidget::NullWidget;
		switch (Widget->GetPage())
		{
		case EMatterFluxWorkbenchPage::WandBackpack:
			Page = BuildWandBackpackPage(*Inventory, *Registry);
			break;
		case EMatterFluxWorkbenchPage::ItemBackpack:
			Page = BuildItemBackpackPage(*Registry);
			break;
		case EMatterFluxWorkbenchPage::QuestJournal:
			Page = BuildQuestJournalPage(*Registry);
			break;
		case EMatterFluxWorkbenchPage::Settings:
			Page = BuildSettingsPage();
			break;
		case EMatterFluxWorkbenchPage::SpellEditor:
		default:
			Page = BuildSpellEditorPage(
				*Inventory, *Registry, SelectedWand, SelectedDefinition);
			break;
		}

		return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
			.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		.Padding(24.0f)
		[
			SNew(SMatterFluxPaperWindow)
			.Header()
			[
				BuildTabBar()
			]
			[Page]
		];
	}

	TSharedRef<SWidget> BuildTabBar()
	{
		const EMatterFluxWorkbenchPage Page = OwnerWidget->GetPage();
		const bool bSpell = Page == EMatterFluxWorkbenchPage::SpellEditor;
		const bool bWand = Page == EMatterFluxWorkbenchPage::WandBackpack;
		const bool bItem = Page == EMatterFluxWorkbenchPage::ItemBackpack;
		const bool bQuest = Page == EMatterFluxWorkbenchPage::QuestJournal;
		const bool bSettings = Page == EMatterFluxWorkbenchPage::Settings;
		auto Tab = [this](
			const FString& Label,
			const bool bSelected,
			TFunction<void()> SelectPage)
		{
			return SNew(SMatterFluxPaperTab)
				.Label(FText::FromString(Label))
				.FontSize(18)
				.bSelected(bSelected)
				.OnClicked([SelectPage = MoveTemp(SelectPage)]() mutable
				{
					SelectPage();
					return FReply::Handled();
				});
		};
		return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(8.0f, 6.0f, 4.0f, 6.0f)
		[
			Tab(TEXT("法术"), bSpell, [Owner = OwnerWidget]()
			{
				if (Owner.IsValid()) Owner->ShowSpellEditor();
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 6.0f, 4.0f, 6.0f)
		[
			Tab(TEXT("法杖"), bWand, [Owner = OwnerWidget]()
			{
				if (Owner.IsValid()) Owner->ShowWandBackpack();
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 6.0f, 4.0f, 6.0f)
		[
			Tab(TEXT("道具"), bItem, [Owner = OwnerWidget]()
			{
				if (Owner.IsValid()) Owner->ShowItemBackpack();
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 6.0f, 4.0f, 6.0f)
		[
			Tab(TEXT("任务"), bQuest, [Owner = OwnerWidget]()
			{
				if (Owner.IsValid()) Owner->ShowQuestJournal();
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 6.0f, 4.0f, 6.0f)
		[
			Tab(TEXT("设置"), bSettings, [Owner = OwnerWidget]()
			{
				if (Owner.IsValid()) Owner->ShowSettingsPage();
			})
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
			.BorderBackgroundColor(Panel)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 5.0f, 8.0f, 5.0f)
		[
			SNew(SMatterFluxPaperTab)
			.Label(FText::FromString(TEXT("×")))
			.FontSize(18)
			.ToolTip(FText::FromString(TEXT("关闭（I / Tab / Esc）")))
			.Padding(FMargin(10.0f, 5.0f))
			.OnClicked([Owner = OwnerWidget]()
			{
				if (Owner.IsValid())
				{
					Owner->RequestClose();
				}
				return FReply::Handled();
			})
		];
	}

	TSharedRef<SWidget> BuildSettingsPage()
	{
		return SNew(SMatterFluxSettingsPanel);
	}

	TSharedRef<SWidget> BuildItemBackpackPage(
		const FMatterFluxContentRegistry& Registry)
	{
		UMatterFluxProgressionComponent* Progression =
			OwnerWidget.IsValid() ? OwnerWidget->ResolveProgression() : nullptr;
		if (!Progression)
		{
			return OutlinedPanel(
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("正在等待同步道具背包……")))
				.Font(WorkbenchFont(14, true))
				.ColorAndOpacity(Ink), FMargin(20.0f));
		}

		TSharedRef<SWrapBox> Grid = SNew(SWrapBox).UseAllottedSize(true);
		FName SelectedItem = OwnerWidget->GetSelectedItem();
		if (!Registry.Items.Contains(SelectedItem)
			|| Progression->GetItemQuantity(SelectedItem) <= 0)
		{
			SelectedItem = Progression->GetItems().IsEmpty()
				? NAME_None : Progression->GetItems()[0].ItemId;
		}
		for (const FMatterFluxItemStack& Stack : Progression->GetItems())
		{
			const FMatterFluxItemDefinition* Definition =
				Registry.Items.Find(Stack.ItemId);
			if (!Definition) continue;
			FString Badge = TEXT("物");
			if (Definition->Id == FName(TEXT("std.coin"))) Badge = TEXT("币");
			else if (Definition->Category == EMatterFluxItemCategory::Consumable)
				Badge = TEXT("药");
			else if (Definition->Category == EMatterFluxItemCategory::Quest)
				Badge = TEXT("任");
			const FName ItemId = Stack.ItemId;
			const bool bUsable =
				Definition->UseAction != EMatterFluxItemUseAction::None;
			Grid->AddSlot().Padding(4.0f)
			[
				SNew(SMagicItemSlot)
					.Badge(FText::FromString(Badge))
					.IconBrush(GetIconBrush(Definition->Icon))
					.Label(FText::FromString(Definition->DisplayName))
				.Subtitle(FText::AsNumber(Stack.Quantity))
				.ToolTipContent(BuildItemToolTipContent(
					*Definition,
					Stack.Quantity))
				.bSelected(SelectedItem == ItemId)
				.OnLeftClick([Owner = OwnerWidget, ItemId]()
				{
					if (Owner.IsValid()) Owner->SelectItem(ItemId);
				})
				.OnRightClick(bUsable
					? TFunction<void()>([Owner = OwnerWidget, ItemId]()
					{
						if (Owner.IsValid()) Owner->UseItem(ItemId);
					}) : TFunction<void()>())
				.OnActivate(bUsable
					? TFunction<void()>([Owner = OwnerWidget, ItemId]()
					{
						if (Owner.IsValid()) Owner->UseItem(ItemId);
					}) : TFunction<void()>())
			];
		}

		const FMatterFluxItemDefinition* SelectedDefinition =
			Registry.Items.Find(SelectedItem);
		const int32 SelectedQuantity =
			Progression->GetItemQuantity(SelectedItem);
		TSharedRef<SVerticalBox> Detail = SNew(SVerticalBox);
		Detail->AddSlot().AutoHeight().Padding(12.0f, 10.0f)
		[
			Heading(SelectedDefinition
				? SelectedDefinition->DisplayName : TEXT("未选择道具"))
		];
		Detail->AddSlot().AutoHeight().Padding(12.0f, 0.0f, 12.0f, 12.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(SelectedDefinition
				? FString::Printf(TEXT("数量  %d / %d\n\n%s"),
					SelectedQuantity, SelectedDefinition->MaxStack,
					*SelectedDefinition->Description)
				: TEXT("从左侧选择一个道具。")))
			.Font(WorkbenchFont(12))
			.ColorAndOpacity(Ink)
			.AutoWrapText(true)
		];
		if (SelectedDefinition
			&& SelectedDefinition->UseAction != EMatterFluxItemUseAction::None)
		{
			const FName ItemId = SelectedDefinition->Id;
			Detail->AddSlot().AutoHeight().HAlign(HAlign_Left).Padding(12.0f)
			[
				SNew(SButton)
				.ButtonStyle(FCoreStyle::Get(), TEXT("NoBorder"))
				.ContentPadding(0.0f)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.OnClicked_Lambda([Owner = OwnerWidget, ItemId]()
				{
					if (Owner.IsValid()) Owner->UseItem(ItemId);
					return FReply::Handled();
				})
				[
					OutlinedPanel(
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("使用")))
						.Font(WorkbenchFont(12, true))
						.Justification(ETextJustify::Center)
						.ColorAndOpacity(Ink), FMargin(18.0f, 7.0f))
				]
			];
		}

		return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.7f).Padding(0.0f, 0.0f, 10.0f, 0.0f)
		[
			OutlinedPanel(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 10.0f)
				[
					Heading(TEXT("道具"))
				]
				+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8.0f)
				[
					SNew(SScrollBox) + SScrollBox::Slot()[Grid]
				], FMargin(2.0f))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			OutlinedPanel(Detail, FMargin(2.0f))
		];
	}

	TSharedRef<SWidget> BuildQuestJournalPage(
		const FMatterFluxContentRegistry& Registry)
	{
		UMatterFluxProgressionComponent* Progression =
			OwnerWidget.IsValid() ? OwnerWidget->ResolveProgression() : nullptr;
		if (!Progression)
		{
			return OutlinedPanel(
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("正在等待同步任务……")))
				.Font(WorkbenchFont(14, true))
				.ColorAndOpacity(Ink), FMargin(20.0f));
		}
		TArray<const FMatterFluxQuestDefinition*> VisibleDefinitions;
		for (const FMatterFluxQuestState& State : Progression->GetQuests())
		{
			const FMatterFluxQuestDefinition* Definition =
				Registry.Quests.Find(State.QuestId);
			if (Definition
				&& Definition->Category != EMatterFluxQuestCategory::Objective
				&& State.Status != EMatterFluxQuestRuntimeStatus::Hidden)
			{
				VisibleDefinitions.Add(Definition);
			}
		}
		VisibleDefinitions.Sort([](
			const FMatterFluxQuestDefinition& A,
			const FMatterFluxQuestDefinition& B)
		{
			if (A.Category != B.Category) return A.Category < B.Category;
			if (A.SortOrder != B.SortOrder) return A.SortOrder < B.SortOrder;
			return A.Id.LexicalLess(B.Id);
		});
		FName SelectedQuest = Progression->GetSelectedQuest();
		if (!Registry.Quests.Contains(SelectedQuest)
			&& !VisibleDefinitions.IsEmpty())
		{
			SelectedQuest = VisibleDefinitions[0]->Id;
		}

		TSharedRef<SVerticalBox> QuestList = SNew(SVerticalBox);
		for (const FMatterFluxQuestDefinition* Definition : VisibleDefinitions)
		{
			const FMatterFluxQuestState* State =
				Progression->FindQuestState(Definition->Id);
			const bool bCompleted = State
				&& State->Status == EMatterFluxQuestRuntimeStatus::Completed;
			const bool bSelected = Definition->Id == SelectedQuest;
			const FName QuestId = Definition->Id;
			QuestList->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SButton)
				.ButtonStyle(FCoreStyle::Get(), TEXT("NoBorder"))
				.ContentPadding(0.0f)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Center)
				.OnClicked_Lambda([Owner = OwnerWidget, QuestId]()
				{
					if (Owner.IsValid()) Owner->SelectQuest(QuestId);
					return FReply::Handled();
				})
				.ToolTipText(FText::FromString(Definition->Description))
				[
					OutlinedPanel(
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(
							TEXT("%s  %s"), bCompleted ? TEXT("■") : TEXT("□"),
							*Definition->DisplayName)))
						.Font(WorkbenchFont(12, bSelected))
						.Justification(ETextJustify::Center)
						.ColorAndOpacity(bSelected ? FLinearColor::White : Ink),
						FMargin(10.0f, 8.0f),
						bSelected ? Selected : FLinearColor::White)
				]
			];
		}

		const FMatterFluxQuestDefinition* SelectedDefinition =
			Registry.Quests.Find(SelectedQuest);
		const FMatterFluxQuestState* SelectedState = SelectedDefinition
			? Progression->FindQuestState(SelectedDefinition->Id) : nullptr;
		TSharedRef<SVerticalBox> Detail = SNew(SVerticalBox);
		Detail->AddSlot().AutoHeight().Padding(16.0f, 14.0f, 16.0f, 8.0f)
		[
			Heading(SelectedDefinition
				? SelectedDefinition->DisplayName : TEXT("暂无任务"))
		];
		if (SelectedDefinition && SelectedState)
		{
			const bool bCompleted = SelectedState->Status
				== EMatterFluxQuestRuntimeStatus::Completed;
			Detail->AddSlot().AutoHeight().Padding(16.0f, 0.0f, 16.0f, 14.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(bCompleted
					&& !SelectedDefinition->CompletedDescription.IsEmpty()
						? SelectedDefinition->CompletedDescription
						: SelectedDefinition->Description))
				.Font(WorkbenchFont(12))
				.ColorAndOpacity(Ink)
				.AutoWrapText(true)
			];
			for (const FName ChildId : SelectedDefinition->Subquests)
			{
				const FMatterFluxQuestDefinition* Child = Registry.Quests.Find(ChildId);
				const FMatterFluxQuestState* ChildState =
					Progression->FindQuestState(ChildId);
				if (!Child || !ChildState
					|| ChildState->Status == EMatterFluxQuestRuntimeStatus::Hidden)
				{
					continue;
				}
				const bool bChildCompleted = ChildState->Status
					== EMatterFluxQuestRuntimeStatus::Completed;
				const FString ProgressText =
					(Child->Objective == EMatterFluxQuestObjectiveKind::KillEnemies
						|| Child->Objective == EMatterFluxQuestObjectiveKind::SpendItem)
					? FString::Printf(TEXT("  %d / %d"),
						ChildState->Progress, Child->TargetCount) : FString();
				Detail->AddSlot().AutoHeight().Padding(16.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(
						TEXT("%s  %s%s%s"),
						bChildCompleted ? TEXT("■") : TEXT("□"),
						*Child->Description,
						*ProgressText,
						Child->bOptional ? TEXT("（可选）") : TEXT(""))))
					.Font(WorkbenchFont(11))
					.ColorAndOpacity(bChildCompleted ? Muted : Ink)
					.AutoWrapText(true)
				];
			}
		}

		return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.8f).Padding(0.0f, 0.0f, 10.0f, 0.0f)
		[
			OutlinedPanel(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 10.0f)
				[
					Heading(TEXT("任务"))
				]
				+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8.0f)
				[
					SNew(SScrollBox) + SScrollBox::Slot()[QuestList]
				], FMargin(2.0f))
		]
		+ SHorizontalBox::Slot().FillWidth(1.7f)
		[
			OutlinedPanel(
				SNew(SScrollBox) + SScrollBox::Slot()[Detail], FMargin(2.0f))
		];
	}

	TSharedRef<SWidget> BuildWandBackpackPage(
		const UMatterFluxMagicInventoryComponent& Inventory,
		const FMatterFluxContentRegistry& Registry)
	{
		TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
		TSharedRef<SWrapBox> WandGrid = SNew(SWrapBox).UseAllottedSize(true);
		for (const FMatterFluxOwnedWand& Wand : Inventory.GetOwnedWands())
		{
			const FMatterFluxWandDefinition* Definition =
				Registry.Wands.Find(Wand.DefinitionId);
			if (!Definition)
			{
				continue;
			}
			FDragPayload Payload;
			Payload.Source = EDragSource::Wand;
			Payload.WandId = Wand.InstanceId;
			Payload.Label = FText::FromString(Definition->DisplayName);
			Payload.Badge = FText::FromString(TEXT("杖"));
			Payload.IconBrush = GetIconBrush(Definition->Icon);
			Payload.Tint = FLinearColor::White;
			const FGuid WandId = Wand.InstanceId;
			WandGrid->AddSlot().Padding(3.0f)
			[
				SNew(SMagicItemSlot)
				.Badge(FText::FromString(TEXT("杖")))
				.IconBrush(Payload.IconBrush)
				.Label(FText::FromString(Definition->DisplayName))
				.Subtitle(FText::GetEmpty())
				.ToolTipContent(BuildWandToolTipContent(*Definition, Wand))
				.Tint(SRGB(144, 94, 48))
				.bSelected(OwnerWidget->GetSelectedWandId() == WandId)
				.DragPayload(Payload)
				.OnLeftClick([Owner = OwnerWidget, WandId]()
				{
					if (Owner.IsValid()) Owner->SelectWand(WandId);
				})
			];
		}
		Content->AddSlot().FillHeight(1.0f).Padding(7.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()[WandGrid]
		];
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				BuildEquipmentRail(Inventory, Registry)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				OutlinedPanel(Content, FMargin(4.0f))
			];
	}

	TSharedRef<SWidget> BuildEquipmentRail(
		const UMatterFluxMagicInventoryComponent& Inventory,
		const FMatterFluxContentRegistry& Registry)
	{
		TSharedRef<SVerticalBox> Equipment = SNew(SVerticalBox);
		TArray<FMatterFluxMagicEquipmentSlotPresentation> EquipmentSlots;
		FMatterFluxMagicWorkbenchInteraction::BuildEquipmentSlotPresentations(
			EquipmentSlots);
		for (const FMatterFluxMagicEquipmentSlotPresentation& SlotPresentation
			: EquipmentSlots)
		{
			const int32 SlotIndex = SlotPresentation.SlotIndex;
			const FGuid EquippedId = Inventory.GetEquippedWands().IsValidIndex(SlotIndex)
				? Inventory.GetEquippedWands()[SlotIndex]
				: FGuid();
			const FMatterFluxOwnedWand* EquippedWand = Inventory.FindWand(EquippedId);
			const FMatterFluxWandDefinition* EquippedDefinition = EquippedWand
				? Registry.Wands.Find(EquippedWand->DefinitionId)
				: nullptr;
			FDragPayload Payload;
			if (EquippedId.IsValid())
			{
				Payload.Source = EDragSource::Wand;
				Payload.WandId = EquippedId;
				Payload.Label = FText::FromString(
					EquippedDefinition ? EquippedDefinition->DisplayName : TEXT("法杖"));
				Payload.IconBrush = EquippedDefinition
					? GetIconBrush(EquippedDefinition->Icon)
					: nullptr;
			}
			const FSlateBrush* EquipmentIcon = EquippedId.IsValid()
				? Payload.IconBrush
				: GetIconBrush(TEXT("paper/add_sign"));
			const int32 TargetSlot = SlotIndex;
			const bool bIsActiveSlot = Inventory.GetActiveEquipmentSlot() == SlotIndex;
			const bool bIsEditedSlot = EquippedId.IsValid()
				&& OwnerWidget->GetSelectedWandId() == EquippedId;
			TSharedPtr<SWidget> WandRichToolTip;
			if (EquippedWand && EquippedDefinition)
			{
				WandRichToolTip = BuildWandToolTipContent(
					*EquippedDefinition,
					*EquippedWand);
			}
			Equipment->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(4.0f)
			[
				SNew(SMagicItemSlot)
				.Badge(FText::FromString(EquippedId.IsValid() ? TEXT("杖") : TEXT("+")))
				.IconBrush(EquipmentIcon)
				.Label(FText::FromString(
					EquippedDefinition ? EquippedDefinition->DisplayName : TEXT("拖入法杖")))
				.Subtitle(FText::FromString(SlotPresentation.KeyBadge))
				.ToolTipContent(WandRichToolTip)
				.ToolTip(WandRichToolTip.IsValid()
					? FText::GetEmpty()
					: FText::FromString(FString::Printf(
						TEXT("将法杖拖入此处，绑定到%s。"),
						*SlotPresentation.KeyLabel)))
				.Tint(EquippedId.IsValid() ? SRGB(144, 94, 48) : SRGB(126, 123, 115))
				.bSelected(bIsEditedSlot)
				.DragPayload(Payload)
				.OnLeftClick([Owner = OwnerWidget, EquippedId, TargetSlot, bIsActiveSlot]()
				{
					if (!Owner.IsValid()) return;
					if (!bIsActiveSlot)
					{
						FMatterFluxMagicEdit Edit;
						Edit.Type = EMatterFluxMagicEditType::SelectEquipmentSlot;
						Edit.EquipmentSlot = TargetSlot;
						Owner->SubmitEdit(Edit);
					}
					if (EquippedId.IsValid()) Owner->SelectWand(EquippedId);
				})
				.OnRightClick([Owner = OwnerWidget, TargetSlot, EquippedId]()
				{
					if (!Owner.IsValid() || !EquippedId.IsValid()) return;
					FMatterFluxMagicEdit Edit;
					Edit.Type = EMatterFluxMagicEditType::UnequipWand;
					Edit.EquipmentSlot = TargetSlot;
					Owner->SubmitEdit(Edit);
				})
				.OnDropPayload([Owner = OwnerWidget, TargetSlot](const FDragPayload& Dropped)
				{
					if (!Owner.IsValid() || Dropped.Source != EDragSource::Wand) return false;
					FMatterFluxMagicEdit Edit;
					Edit.Type = EMatterFluxMagicEditType::EquipWand;
					Edit.WandId = Dropped.WandId;
					Edit.EquipmentSlot = TargetSlot;
					Owner->SubmitEdit(Edit);
					Owner->SelectWand(Dropped.WandId);
					return true;
				})
				.CanAcceptPayload([](const FDragPayload& Dropped)
				{
					return Dropped.Source == EDragSource::Wand
						&& Dropped.WandId.IsValid();
				})
			];
		}
		return SNew(SBox)
			.WidthOverride(93.0f)
			[
				OutlinedPanel(Equipment, FMargin(3.0f))
			];
	}

	TSharedRef<SWidget> BuildSpellBag(
		const UMatterFluxMagicInventoryComponent& Inventory,
		const FMatterFluxContentRegistry& Registry)
	{
		TSharedRef<SWrapBox> SpellGrid = SNew(SWrapBox).UseAllottedSize(true);
		for (const FMatterFluxOwnedSpell& OwnedSpell : Inventory.GetOwnedSpells())
		{
			const FMatterFluxSpellDefinition* Definition =
				Registry.Spells.Find(OwnedSpell.SpellId);
			if (!Definition) continue;
			FDragPayload Payload;
			Payload.Source = EDragSource::SpellInventory;
			Payload.SpellId = OwnedSpell.SpellId;
			Payload.Label = FText::FromString(Definition->DisplayName);
			Payload.Badge = SpellBadge(Definition->Kind);
			Payload.Subtitle = OwnedSpell.Quantity > 1
				? FText::AsNumber(OwnedSpell.Quantity)
				: FText::GetEmpty();
			Payload.IconBrush = GetIconBrush(Definition->Icon);
			Payload.Tint = SpellColor(Definition->Kind);
			const FName SpellId = OwnedSpell.SpellId;
			SpellGrid->AddSlot().Padding(3.0f)
			[
				SNew(SMagicItemSlot)
				.Badge(SpellBadge(Definition->Kind))
				.IconBrush(Payload.IconBrush)
				.Label(FText::FromString(Definition->DisplayName))
				.Subtitle(Payload.Subtitle)
				.ToolTipContent(BuildSpellToolTipContent(*Definition))
				.Tint(SpellColor(Definition->Kind))
				.bSelected(OwnerWidget->GetPendingSpell() == SpellId)
				.DragPayload(Payload)
				.OnLeftClick([Owner = OwnerWidget, SpellId]()
				{
					if (Owner.IsValid()) Owner->SetPendingSpell(SpellId);
				})
			];
		}
		return SNew(SBox)
			.WidthOverride(280.0f)
			[
				OutlinedPanel(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().FillHeight(1.0f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()[SpellGrid]
					],
					FMargin(8.0f))
			];
	}

	TSharedRef<SWidget> BuildSpellEditorPage(
		const UMatterFluxMagicInventoryComponent& Inventory,
		const FMatterFluxContentRegistry& Registry,
		const FMatterFluxOwnedWand* Wand,
		const FMatterFluxWandDefinition* Definition)
	{
		return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 10.0f, 0.0f)
		[
			BuildEquipmentRail(Inventory, Registry)
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 10.0f, 0.0f)
		[
			OutlinedPanel(
				BuildProgramEditor(Registry, Wand, Definition),
				FMargin(14.0f))
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			BuildSpellBag(Inventory, Registry)
		];
	}

	TSharedRef<SWidget> BuildProgramEditor(
		const FMatterFluxContentRegistry& Registry,
		const FMatterFluxOwnedWand* Wand,
		const FMatterFluxWandDefinition* Definition)
	{
		TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
		if (!Wand || !Definition)
		{
			Content->AddSlot().FillHeight(1.0f).VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(SpellIconSize)
				.HeightOverride(SpellIconSize)
				.ToolTipText(FText::FromString(TEXT("选择一根法杖")))
				[
					SNew(SImage)
					.Image(GetIconBrush(TEXT("paper/add_sign")))
				]
			];
			return Content;
		}

		Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				Heading(Definition->DisplayName)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(
					TEXT("%.0f / %.0f"),
					Wand->Mana,
					Definition->ManaMax)))
				.Font(WorkbenchFont(11, true))
				.ColorAndOpacity(Ink)
			]
		];
		Content->AddSlot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 8.0f)
		[
			SNew(SMatterFluxProgressBar)
			.Height(15.0f)
			.Percent(Definition->ManaMax > 0.0f
				? Wand->Mana / Definition->ManaMax
				: 0.0f)
		];

		const auto BuildSpellSlot = [this, &Registry, Wand](const int32 SlotIndex)
			-> TSharedRef<SWidget>
		{
			const FName SpellId = Wand->SpellSlots[SlotIndex];
			const FMatterFluxSpellDefinition* Spell =
				Registry.Spells.Find(SpellId);
			FDragPayload Payload;
			if (Spell)
			{
				Payload.Source = EDragSource::DeckSpell;
				Payload.WandId = Wand->InstanceId;
				Payload.SpellId = SpellId;
				Payload.SpellSlot = SlotIndex;
				Payload.Label = FText::FromString(Spell->DisplayName);
				Payload.Badge = SpellBadge(Spell->Kind);
				Payload.IconBrush = GetIconBrush(Spell->Icon);
				Payload.Tint = SpellColor(Spell->Kind);
			}
			TSharedPtr<SWidget> RichToolTip;
			if (Spell)
			{
				RichToolTip = BuildSpellToolTipContent(*Spell);
			}
			const FGuid WandId = Wand->InstanceId;
			const int32 TargetIndex = SlotIndex;
			return SNew(SMagicItemSlot)
					.Badge(Spell
						? SpellBadge(Spell->Kind)
						: FText::FromString(TEXT("＋")))
					.IconBrush(Spell
						? Payload.IconBrush
						: GetIconBrush(TEXT("paper/add_sign")))
					.Label(FText::FromString(Spell ? Spell->DisplayName : TEXT("+ 法术")))
					.Subtitle(FText::GetEmpty())
					.ToolTipContent(RichToolTip)
					.ToolTip(Spell
						? FText::GetEmpty()
						: FText::FromString(TEXT("把法术拖到这里，或先选择法术再点击该槽。")))
					.Tint(Spell ? SpellColor(Spell->Kind) : SRGB(126, 123, 115))
					.DragPayload(Payload)
					.OnLeftClick([Owner = OwnerWidget, WandId, TargetIndex]()
					{
						if (!Owner.IsValid() || Owner->GetPendingSpell().IsNone()) return;
						FMatterFluxMagicEdit Edit;
						Edit.Type = EMatterFluxMagicEditType::AssignSpell;
						Edit.WandId = WandId;
						Edit.SpellId = Owner->GetPendingSpell();
						Edit.ToSpellSlot = TargetIndex;
						Owner->SubmitEdit(Edit);
						Owner->SetPendingSpell(NAME_None);
					})
					.OnRightClick([Owner = OwnerWidget, WandId, TargetIndex, SpellId]()
					{
						if (!Owner.IsValid() || SpellId.IsNone()) return;
						FMatterFluxMagicEdit Edit;
						Edit.Type = EMatterFluxMagicEditType::RemoveSpell;
						Edit.WandId = WandId;
						Edit.FromSpellSlot = TargetIndex;
						Owner->SubmitEdit(Edit);
					})
					.CanAcceptPayload([WandId, TargetIndex](const FDragPayload& Dropped)
					{
						FMatterFluxMagicDragPayload StablePayload;
						StablePayload.Source = Dropped.Source == EDragSource::SpellInventory
							? EMatterFluxMagicDragSource::SpellInventory
							: Dropped.Source == EDragSource::DeckSpell
								? EMatterFluxMagicDragSource::WandSpellSlot
								: EMatterFluxMagicDragSource::None;
						StablePayload.WandId = Dropped.WandId;
						StablePayload.SpellId = Dropped.SpellId;
						StablePayload.SpellSlot = Dropped.SpellSlot;
						FMatterFluxMagicEdit Ignored;
						return FMatterFluxMagicWorkbenchInteraction::ResolveSpellDrop(
							StablePayload, WandId, TargetIndex, Ignored);
					})
					.OnDropPayload([Owner = OwnerWidget, WandId, TargetIndex](const FDragPayload& Dropped)
					{
						if (!Owner.IsValid()) return false;
						FMatterFluxMagicDragPayload StablePayload;
						StablePayload.Source = Dropped.Source == EDragSource::SpellInventory
							? EMatterFluxMagicDragSource::SpellInventory
							: Dropped.Source == EDragSource::DeckSpell
								? EMatterFluxMagicDragSource::WandSpellSlot
								: EMatterFluxMagicDragSource::None;
						StablePayload.WandId = Dropped.WandId;
						StablePayload.SpellId = Dropped.SpellId;
						StablePayload.SpellSlot = Dropped.SpellSlot;
						FMatterFluxMagicEdit Edit;
						if (!FMatterFluxMagicWorkbenchInteraction::ResolveSpellDrop(
							StablePayload, WandId, TargetIndex, Edit))
						{
							return false;
						}
						Owner->SubmitEdit(Edit);
						return true;
					});
		};

		FMatterFluxSpellProgramLayout Layout;
		FString LayoutError;
		if (!FMatterFluxSpellProgramLayoutBuilder::Build(
			Registry,
			Wand->SpellSlots,
			Layout,
			LayoutError))
		{
			Content->AddSlot().FillHeight(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(LayoutError))
				.Font(WorkbenchFont(11))
				.ColorAndOpacity(SpellTrigger)
			];
			return Content;
		}

		const auto BuildProgramNode = [&BuildSpellSlot](
			const FMatterFluxSpellProgramNode& Node)
			-> TSharedRef<SWidget>
		{
			return SNew(SBox)
				.WidthOverride(ProgramNodeWidth)
				.HeightOverride(ProgramNodeHeight)
				[
					BuildSpellSlot(Node.SlotIndex)
				];
		};

		TArray<const FMatterFluxSpellProgramNode*> RootNodes;
		TMap<int32, TArray<const FMatterFluxSpellProgramNode*>> ChildrenByParent;
		for (const FMatterFluxSpellProgramColumn& Column : Layout.Columns)
		{
			for (const FMatterFluxSpellProgramNode& Node : Column.Nodes)
			{
				if (Node.IsRoot())
				{
					RootNodes.Add(&Node);
				}
				else
				{
					ChildrenByParent.FindOrAdd(Node.ParentSlotIndex).Add(&Node);
				}
			}
		}

		struct FProgramNodePlacement
		{
			const FMatterFluxSpellProgramNode* Node = nullptr;
			FVector2D Position = FVector2D::ZeroVector;
		};

		const auto BuildProgramTree = [
			&BuildProgramNode,
			&ChildrenByParent](const FMatterFluxSpellProgramNode& Root)
			-> TSharedRef<SWidget>
		{
			TMap<int32, float> SubtreeHeights;
			TFunction<float(const FMatterFluxSpellProgramNode&)> MeasureSubtree;
			MeasureSubtree = [
				&MeasureSubtree,
				&SubtreeHeights,
				&ChildrenByParent](const FMatterFluxSpellProgramNode& Node)
				-> float
			{
				float ChildrenHeight = 0.0f;
				if (const TArray<const FMatterFluxSpellProgramNode*>* Children =
					ChildrenByParent.Find(Node.SlotIndex))
				{
					for (int32 ChildIndex = 0;
						ChildIndex < Children->Num();
						++ChildIndex)
					{
						ChildrenHeight += MeasureSubtree(*(*Children)[ChildIndex]);
						if (ChildIndex + 1 < Children->Num())
						{
							ChildrenHeight += ProgramVerticalGap;
						}
					}
				}
				const float Height = FMath::Max(
					ProgramNodeHeight,
					ChildrenHeight);
				SubtreeHeights.Add(Node.SlotIndex, Height);
				return Height;
			};

			const float TreeHeight = MeasureSubtree(Root);
			TArray<FProgramNodePlacement> Placements;
			TMap<int32, FVector2D> PositionBySlot;
			int32 MaxDepth = 0;
			TFunction<void(
				const FMatterFluxSpellProgramNode&,
				int32,
				float)> PlaceSubtree;
			PlaceSubtree = [
				&PlaceSubtree,
				&Placements,
				&PositionBySlot,
				&MaxDepth,
				&SubtreeHeights,
				&ChildrenByParent](
				const FMatterFluxSpellProgramNode& Node,
				const int32 Depth,
				const float Top)
			{
				const float SubtreeHeight =
					SubtreeHeights.FindChecked(Node.SlotIndex);
				const FVector2D Position(
					Depth * (ProgramNodeWidth + ProgramHorizontalGap),
					Top + (SubtreeHeight - ProgramNodeHeight) * 0.5f);
				Placements.Add({ &Node, Position });
				PositionBySlot.Add(Node.SlotIndex, Position);
				MaxDepth = FMath::Max(MaxDepth, Depth);

				const TArray<const FMatterFluxSpellProgramNode*>* Children =
					ChildrenByParent.Find(Node.SlotIndex);
				if (!Children || Children->IsEmpty())
				{
					return;
				}
				float ChildrenHeight = 0.0f;
				for (int32 ChildIndex = 0;
					ChildIndex < Children->Num();
					++ChildIndex)
				{
					ChildrenHeight += SubtreeHeights.FindChecked(
						(*Children)[ChildIndex]->SlotIndex);
					if (ChildIndex + 1 < Children->Num())
					{
						ChildrenHeight += ProgramVerticalGap;
					}
				}
				float ChildTop = Top + (SubtreeHeight - ChildrenHeight) * 0.5f;
				for (const FMatterFluxSpellProgramNode* Child : *Children)
				{
					PlaceSubtree(*Child, Depth + 1, ChildTop);
					ChildTop += SubtreeHeights.FindChecked(Child->SlotIndex)
						+ ProgramVerticalGap;
				}
			};
			PlaceSubtree(Root, 0, 0.0f);

			TSharedRef<SConstraintCanvas> NodeCanvas = SNew(SConstraintCanvas);
			for (const FProgramNodePlacement& Placement : Placements)
			{
				NodeCanvas->AddSlot()
					.Anchors(FAnchors(0.0f, 0.0f))
					.Alignment(FVector2D::ZeroVector)
					.Offset(FMargin(
						Placement.Position.X,
						Placement.Position.Y,
						ProgramNodeWidth,
						ProgramNodeHeight))
					[BuildProgramNode(*Placement.Node)];
			}

			const FVector2D SlotCenterOffset(
				ProgramNodeWidth * 0.5f,
				ProgramNodeHeight * 0.5f);
			TArray<FMagicTreeLine> Lines;
			for (const FProgramNodePlacement& Placement : Placements)
			{
				if (Placement.Node->IsRoot())
				{
					continue;
				}
				const FVector2D* ParentPosition = PositionBySlot.Find(
					Placement.Node->ParentSlotIndex);
				if (ParentPosition)
				{
					FMagicTreeLine& LineToAdd = Lines.AddDefaulted_GetRef();
					LineToAdd.Start = FVector2f(
						*ParentPosition + SlotCenterOffset);
					LineToAdd.End = FVector2f(
						Placement.Position + SlotCenterOffset);
				}
			}

			const FVector2D TreeSize(
				(MaxDepth + 1) * ProgramNodeWidth
					+ MaxDepth * ProgramHorizontalGap,
				TreeHeight);
			return SNew(SMagicTreeCanvas)
				.DesiredSize(TreeSize)
				.Lines(MoveTemp(Lines))
				[NodeCanvas];
		};

		TSharedRef<SVerticalBox> ProgramTrees = SNew(SVerticalBox);
		for (const FMatterFluxSpellProgramNode* Root : RootNodes)
		{
			ProgramTrees->AddSlot().AutoHeight().Padding(
				0.0f, 0.0f, 0.0f, 12.0f)
			[BuildProgramTree(*Root)];
		}

		TSharedRef<SVerticalBox> Program = SNew(SVerticalBox);
		Program->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 12.0f)
		[
			SNew(SScrollBox)
			.Orientation(Orient_Horizontal)
			+ SScrollBox::Slot()[ProgramTrees]
		];

		if (!Layout.ReserveSlotIndices.IsEmpty())
		{
			TSharedRef<SWrapBox> Reserve =
				SNew(SWrapBox).UseAllottedSize(true);
			for (const int32 SlotIndex : Layout.ReserveSlotIndices)
			{
				Reserve->AddSlot().Padding(3.0f)[BuildSpellSlot(SlotIndex)];
			}
			Program->AddSlot().AutoHeight()[Reserve];
		}
		Content->AddSlot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			.Orientation(Orient_Vertical)
			+ SScrollBox::Slot()[Program]
		];
		return Content;
	}

	TSharedRef<SWidget> BuildWandDetails(
		const FMatterFluxContentRegistry& Registry,
		const FMatterFluxOwnedWand& Wand,
		const FMatterFluxWandDefinition& Definition)
	{
		FMatterFluxWandProgramState State;
		State.Mana = Wand.Mana;
		State.DeckCursor = Wand.DeckCursor;
		State.CastSerial = Wand.CastSerial;
		FMatterFluxWandCastPlan Preview;
		FString PreviewError;
		const bool bPreviewValid = FMatterFluxWandProgram::Evaluate(
			Registry,
			Definition.Id,
			Wand.SpellSlots,
			State,
			0x4d4658,
			Preview,
			PreviewError);
		float RootDamage = 0.0f;
		for (const FMatterFluxMagicProjectilePlan& Projectile
			: Preview.Projectiles)
		{
			RootDamage += Projectile.Damage;
		}
		const FString PreviewText = bPreviewValid
			? FString::Printf(
				TEXT("%d 枚飞弹  ·  %d 个施法者效果\n消耗法力 %.0f   飞弹总伤害 %.0f\n施法 %.2f秒   充能 %.2f秒\n下一抽取位置 %d%s"),
				Preview.Projectiles.Num(),
				Preview.CasterEffects.Num(),
				Preview.ManaSpent,
				RootDamage,
				Preview.CastDelay,
				Preview.RechargeTime,
				Preview.NextState.DeckCursor,
				Definition.bShuffle ? TEXT("  （乱序示例）") : TEXT(""))
			: FString::Printf(TEXT("当前程序无法施放：\n%s"), *PreviewError);

		const UMatterFluxMagicWorkbenchWidget* Widget = OwnerWidget.Get();
		const FMatterFluxSpellDefinition* Inspected = Widget
			? Registry.Spells.Find(Widget->GetPendingSpell())
			: nullptr;
		const FString InspectorText = Inspected
			? FString::Printf(
				TEXT("%s\n%s\n\n法力 %.0f   伤害/强度 %.0f   抽取 %d\n速度 %.0f   半径 %.0f"),
				*Inspected->DisplayName,
				*Inspected->Description,
				Inspected->ManaCost,
				Inspected->Damage,
				Inspected->DrawCount,
				Inspected->Speed,
				Inspected->Radius)
			: FString::Printf(
				TEXT("%s\n%s\n\n容量 %d   抽取 %d   %s\n法力回复 %.0f/秒   散布 %.1f 度"),
				*Definition.DisplayName,
				*Definition.Description,
				Definition.Capacity,
				Definition.DrawCount,
				Definition.bShuffle ? TEXT("乱序") : TEXT("顺序"),
				Definition.ManaRechargePerSecond,
				Definition.Spread);

		const auto DetailPanel = [this](
			const FString& Title,
			const FString& Text,
			const FLinearColor Accent)
		{
			(void)Accent;
			return OutlinedPanel(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
						.Text(FText::FromString(Title))
						.Font(WorkbenchFont(16, true))
						.ColorAndOpacity(Ink)
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
						.Text(FText::FromString(Text))
						.Font(WorkbenchFont(14))
						.AutoWrapText(true)
						.ColorAndOpacity(Ink)
				],
				FMargin(14.0f));
		};

		return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 7.0f, 0.0f)
		[
			DetailPanel(
				Definition.bShuffle
					? TEXT("程序预览 / 乱序示例")
					: TEXT("下一次施法预览"),
				PreviewText,
				bPreviewValid ? SpellMulticast : SpellTrigger)
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(7.0f, 0.0f, 0.0f, 0.0f)
		[
			DetailPanel(
				Inspected ? TEXT("已选法术") : TEXT("法杖详情"),
				InspectorText,
				Inspected ? SpellColor(Inspected->Kind) : Selected)
		];
	}

	TWeakObjectPtr<UMatterFluxMagicWorkbenchWidget> OwnerWidget;
	TMap<FString, TSharedPtr<FSlateDynamicImageBrush>> IconBrushes;
};

}

TSharedRef<SWidget> MatterFluxMagicUI::CreateWorkbench(
	TWeakObjectPtr<UMatterFluxMagicWorkbenchWidget> OwnerWidget)
{
	return SNew(SMatterFluxMagicWorkbench)
		.OwnerWidget(OwnerWidget);
}

void MatterFluxMagicUI::RefreshWorkbench(
	const TSharedPtr<SWidget>& Workbench)
{
	if (const TSharedPtr<SMatterFluxMagicWorkbench> TypedWorkbench =
		StaticCastSharedPtr<SMatterFluxMagicWorkbench>(Workbench))
	{
		TypedWorkbench->Refresh();
	}
}
