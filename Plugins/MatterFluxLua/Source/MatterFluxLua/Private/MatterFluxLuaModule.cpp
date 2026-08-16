#include "IMatterFluxScriptRuntime.h"

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

THIRD_PARTY_INCLUDES_START
extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogMatterFluxLua, Log, All);

namespace MatterFluxLua
{
	constexpr SIZE_T MemoryBudgetBytes = 8 * 1024 * 1024;
	constexpr int32 SourceBudgetBytes = 1024 * 1024;
	constexpr int32 OriginBudgetCharacters = 4'096;
	constexpr int32 InstructionBudget = 1'000'000;
	constexpr int32 HookInterval = 1'000;
	constexpr int32 MaximumDefinitionsPerCategory = 1'024;
	constexpr int32 MaximumDecoratorCount = 4'096;
	constexpr size_t MaximumContentStringUtf8Bytes = 256;

	struct FMemoryContext
	{
		SIZE_T UsedBytes = 0;
		SIZE_T BudgetBytes = MemoryBudgetBytes;
	};

	struct FRegistryBuilder
	{
		FMatterFluxContentRegistry Registry;
		bool bManifestSet = false;
		bool bFragmentationSettingsSet = false;

		bool SetManifest(
			const FString& PackId,
			const int32 Revision,
			const int32 SchemaVersion,
			FString& OutError);
		bool AddMaterial(
			const FMatterFluxMaterialDefinition& Definition,
			FString& OutError);
		bool AddReaction(
			const FMatterFluxReactionDefinition& Definition,
			FString& OutError);
		bool AddCombustion(
			const FMatterFluxCombustionDefinition& Definition,
			FString& OutError);
		bool AddDecorator(
			const FMatterFluxDecoratorDefinition& Definition,
			FString& OutError);
		bool AddEntity(
			const FMatterFluxEntityDefinition& Definition,
			FString& OutError);
		bool AddSpell(
			const FMatterFluxSpellDefinition& Definition,
			FString& OutError);
		bool AddWand(
			const FMatterFluxWandDefinition& Definition,
			FString& OutError);
		bool AddItem(
			const FMatterFluxItemDefinition& Definition,
			FString& OutError);
		bool AddQuest(
			const FMatterFluxQuestDefinition& Definition,
			FString& OutError);
		bool SetFragmentationSettings(
			int32 MinDetachedAreaPixels,
			FString& OutError);
		bool Validate(FString& OutError) const;
	};

	struct FExecutionContext
	{
		FRegistryBuilder* Builder = nullptr;
		FString Error;
		int32 RemainingInstructions = InstructionBudget;
	};

	static bool IsValidContentId(const FString& Id)
	{
		if (Id.IsEmpty() || Id.Len() > 64)
		{
			return false;
		}

		for (const TCHAR Character : Id)
		{
			if (!FChar::IsAlnum(Character)
				&& Character != TEXT('_')
				&& Character != TEXT('-')
				&& Character != TEXT('.')
				&& Character != TEXT(':'))
			{
				return false;
			}
		}
		return true;
	}

	static bool IsFiniteNonNegative(const double Value)
	{
		return FMath::IsFinite(Value) && Value >= 0.0;
	}

	struct FDefaultSourceFile
	{
		FString AbsolutePath;
		FString RelativePath;
	};

	static bool BuildDefaultSourceBundle(
		const FString& InLuaRoot,
		FString& OutSource,
		FString& OutOrigin,
		FString* OutError)
	{
		FString LuaRoot = FPaths::ConvertRelativePathToFull(InLuaRoot);
		FPaths::NormalizeDirectoryName(LuaRoot);
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.DirectoryExists(*LuaRoot))
		{
			if (OutError)
			{
				*OutError = FString::Printf(
					TEXT("default Lua root does not exist: '%s'"),
					*LuaRoot);
			}
			return false;
		}

		TArray<FDefaultSourceFile> Files;
		const auto AddFile = [&Files, &LuaRoot](const FString& Path)
		{
			FString AbsolutePath = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(AbsolutePath);
			FString NormalizedRoot = LuaRoot;
			FPaths::NormalizeFilename(NormalizedRoot);
			const FString RootPrefix = NormalizedRoot + TEXT("/");
			if (!AbsolutePath.StartsWith(
				RootPrefix,
				ESearchCase::IgnoreCase))
			{
				return false;
			}
			FDefaultSourceFile& File = Files.AddDefaulted_GetRef();
			File.AbsolutePath = AbsolutePath;
			File.RelativePath = AbsolutePath.Mid(RootPrefix.Len());
			return true;
		};

		if (!AddFile(FPaths::Combine(LuaRoot, TEXT("MatterFluxEngine.lua")))
			|| !AddFile(FPaths::Combine(LuaRoot, TEXT("MatterFluxContent.lua"))))
		{
			if (OutError)
			{
				*OutError = TEXT("default Lua entry files escape the Lua root");
			}
			return false;
		}

		const TCHAR* ModuleDirectories[] = {
			TEXT("Materials"),
			TEXT("World"),
			TEXT("Spells"),
			TEXT("Wands"),
			TEXT("Items"),
			TEXT("Quests")
		};
		for (const TCHAR* DirectoryName : ModuleDirectories)
		{
			const FString DirectoryPath =
				FPaths::Combine(LuaRoot, DirectoryName);
			if (!FileManager.DirectoryExists(*DirectoryPath))
			{
				if (OutError)
				{
					*OutError = FString::Printf(
						TEXT("required Lua module directory is missing: '%s'"),
						*DirectoryPath);
				}
				return false;
			}
			TArray<FString> ModulePaths;
			FileManager.FindFilesRecursive(
				ModulePaths,
				*DirectoryPath,
				TEXT("*.lua"),
				true,
				false,
				false);
			ModulePaths.Sort([](const FString& A, const FString& B)
			{
				return A.Compare(B, ESearchCase::IgnoreCase) < 0;
			});
			for (const FString& ModulePath : ModulePaths)
			{
				if (!AddFile(ModulePath))
				{
					if (OutError)
					{
						*OutError = FString::Printf(
							TEXT("Lua module escapes the Lua root: '%s'"),
							*ModulePath);
					}
					return false;
				}
			}
		}

		IPlatformFile& PlatformFile =
			FPlatformFileManager::Get().GetPlatformFile();
		int64 TotalBytes = 0;
		OutSource.Reset();
		for (const FDefaultSourceFile& File : Files)
		{
			const int64 FileBytes = PlatformFile.FileSize(*File.AbsolutePath);
			const FString Header = FString::Printf(
				TEXT("\n-- file: %s\n"),
				*File.RelativePath);
			FTCHARToUTF8 HeaderUtf8(*Header);
			if (FileBytes < 0)
			{
				if (OutError)
				{
					*OutError = FString::Printf(
						TEXT("default Lua source does not exist: '%s'"),
						*File.AbsolutePath);
				}
				return false;
			}
			TotalBytes += FileBytes + HeaderUtf8.Length();
			if (FileBytes > SourceBudgetBytes || TotalBytes > SourceBudgetBytes)
			{
				if (OutError)
				{
					*OutError = FString::Printf(
						TEXT("default Lua bundle exceeds the %d-byte source budget"),
						SourceBudgetBytes);
				}
				return false;
			}
			FString FileSource;
			if (!FFileHelper::LoadFileToString(
				FileSource,
				*File.AbsolutePath))
			{
				if (OutError)
				{
					*OutError = FString::Printf(
						TEXT("could not read default Lua source '%s'"),
						*File.AbsolutePath);
				}
				return false;
			}
			OutSource += Header;
			OutSource += FileSource;
		}
		OutOrigin = FString::Printf(
			TEXT("default Lua bundle (%d files) at %s"),
			Files.Num(),
			*LuaRoot);
		if (OutOrigin.Len() > OriginBudgetCharacters)
		{
			if (OutError)
			{
				*OutError = TEXT("default Lua bundle origin is too long");
			}
			return false;
		}
		return true;
	}

	bool FRegistryBuilder::SetManifest(
		const FString& PackId,
		const int32 Revision,
		const int32 SchemaVersion,
		FString& OutError)
	{
		if (bManifestSet)
		{
			OutError = TEXT("content.set_manifest may only be called once");
			return false;
		}
		if (!IsValidContentId(PackId))
		{
			OutError = TEXT("manifest pack_id is invalid");
			return false;
		}
		if (SchemaVersion != MATTERFLUX_LUA_SCHEMA_VERSION)
		{
			OutError = FString::Printf(
				TEXT("unsupported content schema %d; expected %d"),
				SchemaVersion,
				MATTERFLUX_LUA_SCHEMA_VERSION);
			return false;
		}
		if (Revision < 0)
		{
			OutError = TEXT("manifest revision must be non-negative");
			return false;
		}

		Registry.Manifest.PackId = PackId;
		Registry.Manifest.Revision = Revision;
		Registry.Manifest.SchemaVersion = SchemaVersion;
		bManifestSet = true;
		return true;
	}

	bool FRegistryBuilder::AddMaterial(
		const FMatterFluxMaterialDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		if (!IsValidContentId(Id))
		{
			OutError = TEXT("material id is invalid");
			return false;
		}
		const bool bValidPhase =
			Definition.Phase == EMatterFluxMaterialPhase::StaticSolid
			|| Definition.Phase == EMatterFluxMaterialPhase::Powder
			|| Definition.Phase == EMatterFluxMaterialPhase::Liquid
			|| Definition.Phase == EMatterFluxMaterialPhase::Gas;
		if (!IsFiniteNonNegative(Definition.Density)
			|| !IsFiniteNonNegative(Definition.Hardness)
			|| !FMath::IsFinite(Definition.Color.R)
			|| !FMath::IsFinite(Definition.Color.G)
			|| !FMath::IsFinite(Definition.Color.B)
			|| !FMath::IsFinite(Definition.Color.A)
			|| !bValidPhase)
		{
			OutError = FString::Printf(
				TEXT("material '%s' contains an invalid number"),
				*Id);
			return false;
		}
		if (Registry.Materials.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate material id '%s'"),
				*Id);
			return false;
		}
		Registry.Materials.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddReaction(
		const FMatterFluxReactionDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		const bool bValidOutputA =
			Definition.OutputA == TEXT("empty")
			|| IsValidContentId(Definition.OutputA.ToString());
		const bool bValidOutputB =
			Definition.OutputB == TEXT("empty")
			|| IsValidContentId(Definition.OutputB.ToString());
		if (!IsValidContentId(Id)
			|| !IsValidContentId(Definition.InputA.ToString())
			|| !IsValidContentId(Definition.InputB.ToString())
			|| !bValidOutputA
			|| !bValidOutputB
			|| Definition.ChancePermille < 0
			|| Definition.ChancePermille > 1000)
		{
			OutError = FString::Printf(
				TEXT("reaction '%s' contains invalid data"),
				*Id);
			return false;
		}
		if (Registry.Reactions.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate reaction id '%s'"),
				*Id);
			return false;
		}
		for (const TPair<FName, FMatterFluxReactionDefinition>& Pair
			: Registry.Reactions)
		{
			const bool bSameInputPair =
				(Pair.Value.InputA == Definition.InputA
					&& Pair.Value.InputB == Definition.InputB)
				|| (Pair.Value.InputA == Definition.InputB
					&& Pair.Value.InputB == Definition.InputA);
			if (bSameInputPair)
			{
				OutError = FString::Printf(
					TEXT("reaction '%s' duplicates the unordered input pair used by '%s'"),
					*Id,
					*Pair.Key.ToString());
				return false;
			}
		}
		Registry.Reactions.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddCombustion(
		const FMatterFluxCombustionDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		if (!IsValidContentId(Id)
			|| !IsValidContentId(Definition.FuelMaterial.ToString())
			|| !IsValidContentId(Definition.FlameMaterial.ToString())
			|| !IsValidContentId(Definition.SmokeMaterial.ToString())
			|| !IsValidContentId(Definition.ResidueMaterial.ToString())
			|| Definition.IgnitionChancePermille < 0
			|| Definition.IgnitionChancePermille > 1000
			|| Definition.SpreadChancePermille < 0
			|| Definition.SpreadChancePermille > 1000
			|| Definition.BurnDurationSteps < 1
			|| Definition.BurnDurationSteps > 255
			|| Definition.SmokeChancePermille < 0
			|| Definition.SmokeChancePermille > 1000)
		{
			OutError = FString::Printf(
				TEXT("combustion '%s' contains invalid data"),
				*Id);
			return false;
		}
		if (Registry.Combustions.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate combustion id '%s'"),
				*Id);
			return false;
		}
		for (const TPair<FName, FMatterFluxCombustionDefinition>& Pair
			: Registry.Combustions)
		{
			if (Pair.Value.FuelMaterial == Definition.FuelMaterial)
			{
				OutError = FString::Printf(
					TEXT("fuel material '%s' already has a combustion rule"),
					*Definition.FuelMaterial.ToString());
				return false;
			}
		}
		Registry.Combustions.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddDecorator(
		const FMatterFluxDecoratorDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		if (!IsValidContentId(Id)
			|| !IsValidContentId(Definition.GeneratorId.ToString())
			|| !IsValidContentId(Definition.MaterialId.ToString()))
		{
			OutError = TEXT("decorator id, generator_id, or material_id is invalid");
			return false;
		}
		if (!IsFiniteNonNegative(Definition.SpawnWeight)
			|| Definition.MinCount < 0
			|| Definition.MaxCount < Definition.MinCount
			|| Definition.MaxCount > MaximumDecoratorCount)
		{
			OutError = FString::Printf(
				TEXT("decorator '%s' must use counts between 0 and %d"),
				*Id,
				MaximumDecoratorCount);
			return false;
		}
		if (Registry.Decorators.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate decorator id '%s'"),
				*Id);
			return false;
		}
		Registry.Decorators.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddEntity(
		const FMatterFluxEntityDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		if (!IsValidContentId(Id)
			|| !IsValidContentId(Definition.Behavior)
			|| !IsFiniteNonNegative(Definition.MaxHealth)
			|| !IsFiniteNonNegative(Definition.MoveSpeed))
		{
			OutError = FString::Printf(
				TEXT("entity '%s' contains invalid data"),
				*Id);
			return false;
		}
		if (Registry.Entities.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate entity id '%s'"),
				*Id);
			return false;
		}
		Registry.Entities.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddSpell(
		const FMatterFluxSpellDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		const bool bFinite =
			IsFiniteNonNegative(Definition.ManaCost)
			&& IsFiniteNonNegative(Definition.Damage)
			&& FMath::IsFinite(Definition.DamageAdd)
			&& IsFiniteNonNegative(Definition.DamageMultiplier)
			&& IsFiniteNonNegative(Definition.Speed)
			&& IsFiniteNonNegative(Definition.SpeedMultiplier)
			&& IsFiniteNonNegative(Definition.Lifetime)
			&& IsFiniteNonNegative(Definition.LifetimeMultiplier)
			&& IsFiniteNonNegative(Definition.Radius)
			&& IsFiniteNonNegative(Definition.EndRadius)
			&& IsFiniteNonNegative(Definition.Range)
			&& IsFiniteNonNegative(Definition.Thickness)
			&& FMath::IsFinite(Definition.SpreadDelta)
			&& FMath::IsFinite(Definition.CastDelayDelta)
			&& FMath::IsFinite(Definition.RechargeTimeDelta)
			&& FMath::IsFinite(Definition.Color.R)
			&& FMath::IsFinite(Definition.Color.G)
			&& FMath::IsFinite(Definition.Color.B)
			&& FMath::IsFinite(Definition.Color.A)
			&& IsFiniteNonNegative(Definition.OrbitRadius)
			&& IsFiniteNonNegative(Definition.CarrierLifetimeOverride)
			&& IsFiniteNonNegative(Definition.VerticalImpulse);
		const bool bValidKind =
			Definition.Kind == EMatterFluxSpellKind::Projectile
			|| Definition.Kind == EMatterFluxSpellKind::Modifier
			|| Definition.Kind == EMatterFluxSpellKind::Multicast
			|| Definition.Kind == EMatterFluxSpellKind::Trigger
			|| Definition.Kind == EMatterFluxSpellKind::TriggerModifier
			|| Definition.Kind == EMatterFluxSpellKind::Jump
			|| Definition.Kind == EMatterFluxSpellKind::Cut
			|| Definition.Kind == EMatterFluxSpellKind::Flame;
		const bool bValidColor = !Definition.bOverrideColor
			|| (Definition.Color.R >= 0.0f && Definition.Color.R <= 1.0f
				&& Definition.Color.G >= 0.0f && Definition.Color.G <= 1.0f
				&& Definition.Color.B >= 0.0f && Definition.Color.B <= 1.0f
				&& Definition.Color.A >= 0.0f && Definition.Color.A <= 1.0f);
		if (!IsValidContentId(Id)
			|| Definition.DisplayName.IsEmpty()
			|| Definition.DisplayName.Len() > 96
			|| Definition.Description.Len() > 512
			|| Definition.Icon.Len() > 256
			|| !bFinite
			|| !bValidKind
			|| !bValidColor
			|| Definition.OrbitRadius > 10000.0f
			|| Definition.CarrierLifetimeOverride > 30.0f
			|| Definition.VerticalImpulse > 5000.0f
			|| Definition.DrawCount < 0
			|| Definition.DrawCount > 16
			|| Definition.TriggerDrawCount < 0
			|| Definition.TriggerDrawCount > 16
			|| Definition.StarterCount < 0
			|| Definition.StarterCount > 999)
		{
			OutError = FString::Printf(
				TEXT("spell '%s' contains invalid data"),
				*Id);
			return false;
		}
		if ((Definition.Kind == EMatterFluxSpellKind::Projectile
				|| Definition.Kind == EMatterFluxSpellKind::Trigger)
			&& (Definition.Speed <= 0.0f
				|| Definition.Lifetime <= 0.0f
				|| Definition.Radius <= 0.0f))
		{
			OutError = FString::Printf(
				TEXT("projectile spell '%s' requires positive speed, lifetime, and radius"),
				*Id);
			return false;
		}
		if (Definition.Kind == EMatterFluxSpellKind::Multicast
			&& Definition.DrawCount < 2)
		{
			OutError = FString::Printf(
				TEXT("multicast spell '%s' must draw at least two cards"),
				*Id);
			return false;
		}
		if (Definition.Kind == EMatterFluxSpellKind::Trigger
			&& Definition.TriggerDrawCount < 1)
		{
			OutError = FString::Printf(
				TEXT("trigger spell '%s' must draw at least one payload card"),
				*Id);
			return false;
		}
		if (Definition.Kind == EMatterFluxSpellKind::TriggerModifier
			&& Definition.DrawCount != 2)
		{
			OutError = FString::Printf(
				TEXT("trigger modifier spell '%s' must expose exactly two children"),
				*Id);
			return false;
		}
		if (Definition.Kind == EMatterFluxSpellKind::Jump
			&& Definition.VerticalImpulse <= 0.0f)
		{
			OutError = FString::Printf(
				TEXT("jump spell '%s' requires positive vertical_impulse"),
				*Id);
			return false;
		}
		if (Definition.Kind == EMatterFluxSpellKind::Cut
			&& (Definition.Range <= 0.0f
				|| Definition.Radius <= 0.0f
				|| Definition.Thickness <= 0.0f))
		{
			OutError = FString::Printf(
				TEXT("cut spell '%s' requires positive range, radius, and thickness"),
				*Id);
			return false;
		}
		if (Definition.Kind == EMatterFluxSpellKind::Flame
			&& (Definition.Range <= 0.0f
				|| Definition.Radius <= 0.0f
				|| Definition.EndRadius < Definition.Radius
				|| Definition.ImpactMaterial.IsNone()))
		{
			OutError = FString::Printf(
				TEXT("flame spell '%s' requires range, radii, and impact_material"),
				*Id);
			return false;
		}
		if (Registry.Spells.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate spell id '%s'"),
				*Id);
			return false;
		}
		Registry.Spells.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddWand(
		const FMatterFluxWandDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		if (!IsValidContentId(Id)
			|| Definition.DisplayName.IsEmpty()
			|| Definition.DisplayName.Len() > 96
			|| Definition.Description.Len() > 512
			|| Definition.Icon.Len() > 256
			|| Definition.Capacity < 1
			|| Definition.Capacity > 32
			|| Definition.DrawCount < 1
			|| Definition.DrawCount > 16
			|| !IsFiniteNonNegative(Definition.CastDelay)
			|| !IsFiniteNonNegative(Definition.RechargeTime)
			|| !IsFiniteNonNegative(Definition.ManaMax)
			|| !IsFiniteNonNegative(Definition.ManaRechargePerSecond)
			|| !FMath::IsFinite(Definition.Spread)
			|| Definition.ManaMax <= 0.0f
			|| Definition.StarterEquipmentSlot < -1
			|| Definition.StarterEquipmentSlot >= 4
			|| Definition.StarterDeck.Num() > Definition.Capacity)
		{
			OutError = FString::Printf(
				TEXT("wand '%s' contains invalid data"),
				*Id);
			return false;
		}
		if (Registry.Wands.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate wand id '%s'"),
				*Id);
			return false;
		}
		Registry.Wands.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddItem(
		const FMatterFluxItemDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		const bool bValidAction =
			Definition.UseAction == EMatterFluxItemUseAction::None
			|| Definition.UseAction == EMatterFluxItemUseAction::RestoreHealth
			|| Definition.UseAction == EMatterFluxItemUseAction::RestoreWandMana
			|| Definition.UseAction == EMatterFluxItemUseAction::GameplayEvent;
		if (!IsValidContentId(Id)
			|| Definition.DisplayName.IsEmpty()
			|| Definition.DisplayName.Len() > 96
			|| Definition.Description.Len() > 512
			|| Definition.Icon.Len() > 256
			|| Definition.MaxStack < 1
			|| Definition.MaxStack > 999999
			|| Definition.StarterCount < 0
			|| Definition.StarterCount > Definition.MaxStack
			|| !bValidAction
			|| !IsFiniteNonNegative(Definition.UseMagnitude)
			|| Definition.UseMagnitude > 1000000.0f
			|| Definition.ConsumeCount < 0
			|| Definition.ConsumeCount > Definition.MaxStack
			|| (Definition.UseAction == EMatterFluxItemUseAction::None
				&& Definition.ConsumeCount != 0)
			|| (Definition.UseAction != EMatterFluxItemUseAction::None
				&& Definition.ConsumeCount < 1)
			|| (Definition.UseAction == EMatterFluxItemUseAction::GameplayEvent
				&& Definition.GameplayEventTag.IsNone()))
		{
			OutError = FString::Printf(
				TEXT("item '%s' contains invalid data"), *Id);
			return false;
		}
		if (Registry.Items.Contains(Definition.Id))
		{
			OutError = FString::Printf(TEXT("duplicate item id '%s'"), *Id);
			return false;
		}
		Registry.Items.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddQuest(
		const FMatterFluxQuestDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		const bool bValidObjective =
			Definition.Objective == EMatterFluxQuestObjectiveKind::CompleteChildren
			|| Definition.Objective == EMatterFluxQuestObjectiveKind::EquipWand
			|| Definition.Objective == EMatterFluxQuestObjectiveKind::EquipSpell
			|| Definition.Objective == EMatterFluxQuestObjectiveKind::KillEnemies
			|| Definition.Objective == EMatterFluxQuestObjectiveKind::SpendItem
			|| Definition.Objective == EMatterFluxQuestObjectiveKind::Never;
		if (!IsValidContentId(Id)
			|| Definition.DisplayName.Len() > 96
			|| Definition.Description.IsEmpty()
			|| Definition.Description.Len() > 512
			|| Definition.CompletedDescription.Len() > 512
			|| !bValidObjective
			|| Definition.TargetCount < 1
			|| Definition.TargetCount > 1000000
			|| Definition.TargetLevel < INDEX_NONE
			|| Definition.TargetLevel > 1000000
			|| Definition.EquipmentSlot < INDEX_NONE
			|| Definition.EquipmentSlot >= 4
			|| Definition.Prerequisites.Num() > 32
			|| Definition.Subquests.Num() > 32
			|| Definition.ActivationRewards.Num() > 32
			|| Definition.CompletionRewards.Num() > 32
			|| (Definition.Objective == EMatterFluxQuestObjectiveKind::CompleteChildren
				&& Definition.Subquests.IsEmpty())
			|| (Definition.Objective == EMatterFluxQuestObjectiveKind::SpendItem
				&& Definition.TargetId.IsNone()))
		{
			OutError = FString::Printf(
				TEXT("quest '%s' contains invalid data"), *Id);
			return false;
		}
		const auto ValidateRewards = [&OutError, &Id](
			const TArray<FMatterFluxQuestRewardDefinition>& Rewards)
		{
			for (const FMatterFluxQuestRewardDefinition& Reward : Rewards)
			{
				if (Reward.ContentId.IsNone()
					|| Reward.Quantity < 1
					|| Reward.Quantity > 999999
					|| Reward.EquipmentSlot < INDEX_NONE
					|| Reward.EquipmentSlot >= 4)
				{
					OutError = FString::Printf(
						TEXT("quest '%s' contains an invalid reward"), *Id);
					return false;
				}
			}
			return true;
		};
		if (!ValidateRewards(Definition.ActivationRewards)
			|| !ValidateRewards(Definition.CompletionRewards))
		{
			return false;
		}
		if (Registry.Quests.Contains(Definition.Id))
		{
			OutError = FString::Printf(TEXT("duplicate quest id '%s'"), *Id);
			return false;
		}
		Registry.Quests.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::SetFragmentationSettings(
		const int32 MinDetachedAreaPixels,
		FString& OutError)
	{
		if (bFragmentationSettingsSet)
		{
			OutError =
				TEXT("content.configure_fragmentation may only be called once");
			return false;
		}
		if (MinDetachedAreaPixels < 1
			|| MinDetachedAreaPixels > 256 * 256)
		{
			OutError =
				TEXT("minimum detached area must be between 1 and 65536 mask cells");
			return false;
		}
		Registry.Fragmentation.MinDetachedAreaPixels =
			MinDetachedAreaPixels;
		bFragmentationSettingsSet = true;
		return true;
	}

	bool FRegistryBuilder::Validate(FString& OutError) const
	{
		if (!bManifestSet)
		{
			OutError = TEXT("content.set_manifest was not called");
			return false;
		}
		for (const TPair<FName, FMatterFluxDecoratorDefinition>& Pair
			: Registry.Decorators)
		{
			if (!Registry.Materials.Contains(Pair.Value.MaterialId))
			{
				OutError = FString::Printf(
					TEXT("decorator '%s' references missing material '%s'"),
					*Pair.Key.ToString(),
					*Pair.Value.MaterialId.ToString());
				return false;
			}
		}
		for (const TPair<FName, FMatterFluxReactionDefinition>& Pair
			: Registry.Reactions)
		{
			const auto IsKnownOutput = [this](const FName MaterialId)
			{
				return MaterialId == TEXT("empty")
					|| Registry.Materials.Contains(MaterialId);
			};
			if (!Registry.Materials.Contains(Pair.Value.InputA)
				|| !Registry.Materials.Contains(Pair.Value.InputB)
				|| !IsKnownOutput(Pair.Value.OutputA)
				|| !IsKnownOutput(Pair.Value.OutputB))
			{
				OutError = FString::Printf(
					TEXT("reaction '%s' references a missing material"),
					*Pair.Key.ToString());
				return false;
			}
		}
		for (const TPair<FName, FMatterFluxCombustionDefinition>& Pair
			: Registry.Combustions)
		{
			if (!Registry.Materials.Contains(Pair.Value.FuelMaterial)
				|| !Registry.Materials.Contains(Pair.Value.FlameMaterial)
				|| !Registry.Materials.Contains(Pair.Value.SmokeMaterial)
				|| !Registry.Materials.Contains(Pair.Value.ResidueMaterial))
			{
				OutError = FString::Printf(
					TEXT("combustion '%s' references a missing material"),
					*Pair.Key.ToString());
				return false;
			}
		}
		TSet<int32> StarterEquipmentSlots;
		for (const TPair<FName, FMatterFluxSpellDefinition>& Pair
			: Registry.Spells)
		{
			if (!Pair.Value.ImpactMaterial.IsNone()
				&& !Registry.Materials.Contains(
					Pair.Value.ImpactMaterial))
			{
				OutError = FString::Printf(
					TEXT("spell '%s' references missing impact material '%s'"),
					*Pair.Key.ToString(),
					*Pair.Value.ImpactMaterial.ToString());
				return false;
			}
		}
		for (const TPair<FName, FMatterFluxWandDefinition>& Pair
			: Registry.Wands)
		{
			const FMatterFluxWandDefinition& Wand = Pair.Value;
			if (Wand.StarterEquipmentSlot >= 0)
			{
				if (StarterEquipmentSlots.Contains(Wand.StarterEquipmentSlot))
				{
					OutError = FString::Printf(
						TEXT("multiple starter wands use equipment slot %d"),
						Wand.StarterEquipmentSlot);
					return false;
				}
				StarterEquipmentSlots.Add(Wand.StarterEquipmentSlot);
			}
			for (const FName SpellId : Wand.StarterDeck)
			{
				if (!Registry.Spells.Contains(SpellId))
				{
					OutError = FString::Printf(
						TEXT("wand '%s' starter_deck references missing spell '%s'"),
						*Pair.Key.ToString(),
						*SpellId.ToString());
					return false;
				}
			}
		}
		const auto ValidateRewardReferences = [this, &OutError](
			const FName QuestId,
			const TArray<FMatterFluxQuestRewardDefinition>& Rewards)
		{
			for (const FMatterFluxQuestRewardDefinition& Reward : Rewards)
			{
				const bool bFound =
					(Reward.Kind == EMatterFluxQuestRewardKind::Item
						&& Registry.Items.Contains(Reward.ContentId))
					|| (Reward.Kind == EMatterFluxQuestRewardKind::Spell
						&& Registry.Spells.Contains(Reward.ContentId))
					|| (Reward.Kind == EMatterFluxQuestRewardKind::Wand
						&& Registry.Wands.Contains(Reward.ContentId));
				if (!bFound)
				{
					OutError = FString::Printf(
						TEXT("quest '%s' reward references missing content '%s'"),
						*QuestId.ToString(),
						*Reward.ContentId.ToString());
					return false;
				}
			}
			return true;
		};
		for (const TPair<FName, FMatterFluxQuestDefinition>& Pair
			: Registry.Quests)
		{
			const FMatterFluxQuestDefinition& Quest = Pair.Value;
			TSet<FName> SeenLinks;
			for (const FName Prerequisite : Quest.Prerequisites)
			{
				if (Prerequisite == Pair.Key
					|| SeenLinks.Contains(Prerequisite)
					|| !Registry.Quests.Contains(Prerequisite))
				{
					OutError = FString::Printf(
						TEXT("quest '%s' has an invalid prerequisite '%s'"),
						*Pair.Key.ToString(), *Prerequisite.ToString());
					return false;
				}
				SeenLinks.Add(Prerequisite);
			}
			SeenLinks.Reset();
			for (const FName Subquest : Quest.Subquests)
			{
				if (Subquest == Pair.Key
					|| SeenLinks.Contains(Subquest)
					|| !Registry.Quests.Contains(Subquest))
				{
					OutError = FString::Printf(
						TEXT("quest '%s' has an invalid child '%s'"),
						*Pair.Key.ToString(), *Subquest.ToString());
					return false;
				}
				SeenLinks.Add(Subquest);
			}
			const bool bTargetExists = Quest.TargetId.IsNone()
				|| (Quest.Objective == EMatterFluxQuestObjectiveKind::EquipWand
					&& Registry.Wands.Contains(Quest.TargetId))
				|| (Quest.Objective == EMatterFluxQuestObjectiveKind::EquipSpell
					&& Registry.Spells.Contains(Quest.TargetId))
				|| (Quest.Objective == EMatterFluxQuestObjectiveKind::KillEnemies
					&& Registry.Entities.Contains(Quest.TargetId))
				|| (Quest.Objective == EMatterFluxQuestObjectiveKind::SpendItem
					&& Registry.Items.Contains(Quest.TargetId));
			if (!bTargetExists)
			{
				OutError = FString::Printf(
					TEXT("quest '%s' references missing target '%s'"),
					*Pair.Key.ToString(), *Quest.TargetId.ToString());
				return false;
			}
			if (!ValidateRewardReferences(
					Pair.Key, Quest.ActivationRewards)
				|| !ValidateRewardReferences(
					Pair.Key, Quest.CompletionRewards))
			{
				return false;
			}
		}
		TSet<FName> Visiting;
		TSet<FName> Visited;
		TFunction<bool(FName)> VisitQuest = [&](const FName QuestId)
		{
			if (Visiting.Contains(QuestId))
			{
				OutError = FString::Printf(
					TEXT("quest graph contains a cycle at '%s'"),
					*QuestId.ToString());
				return false;
			}
			if (Visited.Contains(QuestId))
			{
				return true;
			}
			Visiting.Add(QuestId);
			const FMatterFluxQuestDefinition& Quest =
				Registry.Quests.FindChecked(QuestId);
			for (const FName Link : Quest.Subquests)
			{
				if (!VisitQuest(Link)) return false;
			}
			for (const FName Link : Quest.Prerequisites)
			{
				if (!VisitQuest(Link)) return false;
			}
			Visiting.Remove(QuestId);
			Visited.Add(QuestId);
			return true;
		};
		for (const TPair<FName, FMatterFluxQuestDefinition>& Pair
			: Registry.Quests)
		{
			if (!VisitQuest(Pair.Key)) return false;
		}
		return true;
	}

	static void* LuaAllocator(
		void* UserData,
		void* Pointer,
		const size_t OldSize,
		const size_t NewSize)
	{
		FMemoryContext& Context = *static_cast<FMemoryContext*>(UserData);
		if (NewSize == 0)
		{
			if (Pointer)
			{
				Context.UsedBytes =
					OldSize <= Context.UsedBytes
					? Context.UsedBytes - OldSize
					: 0;
				FMemory::Free(Pointer);
			}
			return nullptr;
		}

		const SIZE_T AccountedOldSize = Pointer ? OldSize : 0;
		const SIZE_T RetainedSize =
			Context.UsedBytes
			- FMath::Min(Context.UsedBytes, AccountedOldSize);
		if (NewSize > Context.BudgetBytes
			|| RetainedSize > Context.BudgetBytes - NewSize)
		{
			return nullptr;
		}
		const SIZE_T NextSize = RetainedSize + NewSize;

		void* Result = FMemory::Realloc(Pointer, NewSize);
		if (Result)
		{
			Context.UsedBytes = NextSize;
		}
		return Result;
	}

	static FExecutionContext& GetExecutionContext(lua_State* State)
	{
		return **static_cast<FExecutionContext**>(lua_getextraspace(State));
	}

	static void InstructionHook(lua_State* State, lua_Debug*)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		Context.RemainingInstructions -= HookInterval;
		if (Context.RemainingInstructions <= 0)
		{
			Context.Error = TEXT("Lua content script exceeded its instruction budget");
			luaL_error(State, "instruction budget exceeded");
		}
	}

	static bool ReadString(
		lua_State* State,
		const int32 Index,
		FString& OutValue,
		FString& OutError)
	{
		if (lua_type(State, Index) != LUA_TSTRING)
		{
			OutError = FString::Printf(TEXT("argument %d must be a string"), Index);
			return false;
		}
		size_t Length = 0;
		const char* Value = lua_tolstring(State, Index, &Length);
		if (!Value || Length == 0)
		{
			OutError = FString::Printf(TEXT("argument %d is not a valid id"), Index);
			return false;
		}
		if (Length > MaximumContentStringUtf8Bytes)
		{
			OutError = FString::Printf(
				TEXT("argument %d exceeds the content string length limit"),
				Index);
			return false;
		}
		for (size_t CharacterIndex = 0; CharacterIndex < Length; ++CharacterIndex)
		{
			if (Value[CharacterIndex] == '\0')
			{
				OutError = FString::Printf(
					TEXT("argument %d contains an embedded null"),
					Index);
				return false;
			}
		}
		OutValue = UTF8_TO_TCHAR(Value);
		return true;
	}

	static bool ReadContentId(
		lua_State* State,
		const int32 Index,
		FString& OutValue,
		FString& OutError)
	{
		if (!ReadString(State, Index, OutValue, OutError))
		{
			return false;
		}
		if (!IsValidContentId(OutValue))
		{
			OutError = FString::Printf(
				TEXT("argument %d is not a valid content id"),
				Index);
			return false;
		}
		return true;
	}

	static bool ReadNumber(
		lua_State* State,
		const int32 Index,
		double& OutValue,
		FString& OutError)
	{
		if (lua_type(State, Index) != LUA_TNUMBER)
		{
			OutError = FString::Printf(TEXT("argument %d must be a number"), Index);
			return false;
		}
		OutValue = lua_tonumber(State, Index);
		if (!FMath::IsFinite(OutValue))
		{
			OutError = FString::Printf(TEXT("argument %d must be finite"), Index);
			return false;
		}
		return true;
	}

	static bool ReadInteger(
		lua_State* State,
		const int32 Index,
		int32& OutValue,
		FString& OutError)
	{
		if (!lua_isinteger(State, Index))
		{
			OutError = FString::Printf(TEXT("argument %d must be an integer"), Index);
			return false;
		}
		const lua_Integer Value = lua_tointeger(State, Index);
		if (Value < MIN_int32 || Value > MAX_int32)
		{
			OutError = FString::Printf(TEXT("argument %d is out of range"), Index);
			return false;
		}
		OutValue = static_cast<int32>(Value);
		return true;
	}

	static bool ReadBoolean(
		lua_State* State,
		const int32 Index,
		bool& OutValue,
		FString& OutError)
	{
		if (lua_type(State, Index) != LUA_TBOOLEAN)
		{
			OutError = FString::Printf(
				TEXT("argument %d must be a boolean"),
				Index);
			return false;
		}
		OutValue = lua_toboolean(State, Index) != 0;
		return true;
	}

	static bool ReadMaterialPhase(
		lua_State* State,
		const int32 Index,
		EMatterFluxMaterialPhase& OutPhase,
		FString& OutError)
	{
		FString Phase;
		if (!ReadString(State, Index, Phase, OutError))
		{
			return false;
		}
		if (Phase == TEXT("static"))
		{
			OutPhase = EMatterFluxMaterialPhase::StaticSolid;
		}
		else if (Phase == TEXT("powder"))
		{
			OutPhase = EMatterFluxMaterialPhase::Powder;
		}
		else if (Phase == TEXT("liquid"))
		{
			OutPhase = EMatterFluxMaterialPhase::Liquid;
		}
		else if (Phase == TEXT("gas"))
		{
			OutPhase = EMatterFluxMaterialPhase::Gas;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("argument %d must be static, powder, liquid, or gas"),
				Index);
			return false;
		}
		return true;
	}

	static bool ReadTableStringField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		FString& OutValue,
		const bool bRequired,
		FString& OutError)
	{
		lua_getfield(State, TableIndex, Field);
		const int32 Type = lua_type(State, -1);
		if (Type == LUA_TNIL)
		{
			lua_pop(State, 1);
			if (bRequired)
			{
				OutError = FString::Printf(
					TEXT("field '%s' is required"),
					UTF8_TO_TCHAR(Field));
				return false;
			}
			return true;
		}
		if (Type != LUA_TSTRING)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' must be a string"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		size_t Length = 0;
		const char* Value = lua_tolstring(State, -1, &Length);
		if (!Value
			|| Length > MaximumContentStringUtf8Bytes)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' exceeds the content string length limit"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		for (size_t Index = 0; Index < Length; ++Index)
		{
			if (Value[Index] == '\0')
			{
				lua_pop(State, 1);
				OutError = FString::Printf(
					TEXT("field '%s' contains an embedded null"),
					UTF8_TO_TCHAR(Field));
				return false;
			}
		}
		OutValue = UTF8_TO_TCHAR(Value);
		lua_pop(State, 1);
		if (bRequired && OutValue.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("field '%s' may not be empty"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		return true;
	}

	static bool ReadTableNumberField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		float& OutValue,
		FString& OutError)
	{
		lua_getfield(State, TableIndex, Field);
		if (lua_type(State, -1) == LUA_TNIL)
		{
			lua_pop(State, 1);
			return true;
		}
		if (lua_type(State, -1) != LUA_TNUMBER)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' must be a number"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		const double Value = lua_tonumber(State, -1);
		lua_pop(State, 1);
		if (!FMath::IsFinite(Value))
		{
			OutError = FString::Printf(
				TEXT("field '%s' must be finite"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		OutValue = static_cast<float>(Value);
		return true;
	}

	static bool ReadTableIntegerField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		int32& OutValue,
		FString& OutError)
	{
		lua_getfield(State, TableIndex, Field);
		if (lua_type(State, -1) == LUA_TNIL)
		{
			lua_pop(State, 1);
			return true;
		}
		if (!lua_isinteger(State, -1))
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' must be an integer"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		const lua_Integer Value = lua_tointeger(State, -1);
		lua_pop(State, 1);
		if (Value < MIN_int32 || Value > MAX_int32)
		{
			OutError = FString::Printf(
				TEXT("field '%s' is out of range"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		OutValue = static_cast<int32>(Value);
		return true;
	}

	static bool ReadTableBooleanField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		bool& OutValue,
		FString& OutError)
	{
		lua_getfield(State, TableIndex, Field);
		if (lua_type(State, -1) == LUA_TNIL)
		{
			lua_pop(State, 1);
			return true;
		}
		if (lua_type(State, -1) != LUA_TBOOLEAN)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' must be a boolean"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		OutValue = lua_toboolean(State, -1) != 0;
		lua_pop(State, 1);
		return true;
	}

	static bool ReadTableContentIdArrayField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		TArray<FName>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();
		lua_getfield(State, TableIndex, Field);
		if (lua_type(State, -1) == LUA_TNIL)
		{
			lua_pop(State, 1);
			return true;
		}
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' must be an array of content ids"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		const size_t Count = lua_rawlen(State, -1);
		if (Count > 32)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' may contain at most 32 entries"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		OutValues.Reserve(static_cast<int32>(Count));
		for (size_t Index = 1; Index <= Count; ++Index)
		{
			lua_rawgeti(State, -1, static_cast<lua_Integer>(Index));
			FString Id;
			if (!ReadContentId(State, -1, Id, OutError))
			{
				lua_pop(State, 2);
				OutError = FString::Printf(
					TEXT("field '%s' entry %d is invalid: %s"),
					UTF8_TO_TCHAR(Field),
					static_cast<int32>(Index),
					*OutError);
				return false;
			}
			OutValues.Add(FName(*Id));
			lua_pop(State, 1);
		}
		lua_pop(State, 1);
		return true;
	}

	static bool ParseQuestRewardKind(
		const FString& Value,
		EMatterFluxQuestRewardKind& OutKind,
		FString& OutError)
	{
		if (Value == TEXT("item"))
		{
			OutKind = EMatterFluxQuestRewardKind::Item;
		}
		else if (Value == TEXT("spell"))
		{
			OutKind = EMatterFluxQuestRewardKind::Spell;
		}
		else if (Value == TEXT("wand"))
		{
			OutKind = EMatterFluxQuestRewardKind::Wand;
		}
		else
		{
			OutError = TEXT("reward kind must be item, spell, or wand");
			return false;
		}
		return true;
	}

	static bool ReadTableQuestRewardArrayField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		TArray<FMatterFluxQuestRewardDefinition>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();
		const int32 AbsoluteTableIndex = lua_absindex(State, TableIndex);
		lua_getfield(State, AbsoluteTableIndex, Field);
		if (lua_type(State, -1) == LUA_TNIL)
		{
			lua_pop(State, 1);
			return true;
		}
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' must be an array of rewards"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		const int32 RewardArrayIndex = lua_absindex(State, -1);
		const size_t Count = lua_rawlen(State, RewardArrayIndex);
		if (Count > 32)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' may contain at most 32 rewards"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		OutValues.Reserve(static_cast<int32>(Count));
		for (size_t Index = 1; Index <= Count; ++Index)
		{
			lua_rawgeti(State, RewardArrayIndex, static_cast<lua_Integer>(Index));
			if (lua_type(State, -1) != LUA_TTABLE)
			{
				lua_pop(State, 2);
				OutError = FString::Printf(
					TEXT("field '%s' reward %d must be a table"),
					UTF8_TO_TCHAR(Field), static_cast<int32>(Index));
				return false;
			}
			const int32 RewardIndex = lua_absindex(State, -1);
			FString Kind;
			FString ContentId;
			FMatterFluxQuestRewardDefinition Reward;
			if (!ReadTableStringField(
					State, RewardIndex, "kind", Kind, true, OutError)
				|| !ReadTableStringField(
					State, RewardIndex, "id", ContentId, true, OutError)
				|| !ReadTableIntegerField(
					State, RewardIndex, "quantity", Reward.Quantity, OutError)
				|| !ReadTableIntegerField(
					State, RewardIndex, "equipment_slot", Reward.EquipmentSlot, OutError)
				|| !ParseQuestRewardKind(Kind, Reward.Kind, OutError)
				|| !IsValidContentId(ContentId))
			{
				lua_pop(State, 2);
				OutError = FString::Printf(
					TEXT("field '%s' reward %d is invalid: %s"),
					UTF8_TO_TCHAR(Field), static_cast<int32>(Index), *OutError);
				return false;
			}
			Reward.ContentId = FName(*ContentId);
			OutValues.Add(MoveTemp(Reward));
			lua_pop(State, 1);
		}
		lua_pop(State, 1);
		return true;
	}

	static int32 FailLuaCall(lua_State* State, const FString& Error)
	{
		GetExecutionContext(State).Error = Error;
		// Do not call luaL_error here. It uses a long jump, which would skip
		// destructors for FString locals in these C++ bridge functions.
		// The whole load is transactional, so recording the first error and
		// rejecting the temporary builder after execution is sufficient.
		return 0;
	}

	static bool CheckDefinitionBudget(
		const int32 ExistingCount,
		const TCHAR* Category,
		FString& OutError)
	{
		if (ExistingCount < MaximumDefinitionsPerCategory)
		{
			return true;
		}
		OutError = FString::Printf(
			TEXT("content pack may contain at most %d %s definitions"),
			MaximumDefinitionsPerCategory,
			Category);
		return false;
	}

	static int32 SetManifest(lua_State* State)
	{
		if (!GetExecutionContext(State).Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		FString PackId;
		int32 Revision = 0;
		int32 SchemaVersion = 0;
		if (lua_gettop(State) != 3
			|| !ReadContentId(State, 1, PackId, Error)
			|| !ReadInteger(State, 2, Revision, Error)
			|| !ReadInteger(State, 3, SchemaVersion, Error))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("set_manifest expects pack_id, revision, schema_version")
				: Error);
		}

		FExecutionContext& Context = GetExecutionContext(State);
		if (!Context.Builder->SetManifest(
			PackId,
			Revision,
			SchemaVersion,
			Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 ConfigureFragmentation(lua_State* State)
	{
		if (!GetExecutionContext(State).Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		int32 MinDetachedAreaPixels = 0;
		if (lua_gettop(State) != 1
			|| !ReadInteger(
				State,
				1,
				MinDetachedAreaPixels,
				Error))
		{
			return FailLuaCall(
				State,
				Error.IsEmpty()
					? TEXT("configure_fragmentation expects min_detached_area_pixels")
					: Error);
		}

		if (!GetExecutionContext(State).Builder
			->SetFragmentationSettings(
				MinDetachedAreaPixels,
				Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 RegisterMaterial(lua_State* State)
	{
		if (!GetExecutionContext(State).Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		if (!CheckDefinitionBudget(
			GetExecutionContext(State).Builder->Registry.Materials.Num(),
			TEXT("material"),
			Error))
		{
			return FailLuaCall(State, Error);
		}
		FString Id;
		double Values[6] = {};
		EMatterFluxMaterialPhase Phase =
			EMatterFluxMaterialPhase::StaticSolid;
		int32 Mobility = 255;
		int32 Dispersion = 128;
		const int32 ArgumentCount = lua_gettop(State);
		if ((ArgumentCount != 7 && ArgumentCount != 10)
			|| !ReadContentId(State, 1, Id, Error))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("register_material expects 7 or 10 arguments")
				: Error);
		}
		for (int32 Index = 0; Index < 6; ++Index)
		{
			if (!ReadNumber(State, Index + 2, Values[Index], Error))
			{
				return FailLuaCall(State, Error);
			}
		}
		if (ArgumentCount == 10
			&& (!ReadMaterialPhase(State, 8, Phase, Error)
				|| !ReadInteger(State, 9, Mobility, Error)
				|| !ReadInteger(State, 10, Dispersion, Error)))
		{
			return FailLuaCall(State, Error);
		}
		if (Mobility < 0
			|| Mobility > 255
			|| Dispersion < 0
			|| Dispersion > 255)
		{
			return FailLuaCall(
				State,
				TEXT("material mobility and dispersion must be between 0 and 255"));
		}

		FMatterFluxMaterialDefinition Definition;
		Definition.Id = FName(*Id);
		Definition.Density = static_cast<float>(Values[0]);
		Definition.Hardness = static_cast<float>(Values[1]);
		Definition.Color = FLinearColor(
			static_cast<float>(Values[2]),
			static_cast<float>(Values[3]),
			static_cast<float>(Values[4]),
			static_cast<float>(Values[5]));
		Definition.Phase = Phase;
		Definition.Mobility = static_cast<uint8>(Mobility);
		Definition.Dispersion = static_cast<uint8>(Dispersion);
		if (!GetExecutionContext(State).Builder->AddMaterial(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 RegisterReaction(lua_State* State)
	{
		if (!GetExecutionContext(State).Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		if (!CheckDefinitionBudget(
			GetExecutionContext(State).Builder->Registry.Reactions.Num(),
			TEXT("reaction"),
			Error))
		{
			return FailLuaCall(State, Error);
		}
		FString Id;
		FString InputA;
		FString InputB;
		FString OutputA;
		FString OutputB;
		int32 ChancePermille = 0;
		if (lua_gettop(State) != 6
			|| !ReadContentId(State, 1, Id, Error)
			|| !ReadContentId(State, 2, InputA, Error)
			|| !ReadContentId(State, 3, InputB, Error)
			|| !ReadContentId(State, 4, OutputA, Error)
			|| !ReadContentId(State, 5, OutputB, Error)
			|| !ReadInteger(State, 6, ChancePermille, Error))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("register_reaction expects 6 arguments")
				: Error);
		}

		FMatterFluxReactionDefinition Definition;
		Definition.Id = FName(*Id);
		Definition.InputA = FName(*InputA);
		Definition.InputB = FName(*InputB);
		Definition.OutputA = FName(*OutputA);
		Definition.OutputB = FName(*OutputB);
		Definition.ChancePermille = ChancePermille;
		if (!GetExecutionContext(State).Builder->AddReaction(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 RegisterCombustion(lua_State* State)
	{
		if (!GetExecutionContext(State).Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		if (!CheckDefinitionBudget(
			GetExecutionContext(State).Builder->Registry.Combustions.Num(),
			TEXT("combustion"),
			Error))
		{
			return FailLuaCall(State, Error);
		}
		FString Id;
		FString Fuel;
		FString Flame;
		FString Smoke;
		FString Residue;
		int32 IgnitionChance = 0;
		int32 SpreadChance = 0;
		int32 BurnDuration = 0;
		int32 SmokeChance = 0;
		if (lua_gettop(State) != 9
			|| !ReadContentId(State, 1, Id, Error)
			|| !ReadContentId(State, 2, Fuel, Error)
			|| !ReadContentId(State, 3, Flame, Error)
			|| !ReadContentId(State, 4, Smoke, Error)
			|| !ReadContentId(State, 5, Residue, Error)
			|| !ReadInteger(State, 6, IgnitionChance, Error)
			|| !ReadInteger(State, 7, SpreadChance, Error)
			|| !ReadInteger(State, 8, BurnDuration, Error)
			|| !ReadInteger(State, 9, SmokeChance, Error))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("register_combustion expects 9 arguments")
				: Error);
		}

		FMatterFluxCombustionDefinition Definition;
		Definition.Id = FName(*Id);
		Definition.FuelMaterial = FName(*Fuel);
		Definition.FlameMaterial = FName(*Flame);
		Definition.SmokeMaterial = FName(*Smoke);
		Definition.ResidueMaterial = FName(*Residue);
		Definition.IgnitionChancePermille = IgnitionChance;
		Definition.SpreadChancePermille = SpreadChance;
		Definition.BurnDurationSteps = BurnDuration;
		Definition.SmokeChancePermille = SmokeChance;
		if (!GetExecutionContext(State).Builder->AddCombustion(
			Definition,
			Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 RegisterDecorator(lua_State* State)
	{
		if (!GetExecutionContext(State).Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		if (!CheckDefinitionBudget(
			GetExecutionContext(State).Builder->Registry.Decorators.Num(),
			TEXT("decorator"),
			Error))
		{
			return FailLuaCall(State, Error);
		}
		FString Id;
		FString GeneratorId;
		FString MaterialId;
		double SpawnWeight = 0.0;
		int32 MinCount = 0;
		int32 MaxCount = 0;
		bool bEnableCollision = false;
		const int32 ArgumentCount = lua_gettop(State);
		if ((ArgumentCount != 6 && ArgumentCount != 7)
			|| !ReadContentId(State, 1, Id, Error)
			|| !ReadContentId(State, 2, GeneratorId, Error)
			|| !ReadContentId(State, 3, MaterialId, Error)
			|| !ReadNumber(State, 4, SpawnWeight, Error)
			|| !ReadInteger(State, 5, MinCount, Error)
			|| !ReadInteger(State, 6, MaxCount, Error)
			|| (ArgumentCount == 7
				&& !ReadBoolean(State, 7, bEnableCollision, Error)))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("register_decorator expects 6 or 7 arguments")
				: Error);
		}

		FMatterFluxDecoratorDefinition Definition;
		Definition.Id = FName(*Id);
		Definition.GeneratorId = FName(*GeneratorId);
		Definition.MaterialId = FName(*MaterialId);
		Definition.SpawnWeight = static_cast<float>(SpawnWeight);
		Definition.MinCount = MinCount;
		Definition.MaxCount = MaxCount;
		Definition.bEnableCollision = bEnableCollision;
		if (!GetExecutionContext(State).Builder->AddDecorator(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 RegisterEntity(lua_State* State)
	{
		if (!GetExecutionContext(State).Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		if (!CheckDefinitionBudget(
			GetExecutionContext(State).Builder->Registry.Entities.Num(),
			TEXT("entity"),
			Error))
		{
			return FailLuaCall(State, Error);
		}
		FString Id;
		FString Behavior;
		double MaxHealth = 0.0;
		double MoveSpeed = 0.0;
		if (lua_gettop(State) != 4
			|| !ReadContentId(State, 1, Id, Error)
			|| !ReadContentId(State, 2, Behavior, Error)
			|| !ReadNumber(State, 3, MaxHealth, Error)
			|| !ReadNumber(State, 4, MoveSpeed, Error))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("register_entity expects 4 arguments")
				: Error);
		}

		FMatterFluxEntityDefinition Definition;
		Definition.Id = FName(*Id);
		Definition.Behavior = Behavior;
		Definition.MaxHealth = static_cast<float>(MaxHealth);
		Definition.MoveSpeed = static_cast<float>(MoveSpeed);
		if (!GetExecutionContext(State).Builder->AddEntity(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static bool ParseSpellKind(
		const FString& Value,
		EMatterFluxSpellKind& OutKind,
		FString& OutError)
	{
		if (Value == TEXT("projectile"))
		{
			OutKind = EMatterFluxSpellKind::Projectile;
		}
		else if (Value == TEXT("modifier"))
		{
			OutKind = EMatterFluxSpellKind::Modifier;
		}
		else if (Value == TEXT("multicast"))
		{
			OutKind = EMatterFluxSpellKind::Multicast;
		}
		else if (Value == TEXT("trigger"))
		{
			OutKind = EMatterFluxSpellKind::Trigger;
		}
		else if (Value == TEXT("trigger_modifier"))
		{
			OutKind = EMatterFluxSpellKind::TriggerModifier;
		}
		else if (Value == TEXT("jump"))
		{
			OutKind = EMatterFluxSpellKind::Jump;
		}
		else if (Value == TEXT("cut"))
		{
			OutKind = EMatterFluxSpellKind::Cut;
		}
		else if (Value == TEXT("flame"))
		{
			OutKind = EMatterFluxSpellKind::Flame;
		}
		else
		{
			OutError =
				TEXT("field 'kind' must be projectile, modifier, multicast, trigger, trigger_modifier, jump, cut, or flame");
			return false;
		}
		return true;
	}

	static int32 RegisterSpell(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (!Context.Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		if (lua_gettop(State) != 1
			|| lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(
				State,
				TEXT("register_spell expects one definition table"));
		}
		if (!CheckDefinitionBudget(
			Context.Builder->Registry.Spells.Num(),
			TEXT("spell"),
			Error))
		{
			return FailLuaCall(State, Error);
		}

		FMatterFluxSpellDefinition Definition;
		FString Id;
		FString Kind;
		FString ImpactMaterial;
		FString TriggerEvent;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(
				State, 1, "name", Definition.DisplayName, true, Error)
			|| !ReadTableStringField(
				State, 1, "description", Definition.Description, false, Error)
			|| !ReadTableStringField(
				State, 1, "icon", Definition.Icon, false, Error)
			|| !ReadTableStringField(State, 1, "kind", Kind, true, Error)
			|| !ReadTableNumberField(
				State, 1, "mana_cost", Definition.ManaCost, Error)
			|| !ReadTableIntegerField(
				State, 1, "draw_count", Definition.DrawCount, Error)
			|| !ReadTableIntegerField(
				State, 1, "trigger_draw_count", Definition.TriggerDrawCount, Error)
			|| !ReadTableNumberField(
				State, 1, "damage", Definition.Damage, Error)
			|| !ReadTableNumberField(
				State, 1, "damage_add", Definition.DamageAdd, Error)
			|| !ReadTableNumberField(
				State, 1, "damage_multiplier", Definition.DamageMultiplier, Error)
			|| !ReadTableNumberField(
				State, 1, "speed", Definition.Speed, Error)
			|| !ReadTableNumberField(
				State, 1, "speed_multiplier", Definition.SpeedMultiplier, Error)
			|| !ReadTableNumberField(
				State, 1, "lifetime", Definition.Lifetime, Error)
			|| !ReadTableNumberField(
				State, 1, "lifetime_multiplier", Definition.LifetimeMultiplier, Error)
			|| !ReadTableNumberField(
				State, 1, "radius", Definition.Radius, Error)
			|| !ReadTableNumberField(
				State, 1, "end_radius", Definition.EndRadius, Error)
			|| !ReadTableNumberField(
				State, 1, "range", Definition.Range, Error)
			|| !ReadTableNumberField(
				State, 1, "thickness", Definition.Thickness, Error)
			|| !ReadTableNumberField(
				State, 1, "spread", Definition.SpreadDelta, Error)
			|| !ReadTableNumberField(
				State, 1, "cast_delay", Definition.CastDelayDelta, Error)
			|| !ReadTableNumberField(
				State, 1, "recharge_time", Definition.RechargeTimeDelta, Error)
			|| !ReadTableBooleanField(
				State, 1, "override_color", Definition.bOverrideColor, Error)
			|| !ReadTableNumberField(
				State, 1, "color_r", Definition.Color.R, Error)
			|| !ReadTableNumberField(
				State, 1, "color_g", Definition.Color.G, Error)
			|| !ReadTableNumberField(
				State, 1, "color_b", Definition.Color.B, Error)
			|| !ReadTableNumberField(
				State, 1, "color_a", Definition.Color.A, Error)
			|| !ReadTableNumberField(
				State, 1, "orbit_radius", Definition.OrbitRadius, Error)
			|| !ReadTableStringField(
				State, 1, "trigger_event", TriggerEvent, false, Error)
			|| !ReadTableBooleanField(
				State, 1, "trigger_random_direction", Definition.bTriggerRandomDirection, Error)
			|| !ReadTableNumberField(
				State, 1, "carrier_lifetime", Definition.CarrierLifetimeOverride, Error)
			|| !ReadTableNumberField(
				State, 1, "vertical_impulse", Definition.VerticalImpulse, Error)
			|| !ReadTableStringField(
				State, 1, "impact_material", ImpactMaterial, false, Error)
			|| !ReadTableIntegerField(
				State, 1, "starter_count", Definition.StarterCount, Error)
			|| !ParseSpellKind(Kind, Definition.Kind, Error))
		{
			return FailLuaCall(State, Error);
		}
		if (!IsValidContentId(Id)
			|| (!ImpactMaterial.IsEmpty()
				&& !IsValidContentId(ImpactMaterial)))
		{
			return FailLuaCall(
				State,
				TEXT("spell id or impact_material is invalid"));
		}
		Definition.Id = FName(*Id);
		Definition.ImpactMaterial = ImpactMaterial.IsEmpty()
			? NAME_None
			: FName(*ImpactMaterial);
		if (TriggerEvent.IsEmpty() || TriggerEvent == TEXT("impact"))
		{
			Definition.TriggerEvent =
				EMatterFluxSpellTriggerEvent::Impact;
		}
		else if (TriggerEvent == TEXT("expired"))
		{
			Definition.TriggerEvent =
				EMatterFluxSpellTriggerEvent::Expired;
		}
		else
		{
			return FailLuaCall(
				State,
				TEXT("field 'trigger_event' must be impact or expired"));
		}
		if (!Context.Builder->AddSpell(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 RegisterWand(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (!Context.Error.IsEmpty())
		{
			return 0;
		}
		FString Error;
		if (lua_gettop(State) != 1
			|| lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(
				State,
				TEXT("register_wand expects one definition table"));
		}
		if (!CheckDefinitionBudget(
			Context.Builder->Registry.Wands.Num(),
			TEXT("wand"),
			Error))
		{
			return FailLuaCall(State, Error);
		}

		FMatterFluxWandDefinition Definition;
		FString Id;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(
				State, 1, "name", Definition.DisplayName, true, Error)
			|| !ReadTableStringField(
				State, 1, "description", Definition.Description, false, Error)
			|| !ReadTableStringField(
				State, 1, "icon", Definition.Icon, false, Error)
			|| !ReadTableIntegerField(
				State, 1, "capacity", Definition.Capacity, Error)
			|| !ReadTableBooleanField(
				State, 1, "shuffle", Definition.bShuffle, Error)
			|| !ReadTableIntegerField(
				State, 1, "draw_count", Definition.DrawCount, Error)
			|| !ReadTableNumberField(
				State, 1, "cast_delay", Definition.CastDelay, Error)
			|| !ReadTableNumberField(
				State, 1, "recharge_time", Definition.RechargeTime, Error)
			|| !ReadTableNumberField(
				State, 1, "mana_max", Definition.ManaMax, Error)
			|| !ReadTableNumberField(
				State, 1, "mana_recharge", Definition.ManaRechargePerSecond, Error)
			|| !ReadTableNumberField(
				State, 1, "spread", Definition.Spread, Error)
			|| !ReadTableIntegerField(
				State, 1, "starter_slot", Definition.StarterEquipmentSlot, Error)
			|| !ReadTableContentIdArrayField(
				State, 1, "starter_deck", Definition.StarterDeck, Error))
		{
			return FailLuaCall(State, Error);
		}
		if (!IsValidContentId(Id))
		{
			return FailLuaCall(State, TEXT("wand id is invalid"));
		}
		Definition.Id = FName(*Id);
		if (!Context.Builder->AddWand(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static bool ParseItemCategory(
		const FString& Value,
		EMatterFluxItemCategory& OutCategory,
		FString& OutError)
	{
		if (Value == TEXT("material")) OutCategory = EMatterFluxItemCategory::Material;
		else if (Value == TEXT("quest")) OutCategory = EMatterFluxItemCategory::Quest;
		else if (Value == TEXT("consumable")) OutCategory = EMatterFluxItemCategory::Consumable;
		else
		{
			OutError = TEXT("item category must be material, quest, or consumable");
			return false;
		}
		return true;
	}

	static bool ParseItemUseAction(
		const FString& Value,
		EMatterFluxItemUseAction& OutAction,
		FString& OutError)
	{
		if (Value.IsEmpty() || Value == TEXT("none")) OutAction = EMatterFluxItemUseAction::None;
		else if (Value == TEXT("restore_health")) OutAction = EMatterFluxItemUseAction::RestoreHealth;
		else if (Value == TEXT("restore_wand_mana")) OutAction = EMatterFluxItemUseAction::RestoreWandMana;
		else if (Value == TEXT("gameplay_event")) OutAction = EMatterFluxItemUseAction::GameplayEvent;
		else
		{
			OutError = TEXT("item use_action must be none, restore_health, restore_wand_mana, or gameplay_event");
			return false;
		}
		return true;
	}

	static int32 RegisterItem(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (!Context.Error.IsEmpty()) return 0;
		FString Error;
		if (lua_gettop(State) != 1 || lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(State, TEXT("register_item expects one definition table"));
		}
		if (!CheckDefinitionBudget(
			Context.Builder->Registry.Items.Num(), TEXT("item"), Error))
		{
			return FailLuaCall(State, Error);
		}

		FMatterFluxItemDefinition Definition;
		FString Id;
		FString Category = TEXT("material");
		FString UseAction = TEXT("none");
		FString GameplayEventTag;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(State, 1, "name", Definition.DisplayName, true, Error)
			|| !ReadTableStringField(State, 1, "description", Definition.Description, false, Error)
			|| !ReadTableStringField(State, 1, "icon", Definition.Icon, false, Error)
			|| !ReadTableStringField(State, 1, "category", Category, false, Error)
			|| !ReadTableIntegerField(State, 1, "max_stack", Definition.MaxStack, Error)
			|| !ReadTableIntegerField(State, 1, "starter_count", Definition.StarterCount, Error)
			|| !ReadTableStringField(State, 1, "use_action", UseAction, false, Error)
			|| !ReadTableNumberField(State, 1, "use_magnitude", Definition.UseMagnitude, Error)
			|| !ReadTableStringField(State, 1, "gameplay_event", GameplayEventTag, false, Error)
			|| !ReadTableIntegerField(State, 1, "consume_count", Definition.ConsumeCount, Error)
			|| !ParseItemCategory(Category, Definition.Category, Error)
			|| !ParseItemUseAction(UseAction, Definition.UseAction, Error)
			|| !IsValidContentId(Id)
			|| (!GameplayEventTag.IsEmpty() && !IsValidContentId(GameplayEventTag)))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("item definition contains an invalid id") : Error);
		}
		Definition.Id = FName(*Id);
		Definition.GameplayEventTag = GameplayEventTag.IsEmpty()
			? NAME_None : FName(*GameplayEventTag);
		if (!Context.Builder->AddItem(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static bool ParseQuestCategory(
		const FString& Value,
		EMatterFluxQuestCategory& OutCategory,
		FString& OutError)
	{
		if (Value == TEXT("main")) OutCategory = EMatterFluxQuestCategory::Main;
		else if (Value == TEXT("side")) OutCategory = EMatterFluxQuestCategory::Side;
		else if (Value == TEXT("objective")) OutCategory = EMatterFluxQuestCategory::Objective;
		else
		{
			OutError = TEXT("quest category must be main, side, or objective");
			return false;
		}
		return true;
	}

	static bool ParseQuestObjective(
		const FString& Value,
		EMatterFluxQuestObjectiveKind& OutObjective,
		FString& OutError)
	{
		if (Value == TEXT("complete_children")) OutObjective = EMatterFluxQuestObjectiveKind::CompleteChildren;
		else if (Value == TEXT("equip_wand")) OutObjective = EMatterFluxQuestObjectiveKind::EquipWand;
		else if (Value == TEXT("equip_spell")) OutObjective = EMatterFluxQuestObjectiveKind::EquipSpell;
		else if (Value == TEXT("kill_enemies")) OutObjective = EMatterFluxQuestObjectiveKind::KillEnemies;
		else if (Value == TEXT("spend_item")) OutObjective = EMatterFluxQuestObjectiveKind::SpendItem;
		else if (Value == TEXT("never")) OutObjective = EMatterFluxQuestObjectiveKind::Never;
		else
		{
			OutError = TEXT("quest objective must be complete_children, equip_wand, equip_spell, kill_enemies, spend_item, or never");
			return false;
		}
		return true;
	}

	static int32 RegisterQuest(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (!Context.Error.IsEmpty()) return 0;
		FString Error;
		if (lua_gettop(State) != 1 || lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(State, TEXT("register_quest expects one definition table"));
		}
		if (!CheckDefinitionBudget(
			Context.Builder->Registry.Quests.Num(), TEXT("quest"), Error))
		{
			return FailLuaCall(State, Error);
		}

		FMatterFluxQuestDefinition Definition;
		FString Id;
		FString Category;
		FString Objective;
		FString TargetId;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(State, 1, "name", Definition.DisplayName, false, Error)
			|| !ReadTableStringField(State, 1, "description", Definition.Description, true, Error)
			|| !ReadTableStringField(State, 1, "completed_description", Definition.CompletedDescription, false, Error)
			|| !ReadTableStringField(State, 1, "category", Category, true, Error)
			|| !ReadTableIntegerField(State, 1, "sort", Definition.SortOrder, Error)
			|| !ReadTableBooleanField(State, 1, "optional", Definition.bOptional, Error)
			|| !ReadTableBooleanField(State, 1, "starter", Definition.bStarter, Error)
			|| !ReadTableBooleanField(State, 1, "focus_on_activate", Definition.bFocusOnActivate, Error)
			|| !ReadTableStringField(State, 1, "objective", Objective, true, Error)
			|| !ReadTableStringField(State, 1, "target_id", TargetId, false, Error)
			|| !ReadTableIntegerField(State, 1, "target_count", Definition.TargetCount, Error)
			|| !ReadTableIntegerField(State, 1, "target_level", Definition.TargetLevel, Error)
			|| !ReadTableIntegerField(State, 1, "equipment_slot", Definition.EquipmentSlot, Error)
			|| !ReadTableContentIdArrayField(State, 1, "prerequisites", Definition.Prerequisites, Error)
			|| !ReadTableContentIdArrayField(State, 1, "children", Definition.Subquests, Error)
			|| !ReadTableQuestRewardArrayField(State, 1, "activation_rewards", Definition.ActivationRewards, Error)
			|| !ReadTableQuestRewardArrayField(State, 1, "completion_rewards", Definition.CompletionRewards, Error)
			|| !ParseQuestCategory(Category, Definition.Category, Error)
			|| !ParseQuestObjective(Objective, Definition.Objective, Error)
			|| !IsValidContentId(Id)
			|| (!TargetId.IsEmpty() && !IsValidContentId(TargetId)))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("quest definition contains an invalid id") : Error);
		}
		Definition.Id = FName(*Id);
		Definition.TargetId = TargetId.IsEmpty() ? NAME_None : FName(*TargetId);
		if (!Context.Builder->AddQuest(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static void RemoveGlobal(lua_State* State, const char* Name)
	{
		lua_pushnil(State);
		lua_setglobal(State, Name);
	}

	static void OpenRestrictedLibraries(lua_State* State)
	{
		luaL_requiref(State, "_G", luaopen_base, 1);
		lua_pop(State, 1);
		luaL_requiref(State, LUA_TABLIBNAME, luaopen_table, 1);
		lua_pop(State, 1);
		luaL_requiref(State, LUA_STRLIBNAME, luaopen_string, 1);
		lua_pop(State, 1);
		luaL_requiref(State, LUA_MATHLIBNAME, luaopen_math, 1);
		lua_pop(State, 1);
		luaL_requiref(State, LUA_UTF8LIBNAME, luaopen_utf8, 1);
		lua_pop(State, 1);

		RemoveGlobal(State, "dofile");
		RemoveGlobal(State, "loadfile");
		RemoveGlobal(State, "load");
		RemoveGlobal(State, "collectgarbage");

		lua_getglobal(State, LUA_MATHLIBNAME);
		lua_pushnil(State);
		lua_setfield(State, -2, "random");
		lua_pushnil(State);
		lua_setfield(State, -2, "randomseed");
		lua_pop(State, 1);
	}

	static void RegisterContentApi(lua_State* State)
	{
		lua_newtable(State);
		lua_pushcfunction(State, SetManifest);
		lua_setfield(State, -2, "set_manifest");
		lua_pushcfunction(State, ConfigureFragmentation);
		lua_setfield(State, -2, "configure_fragmentation");
		lua_pushcfunction(State, RegisterMaterial);
		lua_setfield(State, -2, "register_material");
		lua_pushcfunction(State, RegisterReaction);
		lua_setfield(State, -2, "register_reaction");
		lua_pushcfunction(State, RegisterCombustion);
		lua_setfield(State, -2, "register_combustion");
		lua_pushcfunction(State, RegisterDecorator);
		lua_setfield(State, -2, "register_decorator");
		lua_pushcfunction(State, RegisterEntity);
		lua_setfield(State, -2, "register_entity");
		lua_pushcfunction(State, RegisterSpell);
		lua_setfield(State, -2, "register_spell");
		lua_pushcfunction(State, RegisterWand);
		lua_setfield(State, -2, "register_wand");
		lua_pushcfunction(State, RegisterItem);
		lua_setfield(State, -2, "register_item");
		lua_pushcfunction(State, RegisterQuest);
		lua_setfield(State, -2, "register_quest");
		lua_setglobal(State, "content");
	}

	static FString HashSource(const FString& Source)
	{
		FString NormalizedSource = Source;
		NormalizedSource.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		NormalizedSource.ReplaceInline(TEXT("\r"), TEXT("\n"));
		FTCHARToUTF8 Utf8Source(*NormalizedSource);
		uint8 Digest[FSHA1::DigestSize] = {};
		FSHA1::HashBuffer(Utf8Source.Get(), Utf8Source.Length(), Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}
}

class FMatterFluxLuaModule final : public IMatterFluxScriptRuntime
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!ReloadDefaultContentPack(Error))
		{
			UE_LOG(
				LogMatterFluxLua,
				Warning,
				TEXT("Default content pack was not loaded: %s"),
				*Error);
		}

#if !UE_BUILD_SHIPPING
		LastObservedDefaultSourceHash = ActiveRegistry.IsValid()
			? ActiveRegistry->Manifest.VersionHash
			: FString();
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FMatterFluxLuaModule::TickHotReload),
			0.5f);
#endif
	}

	virtual void ShutdownModule() override
	{
#if !UE_BUILD_SHIPPING
		if (TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}
#endif
		ActiveRegistry.Reset();
	}

	virtual bool LoadContentPackFromSource(
		const FString& Source,
		const FString& Origin,
		FString& OutError) override
	{
		using namespace MatterFluxLua;

		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = TEXT("Lua content packs may only be loaded on the game thread");
			return false;
		}
		if (Origin.Len() > OriginBudgetCharacters)
		{
			OutError = TEXT("Lua content source origin is too long");
			return false;
		}
		for (int32 CharacterIndex = 0;
			CharacterIndex < Origin.Len();
			++CharacterIndex)
		{
			if (Origin[CharacterIndex] == TEXT('\0'))
			{
				OutError =
					TEXT("Lua content source origin contains an embedded null");
				return false;
			}
		}
		if (Source.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s is empty"), *Origin);
			return false;
		}
		if (Source.Len() > SourceBudgetBytes)
		{
			OutError = FString::Printf(
				TEXT("%s exceeds the %d-byte source budget"),
				*Origin,
				SourceBudgetBytes);
			return false;
		}
		for (int32 CharacterIndex = 0;
			CharacterIndex < Source.Len();
			++CharacterIndex)
		{
			if (Source[CharacterIndex] == TEXT('\0'))
			{
				OutError = FString::Printf(
					TEXT("%s contains an embedded null"),
					*Origin);
				return false;
			}
		}
		FMemoryContext MemoryContext;
		lua_State* State = lua_newstate(LuaAllocator, &MemoryContext);
		if (!State)
		{
			OutError = TEXT("failed to create the bounded Lua state");
			return false;
		}

		FRegistryBuilder Builder;
		FExecutionContext ExecutionContext;
		ExecutionContext.Builder = &Builder;
		*static_cast<FExecutionContext**>(lua_getextraspace(State)) =
			&ExecutionContext;

		OpenRestrictedLibraries(State);
		RegisterContentApi(State);
		lua_sethook(State, InstructionHook, LUA_MASKCOUNT, HookInterval);

		FTCHARToUTF8 Utf8Source(*Source);
		if (Utf8Source.Length() > SourceBudgetBytes)
		{
			OutError = FString::Printf(
				TEXT("%s exceeds the %d-byte source budget"),
				*Origin,
				SourceBudgetBytes);
			lua_close(State);
			return false;
		}
		FTCHARToUTF8 Utf8Origin(*Origin);
		int32 Result = luaL_loadbufferx(
			State,
			Utf8Source.Get(),
			Utf8Source.Length(),
			Utf8Origin.Get(),
			"t");
		if (Result == LUA_OK)
		{
			Result = lua_pcall(State, 0, 0, 0);
		}
		if (Result != LUA_OK)
		{
			if (!ExecutionContext.Error.IsEmpty())
			{
				OutError = ExecutionContext.Error;
			}
			else
			{
				const char* LuaError = lua_tostring(State, -1);
				OutError = LuaError
					? UTF8_TO_TCHAR(LuaError)
					: TEXT("unknown Lua error");
			}
			lua_close(State);
			return false;
		}

		lua_close(State);
		if (!ExecutionContext.Error.IsEmpty())
		{
			OutError = ExecutionContext.Error;
			return false;
		}
		if (!Builder.Validate(OutError))
		{
			return false;
		}

		Builder.Registry.Manifest.VersionHash = HashSource(Source);
		ActiveRegistry =
			MakeShared<FMatterFluxContentRegistry, ESPMode::ThreadSafe>(
				MoveTemp(Builder.Registry));
		ContentReloaded.Broadcast(ActiveRegistry);
		UE_LOG(
			LogMatterFluxLua,
			Log,
			TEXT("Loaded Lua content pack '%s' revision %d (%s) from %s"),
			*ActiveRegistry->Manifest.PackId,
			ActiveRegistry->Manifest.Revision,
			*ActiveRegistry->Manifest.VersionHash,
			*Origin);
		return true;
	}

	virtual bool ReloadDefaultContentPack(FString& OutError) override
	{
		FString CombinedSource;
		FString Origin;
		const FString LuaRoot = FPaths::Combine(
			FPaths::ProjectContentDir(),
			TEXT("Lua"));
		if (!MatterFluxLua::BuildDefaultSourceBundle(
			LuaRoot,
			CombinedSource,
			Origin,
			&OutError))
		{
			return false;
		}
		return LoadContentPackFromSource(
			CombinedSource,
			Origin,
			OutError);
	}

	virtual FString GetDefaultContentPackPath() const override
	{
		return FPaths::Combine(
			FPaths::ProjectContentDir(),
			TEXT("Lua"),
			TEXT("MatterFluxContent.lua"));
	}

	virtual FString GetDefaultEngineConfigPath() const override
	{
		return FPaths::Combine(
			FPaths::ProjectContentDir(),
			TEXT("Lua"),
			TEXT("MatterFluxEngine.lua"));
	}

	virtual FMatterFluxContentRegistryPtr GetActiveRegistry() const override
	{
		return ActiveRegistry;
	}

	virtual FOnMatterFluxContentReloaded& OnContentReloaded() override
	{
		return ContentReloaded;
	}

private:
#if !UE_BUILD_SHIPPING
	bool TickHotReload(float)
	{
		FString CombinedSource;
		FString Origin;
		const FString LuaRoot = FPaths::Combine(
			FPaths::ProjectContentDir(),
			TEXT("Lua"));
		if (!MatterFluxLua::BuildDefaultSourceBundle(
			LuaRoot,
			CombinedSource,
			Origin,
			nullptr))
		{
			return true;
		}
		const FString SourceHash =
			MatterFluxLua::HashSource(CombinedSource);
		if (SourceHash == LastObservedDefaultSourceHash)
		{
			return true;
		}

		// Remember the exact bytes attempted. A later completed editor write
		// is detected even if the filesystem timestamp has not advanced.
		LastObservedDefaultSourceHash = SourceHash;
		FString Error;
		if (!LoadContentPackFromSource(
			CombinedSource,
			Origin,
			Error))
		{
			UE_LOG(
				LogMatterFluxLua,
				Error,
				TEXT("Rejected Lua hot reload; previous content remains active: %s"),
				*Error);
		}
		return true;
	}

	FTSTicker::FDelegateHandle TickerHandle;
	FString LastObservedDefaultSourceHash;
#endif

	FMatterFluxContentRegistryPtr ActiveRegistry;
	FOnMatterFluxContentReloaded ContentReloaded;
};

IMPLEMENT_MODULE(FMatterFluxLuaModule, MatterFluxLua)
