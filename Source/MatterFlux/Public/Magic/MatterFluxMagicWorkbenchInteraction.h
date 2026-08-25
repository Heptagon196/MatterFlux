#pragma once

#include "CoreMinimal.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"

/** 法术编辑器拖拽源。它只描述行为，不依赖 Slate 控件生命周期。 */
enum class EMatterFluxMagicDragSource : uint8
{
	None,
	SpellInventory,
	WandSpellSlot
};

/**
 * 一次法术拖拽的最小稳定数据。
 *
 * UI 重建、鼠标离开控件或取消拖拽都不会修改背包；只有成功解析为
 * FMatterFluxMagicEdit 并提交后，权威数据才会变化。
 */
struct MATTERFLUX_API FMatterFluxMagicDragPayload
{
	EMatterFluxMagicDragSource Source = EMatterFluxMagicDragSource::None;
	FGuid WandId;
	FName SpellId = NAME_None;
	int32 SpellSlot = INDEX_NONE;
};

/** 法术编辑页中一个法杖穿戴槽的稳定展示数据。 */
struct MATTERFLUX_API FMatterFluxMagicEquipmentSlotPresentation
{
	int32 SlotIndex = INDEX_NONE;
	FString KeyLabel;
	FString KeyBadge;
};

class MATTERFLUX_API FMatterFluxMagicWorkbenchInteraction
{
public:
	/** 生成全部法杖穿戴槽；数量始终跟随施法系统的槽位定义。 */
	static void BuildEquipmentSlotPresentations(
		TArray<FMatterFluxMagicEquipmentSlotPresentation>& OutSlots);

	/** 所有法术背包、法术树、备用区和拖拽图标共用的外框尺寸。 */
	static FVector2D GetSpellSlotSize();

	/** 拖拽装饰器必须与源法术槽逐像素同尺寸。 */
	static FVector2D GetSpellDragDecoratorSize();

	/** 计算装饰器左上角，使鼠标热点固定落在拖拽 item 的中心。 */
	static FVector2D CalculateSpellDragDecoratorPosition(
		const FVector2D& CursorScreenPosition);

	/** 为法杖容量中的每一个槽生成落点；空槽也绝不能被布局省略。 */
	static void BuildSpellDropTargets(
		const FMatterFluxOwnedWand& Wand,
		TArray<int32>& OutSlotIndices);

	/** 把背包→槽或槽→槽拖拽解析成事务式编辑命令。 */
	static bool ResolveSpellDrop(
		const FMatterFluxMagicDragPayload& Payload,
		const FGuid& TargetWandId,
		int32 TargetSpellSlot,
		FMatterFluxMagicEdit& OutEdit);
};
