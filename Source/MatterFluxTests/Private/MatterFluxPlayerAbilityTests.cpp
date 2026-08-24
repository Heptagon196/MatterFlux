#include "GAS/GA_PlayerCut.h"
#include "GAS/GA_PlayerFlameJet.h"

#include "EnhancedInputComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Game/MatterFluxCharacter.h"
#include "Engine/TargetPoint.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
	bool HasMapping(
		const UInputMappingContext& Context,
		const UInputAction* Action,
		const FKey& Key)
	{
		for (const FEnhancedActionKeyMapping& Mapping
			: Context.GetMappings())
		{
			if (Mapping.Action == Action && Mapping.Key == Key)
			{
				return true;
			}
		}
		return false;
	}

	AFragment2DSourceActor* SpawnCutTarget(
		UWorld& World,
		const FVector& Location,
		const uint32 IdSalt)
	{
		AFragment2DSourceActor* Source =
			World.SpawnActor<AFragment2DSourceActor>(
				Location,
				FRotator::ZeroRotator);
		if (!Source)
		{
			return nullptr;
		}
		FFragmentSourceMask Mask;
		Mask.Width = 12;
		Mask.Height = 12;
		Mask.CellSize = 10.0f;
		Mask.MinFragmentAreaPixels = 1;
		Mask.MaxFragmentsPerBreak = 4;
		Mask.SolidMask.Init(1, Mask.Width * Mask.Height);
		Source->bDestroySourceOnFirstBreak = false;
		const FGuid SourceId =
			FGuid::NewDeterministicGuid(
				TEXT("PlayerAbilityCutTarget"),
				IdSalt);
		return Source->InitializeFromProceduralMask(
			Mask,
			SourceId,
			FLinearColor::White,
			TEXT("wood"))
			? Source
			: nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayerMouseAbilityInputTest,
	"MatterFlux.PlayerAbilities.MouseInputMappings",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayerMouseAbilityInputTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxCharacter* Character = World
		? World->SpawnActor<AMatterFluxCharacter>()
		: nullptr;
	UEnhancedInputComponent* Input = Character
		? NewObject<UEnhancedInputComponent>(Character)
		: nullptr;
	if (!TestNotNull(TEXT("Character spawns"), Character)
		|| !TestNotNull(TEXT("Enhanced Input component exists"), Input))
	{
		return false;
	}
	Character->SetupPlayerInputComponent(Input);
	const UInputMappingContext* Context =
		Character->GetPlayableMappingContext();
	const UInputAction* LeftWand = Character->GetCastWandAction(0);
	const UInputAction* RightWand = Character->GetCastWandAction(1);
	const UInputAction* QWand = Character->GetCastWandAction(2);
	const UInputAction* EWand = Character->GetCastWandAction(3);
	if (!TestNotNull(TEXT("Playable mapping context exists"), Context)
		|| !TestNotNull(TEXT("Left wand action exists"), LeftWand)
		|| !TestNotNull(TEXT("Right wand action exists"), RightWand)
		|| !TestNotNull(TEXT("Q wand action exists"), QWand)
		|| !TestNotNull(TEXT("E wand action exists"), EWand))
	{
		return false;
	}
	TestTrue(
		TEXT("Left mouse maps to equipment slot zero"),
		HasMapping(*Context, LeftWand, EKeys::LeftMouseButton));
	TestTrue(
		TEXT("Right mouse maps to equipment slot one"),
		HasMapping(*Context, RightWand, EKeys::RightMouseButton));
	TestTrue(
		TEXT("Q maps to equipment slot two"),
		HasMapping(*Context, QWand, EKeys::Q));
	TestTrue(
		TEXT("E maps to equipment slot three"),
		HasMapping(*Context, EWand, EKeys::E));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayerAbilityVoxelEffectsTest,
	"MatterFlux.PlayerAbilities.VoxelEffects",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayerAbilityVoxelEffectsTest::RunTest(
	const FString& Parameters)
{
	TArray<FTransform> CutTransforms;
	AMatterFluxCharacter::BuildAbilityEffectTransforms(
		EMatterFluxPlayerAbilityEffect::Cut,
		CutTransforms);
	TestEqual(
		TEXT("Cut creates a twelve-voxel slash"),
		CutTransforms.Num(),
		12);
	TArray<FTransform> FlameTransforms;
	AMatterFluxCharacter::BuildAbilityEffectTransforms(
		EMatterFluxPlayerAbilityEffect::FlameJet,
		FlameTransforms);
	TestEqual(
		TEXT("Flame creates a twenty-four-voxel jet"),
		FlameTransforms.Num(),
		24);
	TestTrue(
		TEXT("Flame expands along the forward axis"),
		FlameTransforms.Last().GetLocation().X
			> FlameTransforms[0].GetLocation().X);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayerForwardCutTest,
	"MatterFlux.PlayerAbilities.LeftClickCutAffectsOnlyTargetsAhead",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayerForwardCutTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ATargetPoint* Avatar = World
		? World->SpawnActor<ATargetPoint>(
			FVector(0.0, 0.0, 100.0),
			FRotator::ZeroRotator)
		: nullptr;
	AFragment2DSourceActor* Ahead = World
		? SpawnCutTarget(
			*World,
			FVector(450.0, 0.0, 100.0),
			1)
		: nullptr;
	AFragment2DSourceActor* Behind = World
		? SpawnCutTarget(
			*World,
			FVector(-450.0, 0.0, 100.0),
			2)
		: nullptr;
	if (!TestNotNull(TEXT("Avatar spawns"), Avatar)
		|| !TestNotNull(TEXT("Ahead target spawns"), Ahead)
		|| !TestNotNull(TEXT("Behind target spawns"), Behind))
	{
		return false;
	}

	const int32 CutCount = UGA_PlayerCut::ExecuteForwardCut(
		*Avatar,
		900.0f,
		140.0f,
		30.0f,
		1200.0f,
		991);
	TestEqual(TEXT("Exactly the forward target accepts the cut"),
		CutCount,
		1);
	TestEqual(TEXT("Forward target commits one damage revision"),
		Ahead->Revision,
		1);
	TestEqual(TEXT("Target behind the player remains unchanged"),
		Behind->Revision,
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayerForwardFlameTest,
	"MatterFlux.PlayerAbilities.RightClickFlameActivatesOnlyTargetsAhead",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayerForwardFlameTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ATargetPoint* Avatar = World
		? World->SpawnActor<ATargetPoint>(
			FVector(0.0, 0.0, 100.0),
			FRotator::ZeroRotator)
		: nullptr;
	AFragment2DSourceActor* Ahead = World
		? SpawnCutTarget(
			*World,
			FVector(450.0, 0.0, 100.0),
			11)
		: nullptr;
	AFragment2DSourceActor* Behind = World
		? SpawnCutTarget(
			*World,
			FVector(-450.0, 0.0, 100.0),
			12)
		: nullptr;
	if (!TestNotNull(TEXT("Avatar spawns"), Avatar)
		|| !TestNotNull(TEXT("Ahead target spawns"), Ahead)
		|| !TestNotNull(TEXT("Behind target spawns"), Behind))
	{
		return false;
	}

	const int32 ActivatedCount =
		UGA_PlayerFlameJet::ExecuteFlameJet(
			*Avatar,
			800.0f,
			45.0f,
			180.0f,
			TEXT("fire"),
			1776);
	TestEqual(
		TEXT("Exactly the forward reactive target ignites"),
		ActivatedCount,
		1);
	TestTrue(
		TEXT("Forward wood target is reacting"),
		Ahead->IsReacting());
	TestFalse(
		TEXT("Target behind the player stays unlit"),
		Behind->IsReacting());
	return true;
}
