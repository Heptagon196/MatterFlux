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
};

struct FMatterFluxReactionDefinition
{
	FName Id;
	FName InputA;
	FName InputB;
	FName OutputA;
	FName OutputB;
	int32 ChancePermille = 1000;
};

struct FMatterFluxCombustionDefinition
{
	FName Id;
	FName FuelMaterial;
	FName FlameMaterial;
	FName SmokeMaterial;
	FName ResidueMaterial;
	int32 IgnitionChancePermille = 1000;
	int32 SpreadChancePermille = 650;
	int32 BurnDurationSteps = 12;
	int32 SmokeChancePermille = 700;
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

enum class EMatterFluxSpellKind : uint8
{
	Projectile,
	Modifier,
	Multicast,
	Trigger,
	TriggerModifier,
	Jump,
	Cut,
	Flame
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
	float EndRadius = 0.0f;
	float Range = 0.0f;
	float Thickness = 0.0f;
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

struct FMatterFluxContentRegistry
{
	FMatterFluxContentManifest Manifest;
	FMatterFluxFragmentationSettings Fragmentation;
	TMap<FName, FMatterFluxMaterialDefinition> Materials;
	TMap<FName, FMatterFluxReactionDefinition> Reactions;
	TMap<FName, FMatterFluxCombustionDefinition> Combustions;
	TMap<FName, FMatterFluxDecoratorDefinition> Decorators;
	TMap<FName, FMatterFluxEntityDefinition> Entities;
	TMap<FName, FMatterFluxSpellDefinition> Spells;
	TMap<FName, FMatterFluxWandDefinition> Wands;
	TMap<FName, FMatterFluxItemDefinition> Items;
	TMap<FName, FMatterFluxQuestDefinition> Quests;
};

using FMatterFluxContentRegistryPtr =
	TSharedPtr<const FMatterFluxContentRegistry, ESPMode::ThreadSafe>;
