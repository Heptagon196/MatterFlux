#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MatterFluxPlayerStatusWidget.generated.h"

class APlayerController;
class AMatterFluxPlayerState;
class UAbilitySystemComponent;
class UMatterFluxMagicInventoryComponent;
struct FOnAttributeChangeData;

namespace MatterFluxPlayerStatusUI
{
	class SMatterFluxPlayerStatus;
}

/** 左下角单个法杖槽的只读显示数据。 */
struct MATTERFLUX_API FMatterFluxPlayerStatusWandView
{
	FText Label;
	float Mana = 0.0f;
	float MaxMana = 0.0f;
	bool bEquipped = false;
	bool bActive = false;
};

/** HUD 一帧所需的稳定数据；表现层不直接修改 GAS 或魔法背包。 */
struct MATTERFLUX_API FMatterFluxPlayerStatusSnapshot
{
	float Health = 0.0f;
	float MaxHealth = 0.0f;
	TArray<FMatterFluxPlayerStatusWandView> Wands;
};

/**
 * PaperMagic 风格的左下角玩家状态 HUD。
 *
 * 生命来自 PlayerState 上的 GAS AttributeSet，法力来自复制的魔法背包；
 * HUD 只监听并投影权威状态，不维护第二份游戏数值。
 */
UCLASS()
class MATTERFLUX_API UMatterFluxPlayerStatusWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	static constexpr int32 WandSlotCount = 4;

	void InitializeForPlayer(APlayerController* InPlayerController);
	void SetSuppressedByFrontEnd(bool bSuppressed);
	void RefreshStatus();

	/** 可测试的纯读取边界：把 PlayerState 投影成固定五行 HUD 数据。 */
	static FMatterFluxPlayerStatusSnapshot BuildStatusSnapshot(
		const AMatterFluxPlayerState* PlayerState);

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	void BindSources();
	void UnbindSources();
	void HandleAttributeChanged(const FOnAttributeChangeData& ChangeData);
	void HandleInventoryChanged();

	UPROPERTY(Transient)
	TObjectPtr<APlayerController> PlayerController;

	TWeakObjectPtr<UAbilitySystemComponent> BoundAbilitySystem;
	TWeakObjectPtr<UMatterFluxMagicInventoryComponent> BoundInventory;
	FDelegateHandle HealthChangedHandle;
	FDelegateHandle MaxHealthChangedHandle;
	FDelegateHandle InventoryChangedHandle;
	FDelegateHandle ContentReloadedHandle;
	TSharedPtr<MatterFluxPlayerStatusUI::SMatterFluxPlayerStatus> StatusView;
};
