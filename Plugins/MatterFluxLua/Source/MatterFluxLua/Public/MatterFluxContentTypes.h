#pragma once

#include "CoreMinimal.h"

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
	/** 浅水处的透明度；0 完全透明，1 完全不透明。 */
	float ShallowOpacity = 1.0f;
	/** 达到吸收距离后的不透明度，必须不小于 ShallowOpacity。 */
	float DeepOpacity = 1.0f;
	/** 从浅水过渡到深水不透明度所需的视线内液体深度（厘米）。 */
	float OpacityDepth = 100.0f;
	/** 传给 UE 折射输入的物理折射率；空气为 1，水约为 1.33。 */
	float RefractionIndex = 1.0f;
};

struct FMatterFluxReactionDefinition
{
	/** 规则如何被触发：两格接触立即变换，或激活后持续并向邻格传播。 */
	enum class EKind : uint8
	{
		Contact,
		Propagating
	};

	FName Id; // Lua 中稳定且全局唯一的规则 ID。
	EKind Kind = EKind::Contact;
	FName InputA; // 主反应物；传播规则中是被逐步消耗的物质。
	FName InputB; // 接触物；传播规则中是激活该反应的物质。
	FName OutputA; // InputA 完成反应后的产物，empty 表示清空。
	FName OutputB; // InputB 完成接触反应后的产物。
	int32 ChancePermille = 1000; // 首次触发概率，使用整数千分比。
	FName EmissionMaterial; // 持续反应活跃时产生的副产物；empty 表示不排放。
	int32 PropagationChancePermille = 0; // 向四邻域传播的每步概率。
	int32 DurationSteps = 0; // 从激活到完成转换的固定模拟步数。
	int32 EmissionChancePermille = 0; // 每个固定步产生副产物事件的概率。
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
	FName SpawnQuestId;
	int32 SpawnCount = 0;
	float SpawnDistance = 600.0f;
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

struct FMatterFluxShopOfferDefinition
{
	EMatterFluxShopProductKind ProductKind = EMatterFluxShopProductKind::Item;
	FName ProductId;
	int32 ProductCount = 1;
	FName CostItemId;
	int32 CostCount = 1;
	int32 PurchaseLimit = INDEX_NONE;
};

struct FMatterFluxShopDefinition
{
	FName Id;
	FString DisplayName;
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
	FName ImpactMaterial;
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
	TArray<FMatterFluxQuestRewardDefinition> ActivationRewards;
	TArray<FMatterFluxQuestRewardDefinition> CompletionRewards;
};

struct FMatterFluxFragmentationSettings
{
	// Detached components smaller than this many mask cells are discarded.
	// Individual sources may raise this threshold, but cannot lower it.
	int32 MinDetachedAreaPixels = 1;
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
	TMap<FName, FMatterFluxCustomMapDefinition> CustomMaps;
};

using FMatterFluxContentRegistryPtr =
	TSharedPtr<const FMatterFluxContentRegistry, ESPMode::ThreadSafe>;
