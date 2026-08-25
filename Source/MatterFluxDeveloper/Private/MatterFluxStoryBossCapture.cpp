#include "CoreMinimal.h"

#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxGameMode.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HighResScreenshot.h"
#include "MatterFluxLog.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "Save/MatterFluxSaveTypes.h"

namespace
{
	struct FStoryBossCaptureState
	{
		double QueuedAt = 0.0;
		double PhaseStartedAt = 0.0;
		int32 Phase = 0;
		bool bQuitAfterCapture = true;
		FString OutputDirectory;
		TArray<TWeakObjectPtr<AMatterFluxCreatureActor>> Bosses;
	};

	bool GStoryBossCapturePending = false;

	bool FailStoryBossCapture(
		const TSharedRef<FStoryBossCaptureState>& State,
		const TCHAR* Message,
		const int32 ExitCode = 4)
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Story boss capture failed in phase %d: %s"),
			State->Phase,
			Message);
		GStoryBossCapturePending = false;
		if (State->bQuitAfterCapture)
		{
			FPlatformMisc::RequestExitWithStatus(false, ExitCode);
		}
		return false;
	}

	TArray<AMatterFluxCreatureActor*> FindStoryBosses(UWorld& World)
	{
		TArray<AMatterFluxCreatureActor*> Bosses;
		for (TActorIterator<AMatterFluxCreatureActor> It(&World); It; ++It)
		{
			if (IsValid(*It)
				&& !It->IsActorBeingDestroyed()
				&& It->GetDefinitionId() == TEXT("std.test_boss"))
			{
				Bosses.Add(*It);
			}
		}
		Bosses.Sort([](
			const AMatterFluxCreatureActor& Left,
			const AMatterFluxCreatureActor& Right)
		{
			const FVector LeftLocation = Left.GetActorLocation();
			const FVector RightLocation = Right.GetActorLocation();
			return LeftLocation.X != RightLocation.X
				? LeftLocation.X < RightLocation.X
				: LeftLocation.Y < RightLocation.Y;
		});
		return Bosses;
	}

	bool TickStoryBossCapture(
		const TSharedRef<FStoryBossCaptureState>& State)
	{
		const double Now = FPlatformTime::Seconds();
		UWorld* World = GEngine && GEngine->GameViewport
			? GEngine->GameViewport->GetWorld()
			: nullptr;
		if (!World || !World->IsGameWorld())
		{
			return Now - State->QueuedAt < 45.0
				? true
				: FailStoryBossCapture(State,
					TEXT("timed out waiting for a game viewport"));
		}

		APlayerController* PlayerController = World->GetFirstPlayerController();
		AMatterFluxCharacter* Character = PlayerController
			? Cast<AMatterFluxCharacter>(PlayerController->GetPawn())
			: nullptr;
		AMatterFluxPlayerState* PlayerState = PlayerController
			? PlayerController->GetPlayerState<AMatterFluxPlayerState>()
			: nullptr;
		AMatterFluxGameMode* GameMode = Cast<AMatterFluxGameMode>(
			World->GetAuthGameMode());
		AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			PlayableWorld = *It;
			break;
		}
		if (!PlayerController || !Character || !PlayerState
			|| !GameMode || !PlayableWorld)
		{
			return Now - State->QueuedAt < 45.0
				? true
				: FailStoryBossCapture(State,
					TEXT("timed out waiting for story runtime actors"));
		}

		if (Now - State->QueuedAt > 90.0)
		{
			return FailStoryBossCapture(State, TEXT("timed out"));
		}
		if (FScreenshotRequest::IsScreenshotRequested())
		{
			return true;
		}

		switch (State->Phase)
		{
		case 0:
		{
			FString Error;
			PlayableWorld->Regenerate(
				AMatterFluxPlayableWorldActor::PaperMagicStorySeed);
			if (!PlayableWorld->LoadCustomMap(
				TEXT("story.paper_magic"), 1, Error))
			{
				return FailStoryBossCapture(State,
					*FString::Printf(TEXT("story map load failed: %s"), *Error));
			}

			FMatterFluxProgressionSaveState FinalQuestState;
			FMatterFluxSavedQuestState& SavedQuest =
				FinalQuestState.Quests.AddDefaulted_GetRef();
			SavedQuest.QuestId = TEXT("std.side_kill_boss");
			SavedQuest.Status = static_cast<uint8>(
				EMatterFluxQuestRuntimeStatus::Active);
			SavedQuest.bActivationRewardsGranted = true;
			FinalQuestState.SelectedQuest = SavedQuest.QuestId;
			if (!PlayerState->GetProgression()->RestoreSaveStateAuthority(
				FinalQuestState, Error))
			{
				return FailStoryBossCapture(State,
					*FString::Printf(
						TEXT("final quest restore failed: %s"), *Error));
			}
			GameMode->PrepareForInitialWorldEntry();
			State->OutputDirectory = FPaths::Combine(
				FPaths::ScreenShotDir(),
				TEXT("MatterFluxStoryBosses"),
				FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
			State->Phase = 1;
			State->PhaseStartedAt = Now;
			UE_LOG(LogMatterFlux, Display,
				TEXT("Story boss capture loaded active final quest; "
					"waiting for the complete world-entry barrier."));
			return true;
		}

		case 1:
			if (!PlayableWorld->IsInitialWorldEntryReady())
			{
				return true;
			}
			if (!GameMode->CompleteExistingPlayerWorldLoad())
			{
				return FailStoryBossCapture(State,
					TEXT("ready story world did not reopen creature spawning"));
			}
			State->Phase = 2;
			State->PhaseStartedAt = Now;
			return true;

		case 2:
		{
			const TArray<AMatterFluxCreatureActor*> Bosses =
				FindStoryBosses(*World);
			if (Bosses.Num() != 2)
			{
				if (Now - State->PhaseStartedAt < 8.0)
				{
					return true;
				}
				return FailStoryBossCapture(State,
					*FString::Printf(
						TEXT("expected two generated bosses, found %d"),
						Bosses.Num()), 5);
			}
			// Keep the player at the authored start long enough to exercise the
			// exact suspected failure: final-region bosses remain alive while the
			// streaming focus is several kilometres away.
			FVector PlayerStart;
			if (!PlayableWorld->TryGetCustomMapMarker(
				TEXT("player_start"), PlayerStart))
			{
				return FailStoryBossCapture(State,
					TEXT("story player marker is missing"));
			}
			PlayableWorld->SetWorldStreamingFocus(PlayerStart);
			State->Bosses.Reset();
			for (AMatterFluxCreatureActor* Boss : Bosses)
			{
				State->Bosses.Add(Boss);
			}
			State->Phase = 3;
			State->PhaseStartedAt = Now;
			return true;
		}

		case 3:
		{
			if (Now - State->PhaseStartedAt < 3.0)
			{
				return true;
			}
			TArray<AMatterFluxCreatureActor*> Bosses = FindStoryBosses(*World);
			if (Bosses.Num() != 2)
			{
				return FailStoryBossCapture(State,
					TEXT("bosses vanished while the streaming focus stayed at player start"),
					5);
			}
			const FVector BossMidpoint =
				(Bosses[0]->GetActorLocation()
					+ Bosses[1]->GetActorLocation()) * 0.5;
			const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
			FVector PlayerLocation;
			if (!Capsule
				|| !PlayableWorld->TryResolveTerrainSpawnLocation(
					BossMidpoint,
					Capsule->GetScaledCapsuleRadius(),
					Capsule->GetScaledCapsuleHalfHeight(),
					4.0f,
					PlayerLocation))
			{
				return FailStoryBossCapture(State,
					TEXT("could not ground the player in the final region"));
			}
			for (AMatterFluxCreatureActor* Boss : Bosses)
			{
				if (AController* Controller = Boss->GetController())
				{
					Controller->StopMovement();
					Controller->SetActorTickEnabled(false);
				}
				if (UCharacterMovementComponent* Movement =
					Boss->GetCharacterMovement())
				{
					Movement->StopMovementImmediately();
					Movement->DisableMovement();
				}
			}
			Character->SetActorLocation(
				PlayerLocation,
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
			PlayerController->SetViewTarget(Character);
			PlayableWorld->SetWorldStreamingFocus(PlayerLocation);
			if (Character->CameraBoom)
			{
				Character->CameraBoom->bEnableCameraLag = false;
				Character->CameraBoom->TargetArmLength = 1700.0f;
				Character->CameraBoom->TargetOffset = FVector::ZeroVector;
				Character->CameraBoom->SetRelativeRotation(
					FRotator(-47.0f, -45.0f, 0.0f));
			}
			State->Phase = 4;
			State->PhaseStartedAt = Now;
			return true;
		}

		case 4:
		{
			if (Now - State->PhaseStartedAt < 3.0
				|| PlayableWorld->IsGenerationInProgress()
				|| PlayableWorld->GetPendingTerrainChunkPrefetchCount() > 0
				|| PlayableWorld->GetPendingProceduralPopulationUpdateCount() > 0
				|| PlayableWorld->GetPendingFragmentSourceSpawnCount() > 0)
			{
				return true;
			}
			const TArray<AMatterFluxCreatureActor*> Bosses =
				FindStoryBosses(*World);
			if (Bosses.Num() != 2)
			{
				return FailStoryBossCapture(State,
					TEXT("final region did not retain two bosses"), 5);
			}
			const FIntPoint ViewportSize = GEngine->GameViewport->Viewport
				? GEngine->GameViewport->Viewport->GetSizeXY()
				: FIntPoint::ZeroValue;
			FVector ViewLocation;
			FRotator ViewRotation;
			PlayerController->GetPlayerViewPoint(
				ViewLocation, ViewRotation);
			UE_LOG(LogMatterFlux, Display,
				TEXT("Story boss capture camera: player=%s view=%s rotation=%s"),
				*Character->GetActorLocation().ToCompactString(),
				*ViewLocation.ToCompactString(),
				*ViewRotation.ToCompactString());
			bool bAllVisible = ViewportSize.X > 0 && ViewportSize.Y > 0;
			for (int32 Index = 0; Index < Bosses.Num(); ++Index)
			{
				AMatterFluxCreatureActor* Boss = Bosses[Index];
				FVector2D ScreenPosition;
				const bool bProjected =
					PlayerController->ProjectWorldLocationToScreen(
						Boss->GetActorLocation()
							+ FVector(0.0f, 0.0f, 80.0f),
						ScreenPosition,
						true);
				const bool bOnScreen = bProjected
					&& ScreenPosition.X >= 0.0f
					&& ScreenPosition.Y >= 0.0f
					&& ScreenPosition.X < ViewportSize.X
					&& ScreenPosition.Y < ViewportSize.Y;
				const bool bVisualsVisible = Boss->BodyVisual
					&& Boss->HeadVisual
					&& Boss->AccentVisual
					&& Boss->BodyVisual->IsVisible()
					&& Boss->HeadVisual->IsVisible()
					&& Boss->AccentVisual->IsVisible();
				const bool bRendered = Boss->WasRecentlyRendered(1.0f);
				UE_LOG(LogMatterFlux, Display,
					TEXT("Story boss capture audit: index=%d location=%s "
						"screen=(%.1f,%.1f) on_screen=%s hidden=%s "
						"visuals=%s rendered=%s"),
					Index,
					*Boss->GetActorLocation().ToCompactString(),
					ScreenPosition.X,
					ScreenPosition.Y,
					bOnScreen ? TEXT("true") : TEXT("false"),
					Boss->IsHidden() ? TEXT("true") : TEXT("false"),
					bVisualsVisible ? TEXT("true") : TEXT("false"),
					bRendered ? TEXT("true") : TEXT("false"));
				bAllVisible = bAllVisible
					&& bOnScreen
					&& !Boss->IsHidden()
					&& bVisualsVisible
					&& bRendered;
			}
			if (!bAllVisible)
			{
				return FailStoryBossCapture(State,
					TEXT("both bosses were not visibly rendered in the final-region camera"),
					5);
			}
			IFileManager::Get().MakeDirectory(
				*State->OutputDirectory, true);
			const FString ScreenshotPath = FPaths::Combine(
				State->OutputDirectory,
				TEXT("01_FinalRegion_TwoBossesVisible.png"));
			FScreenshotRequest::RequestScreenshot(
				ScreenshotPath, true, false, false);
			UE_LOG(LogMatterFlux, Display,
				TEXT("Story boss capture requested screenshot: %s"),
				*ScreenshotPath);
			State->Phase = 5;
			State->PhaseStartedAt = Now;
			return true;
		}

		case 5:
			if (Now - State->PhaseStartedAt < 0.75)
			{
				return true;
			}
			UE_LOG(LogMatterFlux, Display,
				TEXT("Story boss capture complete: bosses=2 output=%s"),
				*State->OutputDirectory);
			GStoryBossCapturePending = false;
			if (State->bQuitAfterCapture)
			{
				FPlatformMisc::RequestExitWithStatus(false, 0);
			}
			return false;

		default:
			return FailStoryBossCapture(State, TEXT("invalid phase"));
		}
	}

	void QueueStoryBossCapture(
		const TArray<FString>& Args,
		UWorld* World)
	{
		(void)World;
		if (GStoryBossCapturePending)
		{
			UE_LOG(LogMatterFlux, Warning,
				TEXT("Story boss capture is already pending."));
			return;
		}
		const TSharedRef<FStoryBossCaptureState> State =
			MakeShared<FStoryBossCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->PhaseStartedAt = State->QueuedAt;
		State->bQuitAfterCapture = Args.IsEmpty()
			|| FCString::Atoi(*Args[0]) != 0;
		GStoryBossCapturePending = true;
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickStoryBossCapture(State);
			}));
	}

	FAutoConsoleCommandWithWorldAndArgs GStoryBossCaptureCommand(
		TEXT("mf.Visual.StoryBosses"),
		TEXT("Load an active final-story quest and capture both visible bosses: "
			"mf.Visual.StoryBosses [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueStoryBossCapture));
}
