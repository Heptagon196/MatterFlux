#include "UI/MatterFluxShellSlate.h"
#include "UI/MatterFluxShellWidget.h"

#include "EngineUtils.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Save/MatterFluxSaveSubsystem.h"
#include "Save/MatterFluxSaveGame.h"
#include "UI/MatterFluxProgressBar.h"
#include "UI/MatterFluxPaperStyle.h"
#include "UI/MatterFluxPaperWindow.h"
#include "UI/MatterFluxSettingsPanel.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace MatterFluxShellUI
{
	using namespace MatterFlux::UI::Paper;
	const FLinearColor& Paper = Surface;

	const TArray<FMatterFluxSaveSlotInfo>& EmptySaveSlots()
	{
		static const TArray<FMatterFluxSaveSlotInfo> Empty;
		return Empty;
	}

	TSharedRef<SWidget> Button(
		const FString& Label,
		TFunction<FReply()> OnClicked,
		const FString& ToolTip = FString(),
		const bool bEnabled = true,
		const float MinWidth = 112.0f)
	{
		TSharedRef<SButton> SlateButton =
			SNew(SButton)
				.ButtonStyle(&FlatButtonStyle())
				.ContentPadding(FMargin(10.0f, 0.0f))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.IsEnabled(bEnabled)
				.OnClicked_Lambda([OnClicked = MoveTemp(OnClicked)]()
				{
					return OnClicked ? OnClicked() : FReply::Handled();
				})
				[
					SNew(STextBlock)
						.Text(FText::FromString(Label))
						.Font(Font(12, true))
						.ColorAndOpacity(Ink)
						.Justification(ETextJustify::Center)
				];
		if (!ToolTip.IsEmpty())
		{
			SlateButton->SetToolTipText(FText::FromString(ToolTip));
		}
		return SNew(SBox)
			.WidthOverride(MinWidth)
			.HeightOverride(38.0f)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
				.BorderBackgroundColor(Ink)
				.Padding(Keyline)
				[
					SlateButton
				]
			];
	}

	FString SlotDisplayName(const FMatterFluxSaveSlotInfo& Slot)
	{
		return Slot.DisplayName.IsEmpty()
			? FString::Printf(TEXT("存档 %d"), Slot.SlotIndex + 1)
			: Slot.DisplayName;
	}

	TSharedRef<SWidget> SlotNameWidget(
		UMatterFluxShellWidget* Owner,
		const FMatterFluxSaveSlotInfo& Slot)
	{
		if (Owner && Owner->IsRenamingSlot(Slot.SlotIndex))
		{
			return Outline(
				SNew(SBox)
				.HeightOverride(28.0f)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Center)
				[
					SNew(SEditableText)
					.Text(FText::FromString(Owner->GetRenameDraft()))
					.Font(Font(11, true))
					.ColorAndOpacity(Ink)
					.Justification(ETextJustify::Center)
					.SelectAllTextWhenFocused(true)
					.OnTextChanged_Lambda([Owner](const FText& Text)
					{
						Owner->SetRenameDraft(Text.ToString());
					})
					.OnTextCommitted_Lambda(
						[Owner](const FText& Text, const ETextCommit::Type Commit)
						{
							Owner->SetRenameDraft(Text.ToString());
							if (Commit == ETextCommit::OnEnter)
							{
								Owner->CommitRenameSlot();
							}
						})
				],
				FMargin(6.0f, 3.0f));
		}
		return SNew(STextBlock)
			.Text(FText::FromString(SlotDisplayName(Slot)))
			.Font(Font(12, true)).ColorAndOpacity(Ink);
	}

	void AddSlotManagementButtons(
		const TSharedRef<SHorizontalBox>& Row,
		UMatterFluxShellWidget* Owner,
		const int32 SlotIndex)
	{
		if (Owner && Owner->IsRenamingSlot(SlotIndex))
		{
			Row->AddSlot().AutoWidth().Padding(4.0f, 0.0f)
			[Button(TEXT("确定"), [Owner]()
			{
				Owner->CommitRenameSlot();
				return FReply::Handled();
			}, TEXT("保存新的存档名称"), true, 62.0f)];
			Row->AddSlot().AutoWidth()
			[Button(TEXT("取消"), [Owner]()
			{
				Owner->CancelRenameSlot();
				return FReply::Handled();
			}, FString(), true, 62.0f)];
			return;
		}
		Row->AddSlot().AutoWidth().Padding(4.0f, 0.0f)
		[Button(TEXT("复制"), [Owner, SlotIndex]()
		{
			Owner->RequestDuplicateSlot(SlotIndex);
			return FReply::Handled();
		}, TEXT("复制为一个新的独立存档"), true, 62.0f)];
		Row->AddSlot().AutoWidth().Padding(4.0f, 0.0f)
		[Button(TEXT("重命名"), [Owner, SlotIndex]()
		{
			Owner->BeginRenameSlot(SlotIndex);
			return FReply::Handled();
		}, TEXT("修改菜单中显示的存档名称"), true, 76.0f)];
		Row->AddSlot().AutoWidth()
		[Button(TEXT("删除"), [Owner, SlotIndex]()
		{
			Owner->RequestDeleteSlot(SlotIndex);
			return FReply::Handled();
		}, TEXT("永久删除这个存档"), true, 62.0f)];
	}

	class SMatterFluxShell : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SMatterFluxShell) {}
			SLATE_ARGUMENT(TWeakObjectPtr<UMatterFluxShellWidget>, OwnerWidget)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args)
		{
			OwnerWidget = Args._OwnerWidget;
			const FString BackgroundPath = FPaths::Combine(
				FPaths::ProjectContentDir(),
				TEXT("UI/TitleBackgroundSeed1337.png"));
			if (FPaths::FileExists(BackgroundPath))
			{
				TitleBackgroundBrush = MakeShared<FSlateDynamicImageBrush>(
					FName(*BackgroundPath),
					FVector2D(1707.0f, 1019.0f));
			}
			Refresh();
		}

		void Refresh()
		{
			ChildSlot.AttachWidget(BuildRoot());
			Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
		}

	private:
		TSharedRef<SWidget> BuildRoot()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			if (!Owner)
			{
				return SNullWidget::NullWidget;
			}
			TSharedRef<SOverlay> Root = SNew(SOverlay);
			if (Owner->IsStartMenuOpen() && TitleBackgroundBrush.IsValid())
			{
				Root->AddSlot()
				[
					SNew(SImage)
					.Image(TitleBackgroundBrush.Get())
				];
			}
			if (Owner->IsMenuOpen())
			{
				const bool bUseWorkbenchFrame =
					Owner->GetView() == EMatterFluxShellView::Settings;
				Root->AddSlot()
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
					.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.64f))
				];
				Root->AddSlot()
				.HAlign(bUseWorkbenchFrame ? HAlign_Fill : HAlign_Center)
				.VAlign(bUseWorkbenchFrame ? VAlign_Fill : VAlign_Center)
				.Padding(24.0f, 64.0f, 24.0f, 24.0f)
				[BuildCurrentMenu()];
			}
			Root->AddSlot()
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Top)
			.Padding(18.0f)
			[BuildTopBar()];

			if (UMatterFluxSaveSubsystem* Save = Owner->GetSaveSubsystem();
				Save && Save->IsBusy())
			{
				Root->AddSlot()
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
					.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.76f))
				];
				Root->AddSlot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[BuildProgressPanel()];
			}
			return Root;
		}

		TSharedRef<SWidget> BuildTopBar()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			const bool bStartMenuOpen = Owner && Owner->IsStartMenuOpen();
			int32 Seed = 0;
			if (UWorld* World = Owner ? Owner->GetWorld() : nullptr)
			{
				for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
				{
					Seed = It->GetMapSeed();
					break;
				}
			}
			TSharedRef<SHorizontalBox> Bar = SNew(SHorizontalBox);
			Bar->AddSlot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("MATTERFLUX")))
				.Font(Font(14, true))
				.ColorAndOpacity(Ink)
			];
			Bar->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(18.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("世界种子  %d"), Seed)))
				.Font(Font(10))
					.ColorAndOpacity(Muted)
			];
			TSharedRef<SHorizontalBox> CoinDisplay = SNew(SHorizontalBox);
			CoinDisplay->AddSlot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					.Text(FText::FromString(TEXT("金币")))
					.Font(Font(10))
					.ColorAndOpacity(Muted)
			];
			CoinDisplay->AddSlot().AutoWidth().VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
					.Text(FText::AsNumber(
						Owner ? Owner->GetOwnedCoinQuantity() : 0))
					.Font(Font(14, true))
					.ColorAndOpacity(Ink)
			];
			Bar->AddSlot().AutoWidth().VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(SBox)
					.WidthOverride(108.0f)
					.HeightOverride(34.0f)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[Outline(CoinDisplay, FMargin(10.0f, 2.0f))]
			];
			if (Owner && !Owner->IsStartMenuOpen())
			{
				Bar->AddSlot().AutoWidth().Padding(4.0f, 0.0f)
				[Button(TEXT("法术"), [Owner]() { Owner->RequestMagicWorkbench(); return FReply::Handled(); }, TEXT("打开法杖与法术编辑"), true, 76.0f)];
				Bar->AddSlot().AutoWidth().Padding(4.0f, 0.0f)
				[Button(TEXT("保存"), [Owner]() { Owner->ShowSaveSlots(); return FReply::Handled(); }, TEXT("保存当前世界"), true, 76.0f)];
			}
			Bar->AddSlot().AutoWidth().Padding(4.0f, 0.0f)
			[Button(TEXT("设置"), [Owner]() { Owner->ShowSettings(); return FReply::Handled(); }, bStartMenuOpen ? FString() : FString(TEXT("画面、声音与窗口设置")), true, 76.0f)];
			Bar->AddSlot().AutoWidth().Padding(4.0f, 0.0f)
			[
				Owner && Owner->IsStartMenuOpen()
					? Button(TEXT("退出"), [Owner]()
					{
						Owner->RequestQuit();
						return FReply::Handled();
					}, FString(), true, 76.0f)
					: Button(Owner && Owner->IsMenuOpen() ? TEXT("关闭") : TEXT("菜单"), [Owner]()
					{
						if (Owner)
						{
							Owner->IsMenuOpen()
								? Owner->CloseMenus()
								: Owner->ShowPauseMenu();
						}
						return FReply::Handled();
					}, TEXT("打开或关闭游戏菜单"), true, 76.0f)
			];
			return SNew(SBox).HeightOverride(46.0f)
			[
				Outline(Bar, FMargin(12.0f, 4.0f))
			];
		}

		TSharedRef<SWidget> BuildCurrentMenu()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			if (!Owner)
			{
				return SNullWidget::NullWidget;
			}
			switch (Owner->GetView())
			{
			case EMatterFluxShellView::StartMenu: return BuildStartMenu();
			case EMatterFluxShellView::SinglePlayerMenu: return BuildSinglePlayerMenu();
			case EMatterFluxShellView::MultiplayerMenu: return BuildMultiplayerMenu();
			case EMatterFluxShellView::CreateRoomMenu: return BuildCreateRoomMenu();
			case EMatterFluxShellView::JoinRoomMenu: return BuildJoinRoomMenu();
			case EMatterFluxShellView::Settings: return BuildSettings();
			case EMatterFluxShellView::SaveSlots: return BuildSlots(false);
			case EMatterFluxShellView::LoadSlots: return BuildSlots(true);
			default: return BuildPauseMenu();
			}
		}

		TSharedRef<SWidget> BuildStartMenu()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
			Content->AddSlot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("MATTERFLUX")))
				.Font(Font(28, true)).ColorAndOpacity(Ink)
			];
			Content->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 4.0f, 0.0f, 20.0f)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("可破坏的像素物质世界")))
				.Font(Font(10)).ColorAndOpacity(Muted)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("故事模式"), [Owner]() { Owner->RequestStoryMode(); return FReply::Handled(); }, FString(), true, 260.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("自由模式"), [Owner]() { Owner->ShowSinglePlayerMenu(); return FReply::Handled(); }, FString(), true, 260.0f)];
			if (UMatterFluxShellWidget::IsMultiplayerEntryEnabled())
			{
				Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
				[Button(TEXT("多人游戏"), [Owner]() { Owner->ShowMultiplayerMenu(); return FReply::Handled(); }, FString(), true, 260.0f)];
			}
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("设置"), [Owner]() { Owner->ShowSettings(); return FReply::Handled(); }, FString(), true, 260.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("退出游戏"), [Owner]() { Owner->RequestQuit(); return FReply::Handled(); }, FString(), true, 260.0f)];
			AddNotice(Content);
			return SNew(SBox).WidthOverride(420.0f)[Outline(Content, FMargin(34.0f, 28.0f))];
		}

		TSharedRef<SWidget> BuildSinglePlayerMenu()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			UMatterFluxSaveSubsystem* Save = Owner ? Owner->GetSaveSubsystem() : nullptr;
			TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 16.0f)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("自由模式")))
					.Font(Font(20, true)).ColorAndOpacity(Ink)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("继续游戏"), [Owner]() { Owner->RequestContinue(); return FReply::Handled(); }, TEXT("载入最近一次本地存档"), Save && Save->HasAnySave(), 260.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("新游戏"), [Owner]() { Owner->RequestNewGame(); return FReply::Handled(); }, TEXT("使用随机种子生成新的单人世界"), true, 260.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("载入存档"), [Owner]() { Owner->ShowLoadSlots(); return FReply::Handled(); }, TEXT("选择一个本地存档槽"), Save && Save->HasAnySave(), 260.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 4.0f)
			[Button(TEXT("返回"), [Owner]() { Owner->ShowStartMenu(); return FReply::Handled(); }, FString(), true, 260.0f)];
			AddNotice(Content);
			return SNew(SBox).WidthOverride(420.0f)[Outline(Content, FMargin(34.0f, 28.0f))];
		}

		TSharedRef<SWidget> BuildMultiplayerMenu()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("多人游戏")))
					.Font(Font(20, true)).ColorAndOpacity(Ink)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("当前版本使用 Listen Server 与地址直连。")))
				.Font(Font(10)).ColorAndOpacity(Muted)
				.AutoWrapText(true)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("创建房间"), [Owner]() { Owner->ShowCreateRoomMenu(); return FReply::Handled(); }, TEXT("选择新世界或已有存档并创建 Listen Server"), true, 260.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("加入房间"), [Owner]() { Owner->ShowJoinRoomMenu(); return FReply::Handled(); }, TEXT("输入房主的 IP 地址或主机名"), true, 260.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 4.0f)
			[Button(TEXT("返回"), [Owner]() { Owner->ShowStartMenu(); return FReply::Handled(); }, FString(), true, 260.0f)];
			AddNotice(Content);
			return SNew(SBox).WidthOverride(420.0f)[Outline(Content, FMargin(34.0f, 28.0f))];
		}

		TSharedRef<SWidget> BuildCreateRoomMenu()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			UMatterFluxSaveSubsystem* Save = Owner ? Owner->GetSaveSubsystem() : nullptr;
			TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("创建房间")))
					.Font(Font(20, true)).ColorAndOpacity(Ink)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("选择房间使用的新世界或已有世界存档。")))
				.Font(Font(10)).ColorAndOpacity(Muted)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 12.0f)
			[Button(TEXT("新建世界并创建"), [Owner]() { Owner->RequestHostRoom(); return FReply::Handled(); }, TEXT("生成随机世界并作为 Listen Server 房主进入"), true, 520.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("从已有存档继续")))
					.Font(Font(11, true)).ColorAndOpacity(Ink)
			];
			TSharedRef<SScrollBox> SlotList = SNew(SScrollBox)
				.Orientation(Orient_Vertical)
				.ScrollBarVisibility(EVisibility::Visible)
				.ScrollBarAlwaysVisible(true);
			const TArray<FMatterFluxSaveSlotInfo>& Slots = Save
				? Save->GetSlots()
				: EmptySaveSlots();
			if (Slots.IsEmpty())
			{
				SlotList->AddSlot().Padding(0.0f, 4.0f)
				[
					Outline(
						SNew(SBox)
						.HeightOverride(48.0f)
						.HAlign(HAlign_Fill)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("还没有可继续的存档")))
							.Font(Font(10)).ColorAndOpacity(Muted)
							.Justification(ETextJustify::Center)
						],
						FMargin(12.0f, 0.0f))
				];
			}
			for (const FMatterFluxSaveSlotInfo& Slot : Slots)
			{
				const int32 SlotIndex = Slot.SlotIndex;
				const bool bSelected = Owner
					&& Owner->GetSelectedHostSlotIndex() == SlotIndex;
				TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
				Row->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
					.BorderBackgroundColor(bSelected
						? FLinearColor(0.78f, 0.78f, 0.78f, 1.0f)
						: Paper)
					.Padding(FMargin(8.0f, 5.0f))
					.OnMouseButtonDown_Lambda(
						[Owner, SlotIndex](const FGeometry&, const FPointerEvent&)
						{
							Owner->SelectHostSlot(SlotIndex);
							return FReply::Handled();
						})
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SlotNameWidget(Owner, Slot)
						]
						+ SVerticalBox::Slot().AutoHeight()
						.Padding(0.0f, 3.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(FString::Printf(
								TEXT("%s种子 %d  ·  %s UTC"),
								bSelected ? TEXT("已选择  ·  ") : TEXT(""),
								Slot.MapSeed,
								*Slot.SavedAtUtc.ToString(TEXT("%Y-%m-%d %H:%M")))))
							.Font(Font(9)).ColorAndOpacity(Muted)
						]
					]
				];
				AddSlotManagementButtons(Row, Owner, SlotIndex);
				SlotList->AddSlot().Padding(0.0f, 4.0f)
				[Outline(Row, FMargin(8.0f, 6.0f),
					bSelected
						? FLinearColor(0.78f, 0.78f, 0.78f, 1.0f)
						: Paper)];
			}
			Content->AddSlot().AutoHeight()
			[
				SNew(SBox).HeightOverride(260.0f)[SlotList]
			];
			const int32 SelectedSlotIndex = Owner
				? Owner->GetSelectedHostSlotIndex()
				: INDEX_NONE;
			const bool bHasSelectedSlot = Save
				&& Save->FindSlot(SelectedSlotIndex);
			Content->AddSlot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[Button(TEXT("从选中存档创建房间"),
				[Owner, SelectedSlotIndex]()
				{
					Owner->RequestHostRoom(SelectedSlotIndex);
					return FReply::Handled();
				}, TEXT("载入选中的世界并创建 Listen Server"),
				bHasSelectedSlot, 760.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[Button(TEXT("返回"), [Owner]() { Owner->ShowMultiplayerMenu(); return FReply::Handled(); }, FString(), true, 180.0f)];
			AddNotice(Content);
			return SNew(SBox).WidthOverride(820.0f)
				[Outline(Content, FMargin(24.0f, 20.0f))];
		}

		TSharedRef<SWidget> BuildJoinRoomMenu()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("加入房间")))
					.Font(Font(20, true)).ColorAndOpacity(Ink)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("输入房主地址；省略端口时使用 7777。")))
				.Font(Font(10)).ColorAndOpacity(Muted)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 10.0f)
			[
				Outline(
					SNew(SBox)
					.HeightOverride(30.0f)
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Center)
					[
						SNew(SEditableText)
						.Text(FText::FromString(Owner ? Owner->GetJoinAddress() : FString()))
						.HintText(FText::FromString(TEXT("例如 127.0.0.1:7777")))
						.Font(Font(12))
						.ColorAndOpacity(Ink)
						.Justification(ETextJustify::Center)
						.OnTextChanged_Lambda([Owner](const FText& Text)
						{
							if (Owner) Owner->SetJoinAddress(Text.ToString());
						})
						.OnTextCommitted_Lambda([Owner](const FText& Text, const ETextCommit::Type Commit)
						{
							if (Owner) Owner->SetJoinAddress(Text.ToString());
							if (Owner && Commit == ETextCommit::OnEnter) Owner->RequestJoinRoom();
						})
					],
					FMargin(8.0f, 4.0f))
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("加入"), [Owner]() { Owner->RequestJoinRoom(); return FReply::Handled(); }, TEXT("连接到输入的房主地址"), true, 260.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 4.0f)
			[Button(TEXT("返回"), [Owner]() { Owner->ShowMultiplayerMenu(); return FReply::Handled(); }, FString(), true, 260.0f)];
			AddNotice(Content);
			return SNew(SBox).WidthOverride(440.0f)[Outline(Content, FMargin(34.0f, 28.0f))];
		}

		TSharedRef<SWidget> BuildPauseMenu()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 16.0f)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("游戏菜单")))
				.Font(Font(20, true)).ColorAndOpacity(Ink)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("继续游戏"), [Owner]() { Owner->CloseMenus(); return FReply::Handled(); }, FString(), true, 250.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("保存游戏"), [Owner]() { Owner->ShowSaveSlots(); return FReply::Handled(); }, FString(), true, 250.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("载入游戏"), [Owner]() { Owner->ShowLoadSlots(); return FReply::Handled(); }, FString(), true, 250.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("设置"), [Owner]() { Owner->ShowSettings(); return FReply::Handled(); }, FString(), true, 250.0f)];
			Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
			[Button(TEXT("返回开始菜单"), [Owner]() { Owner->ShowStartMenu(); return FReply::Handled(); }, TEXT("保留当前世界并回到开始菜单"), true, 250.0f)];
			AddNotice(Content);
			return SNew(SBox).WidthOverride(410.0f)[Outline(Content, FMargin(32.0f, 26.0f))];
		}

		TSharedRef<SWidget> BuildSettings()
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			const FText Help = FText::FromString(
				TEXT("使用下拉框、复选框或滑动条调整；改动会立即应用并保存。"));
			TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox);
			Header->AddSlot().AutoWidth().Padding(8.0f, 6.0f, 4.0f, 6.0f)
			[
				SNew(SMatterFluxPaperTab)
				.Label(FText::FromString(TEXT("设置")))
				.bSelected(true)
				.OnClicked([]() { return FReply::Handled(); })
			];
			Header->AddSlot().FillWidth(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
				.BorderBackgroundColor(Paper)
			];
			Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f)
			[
				SNew(SMatterFluxPaperTab)
				.Label(FText::FromString(TEXT("?")))
				.ToolTip(Help)
				.Padding(FMargin(10.0f, 5.0f))
				.OnClicked([]() { return FReply::Handled(); })
			];
			Header->AddSlot().AutoWidth().VAlign(VAlign_Center)
			.Padding(0.0f, 5.0f, 8.0f, 5.0f)
			[
				SNew(SMatterFluxPaperTab)
				.Label(FText::FromString(TEXT("×")))
				.ToolTip(FText::FromString(TEXT("返回上一页")))
				.Padding(FMargin(10.0f, 5.0f))
				.OnClicked([Owner]()
				{
					if (Owner) Owner->ReturnFromSubmenu();
					return FReply::Handled();
				})
			];
			return SNew(SMatterFluxPaperWindow)
				.Header()
				[
					Header
				]
				[
					SNew(SMatterFluxSettingsPanel)
				];
		}

		TSharedRef<SWidget> BuildSlots(const bool bLoad)
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			UMatterFluxSaveSubsystem* Save = Owner ? Owner->GetSaveSubsystem() : nullptr;
			TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
			Content->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(STextBlock).Text(FText::FromString(bLoad ? TEXT("载入存档") : TEXT("保存游戏")))
				.Font(Font(20, true)).ColorAndOpacity(Ink)
			];
			TSharedRef<SScrollBox> SlotList = SNew(SScrollBox)
				.Orientation(Orient_Vertical)
				.ScrollBarVisibility(EVisibility::Visible)
				.ScrollBarAlwaysVisible(true);
			const TArray<FMatterFluxSaveSlotInfo>& Slots = Save
				? Save->GetSlots()
				: EmptySaveSlots();
			if (bLoad && Slots.IsEmpty())
			{
				SlotList->AddSlot().Padding(0.0f, 4.0f)
				[
					Outline(
						SNew(SBox)
						.HeightOverride(48.0f)
						.HAlign(HAlign_Fill)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("还没有存档")))
							.Font(Font(10)).ColorAndOpacity(Muted)
							.Justification(ETextJustify::Center)
						],
						FMargin(12.0f, 0.0f))
				];
			}
			for (const FMatterFluxSaveSlotInfo& Slot : Slots)
			{
				const int32 SlotIndex = Slot.SlotIndex;
				TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
				Row->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SlotNameWidget(Owner, Slot)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock).Text(FText::FromString(
							FString::Printf(TEXT("种子 %d  ·  %s UTC"), Slot.MapSeed, *Slot.SavedAtUtc.ToString(TEXT("%Y-%m-%d %H:%M")))))
						.Font(Font(9)).ColorAndOpacity(Muted)
					]
				];
				Row->AddSlot().AutoWidth().Padding(8.0f, 0.0f)
				[Button(bLoad ? TEXT("载入") : TEXT("保存"), [Owner, SlotIndex, bLoad]()
				{
					Owner->RequestSlotOperation(SlotIndex, bLoad);
					return FReply::Handled();
				}, bLoad ? TEXT("载入这个世界") : TEXT("覆盖这个存档"), true, 78.0f)];
				AddSlotManagementButtons(Row, Owner, SlotIndex);
				SlotList->AddSlot().Padding(0.0f, 4.0f)
				[Outline(Row, FMargin(12.0f, 8.0f))];
			}
			if (!bLoad)
			{
				const int32 NewSlotIndex = Save
					? Save->GetNextAvailableSlotIndex()
					: INDEX_NONE;
				SlotList->AddSlot().Padding(0.0f, 4.0f)
				[
					Button(TEXT("＋ 新建存档"), [Owner, NewSlotIndex]()
					{
						Owner->RequestSlotOperation(NewSlotIndex, false);
						return FReply::Handled();
					}, TEXT("创建一个新的存档，不覆盖已有世界"),
					NewSlotIndex != INDEX_NONE, 650.0f)
				];
			}
			Content->AddSlot().AutoHeight()
			[
				SNew(SBox).HeightOverride(360.0f)[SlotList]
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[Button(TEXT("返回"), [Owner]() { Owner->ReturnFromSubmenu(); return FReply::Handled(); }, FString(), true, 180.0f)];
			AddNotice(Content);
			return SNew(SBox).WidthOverride(860.0f)[Outline(Content, FMargin(24.0f, 20.0f))];
		}

		TSharedRef<SWidget> BuildProgressPanel()
		{
			TWeakObjectPtr<UMatterFluxShellWidget> WeakOwner = OwnerWidget;
			TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);
			Content->AddSlot().AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([WeakOwner]()
				{
					if (const UMatterFluxShellWidget* Owner = WeakOwner.Get())
					{
						if (const UMatterFluxSaveSubsystem* Save = Owner->GetSaveSubsystem()) return Save->GetOperationTitle();
					}
					return FText::GetEmpty();
				})
				.Font(Font(18, true)).ColorAndOpacity(Ink)
			];
			Content->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([WeakOwner]()
				{
					if (const UMatterFluxShellWidget* Owner = WeakOwner.Get())
					{
						if (const UMatterFluxSaveSubsystem* Save = Owner->GetSaveSubsystem()) return Save->GetOperationStatusText();
					}
					return FText::GetEmpty();
				})
				.Font(Font(10)).ColorAndOpacity(Muted)
			];
			Content->AddSlot().AutoHeight()
			[
				SNew(SMatterFluxProgressBar)
				.Percent_Lambda([WeakOwner]() -> TOptional<float>
				{
					if (const UMatterFluxShellWidget* Owner = WeakOwner.Get())
					{
						if (const UMatterFluxSaveSubsystem* Save = Owner->GetSaveSubsystem())
						{
							return Save->IsOperationProgressDeterminate()
								? TOptional<float>(FMath::Clamp(
									Save->GetOperationProgress(), 0.0f, 1.0f))
								: TOptional<float>();
						}
					}
					return 0.0f;
				})
			];
			Content->AddSlot().AutoHeight().HAlign(HAlign_Right).Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([WeakOwner]()
				{
					if (const UMatterFluxShellWidget* Owner = WeakOwner.Get())
					{
						if (const UMatterFluxSaveSubsystem* Save = Owner->GetSaveSubsystem())
						{
							if (!Save->IsOperationProgressDeterminate())
							{
								return FText::FromString(TEXT("处理中…"));
							}
							const float Progress = FMath::Clamp(
								Save->GetOperationProgress(), 0.0f, 1.0f);
							return FText::FromString(FString::Printf(
								TEXT("%d%%"),
								FMath::RoundToInt(Progress * 100.0f)));
						}
					}
					return FText::FromString(TEXT("0%"));
				})
				.Font(Font(10, true)).ColorAndOpacity(Ink)
			];
			return SNew(SBox).WidthOverride(520.0f)[Outline(Content, FMargin(26.0f, 22.0f))];
		}

		void AddNotice(const TSharedRef<SVerticalBox>& Content)
		{
			UMatterFluxShellWidget* Owner = OwnerWidget.Get();
			if (Owner && !Owner->GetTransientNotice().IsEmpty())
			{
				Content->AddSlot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
				[
					Outline(
						SNew(STextBlock)
						.Text(FText::FromString(Owner->GetTransientNotice()))
						.Font(Font(9, true))
						.ColorAndOpacity(Ink),
						FMargin(8.0f, 5.0f))
				];
			}
		}

		TWeakObjectPtr<UMatterFluxShellWidget> OwnerWidget;
		TSharedPtr<FSlateDynamicImageBrush> TitleBackgroundBrush;
	};
}

TSharedRef<SWidget> MatterFluxShellUI::CreateShell(
	TWeakObjectPtr<UMatterFluxShellWidget> OwnerWidget)
{
	return SNew(SMatterFluxShell)
		.OwnerWidget(OwnerWidget);
}

void MatterFluxShellUI::RefreshShell(const TSharedPtr<SWidget>& Shell)
{
	if (const TSharedPtr<SMatterFluxShell> TypedShell =
		StaticCastSharedPtr<SMatterFluxShell>(Shell))
	{
		TypedShell->Refresh();
	}
}

FString MatterFluxShellUI::GetSlotDisplayName(
	const FMatterFluxSaveSlotInfo& Slot)
{
	return SlotDisplayName(Slot);
}
