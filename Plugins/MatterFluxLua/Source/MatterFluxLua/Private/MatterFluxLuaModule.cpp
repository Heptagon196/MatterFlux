#include "IMatterFluxScriptRuntime.h"

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
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
	constexpr int32 MaximumCreatureBehaviorNodes = 64;
	constexpr int32 MaximumCreatureBehaviorDepth = 8;
	constexpr int32 MaximumCreatureBehaviorChildren = 16;
	constexpr int32 MaximumShopCategories = 16;
	constexpr int32 MaximumCustomMapStamps = 256;
	constexpr int32 MaximumCustomMapMarkers = 64;
	constexpr int32 MaximumCustomMapSceneBoxes = 64;
	constexpr int32 MaximumCustomMapCameras = 8;
	constexpr int32 MaximumCustomMapPourContainers = 8;
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
		bool AddDecorator(
			const FMatterFluxDecoratorDefinition& Definition,
			FString& OutError);
		bool AddEntity(
			const FMatterFluxEntityDefinition& Definition,
			FString& OutError);
		bool AddCreature(
			const FMatterFluxCreatureDefinition& Definition,
			FString& OutError);
		bool AddDialogue(
			const FMatterFluxDialogueDefinition& Definition,
			FString& OutError);
		bool AddShop(
			const FMatterFluxShopDefinition& Definition,
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
		bool AddStructure(
			const FMatterFluxStructureDefinition& Definition,
			FString& OutError);
		bool AddCustomMap(
			const FMatterFluxCustomMapDefinition& Definition,
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

	enum class ECreatureBehaviorSubtreeShape : uint8
	{
		Predicate,
		Action
	};

	static int32 AddCreatureBehaviorNode(
		FMatterFluxCreatureBehaviorProgramDefinition& Program,
		const EMatterFluxCreatureBehaviorNodeKind Kind)
	{
		FMatterFluxCreatureBehaviorNodeDefinition& Node =
			Program.Nodes.AddDefaulted_GetRef();
		Node.Kind = Kind;
		return Program.Nodes.Num() - 1;
	}

	static int32 AddCreatureCondition(
		FMatterFluxCreatureBehaviorProgramDefinition& Program,
		const EMatterFluxCreatureBehaviorCondition Condition)
	{
		const int32 Index = AddCreatureBehaviorNode(
			Program, EMatterFluxCreatureBehaviorNodeKind::Condition);
		Program.Nodes[Index].Condition = Condition;
		return Index;
	}

	static int32 AddCreatureAction(
		FMatterFluxCreatureBehaviorProgramDefinition& Program,
		const EMatterFluxCreatureBehaviorAction Action)
	{
		const int32 Index = AddCreatureBehaviorNode(
			Program, EMatterFluxCreatureBehaviorNodeKind::Action);
		Program.Nodes[Index].Action = Action;
		return Index;
	}

	static int32 AddCreatureSequence(
		FMatterFluxCreatureBehaviorProgramDefinition& Program,
		TArray<int32>&& Children)
	{
		const int32 Index = AddCreatureBehaviorNode(
			Program, EMatterFluxCreatureBehaviorNodeKind::Sequence);
		Program.Nodes[Index].Children = MoveTemp(Children);
		return Index;
	}

	static void BuildLegacyCreatureBehaviorProgram(
		const EMatterFluxCreatureAiMode Mode,
		FMatterFluxCreatureBehaviorProgramDefinition& OutProgram)
	{
		OutProgram = {};
		if (Mode == EMatterFluxCreatureAiMode::Passive)
		{
			OutProgram.RootNodeIndex = AddCreatureAction(
				OutProgram, EMatterFluxCreatureBehaviorAction::Passive);
			return;
		}

		TArray<int32> Branches;
		Branches.Add(AddCreatureSequence(OutProgram, {
			AddCreatureCondition(
				OutProgram,
				EMatterFluxCreatureBehaviorCondition::TargetTooClose),
			AddCreatureAction(
				OutProgram,
				EMatterFluxCreatureBehaviorAction::Retreat)
		}));
		if (Mode == EMatterFluxCreatureAiMode::Boss)
		{
			Branches.Add(AddCreatureSequence(OutProgram, {
				AddCreatureCondition(
					OutProgram,
					EMatterFluxCreatureBehaviorCondition::TargetInAttackRange),
				AddCreatureCondition(
					OutProgram,
					EMatterFluxCreatureBehaviorCondition::SkillReady),
				AddCreatureAction(
					OutProgram,
					EMatterFluxCreatureBehaviorAction::Skill)
			}));
		}
		Branches.Add(AddCreatureSequence(OutProgram, {
			AddCreatureCondition(
				OutProgram,
				EMatterFluxCreatureBehaviorCondition::TargetInAttackRange),
			AddCreatureCondition(
				OutProgram,
				EMatterFluxCreatureBehaviorCondition::AttackReady),
			AddCreatureAction(
				OutProgram,
				EMatterFluxCreatureBehaviorAction::Attack)
		}));
		Branches.Add(AddCreatureSequence(OutProgram, {
			AddCreatureCondition(
				OutProgram,
				EMatterFluxCreatureBehaviorCondition::HasTarget),
			AddCreatureAction(
				OutProgram,
				EMatterFluxCreatureBehaviorAction::Chase)
		}));
		Branches.Add(AddCreatureAction(
			OutProgram, EMatterFluxCreatureBehaviorAction::Patrol));
		OutProgram.RootNodeIndex = AddCreatureBehaviorNode(
			OutProgram, EMatterFluxCreatureBehaviorNodeKind::Selector);
		OutProgram.Nodes[OutProgram.RootNodeIndex].Children =
			MoveTemp(Branches);
	}

	static bool ValidateCreatureBehaviorNode(
		const FMatterFluxCreatureBehaviorProgramDefinition& Program,
		const int32 NodeIndex,
		const int32 Depth,
		TSet<int32>& Visited,
		ECreatureBehaviorSubtreeShape& OutShape,
		FString& OutError)
	{
		if (Depth > MaximumCreatureBehaviorDepth
			|| !Program.Nodes.IsValidIndex(NodeIndex)
			|| Visited.Contains(NodeIndex))
		{
			OutError = TEXT("creature behavior tree contains a cycle, duplicate node, or invalid index");
			return false;
		}
		Visited.Add(NodeIndex);
		const FMatterFluxCreatureBehaviorNodeDefinition& Node =
			Program.Nodes[NodeIndex];
		if (Node.Children.Num() > MaximumCreatureBehaviorChildren)
		{
			OutError = TEXT("creature behavior node has too many children");
			return false;
		}

		switch (Node.Kind)
		{
		case EMatterFluxCreatureBehaviorNodeKind::Condition:
			if (!Node.Children.IsEmpty())
			{
				OutError = TEXT("creature behavior condition may not have children");
				return false;
			}
			OutShape = ECreatureBehaviorSubtreeShape::Predicate;
			return true;

		case EMatterFluxCreatureBehaviorNodeKind::Action:
			if (!Node.Children.IsEmpty())
			{
				OutError = TEXT("creature behavior action may not have children");
				return false;
			}
			OutShape = ECreatureBehaviorSubtreeShape::Action;
			return true;

		case EMatterFluxCreatureBehaviorNodeKind::Sequence:
			if (Node.Children.IsEmpty())
			{
				OutError = TEXT("creature behavior sequence must contain children");
				return false;
			}
			for (int32 ChildOffset = 0;
				ChildOffset < Node.Children.Num(); ++ChildOffset)
			{
				ECreatureBehaviorSubtreeShape ChildShape;
				if (!ValidateCreatureBehaviorNode(
					Program,
					Node.Children[ChildOffset],
					Depth + 1,
					Visited,
					ChildShape,
					OutError))
				{
					return false;
				}
				const bool bLast = ChildOffset + 1 == Node.Children.Num();
				if ((!bLast
						&& ChildShape != ECreatureBehaviorSubtreeShape::Predicate)
					|| (bLast
						&& ChildShape != ECreatureBehaviorSubtreeShape::Action))
				{
					OutError = TEXT("creature behavior sequence must end in one action after its conditions");
					return false;
				}
			}
			OutShape = ECreatureBehaviorSubtreeShape::Action;
			return true;

		case EMatterFluxCreatureBehaviorNodeKind::Selector:
			if (Node.Children.IsEmpty())
			{
				OutError = TEXT("creature behavior selector must contain children");
				return false;
			}
			for (const int32 ChildIndex : Node.Children)
			{
				ECreatureBehaviorSubtreeShape ChildShape;
				if (!ValidateCreatureBehaviorNode(
					Program,
					ChildIndex,
					Depth + 1,
					Visited,
					ChildShape,
					OutError))
				{
					return false;
				}
				if (ChildShape != ECreatureBehaviorSubtreeShape::Action)
				{
					OutError = TEXT("creature behavior selector children must select actions");
					return false;
				}
			}
			OutShape = ECreatureBehaviorSubtreeShape::Action;
			return true;

		default:
			OutError = TEXT("creature behavior tree contains an unknown node kind");
			return false;
		}
	}

	static bool ValidateCreatureBehaviorProgram(
		const FMatterFluxCreatureBehaviorProgramDefinition& Program,
		FString& OutError)
	{
		if (Program.Nodes.IsEmpty()
			|| Program.Nodes.Num() > MaximumCreatureBehaviorNodes
			|| !Program.Nodes.IsValidIndex(Program.RootNodeIndex))
		{
			OutError = TEXT("creature behavior tree must contain a valid bounded root");
			return false;
		}
		TSet<int32> Visited;
		ECreatureBehaviorSubtreeShape RootShape;
		if (!ValidateCreatureBehaviorNode(
			Program,
			Program.RootNodeIndex,
			0,
			Visited,
			RootShape,
			OutError))
		{
			return false;
		}
		if (RootShape != ECreatureBehaviorSubtreeShape::Action
			|| Visited.Num() != Program.Nodes.Num())
		{
			OutError = TEXT("creature behavior tree contains unreachable nodes or no action");
			return false;
		}
		return true;
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
			TEXT("Quests"),
			TEXT("Creatures"),
			TEXT("Dialogues"),
			TEXT("Shops"),
			TEXT("Structures"),
			TEXT("Maps")
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
			|| !FMath::IsFinite(Definition.ShallowOpacity)
			|| Definition.ShallowOpacity < 0.0f
			|| Definition.ShallowOpacity > 1.0f
			|| !FMath::IsFinite(Definition.DeepOpacity)
			|| Definition.DeepOpacity < Definition.ShallowOpacity
			|| Definition.DeepOpacity > 1.0f
			|| !FMath::IsFinite(Definition.OpacityDepth)
			|| Definition.OpacityDepth <= 0.0f
			|| !FMath::IsFinite(Definition.MovementResistance)
			|| Definition.MovementResistance < 0.0f
			|| Definition.MovementResistance > 8.0f
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
		const bool bPropagating = Definition.Kind
			== FMatterFluxReactionDefinition::EKind::Propagating;
		const bool bValidOutputA =
			Definition.OutputA == TEXT("empty")
			|| IsValidContentId(Definition.OutputA.ToString());
		const bool bValidOutputB =
			Definition.OutputB == TEXT("empty")
			|| IsValidContentId(Definition.OutputB.ToString());
		const bool bValidEmission =
			Definition.EmissionMaterial == TEXT("empty")
				? Definition.EmissionChancePermille == 0
				: IsValidContentId(
					Definition.EmissionMaterial.ToString());
		if (!IsValidContentId(Id)
			|| !IsValidContentId(Definition.InputA.ToString())
			|| !IsValidContentId(Definition.InputB.ToString())
			|| !bValidOutputA
			|| !bValidOutputB
			|| Definition.ChancePermille < 0
			|| Definition.ChancePermille > 1000
			|| (bPropagating
				&& (!bValidEmission
					|| Definition.PropagationChancePermille < 0
					|| Definition.PropagationChancePermille > 1000
					|| Definition.DurationSteps < 1
					|| Definition.DurationSteps > 255
					|| Definition.EmissionChancePermille < 0
					|| Definition.EmissionChancePermille > 1000)))
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
			if (Pair.Value.Kind != Definition.Kind)
			{
				continue;
			}
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

	bool FRegistryBuilder::AddCreature(
		const FMatterFluxCreatureDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		if (!ValidateCreatureBehaviorProgram(
			Definition.BehaviorProgram, OutError))
		{
			OutError = FString::Printf(
				TEXT("creature '%s' has an invalid behavior tree: %s"),
				*Id,
				*OutError);
			return false;
		}
		const auto IsValidCastProgram = [](
			const FMatterFluxCreatureCastProgramDefinition& Program)
		{
			return Program.ProjectileCount >= 1
				&& Program.ProjectileCount <= 32
				&& FMath::IsFinite(Program.SpreadDegrees)
				&& Program.SpreadDegrees >= 0.0f
				&& Program.SpreadDegrees <= 180.0f
				&& IsFiniteNonNegative(Program.ProjectileInterval)
				&& IsFiniteNonNegative(Program.RecoverySeconds)
				&& IsFiniteNonNegative(Program.HorizontalImpulse)
				&& IsFiniteNonNegative(Program.VerticalImpulse)
				&& FMath::IsFinite(Program.Color.R)
				&& FMath::IsFinite(Program.Color.G)
				&& FMath::IsFinite(Program.Color.B)
				&& FMath::IsFinite(Program.Color.A);
		};
		const bool bFinite =
			IsFiniteNonNegative(Definition.MaxHealth)
			&& IsFiniteNonNegative(Definition.Width)
			&& IsFiniteNonNegative(Definition.Height)
			&& IsFiniteNonNegative(Definition.Density)
			&& IsFiniteNonNegative(Definition.MoveSpeed)
			&& IsFiniteNonNegative(Definition.PerceptionRange)
			&& IsFiniteNonNegative(Definition.AttackRange)
			&& IsFiniteNonNegative(Definition.RetreatRange)
			&& IsFiniteNonNegative(Definition.TargetMemorySeconds)
			&& IsFiniteNonNegative(Definition.PatrolTurnSeconds)
			&& IsFiniteNonNegative(Definition.PatrolPauseSeconds)
			&& IsFiniteNonNegative(Definition.AttackCooldown)
			&& IsFiniteNonNegative(Definition.SkillCooldown)
			&& FMath::IsFinite(Definition.Color.R)
			&& FMath::IsFinite(Definition.Color.G)
			&& FMath::IsFinite(Definition.Color.B)
			&& FMath::IsFinite(Definition.Color.A)
			&& IsValidCastProgram(Definition.AttackProgram)
			&& IsValidCastProgram(Definition.SkillProgram);
		const bool bUsesAttackRange = Definition.BehaviorProgram.Nodes.ContainsByPredicate(
			[](const FMatterFluxCreatureBehaviorNodeDefinition& Node)
			{
				return Node.Kind == EMatterFluxCreatureBehaviorNodeKind::Condition
					&& Node.Condition
						== EMatterFluxCreatureBehaviorCondition::TargetInAttackRange;
			});
		const bool bUsesRetreatRange = Definition.BehaviorProgram.Nodes.ContainsByPredicate(
			[](const FMatterFluxCreatureBehaviorNodeDefinition& Node)
			{
				return Node.Kind == EMatterFluxCreatureBehaviorNodeKind::Condition
					&& Node.Condition
						== EMatterFluxCreatureBehaviorCondition::TargetTooClose;
			});
		if (!IsValidContentId(Id)
			|| Definition.DisplayName.IsEmpty()
			|| Definition.DisplayName.Len() > 96
			|| !bFinite
			|| Definition.MaxHealth <= 0.0f
			|| Definition.Width < 20.0f || Definition.Width > 400.0f
			|| Definition.Height < 20.0f || Definition.Height > 500.0f
			|| Definition.Density < 0.05f || Definition.Density > 20.0f
			|| Definition.MoveSpeed > 2000.0f
			|| Definition.PerceptionRange > 10000.0f
			|| (bUsesAttackRange
				&& Definition.AttackRange > Definition.PerceptionRange)
			|| (bUsesRetreatRange
				&& Definition.RetreatRange > Definition.PerceptionRange)
			|| Definition.DropItemCount < 0
			|| Definition.DropItemCount > 999999)
		{
			OutError = FString::Printf(
				TEXT("creature '%s' contains invalid data"), *Id);
			return false;
		}
		if (Registry.Creatures.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate creature id '%s'"), *Id);
			return false;
		}
		Registry.Creatures.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddDialogue(
		const FMatterFluxDialogueDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		if (!IsValidContentId(Id)
			|| Definition.DisplayName.IsEmpty()
			|| Definition.DisplayName.Len() > 96
			|| Definition.StartNodeId.IsNone()
			|| Definition.Nodes.IsEmpty()
			|| Definition.Nodes.Num() > 64)
		{
			OutError = FString::Printf(
				TEXT("dialogue '%s' contains invalid data"), *Id);
			return false;
		}
		TSet<FName> NodeIds;
		for (const FMatterFluxDialogueNodeDefinition& Node : Definition.Nodes)
		{
			if (Node.Id.IsNone()
				|| Node.Text.IsEmpty()
				|| Node.Text.Len() > 512
				|| Node.Options.Num() > 8
				|| NodeIds.Contains(Node.Id))
			{
				OutError = FString::Printf(
					TEXT("dialogue '%s' contains an invalid node"), *Id);
				return false;
			}
			NodeIds.Add(Node.Id);
			for (const FMatterFluxDialogueOptionDefinition& Option : Node.Options)
			{
				if (Option.Text.IsEmpty() || Option.Text.Len() > 128)
				{
					OutError = FString::Printf(
						TEXT("dialogue '%s' contains an invalid option"), *Id);
					return false;
				}
			}
		}
		if (!NodeIds.Contains(Definition.StartNodeId))
		{
			OutError = FString::Printf(
				TEXT("dialogue '%s' start node is missing"), *Id);
			return false;
		}
		if (Registry.Dialogues.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate dialogue id '%s'"), *Id);
			return false;
		}
		Registry.Dialogues.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddShop(
		const FMatterFluxShopDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		if (!IsValidContentId(Id)
			|| Definition.DisplayName.IsEmpty()
			|| Definition.DisplayName.Len() > 96
			|| Definition.Categories.Num() > MaximumShopCategories
			|| Definition.Offers.IsEmpty()
			|| Definition.Offers.Num() > 64)
		{
			OutError = FString::Printf(
				TEXT("shop '%s' contains invalid data"), *Id);
			return false;
		}
		TSet<FName> CategoryIds;
		for (const FMatterFluxShopCategoryDefinition& Category
			: Definition.Categories)
		{
			if (!IsValidContentId(Category.Id.ToString())
				|| Category.DisplayName.IsEmpty()
				|| Category.DisplayName.Len() > 48
				|| CategoryIds.Contains(Category.Id))
			{
				OutError = FString::Printf(
					TEXT("shop '%s' contains an invalid category"), *Id);
				return false;
			}
			CategoryIds.Add(Category.Id);
		}
		for (const FMatterFluxShopOfferDefinition& Offer : Definition.Offers)
		{
			if (Offer.ProductId.IsNone()
				|| Offer.ProductCount < 1 || Offer.ProductCount > 999999
				|| Offer.CostItemId.IsNone()
				|| Offer.CostCount < 1 || Offer.CostCount > 999999
				|| Offer.PurchaseLimit < INDEX_NONE
				|| Offer.PurchaseLimit > 999999
				|| (!Offer.CategoryId.IsNone()
					&& !CategoryIds.Contains(Offer.CategoryId)))
			{
				OutError = FString::Printf(
					TEXT("shop '%s' contains an invalid offer"), *Id);
				return false;
			}
		}
		if (Registry.Shops.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate shop id '%s'"), *Id);
			return false;
		}
		Registry.Shops.Add(Definition.Id, Definition);
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
			&& IsFiniteNonNegative(Definition.GravityScale)
			&& FMath::IsFinite(Definition.SpreadDelta)
			&& FMath::IsFinite(Definition.CastDelayDelta)
			&& FMath::IsFinite(Definition.RechargeTimeDelta)
			&& FMath::IsFinite(Definition.Color.R)
			&& FMath::IsFinite(Definition.Color.G)
			&& FMath::IsFinite(Definition.Color.B)
			&& FMath::IsFinite(Definition.Color.A)
			&& IsFiniteNonNegative(Definition.OrbitRadius)
			&& IsFiniteNonNegative(Definition.CarrierLifetimeOverride)
			&& IsFiniteNonNegative(Definition.VerticalImpulse)
			&& FMath::IsFinite(Definition.SpawnForwardOffset)
			&& FMath::IsFinite(Definition.SpawnHeightOffset);
		const bool bValidKind =
			Definition.Kind == EMatterFluxSpellKind::Projectile
			|| Definition.Kind == EMatterFluxSpellKind::Modifier
			|| Definition.Kind == EMatterFluxSpellKind::Multicast
			|| Definition.Kind == EMatterFluxSpellKind::Trigger
			|| Definition.Kind == EMatterFluxSpellKind::TriggerModifier
			|| Definition.Kind == EMatterFluxSpellKind::Jump;
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
			|| Definition.GravityScale > 4.0f
			|| !bFinite
			|| !bValidKind
			|| !bValidColor
			|| Definition.OrbitRadius > 10000.0f
			|| Definition.CarrierLifetimeOverride > 30.0f
			|| Definition.VerticalImpulse > 5000.0f
			|| FMath::Abs(Definition.SpawnForwardOffset) > 5000.0f
			|| FMath::Abs(Definition.SpawnHeightOffset) > 5000.0f
			|| Definition.MaterialAmount < 1
			|| Definition.MaterialAmount > 4096
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
			&& (Definition.Speed < 0.0f
				|| Definition.Lifetime <= 0.0f
				|| Definition.Radius <= 0.0f))
		{
			OutError = FString::Printf(
				TEXT("projectile spell '%s' requires non-negative speed and positive lifetime and radius"),
				*Id);
			return false;
		}
		if (Definition.bUsePlaneVisual
			&& Definition.Kind != EMatterFluxSpellKind::Projectile
			&& Definition.Kind != EMatterFluxSpellKind::Trigger)
		{
			OutError = FString::Printf(
				TEXT("plane visual spell '%s' must produce a projectile"),
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
			|| Definition.StarterCount < 0
			|| Definition.StarterCount > 16
			|| Definition.StarterEquipmentSlot < -1
			|| Definition.StarterEquipmentSlot
				>= MatterFlux::Magic::EquipmentSlotCount
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
			|| Definition.EquipmentSlot
				>= MatterFlux::Magic::EquipmentSlotCount
			|| Definition.Prerequisites.Num() > 32
			|| Definition.Subquests.Num() > 32
			|| Definition.ActivationCreatureSpawns.Num() > 64
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
					|| Reward.EquipmentSlot
						>= MatterFlux::Magic::EquipmentSlotCount)
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
		TSet<FName> SpawnMarkerIds;
		for (const FMatterFluxCreatureSpawnDefinition& Spawn
			: Definition.ActivationCreatureSpawns)
		{
			if (Spawn.CreatureId.IsNone()
				|| Spawn.MarkerId.IsNone()
				|| SpawnMarkerIds.Contains(Spawn.MarkerId))
			{
				OutError = FString::Printf(
					TEXT("quest '%s' contains an invalid creature spawn"), *Id);
				return false;
			}
			SpawnMarkerIds.Add(Spawn.MarkerId);
		}
		if (Registry.Quests.Contains(Definition.Id))
		{
			OutError = FString::Printf(TEXT("duplicate quest id '%s'"), *Id);
			return false;
		}
		Registry.Quests.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddCustomMap(
		const FMatterFluxCustomMapDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		const int64 Width = static_cast<int64>(Definition.MaximumCellExclusive.X)
			- Definition.MinimumCell.X;
		const int64 Height = static_cast<int64>(Definition.MaximumCellExclusive.Y)
			- Definition.MinimumCell.Y;
		constexpr int32 MaximumAbsoluteCustomMapCoordinate = 1000000;
		const auto IsSafeCoordinate = [](const FIntPoint Cell)
		{
			return FMath::Abs(static_cast<int64>(Cell.X))
					<= MaximumAbsoluteCustomMapCoordinate
				&& FMath::Abs(static_cast<int64>(Cell.Y))
					<= MaximumAbsoluteCustomMapCoordinate;
		};
		if (!IsValidContentId(Id)
			|| Definition.DisplayName.IsEmpty()
			|| !IsSafeCoordinate(Definition.MinimumCell)
			|| !IsSafeCoordinate(Definition.MaximumCellExclusive)
			|| Width < 1 || Width > 512
			|| Height < 1 || Height > 512
			|| (Definition.Stamps.IsEmpty()
				&& Definition.PourContainers.IsEmpty())
			|| Definition.Stamps.Num() > MaximumCustomMapStamps
			|| Definition.Markers.Num() > MaximumCustomMapMarkers
			|| Definition.InitialCreatureSpawns.Num() > 64
			|| Definition.SceneBoxes.Num() > MaximumCustomMapSceneBoxes
			|| Definition.Cameras.Num() > MaximumCustomMapCameras
			|| Definition.PourContainers.Num()
				> MaximumCustomMapPourContainers
			|| !FMath::IsFinite(Definition.CellSizeCentimeters)
			|| Definition.CellSizeCentimeters < 1.0f
			|| Definition.CellSizeCentimeters > 1000.0f
			|| !FMath::IsFinite(Definition.MaterialDepthCells)
			|| Definition.MaterialDepthCells < 0.1f
			|| Definition.MaterialDepthCells > 64.0f)
		{
			OutError = FString::Printf(
				TEXT("custom map '%s' contains invalid bounds or counts"), *Id);
			return false;
		}
		const auto IsInside = [&Definition](const FIntPoint Cell)
		{
			return Cell.X >= Definition.MinimumCell.X
				&& Cell.Y >= Definition.MinimumCell.Y
				&& Cell.X < Definition.MaximumCellExclusive.X
				&& Cell.Y < Definition.MaximumCellExclusive.Y;
		};
		for (const FMatterFluxCustomMapStampDefinition& Stamp : Definition.Stamps)
		{
			const bool bValidRectangle =
				Stamp.Shape == EMatterFluxCustomMapStampShape::Rectangle
				&& Stamp.MinimumCell.X <= Stamp.MaximumCellInclusive.X
				&& Stamp.MinimumCell.Y <= Stamp.MaximumCellInclusive.Y
				&& IsInside(Stamp.MinimumCell)
				&& IsInside(Stamp.MaximumCellInclusive);
			const FIntPoint Radius(Stamp.RadiusCells, Stamp.RadiusCells);
			const bool bValidCircle =
				Stamp.Shape == EMatterFluxCustomMapStampShape::Circle
				&& Stamp.RadiusCells >= 1
				&& Stamp.RadiusCells <= 128
				&& IsSafeCoordinate(Stamp.CenterCell)
				&& IsInside(Stamp.CenterCell - Radius)
				&& IsInside(Stamp.CenterCell + Radius);
			if (!IsValidContentId(Stamp.MaterialId.ToString())
				|| (!bValidRectangle && !bValidCircle))
			{
				OutError = FString::Printf(
					TEXT("custom map '%s' contains an invalid stamp"), *Id);
				return false;
			}
		}
		TSet<FName> MarkerIds;
		for (const FMatterFluxCustomMapMarkerDefinition& Marker : Definition.Markers)
		{
			if (!IsValidContentId(Marker.Id.ToString())
				|| MarkerIds.Contains(Marker.Id)
				|| !IsInside(Marker.Cell))
			{
				OutError = FString::Printf(
					TEXT("custom map '%s' contains an invalid marker"), *Id);
				return false;
			}
			MarkerIds.Add(Marker.Id);
		}
		TSet<FName> SpawnMarkerIds;
		for (const FMatterFluxCreatureSpawnDefinition& Spawn
			: Definition.InitialCreatureSpawns)
		{
			if (Spawn.CreatureId.IsNone()
				|| Spawn.MarkerId.IsNone()
				|| !MarkerIds.Contains(Spawn.MarkerId)
				|| SpawnMarkerIds.Contains(Spawn.MarkerId))
			{
				OutError = FString::Printf(
					TEXT("custom map '%s' contains an invalid initial creature spawn"),
					*Id);
				return false;
			}
			SpawnMarkerIds.Add(Spawn.MarkerId);
		}
		const auto IsFiniteVector = [](const FVector& Value)
		{
			return FMath::IsFinite(Value.X)
				&& FMath::IsFinite(Value.Y)
				&& FMath::IsFinite(Value.Z);
		};
		TSet<FName> SceneBoxIds;
		for (const FMatterFluxCustomMapSceneBoxDefinition& Box
			: Definition.SceneBoxes)
		{
			if (!IsValidContentId(Box.Id.ToString())
				|| !IsValidContentId(Box.MaterialId.ToString())
				|| SceneBoxIds.Contains(Box.Id)
				|| !IsFiniteVector(Box.CenterCells)
				|| !IsFiniteVector(Box.SizeCells)
				|| Box.CenterCells.GetAbsMax() > 2048.0
				|| Box.SizeCells.GetMin() <= 0.0
				|| Box.SizeCells.GetAbsMax() > 1024.0)
			{
				OutError = FString::Printf(
					TEXT("custom map '%s' contains an invalid 3D scene box"), *Id);
				return false;
			}
			SceneBoxIds.Add(Box.Id);
		}
		TSet<FName> CameraIds;
		for (const FMatterFluxCustomMapCameraDefinition& Camera
			: Definition.Cameras)
		{
			if (!IsValidContentId(Camera.Id.ToString())
				|| CameraIds.Contains(Camera.Id)
				|| !IsFiniteVector(Camera.LocationCells)
				|| !IsFiniteVector(Camera.TargetCells)
				|| Camera.LocationCells.GetAbsMax() > 4096.0
				|| Camera.TargetCells.GetAbsMax() > 4096.0
				|| Camera.LocationCells.Equals(Camera.TargetCells, 0.01)
				|| !FMath::IsFinite(Camera.FieldOfViewDegrees)
				|| Camera.FieldOfViewDegrees < 20.0f
				|| Camera.FieldOfViewDegrees > 120.0f)
			{
				OutError = FString::Printf(
					TEXT("custom map '%s' contains an invalid 3D camera"), *Id);
				return false;
			}
			CameraIds.Add(Camera.Id);
		}
		TSet<FName> PourContainerIds;
		for (const FMatterFluxCustomMapPourContainerDefinition& Container
			: Definition.PourContainers)
		{
			if (!IsValidContentId(Container.Id.ToString())
				|| !IsValidContentId(Container.ContainerMaterialId.ToString())
				|| !IsValidContentId(Container.LiquidMaterialId.ToString())
				|| PourContainerIds.Contains(Container.Id)
				|| !IsFiniteVector(Container.CenterCells)
				|| Container.CenterCells.GetAbsMax() > 2048.0
				|| Container.InteriorSizeCells.X < 2
				|| Container.InteriorSizeCells.Y < 2
				|| Container.InteriorSizeCells.Z < 2
				|| Container.InteriorSizeCells.GetMax() > 16
				|| Container.StartStep < 0
				|| Container.StartStep > 10000
				|| Container.TiltDurationSteps < 1
				|| Container.TiltDurationSteps > 1000
				|| !FMath::IsFinite(Container.TiltDegrees)
				|| Container.TiltDegrees < 1.0f
				|| Container.TiltDegrees > 89.0f
				|| Container.PourCellsPerStep < 1
				|| Container.PourCellsPerStep > 512)
			{
				OutError = FString::Printf(
					TEXT("custom map '%s' contains an invalid tilting container"),
					*Id);
				return false;
			}
			PourContainerIds.Add(Container.Id);
		}
		if (Registry.CustomMaps.Contains(Definition.Id))
		{
			OutError = FString::Printf(TEXT("duplicate custom map id '%s'"), *Id);
			return false;
		}
		Registry.CustomMaps.Add(Definition.Id, Definition);
		return true;
	}

	bool FRegistryBuilder::AddStructure(
		const FMatterFluxStructureDefinition& Definition,
		FString& OutError)
	{
		const FString Id = Definition.Id.ToString();
		const FString GeneratorId = Definition.GeneratorId.ToString();
		if (!IsValidContentId(Id)
			|| !IsValidContentId(GeneratorId)
			|| GeneratorId != TEXT("two_storey_house")
			|| !FMath::IsFinite(Definition.ContactToleranceCentimeters)
			|| Definition.ContactToleranceCentimeters < 0.0f
			|| Definition.ContactToleranceCentimeters > 100.0f
			|| !FMath::IsFinite(Definition.FloorSnapHeightCentimeters)
			|| Definition.FloorSnapHeightCentimeters < 0.0f
			|| Definition.FloorSnapHeightCentimeters > 200.0f
			|| !FMath::IsFinite(Definition.PreferredFloorPaddingCentimeters)
			|| Definition.PreferredFloorPaddingCentimeters < 0.0f
			|| Definition.PreferredFloorPaddingCentimeters > 500.0f
			|| !FMath::IsFinite(Definition.PreferredFloorVerticalRangeCentimeters)
			|| Definition.PreferredFloorVerticalRangeCentimeters < 0.0f
			|| Definition.PreferredFloorVerticalRangeCentimeters > 1000.0f
			|| !FMath::IsFinite(Definition.ExitGraceSeconds)
			|| Definition.ExitGraceSeconds < 0.0f
			|| Definition.ExitGraceSeconds > 2.0f
			|| !FMath::IsFinite(Definition.FadeSpeed)
			|| Definition.FadeSpeed <= 0.0f
			|| Definition.FadeSpeed > 100.0f
			|| !FMath::IsFinite(Definition.WallGhostOpacity)
			|| Definition.WallGhostOpacity < 0.0f
			|| Definition.WallGhostOpacity > 1.0f
			|| !FMath::IsFinite(Definition.RoofGhostOpacity)
			|| Definition.RoofGhostOpacity < 0.0f
			|| Definition.RoofGhostOpacity > 1.0f)
		{
			OutError = FString::Printf(
				TEXT("structure '%s' contains invalid generator or cutaway data"),
				*Id);
			return false;
		}
		if (Registry.Structures.Contains(Definition.Id))
		{
			OutError = FString::Printf(
				TEXT("duplicate structure id '%s'"), *Id);
			return false;
		}
		Registry.Structures.Add(Definition.Id, Definition);
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
		for (const TPair<FName, FMatterFluxCustomMapDefinition>& Pair
			: Registry.CustomMaps)
		{
			for (const FMatterFluxCreatureSpawnDefinition& Spawn
				: Pair.Value.InitialCreatureSpawns)
			{
				if (!Registry.Creatures.Contains(Spawn.CreatureId))
				{
					OutError = FString::Printf(
						TEXT("custom map '%s' references missing creature '%s'"),
						*Pair.Key.ToString(), *Spawn.CreatureId.ToString());
					return false;
				}
			}
			for (const FMatterFluxCustomMapStampDefinition& Stamp
				: Pair.Value.Stamps)
			{
				if (!Registry.Materials.Contains(Stamp.MaterialId))
				{
					OutError = FString::Printf(
						TEXT("custom map '%s' references missing material '%s'"),
						*Pair.Key.ToString(),
						*Stamp.MaterialId.ToString());
					return false;
				}
			}
			for (const FMatterFluxCustomMapSceneBoxDefinition& Box
				: Pair.Value.SceneBoxes)
			{
				if (!Registry.Materials.Contains(Box.MaterialId))
				{
					OutError = FString::Printf(
						TEXT("custom map '%s' scene box references missing material '%s'"),
						*Pair.Key.ToString(),
						*Box.MaterialId.ToString());
					return false;
				}
			}
			for (const FMatterFluxCustomMapPourContainerDefinition& Container
				: Pair.Value.PourContainers)
			{
				if (!Registry.Materials.Contains(Container.ContainerMaterialId)
					|| !Registry.Materials.Contains(Container.LiquidMaterialId))
				{
					OutError = FString::Printf(
						TEXT("custom map '%s' tilting container references a missing material"),
						*Pair.Key.ToString());
					return false;
				}
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
				|| !IsKnownOutput(Pair.Value.OutputB)
				|| (Pair.Value.Kind
						== FMatterFluxReactionDefinition::EKind::Propagating
					&& Pair.Value.EmissionMaterial != TEXT("empty")
					&& !Registry.Materials.Contains(
						Pair.Value.EmissionMaterial)))
			{
				OutError = FString::Printf(
					TEXT("reaction '%s' references a missing material"),
					*Pair.Key.ToString());
				return false;
			}
		}
		TSet<int32> StarterEquipmentSlots;
		for (const TPair<FName, FMatterFluxSpellDefinition>& Pair
			: Registry.Spells)
		{
			FName MissingMaterial = NAME_None;
			if (!Pair.Value.BodyMaterial.IsNone()
				&& !Registry.Materials.Contains(Pair.Value.BodyMaterial))
			{
				MissingMaterial = Pair.Value.BodyMaterial;
			}
			if (!MissingMaterial.IsNone())
			{
				OutError = FString::Printf(
					TEXT("spell '%s' references missing projectile material '%s'"),
					*Pair.Key.ToString(),
					*MissingMaterial.ToString());
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
		for (const TPair<FName, FMatterFluxShopDefinition>& Pair
			: Registry.Shops)
		{
			for (const FMatterFluxShopOfferDefinition& Offer
				: Pair.Value.Offers)
			{
				const bool bProductExists =
					(Offer.ProductKind == EMatterFluxShopProductKind::Item
						&& Registry.Items.Contains(Offer.ProductId))
					|| (Offer.ProductKind == EMatterFluxShopProductKind::Spell
						&& Registry.Spells.Contains(Offer.ProductId))
					|| (Offer.ProductKind == EMatterFluxShopProductKind::Wand
						&& Registry.Wands.Contains(Offer.ProductId));
				if (!bProductExists
					|| !Registry.Items.Contains(Offer.CostItemId))
				{
					OutError = FString::Printf(
						TEXT("shop '%s' references missing product or currency"),
						*Pair.Key.ToString());
					return false;
				}
			}
		}
		for (const TPair<FName, FMatterFluxDialogueDefinition>& Pair
			: Registry.Dialogues)
		{
			TSet<FName> NodeIds;
			for (const FMatterFluxDialogueNodeDefinition& Node
				: Pair.Value.Nodes)
			{
				NodeIds.Add(Node.Id);
			}
			const auto ValidateDestination =
				[this, &NodeIds, &OutError, &Pair](
					const FName NextNodeId,
					const FName ShopId)
				{
					if ((!NextNodeId.IsNone() && !NodeIds.Contains(NextNodeId))
						|| (!ShopId.IsNone() && !Registry.Shops.Contains(ShopId)))
					{
						OutError = FString::Printf(
							TEXT("dialogue '%s' references a missing node or shop"),
							*Pair.Key.ToString());
						return false;
					}
					return true;
				};
			for (const FMatterFluxDialogueNodeDefinition& Node
				: Pair.Value.Nodes)
			{
				if (!ValidateDestination(Node.NextNodeId, Node.ShopId))
				{
					return false;
				}
				for (const FMatterFluxDialogueOptionDefinition& Option
					: Node.Options)
				{
					if (!ValidateDestination(
						Option.NextNodeId, Option.ShopId))
					{
						return false;
					}
				}
			}
		}
		for (const TPair<FName, FMatterFluxCreatureDefinition>& Pair
			: Registry.Creatures)
		{
			const FMatterFluxCreatureDefinition& Creature = Pair.Value;
			if ((!Creature.AttackProgram.SpellId.IsNone()
					&& !Registry.Spells.Contains(Creature.AttackProgram.SpellId))
				|| (!Creature.SkillProgram.SpellId.IsNone()
					&& !Registry.Spells.Contains(Creature.SkillProgram.SpellId))
				|| (!Creature.DialogueId.IsNone()
					&& !Registry.Dialogues.Contains(Creature.DialogueId))
				|| (!Creature.ShopId.IsNone()
					&& !Registry.Shops.Contains(Creature.ShopId))
				|| (!Creature.DropItemId.IsNone()
					&& !Registry.Items.Contains(Creature.DropItemId)))
			{
				OutError = FString::Printf(
					TEXT("creature '%s' references missing content"),
					*Pair.Key.ToString());
				return false;
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
					&& (Registry.Entities.Contains(Quest.TargetId)
						|| Registry.Creatures.Contains(Quest.TargetId)))
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
			for (const FMatterFluxCreatureSpawnDefinition& Spawn
				: Quest.ActivationCreatureSpawns)
			{
				if (!Registry.Creatures.Contains(Spawn.CreatureId))
				{
					OutError = FString::Printf(
						TEXT("quest '%s' spawn references missing creature '%s'"),
						*Pair.Key.ToString(), *Spawn.CreatureId.ToString());
					return false;
				}
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

	template<typename NumberType>
	static bool ReadRequiredTableNumberField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		NumberType& OutValue,
		FString& OutError)
	{
		lua_getfield(State, TableIndex, Field);
		if (lua_type(State, -1) != LUA_TNUMBER)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' must be a number"), UTF8_TO_TCHAR(Field));
			return false;
		}
		const double Value = lua_tonumber(State, -1);
		lua_pop(State, 1);
		if (!FMath::IsFinite(Value))
		{
			OutError = FString::Printf(
				TEXT("field '%s' must be finite"), UTF8_TO_TCHAR(Field));
			return false;
		}
		OutValue = static_cast<NumberType>(Value);
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

	static bool ReadRequiredTableIntegerField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		int32& OutValue,
		FString& OutError)
	{
		lua_getfield(State, TableIndex, Field);
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

	static bool ReadTableCreatureSpawnArrayField(
		lua_State* State,
		const int32 TableIndex,
		const char* Field,
		TArray<FMatterFluxCreatureSpawnDefinition>& OutValues,
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
				TEXT("field '%s' must be an array of creature spawns"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		const int32 SpawnArrayIndex = lua_absindex(State, -1);
		const size_t Count = lua_rawlen(State, SpawnArrayIndex);
		if (Count > 64)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("field '%s' may contain at most 64 creature spawns"),
				UTF8_TO_TCHAR(Field));
			return false;
		}
		OutValues.Reserve(static_cast<int32>(Count));
		for (size_t Index = 1; Index <= Count; ++Index)
		{
			lua_rawgeti(State, SpawnArrayIndex, static_cast<lua_Integer>(Index));
			if (lua_type(State, -1) != LUA_TTABLE)
			{
				lua_pop(State, 2);
				OutError = FString::Printf(
					TEXT("field '%s' creature spawn %d must be a table"),
					UTF8_TO_TCHAR(Field), static_cast<int32>(Index));
				return false;
			}
			const int32 SpawnIndex = lua_absindex(State, -1);
			FString CreatureId;
			FString MarkerId;
			if (!ReadTableStringField(
					State, SpawnIndex, "creature_id", CreatureId, true, OutError)
				|| !ReadTableStringField(
					State, SpawnIndex, "marker_id", MarkerId, true, OutError)
				|| !IsValidContentId(CreatureId)
				|| !IsValidContentId(MarkerId))
			{
				lua_pop(State, 2);
				OutError = FString::Printf(
					TEXT("field '%s' creature spawn %d is invalid: %s"),
					UTF8_TO_TCHAR(Field), static_cast<int32>(Index), *OutError);
				return false;
			}
			FMatterFluxCreatureSpawnDefinition Spawn;
			Spawn.CreatureId = FName(*CreatureId);
			Spawn.MarkerId = FName(*MarkerId);
			OutValues.Add(MoveTemp(Spawn));
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
		const int32 ArgumentCount = lua_gettop(State);
		FMatterFluxMaterialDefinition Definition;
		int32 Mobility = 255;
		int32 Dispersion = 128;
		int32 LifetimeSteps = 0;
		if (ArgumentCount == 1 && lua_istable(State, 1))
		{
			// Lua 作者只面对命名字段。C++ 在这一条深模块接口后完成
			// 默认值、类型、范围和阶段解析，避免位置参数继续膨胀。
			const int32 TableIndex = lua_absindex(State, 1);
			FString Id;
			FString Phase = TEXT("static");
			float Density = std::numeric_limits<float>::quiet_NaN();
			float Hardness = std::numeric_limits<float>::quiet_NaN();
			float ColorR = std::numeric_limits<float>::quiet_NaN();
			float ColorG = std::numeric_limits<float>::quiet_NaN();
			float ColorB = std::numeric_limits<float>::quiet_NaN();
			float ColorA = std::numeric_limits<float>::quiet_NaN();
			if (!ReadTableStringField(
					State, TableIndex, "id", Id, true, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "density", Density, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "hardness", Hardness, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "color_r", ColorR, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "color_g", ColorG, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "color_b", ColorB, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "color_a", ColorA, Error)
				|| !ReadTableStringField(
					State, TableIndex, "phase", Phase, false, Error)
				|| !ReadTableIntegerField(
					State, TableIndex, "mobility", Mobility, Error)
				|| !ReadTableIntegerField(
					State, TableIndex, "dispersion", Dispersion, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "movement_resistance",
					Definition.MovementResistance, Error)
				|| !ReadTableIntegerField(
					State, TableIndex, "lifetime_steps", LifetimeSteps, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "shallow_opacity",
					Definition.ShallowOpacity, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "deep_opacity",
					Definition.DeepOpacity, Error)
				|| !ReadTableNumberField(
					State, TableIndex, "opacity_depth",
					Definition.OpacityDepth, Error))
			{
				return FailLuaCall(State, Error);
			}
			Definition.Id = FName(*Id);
			Definition.Density = Density;
			Definition.Hardness = Hardness;
			Definition.Color = FLinearColor(ColorR, ColorG, ColorB, ColorA);
			if (Phase == TEXT("static"))
			{
				Definition.Phase = EMatterFluxMaterialPhase::StaticSolid;
			}
			else if (Phase == TEXT("powder"))
			{
				Definition.Phase = EMatterFluxMaterialPhase::Powder;
			}
			else if (Phase == TEXT("liquid"))
			{
				Definition.Phase = EMatterFluxMaterialPhase::Liquid;
			}
			else if (Phase == TEXT("gas"))
			{
				Definition.Phase = EMatterFluxMaterialPhase::Gas;
			}
			else
			{
				return FailLuaCall(
					State,
					TEXT("material phase must be static, powder, liquid, or gas"));
			}
		}
		else
		{
			FString Id;
			double Values[6] = {};
			if ((ArgumentCount != 7 && ArgumentCount != 10)
				|| !ReadContentId(State, 1, Id, Error))
			{
				return FailLuaCall(State, Error.IsEmpty()
					? TEXT("register_material expects one table, 7 arguments, or 10 arguments")
					: Error);
			}
			for (int32 Index = 0; Index < 6; ++Index)
			{
				if (!ReadNumber(State, Index + 2, Values[Index], Error))
				{
					return FailLuaCall(State, Error);
				}
			}
			Definition.Id = FName(*Id);
			Definition.Density = static_cast<float>(Values[0]);
			Definition.Hardness = static_cast<float>(Values[1]);
			Definition.Color = FLinearColor(
				static_cast<float>(Values[2]),
				static_cast<float>(Values[3]),
				static_cast<float>(Values[4]),
				static_cast<float>(Values[5]));
			if (ArgumentCount == 10
				&& (!ReadMaterialPhase(State, 8, Definition.Phase, Error)
					|| !ReadInteger(State, 9, Mobility, Error)
					|| !ReadInteger(State, 10, Dispersion, Error)))
			{
				return FailLuaCall(State, Error);
			}
		}
		if (Mobility < 0
			|| Mobility > 255
			|| Dispersion < 0
			|| Dispersion > 255
			|| LifetimeSteps < 0
			|| LifetimeSteps > 255)
		{
			return FailLuaCall(
				State,
				TEXT("material mobility, dispersion, and lifetime_steps must be between 0 and 255"));
		}
		Definition.Mobility = static_cast<uint8>(Mobility);
		Definition.Dispersion = static_cast<uint8>(Dispersion);
		Definition.LifetimeSteps = static_cast<uint8>(LifetimeSteps);
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
		FMatterFluxReactionDefinition Definition;
		if (lua_gettop(State) == 1 && lua_istable(State, 1))
		{
			const int32 TableIndex = lua_absindex(State, 1);
			const auto ReadIdField = [State, TableIndex, &Error](
				const char* Field, FString& OutValue)
			{
				lua_getfield(State, TableIndex, Field);
				const bool bRead = ReadContentId(State, -1, OutValue, Error);
				lua_pop(State, 1);
				return bRead;
			};
			const auto ReadIntegerField = [State, TableIndex, &Error](
				const char* Field, int32& OutValue)
			{
				lua_getfield(State, TableIndex, Field);
				const bool bRead = ReadInteger(State, -1, OutValue, Error);
				lua_pop(State, 1);
				return bRead;
			};

			FString Id;
			FString Kind;
			FString InputA;
			FString InputB;
			FString OutputA;
			FString OutputB;
			FString Emission;
			if (!ReadIdField("id", Id)
				|| !ReadIdField("kind", Kind)
				|| !ReadIdField("input_a", InputA)
				|| !ReadIdField("input_b", InputB)
				|| !ReadIdField("output_a", OutputA)
				|| !ReadIdField("output_b", OutputB)
				|| !ReadIntegerField(
					"chance_permille", Definition.ChancePermille))
			{
				return FailLuaCall(State, Error);
			}
			Definition.Id = FName(*Id);
			Definition.InputA = FName(*InputA);
			Definition.InputB = FName(*InputB);
			Definition.OutputA = FName(*OutputA);
			Definition.OutputB = FName(*OutputB);
			if (Kind == TEXT("contact"))
			{
				Definition.Kind = FMatterFluxReactionDefinition::EKind::Contact;
			}
			else if (Kind == TEXT("propagating"))
			{
				Definition.Kind = FMatterFluxReactionDefinition::EKind::Propagating;
				if (!ReadIdField("emission_material", Emission)
					|| !ReadIntegerField("propagation_permille",
						Definition.PropagationChancePermille)
					|| !ReadIntegerField("duration_steps", Definition.DurationSteps)
					|| !ReadIntegerField("emission_permille",
						Definition.EmissionChancePermille))
				{
					return FailLuaCall(State, Error);
				}
				Definition.EmissionMaterial = FName(*Emission);
			}
			else
			{
				return FailLuaCall(State,
					TEXT("reaction kind must be 'contact' or 'propagating'"));
			}
		}
		else
		{
			FString Id;
			FString InputA;
			FString InputB;
			FString OutputA;
			FString OutputB;
			if (lua_gettop(State) != 6
				|| !ReadContentId(State, 1, Id, Error)
				|| !ReadContentId(State, 2, InputA, Error)
				|| !ReadContentId(State, 3, InputB, Error)
				|| !ReadContentId(State, 4, OutputA, Error)
				|| !ReadContentId(State, 5, OutputB, Error)
				|| !ReadInteger(State, 6, Definition.ChancePermille, Error))
			{
				return FailLuaCall(State, Error.IsEmpty()
					? TEXT("register_reaction expects a definition table or 6 arguments")
					: Error);
			}
			Definition.Id = FName(*Id);
			Definition.InputA = FName(*InputA);
			Definition.InputB = FName(*InputB);
			Definition.OutputA = FName(*OutputA);
			Definition.OutputB = FName(*OutputB);
		}
		if (!GetExecutionContext(State).Builder->AddReaction(Definition, Error))
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

	static bool ParseCreatureFaction(
		const FString& Value,
		EMatterFluxCreatureFaction& OutFaction,
		FString& OutError)
	{
		if (Value == TEXT("friendly"))
		{
			OutFaction = EMatterFluxCreatureFaction::Friendly;
		}
		else if (Value == TEXT("hostile"))
		{
			OutFaction = EMatterFluxCreatureFaction::Hostile;
		}
		else if (Value == TEXT("neutral"))
		{
			OutFaction = EMatterFluxCreatureFaction::Neutral;
		}
		else
		{
			OutError = TEXT("creature faction must be friendly, hostile, or neutral");
			return false;
		}
		return true;
	}

	static bool ParseCreatureLevel(
		const FString& Value,
		EMatterFluxCreatureLevel& OutLevel,
		FString& OutError)
	{
		if (Value == TEXT("normal")) OutLevel = EMatterFluxCreatureLevel::Normal;
		else if (Value == TEXT("elite")) OutLevel = EMatterFluxCreatureLevel::Elite;
		else if (Value == TEXT("boss")) OutLevel = EMatterFluxCreatureLevel::Boss;
		else
		{
			OutError = TEXT("creature level must be normal, elite, or boss");
			return false;
		}
		return true;
	}

	static bool ParseCreatureAiMode(
		const FString& Value,
		EMatterFluxCreatureAiMode& OutMode,
		FString& OutError)
	{
		if (Value == TEXT("passive")) OutMode = EMatterFluxCreatureAiMode::Passive;
		else if (Value == TEXT("patrol")) OutMode = EMatterFluxCreatureAiMode::Patrol;
		else if (Value == TEXT("skirmisher")) OutMode = EMatterFluxCreatureAiMode::Skirmisher;
		else if (Value == TEXT("boss")) OutMode = EMatterFluxCreatureAiMode::Boss;
		else if (Value == TEXT("behavior_tree")) OutMode = EMatterFluxCreatureAiMode::BehaviorTree;
		else
		{
			OutError = TEXT("creature ai must be passive, patrol, skirmisher, boss, or behavior_tree");
			return false;
		}
		return true;
	}

	static bool ParseCreatureBehaviorCondition(
		const FString& Name,
		EMatterFluxCreatureBehaviorCondition& OutCondition,
		FString& OutError)
	{
		if (Name == TEXT("has_visible_target"))
			OutCondition = EMatterFluxCreatureBehaviorCondition::HasVisibleTarget;
		else if (Name == TEXT("has_target"))
			OutCondition = EMatterFluxCreatureBehaviorCondition::HasTarget;
		else if (Name == TEXT("target_too_close"))
			OutCondition = EMatterFluxCreatureBehaviorCondition::TargetTooClose;
		else if (Name == TEXT("target_in_attack_range"))
			OutCondition = EMatterFluxCreatureBehaviorCondition::TargetInAttackRange;
		else if (Name == TEXT("attack_ready"))
			OutCondition = EMatterFluxCreatureBehaviorCondition::AttackReady;
		else if (Name == TEXT("skill_ready"))
			OutCondition = EMatterFluxCreatureBehaviorCondition::SkillReady;
		else
		{
			OutError = FString::Printf(
				TEXT("unknown creature behavior condition '%s'"), *Name);
			return false;
		}
		return true;
	}

	static bool ParseCreatureBehaviorAction(
		const FString& Name,
		EMatterFluxCreatureBehaviorAction& OutAction,
		FString& OutError)
	{
		if (Name == TEXT("passive"))
			OutAction = EMatterFluxCreatureBehaviorAction::Passive;
		else if (Name == TEXT("patrol"))
			OutAction = EMatterFluxCreatureBehaviorAction::Patrol;
		else if (Name == TEXT("chase"))
			OutAction = EMatterFluxCreatureBehaviorAction::Chase;
		else if (Name == TEXT("retreat"))
			OutAction = EMatterFluxCreatureBehaviorAction::Retreat;
		else if (Name == TEXT("attack"))
			OutAction = EMatterFluxCreatureBehaviorAction::Attack;
		else if (Name == TEXT("skill"))
			OutAction = EMatterFluxCreatureBehaviorAction::Skill;
		else
		{
			OutError = FString::Printf(
				TEXT("unknown creature behavior action '%s'"), *Name);
			return false;
		}
		return true;
	}

	static bool ReadCreatureBehaviorNode(
		lua_State* State,
		const int32 TableIndex,
		const int32 Depth,
		FMatterFluxCreatureBehaviorProgramDefinition& OutProgram,
		TSet<const void*>& ActiveTables,
		int32& OutNodeIndex,
		FString& OutError)
	{
		const int32 AbsoluteIndex = lua_absindex(State, TableIndex);
		if (lua_type(State, AbsoluteIndex) != LUA_TTABLE)
		{
			OutError = TEXT("creature behavior node must be a table");
			return false;
		}
		if (Depth > MaximumCreatureBehaviorDepth
			|| OutProgram.Nodes.Num() >= MaximumCreatureBehaviorNodes)
		{
			OutError = TEXT("creature behavior tree exceeds its node or depth budget");
			return false;
		}
		const void* TableIdentity = lua_topointer(State, AbsoluteIndex);
		if (!TableIdentity || ActiveTables.Contains(TableIdentity))
		{
			OutError = TEXT("creature behavior tree contains a table cycle");
			return false;
		}
		ActiveTables.Add(TableIdentity);
		ON_SCOPE_EXIT
		{
			ActiveTables.Remove(TableIdentity);
		};

		FString Kind;
		if (!ReadTableStringField(
			State, AbsoluteIndex, "kind", Kind, true, OutError))
		{
			return false;
		}
		if (Kind == TEXT("condition") || Kind == TEXT("action"))
		{
			FString Name;
			if (!ReadTableStringField(
				State, AbsoluteIndex, "name", Name, true, OutError))
			{
				return false;
			}
			const bool bCondition = Kind == TEXT("condition");
			OutNodeIndex = AddCreatureBehaviorNode(
				OutProgram,
				bCondition
					? EMatterFluxCreatureBehaviorNodeKind::Condition
					: EMatterFluxCreatureBehaviorNodeKind::Action);
			return bCondition
				? ParseCreatureBehaviorCondition(
					Name,
					OutProgram.Nodes[OutNodeIndex].Condition,
					OutError)
				: ParseCreatureBehaviorAction(
					Name,
					OutProgram.Nodes[OutNodeIndex].Action,
					OutError);
		}

		EMatterFluxCreatureBehaviorNodeKind NodeKind;
		if (Kind == TEXT("selector"))
		{
			NodeKind = EMatterFluxCreatureBehaviorNodeKind::Selector;
		}
		else if (Kind == TEXT("sequence"))
		{
			NodeKind = EMatterFluxCreatureBehaviorNodeKind::Sequence;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("unknown creature behavior node kind '%s'"), *Kind);
			return false;
		}

		lua_getfield(State, AbsoluteIndex, "children");
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			OutError = TEXT("creature behavior composite children must be an array");
			return false;
		}
		const int32 ChildrenIndex = lua_absindex(State, -1);
		const size_t ChildCount = lua_rawlen(State, ChildrenIndex);
		if (ChildCount == 0
			|| ChildCount > MaximumCreatureBehaviorChildren)
		{
			lua_pop(State, 1);
			OutError = TEXT("creature behavior composite has an invalid child count");
			return false;
		}

		OutNodeIndex = AddCreatureBehaviorNode(OutProgram, NodeKind);
		TArray<int32> ChildIndices;
		ChildIndices.Reserve(static_cast<int32>(ChildCount));
		for (size_t ChildOffset = 1; ChildOffset <= ChildCount; ++ChildOffset)
		{
			lua_rawgeti(
				State, ChildrenIndex, static_cast<lua_Integer>(ChildOffset));
			int32 ChildNodeIndex = INDEX_NONE;
			if (!ReadCreatureBehaviorNode(
				State,
				-1,
				Depth + 1,
				OutProgram,
				ActiveTables,
				ChildNodeIndex,
				OutError))
			{
				lua_pop(State, 2);
				return false;
			}
			ChildIndices.Add(ChildNodeIndex);
			lua_pop(State, 1);
		}
		lua_pop(State, 1);
		OutProgram.Nodes[OutNodeIndex].Children = MoveTemp(ChildIndices);
		return true;
	}

	static bool ReadCreatureBehaviorProgram(
		lua_State* State,
		const int32 DefinitionIndex,
		const EMatterFluxCreatureAiMode Mode,
		FMatterFluxCreatureBehaviorProgramDefinition& OutProgram,
		FString& OutError)
	{
		OutProgram = {};
		lua_getfield(State, DefinitionIndex, "behavior_tree");
		if (lua_type(State, -1) == LUA_TNIL)
		{
			lua_pop(State, 1);
			if (Mode == EMatterFluxCreatureAiMode::BehaviorTree)
			{
				OutError = TEXT("behavior_tree AI requires a behavior_tree root");
				return false;
			}
			BuildLegacyCreatureBehaviorProgram(Mode, OutProgram);
			return true;
		}
		if (Mode != EMatterFluxCreatureAiMode::BehaviorTree)
		{
			lua_pop(State, 1);
			OutError = TEXT("behavior_tree field requires ai='behavior_tree'");
			return false;
		}
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			OutError = TEXT("behavior_tree root must be a table");
			return false;
		}
		TSet<const void*> ActiveTables;
		int32 RootNodeIndex = INDEX_NONE;
		const bool bRead = ReadCreatureBehaviorNode(
			State,
			-1,
			0,
			OutProgram,
			ActiveTables,
			RootNodeIndex,
			OutError);
		lua_pop(State, 1);
		if (!bRead)
		{
			return false;
		}
		OutProgram.RootNodeIndex = RootNodeIndex;
		return ValidateCreatureBehaviorProgram(OutProgram, OutError);
	}

	static bool ParseShopProductKind(
		const FString& Value,
		EMatterFluxShopProductKind& OutKind,
		FString& OutError)
	{
		if (Value == TEXT("item")) OutKind = EMatterFluxShopProductKind::Item;
		else if (Value == TEXT("spell")) OutKind = EMatterFluxShopProductKind::Spell;
		else if (Value == TEXT("wand")) OutKind = EMatterFluxShopProductKind::Wand;
		else
		{
			OutError = TEXT("shop product kind must be item, spell, or wand");
			return false;
		}
		return true;
	}

	static bool ReadDialogueOptions(
		lua_State* State,
		const int32 NodeIndex,
		TArray<FMatterFluxDialogueOptionDefinition>& OutOptions,
		FString& OutError)
	{
		OutOptions.Reset();
		lua_getfield(State, NodeIndex, "options");
		if (lua_type(State, -1) == LUA_TNIL)
		{
			lua_pop(State, 1);
			return true;
		}
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			OutError = TEXT("dialogue options must be an array");
			return false;
		}
		const int32 OptionsIndex = lua_absindex(State, -1);
		const size_t Count = lua_rawlen(State, OptionsIndex);
		if (Count > 8)
		{
			lua_pop(State, 1);
			OutError = TEXT("a dialogue node may contain at most 8 options");
			return false;
		}
		for (size_t Index = 1; Index <= Count; ++Index)
		{
			lua_rawgeti(State, OptionsIndex, static_cast<lua_Integer>(Index));
			if (lua_type(State, -1) != LUA_TTABLE)
			{
				lua_pop(State, 2);
				OutError = TEXT("dialogue option must be a table");
				return false;
			}
			const int32 OptionIndex = lua_absindex(State, -1);
			FMatterFluxDialogueOptionDefinition Option;
			FString NextNode;
			FString ShopId;
			if (!ReadTableStringField(State, OptionIndex, "text", Option.Text, true, OutError)
				|| !ReadTableStringField(State, OptionIndex, "next", NextNode, false, OutError)
				|| !ReadTableStringField(State, OptionIndex, "shop_id", ShopId, false, OutError)
				|| !ReadTableBooleanField(State, OptionIndex, "close", Option.bClose, OutError)
				|| (!NextNode.IsEmpty() && !IsValidContentId(NextNode))
				|| (!ShopId.IsEmpty() && !IsValidContentId(ShopId)))
			{
				lua_pop(State, 2);
				return false;
			}
			Option.NextNodeId = NextNode.IsEmpty() ? NAME_None : FName(*NextNode);
			Option.ShopId = ShopId.IsEmpty() ? NAME_None : FName(*ShopId);
			OutOptions.Add(MoveTemp(Option));
			lua_pop(State, 1);
		}
		lua_pop(State, 1);
		return true;
	}

	static bool ReadDialogueNodes(
		lua_State* State,
		const int32 DefinitionIndex,
		TArray<FMatterFluxDialogueNodeDefinition>& OutNodes,
		FString& OutError)
	{
		OutNodes.Reset();
		lua_getfield(State, DefinitionIndex, "nodes");
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			OutError = TEXT("dialogue nodes must be an array");
			return false;
		}
		const int32 NodesIndex = lua_absindex(State, -1);
		const size_t Count = lua_rawlen(State, NodesIndex);
		if (Count == 0 || Count > 64)
		{
			lua_pop(State, 1);
			OutError = TEXT("dialogue must contain between 1 and 64 nodes");
			return false;
		}
		for (size_t Index = 1; Index <= Count; ++Index)
		{
			lua_rawgeti(State, NodesIndex, static_cast<lua_Integer>(Index));
			if (lua_type(State, -1) != LUA_TTABLE)
			{
				lua_pop(State, 2);
				OutError = TEXT("dialogue node must be a table");
				return false;
			}
			const int32 NodeIndex = lua_absindex(State, -1);
			FMatterFluxDialogueNodeDefinition Node;
			FString Id;
			FString NextNode;
			FString ShopId;
			if (!ReadTableStringField(State, NodeIndex, "id", Id, true, OutError)
				|| !ReadTableStringField(State, NodeIndex, "text", Node.Text, true, OutError)
				|| !ReadTableStringField(State, NodeIndex, "next", NextNode, false, OutError)
				|| !ReadTableStringField(State, NodeIndex, "shop_id", ShopId, false, OutError)
				|| !ReadTableBooleanField(State, NodeIndex, "close", Node.bClose, OutError)
				|| !ReadDialogueOptions(State, NodeIndex, Node.Options, OutError)
				|| !IsValidContentId(Id)
				|| (!NextNode.IsEmpty() && !IsValidContentId(NextNode))
				|| (!ShopId.IsEmpty() && !IsValidContentId(ShopId)))
			{
				lua_pop(State, 2);
				return false;
			}
			Node.Id = FName(*Id);
			Node.NextNodeId = NextNode.IsEmpty() ? NAME_None : FName(*NextNode);
			Node.ShopId = ShopId.IsEmpty() ? NAME_None : FName(*ShopId);
			OutNodes.Add(MoveTemp(Node));
			lua_pop(State, 1);
		}
		lua_pop(State, 1);
		return true;
	}

	static bool ReadShopCategories(
		lua_State* State,
		const int32 DefinitionIndex,
		TArray<FMatterFluxShopCategoryDefinition>& OutCategories,
		FString& OutError)
	{
		OutCategories.Reset();
		lua_getfield(State, DefinitionIndex, "categories");
		if (lua_type(State, -1) == LUA_TNIL)
		{
			lua_pop(State, 1);
			return true;
		}
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			OutError = TEXT("shop categories must be an array");
			return false;
		}
		const int32 CategoriesIndex = lua_absindex(State, -1);
		const size_t Count = lua_rawlen(State, CategoriesIndex);
		if (Count > MaximumShopCategories)
		{
			lua_pop(State, 1);
			OutError = FString::Printf(
				TEXT("shop may contain at most %d categories"),
				MaximumShopCategories);
			return false;
		}
		for (size_t Index = 1; Index <= Count; ++Index)
		{
			lua_rawgeti(
				State,
				CategoriesIndex,
				static_cast<lua_Integer>(Index));
			if (lua_type(State, -1) != LUA_TTABLE)
			{
				lua_pop(State, 2);
				OutError = TEXT("shop category must be a table");
				return false;
			}
			const int32 CategoryIndex = lua_absindex(State, -1);
			FString CategoryId;
			FMatterFluxShopCategoryDefinition Category;
			if (!ReadTableStringField(
					State, CategoryIndex, "id", CategoryId, true, OutError)
				|| !ReadTableStringField(
					State,
					CategoryIndex,
					"name",
					Category.DisplayName,
					true,
					OutError)
				|| !IsValidContentId(CategoryId))
			{
				lua_pop(State, 2);
				return false;
			}
			Category.Id = FName(*CategoryId);
			OutCategories.Add(MoveTemp(Category));
			lua_pop(State, 1);
		}
		lua_pop(State, 1);
		return true;
	}

	static bool ReadShopOffers(
		lua_State* State,
		const int32 DefinitionIndex,
		TArray<FMatterFluxShopOfferDefinition>& OutOffers,
		FString& OutError)
	{
		OutOffers.Reset();
		lua_getfield(State, DefinitionIndex, "offers");
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			OutError = TEXT("shop offers must be an array");
			return false;
		}
		const int32 OffersIndex = lua_absindex(State, -1);
		const size_t Count = lua_rawlen(State, OffersIndex);
		if (Count == 0 || Count > 64)
		{
			lua_pop(State, 1);
			OutError = TEXT("shop must contain between 1 and 64 offers");
			return false;
		}
		for (size_t Index = 1; Index <= Count; ++Index)
		{
			lua_rawgeti(State, OffersIndex, static_cast<lua_Integer>(Index));
			if (lua_type(State, -1) != LUA_TTABLE)
			{
				lua_pop(State, 2);
				OutError = TEXT("shop offer must be a table");
				return false;
			}
			const int32 OfferIndex = lua_absindex(State, -1);
			FMatterFluxShopOfferDefinition Offer;
			FString Kind;
			FString ProductId;
			FString CostItemId;
			FString CategoryId;
			if (!ReadTableStringField(State, OfferIndex, "kind", Kind, true, OutError)
				|| !ReadTableStringField(State, OfferIndex, "product_id", ProductId, true, OutError)
				|| !ReadTableIntegerField(State, OfferIndex, "product_count", Offer.ProductCount, OutError)
				|| !ReadTableStringField(State, OfferIndex, "cost_item", CostItemId, true, OutError)
				|| !ReadTableIntegerField(State, OfferIndex, "cost_count", Offer.CostCount, OutError)
				|| !ReadTableIntegerField(State, OfferIndex, "limit", Offer.PurchaseLimit, OutError)
				|| !ReadTableStringField(State, OfferIndex, "category", CategoryId, false, OutError)
				|| !ParseShopProductKind(Kind, Offer.ProductKind, OutError)
				|| !IsValidContentId(ProductId)
				|| !IsValidContentId(CostItemId)
				|| (!CategoryId.IsEmpty() && !IsValidContentId(CategoryId)))
			{
				lua_pop(State, 2);
				return false;
			}
			Offer.ProductId = FName(*ProductId);
			Offer.CostItemId = FName(*CostItemId);
			Offer.CategoryId = CategoryId.IsEmpty()
				? NAME_None
				: FName(*CategoryId);
			OutOffers.Add(MoveTemp(Offer));
			lua_pop(State, 1);
		}
		lua_pop(State, 1);
		return true;
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

	static int32 RegisterCreature(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (!Context.Error.IsEmpty()) return 0;
		FString Error;
		if (lua_gettop(State) != 1 || lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(State, TEXT("register_creature expects one definition table"));
		}
		if (!CheckDefinitionBudget(
			Context.Builder->Registry.Creatures.Num(), TEXT("creature"), Error))
		{
			return FailLuaCall(State, Error);
		}

		FMatterFluxCreatureDefinition Definition;
		FString Id;
		FString Faction = TEXT("neutral");
		FString Level = TEXT("normal");
		FString AiMode = TEXT("passive");
		FString AttackSpell;
		FString SkillSpell;
		FString DialogueId;
		FString ShopId;
		FString DropItemId;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(State, 1, "name", Definition.DisplayName, true, Error)
			|| !ReadTableStringField(State, 1, "faction", Faction, true, Error)
			|| !ReadTableStringField(State, 1, "level", Level, true, Error)
			|| !ReadTableStringField(State, 1, "ai", AiMode, true, Error)
			|| !ReadTableNumberField(State, 1, "health", Definition.MaxHealth, Error)
			|| !ReadTableNumberField(State, 1, "width", Definition.Width, Error)
			|| !ReadTableNumberField(State, 1, "height", Definition.Height, Error)
			|| !ReadTableNumberField(State, 1, "density", Definition.Density, Error)
			|| !ReadTableNumberField(State, 1, "move_speed", Definition.MoveSpeed, Error)
			|| !ReadTableNumberField(State, 1, "perception_range", Definition.PerceptionRange, Error)
			|| !ReadTableNumberField(State, 1, "attack_range", Definition.AttackRange, Error)
			|| !ReadTableNumberField(State, 1, "retreat_range", Definition.RetreatRange, Error)
			|| !ReadTableNumberField(State, 1, "target_memory", Definition.TargetMemorySeconds, Error)
			|| !ReadTableBooleanField(State, 1, "wait_for_first_sight", Definition.bWaitForFirstSight, Error)
			|| !ReadTableNumberField(State, 1, "patrol_turn", Definition.PatrolTurnSeconds, Error)
			|| !ReadTableNumberField(State, 1, "patrol_pause", Definition.PatrolPauseSeconds, Error)
			|| !ReadTableNumberField(State, 1, "attack_cooldown", Definition.AttackCooldown, Error)
			|| !ReadTableNumberField(State, 1, "skill_cooldown", Definition.SkillCooldown, Error)
			|| !ReadTableStringField(State, 1, "attack_spell", AttackSpell, false, Error)
			|| !ReadTableStringField(State, 1, "skill_spell", SkillSpell, false, Error)
			|| !ReadTableIntegerField(State, 1, "attack_projectiles", Definition.AttackProgram.ProjectileCount, Error)
			|| !ReadTableNumberField(State, 1, "attack_spread", Definition.AttackProgram.SpreadDegrees, Error)
			|| !ReadTableNumberField(State, 1, "attack_projectile_interval", Definition.AttackProgram.ProjectileInterval, Error)
			|| !ReadTableNumberField(State, 1, "attack_recovery", Definition.AttackProgram.RecoverySeconds, Error)
			|| !ReadTableBooleanField(State, 1, "attack_radial", Definition.AttackProgram.bRadial, Error)
			|| !ReadTableNumberField(State, 1, "attack_horizontal_impulse", Definition.AttackProgram.HorizontalImpulse, Error)
			|| !ReadTableNumberField(State, 1, "attack_vertical_impulse", Definition.AttackProgram.VerticalImpulse, Error)
			|| !ReadTableBooleanField(State, 1, "attack_override_color", Definition.AttackProgram.bOverrideColor, Error)
			|| !ReadTableNumberField(State, 1, "attack_color_r", Definition.AttackProgram.Color.R, Error)
			|| !ReadTableNumberField(State, 1, "attack_color_g", Definition.AttackProgram.Color.G, Error)
			|| !ReadTableNumberField(State, 1, "attack_color_b", Definition.AttackProgram.Color.B, Error)
			|| !ReadTableNumberField(State, 1, "attack_color_a", Definition.AttackProgram.Color.A, Error)
			|| !ReadTableIntegerField(State, 1, "skill_projectiles", Definition.SkillProgram.ProjectileCount, Error)
			|| !ReadTableNumberField(State, 1, "skill_spread", Definition.SkillProgram.SpreadDegrees, Error)
			|| !ReadTableNumberField(State, 1, "skill_projectile_interval", Definition.SkillProgram.ProjectileInterval, Error)
			|| !ReadTableNumberField(State, 1, "skill_recovery", Definition.SkillProgram.RecoverySeconds, Error)
			|| !ReadTableBooleanField(State, 1, "skill_radial", Definition.SkillProgram.bRadial, Error)
			|| !ReadTableNumberField(State, 1, "skill_horizontal_impulse", Definition.SkillProgram.HorizontalImpulse, Error)
			|| !ReadTableNumberField(State, 1, "skill_vertical_impulse", Definition.SkillProgram.VerticalImpulse, Error)
			|| !ReadTableBooleanField(State, 1, "skill_override_color", Definition.SkillProgram.bOverrideColor, Error)
			|| !ReadTableNumberField(State, 1, "skill_color_r", Definition.SkillProgram.Color.R, Error)
			|| !ReadTableNumberField(State, 1, "skill_color_g", Definition.SkillProgram.Color.G, Error)
			|| !ReadTableNumberField(State, 1, "skill_color_b", Definition.SkillProgram.Color.B, Error)
			|| !ReadTableNumberField(State, 1, "skill_color_a", Definition.SkillProgram.Color.A, Error)
			|| !ReadTableStringField(State, 1, "dialogue_id", DialogueId, false, Error)
			|| !ReadTableStringField(State, 1, "shop_id", ShopId, false, Error)
			|| !ReadTableStringField(State, 1, "drop_item", DropItemId, false, Error)
			|| !ReadTableIntegerField(State, 1, "drop_count", Definition.DropItemCount, Error)
			|| !ReadTableNumberField(State, 1, "color_r", Definition.Color.R, Error)
			|| !ReadTableNumberField(State, 1, "color_g", Definition.Color.G, Error)
			|| !ReadTableNumberField(State, 1, "color_b", Definition.Color.B, Error)
			|| !ReadTableNumberField(State, 1, "color_a", Definition.Color.A, Error)
			|| !ParseCreatureFaction(Faction, Definition.Faction, Error)
			|| !ParseCreatureLevel(Level, Definition.Level, Error)
			|| !ParseCreatureAiMode(AiMode, Definition.AiMode, Error)
			|| !ReadCreatureBehaviorProgram(
				State,
				1,
				Definition.AiMode,
				Definition.BehaviorProgram,
				Error)
			|| !IsValidContentId(Id))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("creature definition contains an invalid id") : Error);
		}
		const auto ParseOptionalId = [&Error](
			const FString& Value, FName& OutValue)
		{
			if (!Value.IsEmpty() && !IsValidContentId(Value))
			{
				Error = TEXT("creature definition contains an invalid content reference");
				return false;
			}
			OutValue = Value.IsEmpty() ? NAME_None : FName(*Value);
			return true;
		};
		Definition.Id = FName(*Id);
		if (!ParseOptionalId(AttackSpell, Definition.AttackProgram.SpellId)
			|| !ParseOptionalId(SkillSpell, Definition.SkillProgram.SpellId)
			|| !ParseOptionalId(DialogueId, Definition.DialogueId)
			|| !ParseOptionalId(ShopId, Definition.ShopId)
			|| !ParseOptionalId(DropItemId, Definition.DropItemId)
			|| !Context.Builder->AddCreature(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 RegisterDialogue(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (!Context.Error.IsEmpty()) return 0;
		FString Error;
		if (lua_gettop(State) != 1 || lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(State, TEXT("register_dialogue expects one definition table"));
		}
		if (!CheckDefinitionBudget(
			Context.Builder->Registry.Dialogues.Num(), TEXT("dialogue"), Error))
		{
			return FailLuaCall(State, Error);
		}
		FMatterFluxDialogueDefinition Definition;
		FString Id;
		FString StartNode;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(State, 1, "name", Definition.DisplayName, true, Error)
			|| !ReadTableStringField(State, 1, "start", StartNode, true, Error)
			|| !ReadDialogueNodes(State, 1, Definition.Nodes, Error)
			|| !IsValidContentId(Id)
			|| !IsValidContentId(StartNode))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("dialogue definition contains an invalid id") : Error);
		}
		Definition.Id = FName(*Id);
		Definition.StartNodeId = FName(*StartNode);
		if (!Context.Builder->AddDialogue(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int32 RegisterShop(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (!Context.Error.IsEmpty()) return 0;
		FString Error;
		if (lua_gettop(State) != 1 || lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(State, TEXT("register_shop expects one definition table"));
		}
		if (!CheckDefinitionBudget(
			Context.Builder->Registry.Shops.Num(), TEXT("shop"), Error))
		{
			return FailLuaCall(State, Error);
		}
		FMatterFluxShopDefinition Definition;
		FString Id;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(State, 1, "name", Definition.DisplayName, true, Error)
			|| !ReadShopCategories(State, 1, Definition.Categories, Error)
			|| !ReadShopOffers(State, 1, Definition.Offers, Error)
			|| !IsValidContentId(Id))
		{
			return FailLuaCall(State, Error.IsEmpty()
				? TEXT("shop definition contains an invalid id") : Error);
		}
		Definition.Id = FName(*Id);
		if (!Context.Builder->AddShop(Definition, Error))
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
		else
		{
			OutError =
				TEXT("field 'kind' must be projectile, modifier, multicast, trigger, trigger_modifier, or jump");
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
		FString BodyMaterial;
		FString VisualShape;
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
				State, 1, "gravity_scale", Definition.GravityScale, Error)
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
				State, 1, "body_material", BodyMaterial, false, Error)
			|| !ReadTableIntegerField(
				State, 1, "material_amount", Definition.MaterialAmount, Error)
			|| !ReadTableNumberField(
				State, 1, "spawn_forward_offset", Definition.SpawnForwardOffset, Error)
			|| !ReadTableNumberField(
				State, 1, "spawn_height_offset", Definition.SpawnHeightOffset, Error)
			|| !ReadTableBooleanField(
				State, 1, "spawn_stationary", Definition.bSpawnStationary, Error)
			|| !ReadTableStringField(
				State, 1, "visual_shape", VisualShape, false, Error)
			|| !ReadTableIntegerField(
				State, 1, "starter_count", Definition.StarterCount, Error)
			|| !ParseSpellKind(Kind, Definition.Kind, Error))
		{
			return FailLuaCall(State, Error);
		}
		if (!IsValidContentId(Id)
			|| (!BodyMaterial.IsEmpty()
				&& !IsValidContentId(BodyMaterial)))
		{
			return FailLuaCall(
				State,
				TEXT("spell id or projectile material is invalid"));
		}
		Definition.Id = FName(*Id);
		Definition.BodyMaterial = BodyMaterial.IsEmpty()
			? NAME_None
			: FName(*BodyMaterial);
		if (VisualShape.IsEmpty() || VisualShape == TEXT("orb"))
		{
			Definition.bUsePlaneVisual = false;
			Definition.bUseVerticalPlaneVisual = false;
		}
		else if (VisualShape == TEXT("plane"))
		{
			Definition.bUsePlaneVisual = true;
			Definition.bUseVerticalPlaneVisual = false;
		}
		else if (VisualShape == TEXT("vertical_plane"))
		{
			Definition.bUsePlaneVisual = true;
			Definition.bUseVerticalPlaneVisual = true;
		}
		else
		{
			return FailLuaCall(
				State,
				TEXT("field 'visual_shape' must be orb, plane, or vertical_plane"));
		}
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
				State, 1, "starter_count", Definition.StarterCount, Error)
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
			|| !ReadTableCreatureSpawnArrayField(
				State, 1, "activation_creature_spawns",
				Definition.ActivationCreatureSpawns, Error)
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

	static int RegisterStructure(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (lua_gettop(State) != 1 || lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(
				State, TEXT("register_structure expects one definition table"));
		}
		if (Context.Builder->Registry.Structures.Num()
			>= MaximumDefinitionsPerCategory)
		{
			return FailLuaCall(State, TEXT("structure definition limit exceeded"));
		}

		FMatterFluxStructureDefinition Definition;
		FString Id;
		FString GeneratorId;
		FString Error;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(
				State, 1, "generator", GeneratorId, true, Error)
			|| !ReadTableNumberField(
				State, 1, "contact_tolerance_cm",
				Definition.ContactToleranceCentimeters, Error)
			|| !ReadTableNumberField(
				State, 1, "floor_snap_height_cm",
				Definition.FloorSnapHeightCentimeters, Error)
			|| !ReadTableNumberField(
				State, 1, "preferred_floor_padding_cm",
				Definition.PreferredFloorPaddingCentimeters, Error)
			|| !ReadTableNumberField(
				State, 1, "preferred_floor_vertical_range_cm",
				Definition.PreferredFloorVerticalRangeCentimeters, Error)
			|| !ReadTableNumberField(
				State, 1, "exit_grace_seconds",
				Definition.ExitGraceSeconds, Error)
			|| !ReadTableNumberField(
				State, 1, "fade_speed", Definition.FadeSpeed, Error)
			|| !ReadTableNumberField(
				State, 1, "wall_opacity", Definition.WallGhostOpacity, Error)
			|| !ReadTableNumberField(
				State, 1, "roof_opacity", Definition.RoofGhostOpacity, Error)
			|| !IsValidContentId(Id)
			|| !IsValidContentId(GeneratorId))
		{
			return FailLuaCall(
				State,
				Error.IsEmpty()
					? TEXT("structure id or generator is invalid") : Error);
		}
		Definition.Id = FName(*Id);
		Definition.GeneratorId = FName(*GeneratorId);
		if (!Context.Builder->AddStructure(Definition, Error))
		{
			return FailLuaCall(State, Error);
		}
		return 0;
	}

	static int RegisterCustomMap(lua_State* State)
	{
		FExecutionContext& Context = GetExecutionContext(State);
		if (lua_gettop(State) != 1 || lua_type(State, 1) != LUA_TTABLE)
		{
			return FailLuaCall(
				State,
				TEXT("register_custom_map expects one compiled definition table"));
		}

		FMatterFluxCustomMapDefinition Definition;
		FString Id;
		FString Error;
		if (!ReadTableStringField(State, 1, "id", Id, true, Error)
			|| !ReadTableStringField(
				State, 1, "name", Definition.DisplayName, true, Error)
			|| !ReadRequiredTableIntegerField(
				State, 1, "min_x", Definition.MinimumCell.X, Error)
			|| !ReadRequiredTableIntegerField(
				State, 1, "min_y", Definition.MinimumCell.Y, Error)
			|| !ReadRequiredTableIntegerField(
				State, 1, "max_x_exclusive", Definition.MaximumCellExclusive.X, Error)
			|| !ReadRequiredTableIntegerField(
				State, 1, "max_y_exclusive", Definition.MaximumCellExclusive.Y, Error)
			|| !ReadTableNumberField(
				State, 1, "cell_size_cm", Definition.CellSizeCentimeters, Error)
			|| !ReadTableNumberField(
				State, 1, "material_depth_cells", Definition.MaterialDepthCells, Error)
			|| !IsValidContentId(Id))
		{
			return FailLuaCall(
				State,
				Error.IsEmpty() ? TEXT("custom map id is invalid") : Error);
		}
		Definition.Id = FName(*Id);
		if (!ReadTableCreatureSpawnArrayField(
			State, 1, "initial_creature_spawns",
			Definition.InitialCreatureSpawns, Error))
		{
			return FailLuaCall(State, Error);
		}

		lua_getfield(State, 1, "stamps");
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			return FailLuaCall(State, TEXT("custom map stamps must be an array"));
		}
		const int32 StampsIndex = lua_absindex(State, -1);
		const size_t StampCount = lua_rawlen(State, StampsIndex);
		if (StampCount > MaximumCustomMapStamps)
		{
			lua_pop(State, 1);
			return FailLuaCall(State, TEXT("custom map stamp count is out of range"));
		}
		Definition.Stamps.Reserve(static_cast<int32>(StampCount));
		for (size_t Index = 1; Index <= StampCount; ++Index)
		{
			lua_rawgeti(State, StampsIndex, static_cast<lua_Integer>(Index));
			const int32 StampIndex = lua_absindex(State, -1);
			if (lua_type(State, StampIndex) != LUA_TTABLE)
			{
				lua_pop(State, 2);
				return FailLuaCall(State, TEXT("custom map stamp must be a table"));
			}
			FMatterFluxCustomMapStampDefinition Stamp;
			FString Shape;
			FString MaterialId;
			if (!ReadTableStringField(State, StampIndex, "shape", Shape, true, Error)
				|| !ReadTableStringField(
					State, StampIndex, "material", MaterialId, true, Error)
				|| !IsValidContentId(MaterialId))
			{
				lua_pop(State, 2);
				return FailLuaCall(
					State,
					Error.IsEmpty() ? TEXT("custom map stamp material is invalid") : Error);
			}
			Stamp.MaterialId = FName(*MaterialId);
			if (Shape == TEXT("rectangle"))
			{
				Stamp.Shape = EMatterFluxCustomMapStampShape::Rectangle;
				if (!ReadRequiredTableIntegerField(
						State, StampIndex, "min_x", Stamp.MinimumCell.X, Error)
					|| !ReadRequiredTableIntegerField(
						State, StampIndex, "min_y", Stamp.MinimumCell.Y, Error)
					|| !ReadRequiredTableIntegerField(
						State, StampIndex, "max_x", Stamp.MaximumCellInclusive.X, Error)
					|| !ReadRequiredTableIntegerField(
						State, StampIndex, "max_y", Stamp.MaximumCellInclusive.Y, Error))
				{
					lua_pop(State, 2);
					return FailLuaCall(State, Error);
				}
			}
			else if (Shape == TEXT("circle"))
			{
				Stamp.Shape = EMatterFluxCustomMapStampShape::Circle;
				if (!ReadRequiredTableIntegerField(
						State, StampIndex, "center_x", Stamp.CenterCell.X, Error)
					|| !ReadRequiredTableIntegerField(
						State, StampIndex, "center_y", Stamp.CenterCell.Y, Error)
					|| !ReadRequiredTableIntegerField(
						State, StampIndex, "radius", Stamp.RadiusCells, Error))
				{
					lua_pop(State, 2);
					return FailLuaCall(State, Error);
				}
			}
			else
			{
				lua_pop(State, 2);
				return FailLuaCall(
					State,
					TEXT("custom map stamp shape must be rectangle or circle"));
			}
			Definition.Stamps.Add(Stamp);
			lua_pop(State, 1);
		}
		lua_pop(State, 1);

		lua_getfield(State, 1, "markers");
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			return FailLuaCall(State, TEXT("custom map markers must be an array"));
		}
		const int32 MarkersIndex = lua_absindex(State, -1);
		const size_t MarkerCount = lua_rawlen(State, MarkersIndex);
		if (MarkerCount > MaximumCustomMapMarkers)
		{
			lua_pop(State, 1);
			return FailLuaCall(State, TEXT("custom map marker count is out of range"));
		}
		Definition.Markers.Reserve(static_cast<int32>(MarkerCount));
		for (size_t Index = 1; Index <= MarkerCount; ++Index)
		{
			lua_rawgeti(State, MarkersIndex, static_cast<lua_Integer>(Index));
			const int32 MarkerIndex = lua_absindex(State, -1);
			FMatterFluxCustomMapMarkerDefinition Marker;
			FString MarkerId;
			if (lua_type(State, MarkerIndex) != LUA_TTABLE
				|| !ReadTableStringField(
					State, MarkerIndex, "id", MarkerId, true, Error)
				|| !IsValidContentId(MarkerId)
				|| !ReadRequiredTableIntegerField(
					State, MarkerIndex, "x", Marker.Cell.X, Error)
				|| !ReadRequiredTableIntegerField(
					State, MarkerIndex, "y", Marker.Cell.Y, Error))
			{
				lua_pop(State, 2);
				return FailLuaCall(
					State,
					Error.IsEmpty() ? TEXT("custom map marker is invalid") : Error);
			}
			Marker.Id = FName(*MarkerId);
			Definition.Markers.Add(Marker);
			lua_pop(State, 1);
		}
		lua_pop(State, 1);

		lua_getfield(State, 1, "scene_boxes");
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			return FailLuaCall(State, TEXT("custom map scene_boxes must be an array"));
		}
		const int32 SceneBoxesIndex = lua_absindex(State, -1);
		const size_t SceneBoxCount = lua_rawlen(State, SceneBoxesIndex);
		if (SceneBoxCount > MaximumCustomMapSceneBoxes)
		{
			lua_pop(State, 1);
			return FailLuaCall(State, TEXT("custom map 3D scene box count is out of range"));
		}
		Definition.SceneBoxes.Reserve(static_cast<int32>(SceneBoxCount));
		for (size_t Index = 1; Index <= SceneBoxCount; ++Index)
		{
			lua_rawgeti(State, SceneBoxesIndex, static_cast<lua_Integer>(Index));
			const int32 BoxIndex = lua_absindex(State, -1);
			FMatterFluxCustomMapSceneBoxDefinition Box;
			FString BoxId;
			FString MaterialId;
			if (lua_type(State, BoxIndex) != LUA_TTABLE
				|| !ReadTableStringField(State, BoxIndex, "id", BoxId, true, Error)
				|| !ReadTableStringField(
					State, BoxIndex, "material", MaterialId, true, Error)
				|| !ReadRequiredTableNumberField(
					State, BoxIndex, "center_x", Box.CenterCells.X, Error)
				|| !ReadRequiredTableNumberField(
					State, BoxIndex, "center_y", Box.CenterCells.Y, Error)
				|| !ReadRequiredTableNumberField(
					State, BoxIndex, "center_z", Box.CenterCells.Z, Error)
				|| !ReadRequiredTableNumberField(
					State, BoxIndex, "size_x", Box.SizeCells.X, Error)
				|| !ReadRequiredTableNumberField(
					State, BoxIndex, "size_y", Box.SizeCells.Y, Error)
				|| !ReadRequiredTableNumberField(
					State, BoxIndex, "size_z", Box.SizeCells.Z, Error)
				|| !ReadTableBooleanField(
					State, BoxIndex, "collision", Box.bCollision, Error)
				|| !IsValidContentId(BoxId)
				|| !IsValidContentId(MaterialId))
			{
				lua_pop(State, 2);
				return FailLuaCall(State,
					Error.IsEmpty()
						? TEXT("custom map 3D scene box is invalid")
						: Error);
			}
			Box.Id = FName(*BoxId);
			Box.MaterialId = FName(*MaterialId);
			Definition.SceneBoxes.Add(Box);
			lua_pop(State, 1);
		}
		lua_pop(State, 1);

		lua_getfield(State, 1, "cameras");
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			return FailLuaCall(State, TEXT("custom map cameras must be an array"));
		}
		const int32 CamerasIndex = lua_absindex(State, -1);
		const size_t CameraCount = lua_rawlen(State, CamerasIndex);
		if (CameraCount > MaximumCustomMapCameras)
		{
			lua_pop(State, 1);
			return FailLuaCall(State, TEXT("custom map 3D camera count is out of range"));
		}
		Definition.Cameras.Reserve(static_cast<int32>(CameraCount));
		for (size_t Index = 1; Index <= CameraCount; ++Index)
		{
			lua_rawgeti(State, CamerasIndex, static_cast<lua_Integer>(Index));
			const int32 CameraIndex = lua_absindex(State, -1);
			FMatterFluxCustomMapCameraDefinition Camera;
			FString CameraId;
			if (lua_type(State, CameraIndex) != LUA_TTABLE
				|| !ReadTableStringField(
					State, CameraIndex, "id", CameraId, true, Error)
				|| !ReadRequiredTableNumberField(
					State, CameraIndex, "location_x", Camera.LocationCells.X, Error)
				|| !ReadRequiredTableNumberField(
					State, CameraIndex, "location_y", Camera.LocationCells.Y, Error)
				|| !ReadRequiredTableNumberField(
					State, CameraIndex, "location_z", Camera.LocationCells.Z, Error)
				|| !ReadRequiredTableNumberField(
					State, CameraIndex, "target_x", Camera.TargetCells.X, Error)
				|| !ReadRequiredTableNumberField(
					State, CameraIndex, "target_y", Camera.TargetCells.Y, Error)
				|| !ReadRequiredTableNumberField(
					State, CameraIndex, "target_z", Camera.TargetCells.Z, Error)
				|| !ReadRequiredTableNumberField(
					State, CameraIndex, "field_of_view", Camera.FieldOfViewDegrees, Error)
				|| !IsValidContentId(CameraId))
			{
				lua_pop(State, 2);
				return FailLuaCall(State,
					Error.IsEmpty()
						? TEXT("custom map 3D camera is invalid")
						: Error);
			}
			Camera.Id = FName(*CameraId);
			Definition.Cameras.Add(Camera);
			lua_pop(State, 1);
		}
		lua_pop(State, 1);

		lua_getfield(State, 1, "pour_containers");
		if (lua_type(State, -1) != LUA_TTABLE)
		{
			lua_pop(State, 1);
			return FailLuaCall(
				State,
				TEXT("custom map pour_containers must be an array"));
		}
		const int32 PourContainersIndex = lua_absindex(State, -1);
		const size_t PourContainerCount = lua_rawlen(State, PourContainersIndex);
		if (PourContainerCount > MaximumCustomMapPourContainers)
		{
			lua_pop(State, 1);
			return FailLuaCall(
				State,
				TEXT("custom map tilting container count is out of range"));
		}
		Definition.PourContainers.Reserve(
			static_cast<int32>(PourContainerCount));
		for (size_t Index = 1; Index <= PourContainerCount; ++Index)
		{
			lua_rawgeti(
				State,
				PourContainersIndex,
				static_cast<lua_Integer>(Index));
			const int32 ContainerIndex = lua_absindex(State, -1);
			FMatterFluxCustomMapPourContainerDefinition Container;
			FString ContainerId;
			FString ContainerMaterialId;
			FString LiquidMaterialId;
			if (lua_type(State, ContainerIndex) != LUA_TTABLE
				|| !ReadTableStringField(
					State, ContainerIndex, "id", ContainerId, true, Error)
				|| !ReadTableStringField(
					State, ContainerIndex, "container_material",
					ContainerMaterialId, true, Error)
				|| !ReadTableStringField(
					State, ContainerIndex, "liquid_material",
					LiquidMaterialId, true, Error)
				|| !ReadRequiredTableNumberField(
					State, ContainerIndex, "center_x", Container.CenterCells.X, Error)
				|| !ReadRequiredTableNumberField(
					State, ContainerIndex, "center_y", Container.CenterCells.Y, Error)
				|| !ReadRequiredTableNumberField(
					State, ContainerIndex, "center_z", Container.CenterCells.Z, Error)
				|| !ReadRequiredTableIntegerField(
					State, ContainerIndex, "inner_width",
					Container.InteriorSizeCells.X, Error)
				|| !ReadRequiredTableIntegerField(
					State, ContainerIndex, "inner_depth",
					Container.InteriorSizeCells.Y, Error)
				|| !ReadRequiredTableIntegerField(
					State, ContainerIndex, "inner_height",
					Container.InteriorSizeCells.Z, Error)
				|| !ReadRequiredTableIntegerField(
					State, ContainerIndex, "start_step", Container.StartStep, Error)
				|| !ReadRequiredTableIntegerField(
					State, ContainerIndex, "tilt_steps",
					Container.TiltDurationSteps, Error)
				|| !ReadRequiredTableNumberField(
					State, ContainerIndex, "tilt_degrees",
					Container.TiltDegrees, Error)
				|| !ReadRequiredTableIntegerField(
					State, ContainerIndex, "pour_cells_per_step",
					Container.PourCellsPerStep, Error)
				|| !IsValidContentId(ContainerId)
				|| !IsValidContentId(ContainerMaterialId)
				|| !IsValidContentId(LiquidMaterialId))
			{
				lua_pop(State, 2);
				return FailLuaCall(
					State,
					Error.IsEmpty()
						? TEXT("custom map tilting container is invalid")
						: Error);
			}
			Container.Id = FName(*ContainerId);
			Container.ContainerMaterialId = FName(*ContainerMaterialId);
			Container.LiquidMaterialId = FName(*LiquidMaterialId);
			Definition.PourContainers.Add(Container);
			lua_pop(State, 1);
		}
		lua_pop(State, 1);

		if (!Context.Builder->AddCustomMap(Definition, Error))
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
		lua_pushcfunction(State, RegisterDecorator);
		lua_setfield(State, -2, "register_decorator");
		lua_pushcfunction(State, RegisterEntity);
		lua_setfield(State, -2, "register_entity");
		lua_pushcfunction(State, RegisterCreature);
		lua_setfield(State, -2, "register_creature");
		lua_pushcfunction(State, RegisterDialogue);
		lua_setfield(State, -2, "register_dialogue");
		lua_pushcfunction(State, RegisterShop);
		lua_setfield(State, -2, "register_shop");
		lua_pushcfunction(State, RegisterSpell);
		lua_setfield(State, -2, "register_spell");
		lua_pushcfunction(State, RegisterWand);
		lua_setfield(State, -2, "register_wand");
		lua_pushcfunction(State, RegisterItem);
		lua_setfield(State, -2, "register_item");
		lua_pushcfunction(State, RegisterQuest);
		lua_setfield(State, -2, "register_quest");
		lua_pushcfunction(State, RegisterStructure);
		lua_setfield(State, -2, "register_structure");
		lua_pushcfunction(State, RegisterCustomMap);
		lua_setfield(State, -2, "register_custom_map");
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
