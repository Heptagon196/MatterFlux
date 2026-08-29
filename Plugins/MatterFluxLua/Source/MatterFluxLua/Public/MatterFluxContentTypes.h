#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::Magic
{
	/** Physical equipment keys: left, right, Q, E, and Space. */
	inline constexpr int32 EquipmentSlotCount = 5;
}

enum class EMatterFluxMaterialPhase : uint8
{
	StaticSolid,
	Powder,
	Liquid,
	Gas
};

struct FMatterFluxContentManifest
{
	FString PackId;
	int32 SchemaVersion = 0;
	int32 Revision = 0;
	FString VersionHash;

	bool IsValid() const
	{
		return !PackId.IsEmpty()
			&& SchemaVersion == MATTERFLUX_LUA_SCHEMA_VERSION
			&& Revision >= 0
			&& !VersionHash.IsEmpty();
	}
};

struct FMatterFluxMaterialDefinition
{
	FName Id;
	float Density = 1.0f;
	float Hardness = 1.0f;
	FLinearColor Color = FLinearColor::White;
	EMatterFluxMaterialPhase Phase = EMatterFluxMaterialPhase::StaticSolid;
	uint8 Mobility = 255;
	uint8 Dispersion = 128;
	/** 对浸入其中的角色/刚体施加的相对移动阻力倍率。 */
	float MovementResistance = 1.0f;
	/** 物质格存在的固定模拟步数；0 表示不会自行消散。 */
	uint8 LifetimeSteps = 0;
	/** 浅水处的透明度；0 完全透明，1 完全不透明。 */
	float ShallowOpacity = 1.0f;
	/** 达到吸收距离后的不透明度，必须不小于 ShallowOpacity。 */
	float DeepOpacity = 1.0f;
	/** 从浅水过渡到深水不透明度所需的视线内液体深度（厘米）。 */
	float OpacityDepth = 100.0f;
	/** 每单位材料的确定性 uint16 默认比能。 */
	uint16 DefaultEnergy = 0;
	/** 与接触材料交换能量的整数千分比。 */
	uint16 ConductivityPermille = 0;
	/** 每个固定步向环境散失的比能。 */
	uint16 CoolingPerStep = 0;
	/** 达到此比能时发生燃烧转化；0 表示不可燃。 */
	uint16 IgnitionThreshold = 0;
	/** 燃烧后的普通材料；不可燃材料保持 empty。 */
	FName CombustionProduct = TEXT("empty");
	/** 燃烧产物获得的比能；0 表示保留点燃瞬间的能量。 */
	uint16 CombustionEnergy = 0;
	/** 燃烧时排放的普通材料；empty 表示不排放。 */
	FName CombustionEmissionMaterial = TEXT("empty");
	/** 每次点燃产生的显式普通材料排放量；不从满格固体 Amount 扣除。 */
	uint16 CombustionEmissionAmount = 0;
	/** 第二种燃烧排放；用于同时产生烟和短寿命火焰等普通材料。 */
	FName CombustionSecondaryEmissionMaterial = TEXT("empty");
	/** 第二种燃烧排放量；0 表示没有第二排放。 */
	uint16 CombustionSecondaryEmissionAmount = 0;
};

enum class EMatterFluxReactionEmissionSourceSide : uint8
{
	A,
	B
};

struct FMatterFluxReactionEmissionDefinition
{
	FName Material;
	uint16 Amount = 0;
	uint16 Energy = 0;
	EMatterFluxReactionEmissionSourceSide SourceSide =
		EMatterFluxReactionEmissionSourceSide::A;
};

struct FMatterFluxReactionDefinition
{
	FName Id; // Lua 中稳定且全局唯一的规则 ID。
	FName InputA; // 接触一侧的材料。
	FName InputB; // 接触另一侧的材料。
	FName OutputA; // InputA 完成反应后的产物，empty 表示清空。
	FName OutputB; // InputB 完成反应后的产物。
	int32 ChancePermille = 1000; // 触发概率，使用整数千分比。
	int32 EnergyDeltaA = 0; // 接触提交时对 A 的比能变化。
	int32 EnergyDeltaB = 0; // 接触提交时对 B 的比能变化。
	TArray<FMatterFluxReactionEmissionDefinition, TInlineAllocator<2>> Emissions;
};

struct FMatterFluxDecoratorDefinition
{
	FName Id;
	FName GeneratorId;
	FName MaterialId;
	float SpawnWeight = 1.0f;
	int32 MinCount = 0;
	int32 MaxCount = 0;
	bool bEnableCollision = false;
};

struct FMatterFluxEntityDefinition
{
	FName Id;
	FString Behavior;
	float MaxHealth = 100.0f;
	float MoveSpeed = 300.0f;
};

enum class EMatterFluxCreatureFaction : uint8
{
	Friendly,
	Hostile,
	Neutral
};

enum class EMatterFluxCreatureLevel : uint8
{
	Normal,
	Elite,
	Boss
};

enum class EMatterFluxCreatureAiMode : uint8
{
	Passive,
	Patrol,
	Skirmisher,
	Boss,
	BehaviorTree
};

/** Node kinds accepted by the restricted Lua creature behavior-tree DSL. */
enum class EMatterFluxCreatureBehaviorNodeKind : uint8
{
	Selector,
	Sequence,
	Condition,
	Action
};

/** Bounded read-only facts that a Lua-authored behavior tree may query. */
enum class EMatterFluxCreatureBehaviorCondition : uint8
{
	HasVisibleTarget,
	HasTarget,
	TargetTooClose,
	TargetInAttackRange,
	AttackReady,
	SkillReady
};

/** Server-authoritative capabilities that a behavior tree may select. */
enum class EMatterFluxCreatureBehaviorAction : uint8
{
	Passive,
	Patrol,
	Chase,
	Retreat,
	Attack,
	Skill
};

struct FMatterFluxCreatureBehaviorNodeDefinition
{
	EMatterFluxCreatureBehaviorNodeKind Kind =
		EMatterFluxCreatureBehaviorNodeKind::Action;
	EMatterFluxCreatureBehaviorCondition Condition =
		EMatterFluxCreatureBehaviorCondition::HasTarget;
	EMatterFluxCreatureBehaviorAction Action =
		EMatterFluxCreatureBehaviorAction::Passive;
	TArray<int32> Children;
};

/**
 * Immutable, bounded behavior tree compiled from Lua. Nodes reference their
 * children by index so the runtime never retains Lua functions or tables.
 */
struct FMatterFluxCreatureBehaviorProgramDefinition
{
	int32 RootNodeIndex = INDEX_NONE;
	TArray<FMatterFluxCreatureBehaviorNodeDefinition> Nodes;

	bool IsEmpty() const
	{
		return RootNodeIndex == INDEX_NONE && Nodes.IsEmpty();
	}
};

/**
 * One bounded creature cast program compiled from Lua. This represents the
 * useful behavior of a PaperMagic spell tree without storing executable Lua
 * callbacks on an Actor.
 */
struct FMatterFluxCreatureCastProgramDefinition
{
	FName SpellId;
	int32 ProjectileCount = 1;
	float SpreadDegrees = 0.0f;
	float ProjectileInterval = 0.0f;
	float RecoverySeconds = 0.0f;
	bool bRadial = false;
	float HorizontalImpulse = 0.0f;
	float VerticalImpulse = 0.0f;
	bool bOverrideColor = false;
	FLinearColor Color = FLinearColor::White;
};

/**
 * Immutable creature program compiled from Lua. The server interprets these
 * bounded values; Lua is never called from an Actor tick.
 */
struct FMatterFluxCreatureDefinition
{
	FName Id;
	FString DisplayName;
	EMatterFluxCreatureFaction Faction = EMatterFluxCreatureFaction::Neutral;
	EMatterFluxCreatureLevel Level = EMatterFluxCreatureLevel::Normal;
	EMatterFluxCreatureAiMode AiMode = EMatterFluxCreatureAiMode::Passive;
	FMatterFluxCreatureBehaviorProgramDefinition BehaviorProgram;
	float MaxHealth = 100.0f;
	float Width = 70.0f;
	float Height = 160.0f;
	/** 生物体积密度；与液体材质 Density 比较决定漂浮或下沉。 */
	float Density = 0.65f;
	float MoveSpeed = 240.0f;
	float PerceptionRange = 900.0f;
	float AttackRange = 650.0f;
	float RetreatRange = 180.0f;
	float TargetMemorySeconds = 5.0f;
	/** Keep this creature idle until it sees a player for the first time. */
	bool bWaitForFirstSight = false;
	float PatrolTurnSeconds = 3.0f;
	float PatrolPauseSeconds = 0.5f;
	float AttackCooldown = 2.0f;
	float SkillCooldown = 10.0f;
	FMatterFluxCreatureCastProgramDefinition AttackProgram;
	FMatterFluxCreatureCastProgramDefinition SkillProgram;
	FName DialogueId;
	FName ShopId;
	FName DropItemId;
	int32 DropItemCount = 0;
	FLinearColor Color = FLinearColor::White;
};

struct FMatterFluxDialogueOptionDefinition
{
	FString Text;
	FName NextNodeId;
	FName ShopId;
	bool bClose = false;
};

struct FMatterFluxDialogueNodeDefinition
{
	FName Id;
	FString Text;
	FName NextNodeId;
	FName ShopId;
	bool bClose = false;
	TArray<FMatterFluxDialogueOptionDefinition> Options;
};

/** A bounded, deterministic dialogue graph authored in Lua. */
struct FMatterFluxDialogueDefinition
{
	FName Id;
	FString DisplayName;
	FName StartNodeId;
	TArray<FMatterFluxDialogueNodeDefinition> Nodes;
};

enum class EMatterFluxShopProductKind : uint8
{
	Item,
	Spell,
	Wand
};

/** One author-defined, ordered filtering tab in a shop. */
struct FMatterFluxShopCategoryDefinition
{
	FName Id;
	FString DisplayName;
};

struct FMatterFluxShopOfferDefinition
{
	EMatterFluxShopProductKind ProductKind = EMatterFluxShopProductKind::Item;
	FName ProductId;
	int32 ProductCount = 1;
	FName CostItemId;
	int32 CostCount = 1;
	int32 PurchaseLimit = INDEX_NONE;
	/** Empty keeps the offer visible in the unfiltered catalog. */
	FName CategoryId;
};

struct FMatterFluxShopDefinition
{
	FName Id;
	FString DisplayName;
	TArray<FMatterFluxShopCategoryDefinition> Categories;
	TArray<FMatterFluxShopOfferDefinition> Offers;
};

enum class EMatterFluxSpellKind : uint8
{
	Projectile,
	Modifier,
	Multicast,
	Trigger,
	TriggerModifier,
	Jump
};

enum class EMatterFluxSpellTriggerEvent : uint8
{
	Impact,
	Expired
};

/**
 * Immutable spell data produced by the restricted Lua content runtime.
 * Lua describes data only; deterministic C++ code interprets the program.
 */
struct FMatterFluxSpellDefinition
{
	FName Id;
	FString DisplayName;
	FString Description;
	FString Icon;
	EMatterFluxSpellKind Kind = EMatterFluxSpellKind::Projectile;
	float ManaCost = 0.0f;
	int32 DrawCount = 0;
	int32 TriggerDrawCount = 0;
	float Damage = 0.0f;
	float DamageAdd = 0.0f;
	float DamageMultiplier = 1.0f;
	float Speed = 0.0f;
	float SpeedMultiplier = 1.0f;
	float Lifetime = 0.0f;
	float LifetimeMultiplier = 1.0f;
	float Radius = 0.0f;
	/** Fraction of world gravity applied while the projectile is in flight. */
	float GravityScale = 0.0f;
	float SpreadDelta = 0.0f;
	float CastDelayDelta = 0.0f;
	float RechargeTimeDelta = 0.0f;
	bool bOverrideColor = false;
	FLinearColor Color = FLinearColor::White;
	float OrbitRadius = 0.0f;
	EMatterFluxSpellTriggerEvent TriggerEvent =
		EMatterFluxSpellTriggerEvent::Impact;
	bool bTriggerRandomDirection = false;
	float CarrierLifetimeOverride = 0.0f;
	float VerticalImpulse = 0.0f;
	/** Optional material composing the projectile body while it is in flight. */
	FName BodyMaterial;
	/** Number of body-material cells released into the simulation on impact. */
	int32 MaterialAmount = 1;
	/** Extra spawn distance along the current cast aim direction. */
	float SpawnForwardOffset = 0.0f;
	/** Extra world-up offset applied at spawn. */
	float SpawnHeightOffset = 0.0f;
	/** Suppresses the authored forward launch speed after placement. */
	bool bSpawnStationary = false;
	/** Uses a thin plane instead of the default compact projectile body. */
	bool bUsePlaneVisual = false;
	/** Plane visual is vertical and extends along travel; false keeps it horizontal. */
	bool bUseVerticalPlaneVisual = false;
	int32 StarterCount = 0;
};

/** Immutable wand chassis data. Mutable mana/deck state lives in inventory. */
struct FMatterFluxWandDefinition
{
	FName Id;
	FString DisplayName;
	FString Description;
	FString Icon;
	int32 Capacity = 1;
	bool bShuffle = false;
	int32 DrawCount = 1;
	float CastDelay = 0.2f;
	float RechargeTime = 0.5f;
	float ManaMax = 100.0f;
	float ManaRechargePerSecond = 20.0f;
	float Spread = 0.0f;
	int32 StarterCount = 0;
	int32 StarterEquipmentSlot = -1;
	TArray<FName> StarterDeck;
};

enum class EMatterFluxItemCategory : uint8
{
	Material,
	Quest,
	Consumable
};

enum class EMatterFluxItemUseAction : uint8
{
	None,
	RestoreHealth,
	RestoreWandMana,
	GameplayEvent
};

/** Immutable item data. Runtime use is interpreted by authoritative C++. */
struct FMatterFluxItemDefinition
{
	FName Id;
	FString DisplayName;
	FString Description;
	FString Icon;
	EMatterFluxItemCategory Category = EMatterFluxItemCategory::Material;
	int32 MaxStack = 99;
	int32 StarterCount = 0;
	EMatterFluxItemUseAction UseAction = EMatterFluxItemUseAction::None;
	float UseMagnitude = 0.0f;
	FName GameplayEventTag;
	int32 ConsumeCount = 0;
};

enum class EMatterFluxQuestCategory : uint8
{
	Main,
	Side,
	Objective
};

enum class EMatterFluxQuestObjectiveKind : uint8
{
	CompleteChildren,
	EquipWand,
	EquipSpell,
	KillEnemies,
	SpendItem,
	Never
};

enum class EMatterFluxQuestRewardKind : uint8
{
	Item,
	Spell,
	Wand
};

struct FMatterFluxQuestRewardDefinition
{
	EMatterFluxQuestRewardKind Kind = EMatterFluxQuestRewardKind::Item;
	FName ContentId;
	int32 Quantity = 1;
	int32 EquipmentSlot = INDEX_NONE;
};

/** One explicit creature/marker instruction owned by a quest or custom map. */
struct FMatterFluxCreatureSpawnDefinition
{
	FName CreatureId;
	FName MarkerId;
};

/**
 * Immutable quest graph node compiled from Lua. Lua describes objectives and
 * rewards; authoritative C++ owns progress, ordering, replication and save IO.
 */
struct FMatterFluxQuestDefinition
{
	FName Id;
	FString DisplayName;
	FString Description;
	FString CompletedDescription;
	EMatterFluxQuestCategory Category = EMatterFluxQuestCategory::Main;
	int32 SortOrder = 0;
	bool bOptional = false;
	bool bStarter = false;
	bool bFocusOnActivate = false;
	EMatterFluxQuestObjectiveKind Objective =
		EMatterFluxQuestObjectiveKind::Never;
	FName TargetId;
	int32 TargetCount = 1;
	int32 TargetLevel = INDEX_NONE;
	int32 EquipmentSlot = INDEX_NONE;
	TArray<FName> Prerequisites;
	TArray<FName> Subquests;
	/** Spawned once when this quest becomes active. */
	TArray<FMatterFluxCreatureSpawnDefinition> ActivationCreatureSpawns;
	TArray<FMatterFluxQuestRewardDefinition> ActivationRewards;
	TArray<FMatterFluxQuestRewardDefinition> CompletionRewards;
};

struct FMatterFluxFragmentationSettings
{
	// Detached components smaller than this many mask cells are discarded.
	// Individual sources may raise this threshold, but cannot lower it.
	int32 MinDetachedAreaPixels = 1;
};

/**
 * Lua-authored structure profile. Generator selects a bounded C++ geometry
 * capability; cutaway values configure the generic material projection.
 */
struct FMatterFluxStructureDefinition
{
	FName Id;
	FName GeneratorId;
	float ContactToleranceCentimeters = 12.0f;
	float FloorSnapHeightCentimeters = 28.0f;
	float PreferredFloorPaddingCentimeters = 90.0f;
	float PreferredFloorVerticalRangeCentimeters = 420.0f;
	float ExitGraceSeconds = 0.18f;
	float FadeSpeed = 4.5f;
	float WallGhostOpacity = 0.055f;
	float RoofGhostOpacity = 0.025f;
};

enum class EMatterFluxCustomMapStampShape : uint8
{
	Rectangle,
	Circle
};

/** 一次有序的地图填充操作；后声明的 Stamp 覆盖先声明的 Stamp。 */
struct FMatterFluxCustomMapStampDefinition
{
	EMatterFluxCustomMapStampShape Shape =
		EMatterFluxCustomMapStampShape::Rectangle;
	FName MaterialId;
	FIntPoint MinimumCell = FIntPoint::ZeroValue;
	FIntPoint MaximumCellInclusive = FIntPoint::ZeroValue;
	FIntPoint CenterCell = FIntPoint::ZeroValue;
	int32 RadiusCells = 0;
};

/** 自定义地图中的稳定命名位置，可供出生、镜头与测试查询共用。 */
struct FMatterFluxCustomMapMarkerDefinition
{
	FName Id;
	FIntPoint Cell = FIntPoint::ZeroValue;
};

/** 自定义地图中的静态三维场景盒；所有数值均以地图格为单位。 */
struct FMatterFluxCustomMapSceneBoxDefinition
{
	FName Id;
	FName MaterialId;
	FVector CenterCells = FVector::ZeroVector;
	FVector SizeCells = FVector::OneVector;
	/** 是否参与角色和物理物体碰撞；纯背景与液体装饰保持关闭。 */
	bool bCollision = false;
};

/** 自定义地图中的透视验收相机；位置和目标均以地图格为单位。 */
struct FMatterFluxCustomMapCameraDefinition
{
	FName Id;
	FVector LocationCells = FVector::ZeroVector;
	FVector TargetCells = FVector::ZeroVector;
	float FieldOfViewDegrees = 42.0f;
};

/** 游戏内倾倒测试使用的确定性体素容器配置；所有空间值均以地图格为单位。 */
struct FMatterFluxCustomMapPourContainerDefinition
{
	FName Id;
	FName ContainerMaterialId;
	FName LiquidMaterialId;
	FVector CenterCells = FVector::ZeroVector;
	FIntVector InteriorSizeCells = FIntVector(7, 5, 5);
	int32 StartStep = 12;
	int32 TiltDurationSteps = 36;
	float TiltDegrees = 72.0f;
	int32 PourCellsPerStep = 8;
};

/** Lua 编译得到的有界、确定性自定义材料地图。 */
struct FMatterFluxCustomMapDefinition
{
	FName Id;
	FString DisplayName;
	FIntPoint MinimumCell = FIntPoint(-32, 0);
	FIntPoint MaximumCellExclusive = FIntPoint(32, 32);
	float CellSizeCentimeters = 28.0f;
	float MaterialDepthCells = 3.0f;
	TArray<FMatterFluxCustomMapStampDefinition> Stamps;
	TArray<FMatterFluxCustomMapMarkerDefinition> Markers;
	/** Non-quest occupants created when this map becomes playable. */
	TArray<FMatterFluxCreatureSpawnDefinition> InitialCreatureSpawns;
	TArray<FMatterFluxCustomMapSceneBoxDefinition> SceneBoxes;
	TArray<FMatterFluxCustomMapCameraDefinition> Cameras;
	TArray<FMatterFluxCustomMapPourContainerDefinition> PourContainers;
};

struct FMatterFluxContentRegistry
{
	FMatterFluxContentManifest Manifest;
	FMatterFluxFragmentationSettings Fragmentation;
	TMap<FName, FMatterFluxMaterialDefinition> Materials;
	TMap<FName, FMatterFluxReactionDefinition> Reactions;
	TMap<FName, FMatterFluxDecoratorDefinition> Decorators;
	TMap<FName, FMatterFluxEntityDefinition> Entities;
	TMap<FName, FMatterFluxCreatureDefinition> Creatures;
	TMap<FName, FMatterFluxDialogueDefinition> Dialogues;
	TMap<FName, FMatterFluxShopDefinition> Shops;
	TMap<FName, FMatterFluxSpellDefinition> Spells;
	TMap<FName, FMatterFluxWandDefinition> Wands;
	TMap<FName, FMatterFluxItemDefinition> Items;
	TMap<FName, FMatterFluxQuestDefinition> Quests;
	TMap<FName, FMatterFluxStructureDefinition> Structures;
	TMap<FName, FMatterFluxCustomMapDefinition> CustomMaps;
};

using FMatterFluxContentRegistryPtr =
	TSharedPtr<const FMatterFluxContentRegistry, ESPMode::ThreadSafe>;
