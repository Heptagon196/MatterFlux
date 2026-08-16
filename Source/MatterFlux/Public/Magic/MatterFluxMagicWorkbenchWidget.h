#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "MatterFluxMagicWorkbenchWidget.generated.h"

class APlayerController;
class UMatterFluxProgressionComponent;

enum class EMatterFluxWorkbenchPage : uint8
{
	SpellEditor,
	WandBackpack,
	ItemBackpack,
	QuestJournal,
	Settings
};

/** Runtime-built inventory/equipment/wand-program editor. */
UCLASS()
class MATTERFLUX_API UMatterFluxMagicWorkbenchWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void InitializeForPlayer(APlayerController* InPlayerController);
	UMatterFluxMagicInventoryComponent* ResolveInventory() const;
	UMatterFluxProgressionComponent* ResolveProgression() const;
	void SubmitEdit(FMatterFluxMagicEdit Edit);
	void SelectWand(FGuid WandId);
	FGuid GetSelectedWandId() const { return SelectedWandId; }
	void SetPendingSpell(FName SpellId);
	FName GetPendingSpell() const { return PendingSpellId; }
	void ShowSpellEditor();
	void ShowWandBackpack();
	void ShowItemBackpack();
	void ShowQuestJournal();
	void ShowSettingsPage();
	EMatterFluxWorkbenchPage GetPage() const { return CurrentPage; }
	void SelectItem(FName ItemId);
	FName GetSelectedItem() const { return SelectedItemId; }
	void UseItem(FName ItemId);
	void SelectQuest(FName QuestId);
	void RequestClose();
	void RefreshWorkbench();

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(
		const FGeometry& InGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	void BindInventory();
	void BindProgression();
	void HandleBackingStateChanged();

	UPROPERTY(Transient)
	TObjectPtr<APlayerController> PlayerController;

	TWeakObjectPtr<UMatterFluxMagicInventoryComponent> BoundInventory;
	TWeakObjectPtr<UMatterFluxProgressionComponent> BoundProgression;
	FDelegateHandle InventoryChangedHandle;
	FDelegateHandle ProgressionChangedHandle;
	FDelegateHandle ContentReloadedHandle;
	TSharedPtr<SWidget> Workbench;
	FGuid SelectedWandId;
	FName PendingSpellId;
	FName SelectedItemId;
	EMatterFluxWorkbenchPage CurrentPage =
		EMatterFluxWorkbenchPage::SpellEditor;
};
