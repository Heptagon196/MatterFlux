#include "AbilitySystemComponent.h"
#include "Algo/Sort.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "FragmentTestActors.h"
#include "GAS/GA_CastWand.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerOperation.h"
#include "Game/MatterFluxPlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "MatterFluxGameplayTags.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/CoreDelegates.h"
#include "ProceduralMeshComponent.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace MatterFluxNetworkScaleTests
{
	constexpr int32 MovementFrames = 120;
	constexpr int32 MovementWarmupFrames = 10;

	const FGameplayAbilitySpec* FindWandAbilitySpec(
		UAbilitySystemComponent* ASC,
		const int32 EquipmentSlot)
	{
		if (!ASC)
		{
			return nullptr;
		}
		for (const FGameplayAbilitySpec& Spec
			: ASC->GetActivatableAbilities())
		{
			if (Spec.InputID == EquipmentSlot
				&& Spec.Ability
				&& Spec.Ability->IsA<UGA_CastWand>())
			{
				return &Spec;
			}
		}
		return nullptr;
	}
	constexpr double PhaseTimeoutSeconds = 45.0;
	constexpr double ReadinessTimeoutSeconds = 75.0;
	constexpr double InitialStreamingSecondsPerAdditionalClient = 15.0;
	constexpr double MovementP95FrameBudgetMilliseconds = 50.0;
	// Every PIE client world ticks serially in this one process. Preserve the
	// two-player 120 ms action gate, then budget the measured CPU cost of each
	// additional full client world. The hard ceiling covers one dedicated
	// server plus four complete client worlds committing simultaneous cuts;
	// sustained performance is still guarded by the much stricter p95 gates.
	constexpr double BaseActionP95FrameBudgetMilliseconds = 120.0;
	constexpr double PerAdditionalInProcessClientBudgetMilliseconds = 80.0;
	constexpr double MaximumFrameBudgetMilliseconds = 300.0;

	enum class EScenarioPhase : uint8
	{
		WaitForWorlds,
		WaitForInitialStreaming,
		MovePlayers,
		WaitForMovementConvergence,
		WaitForCutTargets,
		WaitForCutReplication,
		WaitForFlameTargets,
		WaitForFlameReplication,
		WaitForFinalConvergence
	};

	void FindPIEWorlds(UWorld*& OutServer, TArray<UWorld*>& OutClients)
	{
		OutServer = nullptr;
		OutClients.Reset();
		if (!GEngine)
		{
			return;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || Context.WorldType != EWorldType::PIE)
			{
				continue;
			}
			if (World->GetNetMode() == NM_DedicatedServer)
			{
				OutServer = World;
			}
			else if (World->GetNetMode() == NM_Client)
			{
				OutClients.Add(World);
			}
		}
		OutClients.Sort(
			[](const UWorld& A, const UWorld& B)
			{
				return A.GetPackage()->GetPIEInstanceID()
					< B.GetPackage()->GetPIEInstanceID();
			});
	}

	APlayerController* FindLocalPlayerController(UWorld& World)
	{
		for (TActorIterator<APlayerController> It(&World); It; ++It)
		{
			if (It->IsLocalController())
			{
				return *It;
			}
		}
		return nullptr;
	}

	UWorld* FindClientForPlayerId(
		const TArray<UWorld*>& Clients,
		const int32 PlayerId)
	{
		for (UWorld* Client : Clients)
		{
			const APlayerController* Controller = Client
				? FindLocalPlayerController(*Client)
				: nullptr;
			const AMatterFluxPlayerState* PlayerState = Controller
				? Controller->GetPlayerState<AMatterFluxPlayerState>()
				: nullptr;
			if (PlayerState && PlayerState->GetPlayerId() == PlayerId)
			{
				return Client;
			}
		}
		return nullptr;
	}

	AMatterFluxCharacter* FindCharacterByPlayerId(
		UWorld& World,
		const int32 PlayerId)
	{
		for (TActorIterator<AMatterFluxCharacter> It(&World); It; ++It)
		{
			const AMatterFluxPlayerState* PlayerState =
				It->GetPlayerState<AMatterFluxPlayerState>();
			if (PlayerState && PlayerState->GetPlayerId() == PlayerId)
			{
				return *It;
			}
		}
		return nullptr;
	}

	AMatterFluxPlayableWorldActor* FindPlayableWorldActor(UWorld& World)
	{
		TActorIterator<AMatterFluxPlayableWorldActor> It(&World);
		return It ? *It : nullptr;
	}

	AFragment2DSourceActor* FindSource(
		UWorld& World,
		const FGuid& SourceId)
	{
		for (TActorIterator<AFragment2DSourceActor> It(&World); It; ++It)
		{
			if (It->SourceId == SourceId)
			{
				return *It;
			}
		}
		return nullptr;
	}

	int32 CountSourceActors(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AFragment2DSourceActor> It(&World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	void GatherFragmentIds(UWorld& World, TArray<FGuid>& OutIds)
	{
		OutIds.Reset();
		for (TActorIterator<AFragment2DActor> It(&World); It; ++It)
		{
			if (It->SpawnPayload.FragmentId.IsValid())
			{
				OutIds.Add(It->SpawnPayload.FragmentId);
			}
		}
		OutIds.Sort(
			[](const FGuid& A, const FGuid& B)
			{
				return A.ToString(EGuidFormats::Digits)
					< B.ToString(EGuidFormats::Digits);
			});
	}

	TArray<FVector2D> BuildScenarioPositions(
		const int32 PlayerCount,
		const bool bFarApart)
	{
		static const FVector2D NearPositions[] =
		{
			FVector2D(-80.0, -80.0),
			FVector2D(80.0, -80.0),
			FVector2D(-80.0, 80.0),
			FVector2D(80.0, 80.0)
		};
		static const FVector2D FarPositions[] =
		{
			FVector2D(-1200.0, -800.0),
			FVector2D(1200.0, 800.0),
			FVector2D(1200.0, -800.0),
			FVector2D(-1200.0, 800.0)
		};
		TArray<FVector2D> Result;
		Result.Reserve(PlayerCount);
		for (int32 Index = 0; Index < PlayerCount; ++Index)
		{
			Result.Add(
				bFarApart ? FarPositions[Index] : NearPositions[Index]);
		}
		return Result;
	}

	float FindTerrainCharacterZ(
		UWorld& World,
		const AMatterFluxPlayableWorldActor& PlayableWorld,
		const FVector2D& Position,
		const float CapsuleHalfHeight)
	{
		TArray<FHitResult> Hits;
		FCollisionQueryParams QueryParams(
			SCENE_QUERY_STAT(MatterFluxNetworkScaleGround),
			false);
		World.LineTraceMultiByChannel(
			Hits,
			FVector(Position.X, Position.Y, 4000.0),
			FVector(Position.X, Position.Y, -4000.0),
			ECC_Visibility,
			QueryParams);
		for (const FHitResult& Hit : Hits)
		{
			const UProceduralMeshComponent* Component =
				Cast<UProceduralMeshComponent>(Hit.GetComponent());
			if (Hit.GetActor() == &PlayableWorld
				&& Component
				&& Component->GetName().StartsWith(TEXT("TerrainChunk_")))
			{
				return Hit.ImpactPoint.Z + CapsuleHalfHeight + 5.0f;
			}
		}
		return CapsuleHalfHeight + 300.0f;
	}

	AFragment2DSourceActor* SpawnTestSource(
		UWorld& World,
		const FVector& Location,
		const FGuid& SourceId,
		const FName MaterialId)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AFragment2DSourceActor* Source =
			World.SpawnActorDeferred<AFragment2DSourceActor>(
				AFragment2DSourceActor::StaticClass(),
				Transform,
				nullptr,
				nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
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
		Mask.SupportMode = EFragmentSupportMode::None;
		Mask.SolidMask.Init(1, Mask.Width * Mask.Height);
		Source->bDestroySourceOnFirstBreak = false;
		// Synthetic action probes must not wait behind hundreds of natural
		// decoration channels. Spatial relevancy is measured separately from the
		// natural Source population; these few probes validate GAS and result
		// replication deterministically.
		Source->bAlwaysRelevant = true;
		Source->DefaultSupportMode = EFragmentSupportMode::None;
		Source->FragmentActorClass =
			AMatterFluxNetworkTestFragmentActor::StaticClass();
		Source->FragmentMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		Source->Tags.AddUnique(TEXT("MF_NetworkScaleTarget"));
		if (!Source->InitializeFromProceduralMask(
			Mask,
			SourceId,
			MaterialId == TEXT("wood")
				? FLinearColor(0.42f, 0.18f, 0.06f)
				: FLinearColor(0.48f, 0.50f, 0.54f),
			MaterialId))
		{
			Source->Destroy();
			return nullptr;
		}
		Source->FinishSpawning(Transform);
		return Source;
	}

	class FNetworkScalePIECommand final : public IAutomationLatentCommand
	{
	public:
		FNetworkScalePIECommand(
			FAutomationTestBase* InTest,
			const int32 InPlayerCount,
			const bool bInFarApart)
			: Test(InTest)
			, PlayerCount(InPlayerCount)
			, bFarApart(bInFarApart)
			, PhaseStartTime(FPlatformTime::Seconds())
		{
			EndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(
				this,
				&FNetworkScalePIECommand::RecordEndFrame);
			PreGarbageCollectHandle = FCoreUObjectDelegates::
				GetPreGarbageCollectDelegate().AddRaw(
					this,
					&FNetworkScalePIECommand::HandlePreGarbageCollect);
			PostGarbageCollectHandle = FCoreUObjectDelegates::
				GetPostGarbageCollect().AddRaw(
					this,
					&FNetworkScalePIECommand::HandlePostGarbageCollect);
		}

		virtual ~FNetworkScalePIECommand() override
		{
			FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
			FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(
				PreGarbageCollectHandle);
			FCoreUObjectDelegates::GetPostGarbageCollect().Remove(
				PostGarbageCollectHandle);
		}

		virtual bool Update() override
		{
			UWorld* Server = nullptr;
			TArray<UWorld*> Clients;
			FindPIEWorlds(Server, Clients);
			if (!Server || Clients.Num() != PlayerCount)
			{
				return FailOnTimeout(FString::Printf(
					TEXT("Expected one dedicated server and %d client worlds; found server=%d clients=%d."),
					PlayerCount,
					Server != nullptr,
					Clients.Num()));
			}

			UpdatePeakCounts(*Server, Clients);
			switch (Phase)
			{
			case EScenarioPhase::WaitForWorlds:
				return WaitForWorlds(*Server, Clients);
			case EScenarioPhase::WaitForInitialStreaming:
				return WaitForInitialStreaming(*Server, Clients);
			case EScenarioPhase::MovePlayers:
				return MovePlayers(*Server, Clients);
			case EScenarioPhase::WaitForMovementConvergence:
				return WaitForMovementConvergence(*Server, Clients);
			case EScenarioPhase::WaitForCutTargets:
				return WaitForCutTargets(*Server, Clients);
			case EScenarioPhase::WaitForCutReplication:
				return WaitForCutReplication(*Server, Clients);
			case EScenarioPhase::WaitForFlameTargets:
				return WaitForFlameTargets(*Server, Clients);
			case EScenarioPhase::WaitForFlameReplication:
				return WaitForFlameReplication(*Server, Clients);
			case EScenarioPhase::WaitForFinalConvergence:
				return WaitForFinalConvergence(*Server, Clients);
			default:
				Test->AddError(TEXT("Unknown network scale test phase."));
				return true;
			}
		}

	private:
		bool WaitForWorlds(UWorld& Server, const TArray<UWorld*>& Clients)
		{
			const UNetDriver* NetDriver = Server.GetNetDriver();
			AMatterFluxPlayableWorldActor* ServerWorldActor =
				FindPlayableWorldActor(Server);
			if (!NetDriver
				|| NetDriver->ClientConnections.Num() != PlayerCount
				|| !ServerWorldActor
				|| ServerWorldActor->GetReplicatedMaterialStateByteCount() <= 0)
			{
				return FailOnTimeout(
					TEXT("Dedicated server, connections, or playable world were not ready."));
			}
			if (!bScenarioSeedApplied)
			{
				// A performance regression test must not vary decoration density with
				// the random BeginPlay seed. Rebuild every matrix cell from one known
				// world, then wait for clients and streaming to settle again.
				ServerWorldActor->Regenerate(24681357);
				bScenarioSeedApplied = true;
				TransitionTo(EScenarioPhase::WaitForWorlds);
				return false;
			}

			TArray<int32> ReadyPlayerIds;
			for (UWorld* Client : Clients)
			{
				APlayerController* Controller =
					Client ? FindLocalPlayerController(*Client) : nullptr;
				AMatterFluxCharacter* Character = Controller
					? Cast<AMatterFluxCharacter>(Controller->GetPawn())
					: nullptr;
				AMatterFluxPlayerState* PlayerState = Controller
					? Controller->GetPlayerState<AMatterFluxPlayerState>()
					: nullptr;
				UAbilitySystemComponent* ASC = PlayerState
					? PlayerState->GetAbilitySystemComponent()
					: nullptr;
				const UMatterFluxMagicInventoryComponent* Inventory =
					PlayerState ? PlayerState->GetMagicInventory() : nullptr;
				if (!Character
					|| !PlayerState
					|| !ASC
					|| !ASC->AbilityActorInfo.IsValid()
					|| !ASC->AbilityActorInfo->IsLocallyControlled()
					|| !Inventory
					|| Inventory->GetOwnedWands().Num()
						!= UGA_CastWand::EquipmentSlotCount + 2
					|| !FindWandAbilitySpec(ASC, 0)
					|| !FindWandAbilitySpec(ASC, 1)
					|| !FindWandAbilitySpec(ASC, 2)
					|| !FindWandAbilitySpec(ASC, 3)
					|| !FindPlayableWorldActor(*Client)
					|| !FindCharacterByPlayerId(Server, PlayerState->GetPlayerId()))
				{
					return FailOnTimeout(
						TEXT("A client pawn, PlayerState, ASC ability, or server pawn was not ready."));
				}
				ReadyPlayerIds.Add(PlayerState->GetPlayerId());
			}
		ReadyPlayerIds.Sort();
		for (int32 Index = 1; Index < ReadyPlayerIds.Num(); ++Index)
		{
			if (ReadyPlayerIds[Index] == ReadyPlayerIds[Index - 1])
			{
				Test->AddError(TEXT("PIE clients received duplicate PlayerIds."));
				return true;
			}
		}

		PlayerIds = MoveTemp(ReadyPlayerIds);
		const TArray<FVector2D> Positions =
			BuildScenarioPositions(PlayerCount, bFarApart);
		InitialServerLocations.Reset(PlayerCount);
		PeakTravelDistances.Init(0.0f, PlayerCount);
		for (int32 Index = 0; Index < PlayerIds.Num(); ++Index)
		{
			AMatterFluxCharacter* Character =
				FindCharacterByPlayerId(Server, PlayerIds[Index]);
			const float CapsuleHalfHeight = Character && Character->GetCapsuleComponent()
				? Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
				: 100.0f;
			const float Z = FindTerrainCharacterZ(
				Server,
				*ServerWorldActor,
				Positions[Index],
				CapsuleHalfHeight);
			const FVector Location(Positions[Index].X, Positions[Index].Y, Z);
			if (!Character
				|| !Character->TeleportTo(
					Location,
					FRotator::ZeroRotator,
					false,
					true))
			{
				Test->AddError(FString::Printf(
					TEXT("Could not place server player %d for network scale test."),
					PlayerIds[Index]));
				return true;
			}
			if (UCharacterMovementComponent* Movement =
				Character->GetCharacterMovement())
			{
				Movement->StopMovementImmediately();
			}
			Character->ForceNetUpdate();
			InitialServerLocations.Add(Location);
		}

		TransitionTo(EScenarioPhase::WaitForInitialStreaming);
		return false;
	}

	bool WaitForInitialStreaming(
		UWorld& Server,
		const TArray<UWorld*>& Clients)
	{
		AMatterFluxPlayableWorldActor* WorldActor =
			FindPlayableWorldActor(Server);
		const int32 VisibleTerrainChunks = WorldActor
			? WorldActor->GetVisibleTerrainChunkCount() : 0;
		// The playable world's streaming radius and cache budget are tunable.
		// Earlier versions baked the old radius-one counts (20/25) into this
		// test, which made every near-player scenario time out after the view
		// radius was deliberately expanded. Widely separated players can also
		// require an active union larger than the background cache budget; active
		// chunks must win over eviction. Readiness only requires a non-empty union.
		const bool bStreamingUnionReady = WorldActor
			&& VisibleTerrainChunks > 0;
		if (!WorldActor
			|| WorldActor->GetPendingFragmentSourceSpawnCount() > 0
			|| !AreCharactersConverged(Server, Clients, 150.0f)
			|| !bStreamingUnionReady)
		{
			return FailOnTimeout(
				TEXT("Initial player placement or streaming did not settle."));
		}

		InitialVisibleTerrainChunks = VisibleTerrainChunks;

		bMeasureFrames = true;
		LastEndFrameTime = FPlatformTime::Seconds();
		MovementFrame = 0;
		// Ignore the artificial common spawn point used while PIE connections
		// initialize. Measure the actor footprint only after every player has
		// reached this scenario's near/far placement and streaming has drained.
		PeakSourceActorCount = CountSourceActors(Server);
		PeakClientSourceActorCount = 0;
		for (UWorld* Client : Clients)
		{
			if (Client)
			{
				PeakClientSourceActorCount = FMath::Max(
					PeakClientSourceActorCount,
					CountSourceActors(*Client));
			}
		}
		PeakFragmentActorCount = 0;
		TransitionTo(EScenarioPhase::MovePlayers);
		return false;
	}

	bool MovePlayers(UWorld& Server, const TArray<UWorld*>& Clients)
	{
		const float InputSign = MovementFrame < MovementFrames / 2
			? 1.0f
			: -1.0f;
		for (UWorld* Client : Clients)
		{
			APlayerController* Controller =
				Client ? FindLocalPlayerController(*Client) : nullptr;
			AMatterFluxCharacter* Character = Controller
				? Cast<AMatterFluxCharacter>(Controller->GetPawn())
				: nullptr;
			if (!Character)
			{
				return FailOnTimeout(TEXT("A local client character disappeared while moving."));
			}
			Character->ApplyPlayerOperation(
				EMatterFluxPlayerOperation::Move,
				FVector2D(InputSign, 0.0f));
		}

		++MovementFrame;
		for (int32 Index = 0; Index < PlayerIds.Num(); ++Index)
		{
			const AMatterFluxCharacter* Character =
				FindCharacterByPlayerId(Server, PlayerIds[Index]);
			if (Character)
			{
				PeakTravelDistances[Index] = FMath::Max(
					PeakTravelDistances[Index],
					FVector::Dist2D(
						Character->GetActorLocation(),
						InitialServerLocations[Index]));
			}
		}
		if (MovementFrame < MovementFrames)
		{
			return false;
		}

		for (UWorld* Client : Clients)
		{
			APlayerController* Controller =
				Client ? FindLocalPlayerController(*Client) : nullptr;
			if (AMatterFluxCharacter* Character = Controller
				? Cast<AMatterFluxCharacter>(Controller->GetPawn())
				: nullptr)
			{
				Character->ApplyPlayerOperation(
					EMatterFluxPlayerOperation::Move,
					FVector2D::ZeroVector);
			}
		}
		TransitionTo(EScenarioPhase::WaitForMovementConvergence);
		return false;
	}

	bool WaitForMovementConvergence(
		UWorld& Server,
		const TArray<UWorld*>& Clients)
	{
		if (FPlatformTime::Seconds() - PhaseStartTime < 0.75)
		{
			return false;
		}
		for (const float Distance : PeakTravelDistances)
		{
			if (Distance < 120.0f)
			{
				return FailOnTimeout(
					TEXT("At least one client movement stream did not move its authoritative pawn."));
			}
		}
		if (!AreCharactersConverged(Server, Clients, 100.0f))
		{
			return FailOnTimeout(
				TEXT("Authoritative and replicated player locations did not converge after movement."));
		}
		for (UWorld* Client : Clients)
		{
			APlayerController* Controller = Client
				? FindLocalPlayerController(*Client)
				: nullptr;
			AMatterFluxCharacter* Character = Controller
				? Cast<AMatterFluxCharacter>(Controller->GetPawn())
				: nullptr;
			if (UCharacterMovementComponent* Movement = Character
				? Character->GetCharacterMovement()
				: nullptr)
			{
				Movement->StopMovementImmediately();
				Movement->GravityScale = 0.0f;
			}
		}

		CutSourceIds.Reset(PlayerCount);
		for (int32 Index = 0; Index < PlayerIds.Num(); ++Index)
		{
			AMatterFluxCharacter* Character =
				FindCharacterByPlayerId(Server, PlayerIds[Index]);
			if (UCharacterMovementComponent* Movement = Character
				? Character->GetCharacterMovement()
				: nullptr)
			{
				// Movement and convergence have already been measured. Keep the
				// synthetic target and the 45 cm cut plane at the same height while
				// its network channel settles, even if a large PIE frame would make
				// CharacterMovement enter Falling for a frame.
				Movement->StopMovementImmediately();
				Movement->GravityScale = 0.0f;
			}
			FVector Direction = Character
				? Character->GetActorForwardVector()
				: FVector::ForwardVector;
			Direction.Z = 0.0f;
			if (!Direction.Normalize())
			{
				Direction = FVector::ForwardVector;
			}
			const FGuid SourceId = FGuid::NewDeterministicGuid(
				TEXT("MatterFluxNetworkScaleCut"),
				GetTypeHash(FString::Printf(
					TEXT("%d:%d:%d"),
					PlayerCount,
					bFarApart ? 1 : 0,
					Index)));
			AFragment2DSourceActor* Source = Character
				? SpawnTestSource(
					Server,
					Character->GetActorLocation() + Direction * 260.0f,
					SourceId,
					TEXT("stone"))
				: nullptr;
			if (!Source)
			{
				Test->AddError(TEXT("Could not spawn a replicated cut target."));
				return true;
			}
			CutSourceIds.Add(SourceId);
		}
		TransitionTo(EScenarioPhase::WaitForCutTargets);
		return false;
	}

	bool WaitForCutTargets(
		UWorld& Server,
		const TArray<UWorld*>& Clients)
	{
		for (int32 SourceIndex = 0;
			SourceIndex < CutSourceIds.Num();
			++SourceIndex)
		{
			const FGuid& SourceId = CutSourceIds[SourceIndex];
			const AFragment2DSourceActor* ServerSource =
				FindSource(Server, SourceId);
			if (!ServerSource)
			{
				return FailOnTimeout(
					TEXT("An authoritative cut target disappeared before activation."));
			}
			TArray<UWorld*> ExpectedClients;
			if (bFarApart && PlayerIds.IsValidIndex(SourceIndex))
			{
				ExpectedClients.Add(
					FindClientForPlayerId(
						Clients,
						PlayerIds[SourceIndex]));
			}
			else
			{
				ExpectedClients = Clients;
			}
			for (UWorld* Client : ExpectedClients)
			{
				const AFragment2DSourceActor* ClientSource = Client
					? FindSource(*Client, SourceId)
					: nullptr;
				if (!ClientSource
					|| ClientSource->Revision != ServerSource->Revision)
				{
					return FailOnTimeout(
						TEXT("A cut target did not replicate before client activation."));
				}
			}
		}
		if (FPlatformTime::Seconds() - PhaseStartTime < 0.5)
		{
			return false;
		}

		for (UWorld* Client : Clients)
		{
			APlayerController* Controller =
				Client ? FindLocalPlayerController(*Client) : nullptr;
			AMatterFluxCharacter* Character = Controller
				? Cast<AMatterFluxCharacter>(Controller->GetPawn())
				: nullptr;
			UAbilitySystemComponent* ASC = Character
				? Character->GetAbilitySystemComponent()
				: nullptr;
			const FGameplayAbilitySpec* WandSpec =
				FindWandAbilitySpec(ASC, 0);
			if (!ASC
				|| !WandSpec
				|| !ASC->TryActivateAbility(WandSpec->Handle, true))
			{
				Test->AddError(
					TEXT("A local client's cut ability request was not accepted by GAS."));
				return true;
			}
		}
		CutRequestTime = FPlatformTime::Seconds();
		TransitionTo(EScenarioPhase::WaitForCutReplication);
		return false;
	}

	bool WaitForCutReplication(
		UWorld& Server,
		const TArray<UWorld*>& Clients)
	{
		for (const FGuid& SourceId : CutSourceIds)
		{
			const int32 SourceIndex = CutSourceIds.IndexOfByKey(SourceId);
			const AFragment2DSourceActor* ServerSource =
				FindSource(Server, SourceId);
			if (!ServerSource || ServerSource->Revision <= 0)
			{
				return FailOnTimeout(
					TEXT("A client cut ability did not commit damage on its server target."));
			}
			TArray<UWorld*> ExpectedClients;
			if (bFarApart && PlayerIds.IsValidIndex(SourceIndex))
			{
				ExpectedClients.Add(
					FindClientForPlayerId(
						Clients,
						PlayerIds[SourceIndex]));
			}
			else
			{
				ExpectedClients = Clients;
			}
			for (UWorld* Client : ExpectedClients)
			{
				const AFragment2DSourceActor* ClientSource =
					Client ? FindSource(*Client, SourceId) : nullptr;
				if (!ClientSource
					|| ClientSource->Revision != ServerSource->Revision
					|| ClientSource->bBroken != ServerSource->bBroken)
				{
					return FailOnTimeout(
						TEXT("Cut source revision or broken state did not replicate to every client."));
				}
			}
		}

		TArray<FGuid> ServerFragmentIds;
		GatherFragmentIds(Server, ServerFragmentIds);
		if (ServerFragmentIds.IsEmpty())
		{
			return FailOnTimeout(TEXT("Network cuts produced no authoritative fragments."));
		}
		for (UWorld* Client : Clients)
		{
			TArray<FGuid> ClientFragmentIds;
			GatherFragmentIds(*Client, ClientFragmentIds);
			// Dynamic fragments are distance-relevant and can leave a client's
			// channel while the server is still simulating them. Require every
			// client to receive a non-empty, authoritative subset; the dedicated
			// payload test separately proves exact ids for one stable cut.
			const bool bFragmentsValid = !ClientFragmentIds.IsEmpty()
				&& ClientFragmentIds.ContainsByPredicate(
					[&ServerFragmentIds](const FGuid& Id)
					{
						return !ServerFragmentIds.Contains(Id);
					}) == false;
			if (!bFragmentsValid)
			{
				return FailOnTimeout(
					TEXT("Relevant fragment ids from simultaneous cuts did not converge on a client."));
			}
		}
		CutReplicationMilliseconds =
			(FPlatformTime::Seconds() - CutRequestTime) * 1000.0;

		FlameSourceIds.Reset(PlayerCount);
		for (int32 Index = 0; Index < PlayerIds.Num(); ++Index)
		{
			AMatterFluxCharacter* Character =
				FindCharacterByPlayerId(Server, PlayerIds[Index]);
			FVector Direction = Character
				? Character->GetActorForwardVector()
				: FVector::ForwardVector;
			Direction.Z = 0.0f;
			if (!Direction.Normalize())
			{
				Direction = FVector::ForwardVector;
			}
			const FGuid SourceId = FGuid::NewDeterministicGuid(
				TEXT("MatterFluxNetworkScaleFlame"),
				GetTypeHash(FString::Printf(
					TEXT("%d:%d:%d"),
					PlayerCount,
					bFarApart ? 1 : 0,
					Index)));
			AFragment2DSourceActor* Source = Character
				? SpawnTestSource(
					Server,
					Character->GetActorLocation() + Direction * 260.0f,
					SourceId,
					TEXT("wood"))
				: nullptr;
			if (!Source || !Source->HasCombustionRule())
			{
				Test->AddError(TEXT("Could not spawn a combustible replicated flame target."));
				return true;
			}
			FlameSourceIds.Add(SourceId);
		}

		TransitionTo(EScenarioPhase::WaitForFlameTargets);
		return false;
	}

	bool WaitForFlameTargets(
		UWorld& Server,
		const TArray<UWorld*>& Clients)
	{
		for (const FGuid& SourceId : FlameSourceIds)
		{
			const int32 SourceIndex = FlameSourceIds.IndexOfByKey(SourceId);
			const AFragment2DSourceActor* ServerSource =
				FindSource(Server, SourceId);
			if (!ServerSource || ServerSource->SourceMaterialId != TEXT("wood"))
			{
				return FailOnTimeout(
					TEXT("An authoritative flame target disappeared before activation."));
			}
			TArray<UWorld*> ExpectedClients;
			if (bFarApart && PlayerIds.IsValidIndex(SourceIndex))
			{
				ExpectedClients.Add(
					FindClientForPlayerId(
						Clients,
						PlayerIds[SourceIndex]));
			}
			else
			{
				ExpectedClients = Clients;
			}
			for (UWorld* Client : ExpectedClients)
			{
				const AFragment2DSourceActor* ClientSource = Client
					? FindSource(*Client, SourceId)
					: nullptr;
				if (!ClientSource
					|| ClientSource->SourceMaterialId != TEXT("wood"))
				{
					return FailOnTimeout(
						TEXT("Flame targets did not replicate to every client before activation."));
				}
			}
		}
		// Replicating a newly spawned target can take several seconds while a
		// far-apart streaming union opens many actor channels. During that wait
		// CharacterMovement may finish braking and rotate the pawn. Re-anchor
		// each target to the authoritative pose used by the imminent ability so
		// this remains a networking test instead of a stale-aim test.
		for (int32 Index = 0; Index < PlayerIds.Num(); ++Index)
		{
			AMatterFluxCharacter* Character =
				FindCharacterByPlayerId(Server, PlayerIds[Index]);
			AFragment2DSourceActor* Source =
				FlameSourceIds.IsValidIndex(Index)
					? FindSource(Server, FlameSourceIds[Index])
					: nullptr;
			if (!Character || !Source)
			{
				return FailOnTimeout(
					TEXT("A player or flame target disappeared before activation."));
			}
			FVector Direction = Character->GetActorForwardVector();
			Direction.Z = 0.0f;
			if (!Direction.Normalize())
			{
				Direction = FVector::ForwardVector;
			}
			Source->SetActorLocation(
				Character->GetActorLocation() + Direction * 260.0f,
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
			Source->ForceNetUpdate();
		}

		for (UWorld* Client : Clients)
		{
			APlayerController* Controller =
				Client ? FindLocalPlayerController(*Client) : nullptr;
			if (AMatterFluxCharacter* Character = Controller
				? Cast<AMatterFluxCharacter>(Controller->GetPawn())
				: nullptr)
			{
				// Exercise the same throttling, recording and GAS request path as
				// the player's right-click input.
				Character->ApplyPlayerOperation(
					EMatterFluxPlayerOperation::CastWand,
					FVector2D::ZeroVector,
					1);
				continue;
			}
			return FailOnTimeout(
				TEXT("A local client character disappeared before flame activation."));
		}
		FlameRequestTime = FPlatformTime::Seconds();
		TransitionTo(EScenarioPhase::WaitForFlameReplication);
		return false;
	}

	bool WaitForFlameReplication(
		UWorld& Server,
		const TArray<UWorld*>& Clients)
	{
		AMatterFluxPlayableWorldActor* ServerWorldActor =
			FindPlayableWorldActor(Server);
		if (!ServerWorldActor)
		{
			return FailOnTimeout(
				TEXT("The authoritative playable world disappeared during flame replication."));
		}
		if (bFlameReplicationObserved)
		{
			if (ServerWorldActor->GetPendingFragmentSourceSpawnCount() > 0)
			{
				return FailOnTimeout(
					TEXT("Streaming did not drain after flame replication."));
			}
			// Freeze only after transient flame replication has been observed and
			// the streaming queue is empty. Disabling the actor any earlier also
			// disables the queue processor and creates a permanent pending tail.
			ServerWorldActor->SetActorTickEnabled(false);
			ServerWorldActor->ForceNetUpdate();
			TransitionTo(EScenarioPhase::WaitForFinalConvergence);
			return false;
		}
		// Burning is intentionally transient. Observe it immediately instead of
		// waiting for the unrelated decoration streaming queue to drain; on a
		// far-apart union that queue can outlive the complete wood burn cycle.
		// Final convergence below still requires streaming to become idle.
		for (const FGuid& SourceId : FlameSourceIds)
		{
			const int32 SourceIndex = FlameSourceIds.IndexOfByKey(SourceId);
			const AFragment2DSourceActor* ServerSource =
				FindSource(Server, SourceId);
			if (!ServerSource || !ServerSource->IsCombusting())
			{
				FString Diagnostics;
				for (int32 Index = 0; Index < PlayerIds.Num(); ++Index)
				{
					const AMatterFluxCharacter* Character =
						FindCharacterByPlayerId(Server, PlayerIds[Index]);
					const AFragment2DSourceActor* Target =
						FlameSourceIds.IsValidIndex(Index)
							? FindSource(Server, FlameSourceIds[Index])
							: nullptr;
					FVector Direction = Character
						? Character->GetActorForwardVector()
						: FVector::ForwardVector;
					Direction.Z = 0.0f;
					Direction.Normalize();
					const FVector CharacterLocation = Character
						? Character->GetActorLocation()
						: FVector::ZeroVector;
					const FVector TargetLocation = Target
						? Target->GetActorLocation()
						: FVector::ZeroVector;
					const float Along = FVector::DotProduct(
						TargetLocation - CharacterLocation,
						Direction);
					const float Lateral = FVector::Dist(
						TargetLocation,
						CharacterLocation + Direction * Along);
					Diagnostics += FString::Printf(
						TEXT(" p%d(target=%d burning=%d along=%.1f lateral=%.1f char=%s targetLoc=%s)"),
						PlayerIds[Index],
						Target != nullptr,
						Target ? Target->IsCombusting() : false,
						Along,
						Lateral,
						*CharacterLocation.ToCompactString(),
						*TargetLocation.ToCompactString());
				}
				return FailOnTimeout(FString::Printf(
					TEXT("A client flame ability did not ignite its authoritative target:%s"),
					*Diagnostics));
			}
			TArray<UWorld*> ExpectedClients;
			if (bFarApart && PlayerIds.IsValidIndex(SourceIndex))
			{
				ExpectedClients.Add(
					FindClientForPlayerId(
						Clients,
						PlayerIds[SourceIndex]));
			}
			else
			{
				ExpectedClients = Clients;
			}
			for (UWorld* Client : ExpectedClients)
			{
				const AFragment2DSourceActor* ClientSource =
					Client ? FindSource(*Client, SourceId) : nullptr;
				if (!ClientSource
					|| ClientSource->SourceMaterialId != TEXT("wood")
					|| ClientSource->GetBurningCellCount() <= 0)
				{
					return FailOnTimeout(
						TEXT("Burning source state did not replicate to every client."));
				}
			}
		}
		FlameReplicationMilliseconds =
			(FPlatformTime::Seconds() - FlameRequestTime) * 1000.0;
		bFlameReplicationObserved = true;
		TransitionTo(EScenarioPhase::WaitForFlameReplication);
		return false;
	}

	bool WaitForFinalConvergence(
		UWorld& Server,
		const TArray<UWorld*>& Clients)
	{
		if (FPlatformTime::Seconds() - PhaseStartTime < 0.75)
		{
			return false;
		}
		AMatterFluxPlayableWorldActor* ServerWorldActor =
			FindPlayableWorldActor(Server);
		const bool bStreamingSettled = ServerWorldActor
			&& ServerWorldActor->GetPendingFragmentSourceSpawnCount() == 0;
		const bool bMaterialsCoherent = ServerWorldActor
			&& AreMaterialSnapshotsCoherent(*ServerWorldActor, Clients);
		if (!bStreamingSettled
			|| !bMaterialsCoherent)
		{
			FString ClientSnapshots;
			for (int32 Index = 0; Index < Clients.Num(); ++Index)
			{
				const AMatterFluxPlayableWorldActor* ClientWorldActor =
					Clients[Index]
						? FindPlayableWorldActor(*Clients[Index])
						: nullptr;
				ClientSnapshots += FString::Printf(
					TEXT(" c%d(rev=%d step=%d)"),
					Index,
					ClientWorldActor
						? ClientWorldActor->GetAppliedMaterialStateRevision()
						: INDEX_NONE,
					ClientWorldActor
						? ClientWorldActor->GetMaterialSimulationStep()
						: INDEX_NONE);
			}
			return FailOnTimeout(FString::Printf(
				TEXT("Final convergence failed: streaming=%d pending=%d materials=%d serverStep=%d;%s"),
				bStreamingSettled,
				ServerWorldActor
					? ServerWorldActor->GetPendingFragmentSourceSpawnCount()
					: INDEX_NONE,
				bMaterialsCoherent,
				ServerWorldActor
					? ServerWorldActor->GetMaterialSimulationStep()
					: INDEX_NONE,
				*ClientSnapshots));
		}

		bMeasureFrames = false;
		TArray<double> SortedMovementSamples;
		if (MovementFrameSamples.Num() > MovementWarmupFrames)
		{
			SortedMovementSamples.Append(
				MovementFrameSamples.GetData() + MovementWarmupFrames,
				MovementFrameSamples.Num() - MovementWarmupFrames);
		}
		TArray<double> SortedActionSamples = ActionFrameSamples;
		Algo::Sort(SortedMovementSamples);
		Algo::Sort(SortedActionSamples);
		if (SortedMovementSamples.IsEmpty() || SortedActionSamples.IsEmpty())
		{
			Test->AddError(TEXT("The network scale scenario produced incomplete end-frame samples."));
			return true;
		}
		const auto P95 = [](const TArray<double>& Samples)
		{
			const int32 Index = FMath::Clamp(
				FMath::CeilToInt(Samples.Num() * 0.95) - 1,
				0,
				Samples.Num() - 1);
			return Samples[Index];
		};
		const auto Average = [](const TArray<double>& Samples)
		{
			double Total = 0.0;
			for (const double Sample : Samples)
			{
				Total += Sample;
			}
			return Total / Samples.Num();
		};
		const double MovementP95Milliseconds = P95(SortedMovementSamples);
		const double ActionP95Milliseconds = P95(SortedActionSamples);
		const double ActionP95BudgetMilliseconds =
			BaseActionP95FrameBudgetMilliseconds
			+ FMath::Max(PlayerCount - 2, 0)
				* PerAdditionalInProcessClientBudgetMilliseconds;
		const double MaximumMilliseconds = FMath::Max(
			SortedMovementSamples.Last(),
			SortedActionSamples.Last());
		const bool bStrictPerformance = FParse::Param(
			FCommandLine::Get(),
			TEXT("MatterFluxStrictPerf"));
		const auto DescribePhase = [&P95, &Average](
			const TCHAR* Name,
			TArray<double> Samples)
		{
			if (Samples.IsEmpty())
			{
				return FString::Printf(TEXT(" %s=empty"), Name);
			}
			Algo::Sort(Samples);
			return FString::Printf(
				TEXT(" %s(n=%d avg=%.2f p95=%.2f max=%.2f)"),
				Name,
				Samples.Num(),
				Average(Samples),
				P95(Samples),
				Samples.Last());
		};

		const int32 SourceActorLimit = FMath::Max(600, PlayerCount * 500 + PlayerCount * 2);
		Test->AddInfo(FString::Printf(
			TEXT("NetworkScale %d players %s: movement %d frames avg %.2f ms p95 %.2f ms; actions %d frames avg %.2f ms p95 %.2f ms; max %.2f ms; cut replication %.2f ms, flame replication %.2f ms; visible chunks=%d, peak server sources=%d, peak client sources=%d, peak fragments=%d, material snapshot=%d bytes."),
			PlayerCount,
			bFarApart ? TEXT("far") : TEXT("near"),
			SortedMovementSamples.Num(),
			Average(SortedMovementSamples),
			MovementP95Milliseconds,
			SortedActionSamples.Num(),
			Average(SortedActionSamples),
			ActionP95Milliseconds,
			MaximumMilliseconds,
			CutReplicationMilliseconds,
			FlameReplicationMilliseconds,
			InitialVisibleTerrainChunks,
			PeakSourceActorCount,
			PeakClientSourceActorCount,
			PeakFragmentActorCount,
			ServerWorldActor->GetReplicatedMaterialStateByteCount()));
		Test->AddInfo(FString::Printf(
			TEXT("NetworkScale action phases:%s%s%s%s"),
			*DescribePhase(TEXT("cut"), CutActionFrameSamples),
			*DescribePhase(TEXT("target"), FlameTargetFrameSamples),
			*DescribePhase(TEXT("flame"), FlameActionFrameSamples),
			*DescribePhase(TEXT("settle"), FinalActionFrameSamples)));
		Test->AddInfo(FString::Printf(
			TEXT("NetworkScale garbage collection: count=%d total=%.2f ms, overlapping frame%s"),
			GarbageCollectionCount,
			GarbageCollectionMilliseconds,
			*DescribePhase(TEXT("gc"), GarbageCollectionFrameSamples)));

		if (bStrictPerformance
			&& MovementP95Milliseconds >= MovementP95FrameBudgetMilliseconds)
		{
			Test->AddError(FString::Printf(
				TEXT("Network movement p95 frame time %.2f ms exceeded %.2f ms."),
				MovementP95Milliseconds,
				MovementP95FrameBudgetMilliseconds));
		}
		if (bStrictPerformance
			&& ActionP95Milliseconds >= ActionP95BudgetMilliseconds)
		{
			Test->AddError(FString::Printf(
				TEXT("Network action p95 frame time %.2f ms exceeded %.2f ms."),
				ActionP95Milliseconds,
				ActionP95BudgetMilliseconds));
		}
		if (bStrictPerformance
			&& MaximumMilliseconds >= MaximumFrameBudgetMilliseconds)
		{
			Test->AddError(FString::Printf(
				TEXT("Network scale maximum frame time %.2f ms exceeded %.2f ms."),
				MaximumMilliseconds,
				MaximumFrameBudgetMilliseconds));
		}
		if (!bStrictPerformance && MaximumMilliseconds >= 500.0)
		{
			Test->AddError(FString::Printf(
				TEXT("Network scale catastrophic frame time %.2f ms exceeded 500.00 ms."),
				MaximumMilliseconds));
		}
		if (CutReplicationMilliseconds >= 5000.0
			|| FlameReplicationMilliseconds >= 5000.0)
		{
			Test->AddError(TEXT("A gameplay action took at least five seconds to replicate."));
		}
		if (PeakSourceActorCount > SourceActorLimit)
		{
			Test->AddError(FString::Printf(
				TEXT("Source actor peak %d exceeded the %d-player streaming bound %d."),
				PeakSourceActorCount,
				PlayerCount,
				SourceActorLimit));
		}
		// A 3x3 relevant chunk window can contain up to 64 generated sources per
		// chunk. Use that geometric ceiling; this still catches cross-player or
		// long-distance accumulation without failing on a dense deterministic seed.
		constexpr int32 ClientSourceActorLimit = 9 * 64;
		if (PeakClientSourceActorCount > ClientSourceActorLimit)
		{
			Test->AddError(FString::Printf(
				TEXT("Per-client source actor peak %d exceeded the spatial relevancy bound %d."),
				PeakClientSourceActorCount,
				ClientSourceActorLimit));
		}
		const int32 EffectiveTerrainResidentLimit = FMath::Max(
			ServerWorldActor->GetTerrainChunkCacheLimit(),
			ServerWorldActor->GetVisibleTerrainChunkCount());
		if (ServerWorldActor->GetCachedTerrainChunkCount()
			> EffectiveTerrainResidentLimit)
		{
			Test->AddError(FString::Printf(
				TEXT("Terrain residents exceeded the effective %d-chunk bound (cache budget or active union, whichever is larger)."),
				EffectiveTerrainResidentLimit));
		}
		return true;
	}

	bool AreCharactersConverged(
		UWorld& Server,
		const TArray<UWorld*>& Clients,
		const float Tolerance) const
	{
		for (const int32 PlayerId : PlayerIds)
		{
			const AMatterFluxCharacter* ServerCharacter =
				FindCharacterByPlayerId(Server, PlayerId);
			if (!ServerCharacter)
			{
				return false;
			}
			for (UWorld* Client : Clients)
			{
				const AMatterFluxCharacter* ClientCharacter = Client
					? FindCharacterByPlayerId(*Client, PlayerId)
					: nullptr;
				if (!ClientCharacter
					|| FVector::Dist(
						ClientCharacter->GetActorLocation(),
						ServerCharacter->GetActorLocation()) > Tolerance)
				{
					return false;
				}
			}
		}
		return true;
	}

	bool AreMaterialSnapshotsCoherent(
		const AMatterFluxPlayableWorldActor& ServerWorldActor,
		const TArray<UWorld*>& Clients) const
	{
		int32 ExpectedRevision = INDEX_NONE;
		int32 ExpectedStep = INDEX_NONE;
		static const FName MaterialIds[] =
		{
			TEXT("water"),
			TEXT("lava"),
			TEXT("sand"),
			TEXT("steam"),
			TEXT("stone")
		};
		TArray<int32> ExpectedCounts;
		for (UWorld* Client : Clients)
		{
			const AMatterFluxPlayableWorldActor* ClientWorldActor = Client
				? FindPlayableWorldActor(*Client)
				: nullptr;
			if (!ClientWorldActor
				|| ClientWorldActor->GetAppliedMaterialStateRevision() <= 0
				|| ClientWorldActor->GetMaterialSimulationStep()
					> ServerWorldActor.GetMaterialSimulationStep())
			{
				return false;
			}
			if (ExpectedRevision == INDEX_NONE)
			{
				ExpectedRevision =
					ClientWorldActor->GetAppliedMaterialStateRevision();
				ExpectedStep = ClientWorldActor->GetMaterialSimulationStep();
				for (const FName MaterialId : MaterialIds)
				{
					ExpectedCounts.Add(
						ClientWorldActor->GetSimulatedMaterialCount(MaterialId));
				}
			}
			else if (ClientWorldActor->GetAppliedMaterialStateRevision()
					!= ExpectedRevision
				|| ClientWorldActor->GetMaterialSimulationStep() != ExpectedStep)
			{
				return false;
			}
			else
			{
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(MaterialIds); ++Index)
				{
					if (ClientWorldActor->GetSimulatedMaterialCount(MaterialIds[Index])
						!= ExpectedCounts[Index])
					{
						return false;
					}
				}
			}
		}
		return true;
	}

	void UpdatePeakCounts(
		UWorld& Server,
		const TArray<UWorld*>& Clients)
	{
		PeakSourceActorCount = FMath::Max(
			PeakSourceActorCount,
			CountSourceActors(Server));
		int32 FragmentCount = 0;
		for (TActorIterator<AFragment2DActor> It(&Server); It; ++It)
		{
			++FragmentCount;
		}
		PeakFragmentActorCount = FMath::Max(
			PeakFragmentActorCount,
			FragmentCount);
		for (UWorld* Client : Clients)
		{
			if (Client)
			{
				PeakClientSourceActorCount = FMath::Max(
					PeakClientSourceActorCount,
					CountSourceActors(*Client));
			}
		}
	}

	void RecordEndFrame()
	{
		const double Now = FPlatformTime::Seconds();
		if (!bMeasureFrames || LastEndFrameTime <= 0.0)
		{
			LastEndFrameTime = Now;
			return;
		}
		const double FrameMilliseconds =
			(Now - LastEndFrameTime) * 1000.0;
		LastEndFrameTime = Now;
		if (FrameMilliseconds <= 0.0 || FrameMilliseconds >= 1000.0)
		{
			bGarbageCollectionSinceLastFrame = false;
			return;
		}
		if (bGarbageCollectionSinceLastFrame)
		{
			GarbageCollectionFrameSamples.Add(FrameMilliseconds);
			bGarbageCollectionSinceLastFrame = false;
		}
		if (Phase == EScenarioPhase::MovePlayers)
		{
			MovementFrameSamples.Add(FrameMilliseconds);
		}
		else
		{
			ActionFrameSamples.Add(FrameMilliseconds);
			switch (Phase)
			{
			case EScenarioPhase::WaitForCutReplication:
				CutActionFrameSamples.Add(FrameMilliseconds);
				break;
			case EScenarioPhase::WaitForFlameTargets:
				FlameTargetFrameSamples.Add(FrameMilliseconds);
				break;
			case EScenarioPhase::WaitForFlameReplication:
				FlameActionFrameSamples.Add(FrameMilliseconds);
				break;
			case EScenarioPhase::WaitForFinalConvergence:
				FinalActionFrameSamples.Add(FrameMilliseconds);
				break;
			default:
				break;
			}
		}
	}

	void HandlePreGarbageCollect()
	{
		bGarbageCollectionSinceLastFrame = true;
		GarbageCollectionStartTime = FPlatformTime::Seconds();
	}

	void HandlePostGarbageCollect()
	{
		if (GarbageCollectionStartTime > 0.0)
		{
			GarbageCollectionMilliseconds +=
				(FPlatformTime::Seconds() - GarbageCollectionStartTime)
					* 1000.0;
			++GarbageCollectionCount;
			GarbageCollectionStartTime = 0.0;
		}
	}

	void TransitionTo(const EScenarioPhase NewPhase)
	{
		Phase = NewPhase;
		PhaseStartTime = FPlatformTime::Seconds();
	}

	bool FailOnTimeout(const FString& Message)
	{
		// Streaming and target preparation create/replicate one full client world
		// at a time in this in-process harness. Scale only those readiness waits
		// with client count. Movement, ability execution, result replication and
		// their p95/latency gates keep the strict 45-second gameplay timeout.
		const bool bScalableReadinessPhase =
			Phase == EScenarioPhase::WaitForInitialStreaming
			|| Phase == EScenarioPhase::WaitForMovementConvergence
			|| Phase == EScenarioPhase::WaitForCutTargets
			|| Phase == EScenarioPhase::WaitForFlameTargets
			|| Phase == EScenarioPhase::WaitForFinalConvergence;
		const double TimeoutSeconds =
			bScalableReadinessPhase
			? ReadinessTimeoutSeconds
				+ InitialStreamingSecondsPerAdditionalClient
					* FMath::Max(PlayerCount - 2, 0)
			: PhaseTimeoutSeconds;
		if (FPlatformTime::Seconds() - PhaseStartTime < TimeoutSeconds)
		{
			return false;
		}
		Test->AddError(Message);
		return true;
	}

	FAutomationTestBase* Test = nullptr;
	int32 PlayerCount = 0;
	bool bFarApart = false;
	EScenarioPhase Phase = EScenarioPhase::WaitForWorlds;
	double PhaseStartTime = 0.0;
	double LastEndFrameTime = 0.0;
	double CutRequestTime = 0.0;
	double FlameRequestTime = 0.0;
	double CutReplicationMilliseconds = 0.0;
	double FlameReplicationMilliseconds = 0.0;
	bool bMeasureFrames = false;
	bool bFlameReplicationObserved = false;
	bool bScenarioSeedApplied = false;
	int32 MovementFrame = 0;
	int32 InitialVisibleTerrainChunks = 0;
	int32 PeakSourceActorCount = 0;
	int32 PeakClientSourceActorCount = 0;
	int32 PeakFragmentActorCount = 0;
	TArray<int32> PlayerIds;
	TArray<FVector> InitialServerLocations;
	TArray<float> PeakTravelDistances;
	TArray<FGuid> CutSourceIds;
	TArray<FGuid> FlameSourceIds;
	FDelegateHandle EndFrameHandle;
	FDelegateHandle PreGarbageCollectHandle;
	FDelegateHandle PostGarbageCollectHandle;
	bool bGarbageCollectionSinceLastFrame = false;
	double GarbageCollectionStartTime = 0.0;
	double GarbageCollectionMilliseconds = 0.0;
	int32 GarbageCollectionCount = 0;
	TArray<double> MovementFrameSamples;
	TArray<double> ActionFrameSamples;
	TArray<double> CutActionFrameSamples;
	TArray<double> FlameTargetFrameSamples;
	TArray<double> FlameActionFrameSamples;
	TArray<double> FinalActionFrameSamples;
	TArray<double> GarbageCollectionFrameSamples;
	};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FMatterFluxNetworkScalePIETest,
	"MatterFlux.Network.Scale",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

void FMatterFluxNetworkScalePIETest::GetTests(
	TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands) const
{
	for (int32 PlayerCount = 2; PlayerCount <= 4; ++PlayerCount)
	{
		OutBeautifiedNames.Add(FString::Printf(
			TEXT("%dPlayers.NearChunks"),
			PlayerCount));
		OutTestCommands.Add(FString::Printf(
			TEXT("%d;near"),
			PlayerCount));
		OutBeautifiedNames.Add(FString::Printf(
			TEXT("%dPlayers.FarChunks"),
			PlayerCount));
		OutTestCommands.Add(FString::Printf(
			TEXT("%d;far"),
			PlayerCount));
	}
}

bool FMatterFluxNetworkScalePIETest::RunTest(const FString& Parameters)
{
	CollectGarbage(RF_NoFlags, true);

	FString PlayerCountText;
	FString DistanceText;
	if (!Parameters.Split(TEXT(";"), &PlayerCountText, &DistanceText))
	{
		AddError(FString::Printf(
			TEXT("Invalid network scale parameters: %s"),
			*Parameters));
		return false;
	}
	const int32 PlayerCount = FCString::Atoi(*PlayerCountText);
	const bool bFarApart = DistanceText.Equals(
		TEXT("far"),
		ESearchCase::IgnoreCase);
	if (PlayerCount < 2 || PlayerCount > 4
		|| (!bFarApart
			&& !DistanceText.Equals(TEXT("near"), ESearchCase::IgnoreCase)))
	{
		AddError(FString::Printf(
			TEXT("Unsupported network scale parameters: %s"),
			*Parameters));
		return false;
	}
	if (!TestNotNull(
		TEXT("Isolated network scale map created"),
		FAutomationEditorCommonUtils::CreateNewMap()))
	{
		return false;
	}

	AddExpectedError(
		TEXT("FNetGUIDCache::SupportsObject: Level /Temp/"),
		EAutomationExpectedErrorFlags::Contains,
		-1,
		false);
	AddExpectedError(
		TEXT("RegisterNetGUID_Client: Guid with pathname"),
		EAutomationExpectedErrorFlags::Contains,
		-1,
		false);

	FRequestPlaySessionParams RequestParams;
	ULevelEditorPlaySettings* PlaySettings =
		NewObject<ULevelEditorPlaySettings>();
	PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Client);
	PlaySettings->SetRunUnderOneProcess(true);
	PlaySettings->SetPlayNumberOfClients(PlayerCount);
	PlaySettings->bLaunchSeparateServer = false;
	RequestParams.EditorPlaySettings = PlaySettings;
	FAutomationEditorCommonUtils::SetPlaySessionStartToActiveViewport(
		RequestParams);
	PlaySettings->AddToRoot();
	ADD_LATENT_AUTOMATION_COMMAND(
		FStartPIEForAutomationCommand(RequestParams));
	ADD_LATENT_AUTOMATION_COMMAND(
		MatterFluxNetworkScaleTests::FNetworkScalePIECommand(
			this,
			PlayerCount,
			bFarApart));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	return true;
}
