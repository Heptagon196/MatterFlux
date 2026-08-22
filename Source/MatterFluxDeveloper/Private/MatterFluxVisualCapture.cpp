#include "CoreMinimal.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxGroundStateChunkActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "Game/MatterFluxTwoStoreyHouseActor.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "MatterFluxLog.h"
#include "IMatterFluxScriptRuntime.h"
#include "Game/MatterFluxCharacter.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Magic/MatterFluxMagicProjectile.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Material/MatterFluxMaterialWorld.h"
#include "Material/MatterFluxBuoyancyComponent.h"
#include "Material/MatterFluxLiquidBuoyancy.h"
#include "Material/MatterFluxCustomMap.h"
#include "Material/MatterFluxCustomMapPour.h"
#include "Misc/DateTime.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Save/MatterFluxSaveSubsystem.h"
#include "UnrealClient.h"

namespace
{
	bool GVisualCapturePending = false;
	bool GOccludedPlayerCapturePending = false;
	bool GTreeCutCapturePending = false;
	bool GTreeBatchCutCapturePending = false;
	bool GShellCapturePending = false;
	bool GStabilityCapturePending = false;
	bool GLiquidPoolCapturePending = false;
	bool GDeepLiquidWalkCapturePending = false;
	bool GLiquidDropCapturePending = false;
	bool GPhysicsPushCapturePending = false;
	bool GAcidReactionCapturePending = false;

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
		bool bBurnInspectionOnly = false;
		bool bRepeatedItemCut = false;
		bool bAccepted = false;
		bool bRequestedTreeMaterialization = false;
		FGuid PinnedSourceId;
		FGuid AggregateId;
		TArray<TWeakObjectPtr<AActor>> TemporarilyHiddenActors;
		TWeakObjectPtr<AMatterFluxCharacter> Character;
		TWeakObjectPtr<AActor> OriginalViewTarget;
		TWeakObjectPtr<ACameraActor> CaptureCamera;
		TWeakObjectPtr<AFragment2DActor> DetachedItem;
		float OriginalCameraArmLength = 0.0f;
		FVector OriginalCameraTargetOffset = FVector::ZeroVector;
		bool bOriginalCharacterVisualVisible = true;
		FVector CameraFocus = FVector::ZeroVector;
		FVector CameraFront = FVector::ForwardVector;
		FVector CameraRight = FVector::RightVector;
		float CaptureDistance = 560.0f;
		float CaptureOrthoWidth = 320.0f;
	};

	void PositionTreeCaptureCamera(
		const FTreeCutCaptureState& State,
		const FVector& HorizontalDirection,
		const float HeightRatio = 1.0f)
	{
		ACameraActor* Camera = State.CaptureCamera.Get();
		if (!Camera)
		{
			return;
		}
		const FVector Direction = FVector(
			HorizontalDirection.X,
			HorizontalDirection.Y,
			0.0f).GetSafeNormal();
		const FVector CameraLocation = State.CameraFocus
			+ Direction * State.CaptureDistance
			+ FVector::UpVector * State.CaptureDistance * HeightRatio;
		Camera->SetActorLocationAndRotation(
			CameraLocation,
			(State.CameraFocus - CameraLocation).Rotation());
		if (UCameraComponent* CameraComponent = Camera->GetCameraComponent())
		{
			CameraComponent->ProjectionMode = ECameraProjectionMode::Perspective;
			CameraComponent->FieldOfView = 48.0f;
			CameraComponent->bConstrainAspectRatio = false;
			CameraComponent->PostProcessBlendWeight = 1.0f;
			CameraComponent->PostProcessSettings.bOverride_AutoExposureMethod = true;
			CameraComponent->PostProcessSettings.AutoExposureMethod = AEM_Manual;
			CameraComponent->PostProcessSettings
				.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
			CameraComponent->PostProcessSettings
				.AutoExposureApplyPhysicalCameraExposure = false;
			CameraComponent->PostProcessSettings.bOverride_AutoExposureBias = true;
			CameraComponent->PostProcessSettings.AutoExposureBias = 0.0f;
			// 方位切换通过瞬移同一台相机完成。若沿用默认运动模糊，
			// 截图请求所在帧会把树干和树冠拉成径向薄片，导致视觉验收
			// 本身制造出不存在于稳定游戏镜头中的“书本形”伪影。
			CameraComponent->PostProcessSettings.bOverride_MotionBlurAmount = true;
			CameraComponent->PostProcessSettings.MotionBlurAmount = 0.0f;
			CameraComponent->PostProcessSettings.bOverride_BloomIntensity = true;
			CameraComponent->PostProcessSettings.BloomIntensity = 0.05f;
			CameraComponent->PostProcessSettings.bOverride_VignetteIntensity = true;
			CameraComponent->PostProcessSettings.VignetteIntensity = 0.0f;
		}
	}

	void HideActorForTreeCapture(
		FTreeCutCaptureState& State,
		AActor* Actor)
	{
		if (!IsValid(Actor) || Actor->IsHidden())
		{
			return;
		}
		State.TemporarilyHiddenActors.AddUnique(Actor);
		Actor->SetActorHiddenInGame(true);
	}

	void EnforceTreeCaptureIsolation(
		FTreeCutCaptureState& State,
		UWorld& World)
	{
		if (!State.AggregateId.IsValid())
		{
			return;
		}
		for (TActorIterator<AFragment2DSourceActor> It(&World); It; ++It)
		{
			if (It->AggregateId != State.AggregateId)
			{
				HideActorForTreeCapture(State, *It);
			}
		}
		for (TActorIterator<AMatterFluxCreatureActor> It(&World); It; ++It)
		{
			HideActorForTreeCapture(State, *It);
		}
		for (TActorIterator<AMatterFluxTwoStoreyHouseActor> It(&World); It; ++It)
		{
			HideActorForTreeCapture(State, *It);
		}
		HideActorForTreeCapture(State, State.Character.Get());
	}

	void RestoreTreeCutCamera(const FTreeCutCaptureState& State)
	{
		if (UWorld* World = State.World.Get())
		{
			if (APlayerController* PlayerController =
				World->GetFirstPlayerController())
			{
				if (State.OriginalViewTarget.IsValid())
				{
					PlayerController->SetViewTarget(
						State.OriginalViewTarget.Get());
				}
			}
		}
		if (State.CaptureCamera.IsValid())
		{
			State.CaptureCamera->Destroy();
		}
		if (UWorld* World = State.World.Get())
		{
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				It->SetFragmentSourceDebugIsolatedAggregate(FGuid());
				It->SetFragmentSourceStreamingPinned(
					State.PinnedSourceId,
					false);
				break;
			}
		}
		for (const TWeakObjectPtr<AActor>& HiddenActor
			: State.TemporarilyHiddenActors)
		{
			if (HiddenActor.IsValid())
			{
				HiddenActor->SetActorHiddenInGame(false);
			}
		}
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

	void LogTreeCarrierPhysics(UWorld& World, const TCHAR* Phase)
	{
		for (TActorIterator<AFragment2DActor> It(&World); It; ++It)
		{
			AFragment2DActor* Carrier = *It;
			if (!IsValid(Carrier)
				|| !Carrier->ActorHasTag(TEXT("MatterFluxFragment")))
			{
				continue;
			}
			UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(
				Carrier->GetRootComponent());
			if (!Body)
			{
				continue;
			}
			UE_LOG(
				LogMatterFlux,
				Display,
				TEXT("Tree carrier [%s]: simulate=%s collision=%d location=%s rotation=%s linear=%s angular=%s"),
				Phase,
				Body->IsSimulatingPhysics()
					? TEXT("true")
					: TEXT("false"),
				static_cast<int32>(Body->GetCollisionEnabled()),
				*Carrier->GetActorLocation().ToCompactString(),
				*Carrier->GetActorRotation().ToCompactString(),
				*Body->GetPhysicsLinearVelocity().ToCompactString(),
				*Body->GetPhysicsAngularVelocityInDegrees().ToCompactString());
			break;
		}
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
		// GameMode/PlayerController 会在 pawn 传送后的下一帧重新自动管理
		// ViewTarget；测试相机必须在整个序列中保持为活动视角。
		if (State->CaptureCamera.IsValid())
		{
			if (APlayerController* PlayerController =
				World->GetFirstPlayerController())
			{
				PlayerController->SetPause(false);
				if (PlayerController->GetViewTarget()
					!= State->CaptureCamera.Get())
				{
					PlayerController->SetViewTarget(
						State->CaptureCamera.Get());
				}
			}
		}
		else if (APlayerController* PlayerController =
			World->GetFirstPlayerController())
		{
			PlayerController->SetPause(false);
		}
		EnforceTreeCaptureIsolation(*State, *World);
		if (Now < State->NextActionAt)
		{
			return true;
		}

		if (State->Phase == 0)
		{
			AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
			FVector Focus = FVector::ZeroVector;
			if (const APlayerController* PlayerController =
				World->GetFirstPlayerController())
			{
				if (const APawn* Pawn = PlayerController->GetPawn())
				{
					Focus = Pawn->GetActorLocation();
				}
			}
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				if (!It->IsGenerationInProgress()
					&& It->GetCachedFragmentSourceCount() > 0)
				{
					PlayableWorld = *It;
					break;
				}
			}
			FGuid AggregateId;
			FGuid RootSourceId;
			FBox Bounds(ForceInit);
			FTransform RootWorldTransform = FTransform::Identity;
			if (!PlayableWorld
				|| !PlayableWorld->FindNearestTreeAggregateForVisualInspection(
					Focus,
					AggregateId,
					RootSourceId,
					Bounds,
					RootWorldTransform))
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
			State->PinnedSourceId = RootSourceId;
			State->AggregateId = AggregateId;
			State->OutputDirectory = FPaths::Combine(
				FPaths::ScreenShotDir(),
				State->bBurnInspectionOnly
					? TEXT("MatterFluxTreeBurn")
					: State->bRepeatedItemCut
						? TEXT("MatterFluxTreeItemCut")
					: TEXT("MatterFluxTreeCut"),
				FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
			IFileManager::Get().MakeDirectory(
				*State->OutputDirectory,
				true);

			if (APlayerController* PlayerController =
				World->GetFirstPlayerController())
			{
				State->OriginalViewTarget = PlayerController->GetViewTarget();
				if (APawn* Pawn = PlayerController->GetPawn())
				{
					const FVector CameraFocus =
						Bounds.IsValid
							? Bounds.GetCenter()
							: RootWorldTransform.GetLocation();
					State->CameraFocus = CameraFocus;
					State->CameraFront =
						RootWorldTransform.GetUnitAxis(EAxis::Y);
					State->CameraRight =
						RootWorldTransform.GetUnitAxis(EAxis::X);
					State->CaptureDistance = Bounds.IsValid
						? FMath::Clamp(
							Bounds.GetExtent().Size() * 3.0f,
							430.0f,
							680.0f)
						: 560.0f;
					State->CaptureOrthoWidth = Bounds.IsValid
						? FMath::Clamp(
							Bounds.GetSize().Z * 1.65f,
							260.0f,
							420.0f)
						: 320.0f;
					const FVector CaptureLocation = CameraFocus
						+ State->CameraFront * State->CaptureDistance
						+ FVector::UpVector * State->CaptureDistance;
					ACameraActor* CaptureCamera =
						World->SpawnActor<ACameraActor>(
							CaptureLocation,
							(CameraFocus - CaptureLocation).Rotation());
					if (CaptureCamera)
					{
						State->CaptureCamera = CaptureCamera;
						PositionTreeCaptureCamera(
							*State,
							State->CameraFront);
						PlayerController->SetViewTarget(CaptureCamera);
					}
					FVector PawnLocation =
						CameraFocus + FVector(-180.0f, -180.0f, 0.0f);
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
								Bounds.IsValid
									? FMath::Clamp(
										Bounds.GetExtent().Size() * 3.5f,
										480.0f,
										760.0f)
									: 620.0f;
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

			// Keep terrain and the selected aggregate, but remove all other
			// decoration geometry from the visual-QA frame. This prevents nearby
			// trees from hiding silhouette, underside and material-overlap defects.
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				It->SetFragmentSourceDebugIsolatedAggregate(AggregateId);
				break;
			}
			EnforceTreeCaptureIsolation(*State, *World);

			UE_LOG(
				LogMatterFlux,
				Display,
				TEXT("Tree cut capture target=%s output=%s"),
				*RootSourceId.ToString(EGuidFormats::Digits),
				*State->OutputDirectory);
			State->Phase = 1;
			State->NextActionAt = Now + 1.0;
			return true;
		}

		switch (State->Phase)
		{
		case 1:
			PositionTreeCaptureCamera(*State, State->CameraFront);
			RequestTreeCutScreenshot(*State, TEXT("01_StaticFront.png"));
			State->NextActionAt = Now + 0.6;
			break;
		case 2:
			PositionTreeCaptureCamera(
				*State,
				(State->CameraFront + State->CameraRight).GetSafeNormal());
			RequestTreeCutScreenshot(*State, TEXT("02_StaticFrontRight45.png"));
			State->NextActionAt = Now + 0.6;
			break;
		case 3:
			PositionTreeCaptureCamera(*State, State->CameraRight);
			RequestTreeCutScreenshot(*State, TEXT("03_StaticRight.png"));
			State->NextActionAt = Now + 0.6;
			break;
		case 4:
			PositionTreeCaptureCamera(
				*State,
				(-State->CameraFront + State->CameraRight).GetSafeNormal());
			RequestTreeCutScreenshot(*State, TEXT("04_StaticBackRight45.png"));
			State->NextActionAt = Now + 0.6;
			break;
		case 5:
			PositionTreeCaptureCamera(*State, -State->CameraFront);
			RequestTreeCutScreenshot(*State, TEXT("05_StaticBack.png"));
			State->NextActionAt = Now + 0.6;
			break;
		case 6:
			PositionTreeCaptureCamera(
				*State,
				(State->CameraFront - State->CameraRight).GetSafeNormal());
			RequestTreeCutScreenshot(*State, TEXT("06_StaticFrontLeft45.png"));
			State->NextActionAt = Now + 0.6;
			break;
		case 7:
			PositionTreeCaptureCamera(*State, -State->CameraRight);
			RequestTreeCutScreenshot(*State, TEXT("07_StaticLeft.png"));
			State->NextActionAt = Now + 0.6;
			break;
		case 8:
			PositionTreeCaptureCamera(
				*State,
				(State->CameraFront + State->CameraRight).GetSafeNormal(),
				-0.04f);
			RequestTreeCutScreenshot(
				*State,
				TEXT("08_LowFrontRight45_RootAndUnderside.png"));
			State->NextActionAt = Now + 0.6;
			break;
		case 9:
		{
			AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				PlayableWorld = *It;
				break;
			}
			if (!PlayableWorld)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Tree inspection lost its playable world."));
				GTreeCutCapturePending = false;
				RestoreTreeCutCamera(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExit(false);
				}
				return false;
			}
			if (State->bBurnInspectionOnly)
			{
				const FVector ViewDirection =
					(State->CameraFront + State->CameraRight).GetSafeNormal();
				const FVector IgnitionPoint = State->CameraFocus
					+ FVector::UpVector * State->CaptureOrthoWidth * 0.22f
					+ ViewDirection * State->CaptureOrthoWidth * 0.16f;
				if (!PlayableWorld->IgniteLogicalFragmentAggregate(
					State->AggregateId,
					IgnitionPoint,
					TEXT("fire"),
					State->EventSeed))
				{
					UE_LOG(
						LogMatterFlux,
						Error,
						TEXT("Tree burn inspection could not ignite the logical aggregate."));
					GTreeCutCapturePending = false;
					RestoreTreeCutCamera(*State);
					if (State->bQuitAfterCapture)
					{
						FPlatformMisc::RequestExit(false);
					}
					return false;
				}
				PositionTreeCaptureCamera(
					*State,
					(State->CameraFront + State->CameraRight).GetSafeNormal());
				State->NextActionAt = Now + 0.18;
				break;
			}
			PlayableWorld->MaterializeFragmentAggregate(State->AggregateId);
			AFragment2DSourceActor* Tree = nullptr;
			for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
			{
				if (It->SourceId == State->PinnedSourceId)
				{
					Tree = *It;
					break;
				}
			}
			if (!Tree)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Tree cut inspection could not materialize its root."));
				GTreeCutCapturePending = false;
				RestoreTreeCutCamera(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExit(false);
				}
				return false;
			}
			State->Tree = Tree;
			PlayableWorld->SetFragmentSourceStreamingPinned(Tree->SourceId, true);
			PositionTreeCaptureCamera(
				*State,
				(State->CameraFront + State->CameraRight).GetSafeNormal());
			FFragmentWorldCutRequest CutRequest;
			CutRequest.CutShape.Type = EFragmentDamageShapeType::Line;
			CutRequest.CutShape.WorldTransform = Tree->GetActorTransform();
			// Cut just above the rooted first voxel. A center cut leaves a tall
			// stump and mostly tests a falling canopy instead of a felled tree.
			const FVector RootLocalCut(
				0.0f,
				0.0f,
				(-static_cast<float>(Tree->GetMaskHeight()) * 0.5f + 1.5f)
					* Tree->GetCellSize());
			CutRequest.CutShape.WorldTransform.SetLocation(
				Tree->GetActorTransform().TransformPosition(RootLocalCut));
			// 可视验收只覆盖 2 格实体树干，不把 mask padding 或旁边
			// 花草纳入唯一目标预算；仍然走与玩家法术相同的世界切割
			// 服务，并在执行后校验选定聚合根确实提交了 revision。
			CutRequest.CutShape.Extents.X = Tree->GetCellSize() * 2.2f;
			CutRequest.CutShape.Thickness =
				Tree->GetCellSize() * 1.1f;
			CutRequest.DamagePower = 500.0f;
			CutRequest.EventSeed = State->EventSeed;
			CutRequest.TargetPadding = 0.0f;
			CutRequest.MaxAffectedSources = 1;
			const int32 RootRevisionBefore = Tree->Revision;
			UFragmentSimulationSubsystem* Subsystem =
				World->GetSubsystem<UFragmentSimulationSubsystem>();
			if (!Subsystem
				|| UFragmentSimulationSubsystem::ExecuteWorldCut(
					World,
					CutRequest) != 1
				|| Tree->Revision != RootRevisionBefore + 1)
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
		case 10:
			if (State->bBurnInspectionOnly)
			{
				RequestTreeCutScreenshot(
					*State,
					TEXT("09_Burning_0p2s_FrontRight45.png"));
				State->NextActionAt = Now + 0.5;
				break;
			}
			LogTreeCarrierPhysics(*World, TEXT("just-detached"));
			RequestTreeCutScreenshot(*State, TEXT("09_JustDetached45.png"));
			State->NextActionAt = Now + 0.45;
			break;
		case 11:
			if (State->bBurnInspectionOnly)
			{
				RequestTreeCutScreenshot(
					*State,
					TEXT("10_Burning_0p7s_FrontRight45.png"));
				State->NextActionAt = Now + 1.1;
				break;
			}
			LogTreeCarrierPhysics(*World, TEXT("falling-early"));
			RequestTreeCutScreenshot(*State, TEXT("10_FallingEarly45.png"));
			State->NextActionAt = Now + 0.75;
			break;
		case 12:
			if (State->bBurnInspectionOnly)
			{
				RequestTreeCutScreenshot(
					*State,
					TEXT("11_Burning_1p8s_FrontRight45.png"));
				State->NextActionAt = Now + 2.2;
				break;
			}
			LogTreeCarrierPhysics(*World, TEXT("falling-late"));
			RequestTreeCutScreenshot(*State, TEXT("11_FallingLate45.png"));
			State->NextActionAt = Now + 1.25;
			break;
		case 13:
			if (State->bBurnInspectionOnly)
			{
				RequestTreeCutScreenshot(
					*State,
					TEXT("12_Burning_4p0s_FrontRight45.png"));
				State->NextActionAt = Now + 0.5;
				State->Phase = 17;
				break;
			}
			LogTreeCarrierPhysics(*World, TEXT("after-impact"));
			RequestTreeCutScreenshot(*State, TEXT("12_AfterImpact45.png"));
			State->NextActionAt = Now + 1.0;
			if (!State->bRepeatedItemCut)
			{
				State->Phase = 17;
			}
			break;
		case 14:
		{
			AFragment2DActor* Item = nullptr;
			for (TActorIterator<AFragment2DActor> It(World); It; ++It)
			{
				if (IsValid(*It)
					&& It->ActorHasTag(TEXT("MatterFluxFragment")))
				{
					Item = *It;
					break;
				}
			}
			if (!Item)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Repeated item-cut capture lost the detached carrier."));
				State->Phase = 17;
				State->NextActionAt = Now;
				break;
			}
			State->DetachedItem = Item;
			const FBox Bounds = Item->GetComponentsBoundingBox(true);
			State->CameraFocus = Bounds.IsValid
				? Bounds.GetCenter()
				: Item->GetActorLocation();
			PositionTreeCaptureCamera(
				*State,
				(State->CameraFront + State->CameraRight).GetSafeNormal(),
				0.65f);
			FFragmentWorldCutRequest ItemCut;
			ItemCut.CutShape.Type = EFragmentDamageShapeType::Box;
			ItemCut.CutShape.WorldTransform = FTransform(State->CameraFocus);
			const FVector Extent = Bounds.IsValid
				? Bounds.GetExtent()
				: FVector(40.0f);
			ItemCut.CutShape.Extents = FVector2D(
				FMath::Max(Extent.X, 20.0),
				FMath::Max(Extent.Z, 20.0));
			ItemCut.DamagePower = 0.0f;
			ItemCut.EventSeed = State->EventSeed + 1000;
			ItemCut.MaxAffectedSources = 4;
			for (int32 CutIndex = 0;
				CutIndex < Item->GetCutsBeforeFade() - 1;
				++CutIndex)
			{
				ItemCut.EventSeed += CutIndex;
				UFragmentSimulationSubsystem::ExecuteWorldCut(World, ItemCut);
			}
			const UPrimitiveComponent* ItemBody =
				Cast<UPrimitiveComponent>(Item->GetRootComponent());
			const bool bPreFadeAccepted = ItemBody
				&& Item->GetAcceptedCutCount()
					== Item->GetCutsBeforeFade() - 1
				&& !Item->IsCutFadeActive()
				&& ItemBody->GetCollisionEnabled()
					!= ECollisionEnabled::NoCollision;
			UE_LOG(LogMatterFlux, Display,
				TEXT("Repeated item-cut pre-fade: accepted=%s cuts=%d/%d collision=%d"),
				bPreFadeAccepted ? TEXT("true") : TEXT("false"),
				Item->GetAcceptedCutCount(),
				Item->GetCutsBeforeFade(),
				ItemBody
					? static_cast<int32>(ItemBody->GetCollisionEnabled())
					: -1);
			State->bAccepted = bPreFadeAccepted;
			RequestTreeCutScreenshot(
				*State,
				TEXT("13_DetachedItemBeforeFinalCut.png"));
			State->NextActionAt = Now + 0.6;
			break;
		}
		case 15:
		{
			AFragment2DActor* Item = State->DetachedItem.Get();
			if (!Item)
			{
				State->bAccepted = false;
				State->NextActionAt = Now;
				break;
			}
			const FBox Bounds = Item->GetComponentsBoundingBox(true);
			FFragmentWorldCutRequest FinalCut;
			FinalCut.CutShape.Type = EFragmentDamageShapeType::Box;
			FinalCut.CutShape.WorldTransform = FTransform(
				Bounds.IsValid ? Bounds.GetCenter() : Item->GetActorLocation());
			const FVector Extent = Bounds.IsValid
				? Bounds.GetExtent()
				: FVector(40.0f);
			FinalCut.CutShape.Extents = FVector2D(
				FMath::Max(Extent.X, 20.0),
				FMath::Max(Extent.Z, 20.0));
			FinalCut.DamagePower = 0.0f;
			FinalCut.EventSeed = State->EventSeed + 2000;
			FinalCut.MaxAffectedSources = 4;
			UFragmentSimulationSubsystem::ExecuteWorldCut(World, FinalCut);
			const UPrimitiveComponent* ItemBody =
				Cast<UPrimitiveComponent>(Item->GetRootComponent());
			State->bAccepted &= ItemBody
				&& Item->IsCutFadeActive()
				&& Item->GetAcceptedCutCount() == Item->GetCutsBeforeFade()
				&& ItemBody->GetCollisionEnabled()
					== ECollisionEnabled::NoCollision;
			State->NextActionAt = Now + 0.2;
			break;
		}
		case 16:
		{
			AFragment2DActor* Item = State->DetachedItem.Get();
			State->bAccepted &= Item
				&& Item->GetTransientFadeAlpha() < 0.99f;
			UE_LOG(LogMatterFlux, Display,
				TEXT("Repeated item-cut fade: accepted=%s alpha=%.3f aggregateLayers=%d"),
				State->bAccepted ? TEXT("true") : TEXT("false"),
				Item ? Item->GetTransientFadeAlpha() : -1.0f,
				Item ? Item->GetAggregateMemberCount() : -1);
			RequestTreeCutScreenshot(
				*State,
				TEXT("14_DetachedItemFading.png"));
			State->NextActionAt = Now + 0.85;
			break;
		}
		case 17:
			State->bAccepted &= !State->DetachedItem.IsValid();
			UE_LOG(LogMatterFlux, Display,
				TEXT("Repeated item-cut retirement: accepted=%s"),
				State->bAccepted ? TEXT("true") : TEXT("false"));
			RequestTreeCutScreenshot(
				*State,
				TEXT("15_DetachedItemRetired.png"));
			State->NextActionAt = Now + 0.5;
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
				FPlatformMisc::RequestExitWithStatus(
					false,
					!State->bRepeatedItemCut || State->bAccepted ? 0 : 5);
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
						// Exercise the same front-end transition as a real click. This
						// also cancels any still-pending multiplayer join before the
						// single-player world operation begins.
						Controller->ShowSinglePlayerMenu();
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
		State->bBurnInspectionOnly =
			Args.Num() > 2
			&& Args[2].Equals(TEXT("burn"), ESearchCase::IgnoreCase);
		State->bRepeatedItemCut =
			Args.Num() > 2
			&& Args[2].Equals(TEXT("item"), ESearchCase::IgnoreCase);
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
			TEXT("Queued tree inspection sequence: seed=%d quit=%s mode=%s"),
			State->EventSeed,
			State->bQuitAfterCapture ? TEXT("true") : TEXT("false"),
			State->bBurnInspectionOnly
				? TEXT("burn")
				: State->bRepeatedItemCut ? TEXT("item") : TEXT("cut"));
	}

	FAutoConsoleCommandWithWorldAndArgs GTreeCutCaptureCommand(
		TEXT("mf.Visual.TreeCutSequence"),
		TEXT("Capture one isolated tree, then cut, repeatedly cut its detached item, or burn it: mf.Visual.TreeCutSequence [event-seed=1337] [quit-after=1] [cut|item|burn]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueTreeCutCapture));

	struct FTreeBatchCutTarget
	{
		FGuid AggregateId;
		FGuid RootSourceId;
		FTransform RootTransform = FTransform::Identity;
		FFragmentSourceMask RootMask;
		TArray<FGuid> CanopySourceIds;
		TWeakObjectPtr<AFragment2DActor> Carrier;
		FVector DetachedLocation = FVector::ZeroVector;
		bool bRootCutCommitted = false;
	};

	struct FTreeBatchCutCaptureState
	{
		double QueuedAt = 0.0;
		double PhaseStartedAt = 0.0;
		int32 Phase = 0;
		int32 RequestedTreeCount = 12;
		int32 AcceptedCutCount = 0;
		bool bQuitAfterCapture = true;
		bool bAccepted = false;
		FString OutputDirectory;
		TArray<FTreeBatchCutTarget> Targets;
		TWeakObjectPtr<AMatterFluxPlayableWorldActor> PlayableWorld;
		TWeakObjectPtr<ACameraActor> Camera;
	};

	void RequestTreeBatchCutScreenshot(
		const FTreeBatchCutCaptureState& State,
		const TCHAR* FileName)
	{
		IFileManager::Get().MakeDirectory(*State.OutputDirectory, true);
		const FString Path = FPaths::Combine(State.OutputDirectory, FileName);
		FScreenshotRequest::RequestScreenshot(Path, true, false, false);
		UE_LOG(LogMatterFlux, Display,
			TEXT("Requested batch tree-cut screenshot: %s"), *Path);
	}

	AFragment2DActor* FindTreeBatchCarrier(
		UWorld& World,
		const FTreeBatchCutTarget& Target)
	{
		for (TActorIterator<AFragment2DActor> It(&World); It; ++It)
		{
			AFragment2DActor* Candidate = *It;
			for (const FGuid& SourceId : Target.CanopySourceIds)
			{
				if (Candidate->ContainsAggregateSource(SourceId))
				{
					return Candidate;
				}
			}
		}
		return nullptr;
	}

	bool TickTreeBatchCutCapture(
		const TSharedRef<FTreeBatchCutCaptureState>& State)
	{
		UWorld* World = GEngine && GEngine->GameViewport
			? GEngine->GameViewport->GetWorld()
			: nullptr;
		const double Now = FPlatformTime::Seconds();
		if (!World || !World->IsGameWorld())
		{
			if (Now - State->QueuedAt > 30.0)
			{
				GTreeBatchCutCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		if (State->Camera.IsValid())
		{
			if (APlayerController* Controller = World->GetFirstPlayerController())
			{
				Controller->SetViewTarget(State->Camera.Get());
			}
		}

		if (State->Phase == 0)
		{
			AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				if (!It->IsGenerationInProgress()
					&& It->GetCachedFragmentSourceCount() > 0)
				{
					PlayableWorld = *It;
					break;
				}
			}
			AMatterFluxPlayerController* Controller =
				Cast<AMatterFluxPlayerController>(World->GetFirstPlayerController());
			if (!PlayableWorld || !Controller)
			{
				if (Now - State->QueuedAt <= 30.0)
				{
					return true;
				}
				GTreeBatchCutCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			Controller->EnterGameplayForVisualCapture();
			Controller->HideUIForVisualCapture();
			if (GEngine)
			{
				GEngine->ClearOnScreenDebugMessages();
				GEngine->Exec(World, TEXT("DisableAllScreenMessages"));
			}

			MatterFlux::PlayableLevel::FLevelLayout Layout;
			const FMatterFluxContentRegistryPtr Registry =
				IMatterFluxScriptRuntime::Get().GetActiveRegistry();
			if (!MatterFlux::PlayableLevel::BuildLevelLayout(
				PlayableWorld->GetMapSeed(),
				Layout,
				Registry.Get()))
			{
				GTreeBatchCutCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}

			FVector Focus = FVector::ZeroVector;
			if (const APawn* Pawn = Controller->GetPawn())
			{
				Focus = Pawn->GetActorLocation();
			}
			TArray<const MatterFlux::PlayableLevel::FLevelFragmentSource*> Roots;
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
				: Layout.FragmentSources)
			{
				if (Source.bAggregateRoot
					&& Source.Name == TEXT("TreeTrunk")
					&& Source.AggregateId.IsValid())
				{
					Roots.Add(&Source);
				}
			}
			Roots.Sort([Focus](const auto& A, const auto& B)
			{
				const double DistanceA = FVector::DistSquared(
					A.Transform.GetLocation(), Focus);
				const double DistanceB = FVector::DistSquared(
					B.Transform.GetLocation(), Focus);
				if (!FMath::IsNearlyEqual(DistanceA, DistanceB))
				{
					return DistanceA < DistanceB;
				}
				return A.SourceId.ToString(EGuidFormats::Digits)
					< B.SourceId.ToString(EGuidFormats::Digits);
			});
			const int32 TreeCount = FMath::Min(
				State->RequestedTreeCount,
				Roots.Num());
			FBox BatchBounds(ForceInit);
			for (int32 Index = 0; Index < TreeCount; ++Index)
			{
				const auto& Root = *Roots[Index];
				FTreeBatchCutTarget& Target = State->Targets.AddDefaulted_GetRef();
				Target.AggregateId = Root.AggregateId;
				Target.RootSourceId = Root.SourceId;
				Target.RootTransform = Root.Transform;
				Target.RootMask = Root.Mask;
				for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Member
					: Layout.FragmentSources)
				{
					if (Member.AggregateId != Root.AggregateId)
					{
						continue;
					}
					const FVector HalfExtent(
						Member.Mask.Width * Member.Mask.CellSize * 0.5f,
						Member.Mask.CellSize * 0.5f,
						Member.Mask.Height * Member.Mask.CellSize * 0.5f);
					BatchBounds += FBox(-HalfExtent, HalfExtent).TransformBy(
						Member.Transform.ToMatrixWithScale());
					if (Member.SourceId != Root.SourceId
						&& (Member.Name == TEXT("TreeBranch")
							|| Member.Name == TEXT("TreeLeaves")))
					{
						Target.CanopySourceIds.Add(Member.SourceId);
					}
				}
			}

			if (State->Targets.IsEmpty() || !BatchBounds.IsValid)
			{
				GTreeBatchCutCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			const FVector Center = BatchBounds.GetCenter();
			const float Radius = FMath::Max(BatchBounds.GetExtent().Size2D(), 600.0f);
			const FVector CameraLocation = Center
				+ FVector(-0.75f, -0.75f, 1.15f).GetSafeNormal()
					* Radius * 1.65f;
			State->Camera = World->SpawnActor<ACameraActor>(
				CameraLocation,
				(Center - CameraLocation).Rotation());
			if (!State->Camera.IsValid())
			{
				GTreeBatchCutCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			State->Camera->GetCameraComponent()->SetFieldOfView(55.0f);
			Controller->SetViewTarget(State->Camera.Get());
			State->PlayableWorld = PlayableWorld;
			State->Phase = 1;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (State->Phase == 1 && Now - State->PhaseStartedAt >= 0.8)
		{
			RequestTreeBatchCutScreenshot(*State, TEXT("01_ForestBeforeBatchCut.png"));
			State->Phase = 2;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (State->Phase == 2 && Now - State->PhaseStartedAt >= 0.7)
		{
			for (int32 Index = 0; Index < State->Targets.Num(); ++Index)
			{
				FTreeBatchCutTarget& Target = State->Targets[Index];
				FFragmentWorldCutRequest CutRequest;
				CutRequest.CutShape.Type = EFragmentDamageShapeType::Line;
				CutRequest.CutShape.WorldTransform = Target.RootTransform;
				const FVector RootLocalCut(
					0.0f,
					0.0f,
					(-static_cast<float>(Target.RootMask.Height) * 0.5f + 1.5f)
						* Target.RootMask.CellSize);
				CutRequest.CutShape.WorldTransform.SetLocation(
					Target.RootTransform.TransformPosition(RootLocalCut));
				CutRequest.CutShape.Extents.X = Target.RootMask.CellSize * 2.2f;
				CutRequest.CutShape.Thickness = Target.RootMask.CellSize * 1.1f;
				CutRequest.DamagePower = 500.0f;
				CutRequest.EventSeed = 1337 + Index * 101;
				// Match the player ability. A one-target test is not representative:
				// a flower sharing the cut volume can legitimately be the nearest
				// target, while gameplay continues through up to four logical items.
				CutRequest.MaxAffectedSources = 4;
				UFragmentSimulationSubsystem::ExecuteWorldCut(World, CutRequest);
				int32 RootRevision = INDEX_NONE;
				TArray<uint8> RootRuntimeMask;
				Target.bRootCutCommitted = State->PlayableWorld.IsValid()
					&& State->PlayableWorld->GetFragmentSourceRuntimeState(
						Target.RootSourceId,
						RootRevision,
						RootRuntimeMask)
					&& RootRevision > 0
					&& RootRuntimeMask.Contains(1);
				State->AcceptedCutCount += Target.bRootCutCommitted ? 1 : 0;
				Target.Carrier = FindTreeBatchCarrier(*World, Target);
				if (Target.Carrier.IsValid())
				{
					Target.DetachedLocation = Target.Carrier->GetActorLocation();
				}
			}
			State->Phase = 3;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (State->Phase == 3 && Now - State->PhaseStartedAt >= 0.15)
		{
			RequestTreeBatchCutScreenshot(*State, TEXT("02_AllTreesJustDetached.png"));
			State->Phase = 4;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (State->Phase == 4 && Now - State->PhaseStartedAt >= 2.5)
		{
			int32 FailedTrees = 0;
			for (int32 Index = 0; Index < State->Targets.Num(); ++Index)
			{
				FTreeBatchCutTarget& Target = State->Targets[Index];
				AFragment2DActor* Carrier = Target.Carrier.Get();
				if (!Carrier)
				{
					Carrier = FindTreeBatchCarrier(*World, Target);
				}
				int32 MissingCanopyMembers = 0;
				for (const FGuid& SourceId : Target.CanopySourceIds)
				{
					MissingCanopyMembers +=
						!Carrier || !Carrier->ContainsAggregateSource(SourceId) ? 1 : 0;
				}
				int32 StaticCanopyActors = 0;
				for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
				{
					if (It->AggregateId == Target.AggregateId
						&& !It->IsActorBeingDestroyed()
						&& Target.CanopySourceIds.Contains(It->SourceId)
						&& It->GetRuntimeMask().Contains(1))
					{
						++StaticCanopyActors;
					}
				}
				const float HorizontalTravel = Carrier
					? FVector::Dist2D(Target.DetachedLocation, Carrier->GetActorLocation())
					: 0.0f;
				const float FallDistance = Carrier
					? Target.DetachedLocation.Z - Carrier->GetActorLocation().Z
					: 0.0f;
				const float TiltDegrees = Carrier
					? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
						FMath::Abs(FVector::DotProduct(
							Carrier->GetActorUpVector(), FVector::UpVector)),
						0.0f,
						1.0f)))
					: 0.0f;
				const bool bLeavesMovedWithCarrier = Target.bRootCutCommitted
					&& Carrier
					&& MissingCanopyMembers == 0
					&& StaticCanopyActors == 0;
				const bool bTreeLeftStandingProjection =
					HorizontalTravel < 12.0f
					&& FallDistance < 18.0f
					&& TiltDegrees < 12.0f;
				const bool bTreeAccepted = bLeavesMovedWithCarrier
					&& !bTreeLeftStandingProjection;
				FailedTrees += bTreeAccepted ? 0 : 1;
				UE_LOG(LogMatterFlux, Display,
					TEXT("Batch tree-cut [%02d]: accepted=%s carrier=%s missingCanopy=%d staticCanopy=%d horizontal=%.2f fall=%.2f tilt=%.2f"),
					Index,
					bTreeAccepted ? TEXT("true") : TEXT("false"),
					Carrier ? *Carrier->GetName() : TEXT("none"),
					MissingCanopyMembers,
					StaticCanopyActors,
					HorizontalTravel,
					FallDistance,
					TiltDegrees);
			}
			State->bAccepted = State->AcceptedCutCount == State->Targets.Num()
				&& FailedTrees == 0;
			UE_LOG(LogMatterFlux, Display,
				TEXT("Batch tree-cut acceptance: accepted=%s requested=%d cut=%d failed=%d"),
				State->bAccepted ? TEXT("true") : TEXT("false"),
				State->Targets.Num(),
				State->AcceptedCutCount,
				FailedTrees);
			RequestTreeBatchCutScreenshot(*State, TEXT("03_ForestAfterSettling.png"));
			State->Phase = 5;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (State->Phase == 5 && Now - State->PhaseStartedAt >= 0.8)
		{
			GTreeBatchCutCapturePending = false;
			if (State->bQuitAfterCapture)
			{
				FPlatformMisc::RequestExitWithStatus(
					false,
					State->bAccepted ? 0 : 5);
			}
			return false;
		}
		return true;
	}

	void QueueTreeBatchCutCapture(const TArray<FString>& Args, UWorld*)
	{
		if (GTreeBatchCutCapturePending)
		{
			return;
		}
		GTreeBatchCutCapturePending = true;
		const TSharedRef<FTreeBatchCutCaptureState> State =
			MakeShared<FTreeBatchCutCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->PhaseStartedAt = State->QueuedAt;
		State->RequestedTreeCount = Args.IsValidIndex(0)
			? FMath::Clamp(FCString::Atoi(*Args[0]), 4, 24)
			: 12;
		State->bQuitAfterCapture = !Args.IsValidIndex(1)
			|| FCString::Atoi(*Args[1]) != 0;
		State->OutputDirectory = FPaths::Combine(
			FPaths::ScreenShotDir(),
			TEXT("MatterFluxTreeBatchCut"),
			FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickTreeBatchCutCapture(State);
			}));
	}

	FAutoConsoleCommandWithWorldAndArgs GTreeBatchCutCaptureCommand(
		TEXT("mf.Visual.TreeBatchCut"),
		TEXT("Batch-cut many generated trees and audit canopy transfer and fall: mf.Visual.TreeBatchCut [tree-count=12] [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueTreeBatchCutCapture));

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

	struct FHouseCaptureState
	{
		double QueuedAt = 0.0;
		double PhaseStartedAt = 0.0;
		int32 Phase = 0;
		int32 MapSeed = 1337;
		bool bQuitAfterCapture = true;
		FString OutputDirectory;
		TWeakObjectPtr<AMatterFluxCharacter> Character;
		TWeakObjectPtr<AMatterFluxTwoStoreyHouseActor> House;
		TWeakObjectPtr<AMatterFluxCreatureActor> IndoorResident;
		TArray<TWeakObjectPtr<AMatterFluxCreatureActor>> Creatures;
		float OriginalCameraArmLength = 0.0f;
		FVector OriginalCameraTargetOffset = FVector::ZeroVector;
		FTransform OriginalCharacterTransform = FTransform::Identity;
		bool bOriginalCameraLagEnabled = true;
		float MaximumPlayerFeetZ = -TNumericLimits<float>::Max();
		float MinimumPlayerFeetZAfterUpper = TNumericLimits<float>::Max();
		float MinimumResidentFeetZ = TNumericLimits<float>::Max();
		float MaximumResidentFeetZ = -TNumericLimits<float>::Max();
		float MinimumResidentFeetZAfterUpper = TNumericLimits<float>::Max();
		int32 ExteriorCutCaptureStage = 0;
	};

	void RequestHouseScreenshot(
		const FHouseCaptureState& State,
		const TCHAR* Filename)
	{
		IFileManager::Get().MakeDirectory(*State.OutputDirectory, true);
		const FString Path = FPaths::Combine(State.OutputDirectory, Filename);
		FScreenshotRequest::RequestScreenshot(
			Path, false, false, false, FIntRect(), true);
		UE_LOG(LogMatterFlux, Display,
			TEXT("House sequence requested screenshot: %s"), *Path);
	}

	void FrameHouseForCapture(
		AMatterFluxCharacter& Character,
		const AMatterFluxTwoStoreyHouseActor& House)
	{
		if (!Character.CameraBoom)
		{
			return;
		}
		// TargetOffset 是世界空间偏移。每帧抵消角色在楼梯上的移动，
		// 让六张验收图始终以整栋房屋中心为固定构图，而不是追着角色裁图。
		const FVector Focus = House.GetActorTransform().TransformPosition(
			FVector(0.0f, 0.0f, 410.0f));
		Character.CameraBoom->TargetArmLength = 3000.0f;
		Character.CameraBoom->TargetOffset =
			Focus - Character.GetActorLocation();
	}

	void RestoreHouseCapture(const FHouseCaptureState& State)
	{
		if (AMatterFluxCharacter* Character = State.Character.Get())
		{
			Character->SetActorTransform(
				State.OriginalCharacterTransform,
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
			if (Character->CameraBoom)
			{
				Character->CameraBoom->TargetArmLength =
					State.OriginalCameraArmLength;
				Character->CameraBoom->TargetOffset =
					State.OriginalCameraTargetOffset;
				Character->CameraBoom->bEnableCameraLag =
					State.bOriginalCameraLagEnabled;
			}
		}
		for (const TWeakObjectPtr<AMatterFluxCreatureActor>& Creature
			: State.Creatures)
		{
			if (Creature.IsValid())
			{
				Creature->Destroy();
			}
		}
		if (AMatterFluxTwoStoreyHouseActor* House = State.House.Get())
		{
			House->SetCutawayViewerOverride(nullptr);
			House->RefreshCutawayImmediately();
		}
	}

	bool TickHouseCapture(const TSharedRef<FHouseCaptureState>& State)
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
				TEXT("House capture timed out waiting for a game viewport."));
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
		APlayerController* PlayerController = World->GetFirstPlayerController();
		AMatterFluxCharacter* Character = PlayerController
			? Cast<AMatterFluxCharacter>(PlayerController->GetPawn())
			: nullptr;
		if (!PlayableWorld || !Character)
		{
			return true;
		}
		if (PlayableWorld->GetMapSeed() != State->MapSeed)
		{
			PlayableWorld->Regenerate(State->MapSeed);
			State->PhaseStartedAt = Now;
			return true;
		}
		if (PlayableWorld->IsGenerationInProgress()
			|| PlayableWorld->GetPendingFragmentSourceSpawnCount() > 0)
		{
			return true;
		}
		AMatterFluxTwoStoreyHouseActor* House =
			PlayableWorld->GetGeneratedHouse();
		if (!House)
		{
			return true;
		}
		if (State->Character.IsValid() && World->IsPaused())
		{
			PlayerController->SetPause(false);
		}

		if (!State->Character.IsValid())
		{
			State->Character = Character;
			State->House = House;
			State->OriginalCameraArmLength = Character->CameraBoom
				? Character->CameraBoom->TargetArmLength : 1480.0f;
			State->OriginalCameraTargetOffset = Character->CameraBoom
				? Character->CameraBoom->TargetOffset : FVector::ZeroVector;
			State->OriginalCharacterTransform = Character->GetActorTransform();
			State->bOriginalCameraLagEnabled = Character->CameraBoom
				? Character->CameraBoom->bEnableCameraLag : true;
			if (AMatterFluxPlayerController* MatterFluxController =
				Cast<AMatterFluxPlayerController>(PlayerController))
			{
				MatterFluxController->CloseShellMenu();
				MatterFluxController->HandleShellStateChanged(false, false);
			}
			PlayerController->SetPause(false);
			PlayerController->ResetIgnoreMoveInput();
			Character->SetActorTickEnabled(true);
			if (UCharacterMovementComponent* Movement =
				Character->GetCharacterMovement())
			{
				Movement->SetComponentTickEnabled(true);
				Movement->StopMovementImmediately();
				Movement->SetMovementMode(MOVE_Walking);
			}
			if (Character->CameraBoom)
			{
				Character->CameraBoom->bEnableCameraLag = false;
			}

			const auto SpawnCreature = [World, House](
				const FName DefinitionId,
				const FVector& LocalLocation)
			{
				const FVector WorldLocation = House->GetActorTransform()
					.TransformPosition(LocalLocation);
				AMatterFluxCreatureActor* Creature =
					World->SpawnActorDeferred<AMatterFluxCreatureActor>(
						AMatterFluxCreatureActor::StaticClass(),
						FTransform(FRotator::ZeroRotator, WorldLocation),
						nullptr,
						nullptr,
						ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
				if (Creature)
				{
					Creature->InitializeCreature(DefinitionId);
					Creature->FinishSpawning(
						FTransform(FRotator::ZeroRotator, WorldLocation));
				}
				return Creature;
			};
			if (AMatterFluxCreatureActor* Lower = SpawnCreature(
				TEXT("std.merchant_base"),
				FVector(250.0f, -125.0f, 114.0f)))
			{
				State->Creatures.Add(Lower);
			}
			if (AMatterFluxCreatureActor* Upper = SpawnCreature(
				TEXT("std.merchant_base"),
				FVector(-170.0f, -220.0f, 474.0f)))
			{
				State->Creatures.Add(Upper);
			}
			AMatterFluxCreatureActor* ExteriorViewer = SpawnCreature(
				TEXT("std.merchant_base"),
				FVector(1600.0f, 1600.0f, 180.0f));
			if (ExteriorViewer)
			{
				State->Creatures.Add(ExteriorViewer);
				House->SetCutawayViewerOverride(ExteriorViewer);
			}

			const float GroundZ = House->GetFloorSurfaceWorldZ(0);
			const FVector ExteriorLocal(2600.0f, 2600.0f,
				GroundZ - House->GetActorLocation().Z + 91.0f);
			Character->SetActorLocation(
				House->GetActorTransform().TransformPosition(ExteriorLocal),
				false, nullptr, ETeleportType::TeleportPhysics);
			FrameHouseForCapture(*Character, *House);
			State->PhaseStartedAt = Now;
			return true;
		}

		FrameHouseForCapture(*Character, *House);

		if (FScreenshotRequest::IsScreenshotRequested())
		{
			return true;
		}
		if (Now - State->QueuedAt > 95.0)
		{
			UE_LOG(LogMatterFlux, Error,
				TEXT("House capture timed out in phase %d."), State->Phase);
			RestoreHouseCapture(*State);
			if (State->bQuitAfterCapture)
			{
				FPlatformMisc::RequestExitWithStatus(false, 4);
			}
			return false;
		}

		switch (State->Phase)
		{
		case 0:
			if (Now - State->PhaseStartedAt < 2.0
				&& State->ExteriorCutCaptureStage == 0)
			{
				return true;
			}
			if (State->ExteriorCutCaptureStage == 0)
			{
				RequestHouseScreenshot(
					*State,
					TEXT("00_WallBeforeCut.png"));
				State->ExteriorCutCaptureStage = 1;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (State->ExteriorCutCaptureStage == 1)
			{
				AFragment2DSourceActor* TargetWall = nullptr;
				float BestCameraFacingScore = -TNumericLimits<float>::Max();
				const FVector CameraLocation = PlayerController
					&& PlayerController->PlayerCameraManager
						? PlayerController->PlayerCameraManager->GetCameraLocation()
						: Character->GetActorLocation();
				for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
				{
					AFragment2DSourceActor* Candidate = *It;
					if (!IsValid(Candidate)
						|| Candidate->GetOwner() != House
						|| Candidate->SourceMaterialId != TEXT("stone")
						|| !Candidate->ActorHasTag(
							TEXT("MatterFluxHouseGroup.LowerWalls")))
					{
						continue;
					}
					const FVector Local = House->GetActorTransform()
						.InverseTransformPosition(Candidate->GetActorLocation());
					FHitResult VisibilityHit;
					FCollisionQueryParams VisibilityParams(
						SCENE_QUERY_STAT(MatterFluxHouseCaptureWall),
						false);
					VisibilityParams.AddIgnoredActor(House);
					const bool bDirectlyVisible = World->LineTraceSingleByChannel(
						VisibilityHit,
						CameraLocation,
						Candidate->GetActorLocation(),
						ECC_Visibility,
						VisibilityParams)
						&& VisibilityHit.GetActor() == Candidate;
					FVector2D ScreenPosition = FVector2D::ZeroVector;
					const bool bOnScreen = PlayerController
						&& PlayerController->ProjectWorldLocationToScreen(
							Candidate->GetActorLocation(),
							ScreenPosition,
							true);
					// The lower-left plaster panel is unobstructed in the fixed
					// house composition. The image gate must modify that real panel,
					// not a rear wall merely visible through the existing doorway.
					const FVector2D ScreenCenter(455.0, 570.0);
					const float ScreenPenalty = bOnScreen
						? FVector2D::Distance(ScreenPosition, ScreenCenter)
						: 5000.0f;
					// A visible, broad wall is a valid screenshot target. This
					// deliberately rejects narrow corner posts and occluded walls.
					const float Score = (bDirectlyVisible ? 1000000.0f : 0.0f)
						+ static_cast<float>(Candidate->GetMaskWidth()) * 5.0f
						- ScreenPenalty + Local.X * 0.01f + Local.Y * 0.01f;
					if (!TargetWall || Score > BestCameraFacingScore)
					{
						TargetWall = Candidate;
						BestCameraFacingScore = Score;
					}
				}
				UFragmentSimulationSubsystem* FragmentSubsystem =
					World->GetSubsystem<UFragmentSimulationSubsystem>();
				if (!TargetWall || !FragmentSubsystem)
				{
					UE_LOG(LogMatterFlux, Error,
						TEXT("House visual sequence could not find its real lower-wall cut target."));
					RestoreHouseCapture(*State);
					if (State->bQuitAfterCapture)
					{
						FPlatformMisc::RequestExitWithStatus(false, 4);
					}
					return false;
				}
				const FVector TargetLocal = House->GetActorTransform()
					.InverseTransformPosition(TargetWall->GetActorLocation());
				UE_LOG(LogMatterFlux, Display,
					TEXT("House visual sequence selected visible wall local=(%.1f, %.1f, %.1f) mask=%dx%d."),
					TargetLocal.X,
					TargetLocal.Y,
					TargetLocal.Z,
					TargetWall->GetMaskWidth(),
					TargetWall->GetMaskHeight());
				FFragmentDamageEvent Event;
				Event.SourceId = TargetWall->SourceId;
				Event.BaseRevision = TargetWall->Revision;
				Event.DamageShape.Type = EFragmentDamageShapeType::Circle;
				Event.DamageShape.WorldTransform = TargetWall->GetActorTransform();
				Event.DamageShape.Radius = TargetWall->GetCellSize() * 2.6f;
				Event.DamagePower = 1200.0f;
				Event.EventSeed = 0x484F5553;
				if (!FragmentSubsystem->RequestFragmentDamage(TargetWall, Event))
				{
					UE_LOG(LogMatterFlux, Error,
						TEXT("House visual sequence could not commit its real lower-wall cut."));
					RestoreHouseCapture(*State);
					if (State->bQuitAfterCapture)
					{
						FPlatformMisc::RequestExitWithStatus(false, 4);
					}
					return false;
				}
				UE_LOG(LogMatterFlux, Display,
					TEXT("House visual sequence cut wall source=%s revision=%d."),
					*TargetWall->SourceId.ToString(EGuidFormats::Digits),
					TargetWall->Revision);
				State->ExteriorCutCaptureStage = 2;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (State->ExteriorCutCaptureStage == 2)
			{
				if (Now - State->PhaseStartedAt < 1.25)
				{
					return true;
				}
				RequestHouseScreenshot(
					*State,
					TEXT("00_WallAfterCut.png"));
				State->ExteriorCutCaptureStage = 3;
				State->PhaseStartedAt = Now;
				return true;
			}
			State->Phase = 1;
			State->PhaseStartedAt = Now;
			return true;

		case 1:
		{
			House->SetCutawayViewerOverride(nullptr);
			const float GroundZ = House->GetFloorSurfaceWorldZ(0);
			const FVector Local(-170.0f, -120.0f,
				GroundZ - House->GetActorLocation().Z + 91.0f);
			Character->SetActorLocation(
				House->GetActorTransform().TransformPosition(Local),
				false, nullptr, ETeleportType::TeleportPhysics);
			FrameHouseForCapture(*Character, *House);
			House->RefreshCutawayImmediately();
			State->Phase = 2;
			State->PhaseStartedAt = Now;
			return true;
		}

		case 2:
			if (Now - State->PhaseStartedAt < 1.5)
			{
				return true;
			}
			RequestHouseScreenshot(*State, TEXT("01_GroundFloorCutaway.png"));
			State->Phase = 3;
			State->PhaseStartedAt = Now;
			return true;

		case 3:
		{
			PlayerController->SetPause(false);
			PlayerController->ResetIgnoreMoveInput();
			if (UCharacterMovementComponent* Movement =
				Character->GetCharacterMovement())
			{
				Movement->StopMovementImmediately();
				Movement->SetMovementMode(MOVE_Walking);
			}
			const float GroundZ = House->GetFloorSurfaceWorldZ(0);
			const FVector Local(-350.0f, 245.0f,
				GroundZ - House->GetActorLocation().Z + 91.0f);
			Character->SetActorLocation(
				House->GetActorTransform().TransformPosition(Local),
				false, nullptr, ETeleportType::TeleportPhysics);
			State->MaximumPlayerFeetZ = GroundZ;
			State->Phase = 4;
			State->PhaseStartedAt = Now;
			return true;
		}

		case 4:
		{
			PlayerController->SetPause(false);
			Character->AddMovementInput(House->GetActorForwardVector(), 1.0f, true);
			const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
			const float FeetZ = Character->GetActorLocation().Z
				- (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);
			State->MaximumPlayerFeetZ = FMath::Max(
				State->MaximumPlayerFeetZ, FeetZ);
			const FVector Local = House->GetActorTransform()
				.InverseTransformPosition(Character->GetActorLocation());
			if (Local.X >= -20.0f && Now - State->PhaseStartedAt > 0.5)
			{
				RequestHouseScreenshot(*State, TEXT("02_PlayerClimbingStairs.png"));
				State->Phase = 5;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (Now - State->PhaseStartedAt > 8.0)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Player failed to move onto the middle of the stair: local=%s maxFeetZ=%.1f"),
					*Local.ToCompactString(), State->MaximumPlayerFeetZ);
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		case 5:
		{
			PlayerController->SetPause(false);
			Character->AddMovementInput(House->GetActorForwardVector(), 1.0f, true);
			const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
			const float FeetZ = Character->GetActorLocation().Z
				- (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);
			State->MaximumPlayerFeetZ = FMath::Max(
				State->MaximumPlayerFeetZ, FeetZ);
			const float UpperZ = House->GetFloorSurfaceWorldZ(1);
			if (FeetZ >= UpperZ - 55.0f)
			{
				if (UCharacterMovementComponent* Movement =
					Character->GetCharacterMovement())
				{
					Movement->StopMovementImmediately();
				}
				State->Phase = 6;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (Now - State->PhaseStartedAt > 8.0)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Player failed to reach the upper floor: maxFeetZ=%.1f upperZ=%.1f"),
					State->MaximumPlayerFeetZ, UpperZ);
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		case 6:
			if (Now - State->PhaseStartedAt < 1.0)
			{
				return true;
			}
			State->MinimumPlayerFeetZAfterUpper =
				House->GetFloorSurfaceWorldZ(1);
			House->RefreshCutawayImmediately();
			RequestHouseScreenshot(*State, TEXT("03_UpperFloorReached.png"));
			State->Phase = 7;
			State->PhaseStartedAt = Now;
			return true;

		case 7:
		case 8:
		{
			PlayerController->SetPause(false);
			Character->AddMovementInput(
				-House->GetActorForwardVector(), 1.0f, true);
			const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
			const float FeetZ = Character->GetActorLocation().Z
				- (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);
			State->MinimumPlayerFeetZAfterUpper = FMath::Min(
				State->MinimumPlayerFeetZAfterUpper, FeetZ);
			const FVector Local = House->GetActorTransform()
				.InverseTransformPosition(Character->GetActorLocation());
			const float GroundZ = House->GetFloorSurfaceWorldZ(0);
			const float UpperZ = House->GetFloorSurfaceWorldZ(1);
			if (State->Phase == 7
				&& Local.X <= 20.0f
				&& FeetZ <= UpperZ - 100.0f
				&& Now - State->PhaseStartedAt > 0.5)
			{
				RequestHouseScreenshot(
					*State, TEXT("04_PlayerDescendingStairs.png"));
				State->Phase = 8;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (State->Phase == 8 && FeetZ <= GroundZ + 55.0f)
			{
				if (UCharacterMovementComponent* Movement =
					Character->GetCharacterMovement())
				{
					Movement->StopMovementImmediately();
				}
				State->Phase = 9;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (Now - State->PhaseStartedAt > 8.0)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Player failed to descend stair phase %d: local=%s feetZ=%.1f minAfterUpper=%.1f"),
					State->Phase, *Local.ToCompactString(), FeetZ,
					State->MinimumPlayerFeetZAfterUpper);
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		case 9:
			if (Now - State->PhaseStartedAt < 0.75)
			{
				return true;
			}
			House->RefreshCutawayImmediately();
			RequestHouseScreenshot(
				*State, TEXT("05_PlayerReturnedGroundFloor.png"));
			State->Phase = 10;
			State->PhaseStartedAt = Now;
			return true;

		case 10:
		{
			if (Now - State->PhaseStartedAt < 1.0)
			{
				return true;
			}
			const float GroundZ = House->GetFloorSurfaceWorldZ(0);
			const FVector PlayerLocal(-170.0f, -120.0f,
				GroundZ - House->GetActorLocation().Z + 91.0f);
			Character->SetActorLocation(
				House->GetActorTransform().TransformPosition(PlayerLocal),
				false, nullptr, ETeleportType::TeleportPhysics);
			if (UCharacterMovementComponent* Movement =
				Character->GetCharacterMovement())
			{
				Movement->StopMovementImmediately();
				Movement->SetMovementMode(MOVE_Walking);
			}
			const FVector ResidentLocal(-330.0f, 245.0f,
				GroundZ - House->GetActorLocation().Z + 73.0f);
			const FVector ResidentWorld = House->GetActorTransform()
				.TransformPosition(ResidentLocal);
			AMatterFluxCreatureActor* Resident =
				World->SpawnActorDeferred<AMatterFluxCreatureActor>(
					AMatterFluxCreatureActor::StaticClass(),
					FTransform(FRotator::ZeroRotator, ResidentWorld),
					nullptr,
					nullptr,
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Resident)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("House capture failed to spawn the indoor resident."));
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			Resident->InitializeCreature(TEXT("std.house_resident"));
			Resident->FinishSpawning(
				FTransform(FRotator::ZeroRotator, ResidentWorld));
			State->IndoorResident = Resident;
			State->Creatures.Add(Resident);
			State->MinimumResidentFeetZ = GroundZ;
			State->MaximumResidentFeetZ = GroundZ;
			House->RefreshCutawayImmediately();
			State->Phase = 11;
			State->PhaseStartedAt = Now;
			return true;
		}

		case 11:
		case 12:
		{
			AMatterFluxCreatureActor* Resident = State->IndoorResident.Get();
			if (!Resident)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Indoor resident disappeared during stair traversal."));
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			const UCapsuleComponent* Capsule = Resident->GetCapsuleComponent();
			const float FeetZ = Resident->GetActorLocation().Z
				- (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);
			State->MinimumResidentFeetZ = FMath::Min(
				State->MinimumResidentFeetZ, FeetZ);
			State->MaximumResidentFeetZ = FMath::Max(
				State->MaximumResidentFeetZ, FeetZ);
			if (!AMatterFluxTwoStoreyHouseActor::FindContainingHouse(
				*World, Resident->GetActorLocation(), 80.0f))
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Indoor resident left the house during traversal: %s"),
					*Resident->GetActorLocation().ToCompactString());
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			const FVector Local = House->GetActorTransform()
				.InverseTransformPosition(Resident->GetActorLocation());
			const float GroundZ = House->GetFloorSurfaceWorldZ(0);
			const float UpperZ = House->GetFloorSurfaceWorldZ(1);
			if (State->Phase == 11
				&& Local.X >= -20.0f
				&& FeetZ >= GroundZ + 100.0f)
			{
				RequestHouseScreenshot(
					*State, TEXT("06_CreatureClimbingStairs.png"));
				State->Phase = 12;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (State->Phase == 12 && FeetZ >= UpperZ - 55.0f)
			{
				if (UCharacterMovementComponent* Movement =
					Resident->GetCharacterMovement())
				{
					Movement->StopMovementImmediately();
					Movement->DisableMovement();
				}
				State->MinimumResidentFeetZAfterUpper = UpperZ;
				State->Phase = 13;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (Now - State->PhaseStartedAt > 12.0)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Indoor resident failed stair phase %d: local=%s feetZ=%.1f range=[%.1f, %.1f]"),
					State->Phase, *Local.ToCompactString(), FeetZ,
					State->MinimumResidentFeetZ,
					State->MaximumResidentFeetZ);
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		case 13:
			if (Now - State->PhaseStartedAt < 0.75)
			{
				return true;
			}
			House->RefreshCutawayImmediately();
			RequestHouseScreenshot(
				*State, TEXT("07_CreatureReachedUpperFloor.png"));
			if (AMatterFluxCreatureActor* Resident =
				State->IndoorResident.Get())
			{
				if (UCharacterMovementComponent* Movement =
					Resident->GetCharacterMovement())
				{
					Movement->SetMovementMode(MOVE_Walking);
				}
			}
			State->Phase = 14;
			State->PhaseStartedAt = Now;
			return true;

		case 14:
		case 15:
		{
			AMatterFluxCreatureActor* Resident = State->IndoorResident.Get();
			if (!Resident)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Indoor resident disappeared during descent."));
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			const UCapsuleComponent* Capsule = Resident->GetCapsuleComponent();
			const float FeetZ = Resident->GetActorLocation().Z
				- (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);
			State->MinimumResidentFeetZAfterUpper = FMath::Min(
				State->MinimumResidentFeetZAfterUpper, FeetZ);
			const FVector Local = House->GetActorTransform()
				.InverseTransformPosition(Resident->GetActorLocation());
			const float GroundZ = House->GetFloorSurfaceWorldZ(0);
			const float UpperZ = House->GetFloorSurfaceWorldZ(1);
			if (!AMatterFluxTwoStoreyHouseActor::FindContainingHouse(
				*World, Resident->GetActorLocation(), 80.0f))
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Indoor resident left the house during descent: %s"),
					*Resident->GetActorLocation().ToCompactString());
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			if (State->Phase == 14
				&& Local.X <= 20.0f
				&& FeetZ <= UpperZ - 100.0f
				&& FeetZ >= GroundZ + 80.0f)
			{
				RequestHouseScreenshot(
					*State, TEXT("08_CreatureDescendingStairs.png"));
				State->Phase = 15;
				State->PhaseStartedAt = Now;
				return true;
			}
			if (State->Phase == 15 && FeetZ <= GroundZ + 55.0f)
			{
				if (UCharacterMovementComponent* Movement =
					Resident->GetCharacterMovement())
				{
					Movement->StopMovementImmediately();
					Movement->DisableMovement();
				}
				State->Phase = 16;
				State->PhaseStartedAt = Now;
				return true;
			}
			const double Timeout = State->Phase == 14 ? 25.0 : 12.0;
			if (Now - State->PhaseStartedAt > Timeout)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Indoor resident failed descent phase %d: local=%s feetZ=%.1f minAfterUpper=%.1f"),
					State->Phase, *Local.ToCompactString(), FeetZ,
					State->MinimumResidentFeetZAfterUpper);
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		case 16:
			if (Now - State->PhaseStartedAt < 0.75)
			{
				return true;
			}
			House->RefreshCutawayImmediately();
			RequestHouseScreenshot(
				*State, TEXT("09_CreatureReturnedGroundFloor.png"));
			State->Phase = 17;
			State->PhaseStartedAt = Now;
			return true;

		default:
			if (Now - State->PhaseStartedAt < 1.5)
			{
				return true;
			}
			const float GroundZ = House->GetFloorSurfaceWorldZ(0);
			const float UpperZ = House->GetFloorSurfaceWorldZ(1);
			if (State->MaximumPlayerFeetZ < UpperZ - 55.0f
				|| State->MinimumPlayerFeetZAfterUpper > GroundZ + 55.0f
				|| State->MaximumResidentFeetZ < UpperZ - 55.0f
				|| State->MinimumResidentFeetZAfterUpper > GroundZ + 55.0f)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("House round-trip thresholds failed: player=[%.1f, %.1f] resident=[%.1f, %.1f] ground=%.1f upper=%.1f"),
					State->MinimumPlayerFeetZAfterUpper,
					State->MaximumPlayerFeetZ,
					State->MinimumResidentFeetZAfterUpper,
					State->MaximumResidentFeetZ,
					GroundZ, UpperZ);
				RestoreHouseCapture(*State);
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			UE_LOG(LogMatterFlux, Display,
				TEXT("House capture passed round trip: playerFeetZ=[%.1f, %.1f] residentFeetZ=[%.1f, %.1f] output=%s"),
				State->MinimumPlayerFeetZAfterUpper,
				State->MaximumPlayerFeetZ,
				State->MinimumResidentFeetZAfterUpper,
				State->MaximumResidentFeetZ,
				*State->OutputDirectory);
			RestoreHouseCapture(*State);
			if (State->bQuitAfterCapture)
			{
				FPlatformMisc::RequestExit(false);
			}
			return false;
		}
	}

	void QueueHouseCapture(const TArray<FString>& Args, UWorld*)
	{
		const TSharedRef<FHouseCaptureState> State =
			MakeShared<FHouseCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->PhaseStartedAt = State->QueuedAt;
		State->MapSeed = Args.Num() > 0
			? FMath::Max(FCString::Atoi(*Args[0]), 1) : 1337;
		State->bQuitAfterCapture = Args.Num() <= 1
			|| FCString::Atoi(*Args[1]) != 0;
		State->OutputDirectory = FPaths::Combine(
			FPaths::ScreenShotDir(),
			TEXT("MatterFluxHouse"),
			FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickHouseCapture(State);
			}));
		UE_LOG(LogMatterFlux, Display,
			TEXT("Queued house capture: seed=%d quit=%s output=%s"),
			State->MapSeed,
			State->bQuitAfterCapture ? TEXT("true") : TEXT("false"),
			*State->OutputDirectory);
	}

	FAutoConsoleCommandWithWorldAndArgs GHouseCaptureCommand(
		TEXT("mf.Visual.HouseSequence"),
		TEXT("Capture exterior, floor cutaway, and live player/AI stair round trips: mf.Visual.HouseSequence [map-seed=1337] [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueHouseCapture));

	struct FLiquidPoolCaptureState
	{
		double QueuedAt = 0.0;
		double PhaseStartedAt = 0.0;
		int32 Phase = 0;
		int32 MapSeed = 1337;
		bool bQuitAfterCapture = true;
		FVector Focus = FVector::ZeroVector;
		FVector2D LakeSize = FVector2D::ZeroVector;
		FString OutputDirectory;
		TWeakObjectPtr<ACameraActor> Camera;
	};

	void RequestLiquidScreenshot(
		const FLiquidPoolCaptureState& State,
		const TCHAR* FileName)
	{
		const FString Path = FPaths::Combine(State.OutputDirectory, FileName);
		IFileManager::Get().MakeDirectory(*State.OutputDirectory, true);
		FScreenshotRequest::RequestScreenshot(Path, true, false, false);
		UE_LOG(LogMatterFlux, Display,
			TEXT("Requested liquid-pool screenshot: %s"), *Path);
	}

	void PlaceLiquidCamera(
		FLiquidPoolCaptureState& State,
		const FVector& RelativeLocation)
	{
		if (!State.Camera.IsValid())
		{
			return;
		}
		const FVector CameraLocation = State.Focus + RelativeLocation;
		State.Camera->SetActorLocation(CameraLocation);
		State.Camera->SetActorRotation(
			FRotationMatrix::MakeFromX(
				State.Focus - CameraLocation).Rotator());
	}

	bool TickLiquidPoolCapture(
		const TSharedRef<FLiquidPoolCaptureState>& State)
	{
		UWorld* World = GEngine && GEngine->GameViewport
			? GEngine->GameViewport->GetWorld()
			: nullptr;
		if (!World || !World->IsGameWorld())
		{
			if (FPlatformTime::Seconds() - State->QueuedAt > 30.0)
			{
				GLiquidPoolCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		const double Now = FPlatformTime::Seconds();
		if (State->Phase == 0)
		{
			AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				PlayableWorld = *It;
				break;
			}
			AMatterFluxPlayerController* Controller =
				Cast<AMatterFluxPlayerController>(World->GetFirstPlayerController());
			if (!PlayableWorld || !Controller)
			{
				return true;
			}
			Controller->EnterGameplayForVisualCapture();
			if (PlayableWorld->GetMapSeed() != State->MapSeed)
			{
				PlayableWorld->Regenerate(State->MapSeed);
			}
			if (AMatterFluxTwoStoreyHouseActor* House =
				PlayableWorld->GetGeneratedHouse())
			{
				House->SetActorHiddenInGame(true);
			}

			MatterFlux::PlayableLevel::FLevelLayout Layout;
			if (!MatterFlux::PlayableLevel::BuildLevelLayout(
					State->MapSeed,
					Layout,
					nullptr))
			{
				return true;
			}
			const MatterFlux::PlayableLevel::FLevelLayer* Lake =
				Layout.FindLayer(TEXT("Lake"));
			if (!Lake || Lake->Instances.IsEmpty())
			{
				return true;
			}
			FBox Bounds(ForceInit);
			for (const FTransform& Transform : Lake->Instances)
			{
				const FVector Extent = Transform.GetScale3D() * 50.0f;
				Bounds += FBox(
					Transform.GetLocation() - Extent,
					Transform.GetLocation() + Extent);
			}
			State->Focus = Bounds.GetCenter() - FVector(0.0f, 0.0f, 28.0f);
			State->LakeSize = FVector2D(Bounds.GetSize());
			State->Camera = World->SpawnActor<ACameraActor>();
			if (!State->Camera.IsValid())
			{
				return true;
			}
			State->Camera->GetCameraComponent()->SetFieldOfView(48.0f);
			Controller->SetViewTarget(State->Camera.Get());
			PlaceLiquidCamera(
				*State,
				FVector(-620.0f, -660.0f, 430.0f));
			State->Phase = 1;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (Now - State->PhaseStartedAt < 1.5)
		{
			return true;
		}
		if (State->Phase == 1)
		{
			RequestLiquidScreenshot(*State, TEXT("01_Oblique_ShallowToDeep.png"));
			State->Phase = 2;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 2)
		{
			PlaceLiquidCamera(
				*State,
				FVector(560.0f, -620.0f, 360.0f));
			State->Phase = 3;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 3)
		{
			RequestLiquidScreenshot(*State, TEXT("02_OppositeBank_Transparency.png"));
			State->Phase = 4;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 4)
		{
			PlaceLiquidCamera(
				*State,
				FVector(-40.0f, -80.0f, 980.0f));
			State->Phase = 5;
			State->PhaseStartedAt = Now;
			return true;
		}

		RequestLiquidScreenshot(*State, TEXT("03_HighAngle_FullPool.png"));
		UE_LOG(LogMatterFlux, Display,
			TEXT("Liquid-pool capture complete: seed=%d size=(%.1f, %.1f) output=%s"),
			State->MapSeed,
			State->LakeSize.X,
			State->LakeSize.Y,
			*State->OutputDirectory);
		GLiquidPoolCapturePending = false;
		if (State->bQuitAfterCapture)
		{
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([](float)
				{
					FPlatformMisc::RequestExit(false);
					return false;
				}),
				1.5f);
		}
		return false;
	}

	void QueueLiquidPoolCapture(const TArray<FString>& Args, UWorld*)
	{
		if (GLiquidPoolCapturePending)
		{
			return;
		}
		GLiquidPoolCapturePending = true;
		const TSharedRef<FLiquidPoolCaptureState> State =
			MakeShared<FLiquidPoolCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->PhaseStartedAt = State->QueuedAt;
		State->MapSeed = Args.Num() > 0
			? FMath::Max(FCString::Atoi(*Args[0]), 1)
			: 1337;
		State->bQuitAfterCapture = Args.Num() <= 1
			|| FCString::Atoi(*Args[1]) != 0;
		State->OutputDirectory = FPaths::Combine(
			FPaths::ScreenShotDir(),
			TEXT("MatterFluxLiquidPool"),
			FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickLiquidPoolCapture(State);
			}));
	}

	FAutoConsoleCommandWithWorldAndArgs GLiquidPoolCaptureCommand(
		TEXT("mf.Visual.LiquidPool"),
		TEXT("Capture three focused views of the deterministic lake: mf.Visual.LiquidPool [map-seed=1337] [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueLiquidPoolCapture));

	struct FDeepLiquidWalkCaptureState
	{
		double QueuedAt = 0.0;
		double PhaseStartedAt = 0.0;
		int32 Phase = 0;
		int32 MapSeed = 1337;
		int64 InitialWaterAmount = 0;
		float MinimumStartingDepth = 0.0f;
		float InitialPlayerSurfaceZ = 0.0f;
		float InitialCreatureSurfaceZ = 0.0f;
		FMatterFluxLiquidProjectionHeightAudit InitialProjectionAudit;
		bool bHasInitialProjectionAudit = false;
		int32 RepeatedTraversalIndex = 0;
		bool bQuitAfterCapture = true;
		bool bAcceptanceFailed = false;
		bool bTraversalMetricsPrimed = false;
		bool bBodiesRemovedForSettle = false;
		bool bPlayerCurrentLiquidAtTraversalEnd = false;
		bool bCreatureCurrentLiquidAtTraversalEnd = false;
		bool bPlayerLocallyDisplacedAtTraversalEnd = false;
		bool bCreatureLocallyDisplacedAtTraversalEnd = false;
		float PlayerDistanceAtTraversalEnd = 0.0f;
		float CreatureDistanceAtTraversalEnd = 0.0f;
		float PlayerTravelDistance = 0.0f;
		float CreatureTravelDistance = 0.0f;
		float PlayerSubmergedAtTraversalEnd = 0.0f;
		float CreatureSubmergedAtTraversalEnd = 0.0f;
		int32 MaximumDirtyProjectionChunks = 0;
		int32 MaximumRebuiltProjectionChunks = 0;
		int32 MaximumCheckerboardPasses = 0;
		bool bTravelTrackingInitialized = false;
		FVector Focus = FVector::ZeroVector;
		FVector PlayerStart = FVector::ZeroVector;
		FVector CreatureStart = FVector::ZeroVector;
		FVector PreviousPlayerLocation = FVector::ZeroVector;
		FVector PreviousCreatureLocation = FVector::ZeroVector;
		FString OutputDirectory;
		TWeakObjectPtr<AMatterFluxPlayableWorldActor> PlayableWorld;
		TWeakObjectPtr<AMatterFluxCharacter> Character;
		TWeakObjectPtr<AMatterFluxCreatureActor> Creature;
		TWeakObjectPtr<ACameraActor> Camera;
	};

	void RequestDeepLiquidWalkScreenshot(
		const FDeepLiquidWalkCaptureState& State,
		const TCHAR* FileName)
	{
		IFileManager::Get().MakeDirectory(*State.OutputDirectory, true);
		const FString Path = FPaths::Combine(State.OutputDirectory, FileName);
		FScreenshotRequest::RequestScreenshot(Path, true, false, false);
		UE_LOG(LogMatterFlux, Display,
			TEXT("Requested deep-liquid walk screenshot: %s"), *Path);
	}

	float GetLakeCellDepth(
		const MatterFlux::PlayableLevel::FLevelLayout& Layout,
		const FTransform& Transform)
	{
		const FVector Location = Transform.GetLocation();
		const int32 TerrainX = FMath::Clamp(
			FMath::RoundToInt(
				(Location.X - Layout.Terrain.FirstCellCenter.X)
					/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Width - 1);
		const int32 TerrainY = FMath::Clamp(
			FMath::RoundToInt(
				(Location.Y - Layout.Terrain.FirstCellCenter.Y)
					/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Height - 1);
		return Location.Z + Transform.GetScale3D().Z * 50.0f
			- Layout.Terrain.HeightAt(TerrainX, TerrainY);
	}

	const FTransform* FindDeepLakeCellNear(
		const MatterFlux::PlayableLevel::FLevelLayout& Layout,
		const MatterFlux::PlayableLevel::FLevelLayer& Lake,
		const FVector2D Desired,
		const FVector2D* Avoid = nullptr)
	{
		const FTransform* Best = nullptr;
		double BestScore = TNumericLimits<double>::Max();
		for (const FTransform& Transform : Lake.Instances)
		{
			const FVector Location = Transform.GetLocation();
			const float Depth = GetLakeCellDepth(Layout, Transform);
			if (Depth < 90.0f
				|| (Avoid
					&& FVector2D::Distance(
						FVector2D(Location), *Avoid) < 70.0f))
			{
				continue;
			}
			const double Score = FVector2D::DistSquared(
				FVector2D(Location), Desired);
			if (Score < BestScore)
			{
				Best = &Transform;
				BestScore = Score;
			}
		}
		return Best;
	}

	bool HasLiquidNear(
		const AMatterFluxPlayableWorldActor& PlayableWorld,
		const FVector& Location)
	{
		static const FVector2D Offsets[] = {
			FVector2D(64.0f, 0.0f),
			FVector2D(-64.0f, 0.0f),
			FVector2D(0.0f, 64.0f),
			FVector2D(0.0f, -64.0f),
			FVector2D(96.0f, 96.0f),
			FVector2D(-96.0f, -96.0f)
		};
		for (const FVector2D Offset : Offsets)
		{
			MatterFlux::Liquid::FLiquidColumn Column;
			if (PlayableWorld.TrySampleLiquidColumnAtWorldLocation(
					Location + FVector(Offset.X, Offset.Y, 0.0f),
					Column))
			{
				return true;
			}
		}
		return false;
	}

	bool IsLiquidLocallyDisplaced(
		const AMatterFluxPlayableWorldActor& PlayableWorld,
		const FVector& Location)
	{
		MatterFlux::Liquid::FLiquidColumn LocalColumn;
		if (!PlayableWorld.TrySampleLiquidColumnAtWorldLocation(
				Location, LocalColumn))
		{
			return true;
		}
		MatterFlux::Liquid::FLiquidColumn AmbientColumn;
		return PlayableWorld.TrySampleAmbientLiquidColumnAtWorldLocation(
				Location, AmbientColumn)
			&& LocalColumn.MaterialId == AmbientColumn.MaterialId
			&& LocalColumn.SurfaceZ < AmbientColumn.SurfaceZ - 1.0f;
	}

	bool TickDeepLiquidWalkCapture(
		const TSharedRef<FDeepLiquidWalkCaptureState>& State)
	{
		UWorld* World = GEngine && GEngine->GameViewport
			? GEngine->GameViewport->GetWorld()
			: nullptr;
		if (!World || !World->IsGameWorld())
		{
			if (FPlatformTime::Seconds() - State->QueuedAt > 30.0)
			{
				GDeepLiquidWalkCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		const double Now = FPlatformTime::Seconds();
		if (State->Phase == 0)
		{
			AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				PlayableWorld = *It;
				break;
			}
			AMatterFluxPlayerController* Controller =
				Cast<AMatterFluxPlayerController>(World->GetFirstPlayerController());
			AMatterFluxCharacter* Character = Controller
				? Cast<AMatterFluxCharacter>(Controller->GetPawn())
				: nullptr;
			if (!PlayableWorld || !Controller || !Character)
			{
				return true;
			}

			Controller->EnterGameplayForVisualCapture();
			Controller->HideUIForVisualCapture();
			if (GEngine)
			{
				GEngine->ClearOnScreenDebugMessages();
				GEngine->Exec(World, TEXT("DisableAllScreenMessages"));
			}
			if (PlayableWorld->GetMapSeed() != State->MapSeed)
			{
				PlayableWorld->Regenerate(State->MapSeed);
			}
			if (AMatterFluxTwoStoreyHouseActor* House =
				PlayableWorld->GetGeneratedHouse())
			{
				House->SetActorHiddenInGame(true);
			}

			MatterFlux::PlayableLevel::FLevelLayout Layout;
			if (!MatterFlux::PlayableLevel::BuildLevelLayout(
					State->MapSeed, Layout, nullptr))
			{
				return true;
			}
			const MatterFlux::PlayableLevel::FLevelLayer* Lake =
				Layout.FindLayer(TEXT("Lake"));
			if (!Lake || Lake->Instances.IsEmpty())
			{
				return true;
			}

			FBox LakeBounds(ForceInit);
			float HighestSurface = -TNumericLimits<float>::Max();
			for (const FTransform& Transform : Lake->Instances)
			{
				const FVector Location = Transform.GetLocation();
				LakeBounds += Location;
				HighestSurface = FMath::Max(
					HighestSurface,
					Location.Z + Transform.GetScale3D().Z * 50.0f);
			}
			const FVector LakeCenter = LakeBounds.GetCenter();
			const FVector2D PlayerDesired(
				LakeCenter.X - 85.0f, LakeCenter.Y - 58.0f);
			const FVector2D CreatureDesired(
				LakeCenter.X - 85.0f, LakeCenter.Y + 58.0f);
			const FTransform* PlayerCell = FindDeepLakeCellNear(
				Layout, *Lake, PlayerDesired);
			const FVector2D PlayerCellLocation = PlayerCell
				? FVector2D(PlayerCell->GetLocation())
				: FVector2D::ZeroVector;
			const FTransform* CreatureCell = FindDeepLakeCellNear(
				Layout, *Lake, CreatureDesired, &PlayerCellLocation);
			if (!PlayerCell || !CreatureCell)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Deep-liquid walk capture could not find two 90 cm lake cells"));
				GDeepLiquidWalkCapturePending = false;
				FPlatformMisc::RequestExitWithStatus(false, 4);
				return false;
			}

			MatterFlux::Liquid::FLiquidColumn PlayerColumn;
			MatterFlux::Liquid::FLiquidColumn CreatureColumn;
			if (!PlayableWorld->TrySampleLiquidColumnAtWorldLocation(
					PlayerCell->GetLocation(), PlayerColumn)
				|| !PlayableWorld->TrySampleLiquidColumnAtWorldLocation(
					CreatureCell->GetLocation(), CreatureColumn))
			{
				return true;
			}
			// Isolate this visual proof from roaming map-population creatures; the
			// controlled creature spawned below is the one whose displacement and
			// locomotion are measured.
			for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
			{
				if (It->BuoyancyComponent)
				{
					It->BuoyancyComponent->SetComponentTickEnabled(false);
				}
				It->Destroy();
			}

			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AMatterFluxCreatureActor* Creature =
				World->SpawnActor<AMatterFluxCreatureActor>(
					CreatureCell->GetLocation(),
					FRotator::ZeroRotator,
					SpawnParameters);
			if (!Creature)
			{
				return true;
			}
			Creature->InitializeCreature(TEXT("std.slime"));
			if (AController* CreatureController = Creature->GetController())
			{
				CreatureController->SetActorTickEnabled(false);
			}
			UCharacterMovementComponent* PlayerMovement =
				Character->GetCharacterMovement();
			UCharacterMovementComponent* CreatureMovement =
				Creature->GetCharacterMovement();
			if (!PlayerMovement || !CreatureMovement)
			{
				return true;
			}
			CreatureMovement->bRunPhysicsWithNoController = true;
			PlayerMovement->MaxWalkSpeed = 140.0f;
			CreatureMovement->MaxWalkSpeed = 140.0f;
			PlayerMovement->StopMovementImmediately();
			CreatureMovement->StopMovementImmediately();

			const float PlayerHalfHeight = Character->GetCapsuleComponent()
				->GetScaledCapsuleHalfHeight();
			const float CreatureHalfHeight = Creature->GetCapsuleComponent()
				->GetScaledCapsuleHalfHeight();
			State->PlayerStart = FVector(
				PlayerCell->GetLocation().X,
				PlayerCell->GetLocation().Y,
				PlayerColumn.BottomZ + PlayerHalfHeight + 2.0f);
			State->CreatureStart = FVector(
				CreatureCell->GetLocation().X,
				CreatureCell->GetLocation().Y,
				CreatureColumn.BottomZ + CreatureHalfHeight + 2.0f);
			Character->SetActorLocation(
				State->PlayerStart, false, nullptr,
				ETeleportType::TeleportPhysics);
			Creature->SetActorLocation(
				State->CreatureStart, false, nullptr,
				ETeleportType::TeleportPhysics);
			Character->SetActorRotation(FRotator::ZeroRotator);
			Creature->SetActorRotation(FRotator::ZeroRotator);

			State->PlayableWorld = PlayableWorld;
			State->Character = Character;
			State->Creature = Creature;
			State->InitialWaterAmount =
				PlayableWorld->GetSimulatedMaterialAmount(TEXT("water"));
			State->bHasInitialProjectionAudit =
				PlayableWorld->TryGetLiquidProjectionHeightAudit(
					TEXT("water"), State->InitialProjectionAudit);
			State->MinimumStartingDepth = FMath::Min(
				PlayerColumn.SurfaceZ - PlayerColumn.BottomZ,
				CreatureColumn.SurfaceZ - CreatureColumn.BottomZ);
			State->InitialPlayerSurfaceZ = PlayerColumn.SurfaceZ;
			State->InitialCreatureSurfaceZ = CreatureColumn.SurfaceZ;
			State->PreviousPlayerLocation = State->PlayerStart;
			State->PreviousCreatureLocation = State->CreatureStart;
			State->bTravelTrackingInitialized = true;
			State->Focus = FVector(
				LakeCenter.X, LakeCenter.Y, HighestSurface - 35.0f);
			State->Camera = World->SpawnActor<ACameraActor>(
				State->Focus + FVector(460.0f, -680.0f, 430.0f),
				FRotator::ZeroRotator);
			if (!State->Camera.IsValid())
			{
				return true;
			}
			State->Camera->SetActorRotation(
				(State->Focus - State->Camera->GetActorLocation()).Rotation());
			State->Camera->GetCameraComponent()->SetFieldOfView(46.0f);
			Controller->SetViewTarget(State->Camera.Get());
			State->Phase = 1;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (!State->PlayableWorld.IsValid()
			|| !State->Character.IsValid()
			|| !State->Creature.IsValid())
		{
			GDeepLiquidWalkCapturePending = false;
			FPlatformMisc::RequestExitWithStatus(false, 4);
			return false;
		}
		// Phase 1 includes the one-time projection bootstrap. Measure only the
		// player/creature interaction and subsequent refill work.
		if (State->Phase >= 2)
		{
			State->MaximumDirtyProjectionChunks = FMath::Max(
				State->MaximumDirtyProjectionChunks,
				State->PlayableWorld->GetLastLiquidProjectionDirtyChunkCount());
			State->MaximumRebuiltProjectionChunks = FMath::Max(
				State->MaximumRebuiltProjectionChunks,
				State->PlayableWorld->GetLastLiquidProjectionRebuiltChunkCount());
			State->MaximumCheckerboardPasses = FMath::Max(
				State->MaximumCheckerboardPasses,
				State->PlayableWorld->GetLastLiquidProjectionCheckerboardPassCount());
		}
		// The configured population timer may spawn roaming creatures after this
		// capture has started. Remove every non-controlled creature continuously so
		// the proof measures exactly the player and the one selected animal; an
		// independently ticking hidden buoyancy component is still a physical body.
		for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
		{
			if (*It == State->Creature.Get())
			{
				continue;
			}
			if (It->BuoyancyComponent)
			{
				It->BuoyancyComponent->SetComponentTickEnabled(false);
			}
			It->Destroy();
		}
		if (State->bTravelTrackingInitialized
			&& !State->bBodiesRemovedForSettle)
		{
			const FVector PlayerLocation = State->Character->GetActorLocation();
			const FVector CreatureLocation = State->Creature->GetActorLocation();
			State->PlayerTravelDistance += FVector::Dist2D(
				State->PreviousPlayerLocation, PlayerLocation);
			State->CreatureTravelDistance += FVector::Dist2D(
				State->PreviousCreatureLocation, CreatureLocation);
			State->PreviousPlayerLocation = PlayerLocation;
			State->PreviousCreatureLocation = CreatureLocation;
			if (State->Character->BuoyancyComponent)
			{
				State->PlayerSubmergedAtTraversalEnd = FMath::Max(
					State->PlayerSubmergedAtTraversalEnd,
					State->Character->BuoyancyComponent
						->GetLastSubmergedFraction());
			}
			if (State->Creature->BuoyancyComponent)
			{
				State->CreatureSubmergedAtTraversalEnd = FMath::Max(
					State->CreatureSubmergedAtTraversalEnd,
					State->Creature->BuoyancyComponent
						->GetLastSubmergedFraction());
			}
			State->bPlayerLocallyDisplacedAtTraversalEnd |=
				IsLiquidLocallyDisplaced(
					*State->PlayableWorld, PlayerLocation);
			State->bCreatureLocallyDisplacedAtTraversalEnd |=
				IsLiquidLocallyDisplaced(
					*State->PlayableWorld, CreatureLocation);
		}

		if (State->Phase == 1 && Now - State->PhaseStartedAt >= 1.2)
		{
			MatterFlux::Liquid::FLiquidColumn PlayerAmbientColumn;
			MatterFlux::Liquid::FLiquidColumn CreatureAmbientColumn;
			if (State->PlayableWorld->TrySampleAmbientLiquidColumnAtWorldLocation(
					State->PlayerStart, PlayerAmbientColumn)
				&& State->PlayableWorld->TrySampleAmbientLiquidColumnAtWorldLocation(
					State->CreatureStart, CreatureAmbientColumn))
			{
				State->InitialPlayerSurfaceZ = PlayerAmbientColumn.SurfaceZ;
				State->InitialCreatureSurfaceZ = CreatureAmbientColumn.SurfaceZ;
			}
			State->InitialWaterAmount = State->PlayableWorld
				->GetSimulatedMaterialAmount(TEXT("water"));
			State->bHasInitialProjectionAudit = State->PlayableWorld
				->TryGetLiquidProjectionHeightAudit(
					TEXT("water"), State->InitialProjectionAudit);
			RequestDeepLiquidWalkScreenshot(
				*State, TEXT("01_StandingDeepWater_Displacing.png"));
			State->Phase = 2;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 2)
		{
			State->Character->AddMovementInput(
				FVector::XAxisVector, 1.0f, true);
			State->Creature->AddMovementInput(
				FVector::XAxisVector, 1.0f, true);
			if (Now - State->PhaseStartedAt >= 0.45)
			{
				RequestDeepLiquidWalkScreenshot(
					*State, TEXT("02_MidWalk_DisplacedVolumes.png"));
				State->Phase = 3;
				State->PhaseStartedAt = Now;
			}
			return true;
		}
		if (State->Phase == 3)
		{
			State->Character->AddMovementInput(
				FVector::XAxisVector, -1.0f, true);
			State->Creature->AddMovementInput(
				FVector::XAxisVector, -1.0f, true);
			if (Now - State->PhaseStartedAt >= 0.45)
			{
				RequestDeepLiquidWalkScreenshot(
					*State, TEXT("03_EndWalk_NewVolumes.png"));
				State->Phase = 4;
				State->PhaseStartedAt = Now;
			}
			return true;
		}
		if (State->Phase == 4)
		{
			const float Direction = (State->RepeatedTraversalIndex & 1) == 0
				? -1.0f
				: 1.0f;
			State->Character->AddMovementInput(
				FVector::XAxisVector, Direction, true);
			State->Creature->AddMovementInput(
				FVector::XAxisVector, Direction, true);
			if (Now - State->PhaseStartedAt >= 0.9)
			{
				++State->RepeatedTraversalIndex;
				State->PhaseStartedAt = Now;
				if (State->RepeatedTraversalIndex >= 4)
				{
					State->Character->GetCharacterMovement()
						->StopMovementImmediately();
					State->Creature->GetCharacterMovement()
						->StopMovementImmediately();
					State->Character->SetActorLocation(
						State->PlayerStart, false, nullptr,
						ETeleportType::TeleportPhysics);
					State->Creature->SetActorLocation(
						State->CreatureStart, false, nullptr,
						ETeleportType::TeleportPhysics);
					RequestDeepLiquidWalkScreenshot(
						*State,
						TEXT("04_AfterFourRepeatedTraversals.png"));
					State->Phase = 5;
				}
			}
			return true;
		}
		if (State->Phase == 5
			&& !State->bTraversalMetricsPrimed
			&& Now - State->PhaseStartedAt >= 0.25)
		{
			if (State->Character->BuoyancyComponent)
			{
				State->Character->BuoyancyComponent->TickComponent(
					0.1f, LEVELTICK_All, nullptr);
			}
			if (State->Creature->BuoyancyComponent)
			{
				State->Creature->BuoyancyComponent->TickComponent(
					0.1f, LEVELTICK_All, nullptr);
			}
			State->bTraversalMetricsPrimed = true;
			return true;
		}
		if (State->Phase == 5
			&& State->bTraversalMetricsPrimed
			&& !State->bBodiesRemovedForSettle
			&& Now - State->PhaseStartedAt >= 1.0)
		{
			AMatterFluxPlayableWorldActor* PlayableWorld =
				State->PlayableWorld.Get();
			const FVector PlayerLocation = State->Character->GetActorLocation();
			const FVector CreatureLocation = State->Creature->GetActorLocation();
			MatterFlux::Liquid::FLiquidColumn Column;
			State->bPlayerCurrentLiquidAtTraversalEnd =
				PlayableWorld->TrySampleLiquidColumnAtWorldLocation(
					PlayerLocation, Column);
			State->bCreatureCurrentLiquidAtTraversalEnd =
				PlayableWorld->TrySampleLiquidColumnAtWorldLocation(
					CreatureLocation, Column);
			State->bPlayerLocallyDisplacedAtTraversalEnd |=
				IsLiquidLocallyDisplaced(*PlayableWorld, PlayerLocation);
			State->bCreatureLocallyDisplacedAtTraversalEnd |=
				IsLiquidLocallyDisplaced(*PlayableWorld, CreatureLocation);
			State->PlayerDistanceAtTraversalEnd = State->PlayerTravelDistance;
			State->CreatureDistanceAtTraversalEnd = State->CreatureTravelDistance;
			State->PlayerSubmergedAtTraversalEnd = FMath::Max(
				State->PlayerSubmergedAtTraversalEnd,
				State->Character->BuoyancyComponent
					? State->Character->BuoyancyComponent
						->GetLastSubmergedFraction()
					: 0.0f);
			State->CreatureSubmergedAtTraversalEnd = FMath::Max(
				State->CreatureSubmergedAtTraversalEnd,
				State->Creature->BuoyancyComponent
					? State->Creature->BuoyancyComponent
						->GetLastSubmergedFraction()
					: 0.0f);
			State->Character->SetActorLocation(
				PlayerLocation + FVector(0.0f, 0.0f, 1000.0f),
				false, nullptr, ETeleportType::TeleportPhysics);
			State->Creature->SetActorLocation(
				CreatureLocation + FVector(0.0f, 0.0f, 1000.0f),
				false, nullptr, ETeleportType::TeleportPhysics);
			State->Character->SetActorHiddenInGame(true);
			State->Creature->SetActorHiddenInGame(true);
			State->Character->SetActorEnableCollision(false);
			State->Creature->SetActorEnableCollision(false);
			if (State->Character->BuoyancyComponent)
			{
				State->Character->BuoyancyComponent->SetComponentTickEnabled(false);
			}
			if (State->Creature->BuoyancyComponent)
			{
				State->Creature->BuoyancyComponent->SetComponentTickEnabled(false);
			}
			State->Character->SetActorTickEnabled(false);
			State->Creature->SetActorTickEnabled(false);
			State->bBodiesRemovedForSettle = true;
			return true;
		}
		// Allow the low-frequency material solver—not render frames—to advance
		// through a complete wide-wake relaxation before final acceptance.
		if (State->Phase == 5 && Now - State->PhaseStartedAt >= 8.0)
		{
			AMatterFluxPlayableWorldActor* PlayableWorld =
				State->PlayableWorld.Get();
			MatterFlux::Liquid::FLiquidColumn PlayerStartColumn;
			MatterFlux::Liquid::FLiquidColumn CreatureStartColumn;
			MatterFlux::Liquid::FLiquidColumn PlayerAmbientColumn;
			MatterFlux::Liquid::FLiquidColumn CreatureAmbientColumn;
			const bool bPlayerTrailRefilled =
				PlayableWorld->TrySampleLiquidColumnAtWorldLocation(
					State->PlayerStart, PlayerStartColumn);
			const bool bCreatureTrailRefilled =
				PlayableWorld->TrySampleLiquidColumnAtWorldLocation(
					State->CreatureStart, CreatureStartColumn);
			const bool bPlayerAmbientRefilled =
				PlayableWorld->TrySampleAmbientLiquidColumnAtWorldLocation(
					State->PlayerStart, PlayerAmbientColumn);
			const bool bCreatureAmbientRefilled =
				PlayableWorld->TrySampleAmbientLiquidColumnAtWorldLocation(
					State->CreatureStart, CreatureAmbientColumn);
			const float PlayerStartingSurfaceDelta = bPlayerAmbientRefilled
				? PlayerAmbientColumn.SurfaceZ - State->InitialPlayerSurfaceZ
				: TNumericLimits<float>::Max();
			const float CreatureStartingSurfaceDelta = bCreatureAmbientRefilled
				? CreatureAmbientColumn.SurfaceZ - State->InitialCreatureSurfaceZ
				: TNumericLimits<float>::Max();
			const float MaximumStartingSurfaceLift = FMath::Max3(
				0.0f,
				PlayerStartingSurfaceDelta,
				CreatureStartingSurfaceDelta);
			const float PlayerDistance = State->PlayerDistanceAtTraversalEnd;
			const float CreatureDistance = State->CreatureDistanceAtTraversalEnd;
			const int64 FinalWaterAmount =
				PlayableWorld->GetSimulatedMaterialAmount(TEXT("water"));
			const int64 WaterAmountDelta =
				FinalWaterAmount - State->InitialWaterAmount;
			const int64 AbsoluteWaterAmountDelta = WaterAmountDelta >= 0
				? WaterAmountDelta : -WaterAmountDelta;
			// The generated world also advances its authored water/lava chemistry
			// during this capture. Exact displacement conservation is covered by
			// the pure material-world test; this live acceptance allows only the
			// small (<0.5%) global chemistry drift observed outside the lake.
			const int64 AllowedWaterChemistryDrift = FMath::Max<int64>(
				64,
				State->InitialWaterAmount / 200);
			const float PlayerSubmerged = State->PlayerSubmergedAtTraversalEnd;
			const float CreatureSubmerged = State->CreatureSubmergedAtTraversalEnd;
			FMatterFluxLiquidProjectionHeightAudit FinalProjectionAudit;
			const bool bHasFinalProjectionAudit =
				PlayableWorld->TryGetLiquidProjectionHeightAudit(
					TEXT("water"), FinalProjectionAudit);
			const float ProjectionOffsetDrift =
				FinalProjectionAudit.MedianOffset
					- State->InitialProjectionAudit.MedianOffset;
			const bool bProjectionTracksCanonical =
				State->bHasInitialProjectionAudit
				&& bHasFinalProjectionAudit
				&& FMath::Abs(State->InitialProjectionAudit.MedianOffset) <= 16.01f
				&& FMath::Abs(FinalProjectionAudit.MedianOffset) <= 16.01f
				&& FMath::Abs(ProjectionOffsetDrift) <= 16.01f
				&& FinalProjectionAudit.MaximumTriangleHeightSpan <= 32.01f;
			const bool bAccepted = State->MinimumStartingDepth >= 90.0f
				&& PlayerDistance >= 40.0f
				&& CreatureDistance >= 40.0f
				&& PlayerSubmerged > 0.05f
				&& CreatureSubmerged > 0.05f
				&& State->bPlayerLocallyDisplacedAtTraversalEnd
				&& State->bCreatureLocallyDisplacedAtTraversalEnd
				&& bPlayerTrailRefilled
				&& bCreatureTrailRefilled
				&& bPlayerAmbientRefilled
				&& bCreatureAmbientRefilled
				&& MaximumStartingSurfaceLift <= 4.0f
				&& bProjectionTracksCanonical
				&& AbsoluteWaterAmountDelta <= AllowedWaterChemistryDrift;
			UE_LOG(LogMatterFlux, Display,
				TEXT("Deep-liquid walk acceptance: accepted=%s depth=%.1f playerDistance=%.1f creatureDistance=%.1f playerSubmerged=%.3f creatureSubmerged=%.3f currentLiquid=(%s,%s) locallyDisplaced=(%s,%s) trailRefilled=(%s,%s) startSurfaceDelta=(%.2f,%.2f) maxLift=%.2f projectionAudit=%s projectionMedianOffset=%.2f->%.2f projectionDrift=%.2f projectionLocalMax=%.2f maxTriangleSpan=%.2f surfacePatches=%d dirtyChunksMax=%d rebuiltChunksMax=%d checkerboardPassesMax=%d waterAmount=%lld/%lld drift=%lld allowed=%lld"),
				bAccepted ? TEXT("true") : TEXT("false"),
				State->MinimumStartingDepth,
				PlayerDistance,
				CreatureDistance,
				PlayerSubmerged,
				CreatureSubmerged,
				State->bPlayerCurrentLiquidAtTraversalEnd ? TEXT("true") : TEXT("false"),
				State->bCreatureCurrentLiquidAtTraversalEnd ? TEXT("true") : TEXT("false"),
				State->bPlayerLocallyDisplacedAtTraversalEnd ? TEXT("true") : TEXT("false"),
				State->bCreatureLocallyDisplacedAtTraversalEnd ? TEXT("true") : TEXT("false"),
				bPlayerTrailRefilled ? TEXT("true") : TEXT("false"),
				bCreatureTrailRefilled ? TEXT("true") : TEXT("false"),
				PlayerStartingSurfaceDelta,
				CreatureStartingSurfaceDelta,
				MaximumStartingSurfaceLift,
				bProjectionTracksCanonical ? TEXT("true") : TEXT("false"),
				State->InitialProjectionAudit.MedianOffset,
				FinalProjectionAudit.MedianOffset,
				ProjectionOffsetDrift,
				FinalProjectionAudit.MaximumAbsoluteLocalOffset,
				FinalProjectionAudit.MaximumTriangleHeightSpan,
				FinalProjectionAudit.SurfacePatchCount,
				State->MaximumDirtyProjectionChunks,
				State->MaximumRebuiltProjectionChunks,
				State->MaximumCheckerboardPasses,
				FinalWaterAmount,
				State->InitialWaterAmount,
				WaterAmountDelta,
				AllowedWaterChemistryDrift);
			if (!bAccepted)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Deep-liquid walk acceptance failed; see the preceding metrics"));
			}
			RequestDeepLiquidWalkScreenshot(
				*State, TEXT("05_TrailsRefilled_SurfaceStillCanonical.png"));
			State->Phase = 6;
			State->PhaseStartedAt = Now;
			State->bAcceptanceFailed = !bAccepted;
			return true;
		}
		if (State->Phase == 6 && Now - State->PhaseStartedAt >= 1.0)
		{
			GDeepLiquidWalkCapturePending = false;
			if (State->bQuitAfterCapture)
			{
				if (State->bAcceptanceFailed)
				{
					FPlatformMisc::RequestExitWithStatus(false, 5);
				}
				else
				{
					FPlatformMisc::RequestExit(false);
				}
			}
			return false;
		}
		return true;
	}

	void QueueDeepLiquidWalkCapture(const TArray<FString>& Args, UWorld*)
	{
		if (GDeepLiquidWalkCapturePending)
		{
			return;
		}
		GDeepLiquidWalkCapturePending = true;
		const TSharedRef<FDeepLiquidWalkCaptureState> State =
			MakeShared<FDeepLiquidWalkCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->PhaseStartedAt = State->QueuedAt;
		State->MapSeed = Args.Num() > 0
			? FMath::Max(FCString::Atoi(*Args[0]), 1)
			: 1337;
		State->bQuitAfterCapture = Args.Num() <= 1
			|| FCString::Atoi(*Args[1]) != 0;
		State->OutputDirectory = FPaths::Combine(
			FPaths::ScreenShotDir(),
			TEXT("MatterFluxDeepLiquidWalk"),
			FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickDeepLiquidWalkCapture(State);
			}));
	}

	FAutoConsoleCommandWithWorldAndArgs GDeepLiquidWalkCaptureCommand(
		TEXT("mf.Visual.DeepLiquidWalk"),
		TEXT("Capture real player and creature locomotion through a 90+ cm depth-transparent lake: mf.Visual.DeepLiquidWalk [map-seed=1337] [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueDeepLiquidWalkCapture));

	struct FLiquidDropCaptureState
	{
		double QueuedAt = 0.0;
		double PhaseStartedAt = 0.0;
		int32 Phase = 0;
		int32 SimulatedSteps = 0;
		bool bQuitAfterCapture = true;
		bool bCaptureScreenshots = true;
		FName MapId = TEXT("test.liquid_density_drops");
		FString OutputDirectory;
		TUniquePtr<MatterFlux::Material::FChunkedMaterialWorld> CustomWorld;
		TUniquePtr<MatterFlux::Material::FCustomMapPourSimulation> PourSimulation;
		MatterFlux::Material::FCustomMapScene Scene;
		TWeakObjectPtr<AActor> PreviewActor;
		TWeakObjectPtr<ACameraActor> Camera;
		TWeakObjectPtr<AMatterFluxCharacter> Character;
		TWeakObjectPtr<AMatterFluxCreatureActor> Creature;
		TWeakObjectPtr<AStaticMeshActor> PhysicsBody;
		TMap<FName, TWeakObjectPtr<UInstancedStaticMeshComponent>> Batches;
		double LastInteractiveStepAt = 0.0;
	};

	void IsolatePlayableCustomMapPrimitives(
		UWorld& World,
		const AActor* PreviewActor,
		const AMatterFluxCharacter* Character,
		const AMatterFluxCreatureActor* Creature,
		const AStaticMeshActor* PhysicsBody)
	{
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor
				|| Actor == PreviewActor
				|| Actor == Character
				|| Actor == Creature
				|| Actor == PhysicsBody)
			{
				continue;
			}
			TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
			for (UPrimitiveComponent* Primitive : Primitives)
			{
				if (Primitive)
				{
					Primitive->SetVisibility(false, true);
					Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				}
			}
		}
	}

	UInstancedStaticMeshComponent* CreateLiquidDropBatch(
		AActor& Owner,
		UStaticMesh& CubeMesh,
		UMaterialInterface& PaletteMaterial,
		const FName Name,
		const FLinearColor& Color,
		const bool bCollision)
	{
		UInstancedStaticMeshComponent* Batch =
			NewObject<UInstancedStaticMeshComponent>(&Owner, Name);
		Owner.AddInstanceComponent(Batch);
		Batch->SetupAttachment(Owner.GetRootComponent());
		Batch->SetStaticMesh(&CubeMesh);
		Batch->SetCollisionEnabled(
			bCollision
				? ECollisionEnabled::QueryAndPhysics
				: ECollisionEnabled::NoCollision);
		Batch->SetCollisionResponseToAllChannels(
			bCollision ? ECR_Block : ECR_Ignore);
		Batch->SetGenerateOverlapEvents(false);
		Batch->SetCanEverAffectNavigation(false);
		Batch->SetCastShadow(true);
		UMaterialInstanceDynamic* Material =
			UMaterialInstanceDynamic::Create(&PaletteMaterial, Batch);
		Material->SetVectorParameterValue(TEXT("Color"), Color);
		Batch->SetMaterial(0, Material);
		Batch->RegisterComponent();
		return Batch;
	}

	bool CreateLiquidDropPreview(
		FLiquidDropCaptureState& State,
		UWorld& World,
		const FMatterFluxContentRegistry& Registry,
		FString& OutError)
	{
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
			nullptr,
			TEXT("/Engine/BasicShapes/Cube.Cube"));
		UMaterialInterface* PaletteMaterial = LoadObject<UMaterialInterface>(
			nullptr,
			TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette"));
		if (!CubeMesh || !PaletteMaterial)
		{
			OutError = TEXT("Liquid-drop capture could not load cube or palette material");
			return false;
		}

		FActorSpawnParameters Parameters;
		Parameters.Name = MakeUniqueObjectName(
			&World,
			AActor::StaticClass(),
			TEXT("LiquidDropPreview"));
		AActor* Preview = World.SpawnActor<AActor>(Parameters);
		if (!Preview)
		{
			OutError = TEXT("Liquid-drop capture could not spawn preview actor");
			return false;
		}
		USceneComponent* Root = NewObject<USceneComponent>(Preview, TEXT("Root"));
		Preview->AddInstanceComponent(Root);
		Preview->SetRootComponent(Root);
		Root->RegisterComponent();

		TSet<FName> MaterialIds;
		TArray<MatterFlux::Material::FCellSnapshot> Cells;
		State.CustomWorld->GetActiveCells(Cells);
		for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
		{
			MaterialIds.Add(Cell.MaterialId);
		}
		for (const MatterFlux::Material::FCustomMapSceneBox& Box
			: State.Scene.Boxes)
		{
			MaterialIds.Add(Box.MaterialId);
		}
		for (const MatterFlux::Material::FCustomMapPourContainer& Container
			: State.Scene.PourContainers)
		{
			MaterialIds.Add(Container.ContainerMaterialId);
			MaterialIds.Add(Container.LiquidMaterialId);
		}
		TArray<FName> SortedMaterialIds = MaterialIds.Array();
		SortedMaterialIds.Sort(FNameLexicalLess());
		for (const FName MaterialId : SortedMaterialIds)
		{
			const FMatterFluxMaterialDefinition* Definition =
				Registry.Materials.Find(MaterialId);
			if (!Definition)
			{
				OutError = FString::Printf(
					TEXT("3D custom-map preview references missing material '%s'"),
					*MaterialId.ToString());
				return false;
			}
			const bool bMaterialHasCollision = State.Scene.Boxes.ContainsByPredicate(
				[MaterialId](const MatterFlux::Material::FCustomMapSceneBox& Box)
				{
					return Box.MaterialId == MaterialId && Box.bCollision;
				});
			State.Batches.Add(
				MaterialId,
				CreateLiquidDropBatch(
					*Preview,
					*CubeMesh,
					*PaletteMaterial,
					FName(*FString::Printf(
						TEXT("CustomMap_%s"), *MaterialId.ToString())),
					Definition->Color,
					bMaterialHasCollision));
		}
		State.PreviewActor = Preview;
		return true;
	}

	void RefreshLiquidDropPreview(FLiquidDropCaptureState& State)
	{
		for (TPair<FName, TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : State.Batches)
		{
			if (Pair.Value.IsValid())
			{
				Pair.Value->ClearInstances();
			}
		}
		for (const MatterFlux::Material::FCustomMapSceneBox& Box
			: State.Scene.Boxes)
		{
			const TWeakObjectPtr<UInstancedStaticMeshComponent>* Batch =
				State.Batches.Find(Box.MaterialId);
			if (Batch && Batch->IsValid())
			{
				Batch->Get()->AddInstance(FTransform(
					FQuat::Identity,
					Box.Center,
					Box.Size / 100.0));
			}
		}
		const auto AddWorld = [&State](
			const MatterFlux::Material::FChunkedMaterialWorld& MaterialWorld)
		{
			TArray<MatterFlux::Material::FCellSnapshot> Cells;
			MaterialWorld.GetActiveCells(Cells);
			for (const MatterFlux::Material::FCellSnapshot& Cell : Cells)
			{
				const TWeakObjectPtr<UInstancedStaticMeshComponent>* Batch =
					State.Batches.Find(Cell.MaterialId);
				if (!Batch || !Batch->IsValid())
				{
					continue;
				}
				const float CellSize = State.Scene.CellSizeCentimeters;
				const float Thickness = FMath::Max(
					State.Scene.MaterialDepthCells * CellSize,
					2.0f);
				Batch->Get()->AddInstance(FTransform(
					FQuat::Identity,
					FVector(
						Cell.WorldCell.X * CellSize,
						Cell.WorldCell.Y * CellSize,
						Cell.SupportHeight + Thickness * 0.5f),
					FVector(
						CellSize / 100.0f,
						CellSize / 100.0f,
						Thickness / 100.0f)));
			}
		};
		if (State.CustomWorld)
		{
			AddWorld(*State.CustomWorld);
		}
		if (State.PourSimulation)
		{
			MatterFlux::Material::FCustomMapPourSnapshot Snapshot;
			State.PourSimulation->GetSnapshot(Snapshot);
			for (const MatterFlux::Material::FCustomMapPourContainerSnapshot& Container
				: Snapshot.Containers)
			{
				const TWeakObjectPtr<UInstancedStaticMeshComponent>* Batch =
					State.Batches.Find(Container.ContainerMaterialId);
				if (!Batch || !Batch->IsValid())
				{
					continue;
				}
				const FVector Interior = Container.InteriorSize;
				const float Wall = Container.WallThickness;
				const auto AddPanel = [&Container, Batch](
					const FVector LocalCenter,
					const FVector Size)
				{
					Batch->Get()->AddInstance(FTransform(
						Container.Transform.GetRotation(),
						Container.Transform.TransformPosition(LocalCenter),
						Size / 100.0f));
				};
				// 顶部敞口的五面容器；所有面共用一个刚体姿态，不会彼此错位。
				AddPanel(
					FVector(0.0f, 0.0f, -Interior.Z * 0.5f - Wall * 0.5f),
					FVector(Interior.X + Wall * 2.0f,
						Interior.Y + Wall * 2.0f, Wall));
				AddPanel(
					FVector(-Interior.X * 0.5f - Wall * 0.5f, 0.0f, 0.0f),
					FVector(Wall, Interior.Y + Wall * 2.0f, Interior.Z));
				AddPanel(
					FVector(Interior.X * 0.5f + Wall * 0.5f, 0.0f, 0.0f),
					FVector(Wall, Interior.Y + Wall * 2.0f, Interior.Z));
				AddPanel(
					FVector(0.0f, -Interior.Y * 0.5f - Wall * 0.5f, 0.0f),
					FVector(Interior.X, Wall, Interior.Z));
				AddPanel(
					FVector(0.0f, Interior.Y * 0.5f + Wall * 0.5f, 0.0f),
					FVector(Interior.X, Wall, Interior.Z));
			}
			const auto AddVoxels = [&State](const auto& Voxels)
			{
				for (const MatterFlux::Material::FCustomMapPourVoxel& Voxel : Voxels)
				{
					const TWeakObjectPtr<UInstancedStaticMeshComponent>* Batch =
						State.Batches.Find(Voxel.MaterialId);
					if (Batch && Batch->IsValid())
					{
						Batch->Get()->AddInstance(FTransform(
							Voxel.Rotation,
							Voxel.Position,
							Voxel.Size / 100.0f));
					}
				}
			};
			AddVoxels(Snapshot.HeldVoxels);
			AddVoxels(Snapshot.FallingVoxels);
			AddVoxels(Snapshot.SettledVoxels);
		}
	}

	void AppendProjectedBodyCells(
		const FBox& Bounds,
		const float CellSize,
		TSet<FIntPoint>& OutCells)
	{
		if (!Bounds.IsValid || CellSize <= UE_SMALL_NUMBER)
		{
			return;
		}
		const int32 MinX = FMath::FloorToInt(Bounds.Min.X / CellSize);
		const int32 MaxX = FMath::CeilToInt(Bounds.Max.X / CellSize);
		const int32 MinY = FMath::FloorToInt(Bounds.Min.Y / CellSize);
		const int32 MaxY = FMath::CeilToInt(Bounds.Max.Y / CellSize);
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FVector CellCenter(
					X * CellSize,
					Y * CellSize,
					Bounds.GetCenter().Z);
				if (Bounds.IsInsideOrOn(CellCenter))
				{
					OutCells.Add(FIntPoint(X, Y));
				}
			}
		}
	}

	void ApplyCustomMapBodyDisplacement(FLiquidDropCaptureState& State)
	{
		if (!State.CustomWorld)
		{
			return;
		}
		TSet<FIntPoint> OccupiedSet;
		if (State.Character.IsValid()
			&& State.Character->GetCapsuleComponent())
		{
			AppendProjectedBodyCells(
				State.Character->GetCapsuleComponent()->Bounds.GetBox(),
				State.Scene.CellSizeCentimeters,
				OccupiedSet);
		}
		if (State.Creature.IsValid()
			&& State.Creature->GetCapsuleComponent())
		{
			AppendProjectedBodyCells(
				State.Creature->GetCapsuleComponent()->Bounds.GetBox(),
				State.Scene.CellSizeCentimeters,
				OccupiedSet);
		}
		if (State.PhysicsBody.IsValid()
			&& State.PhysicsBody->GetStaticMeshComponent())
		{
			AppendProjectedBodyCells(
				State.PhysicsBody->GetStaticMeshComponent()->Bounds.GetBox(),
				State.Scene.CellSizeCentimeters,
				OccupiedSet);
		}
		TArray<FIntPoint> OccupiedCells = OccupiedSet.Array();
		OccupiedCells.Sort([](const FIntPoint& Left, const FIntPoint& Right)
		{
			return Left.Y == Right.Y
				? Left.X < Right.X
				: Left.Y < Right.Y;
		});
		State.CustomWorld->DisplaceLiquids(OccupiedCells);
	}

	bool PlaceCustomMapDisplacementBodies(
		FLiquidDropCaptureState& State,
		UWorld& World,
		FString& OutError)
	{
		if (!State.Character.IsValid())
		{
			OutError = TEXT("custom-map displacement capture has no player character");
			return false;
		}
		const FVector* LightPool =
			State.Scene.MarkerLocations.Find(TEXT("light_drop_chamber"));
		const FVector* DensePool =
			State.Scene.MarkerLocations.Find(TEXT("dense_drop_chamber"));
		if (!LightPool || !DensePool)
		{
			OutError = TEXT("custom map has no liquid chamber markers");
			return false;
		}

		AMatterFluxCharacter* Character = State.Character.Get();
		const float PlayerHalfHeight = Character->GetCapsuleComponent()
			? Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
			: 88.0f;
		Character->SetActorLocation(
			*DensePool + FVector(-70.0f, 0.0f, PlayerHalfHeight + 2.0f),
			false,
			nullptr,
			ETeleportType::TeleportPhysics);

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AMatterFluxCreatureActor* Creature =
			World.SpawnActor<AMatterFluxCreatureActor>(
				*LightPool + FVector(0.0f, 0.0f, 100.0f),
				FRotator::ZeroRotator,
				SpawnParameters);
		if (!Creature)
		{
			OutError = TEXT("could not spawn custom-map displacement creature");
			return false;
		}
		const float CreatureHalfHeight = Creature->GetCapsuleComponent()
			? Creature->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
			: 88.0f;
		Creature->SetActorLocation(
			*LightPool + FVector(84.0f, 0.0f, CreatureHalfHeight + 2.0f),
			false,
			nullptr,
			ETeleportType::TeleportPhysics);
		Creature->SetActorTickEnabled(false);
		State.Creature = Creature;

		AStaticMeshActor* PhysicsBody = World.SpawnActor<AStaticMeshActor>(
			*DensePool + FVector(90.0f, 0.0f, 34.0f),
			FRotator(0.0f, 25.0f, 0.0f),
			SpawnParameters);
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
			nullptr,
			TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!PhysicsBody || !PhysicsBody->GetStaticMeshComponent() || !CubeMesh)
		{
			OutError = TEXT("could not spawn custom-map displacement physics body");
			return false;
		}
		UStaticMeshComponent* BodyMesh = PhysicsBody->GetStaticMeshComponent();
		BodyMesh->SetMobility(EComponentMobility::Movable);
		BodyMesh->SetStaticMesh(CubeMesh);
		BodyMesh->SetWorldScale3D(FVector(0.68f));
		if (UMaterialInterface* PaletteMaterial = LoadObject<UMaterialInterface>(
				nullptr,
				TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette")))
		{
			UMaterialInstanceDynamic* BodyMaterial =
				UMaterialInstanceDynamic::Create(PaletteMaterial, BodyMesh);
			BodyMaterial->SetVectorParameterValue(
				TEXT("Color"), FLinearColor(0.82f, 0.86f, 0.90f));
			BodyMesh->SetMaterial(0, BodyMaterial);
		}
		BodyMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		BodyMesh->SetCollisionResponseToAllChannels(ECR_Block);
		State.PhysicsBody = PhysicsBody;

		ApplyCustomMapBodyDisplacement(State);
		return true;
	}

	void RequestLiquidDropScreenshot(
		const FLiquidDropCaptureState& State,
		const TCHAR* FileName)
	{
		IFileManager::Get().MakeDirectory(*State.OutputDirectory, true);
		const FString Path = FPaths::Combine(State.OutputDirectory, FileName);
		FScreenshotRequest::RequestScreenshot(Path, true, false, false);
		UE_LOG(LogMatterFlux, Display,
			TEXT("Requested liquid-drop screenshot: %s"), *Path);
	}

	bool FailCustomMap3DCapture(
		const FLiquidDropCaptureState& State,
		const FString& Error)
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("3D custom-map capture failed for '%s': %s"),
			*State.MapId.ToString(),
			*Error);
		GLiquidDropCapturePending = false;
		if (State.bQuitAfterCapture)
		{
			FPlatformMisc::RequestExitWithStatus(false, 4);
		}
		return false;
	}

	bool TickLiquidDropCapture(const TSharedRef<FLiquidDropCaptureState>& State)
	{
		UWorld* World = GEngine && GEngine->GameViewport
			? GEngine->GameViewport->GetWorld()
			: nullptr;
		if (!World || !World->IsGameWorld())
		{
			if (FPlatformTime::Seconds() - State->QueuedAt > 30.0)
			{
				GLiquidDropCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		const double Now = FPlatformTime::Seconds();
		if (State->Phase > 0 && State->PreviewActor.IsValid())
		{
			IsolatePlayableCustomMapPrimitives(
				*World,
				State->PreviewActor.Get(),
				State->Character.Get(),
				State->Creature.Get(),
				State->PhysicsBody.Get());
		}
		if (State->Phase == 0)
		{
			const FMatterFluxContentRegistryPtr Registry =
				IMatterFluxScriptRuntime::Get().GetActiveRegistry();
			AMatterFluxPlayerController* Controller =
				Cast<AMatterFluxPlayerController>(World->GetFirstPlayerController());
			if (!Registry.IsValid() || !Controller)
			{
				return true;
			}
			Controller->EnterGameplayForVisualCapture();
			Controller->HideUIForVisualCapture();
			AMatterFluxCharacter* Character =
				Cast<AMatterFluxCharacter>(Controller->GetPawn());
			if (!Character)
			{
				return true;
			}
			if (GEngine)
			{
				GEngine->ClearOnScreenDebugMessages();
			}
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(true);
				It->SetActorEnableCollision(false);
				It->SetActorTickEnabled(false);
				if (AMatterFluxTwoStoreyHouseActor* House = It->GetGeneratedHouse())
				{
					House->SetActorHiddenInGame(true);
					House->SetActorEnableCollision(false);
				}
			}
			for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(true);
				It->SetActorEnableCollision(false);
			}
			for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(true);
				It->SetActorEnableCollision(false);
			}
			for (TActorIterator<AFragment2DActor> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(true);
				It->SetActorEnableCollision(false);
			}
			for (TActorIterator<AMatterFluxGroundStateChunkActor> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(true);
				It->SetActorEnableCollision(false);
				It->SetActorTickEnabled(false);
			}
			for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
			{
				It->Destroy();
			}
			for (TActorIterator<APawn> It(World); It; ++It)
			{
				if (*It != Character)
				{
					It->SetActorHiddenInGame(true);
					It->SetActorEnableCollision(false);
					It->SetActorTickEnabled(false);
				}
			}

			State->CustomWorld =
				MakeUnique<MatterFlux::Material::FChunkedMaterialWorld>();
			FString Error;
			if (!MatterFlux::Material::BuildCustomMap(
					State->MapId,
					*Registry,
					8403,
					*State->CustomWorld,
					State->Scene,
					Error))
			{
				return FailCustomMap3DCapture(*State, Error);
			}
			if (!State->Scene.PourContainers.IsEmpty())
			{
				State->PourSimulation = MakeUnique<
					MatterFlux::Material::FCustomMapPourSimulation>();
				if (!State->PourSimulation->Initialize(
						State->Scene, *Registry, 8403, Error))
				{
					return FailCustomMap3DCapture(*State, Error);
				}
			}
			if (State->Scene.Cameras.IsEmpty())
			{
				return FailCustomMap3DCapture(
					*State,
					TEXT("map has no authored 3D inspection camera"));
			}
			if (!CreateLiquidDropPreview(*State, *World, *Registry, Error))
			{
				return FailCustomMap3DCapture(*State, Error);
			}

			const FVector* PlayerStart =
				State->Scene.MarkerLocations.Find(TEXT("player_start"));
			if (!PlayerStart)
			{
				return FailCustomMap3DCapture(
					*State,
					TEXT("map has no player_start marker"));
			}
			State->Character = Character;
			Character->SetActorHiddenInGame(false);
			Character->SetActorEnableCollision(true);
			const float CapsuleHalfHeight = Character->GetCapsuleComponent()
				? Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
				: 88.0f;
			Character->SetActorLocation(
				*PlayerStart + FVector(0.0f, 0.0f, CapsuleHalfHeight + 4.0f),
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
			if (Character->CameraBoom)
			{
				Character->CameraBoom->TargetArmLength = 1320.0f;
				Character->CameraBoom->SetRelativeRotation(
					FRotator(-45.0f, -45.0f, 0.0f));
			}
			if (Character->FollowCamera)
			{
				Character->FollowCamera->ProjectionMode =
					ECameraProjectionMode::Perspective;
				Character->FollowCamera->SetFieldOfView(48.0f);
			}
			if (State->PourSimulation && State->bCaptureScreenshots)
			{
				const MatterFlux::Material::FCustomMapSceneCamera& AuthoredCamera =
					State->Scene.Cameras[0];
				FActorSpawnParameters CameraParameters;
				CameraParameters.Name = MakeUniqueObjectName(
					World, ACameraActor::StaticClass(), TEXT("CustomMapCaptureCamera"));
				ACameraActor* CaptureCamera = World->SpawnActor<ACameraActor>(
					AuthoredCamera.Location,
					(AuthoredCamera.Target - AuthoredCamera.Location).Rotation(),
					CameraParameters);
				if (!CaptureCamera)
				{
					return FailCustomMap3DCapture(
						*State, TEXT("could not spawn authored inspection camera"));
				}
				CaptureCamera->GetCameraComponent()->SetFieldOfView(
					AuthoredCamera.FieldOfViewDegrees);
				State->Camera = CaptureCamera;
				Controller->SetViewTarget(CaptureCamera);
			}
			else
			{
				Controller->SetViewTarget(Character);
			}
			RefreshLiquidDropPreview(*State);
			IsolatePlayableCustomMapPrimitives(
				*World,
				State->PreviewActor.Get(),
				Character,
				State->Creature.Get(),
				State->PhysicsBody.Get());
			if (!State->bCaptureScreenshots)
			{
				State->Phase = 7;
				State->LastInteractiveStepAt = Now;
				UE_LOG(LogMatterFlux, Display,
					TEXT("Entered uncapped interactive custom-map mode: map=%s"),
					*State->MapId.ToString());
				return true;
			}
			State->Phase = 1;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (Now - State->PhaseStartedAt < 1.2)
		{
			return true;
		}
		if (State->Phase == 1)
		{
			RequestLiquidDropScreenshot(
				*State,
				State->PourSimulation
					? TEXT("01_Containers_Full.png")
					: TEXT("01_LiquidPools_BeforeBodies.png"));
			State->Phase = 2;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 2)
		{
			if (!State->PourSimulation)
			{
				FString Error;
				if (!PlaceCustomMapDisplacementBodies(*State, *World, Error))
				{
					return FailCustomMap3DCapture(*State, Error);
				}
			}
			const int32 FirstTarget = State->PourSimulation ? 32 : 0;
			for (int32 Step = 0; Step < FirstTarget; ++Step)
			{
				State->CustomWorld->Step();
				ApplyCustomMapBodyDisplacement(*State);
				if (State->PourSimulation)
				{
					State->PourSimulation->Step();
				}
			}
			State->SimulatedSteps = FirstTarget;
			RefreshLiquidDropPreview(*State);
			State->Phase = 3;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 3)
		{
			RequestLiquidDropScreenshot(
				*State,
				State->PourSimulation
					? TEXT("02_Containers_SynchronizedPour.png")
					: TEXT("02_PlayerCreatureObject_Displacing.png"));
			State->Phase = 4;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 4)
		{
			const int32 FinalTarget = State->PourSimulation ? 180 : 120;
			for (; State->SimulatedSteps < FinalTarget; ++State->SimulatedSteps)
			{
				State->CustomWorld->Step();
				ApplyCustomMapBodyDisplacement(*State);
				if (State->PourSimulation)
				{
					State->PourSimulation->Step();
				}
			}
			RefreshLiquidDropPreview(*State);
			State->Phase = 5;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 5)
		{
			RequestLiquidDropScreenshot(
				*State,
				State->PourSimulation
					? TEXT("03_Liquids_SettledAndLayered.png")
					: TEXT("03_BodyVolumes_RemainVacated.png"));
			State->Phase = 6;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (State->Phase == 6)
		{
			UE_LOG(LogMatterFlux, Display,
				TEXT("Playable custom-map capture complete: map=%s output=%s"),
				*State->MapId.ToString(),
				*State->OutputDirectory);
			State->Phase = 7;
			State->LastInteractiveStepAt = Now;
			if (State->bQuitAfterCapture)
			{
				GLiquidDropCapturePending = false;
				FPlatformMisc::RequestExit(false);
				return false;
			}
			if (State->PourSimulation && State->Character.IsValid())
			{
				if (APlayerController* Controller =
					World->GetFirstPlayerController())
				{
					Controller->SetViewTarget(State->Character.Get());
				}
			}
		}
		// quit=0 时保留正常角色、斜视镜头和碰撞，并持续推进材料世界，
		// 让这张自定义地图成为可直接走动的游戏内测试场而非静态展台。
		if (Now - State->LastInteractiveStepAt >= 0.1)
		{
			State->CustomWorld->Step();
			ApplyCustomMapBodyDisplacement(*State);
			if (State->PourSimulation)
			{
				State->PourSimulation->Step();
			}
			RefreshLiquidDropPreview(*State);
			State->LastInteractiveStepAt = Now;
		}
		return true;
	}

	void QueueCustomMap3DCaptureInternal(
		const FName MapId,
		const bool bQuitAfterCapture,
		const bool bCaptureScreenshots = true)
	{
		if (GLiquidDropCapturePending)
		{
			return;
		}
		GLiquidDropCapturePending = true;
		const TSharedRef<FLiquidDropCaptureState> State =
			MakeShared<FLiquidDropCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->PhaseStartedAt = State->QueuedAt;
		State->MapId = MapId;
		State->bQuitAfterCapture = bQuitAfterCapture;
		State->bCaptureScreenshots = bCaptureScreenshots;
		State->OutputDirectory = FPaths::Combine(
			FPaths::ScreenShotDir(),
			TEXT("MatterFluxCustomMap3D"),
			FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickLiquidDropCapture(State);
			}));
	}

	void QueueLiquidDropCapture(const TArray<FString>& Args, UWorld*)
	{
		QueueCustomMap3DCaptureInternal(
			TEXT("test.liquid_density_drops"),
			Args.IsEmpty() || FCString::Atoi(*Args[0]) != 0);
	}

	void QueueContainerPourCapture(const TArray<FString>& Args, UWorld*)
	{
		const bool bUncappedInteractive = !Args.IsEmpty()
			&& (Args[0] == TEXT("2")
				|| Args[0].Equals(TEXT("play"), ESearchCase::IgnoreCase)
				|| Args[0].Equals(TEXT("uncapped"), ESearchCase::IgnoreCase));
		if (bUncappedInteractive)
		{
			if (IConsoleVariable* MaxFps =
				IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
			{
				MaxFps->Set(0.0f, ECVF_SetByConsole);
			}
			if (IConsoleVariable* VSync =
				IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync")))
			{
				VSync->Set(0, ECVF_SetByConsole);
			}
			FApp::SetUseFixedTimeStep(false);
			if (GEngine)
			{
				GEngine->SetMaxFPS(0.0f);
			}
			QueueCustomMap3DCaptureInternal(
				TEXT("test.stacked_container_pour"), false, false);
			return;
		}
		QueueCustomMap3DCaptureInternal(
			TEXT("test.stacked_container_pour"),
			Args.IsEmpty() || FCString::Atoi(*Args[0]) != 0);
	}

	void QueueCustomMap3DCapture(const TArray<FString>& Args, UWorld*)
	{
		const FName MapId = Args.IsEmpty()
			? FName(TEXT("test.liquid_density_drops"))
			: FName(*Args[0]);
		const bool bQuitAfterCapture = Args.Num() < 2
			|| FCString::Atoi(*Args[1]) != 0;
		QueueCustomMap3DCaptureInternal(MapId, bQuitAfterCapture);
	}

	FAutoConsoleCommandWithWorldAndArgs GLiquidDropCaptureCommand(
		TEXT("mf.Visual.LiquidDrops"),
		TEXT("Capture round droplets entering lighter and denser pools: mf.Visual.LiquidDrops [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueLiquidDropCapture));

	FAutoConsoleCommandWithWorldAndArgs GCustomMap3DCaptureCommand(
		TEXT("mf.Visual.CustomMap3D"),
		TEXT("Enter a Lua-authored horizontal gameplay arena and capture three simulation phases: mf.Visual.CustomMap3D [map-id=test.liquid_density_drops] [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueCustomMap3DCapture));

	FAutoConsoleCommandWithWorldAndArgs GContainerPourCaptureCommand(
		TEXT("mf.Visual.ContainerPour"),
		TEXT("Stacked water/acid container test modes: 0=capture then play, 1=capture then exit, 2/play=uncapped play without screenshots"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueContainerPourCapture));

	struct FPhysicsPushCaptureState
	{
		double QueuedAt = 0.0;
		double PhaseStartedAt = 0.0;
		int32 Phase = 0;
		bool bQuitAfterCapture = true;
		bool bAccepted = false;
		FString OutputDirectory;
		FVector PlayerBodyStart = FVector::ZeroVector;
		FVector CreatureBodyStart = FVector::ZeroVector;
		FVector PlayerStart = FVector::ZeroVector;
		FVector CreatureStart = FVector::ZeroVector;
		TWeakObjectPtr<AMatterFluxCharacter> Character;
		TWeakObjectPtr<AMatterFluxCreatureActor> Creature;
		TWeakObjectPtr<AStaticMeshActor> Floor;
		TWeakObjectPtr<AFragment2DActor> PlayerBody;
		TWeakObjectPtr<AFragment2DActor> CreatureBody;
		TWeakObjectPtr<ACameraActor> Camera;
	};

	AFragment2DActor* SpawnPhysicsPushFragment(
		UWorld& World,
		UMaterialInterface& PaletteMaterial,
		const FVector& Location,
		const FLinearColor& Color,
		const TCHAR* StableId)
	{
		FFragmentSourceMask Mask;
		Mask.Width = 4;
		Mask.Height = 4;
		Mask.CellSize = 18.0f;
		Mask.SupportMode = EFragmentSupportMode::None;
		Mask.GeometryStyle = EFragmentSourceGeometryStyle::VoxelBlocks;
		Mask.SolidMask = {
			1, 1, 1, 0,
			1, 1, 1, 1,
			1, 1, 1, 1,
			0, 1, 1, 1
		};
		MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
		if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
			Mask.SolidMask,
			Mask.Width,
			Mask.Height,
			Mask.CellSize,
			Geometry))
		{
			return nullptr;
		}

		FFragmentSpawnPayload Payload;
		Payload.FragmentId = FGuid::NewDeterministicGuid(StableId, 1);
		Payload.OuterContours = MoveTemp(Geometry.OuterContours);
		Payload.HoleContours = MoveTemp(Geometry.HoleContours);
		Payload.TriangleIndices = MoveTemp(Geometry.TriangleIndices);
		Payload.Vertices2D = MoveTemp(Geometry.Vertices2D);
		Payload.CollisionContours = MoveTemp(Geometry.CollisionContours);
		Payload.DetachedVoxelMask = Mask;
		Payload.MaterialId = TEXT("wood");
		Payload.Thickness = Mask.CellSize;
		Payload.InitialTransform = FTransform(Location);
		Payload.Mass = 20.0f;

		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AFragment2DActor* Fragment = World.SpawnActor<AFragment2DActor>(
			Location,
			FRotator::ZeroRotator,
			Parameters);
		if (!Fragment)
		{
			return nullptr;
		}
		Fragment->FragmentMaterial = &PaletteMaterial;
		Fragment->FragmentColor = Color;
		if (!Fragment->InitializeFromPayload(Payload))
		{
			Fragment->Destroy();
			return nullptr;
		}
		return Fragment;
	}

	AStaticMeshActor* SpawnPhysicsPushBox(
		UWorld& World,
		UStaticMesh& CubeMesh,
		UMaterialInterface& PaletteMaterial,
		const FVector& Location,
		const FVector& Scale,
		const FLinearColor& Color,
		const bool bSimulatePhysics)
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(
			Location,
			FRotator::ZeroRotator,
			Parameters);
		if (!Actor || !Actor->GetStaticMeshComponent())
		{
			return nullptr;
		}
		UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
		// Runtime-spawned acceptance geometry must remain movable while its mesh,
		// scale, and collision are configured. The floor simply leaves physics
		// simulation disabled after setup.
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetStaticMesh(&CubeMesh);
		Mesh->SetWorldScale3D(Scale);
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Mesh->SetCollisionResponseToAllChannels(ECR_Block);
		Mesh->SetCollisionObjectType(
			bSimulatePhysics ? ECC_PhysicsBody : ECC_WorldStatic);
		UMaterialInstanceDynamic* Material =
			UMaterialInstanceDynamic::Create(&PaletteMaterial, Mesh);
		Material->SetVectorParameterValue(TEXT("Color"), Color);
		Mesh->SetMaterial(0, Material);
		if (bSimulatePhysics)
		{
			Mesh->SetMassOverrideInKg(NAME_None, 20.0f, true);
			Mesh->SetLinearDamping(2.0f);
			Mesh->SetAngularDamping(4.0f);
			Mesh->SetSimulatePhysics(true);
		}
		return Actor;
	}

	void RequestPhysicsPushScreenshot(
		const FPhysicsPushCaptureState& State,
		const TCHAR* FileName)
	{
		IFileManager::Get().MakeDirectory(*State.OutputDirectory, true);
		const FString Path = FPaths::Combine(State.OutputDirectory, FileName);
		FScreenshotRequest::RequestScreenshot(Path, true, false, false);
		UE_LOG(LogMatterFlux, Display,
			TEXT("Requested physics-push screenshot: %s"), *Path);
	}

	bool TickPhysicsPushCapture(
		const TSharedRef<FPhysicsPushCaptureState>& State)
	{
		UWorld* World = GEngine && GEngine->GameViewport
			? GEngine->GameViewport->GetWorld()
			: nullptr;
		if (!World || !World->IsGameWorld())
		{
			if (FPlatformTime::Seconds() - State->QueuedAt > 30.0)
			{
				GPhysicsPushCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		const double Now = FPlatformTime::Seconds();
		if (State->Phase == 0)
		{
			AMatterFluxPlayerController* Controller =
				Cast<AMatterFluxPlayerController>(World->GetFirstPlayerController());
			AMatterFluxCharacter* Character = Controller
				? Cast<AMatterFluxCharacter>(Controller->GetPawn())
				: nullptr;
			UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
				nullptr,
				TEXT("/Engine/BasicShapes/Cube.Cube"));
			UMaterialInterface* PaletteMaterial = LoadObject<UMaterialInterface>(
				nullptr,
				TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette"));
			if (!Controller || !Character || !CubeMesh || !PaletteMaterial)
			{
				if (Now - State->QueuedAt > 30.0)
				{
					GPhysicsPushCapturePending = false;
					UE_LOG(LogMatterFlux, Error,
						TEXT("Physics-push capture timed out waiting for gameplay assets."));
					if (State->bQuitAfterCapture)
					{
						FPlatformMisc::RequestExitWithStatus(false, 4);
					}
					return false;
				}
				return true;
			}
			Controller->EnterGameplayForVisualCapture();
			Controller->HideUIForVisualCapture();
			if (GEngine)
			{
				GEngine->ClearOnScreenDebugMessages();
				GEngine->Exec(World, TEXT("DisableAllScreenMessages"));
			}

			constexpr float PlatformZ = 3000.0f;
			State->Floor = SpawnPhysicsPushBox(
				*World,
				*CubeMesh,
				*PaletteMaterial,
				FVector(0.0f, 0.0f, PlatformZ),
				FVector(10.0f, 12.0f, 0.20f),
				FLinearColor(0.12f, 0.16f, 0.18f),
				false);
			State->PlayerBody = SpawnPhysicsPushFragment(
				*World,
				*PaletteMaterial,
				FVector(-170.0f, -30.0f, PlatformZ + 46.0f),
				FLinearColor(0.20f, 0.65f, 1.0f),
				TEXT("PhysicsPushPlayerFragment"));
			State->CreatureBody = SpawnPhysicsPushFragment(
				*World,
				*PaletteMaterial,
				FVector(-100.0f, 170.0f, PlatformZ + 46.0f),
				FLinearColor(1.0f, 0.28f, 0.62f),
				TEXT("PhysicsPushCreatureFragment"));

			FActorSpawnParameters CreatureParameters;
			CreatureParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			State->Creature = World->SpawnActor<AMatterFluxCreatureActor>(
				FVector(-330.0f, 170.0f, PlatformZ + 108.0f),
				FRotator::ZeroRotator,
				CreatureParameters);
			if (!State->Floor.IsValid()
				|| !State->PlayerBody.IsValid()
				|| !State->CreatureBody.IsValid()
				|| !State->Creature.IsValid())
			{
				GPhysicsPushCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			if (AController* CreatureController = State->Creature->GetController())
			{
				CreatureController->SetActorTickEnabled(false);
			}
			State->Creature->GetCharacterMovement()->bRunPhysicsWithNoController = true;
			State->Creature->GetCharacterMovement()->MaxWalkSpeed = 300.0f;
			State->Character = Character;
			Character->SetActorLocation(
				FVector(-170.0f, -260.0f, PlatformZ + 116.0f),
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
			Character->GetCharacterMovement()->StopMovementImmediately();

			State->Camera = World->SpawnActor<ACameraActor>(
				FVector(620.0f, -980.0f, PlatformZ + 850.0f),
				FRotator::ZeroRotator);
			if (!State->Camera.IsValid())
			{
				GPhysicsPushCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			const FVector CameraTarget(-40.0f, 0.0f, PlatformZ + 40.0f);
			State->Camera->SetActorRotation(
				(CameraTarget - State->Camera->GetActorLocation()).Rotation());
			State->Camera->GetCameraComponent()->SetFieldOfView(48.0f);
			Controller->SetViewTarget(State->Camera.Get());
			State->Phase = 1;
			State->PhaseStartedAt = Now;
			return true;
		}

		if (State->Phase == 1 && Now - State->PhaseStartedAt >= 1.0)
		{
			State->PlayerBodyStart = State->PlayerBody->GetActorLocation();
			State->CreatureBodyStart = State->CreatureBody->GetActorLocation();
			State->PlayerStart = State->Character->GetActorLocation();
			State->CreatureStart = State->Creature->GetActorLocation();
			RequestPhysicsPushScreenshot(*State, TEXT("01_BeforePush.png"));
			State->Phase = 2;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 2)
		{
			if (Now - State->PhaseStartedAt < 1.5)
			{
				State->Character->AddMovementInput(FVector::YAxisVector, 1.0f, true);
				State->Creature->GetCharacterMovement()->RequestDirectMove(
					FVector::XAxisVector
						* State->Creature->GetCharacterMovement()->MaxWalkSpeed,
					false);
				return true;
			}
			const float PlayerBodyDistance = FVector::Dist2D(
				State->PlayerBodyStart,
				State->PlayerBody->GetActorLocation());
			const float CreatureBodyDistance = FVector::Dist2D(
				State->CreatureBodyStart,
				State->CreatureBody->GetActorLocation());
			const float PlayerDistance = FVector::Dist2D(
				State->PlayerStart,
				State->Character->GetActorLocation());
			const float CreatureDistance = FVector::Dist2D(
				State->CreatureStart,
				State->Creature->GetActorLocation());
			const UPrimitiveComponent* PlayerPrimitive = Cast<UPrimitiveComponent>(
				State->PlayerBody->GetRootComponent());
			const UPrimitiveComponent* CreaturePrimitive = Cast<UPrimitiveComponent>(
				State->CreatureBody->GetRootComponent());
			const float PlayerBodySpeed = PlayerPrimitive
				? PlayerPrimitive->GetPhysicsLinearVelocity().Size2D()
				: MAX_flt;
			const float CreatureBodySpeed = CreaturePrimitive
				? CreaturePrimitive->GetPhysicsLinearVelocity().Size2D()
				: MAX_flt;
			const float LargerDistance = FMath::Max(
				PlayerBodyDistance,
				CreatureBodyDistance);
			const float DistanceRatio = LargerDistance > UE_SMALL_NUMBER
				? FMath::Min(PlayerBodyDistance, CreatureBodyDistance)
					/ LargerDistance
				: 0.0f;
			State->bAccepted = PlayerDistance >= 150.0f
				&& CreatureDistance >= 150.0f
				&& PlayerBodyDistance >= 10.0f
				&& CreatureBodyDistance >= 10.0f
				&& PlayerBodyDistance <= 300.0f
				&& CreatureBodyDistance <= 300.0f
				&& PlayerBodySpeed <= 300.0f
				&& CreatureBodySpeed <= 300.0f
				&& DistanceRatio >= 0.4f;
			UE_LOG(LogMatterFlux, Display,
				TEXT("Physics-push acceptance: accepted=%s playerDistance=%.2f playerBodyDistance=%.2f playerBodySpeed=%.2f creatureDistance=%.2f creatureBodyDistance=%.2f creatureBodySpeed=%.2f distanceRatio=%.3f"),
				State->bAccepted ? TEXT("true") : TEXT("false"),
				PlayerDistance,
				PlayerBodyDistance,
				PlayerBodySpeed,
				CreatureDistance,
				CreatureBodyDistance,
				CreatureBodySpeed,
				DistanceRatio);
			if (!State->bAccepted)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Physics-push acceptance failed: both characters must move equal bodies without launch or severe imbalance."));
			}
			RequestPhysicsPushScreenshot(*State, TEXT("02_AfterPush.png"));
			State->Phase = 3;
			State->PhaseStartedAt = Now;
			return true;
		}
		if (State->Phase == 3 && Now - State->PhaseStartedAt >= 1.0)
		{
			GPhysicsPushCapturePending = false;
			if (State->bQuitAfterCapture)
			{
				FPlatformMisc::RequestExitWithStatus(
					false,
					State->bAccepted ? 0 : 5);
			}
			return false;
		}
		return true;
	}

	void QueuePhysicsPushCapture(const TArray<FString>& Args, UWorld*)
	{
		if (GPhysicsPushCapturePending)
		{
			return;
		}
		GPhysicsPushCapturePending = true;
		const TSharedRef<FPhysicsPushCaptureState> State =
			MakeShared<FPhysicsPushCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->PhaseStartedAt = State->QueuedAt;
		State->bQuitAfterCapture = Args.IsEmpty()
			|| FCString::Atoi(*Args[0]) != 0;
		State->OutputDirectory = FPaths::Combine(
			FPaths::ScreenShotDir(),
			TEXT("MatterFluxPhysicsPush"),
			FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickPhysicsPushCapture(State);
			}));
	}

	FAutoConsoleCommandWithWorldAndArgs GPhysicsPushCaptureCommand(
		TEXT("mf.Visual.PhysicsPush"),
		TEXT("Capture player and creature pushing equal real cut-fragment bodies on both horizontal axes: mf.Visual.PhysicsPush [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueuePhysicsPushCapture));

	struct FAcidReactionCaptureState
	{
		double QueuedAt = 0.0;
		double PhaseStartedAt = 0.0;
		int32 Phase = 0;
		int32 MapSeed = 1337;
		bool bQuitAfterCapture = true;
		bool bRegenerationRequested = false;
		FVector Focus = FVector::ZeroVector;
		FVector TreeBase = FVector::ZeroVector;
		FVector PoolCenter = FVector::ZeroVector;
		FGuid TreeAggregateId;
		FString OutputDirectory;
		TWeakObjectPtr<AMatterFluxPlayableWorldActor> PlayableWorld;
		TWeakObjectPtr<ACameraActor> Camera;
	};

	FVector FindFlatAcidCaptureCenter(
		const AMatterFluxPlayableWorldActor& PlayableWorld,
		const FVector& TreeBase,
		const float CellSize)
	{
		FVector BestCenter = TreeBase + FVector(400.0f, 0.0f, 0.0f);
		float BestHeight = TreeBase.Z;
		float BestHeightRange = MAX_flt;
		for (int32 CandidateY = -24; CandidateY <= 24; CandidateY += 4)
		{
			for (int32 CandidateX = -24; CandidateX <= 24; CandidateX += 4)
			{
				if (CandidateX * CandidateX + CandidateY * CandidateY < 18 * 18)
				{
					continue;
				}
				const FVector Candidate = TreeBase + FVector(
					CandidateX * CellSize,
					CandidateY * CellSize,
					0.0f);
				float MinimumHeight = MAX_flt;
				float MaximumHeight = -MAX_flt;
				bool bCompletePatch = true;
				for (int32 LocalY = -7; LocalY <= 7 && bCompletePatch; ++LocalY)
				{
					for (int32 LocalX = -7; LocalX <= 7; ++LocalX)
					{
						float Height = 0.0f;
						if (!PlayableWorld.TrySampleTerrainHeightAtWorldLocation(
							Candidate + FVector(
								LocalX * CellSize,
								LocalY * CellSize,
								0.0f),
							Height))
						{
							bCompletePatch = false;
							break;
						}
						MinimumHeight = FMath::Min(MinimumHeight, Height);
						MaximumHeight = FMath::Max(MaximumHeight, Height);
					}
				}
				const float HeightRange = MaximumHeight - MinimumHeight;
				if (bCompletePatch && HeightRange < BestHeightRange)
				{
					BestHeightRange = HeightRange;
					BestHeight = (MinimumHeight + MaximumHeight) * 0.5f;
					BestCenter = Candidate;
				}
			}
		}
		BestCenter.Z = BestHeight;
		UE_LOG(LogMatterFlux, Display,
			TEXT("Acid puddle capture selected a %.1f cm height-range patch at %s"),
			BestHeightRange,
			*BestCenter.ToCompactString());
		return BestCenter;
	}

	void RequestAcidReactionScreenshot(
		const FAcidReactionCaptureState& State,
		const TCHAR* FileName)
	{
		const FString Path = FPaths::Combine(State.OutputDirectory, FileName);
		IFileManager::Get().MakeDirectory(*State.OutputDirectory, true);
		FScreenshotRequest::RequestScreenshot(Path, true, false, false);
		UE_LOG(LogMatterFlux, Display,
			TEXT("Requested acid-reaction screenshot: %s"), *Path);
	}

	void PlaceAcidReactionCamera(
		FAcidReactionCaptureState& State,
		const FVector& RelativeLocation)
	{
		ACameraActor* Camera = State.Camera.Get();
		if (!Camera)
		{
			return;
		}
		const FVector CameraLocation = State.Focus + RelativeLocation;
		Camera->SetActorLocationAndRotation(
			CameraLocation,
			(State.Focus - CameraLocation).Rotation());
		if (UCameraComponent* CameraComponent = Camera->GetCameraComponent())
		{
			CameraComponent->ProjectionMode = ECameraProjectionMode::Perspective;
			CameraComponent->FieldOfView = 44.0f;
			CameraComponent->bConstrainAspectRatio = false;
			CameraComponent->PostProcessBlendWeight = 1.0f;
			CameraComponent->PostProcessSettings.bOverride_AutoExposureMethod = true;
			CameraComponent->PostProcessSettings.AutoExposureMethod = AEM_Manual;
			CameraComponent->PostProcessSettings
				.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
			CameraComponent->PostProcessSettings
				.AutoExposureApplyPhysicalCameraExposure = false;
			CameraComponent->PostProcessSettings.bOverride_MotionBlurAmount = true;
			CameraComponent->PostProcessSettings.MotionBlurAmount = 0.0f;
			CameraComponent->PostProcessSettings.bOverride_VignetteIntensity = true;
			CameraComponent->PostProcessSettings.VignetteIntensity = 0.0f;
		}
	}

	bool TickAcidReactionCapture(
		const TSharedRef<FAcidReactionCaptureState>& State)
	{
		UWorld* World = GEngine && GEngine->GameViewport
			? GEngine->GameViewport->GetWorld()
			: nullptr;
		const double Now = FPlatformTime::Seconds();
		if (!World || !World->IsGameWorld())
		{
			if (Now - State->QueuedAt > 30.0)
			{
				GAcidReactionCapturePending = false;
				if (State->bQuitAfterCapture)
				{
					FPlatformMisc::RequestExitWithStatus(false, 4);
				}
				return false;
			}
			return true;
		}

		AMatterFluxPlayerController* Controller =
			Cast<AMatterFluxPlayerController>(World->GetFirstPlayerController());
		if (State->Camera.IsValid() && Controller
			&& Controller->GetViewTarget() != State->Camera.Get())
		{
			Controller->SetViewTarget(State->Camera.Get());
		}
		if (Now < State->PhaseStartedAt)
		{
			return true;
		}

		if (State->Phase == 0)
		{
			AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				PlayableWorld = *It;
				break;
			}
			if (!PlayableWorld || !Controller)
			{
				return true;
			}
			Controller->EnterGameplayForVisualCapture();
			Controller->HideUIForVisualCapture();
			if (GEngine)
			{
				GEngine->ClearOnScreenDebugMessages();
			}
			if (PlayableWorld->GetMapSeed() != State->MapSeed
				&& !State->bRegenerationRequested)
			{
				State->bRegenerationRequested = true;
				PlayableWorld->Regenerate(State->MapSeed);
				return true;
			}
			if (PlayableWorld->IsGenerationInProgress()
				|| PlayableWorld->GetCachedFragmentSourceCount() <= 0)
			{
				return true;
			}

			FGuid AggregateId;
			FGuid RootSourceId;
			FBox TreeBounds(ForceInit);
			FTransform RootTransform = FTransform::Identity;
			const FVector SearchFocus = Controller->GetPawn()
				? Controller->GetPawn()->GetActorLocation()
				: FVector::ZeroVector;
			if (!PlayableWorld->FindNearestTreeAggregateForVisualInspection(
				SearchFocus,
				AggregateId,
				RootSourceId,
				TreeBounds,
				RootTransform))
			{
				return true;
			}

			State->PlayableWorld = PlayableWorld;
			State->TreeAggregateId = AggregateId;
			State->TreeBase = FVector(
				RootTransform.GetLocation().X,
				RootTransform.GetLocation().Y,
				TreeBounds.IsValid ? TreeBounds.Min.Z : RootTransform.GetLocation().Z);
			const float CellSize = MatterFlux::PlayableLevel::TerrainCellSize;
			State->PoolCenter = FindFlatAcidCaptureCenter(
				*PlayableWorld,
				State->TreeBase,
				CellSize);
			State->Focus = State->PoolCenter + FVector(0.0f, 0.0f, 30.0f);
			PlayableWorld->SetWorldStreamingFocus(State->PoolCenter);
			for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(true);
			}
			for (TActorIterator<AMatterFluxCharacter> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(true);
			}

			// 两个长方形只作为非平衡初态；固定步浅水模拟会在截图前
			// 将它们松弛成带种子扰动的近圆液滩。试样与树保持足够距离，
			// 避免第一张图提前触发 Source 腐蚀。
			for (int32 Y = -4; Y <= 4; ++Y)
			{
				for (int32 X = -2; X <= 1; ++X)
				{
					PlayableWorld->SetSimulatedMaterialAtWorldLocation(
						State->PoolCenter
							+ FVector(X * CellSize, Y * CellSize, 0.0f),
						TEXT("acid"));
				}
				for (int32 X = 12; X <= 15; ++X)
				{
					PlayableWorld->SetSimulatedMaterialAtWorldLocation(
						State->PoolCenter
							+ FVector(X * CellSize, Y * CellSize, 0.0f),
						TEXT("water"));
				}
			}

			State->Camera = World->SpawnActor<ACameraActor>();
			if (!State->Camera.IsValid())
			{
				return true;
			}
			Controller->SetViewTarget(State->Camera.Get());
			PlaceAcidReactionCamera(
				*State,
				FVector(0.0f, -180.0f, 620.0f));
			State->OutputDirectory = FPaths::Combine(
				FPaths::ScreenShotDir(),
				TEXT("MatterFluxAcidReaction"),
				FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
			State->Phase = 1;
			// 0.05 秒固定步下等待约 96 步，让长方形初态完成液面均衡。
			State->PhaseStartedAt = Now + 4.8;
			return true;
		}

		if (State->Phase == 1)
		{
			if (Controller)
			{
				Controller->SetPause(true);
			}
			RequestAcidReactionScreenshot(
				*State,
				TEXT("01_AcidWater_BeforeReaction.png"));
			State->Phase = 2;
			State->PhaseStartedAt = Now + 0.8;
			return true;
		}

		if (State->Phase == 2)
		{
			AMatterFluxPlayableWorldActor* PlayableWorld = State->PlayableWorld.Get();
			if (!PlayableWorld)
			{
				return true;
			}
			// 第一张完成后才把少量酸写到树根，避免“反应前”构图已经
			// 把整棵树切散；这里同时验证液体与真实 mask Source 的桥接。
			PlayableWorld->SetSimulatedMaterialAtWorldLocation(
				State->TreeBase,
				TEXT("acid"));
			PlayableWorld->SetWorldStreamingFocus(State->TreeBase);
			for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(false);
			}
			for (TActorIterator<AMatterFluxCharacter> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(false);
			}
			// 腐蚀帧以树根而非树冠为视觉中心，避免树叶遮住液体接触点。
			State->Focus = State->TreeBase + FVector(0.0f, 0.0f, 52.0f);
			PlaceAcidReactionCamera(
				*State,
				FVector(-290.0f, -340.0f, 190.0f));
			if (Controller)
			{
				Controller->SetPause(false);
			}
			State->Phase = 3;
			State->PhaseStartedAt = Now + 0.35;
			return true;
		}

		if (State->Phase == 3)
		{
			RequestAcidReactionScreenshot(
				*State,
				TEXT("02_TreeCorrosion_AndGas.png"));
			State->Focus = State->TreeBase + FVector(0.0f, 0.0f, 55.0f);
			PlaceAcidReactionCamera(
				*State,
				FVector(30.0f, -100.0f, 610.0f));
			State->Phase = 4;
			State->PhaseStartedAt = Now + 0.8;
			return true;
		}

		if (State->Phase == 4)
		{
			RequestAcidReactionScreenshot(
				*State,
				TEXT("03_HighAngle_AcidWaterBoundary.png"));
			// 高角度图用于看液体分层；再冻结现场并从树根另一侧近拍，
			// 专门验收前后树干切片是否形成同高的方柱树桩。
			State->Focus = State->TreeBase + FVector(0.0f, 0.0f, 28.0f);
			PlaceAcidReactionCamera(
				*State,
				FVector(-190.0f, -235.0f, 150.0f));
			State->Phase = 5;
			State->PhaseStartedAt = Now + 0.25;
			return true;
		}

		if (State->Phase == 5)
		{
			if (Controller)
			{
				Controller->SetPause(true);
			}
			// 这张结构检查图只保留树桩本身；动态倒木会自然落在树根附近，
			// 若不隔离会把真正的 2x2 方柱树桩完全挡住。正常游戏遮挡关系
			// 已由前两张未隔离截图覆盖。
			for (TActorIterator<AFragment2DActor> It(World); It; ++It)
			{
				It->SetActorHiddenInGame(true);
			}
			for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
			{
				if (It->AggregateId != State->TreeAggregateId)
				{
					It->SetActorHiddenInGame(true);
				}
			}
			RequestAcidReactionScreenshot(
				*State,
				TEXT("04_IsolatedSynchronizedStump.png"));
			AMatterFluxPlayableWorldActor* PlayableWorld = State->PlayableWorld.Get();
			UE_LOG(LogMatterFlux, Display,
				TEXT("Acid-reaction capture complete: acid=%d water=%d gas=%d output=%s"),
				PlayableWorld ? PlayableWorld->GetSimulatedMaterialCount(TEXT("acid")) : -1,
				PlayableWorld ? PlayableWorld->GetSimulatedMaterialCount(TEXT("water")) : -1,
				PlayableWorld ? PlayableWorld->GetSimulatedMaterialCount(TEXT("acid_gas")) : -1,
				*State->OutputDirectory);
			GAcidReactionCapturePending = false;
			if (State->bQuitAfterCapture)
			{
				FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([](float)
					{
						FPlatformMisc::RequestExit(false);
						return false;
					}),
					1.5f);
			}
			return false;
		}
		return true;
	}

	void QueueAcidReactionCapture(const TArray<FString>& Args, UWorld*)
	{
		if (GAcidReactionCapturePending)
		{
			return;
		}
		GAcidReactionCapturePending = true;
		const TSharedRef<FAcidReactionCaptureState> State =
			MakeShared<FAcidReactionCaptureState>();
		State->QueuedAt = FPlatformTime::Seconds();
		State->PhaseStartedAt = State->QueuedAt;
		State->MapSeed = Args.Num() > 0
			? FMath::Max(FCString::Atoi(*Args[0]), 1)
			: 1337;
		State->bQuitAfterCapture = Args.Num() <= 1
			|| FCString::Atoi(*Args[1]) != 0;
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float)
			{
				return TickAcidReactionCapture(State);
			}));
	}

	FAutoConsoleCommandWithWorldAndArgs GAcidReactionCaptureCommand(
		TEXT("mf.Visual.AcidReaction"),
		TEXT("Capture acid/water contact and tree corrosion: mf.Visual.AcidReaction [map-seed=1337] [quit=1]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueAcidReactionCapture));
}
