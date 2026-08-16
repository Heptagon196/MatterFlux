#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

#include "AbilitySystemComponent.h"
#include "GAS/GA_CastWand.h"
#include "GAS/GA_FragmentDebugDamage.h"
#include "GAS/GA_PlayerCut.h"
#include "GAS/GA_PlayerFlameJet.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "MatterFluxGameplayTags.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "GAS/MatterFluxPlayerAttributeSet.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxPlayerStateASCTest, "MatterFlux.GAS.PlayerStateASCDefaults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayerStateASCTest::RunTest(const FString& Parameters)
{
	const AMatterFluxPlayerState* PlayerStateCDO = GetDefault<AMatterFluxPlayerState>();
	TestNotNull(TEXT("PlayerState CDO exists"), PlayerStateCDO);
	const UAbilitySystemComponent* ASC = PlayerStateCDO ? PlayerStateCDO->GetAbilitySystemComponent() : nullptr;
	TestNotNull(TEXT("PlayerState owns an AbilitySystemComponent"), ASC);
	if (ASC) TestTrue(TEXT("ASC is marked replicated"), ASC->GetIsReplicated());
	bool bHasDebugAbility = false;
	bool bHasCutAbility = false;
	bool bHasFlameAbility = false;
	bool bHasWandAbility = false;
	if (PlayerStateCDO)
	{
		for (const TSubclassOf<UGameplayAbility> AbilityClass : PlayerStateCDO->GetDefaultAbilities())
		{
			bHasDebugAbility |= AbilityClass == UGA_FragmentDebugDamage::StaticClass();
			bHasCutAbility |= AbilityClass == UGA_PlayerCut::StaticClass();
			bHasFlameAbility |= AbilityClass == UGA_PlayerFlameJet::StaticClass();
			bHasWandAbility |= AbilityClass == UGA_CastWand::StaticClass();
		}
	}
	TestFalse(TEXT("PlayerState does not grant the fragment debug ability by default"), bHasDebugAbility);
	TestFalse(TEXT("Cut is a wand spell, not a default player ability"), bHasCutAbility);
	TestFalse(TEXT("Flame is a wand spell, not a default player ability"), bHasFlameAbility);
	TestTrue(TEXT("PlayerState grants the wand cast ability by default"), bHasWandAbility);
	TestNotNull(
		TEXT("PlayerState owns a replicated magic inventory component"),
		PlayerStateCDO ? PlayerStateCDO->GetMagicInventory() : nullptr);
	TestNotNull(
		TEXT("PlayerState owns a replicated progression component"),
		PlayerStateCDO ? PlayerStateCDO->GetProgression() : nullptr);
	TestNotNull(
		TEXT("PlayerState owns replicated player attributes"),
		PlayerStateCDO ? PlayerStateCDO->GetPlayerAttributes() : nullptr);
	const AMatterFluxPlayerController* ControllerCDO = GetDefault<AMatterFluxPlayerController>();
	TestNotNull(TEXT("PlayerController CDO exists"), ControllerCDO);
	if (ControllerCDO)
	{
		TestFalse(TEXT("Shipping-safe defaults do not install debug input"), ControllerCDO->AreDebugControlsEnabled());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWandAbilityInputIdsTest,
	"MatterFlux.GAS.WandAbilityHasOneSpecPerEquipmentKey",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxWandAbilityInputIdsTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>()
		: nullptr;
	if (!TestNotNull(TEXT("Authority PlayerState spawns"), PlayerState))
	{
		return false;
	}
	PlayerState->GrantDefaultAbilities();
	UAbilitySystemComponent* ASC =
		PlayerState->GetAbilitySystemComponent();
	TSet<int32> WandInputIds;
	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		if (Spec.Ability && Spec.Ability->IsA<UGA_CastWand>())
		{
			WandInputIds.Add(Spec.InputID);
		}
	}
	TestEqual(TEXT("One generic wand ability spec exists per equipment slot"),
		WandInputIds.Num(),
		UGA_CastWand::EquipmentSlotCount);
	for (int32 Slot = 0; Slot < UGA_CastWand::EquipmentSlotCount; ++Slot)
	{
		TestTrue(
			*FString::Printf(TEXT("Equipment slot %d has a GAS InputID"), Slot),
			WandInputIds.Contains(Slot));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayerAbilityDefaultsTest,
	"MatterFlux.GAS.PlayerAbilityDefaults",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayerAbilityDefaultsTest::RunTest(
	const FString& Parameters)
{
	const UGA_PlayerCut* Cut = GetDefault<UGA_PlayerCut>();
	const UGA_PlayerFlameJet* Flame =
		GetDefault<UGA_PlayerFlameJet>();
	const UGA_CastWand* Wand = GetDefault<UGA_CastWand>();
	TestNotNull(TEXT("Cut ability CDO exists"), Cut);
	TestNotNull(TEXT("Flame ability CDO exists"), Flame);
	TestNotNull(TEXT("Wand ability CDO exists"), Wand);
	if (!Cut || !Flame || !Wand)
	{
		return false;
	}
	TestEqual(
		TEXT("Cut executes only on the server"),
		Cut->GetNetExecutionPolicy(),
		EGameplayAbilityNetExecutionPolicy::ServerOnly);
	TestEqual(
		TEXT("Flame executes only on the server"),
		Flame->GetNetExecutionPolicy(),
		EGameplayAbilityNetExecutionPolicy::ServerOnly);
	TestEqual(
		TEXT("Wand executes only on the server"),
		Wand->GetNetExecutionPolicy(),
		EGameplayAbilityNetExecutionPolicy::ServerOnly);
	TestTrue(
		TEXT("Cut owns its gameplay ability tag"),
		Cut->GetAssetTags().HasTagExact(
			FGameplayTag::RequestGameplayTag(
				TEXT("Ability.Player.Cut"))));
	TestTrue(
		TEXT("Flame owns its gameplay ability tag"),
		Flame->GetAssetTags().HasTagExact(
			FGameplayTag::RequestGameplayTag(
				TEXT("Ability.Player.FlameJet"))));
	TestTrue(
		TEXT("Wand owns its gameplay ability tag"),
		Wand->GetAssetTags().HasTagExact(TAG_Ability_Player_CastWand));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxDebugAbilityDefaultsTest, "MatterFlux.GAS.DebugAbilityDefaults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDebugAbilityDefaultsTest::RunTest(const FString& Parameters)
{
	const UGA_FragmentDebugDamage* AbilityCDO = GetDefault<UGA_FragmentDebugDamage>();
	TestNotNull(TEXT("Debug ability CDO exists"), AbilityCDO);
	if (!AbilityCDO) return false;
	TestEqual(TEXT("Debug ability is server-only"), AbilityCDO->GetNetExecutionPolicy(), EGameplayAbilityNetExecutionPolicy::ServerOnly);
	TestEqual(TEXT("Debug ability is instanced per actor"), AbilityCDO->GetInstancingPolicy(), EGameplayAbilityInstancingPolicy::InstancedPerActor);
	TestTrue(TEXT("Debug ability owns Ability.Fragment.DebugDamage tag"), AbilityCDO->GetAssetTags().HasTagExact(TAG_Ability_Fragment_DebugDamage));
	TestNull(TEXT("Null world has no debug source actor"), UGA_FragmentDebugDamage::FindDebugSourceActor(nullptr));
	return true;
}
