#include "CoreMinimal.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "GameFramework/SpringArmComponent.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "MatterFluxLog.h"
#include "Game/MatterFluxCharacter.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Save/MatterFluxSaveSubsystem.h"
#include "UnrealClient.h"

namespace
{
	bool GVisualCapturePending = false;
	bool GOccludedPlayerCapturePending = false;
	bool GTreeCutCapturePending = false;
	bool GShellCapturePending = false;
	bool GStabilityCapturePending = false;

	bool TryApplySpellTreeVisualPreset(
		AMatterFluxPlayerController& PlayerController,
		const int32 EquipmentSlot,
		const FString& Preset,
		FString& OutError)
	{
		OutError.Reset();
		AMatterFluxPlayerState* PlayerState =
			PlayerController.GetPlayerState<AMatterFluxPlayerState>();
		UMatterFluxMagicInventoryComponent* Inventory = PlayerState
			? PlayerState->GetMagicInventory()
			: nullptr;
		if (!Inventory || Inventory->GetInventoryRevision() <= 0)
		{
			OutError = TEXT("magic inventory is not ready");
			return false;
		}
		const FGuid WandId = Inventory->GetEquippedWandId(EquipmentSlot);
		const FMatterFluxOwnedWand* Wand = Inventory->FindWand(WandId);
		if (!Wand)
		{
			OutError = TEXT("visual preset target wand is not ready");
			return false;
		}

		TArray<FName> DesiredSlots;
		if (Preset.Equals(TEXT("nested"), ESearchCase::IgnoreCase))
		{
			DesiredSlots = {
				TEXT("spell.double_cast"),
				TEXT("spell.add_damage"),
				TEXT("spell.spark_trigger"),
				TEXT("spell.spark_bolt"),
				TEXT("spell.ember_bolt")
			};
		}
		else if (Preset.Equals(TEXT("forest"), ESearchCase::IgnoreCase))
		{
			DesiredSlots = {
				TEXT("std.default"),
				TEXT("std.double_cast"),
				TEXT("std.add_damage"),
				TEXT("std.default"),
				TEXT("std.trigger_on_collision"),
				TEXT("std.default"),
				TEXT("std.jump"),
				TEXT("std.jump")
			};
		}
		else if (Preset.Equals(TEXT("incomplete"), ESearchCase::IgnoreCase))
		{
			DesiredSlots = {
				TEXT("std.double_cast"),
				NAME_None,
				TEXT("std.default"),
				TEXT("std.jump")
			};
		}
		else if (Preset.Equals(TEXT("empty"), ESearchCase::IgnoreCase))
		{
			DesiredSlots.Reset();
		}
		else
		{
			OutError = FString::Printf(
				TEXT("unknown spell-tree visual preset '%s'"),
				*Preset);
			return false;
		}

		const int32 SlotCount = Wand->SpellSlots.Num();
		for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
		{
			const FMatterFluxOwnedWand* Current = Inventory->FindWand(WandId);
			if (!Current || Current->SpellSlots[SlotIndex].IsNone())
			{
				continue;
			}
			FMatterFluxMagicEdit Edit;
			Edit.Type = EMatterFluxMagicEditType::RemoveSpell;
			Edit.ExpectedRevision = Inventory->GetInventoryRevision();
			Edit.WandId = WandId;
			Edit.FromSpellSlot = SlotIndex;
			if (!Inventory->ApplyEditAuthority(Edit, OutError))
			{
				return false;
			}
		}
		for (int32 SlotIndex = 0;
			SlotIndex < DesiredSlots.Num() && SlotIndex < SlotCount;
			++SlotIndex)
		{
			if (DesiredSlots[SlotIndex].IsNone())
			{
				continue;
			}
			FMatterFluxMagicEdit Edit;
			Edit.Type = EMatterFluxMagicEditType::AssignSpell;
			Edit.ExpectedRevision = Inventory->GetInventoryRevision();
			Edit.WandId = WandId;
			Edit.SpellId = DesiredSlots[SlotIndex];
			Edit.ToSpellSlot = SlotIndex;
			if (!Inventory->ApplyEditAuthority(Edit, OutError))
			{
				return false;
			}
		}
		return true;
	}

	struct FTreeCutCaptureState
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AFragment2DSourceActor> Tree;
		FString OutputDirectory;
		double NextActionAt = 0.0;
		double WaitStartedAt = 0.0;
		double QueuedAt = 0.0;
		int32 Phase = 0;
		int32 EventSeed = 1337;
		bool bQuitAfterCapture = true;
		TWeakObjectPtr<AMatterFluxCharacter> Character;
		float OriginalCameraArmLength = 0.0f;
		FVector OriginalCameraTargetOffset = FVector::ZeroVector;
		bool bOriginalCharacterVisualVisible = true;
	};

	void RestoreTreeCutCamera(const FTreeCutCaptureState& State)
	{
		AMatterFluxCharacter* Character = State.Character.Get();
		if (!Character)
		{
			return;
		}
		if (Character->CameraBoom)
		{
			Character->CameraBoom->TargetArmLength =
				State.OriginalCameraArmLength;
			Character->CameraBoom->TargetOffset =
				State.OriginalCameraTargetOffset;
		}
		if (Character->CharacterVisual)
		{
			Character->CharacterVisual->SetVisibility(
				State.bOriginalCharacterVisualVisible,
				true);
		}
	}

	AFragment2DSourceActor* FindNearestUncutTree(UWorld& World)
	{
		FVector Focus = FVector::ZeroVector;
		if (const APlayerController* PlayerController =
			World.GetFirstPlayerController())
		{
			if (const APawn* Pawn = PlayerController->GetPawn())
			{
				Focus = Pawn->GetActorLocation();
			}
		}

		AFragment2DSourceActor* BestTree = nullptr;
		double BestDistanceSquared = TNumericLimits<double>::Max();
		FString BestId;
		for (TActorIterator<AFragment2DSourceActor> It(&World); It; ++It)
		{
			AFragment2DSourceActor* Candidate = *It;
			if (!IsValid(Candidate)
				|| Candidate->bBroken
				|| !Candidate->bAggregateRoot
				|| Candidate->SourceMaterialId != TEXT("wood")
				|| Candidate->Revision != 0)
			{
				continue;
			}
			const double DistanceSquared = FVector::DistSquared(
				Candidate->GetActorLocation(),
				Focus);
			const FString CandidateId =
				Candidate->SourceId.ToString(EGuidFormats::Digits);
			if (!BestTree
				|| DistanceSquared < BestDistanceSquared
				|| (DistanceSquared == BestDistanceSquared
					&& CandidateId < BestId))
			{
				BestTree = Candidate;
				BestDistanceSquared = DistanceSquared;
				BestId = CandidateId;
			}
		}
		return BestTree;
	}

	void RequestTreeCutScreenshot(
		const FTreeCutCaptureState& State,
		const TCHAR* Filename)
	{
		const FString Path = FPaths::Combine(
			State.OutputDirectory,
			Filename);
		FScreenshotRequest::RequestScreenshot(
			Path,
			false,
			false,
			false,
			FIntRect(),
			true);
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("Tree cut sequence requested screenshot: %s"),
			*Path);
	}

	bool TickTreeCutCapture(
		const TSharedRef<FTreeCutCaptureState>& State)
	{
		const double Now = FPlatformTime::Seconds();
		UWorld* World =
			GEngine && GEngine->GameViewport
				? GEngine->GameViewport->GetWorld()
				: nullptr;
		if (!World || !World->IsGameWorld())
		{
			if (Now - State->QueuedAt >= 30.0)
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Tree cut capture timed out waiting for a game viewport."));
				GTreeCutCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		if (State->WaitStartedAt <= 0.0)
		{
			State->WaitStartedAt = Now;
		}
		if (Now < State->NextActionAt)
		{
			return true;
		}

		if (State->Phase == 0)
		{
			AFragment2DSourceActor* Tree =
				FindNearestUncutTree(*World);
			if (!Tree)
			{
				if (Now - State->WaitStartedAt < 30.0)
				{
					return true;
				}
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Tree cut capture timed out waiting for a generated tree."));
				GTreeCutCapturePending = false;
				RestoreTreeCutCamera(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExit(false);
				}
				return false;
			}

			State->World = World;
			State->Tree = Tree;
			State->OutputDirectory = FPaths::Combine(
				FPaths::ScreenShotDir(),
				TEXT("MatterFluxTreeCut"),
				FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
			IFileManager::Get().MakeDirectory(
				*State->OutputDirectory,
				true);

			if (APlayerController* PlayerController =
				World->GetFirstPlayerController())
			{
				if (APawn* Pawn = PlayerController->GetPawn())
				{
					const FBox Bounds =
						Tree->GetComponentsBoundingBox(true);
					const FVector CameraFocus =
						Bounds.IsValid
							? Bounds.GetCenter()
							: Tree->GetActorLocation();
					FVector PawnLocation =
						CameraFocus + FVector(-260.0f, -260.0f, 0.0f);
					PawnLocation.Z =
						Bounds.IsValid
							? Bounds.Min.Z + 100.0f
							: PawnLocation.Z;
					Pawn->SetActorLocation(
						PawnLocation,
						false,
						nullptr,
						ETeleportType::TeleportPhysics);

					if (AMatterFluxCharacter* Character =
						Cast<AMatterFluxCharacter>(Pawn))
					{
						State->Character = Character;
						if (Character->CameraBoom)
						{
							State->OriginalCameraArmLength =
								Character->CameraBoom->TargetArmLength;
							State->OriginalCameraTargetOffset =
								Character->CameraBoom->TargetOffset;
							Character->CameraBoom->TargetArmLength =
								1200.0f;
							Character->CameraBoom->TargetOffset =
								CameraFocus - PawnLocation;
						}
						if (Character->CharacterVisual)
						{
							State->bOriginalCharacterVisualVisible =
								Character->CharacterVisual->IsVisible();
							Character->CharacterVisual->SetVisibility(
								false,
								true);
						}
					}
				}
			}

			UE_LOG(
				LogMatterFlux,
				Display,
				TEXT("Tree cut capture target=%s output=%s"),
				*Tree->SourceId.ToString(EGuidFormats::Digits),
				*State->OutputDirectory);
			State->Phase = 1;
			State->NextActionAt = Now + 1.0;
			return true;
		}

		if (!State->Tree.IsValid())
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Tree cut capture target disappeared before the cut."));
			GTreeCutCapturePending = false;
			RestoreTreeCutCamera(*State);
			if (State->bQuitAfterCapture)
			{
				FPlatformMisc::RequestExit(false);
			}
			return false;
		}

		AFragment2DSourceActor* Tree = State->Tree.Get();
		switch (State->Phase)
		{
		case 1:
			RequestTreeCutScreenshot(*State, TEXT("01_BeforeCut.png"));
			State->NextActionAt = Now + 0.8;
			break;
		case 2:
		{
			FFragmentDamageEvent Event;
			Event.SourceId = Tree->SourceId;
			Event.BaseRevision = Tree->Revision;
			Event.DamageShape.Type = EFragmentDamageShapeType::Line;
			Event.DamageShape.WorldTransform = Tree->GetActorTransform();
			Event.DamageShape.Extents.X =
				Tree->GetCellSize()
					* static_cast<float>(Tree->GetMaskWidth() + 2);
			Event.DamageShape.Thickness =
				Tree->GetCellSize() * 1.1f;
			Event.DamagePower = 500.0f;
			Event.EventSeed = State->EventSeed;
			UFragmentSimulationSubsystem* Subsystem =
				World->GetSubsystem<UFragmentSimulationSubsystem>();
			if (!Subsystem
				|| !Subsystem->RequestFragmentDamage(Tree, Event))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Tree cut capture could not commit the tree cut."));
				GTreeCutCapturePending = false;
				RestoreTreeCutCamera(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExit(false);
				}
				return false;
			}
			State->NextActionAt = Now + 0.12;
			break;
		}
		case 3:
			RequestTreeCutScreenshot(*State, TEXT("02_JustDetached.png"));
			State->NextActionAt = Now + 0.45;
			break;
		case 4:
			RequestTreeCutScreenshot(*State, TEXT("03_FallingEarly.png"));
			State->NextActionAt = Now + 0.75;
			break;
		case 5:
			RequestTreeCutScreenshot(*State, TEXT("04_FallingLate.png"));
			State->NextActionAt = Now + 1.25;
			break;
		case 6:
			RequestTreeCutScreenshot(*State, TEXT("05_AfterImpact.png"));
			State->NextActionAt = Now + 1.0;
			break;
		default:
			GTreeCutCapturePending = false;
			RestoreTreeCutCamera(*State);
			UE_LOG(
				LogMatterFlux,
				Display,
				TEXT("Tree cut capture sequence complete: %s"),
				*State->OutputDirectory);
			if (State->bQuitAfterCapture)
			{
				FPlatformMisc::RequestExit(false);
			}
			return false;
		}
		++State->Phase;
		return true;
	}

	void QueueVisualCapture(
		const TArray<FString>& Args,
		UWorld*)
	{
		if (GVisualCapturePending)
		{
			UE_LOG(
				LogMatterFlux,
				Warning,
				TEXT("mf.Visual.Capture ignored because a capture is already pending."));
			return;
		}

		const double DelaySeconds = Args.Num() > 0
			? FMath::Clamp(FCString::Atod(*Args[0]), 0.0, 60.0)
			: 5.0;
		const int32 Multiplier = Args.Num() > 1
			? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 4)
			: 2;
		const bool bQuitAfterCapture =
			Args.Num() > 2 && FCString::Atoi(*Args[2]) != 0;
		const int32 RequestedMapSeed = Args.Num() > 3
			? FMath::Max(FCString::Atoi(*Args[3]), 0)
			: 0;
		const bool bOpenMagicWorkbench =
			Args.Num() > 4 && FCString::Atoi(*Args[4]) != 0;
		const bool bOpenWandBackpack =
			Args.Num() > 5 && FCString::Atoi(*Args[5]) != 0;
		const int32 WorkbenchEquipmentSlot = Args.Num() > 6
			? FMath::Clamp(FCString::Atoi(*Args[6]), 0, 3)
			: 0;
		const FString SpellTreePreset = Args.Num() > 7
			? Args[7].TrimStartAndEnd()
			: FString();
		const FString WorkbenchPage = Args.Num() > 8
			? Args[8].TrimStartAndEnd().ToLower()
			: (bOpenWandBackpack ? TEXT("wand") : TEXT("spell"));

		GVisualCapturePending = true;
		const double QueuedAt = FPlatformTime::Seconds();
		const TSharedRef<double> ViewportReadyAt =
			MakeShared<double>(-1.0);
		const TSharedRef<bool> bSeedApplied =
			MakeShared<bool>(RequestedMapSeed == 0);
		const TSharedRef<bool> bWorkbenchOpened =
			MakeShared<bool>(!bOpenMagicWorkbench);
		const TSharedRef<bool> bSpellTreePresetApplied =
			MakeShared<bool>(SpellTreePreset.IsEmpty());
		const TSharedRef<FString> SpellTreePresetError =
			MakeShared<FString>();
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[
					DelaySeconds,
					Multiplier,
					bQuitAfterCapture,
					RequestedMapSeed,
					bOpenMagicWorkbench,
					bOpenWandBackpack,
					WorkbenchEquipmentSlot,
					SpellTreePreset,
					WorkbenchPage,
					ViewportReadyAt,
					bSeedApplied,
					bWorkbenchOpened,
					bSpellTreePresetApplied,
					SpellTreePresetError,
					QueuedAt](
					float)
				{
					UWorld* World =
						GEngine && GEngine->GameViewport
							? GEngine->GameViewport->GetWorld()
							: nullptr;
					if (!World || !World->IsGameWorld())
					{
						if (FPlatformTime::Seconds() - QueuedAt >= 30.0)
						{
							UE_LOG(
								LogMatterFlux,
								Error,
								TEXT("Visual capture timed out waiting for a game viewport."));
							GVisualCapturePending = false;
							if (bQuitAfterCapture)
							{
								FPlatformMisc::RequestExitWithStatus(false, 4);
							}
							return false;
						}
						return true;
					}

					const double Now = FPlatformTime::Seconds();
					if (!*bSeedApplied)
					{
						AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
						for (TActorIterator<AMatterFluxPlayableWorldActor> It(World);
							It;
							++It)
						{
							PlayableWorld = *It;
							break;
						}
						if (!PlayableWorld)
						{
							if (Now - QueuedAt >= 30.0)
							{
								UE_LOG(
									LogMatterFlux,
									Error,
									TEXT("Visual capture timed out waiting for a playable world actor to apply map seed %d."),
									RequestedMapSeed);
								GVisualCapturePending = false;
								if (bQuitAfterCapture)
								{
									FPlatformMisc::RequestExitWithStatus(
										false,
										4);
								}
								return false;
							}
							return true;
						}
						if (PlayableWorld->GetMapSeed() != RequestedMapSeed)
						{
							PlayableWorld->Regenerate(RequestedMapSeed);
						}
						*bSeedApplied = true;
						*ViewportReadyAt = -1.0;
					}

					if (!*bWorkbenchOpened)
					{
						AMatterFluxPlayerController* PlayerController =
							Cast<AMatterFluxPlayerController>(
								World->GetFirstPlayerController());
						if (!PlayerController || !PlayerController->IsLocalController())
						{
							if (Now - QueuedAt >= 30.0)
							{
								UE_LOG(
									LogMatterFlux,
									Error,
									TEXT("Visual capture timed out waiting for a local MatterFlux player controller."));
								GVisualCapturePending = false;
								if (bQuitAfterCapture)
								{
									FPlatformMisc::RequestExitWithStatus(false, 4);
								}
								return false;
							}
							return true;
						}
						if (!*bSpellTreePresetApplied)
						{
							if (!TryApplySpellTreeVisualPreset(
								*PlayerController,
								WorkbenchEquipmentSlot,
								SpellTreePreset,
								*SpellTreePresetError))
							{
								if (Now - QueuedAt >= 30.0)
								{
									UE_LOG(
										LogMatterFlux,
										Error,
										TEXT("Visual capture could not apply spell-tree preset '%s': %s"),
										*SpellTreePreset,
										**SpellTreePresetError);
									GVisualCapturePending = false;
									if (bQuitAfterCapture)
									{
										FPlatformMisc::RequestExitWithStatus(false, 4);
									}
									return false;
								}
								return true;
							}
							*bSpellTreePresetApplied = true;
							*ViewportReadyAt = -1.0;
						}
						if (!PlayerController->IsMagicWorkbenchOpen())
						{
							PlayerController->ToggleMagicWorkbench();
						}
						if (!PlayerController->ShowMagicWorkbenchNamedPage(
							WorkbenchPage))
						{
							UE_LOG(LogMatterFlux, Error,
								TEXT("Unknown visual capture workbench page '%s'."),
								*WorkbenchPage);
							GVisualCapturePending = false;
							return false;
						}
						if ((WorkbenchPage == TEXT("spell")
								|| WorkbenchPage == TEXT("wand"))
							&& !PlayerController->SelectMagicWorkbenchEquipmentSlot(
								WorkbenchEquipmentSlot))
						{
							return true;
						}
						*bWorkbenchOpened = true;
						*ViewportReadyAt = -1.0;
					}

					if (*ViewportReadyAt < 0.0)
					{
						*ViewportReadyAt = Now;
					}
					if (Now - *ViewportReadyAt < DelaySeconds)
					{
						return true;
					}

					if (bOpenMagicWorkbench)
					{
						const FString ScreenshotPath = FPaths::Combine(
							FPaths::ScreenShotDir(),
							FString::Printf(
								TEXT("MatterFluxMagicWorkbench-%s.png"),
								*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
						FScreenshotRequest::RequestScreenshot(
							ScreenshotPath,
							true,
							false,
							false);
						UE_LOG(
							LogMatterFlux,
							Display,
							TEXT("Requested magic workbench UI screenshot: %s"),
							*ScreenshotPath);
						if (bQuitAfterCapture)
						{
							FTSTicker::GetCoreTicker().AddTicker(
								FTickerDelegate::CreateLambda(
									[](float)
									{
										FPlatformMisc::RequestExit(false);
										return false;
									}),
								3.0f);
						}
						else
						{
							GVisualCapturePending = false;
						}
						return false;
					}

					const FString Command =
						FString::Printf(TEXT("HighResShot %d"), Multiplier);
					UE_LOG(
						LogMatterFlux,
						Display,
						TEXT("Executing delayed visual capture: %s (output: %s)"),
						*Command,
						*FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir()));
					const bool bAccepted = GEngine->GameViewport->Exec(
						World,
						*Command,
						*GLog);
					if (!bAccepted)
					{
						UE_LOG(
							LogMatterFlux,
							Error,
							TEXT("Delayed visual capture command was not accepted by the game viewport."));
						GVisualCapturePending = false;
						return false;
					}

					if (bQuitAfterCapture)
					{
						FTSTicker::GetCoreTicker().AddTicker(
							FTickerDelegate::CreateLambda(
								[](float)
								{
									FPlatformMisc::RequestExit(false);
									return false;
								}),
							5.0f);
					}
					else
					{
						GVisualCapturePending = false;
					}
					return false;
				}),
			0.25f);

		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("Queued visual capture: delay=%.2fs multiplier=%d quit=%s map-seed=%d workbench=%s page=%s equipment-slot=%d spell-tree-preset=%s"),
			DelaySeconds,
			Multiplier,
			bQuitAfterCapture ? TEXT("true") : TEXT("false"),
			RequestedMapSeed,
			bOpenMagicWorkbench ? TEXT("true") : TEXT("false"),
			*WorkbenchPage,
			WorkbenchEquipmentSlot,
			SpellTreePreset.IsEmpty() ? TEXT("current") : *SpellTreePreset);
	}

	FAutoConsoleCommandWithWorldAndArgs GVisualCaptureCommand(
		TEXT("mf.Visual.Capture"),
		TEXT("Capture after the game viewport is ready: mf.Visual.Capture [delay=5] [multiplier=2] [quit=0] [map-seed=0] [open-workbench=0] [legacy-wand-page=0] [equipment-slot=0] [spell-tree-preset] [page=spell|wand|item|quest]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueVisualCapture));

	void QueueOccludedPlayerCapture(
		const TArray<FString>& Args,
		UWorld*)
	{
		if (GOccludedPlayerCapturePending)
		{
			UE_LOG(LogMatterFlux, Warning,
				TEXT("mf.Visual.CaptureOccludedPlayer ignored because a capture is already pending."));
			return;
		}

		const double DelaySeconds = Args.Num() > 0
			? FMath::Clamp(FCString::Atod(*Args[0]), 0.1, 15.0)
			: 1.0;
		const bool bQuitAfterCapture =
			Args.Num() <= 1 || FCString::Atoi(*Args[1]) != 0;
		const double QueuedAt = FPlatformTime::Seconds();
		const TSharedRef<double> OccluderReadyAt = MakeShared<double>(-1.0);
		const TSharedRef<TWeakObjectPtr<AStaticMeshActor>> Occluder =
			MakeShared<TWeakObjectPtr<AStaticMeshActor>>();

		GOccludedPlayerCapturePending = true;
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[DelaySeconds,
				 bQuitAfterCapture,
				 QueuedAt,
				 OccluderReadyAt,
				 Occluder](float)
				{
					UWorld* World = GEngine && GEngine->GameViewport
						? GEngine->GameViewport->GetWorld()
						: nullptr;
					AMatterFluxCharacter* Character = World
						? Cast<AMatterFluxCharacter>(
							World->GetFirstPlayerController()
								? World->GetFirstPlayerController()->GetPawn()
								: nullptr)
						: nullptr;
					if (!World || !World->IsGameWorld() || !Character)
					{
						if (FPlatformTime::Seconds() - QueuedAt >= 30.0)
						{
							UE_LOG(LogMatterFlux, Error,
								TEXT("Occluded-player capture timed out waiting for the local character."));
							GOccludedPlayerCapturePending = false;
							if (bQuitAfterCapture)
							{
								FPlatformMisc::RequestExitWithStatus(false, 4);
							}
							return false;
						}
						return true;
					}

					const double Now = FPlatformTime::Seconds();
					if (!Occluder->IsValid())
					{
						const FVector CameraLocation = Character->FollowCamera
							? Character->FollowCamera->GetComponentLocation()
							: FVector::ZeroVector;
						const FVector LowerBodyTarget =
							Character->GetActorLocation() - FVector(0.0f, 0.0f, 28.0f);
						const FVector TowardCamera =
							(CameraLocation - LowerBodyTarget).GetSafeNormal();
						if (TowardCamera.IsNearlyZero())
						{
							return true;
						}

						UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
							nullptr,
							TEXT("/Engine/BasicShapes/Cube.Cube"));
						UMaterialInterface* PaletteMaterial =
							LoadObject<UMaterialInterface>(
								nullptr,
								TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette"));
						if (!CubeMesh || !PaletteMaterial)
						{
							UE_LOG(LogMatterFlux, Error,
								TEXT("Occluded-player capture could not load its cube or palette material."));
							GOccludedPlayerCapturePending = false;
							if (bQuitAfterCapture)
							{
								FPlatformMisc::RequestExitWithStatus(false, 4);
							}
							return false;
						}

						FActorSpawnParameters SpawnParameters;
						SpawnParameters.Name = MakeUniqueObjectName(
							World,
							AStaticMeshActor::StaticClass(),
							TEXT("OutlineCaptureOccluder"));
						AStaticMeshActor* SpawnedOccluder =
							World->SpawnActor<AStaticMeshActor>(
								LowerBodyTarget + TowardCamera * 420.0f,
								Character->FollowCamera
									? Character->FollowCamera->GetComponentRotation()
									: FRotator::ZeroRotator,
								SpawnParameters);
						if (!SpawnedOccluder)
						{
							return true;
						}

						UStaticMeshComponent* Mesh =
							SpawnedOccluder->GetStaticMeshComponent();
						Mesh->SetMobility(EComponentMobility::Movable);
						Mesh->SetStaticMesh(CubeMesh);
						Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						Mesh->SetGenerateOverlapEvents(false);
						Mesh->SetCanEverAffectNavigation(false);
						Mesh->SetCastShadow(false);
						Mesh->SetRenderCustomDepth(false);
						Mesh->SetCustomDepthStencilValue(0);
						SpawnedOccluder->SetActorScale3D(FVector(0.06f, 0.90f, 0.42f));

						UMaterialInstanceDynamic* Material =
							UMaterialInstanceDynamic::Create(
								PaletteMaterial,
								SpawnedOccluder);
						Material->SetVectorParameterValue(
							TEXT("Color"),
							FLinearColor(0.62f, 0.72f, 0.82f, 1.0f));
						Mesh->SetMaterial(0, Material);
						*Occluder = SpawnedOccluder;
						*OccluderReadyAt = Now;
						UE_LOG(LogMatterFlux, Display,
							TEXT("Placed deterministic lower-body occluder for outline capture."));
						return true;
					}

					if (Now - *OccluderReadyAt < DelaySeconds)
					{
						const FVector LowerBodyTarget =
							Character->GetActorLocation() - FVector(0.0f, 0.0f, 28.0f);
						const FVector CameraLocation = Character->FollowCamera
							? Character->FollowCamera->GetComponentLocation()
							: FVector::ZeroVector;
						const FVector TowardCamera =
							(CameraLocation - LowerBodyTarget).GetSafeNormal();
						if (!TowardCamera.IsNearlyZero())
						{
							Occluder->Get()->SetActorLocation(
								LowerBodyTarget + TowardCamera * 420.0f);
							if (Character->FollowCamera)
							{
								Occluder->Get()->SetActorRotation(
									Character->FollowCamera->GetComponentRotation());
							}
						}
						return true;
					}

					FVector2D TargetScreen;
					FVector2D OccluderScreen;
					const FVector LowerBodyTarget =
						Character->GetActorLocation() - FVector(0.0f, 0.0f, 28.0f);
					APlayerController* PlayerController =
						World->GetFirstPlayerController();
					if (PlayerController)
					{
						PlayerController->ProjectWorldLocationToScreen(
							LowerBodyTarget,
							TargetScreen);
						PlayerController->ProjectWorldLocationToScreen(
							Occluder->Get()->GetActorLocation(),
							OccluderScreen);
						UE_LOG(LogMatterFlux, Display,
							TEXT("Occluded-player capture projection: target=(%.1f, %.1f) occluder=(%.1f, %.1f) delta=%.2fpx"),
							TargetScreen.X,
							TargetScreen.Y,
							OccluderScreen.X,
							OccluderScreen.Y,
							FVector2D::Distance(TargetScreen, OccluderScreen));
					}

					const FString ScreenshotPath = FPaths::Combine(
						FPaths::ScreenShotDir(),
						FString::Printf(
							TEXT("MatterFluxOccludedPlayerOutline-%s.png"),
							*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
					FScreenshotRequest::RequestScreenshot(
						ScreenshotPath,
						true,
						false,
						false);
					UE_LOG(LogMatterFlux, Display,
						TEXT("Requested occluded-player outline screenshot: %s"),
						*ScreenshotPath);

					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[Occluder, bQuitAfterCapture](float)
							{
								if (Occluder->IsValid())
								{
									Occluder->Get()->Destroy();
								}
								GOccludedPlayerCapturePending = false;
								if (bQuitAfterCapture)
								{
									FPlatformMisc::RequestExit(false);
								}
								return false;
							}),
						3.0f);
					return false;
				}),
			0.25f);

		UE_LOG(LogMatterFlux, Display,
			TEXT("Queued occluded-player outline capture: delay=%.2fs quit=%s"),
			DelaySeconds,
			bQuitAfterCapture ? TEXT("true") : TEXT("false"));
	}

	FAutoConsoleCommandWithWorldAndArgs GOccludedPlayerCaptureCommand(
		TEXT("mf.Visual.CaptureOccludedPlayer"),
		TEXT("Capture the player partially hidden by a deterministic opaque blocker: mf.Visual.CaptureOccludedPlayer [delay=1] [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueOccludedPlayerCapture));

	void QueueShellCapture(
		const TArray<FString>& Args,
		UWorld*)
	{
		if (GShellCapturePending)
		{
			UE_LOG(LogMatterFlux, Warning,
				TEXT("mf.UI.Capture ignored because a shell capture is already pending."));
			return;
		}
		const FString RequestedView = Args.Num() > 0
			? Args[0].TrimStartAndEnd().ToLower()
			: TEXT("start");
		const double DelaySeconds = Args.Num() > 1
			? FMath::Clamp(FCString::Atod(*Args[1]), 0.1, 15.0)
			: 1.0;
		const bool bQuitAfterCapture = Args.Num() <= 2
			|| FCString::Atoi(*Args[2]) != 0;
		const bool bKnownView = RequestedView == TEXT("start")
			|| RequestedView == TEXT("single")
			|| RequestedView == TEXT("multiplayer")
			|| RequestedView == TEXT("create")
			|| RequestedView == TEXT("join")
			|| RequestedView == TEXT("settings")
			|| RequestedView == TEXT("save")
			|| RequestedView == TEXT("load")
			|| RequestedView == TEXT("progress")
			|| RequestedView == TEXT("load-progress");
		if (!bKnownView)
		{
			UE_LOG(LogMatterFlux, Error,
				TEXT("Unknown mf.UI.Capture view '%s'. Expected start, single, multiplayer, create, join, settings, save, load, progress, or load-progress."),
				*RequestedView);
			return;
		}

		GShellCapturePending = true;
		const double QueuedAt = FPlatformTime::Seconds();
		const TSharedRef<double> ViewReadyAt = MakeShared<double>(-1.0);
		const TSharedRef<bool> bConfigured = MakeShared<bool>(false);
		const TSharedRef<int32> LoadCapturePhase = MakeShared<int32>(0);
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([
				RequestedView,
				DelaySeconds,
				bQuitAfterCapture,
				QueuedAt,
				ViewReadyAt,
				bConfigured,
				LoadCapturePhase](float)
			{
				UWorld* World = GEngine && GEngine->GameViewport
					? GEngine->GameViewport->GetWorld()
					: nullptr;
				AMatterFluxPlayerController* Controller = World
					? Cast<AMatterFluxPlayerController>(
						World->GetFirstPlayerController())
					: nullptr;
				const double Now = FPlatformTime::Seconds();
				if (!World || !World->IsGameWorld()
					|| !Controller || !Controller->IsLocalController())
				{
					if (Now - QueuedAt >= 30.0)
					{
						UE_LOG(LogMatterFlux, Error,
							TEXT("Shell capture timed out waiting for the local game world."));
						GShellCapturePending = false;
						if (bQuitAfterCapture)
						{
							FPlatformMisc::RequestExitWithStatus(false, 4);
						}
						return false;
					}
					return true;
				}

				if (!*bConfigured)
				{
					if (RequestedView == TEXT("start"))
					{
						Controller->ShowStartMenu();
					}
					else if (RequestedView == TEXT("single"))
					{
						Controller->ShowSinglePlayerMenu();
					}
					else if (RequestedView == TEXT("multiplayer"))
					{
						Controller->ShowMultiplayerMenu();
					}
					else if (RequestedView == TEXT("create"))
					{
						Controller->ShowCreateRoomMenu();
					}
					else if (RequestedView == TEXT("join"))
					{
						Controller->ShowJoinRoomMenu();
					}
					else if (RequestedView == TEXT("settings"))
					{
						Controller->ShowSettingsMenu();
					}
					else if (RequestedView == TEXT("save"))
					{
						Controller->ShowSaveMenu();
					}
					else if (RequestedView == TEXT("load"))
					{
						Controller->ShowLoadMenu();
					}
					else if (RequestedView == TEXT("progress"))
					{
						UMatterFluxSaveSubsystem* Save =
							World->GetGameInstance()
								? World->GetGameInstance()->GetSubsystem<
									UMatterFluxSaveSubsystem>()
								: nullptr;
						if (!Save || !Save->RequestNewGame(Controller, 20260803))
						{
							if (Now - QueuedAt < 30.0)
							{
								return true;
							}
							UE_LOG(LogMatterFlux, Error,
								TEXT("Shell progress capture could not start world generation."));
							GShellCapturePending = false;
							if (bQuitAfterCapture)
							{
								FPlatformMisc::RequestExitWithStatus(false, 4);
							}
							return false;
						}
						// Refresh after the operation starts so the shell creates the
						// progress overlay, then unpause standalone generation.
						Controller->ShowStartMenu();
						Controller->HandleShellStateChanged(true, true);
					}
					else
					{
						UMatterFluxSaveSubsystem* Save =
							World->GetGameInstance()
								? World->GetGameInstance()->GetSubsystem<
									UMatterFluxSaveSubsystem>()
								: nullptr;
						if (!Save)
						{
							return true;
						}
						if (*LoadCapturePhase == 0)
						{
							if (Save->IsBusy())
							{
								return true;
							}
							if (Save->GetOperation()
								== EMatterFluxSaveOperation::Failed)
							{
								Save->AcknowledgeResult();
							}
							if (Save->GetMostRecentSlotIndex() == INDEX_NONE)
							{
								if (!Save->RequestSave(Controller, 0))
								{
									if (Now - QueuedAt >= 45.0)
									{
										UE_LOG(LogMatterFlux, Error,
											TEXT("Shell load-progress capture could not create its temporary save."));
										GShellCapturePending = false;
										if (bQuitAfterCapture)
										{
											FPlatformMisc::RequestExitWithStatus(false, 4);
										}
										return false;
									}
									return true;
								}
								*LoadCapturePhase = 1;
								Controller->ShowSaveMenu();
								Controller->HandleShellStateChanged(true, true);
								return true;
							}
							*LoadCapturePhase = 2;
						}
						if (*LoadCapturePhase == 1)
						{
							if (Save->IsBusy())
							{
								return true;
							}
							if (Save->GetOperation()
								== EMatterFluxSaveOperation::Failed
								|| Save->GetMostRecentSlotIndex() == INDEX_NONE)
							{
								UE_LOG(LogMatterFlux, Error,
									TEXT("Shell load-progress temporary save failed: %s"),
									*Save->GetLastResultMessage());
								GShellCapturePending = false;
								if (bQuitAfterCapture)
								{
									FPlatformMisc::RequestExitWithStatus(false, 4);
								}
								return false;
							}
							if (Save->GetOperation()
								== EMatterFluxSaveOperation::Complete)
							{
								Save->AcknowledgeResult();
							}
							*LoadCapturePhase = 2;
						}
						if (*LoadCapturePhase == 2)
						{
							const int32 SlotIndex =
								Save->GetMostRecentSlotIndex();
							if (SlotIndex == INDEX_NONE
								|| !Save->RequestLoad(Controller, SlotIndex))
							{
								return true;
							}
							*LoadCapturePhase = 3;
							Controller->ShowLoadMenu();
							Controller->HandleShellStateChanged(true, true);
						}
					}
					*bConfigured = true;
					*ViewReadyAt = Now;
					return true;
				}

				const bool bCaptureActiveProgress =
					RequestedView == TEXT("progress")
					|| RequestedView == TEXT("load-progress");
				const double EffectiveDelay = bCaptureActiveProgress
					? FMath::Min(DelaySeconds, 0.35)
					: DelaySeconds;
				if (Now - *ViewReadyAt < EffectiveDelay)
				{
					return true;
				}
				const FString ScreenshotPath = FPaths::Combine(
					FPaths::ScreenShotDir(),
					FString::Printf(TEXT("MatterFluxShell-%s-%s.png"),
						*RequestedView,
						*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
				FScreenshotRequest::RequestScreenshot(
					ScreenshotPath,
					true,
					false,
					false);
				UE_LOG(LogMatterFlux, Display,
					TEXT("Requested shell UI screenshot: %s"),
					*ScreenshotPath);
				if (bQuitAfterCapture)
				{
					const float QuitDelay =
						RequestedView == TEXT("load-progress")
							? 8.0f
							: 2.0f;
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda([](float)
						{
							FPlatformMisc::RequestExit(false);
							return false;
						}),
						QuitDelay);
				}
				GShellCapturePending = false;
				return false;
			}),
			0.1f);

		UE_LOG(LogMatterFlux, Display,
			TEXT("Queued shell capture: view=%s delay=%.2fs quit=%s"),
			*RequestedView,
			DelaySeconds,
			bQuitAfterCapture ? TEXT("true") : TEXT("false"));
	}

	FAutoConsoleCommandWithWorldAndArgs GShellCaptureCommand(
		TEXT("mf.UI.Capture"),
		TEXT("Capture a shell page: mf.UI.Capture [start|single|multiplayer|create|join|settings|save|load|progress|load-progress] [delay-seconds=1] [quit-after=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueShellCapture));

	void QueueTreeCutCapture(
		const TArray<FString>& Args,
		UWorld*)
	{
		if (GTreeCutCapturePending)
		{
			UE_LOG(
				LogMatterFlux,
				Warning,
				TEXT("mf.Visual.TreeCutSequence ignored because a sequence is already pending."));
			return;
		}

		const TSharedRef<FTreeCutCaptureState> State =
			MakeShared<FTreeCutCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		if (Args.Num() > 0)
		{
			LexTryParseString(State->EventSeed, *Args[0]);
		}
		State->bQuitAfterCapture =
			Args.Num() <= 1 || FCString::Atoi(*Args[1]) != 0;
		GTreeCutCapturePending = true;
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[State](float)
				{
					return TickTreeCutCapture(State);
				}),
			0.05f);
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("Queued tree cut capture sequence: seed=%d quit=%s"),
			State->EventSeed,
			State->bQuitAfterCapture ? TEXT("true") : TEXT("false"));
	}

	FAutoConsoleCommandWithWorldAndArgs GTreeCutCaptureCommand(
		TEXT("mf.Visual.TreeCutSequence"),
		TEXT("Capture a real generated tree before, during, and after a cut: mf.Visual.TreeCutSequence [event-seed=1337] [quit-after=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueTreeCutCapture));

	struct FVisualStabilityCaptureState
	{
		double QueuedAt = 0.0;
		double SceneReadyAt = -1.0;
		double LastCaptureAt = -1.0;
		int32 FrameCount = 8;
		int32 CapturedFrames = 0;
		int32 MapSeed = 1337;
		double SettleSeconds = 8.0;
		double IntervalSeconds = 0.25;
		bool bQuitAfterCapture = true;
		FString OutputDirectory;
	};

	bool TickVisualStabilityCapture(
		const TSharedRef<FVisualStabilityCaptureState>& State)
	{
		const double Now = FPlatformTime::Seconds();
		UWorld* World = GEngine && GEngine->GameViewport
			? GEngine->GameViewport->GetWorld()
			: nullptr;
		if (!World || !World->IsGameWorld())
		{
			if (Now - State->QueuedAt < 45.0)
			{
				return true;
			}
			UE_LOG(LogMatterFlux, Error,
				TEXT("Visual stability capture timed out waiting for a game viewport."));
			GStabilityCapturePending = false;
			if (State->bQuitAfterCapture)
			{
				FPlatformMisc::RequestExitWithStatus(false, 4);
			}
			return false;
		}

		AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			PlayableWorld = *It;
			break;
		}
		if (!PlayableWorld)
		{
			return true;
		}
		if (PlayableWorld->GetMapSeed() != State->MapSeed)
		{
			PlayableWorld->Regenerate(State->MapSeed);
			State->SceneReadyAt = -1.0;
			return true;
		}
		if (PlayableWorld->IsGenerationInProgress()
			|| PlayableWorld->GetPendingFragmentSourceSpawnCount() > 0)
		{
			State->SceneReadyAt = -1.0;
			return true;
		}
		if (State->SceneReadyAt < 0.0)
		{
			State->SceneReadyAt = Now;
		}
		if (Now - State->SceneReadyAt < State->SettleSeconds
			|| FScreenshotRequest::IsScreenshotRequested()
			|| (State->LastCaptureAt >= 0.0
				&& Now - State->LastCaptureAt < State->IntervalSeconds))
		{
			return true;
		}

		IFileManager::Get().MakeDirectory(
			*State->OutputDirectory,
			true);
		const FString ScreenshotPath = FPaths::Combine(
			State->OutputDirectory,
			FString::Printf(
				TEXT("Frame_%03d.png"),
				State->CapturedFrames));
		FScreenshotRequest::RequestScreenshot(
			ScreenshotPath,
			false,
			false,
			false);
		State->LastCaptureAt = Now;
		++State->CapturedFrames;
		UE_LOG(LogMatterFlux, Display,
			TEXT("Requested visual stability frame %d/%d: %s"),
			State->CapturedFrames,
			State->FrameCount,
			*ScreenshotPath);
		if (State->CapturedFrames < State->FrameCount)
		{
			return true;
		}

		GStabilityCapturePending = false;
		if (State->bQuitAfterCapture)
		{
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([](float)
				{
					FPlatformMisc::RequestExit(false);
					return false;
				}),
				2.0f);
		}
		return false;
	}

	void QueueVisualStabilityCapture(
		const TArray<FString>& Args,
		UWorld*)
	{
		if (GStabilityCapturePending)
		{
			UE_LOG(LogMatterFlux, Warning,
				TEXT("mf.Visual.StabilitySequence ignored because a sequence is already pending."));
			return;
		}
		const TSharedRef<FVisualStabilityCaptureState> State =
			MakeShared<FVisualStabilityCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->FrameCount = Args.Num() > 0
			? FMath::Clamp(FCString::Atoi(*Args[0]), 2, 32)
			: 8;
		State->IntervalSeconds = Args.Num() > 1
			? FMath::Clamp(FCString::Atod(*Args[1]), 0.10, 2.0)
			: 0.25;
		State->MapSeed = Args.Num() > 2
			? FMath::Max(FCString::Atoi(*Args[2]), 1)
			: 1337;
		State->SettleSeconds = Args.Num() > 3
			? FMath::Clamp(FCString::Atod(*Args[3]), 0.0, 30.0)
			: 8.0;
		State->bQuitAfterCapture = Args.Num() <= 4
			|| FCString::Atoi(*Args[4]) != 0;
		State->OutputDirectory = FPaths::Combine(
			FPaths::ScreenShotDir(),
			TEXT("MatterFluxStability"),
			FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		GStabilityCapturePending = true;
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[State](float)
				{
					return TickVisualStabilityCapture(State);
				}),
			0.05f);
		UE_LOG(LogMatterFlux, Display,
			TEXT("Queued visual stability sequence: frames=%d interval=%.2fs seed=%d settle=%.2fs quit=%s output=%s"),
			State->FrameCount,
			State->IntervalSeconds,
			State->MapSeed,
			State->SettleSeconds,
			State->bQuitAfterCapture ? TEXT("true") : TEXT("false"),
			*State->OutputDirectory);
	}

	FAutoConsoleCommandWithWorldAndArgs GVisualStabilityCaptureCommand(
		TEXT("mf.Visual.StabilitySequence"),
		TEXT("Capture fixed-camera frames for flicker diagnosis: mf.Visual.StabilitySequence [frames=8] [interval=0.25] [map-seed=1337] [settle=8] [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueVisualStabilityCapture));
}
