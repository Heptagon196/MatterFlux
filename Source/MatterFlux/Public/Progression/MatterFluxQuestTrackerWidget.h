#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MatterFluxQuestTrackerWidget.generated.h"

class APlayerController;
class UMatterFluxProgressionComponent;
namespace MatterFluxProgressionUI
{
	class SMatterFluxQuestTracker;
}

/** Small non-interactive HUD projection of the selected replicated quest. */
UCLASS()
class MATTERFLUX_API UMatterFluxQuestTrackerWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void InitializeForPlayer(APlayerController* InPlayerController);
	UMatterFluxProgressionComponent* ResolveProgression() const;
	void RefreshTracker();

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	void BindProgression();

	UPROPERTY(Transient)
	TObjectPtr<APlayerController> PlayerController;

	TWeakObjectPtr<UMatterFluxProgressionComponent> BoundProgression;
	FDelegateHandle ProgressionChangedHandle;
	FDelegateHandle ContentReloadedHandle;
	TSharedPtr<MatterFluxProgressionUI::SMatterFluxQuestTracker> Tracker;
};
