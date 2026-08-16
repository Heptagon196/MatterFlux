#include "Game/MatterFluxCharacter.h"

#include "AbilitySystemComponent.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "EngineUtils.h"
#include "GAS/GA_CastWand.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MatterFluxLog.h"

namespace
{
	bool ActivatePlayerWand(
		UWorld* World,
		const int32 EquipmentSlot)
	{
		if (!World
			|| EquipmentSlot < 0
			|| EquipmentSlot >= UGA_CastWand::EquipmentSlotCount)
		{
			return false;
		}
		for (TActorIterator<AMatterFluxCharacter> It(World); It; ++It)
		{
			AMatterFluxCharacter* Character = *It;
			UAbilitySystemComponent* ASC = Character
				? Character->GetAbilitySystemComponent()
				: nullptr;
			if (!Character
				|| !Character->HasAuthority()
				|| !ASC)
			{
				continue;
			}
			for (const FGameplayAbilitySpec& Spec
				: ASC->GetActivatableAbilities())
			{
				if (Spec.InputID == EquipmentSlot
					&& Spec.Ability
					&& Spec.Ability->IsA<UGA_CastWand>())
				{
					return ASC->TryActivateAbility(
						Spec.Handle,
						true);
				}
			}
		}
		return false;
	}

	void QueueVisualCapture(
		UWorld* World,
		const float DelaySeconds,
		const int32 Multiplier,
		const bool bQuitAfterCapture)
	{
		IConsoleObject* CaptureObject =
			IConsoleManager::Get().FindConsoleObject(
				TEXT("mf.Visual.Capture"));
		IConsoleCommand* CaptureCommand = CaptureObject
			? CaptureObject->AsCommand()
			: nullptr;
		if (!CaptureCommand)
		{
			return;
		}
		TArray<FString> CaptureArgs;
		CaptureArgs.Add(FString::SanitizeFloat(DelaySeconds));
		CaptureArgs.Add(FString::FromInt(Multiplier));
		CaptureArgs.Add(bQuitAfterCapture ? TEXT("1") : TEXT("0"));
		CaptureCommand->Execute(CaptureArgs, World, *GLog);
	}

	void PlayerAbilityCommand(
		const TArray<FString>& Args,
		UWorld* World)
	{
		if (!World || Args.IsEmpty())
		{
			UE_LOG(
				LogMatterFlux,
				Warning,
				TEXT("Usage: mf.Player.Ability Cut|Flame|Left|Right|Q|E|0..3 [repeat-seconds=0] [capture-delay=-1] [capture-multiplier=1] [quit-after=0]"));
			return;
		}

		int32 EquipmentSlot = INDEX_NONE;
		if (Args[0].Equals(TEXT("Cut"), ESearchCase::IgnoreCase)
			|| Args[0].Equals(TEXT("Left"), ESearchCase::IgnoreCase))
		{
			EquipmentSlot = 0;
		}
		else if (Args[0].Equals(TEXT("Flame"), ESearchCase::IgnoreCase)
			|| Args[0].Equals(TEXT("Right"), ESearchCase::IgnoreCase))
		{
			EquipmentSlot = 1;
		}
		else if (Args[0].Equals(TEXT("Q"), ESearchCase::IgnoreCase))
		{
			EquipmentSlot = 2;
		}
		else if (Args[0].Equals(TEXT("E"), ESearchCase::IgnoreCase))
		{
			EquipmentSlot = 3;
		}
		else if (!LexTryParseString(EquipmentSlot, *Args[0])
			|| EquipmentSlot < 0
			|| EquipmentSlot >= UGA_CastWand::EquipmentSlotCount)
		{
			UE_LOG(
				LogMatterFlux,
				Warning,
				TEXT("mf.Player.Ability must name a wand key or equipment slot 0..3."));
			return;
		}
		const double RepeatSeconds = Args.Num() > 1
			? FMath::Clamp(FCString::Atod(*Args[1]), 0.0, 60.0)
			: 0.0;
		const float CaptureDelay = Args.Num() > 2
			? FMath::Clamp(FCString::Atof(*Args[2]), -1.0f, 60.0f)
			: -1.0f;
		const int32 CaptureMultiplier = Args.Num() > 3
			? FMath::Clamp(FCString::Atoi(*Args[3]), 1, 4)
			: 1;
		const bool bQuitAfterCapture =
			Args.Num() > 4 && FCString::Atoi(*Args[4]) != 0;

		const bool bActivated =
			ActivatePlayerWand(World, EquipmentSlot);
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("mf.Player.Ability slot %d: %s"),
			EquipmentSlot,
			bActivated ? TEXT("activated") : TEXT("waiting for player ASC"));

		if (RepeatSeconds > 0.0)
		{
			const TWeakObjectPtr<UWorld> WeakWorld(World);
			const TSharedRef<double> StartedAt =
				MakeShared<double>(FPlatformTime::Seconds());
			const TSharedRef<double> LastActivationAt =
				MakeShared<double>(-1.0);
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[WeakWorld,
						EquipmentSlot,
						RepeatSeconds,
						StartedAt,
						LastActivationAt](float)
					{
						const double Now =
							FPlatformTime::Seconds();
						if (!WeakWorld.IsValid()
							|| Now - *StartedAt
								>= RepeatSeconds)
						{
							return false;
						}
						if (*LastActivationAt < 0.0
							|| Now - *LastActivationAt
								>= 0.1)
						{
							ActivatePlayerWand(
								WeakWorld.Get(),
								EquipmentSlot);
							*LastActivationAt = Now;
						}
						return true;
					}),
				0.1f);
		}

		if (CaptureDelay >= 0.0f)
		{
			QueueVisualCapture(
				World,
				CaptureDelay,
				CaptureMultiplier,
				bQuitAfterCapture);
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GPlayerAbilityCommand(
		TEXT("mf.Player.Ability"),
		TEXT("Activate an equipped wand through its real GAS InputID: mf.Player.Ability Cut|Flame|Left|Right|Q|E|0..3 [repeat-seconds=0] [capture-delay=-1] [capture-multiplier=1] [quit-after=0]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&PlayerAbilityCommand));
}
