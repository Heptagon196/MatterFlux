#include "Magic/MatterFluxMagicWorkbenchWidget.h"
#include "Magic/MatterFluxMagicWorkbenchSlate.h"

#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "IMatterFluxScriptRuntime.h"
#include "InputCoreTypes.h"
#include "Input/Reply.h"

void UMatterFluxMagicWorkbenchWidget::InitializeForPlayer(
	APlayerController* InPlayerController)
{
	PlayerController = InPlayerController;
	SetIsFocusable(true);
	BindInventory();
	BindProgression();
	RefreshWorkbench();
}

UMatterFluxProgressionComponent*
UMatterFluxMagicWorkbenchWidget::ResolveProgression() const
{
	const AMatterFluxPlayerState* PlayerState = PlayerController
		? PlayerController->GetPlayerState<AMatterFluxPlayerState>() : nullptr;
	return PlayerState ? PlayerState->GetProgression() : nullptr;
}

UMatterFluxMagicInventoryComponent*
UMatterFluxMagicWorkbenchWidget::ResolveInventory() const
{
	const AMatterFluxPlayerState* PlayerState = PlayerController
		? PlayerController->GetPlayerState<AMatterFluxPlayerState>()
		: nullptr;
	return PlayerState ? PlayerState->GetMagicInventory() : nullptr;
}

void UMatterFluxMagicWorkbenchWidget::SubmitEdit(
	FMatterFluxMagicEdit Edit)
{
	if (UMatterFluxMagicInventoryComponent* Inventory = ResolveInventory())
	{
		Edit.ExpectedRevision = Inventory->GetInventoryRevision();
		Inventory->RequestEdit(Edit);
	}
}

void UMatterFluxMagicWorkbenchWidget::SelectWand(const FGuid WandId)
{
	SelectedWandId = WandId;
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::SetPendingSpell(const FName SpellId)
{
	PendingSpellId = PendingSpellId == SpellId ? NAME_None : SpellId;
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::ShowSpellEditor()
{
	CurrentPage = EMatterFluxWorkbenchPage::SpellEditor;
	if (const UMatterFluxMagicInventoryComponent* Inventory = ResolveInventory())
	{
		const FGuid ActiveWandId = Inventory->GetActiveWandId();
		if (ActiveWandId.IsValid())
		{
			SelectedWandId = ActiveWandId;
		}
	}
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::ShowWandBackpack()
{
	CurrentPage = EMatterFluxWorkbenchPage::WandBackpack;
	PendingSpellId = NAME_None;
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::ShowItemBackpack()
{
	CurrentPage = EMatterFluxWorkbenchPage::ItemBackpack;
	PendingSpellId = NAME_None;
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::ShowQuestJournal()
{
	CurrentPage = EMatterFluxWorkbenchPage::QuestJournal;
	PendingSpellId = NAME_None;
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::ShowSettingsPage()
{
	CurrentPage = EMatterFluxWorkbenchPage::Settings;
	PendingSpellId = NAME_None;
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::SelectItem(const FName ItemId)
{
	SelectedItemId = ItemId;
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::UseItem(const FName ItemId)
{
	if (UMatterFluxProgressionComponent* Progression = ResolveProgression())
	{
		Progression->RequestUseItem(ItemId);
	}
}

void UMatterFluxMagicWorkbenchWidget::SelectQuest(const FName QuestId)
{
	if (UMatterFluxProgressionComponent* Progression = ResolveProgression())
	{
		Progression->RequestSelectQuest(QuestId);
	}
}

void UMatterFluxMagicWorkbenchWidget::RequestClose()
{
	if (AMatterFluxPlayerController* MatterFluxController =
		Cast<AMatterFluxPlayerController>(PlayerController))
	{
		if (MatterFluxController->IsMagicWorkbenchOpen())
		{
			MatterFluxController->ToggleMagicWorkbench();
		}
	}
}

void UMatterFluxMagicWorkbenchWidget::RefreshWorkbench()
{
	BindInventory();
	BindProgression();
	if (UMatterFluxMagicInventoryComponent* Inventory = ResolveInventory())
	{
		if (!Inventory->FindWand(SelectedWandId))
		{
			SelectedWandId = Inventory->GetActiveWandId();
			if (!SelectedWandId.IsValid()
				&& !Inventory->GetOwnedWands().IsEmpty())
			{
				SelectedWandId =
					Inventory->GetOwnedWands()[0].InstanceId;
			}
		}
	}
	MatterFluxMagicUI::RefreshWorkbench(Workbench);
}

void UMatterFluxMagicWorkbenchWidget::HandleBackingStateChanged()
{
	// The magic inventory also broadcasts for 5 Hz mana regeneration. The
	// controller refreshes explicitly before opening, so rebuilding a collapsed
	// workbench only wastes Slate and game-thread time.
	if (GetVisibility() != ESlateVisibility::Collapsed)
	{
		RefreshWorkbench();
	}
}

void UMatterFluxMagicWorkbenchWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (!ContentReloadedHandle.IsValid())
	{
		ContentReloadedHandle =
			IMatterFluxScriptRuntime::Get().OnContentReloaded().AddWeakLambda(
				this,
				[this](const FMatterFluxContentRegistryPtr)
				{
					RefreshWorkbench();
				});
	}
	BindInventory();
	BindProgression();
	RefreshWorkbench();
}

void UMatterFluxMagicWorkbenchWidget::NativeDestruct()
{
	if (ContentReloadedHandle.IsValid()
		&& IMatterFluxScriptRuntime::IsAvailable())
	{
		IMatterFluxScriptRuntime::Get().OnContentReloaded().Remove(
			ContentReloadedHandle);
	}
	ContentReloadedHandle.Reset();
	if (BoundInventory.IsValid() && InventoryChangedHandle.IsValid())
	{
		BoundInventory->OnInventoryChanged().Remove(InventoryChangedHandle);
	}
	InventoryChangedHandle.Reset();
	BoundInventory.Reset();
	if (BoundProgression.IsValid() && ProgressionChangedHandle.IsValid())
	{
		BoundProgression->OnProgressionChanged().Remove(
			ProgressionChangedHandle);
	}
	ProgressionChangedHandle.Reset();
	BoundProgression.Reset();
	Super::NativeDestruct();
}

FReply UMatterFluxMagicWorkbenchWidget::NativeOnKeyDown(
	const FGeometry& InGeometry,
	const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Escape || Key == EKeys::I || Key == EKeys::Tab
		|| Key == EKeys::J)
	{
		RequestClose();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

TSharedRef<SWidget> UMatterFluxMagicWorkbenchWidget::RebuildWidget()
{
	Workbench = MatterFluxMagicUI::CreateWorkbench(this);
	return Workbench.ToSharedRef();
}

void UMatterFluxMagicWorkbenchWidget::ReleaseSlateResources(
	const bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	Workbench.Reset();
}

void UMatterFluxMagicWorkbenchWidget::BindInventory()
{
	UMatterFluxMagicInventoryComponent* Inventory = ResolveInventory();
	if (BoundInventory.Get() == Inventory)
	{
		return;
	}
	if (BoundInventory.IsValid() && InventoryChangedHandle.IsValid())
	{
		BoundInventory->OnInventoryChanged().Remove(InventoryChangedHandle);
	}
	InventoryChangedHandle.Reset();
	BoundInventory = Inventory;
	if (Inventory)
	{
		InventoryChangedHandle = Inventory->OnInventoryChanged().AddUObject(
			this,
			&UMatterFluxMagicWorkbenchWidget::HandleBackingStateChanged);
	}
}

void UMatterFluxMagicWorkbenchWidget::BindProgression()
{
	UMatterFluxProgressionComponent* Progression = ResolveProgression();
	if (BoundProgression.Get() == Progression)
	{
		return;
	}
	if (BoundProgression.IsValid() && ProgressionChangedHandle.IsValid())
	{
		BoundProgression->OnProgressionChanged().Remove(
			ProgressionChangedHandle);
	}
	ProgressionChangedHandle.Reset();
	BoundProgression = Progression;
	if (Progression)
	{
		ProgressionChangedHandle =
			Progression->OnProgressionChanged().AddUObject(
				this,
				&UMatterFluxMagicWorkbenchWidget::HandleBackingStateChanged);
	}
}


