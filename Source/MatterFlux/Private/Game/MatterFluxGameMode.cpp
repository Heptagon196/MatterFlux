#include "Game/MatterFluxGameMode.h"

#include "Game/MatterFluxCharacter.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Game/MatterFluxGameState.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "TimerManager.h"
#include "EngineUtils.h"
#include "Misc/Crc.h"

AMatterFluxGameMode::AMatterFluxGameMode()
{
	DefaultPawnClass = AMatterFluxCharacter::StaticClass();
	PlayerControllerClass = AMatterFluxPlayerController::StaticClass();
	PlayerStateClass = AMatterFluxPlayerState::StaticClass();
	GameStateClass = AMatterFluxGameState::StaticClass();
	PlayableWorldClass = AMatterFluxPlayableWorldActor::StaticClass();
}

APawn* AMatterFluxGameMode::SpawnDefaultPawnAtTransform_Implementation(
	AController* NewPlayer,
	const FTransform& SpawnTransform)
{
	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	UWorld* World = GetWorld();
	if (!PawnClass || !World)
	{
		return nullptr;
	}
	const APawn* PawnToFit = PawnClass->GetDefaultObject<APawn>();
	AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		PlayableWorld = *It;
		break;
	}
	const ACharacter* CharacterToFit = Cast<ACharacter>(PawnToFit);
	const UCapsuleComponent* PawnCapsule = CharacterToFit
		? CharacterToFit->GetCapsuleComponent()
		: nullptr;
	const float PawnRadius = PawnCapsule
		? PawnCapsule->GetScaledCapsuleRadius()
		: 0.0f;
	const float PawnHalfHeight = PawnCapsule
		? PawnCapsule->GetScaledCapsuleHalfHeight()
		: 0.0f;

	// The playable map intentionally needs only one authored PlayerStart. A
	// listen host and joining clients therefore share its base transform. UE's
	// default path may select that occupied start and then fail the pawn spawn.
	// Try deterministic nearby cells so every peer gets a valid pawn without
	// requiring a fixed number of PlayerStart actors in a procedural world.
	static const FIntPoint SpawnOffsets[] =
	{
		FIntPoint(0, 0),
		FIntPoint(1, 0),
		FIntPoint(-1, 0),
		FIntPoint(0, 1),
		FIntPoint(0, -1),
		FIntPoint(1, 1),
		FIntPoint(1, -1),
		FIntPoint(-1, 1),
		FIntPoint(-1, -1),
		FIntPoint(2, 0),
		FIntPoint(-2, 0),
		FIntPoint(0, 2),
		FIntPoint(0, -2)
	};

	for (const FIntPoint& Offset : SpawnOffsets)
	{
		FTransform Candidate = SpawnTransform;
		Candidate.AddToTranslation(FVector(
			static_cast<double>(Offset.X) * MultiplayerSpawnSpacing,
			static_cast<double>(Offset.Y) * MultiplayerSpawnSpacing,
			0.0));
		if (PlayableWorld && PawnCapsule)
		{
			FVector GroundedLocation;
			if (!PlayableWorld->TryResolveTerrainSpawnLocation(
				Candidate.GetLocation(),
				PawnRadius,
				PawnHalfHeight,
				4.0f,
				GroundedLocation))
			{
				continue;
			}
			Candidate.SetLocation(GroundedLocation);
		}
		if (PawnToFit
			&& World->EncroachingBlockingGeometry(
				PawnToFit,
				Candidate.GetLocation(),
				Candidate.Rotator()))
		{
			continue;
		}

		FActorSpawnParameters SpawnInfo;
		SpawnInfo.Instigator = GetInstigator();
		SpawnInfo.ObjectFlags |= RF_Transient;
		SpawnInfo.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::
				AdjustIfPossibleButDontSpawnIfColliding;
		if (APawn* Pawn = World->SpawnActor<APawn>(
			PawnClass,
			Candidate,
			SpawnInfo))
		{
			return Pawn;
		}
	}

	UE_LOG(
		LogMatterFlux,
		Error,
		TEXT("Could not find a collision-free multiplayer spawn near %s for %s"),
		*SpawnTransform.GetLocation().ToCompactString(),
		*GetNameSafe(NewPlayer));
	return nullptr;
}

void AMatterFluxGameMode::StartPlay()
{
	if (HasAuthority())
	{
		IMatterFluxScriptRuntime& ScriptRuntime =
			IMatterFluxScriptRuntime::Get();
		FMatterFluxContentRegistryPtr Registry =
			ScriptRuntime.GetActiveRegistry();
		if (!Registry.IsValid())
		{
			FString Error;
			if (!ScriptRuntime.ReloadDefaultContentPack(Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Authoritative content pack failed to load: %s"),
					*Error);
			}
			Registry = ScriptRuntime.GetActiveRegistry();
		}

		if (Registry.IsValid())
		{
			if (AMatterFluxGameState* MatterFluxGameState =
				GetGameState<AMatterFluxGameState>())
			{
				MatterFluxGameState->SetAuthoritativeContentIdentity(
					Registry->Manifest.PackId,
					Registry->Manifest.Revision,
					Registry->Manifest.VersionHash);
			}
		}

		bool bHasPlayableWorld = false;
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(GetWorld()); It; ++It)
		{
			bHasPlayableWorld = true;
			break;
		}

		if (!bHasPlayableWorld && PlayableWorldClass)
		{
			UWorld* World = GetWorld();
			AMatterFluxPlayableWorldActor* PlayableWorld =
				World->SpawnActorDeferred<AMatterFluxPlayableWorldActor>(
				PlayableWorldClass,
				FTransform::Identity,
				nullptr,
				nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (PlayableWorld)
			{
				const bool bShowTitleScreen =
					GetNetMode() != NM_DedicatedServer
					&& !FApp::IsUnattended()
					&& !FParse::Param(
						FCommandLine::Get(),
						TEXT("MatterFluxSkipStartMenu"))
					&& !World->URL.HasOption(TEXT("MatterFluxStarted"));
				const int32 InitialSeed =
					ResolveInitialPlayableWorldSeed(bShowTitleScreen);
				if (InitialSeed > 0)
				{
					PlayableWorld->SetInitialMapSeed(InitialSeed);
				}
				PlayableWorld->FinishSpawning(FTransform::Identity);
			}
		}
	}

	Super::StartPlay();
	if (HasAuthority() && GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(
			PlayerEntryTimer,
			this,
			&AMatterFluxGameMode::AdvancePendingPlayerEntries,
			0.05f,
			true,
			0.0f);
		GetWorld()->GetTimerManager().SetTimer(
			CreatureSpawnTimer,
			this,
			&AMatterFluxGameMode::RefreshConfiguredCreatureSpawns,
			0.5f,
			true,
			0.2f);
	}
}

void AMatterFluxGameMode::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(PlayerEntryTimer);
		GetWorld()->GetTimerManager().ClearTimer(CreatureSpawnTimer);
	}
	if (AMatterFluxPlayableWorldActor* PlayableWorld = FindPlayableWorld())
	{
		for (const FPendingPlayerEntry& Entry : PendingPlayerEntries)
		{
			if (Entry.bSpawnRegionRequested)
			{
				PlayableWorld->ReleasePlayerSpawnRegion(Entry.TerrainChunk);
			}
		}
		ReleaseConfiguredCreatureTerrainPins(*PlayableWorld);
	}
	PendingPlayerEntries.Reset();
	Super::EndPlay(EndPlayReason);
}

void AMatterFluxGameMode::HandleStartingNewPlayer_Implementation(
	APlayerController* NewPlayer)
{
	if (!HasAuthority() || !IsValid(NewPlayer) || NewPlayer->GetPawn())
	{
		return;
	}
	for (const FPendingPlayerEntry& Entry : PendingPlayerEntries)
	{
		if (Entry.Controller == NewPlayer)
		{
			return;
		}
	}
	FPendingPlayerEntry& Entry = PendingPlayerEntries.AddDefaulted_GetRef();
	Entry.Controller = NewPlayer;
}

void AMatterFluxGameMode::PrepareForInitialWorldEntry()
{
	if (!HasAuthority())
	{
		return;
	}
	bCreatureSpawnGateOpen = false;
	bInitialCreaturePassComplete = false;
}

bool AMatterFluxGameMode::CompleteExistingPlayerWorldLoad()
{
	if (!HasAuthority())
	{
		return false;
	}
	AMatterFluxPlayableWorldActor* PlayableWorld = FindPlayableWorld();
	if (!PlayableWorld || !PlayableWorld->IsInitialWorldEntryReady())
	{
		return false;
	}
	bCreatureSpawnGateOpen = true;
	RefreshConfiguredCreatureSpawns();
	bInitialCreaturePassComplete = true;
	return true;
}

AMatterFluxPlayableWorldActor* AMatterFluxGameMode::FindPlayableWorld() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AMatterFluxGameMode::ReleaseConfiguredCreatureTerrainPins(
	AMatterFluxPlayableWorldActor& PlayableWorld)
{
	for (const TPair<FName, FIntPoint>& Pair
		: ConfiguredCreatureTerrainPins)
	{
		PlayableWorld.ReleasePlayerSpawnRegion(Pair.Value);
	}
	ConfiguredCreatureTerrainPins.Reset();
}

void AMatterFluxGameMode::AdvancePendingPlayerEntries()
{
	if (!HasAuthority() || PendingPlayerEntries.IsEmpty())
	{
		return;
	}
	AMatterFluxPlayableWorldActor* PlayableWorld = FindPlayableWorld();
	if (!PlayableWorld)
	{
		return;
	}

	for (int32 Index = PendingPlayerEntries.Num() - 1; Index >= 0; --Index)
	{
		FPendingPlayerEntry& Entry = PendingPlayerEntries[Index];
		APlayerController* Controller = Entry.Controller.Get();
		if (!IsValid(Controller) || Controller->GetPawn())
		{
			if (Entry.bSpawnRegionRequested)
			{
				PlayableWorld->ReleasePlayerSpawnRegion(Entry.TerrainChunk);
			}
			PendingPlayerEntries.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			continue;
		}
		if (!Entry.bSpawnRegionRequested)
		{
			FVector StartLocation = FVector::ZeroVector;
			FVector CustomMapPlayerStart;
			if (PlayableWorld->IsCustomMapActive()
				&& PlayableWorld->TryGetCustomMapMarker(
					TEXT("player_start"), CustomMapPlayerStart))
			{
				Entry.bUseSpawnTransform = true;
				Entry.SpawnTransform = FTransform(
					FRotator::ZeroRotator,
					CustomMapPlayerStart);
				StartLocation = CustomMapPlayerStart;
			}
			else
			{
				AActor* StartSpot = FindPlayerStart(Controller);
				Entry.StartSpot = StartSpot;
				Controller->StartSpot = StartSpot;
				StartLocation = StartSpot
					? StartSpot->GetActorLocation()
					: FVector::ZeroVector;
			}
			Entry.bSpawnRegionRequested =
				PlayableWorld->RequestPlayerSpawnRegion(
					StartLocation,
					Entry.TerrainChunk);
		}
	}

	if (PendingPlayerEntries.IsEmpty())
	{
		return;
	}
	for (const FPendingPlayerEntry& Entry : PendingPlayerEntries)
	{
		if (!Entry.bSpawnRegionRequested
			|| !PlayableWorld->IsPlayerSpawnRegionTerrainReady(
				Entry.TerrainChunk))
		{
			return;
		}
	}

	if (!bInitialCreaturePassComplete)
	{
		if (!PlayableWorld->IsInitialWorldEntryReady())
		{
			return;
		}
		// This is the only point that opens configured creature spawning. Return
		// after the synchronous first pass so player Restart/Possess necessarily
		// happens on a later timer tick.
		bCreatureSpawnGateOpen = true;
		RefreshConfiguredCreatureSpawns();
		bInitialCreaturePassComplete = true;
		return;
	}

	for (int32 Index = PendingPlayerEntries.Num() - 1; Index >= 0; --Index)
	{
		FPendingPlayerEntry& Entry = PendingPlayerEntries[Index];
		APlayerController* Controller = Entry.Controller.Get();
		if (!IsValid(Controller))
		{
			PlayableWorld->ReleasePlayerSpawnRegion(Entry.TerrainChunk);
			PendingPlayerEntries.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			continue;
		}
		if (Entry.bUseSpawnTransform)
		{
			RestartPlayerAtTransform(Controller, Entry.SpawnTransform);
		}
		else
		{
			if (AActor* StartSpot = Entry.StartSpot.Get())
			{
				Controller->StartSpot = StartSpot;
			}
			Super::HandleStartingNewPlayer_Implementation(Controller);
		}
		if (Controller->GetPawn())
		{
			PlayableWorld->ReleasePlayerSpawnRegion(Entry.TerrainChunk);
			PendingPlayerEntries.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		}
	}
}

bool AMatterFluxGameMode::ShouldActivateQuestCreatureSpawns(
	const FName QuestId) const
{
	const AGameStateBase* CurrentGameState = GetGameState<AGameStateBase>();
	if (!CurrentGameState) return false;
	for (APlayerState* PlayerState : CurrentGameState->PlayerArray)
	{
		const AMatterFluxPlayerState* MatterFluxState =
			Cast<AMatterFluxPlayerState>(PlayerState);
		const UMatterFluxProgressionComponent* Progression =
			MatterFluxState ? MatterFluxState->GetProgression() : nullptr;
		const FMatterFluxQuestState* Quest = Progression
			? Progression->FindQuestState(QuestId) : nullptr;
		if (Quest && Quest->Status == EMatterFluxQuestRuntimeStatus::Active)
		{
			// Equipping completes while the workbench is still visible. Keep the
			// enclosed-arena enemies deferred until the player closes it.
			if (QuestId == TEXT("std.init_quest.kill_enemy"))
			{
				for (FConstPlayerControllerIterator It =
					GetWorld()->GetPlayerControllerIterator(); It; ++It)
				{
					const AMatterFluxPlayerController* Controller =
						Cast<AMatterFluxPlayerController>(It->Get());
					if (Controller
						&& Controller->GetPlayerState<AMatterFluxPlayerState>()
							== MatterFluxState
						&& Controller->IsMagicWorkbenchOpen())
					{
						return false;
					}
				}
			}
			return true;
		}
	}
	return false;
}

void AMatterFluxGameMode::RefreshConfiguredCreatureSpawns()
{
	if (!HasAuthority() || !GetWorld()) return;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid()) return;
	AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(GetWorld()); It; ++It)
	{
		PlayableWorld = *It;
		break;
	}
	if (!PlayableWorld) return;
	if (!bCreatureSpawnGateOpen)
	{
		return;
	}
	const FName CurrentCustomMapId = PlayableWorld->GetActiveCustomMapId();
	const int32 CurrentCustomMapLoadSerial =
		PlayableWorld->GetCustomMapLoadSerial();
	if (!bCreatureSpawnModeInitialized
		|| CurrentCustomMapId != SpawnedForCustomMapId
		|| CurrentCustomMapLoadSerial != SpawnedForCustomMapLoadSerial)
	{
		ReleaseConfiguredCreatureTerrainPins(*PlayableWorld);
		for (TActorIterator<AMatterFluxCreatureActor> It(GetWorld()); It; ++It)
		{
			if (It->Tags.Contains(TEXT("MatterFluxConfiguredCreature")))
			{
				It->Destroy();
			}
		}
		SpawnedCreatureWaves.Reset();
		SpawnedForCustomMapId = CurrentCustomMapId;
		SpawnedForCustomMapLoadSerial = CurrentCustomMapLoadSerial;
		bCreatureSpawnModeInitialized = true;
	}
	// Story creatures belong to an authored map. Free mode intentionally has
	// neither the PaperMagic task chain nor its merchant/enemy population.
	if (CurrentCustomMapId.IsNone()) return;

	const FMatterFluxCustomMapDefinition* MapDefinition =
		Registry->CustomMaps.Find(CurrentCustomMapId);
	if (!MapDefinition)
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Active custom map '%s' has no content definition"),
			*CurrentCustomMapId.ToString());
		return;
	}

	struct FConfiguredCreatureWave
	{
		FName Key;
		FName QuestId;
		TArray<FMatterFluxCreatureSpawnDefinition> Spawns;
	};
	TArray<FConfiguredCreatureWave> Waves;
	if (!MapDefinition->InitialCreatureSpawns.IsEmpty())
	{
		FConfiguredCreatureWave& Wave = Waves.AddDefaulted_GetRef();
		Wave.Key = FName(*FString::Printf(
			TEXT("map.%s.initial"), *CurrentCustomMapId.ToString()));
		Wave.Spawns = MapDefinition->InitialCreatureSpawns;
	}
	TArray<FName> QuestIds;
	Registry->Quests.GetKeys(QuestIds);
	QuestIds.Sort(FNameLexicalLess());
	for (const FName QuestId : QuestIds)
	{
		const FMatterFluxQuestDefinition& Quest =
			Registry->Quests.FindChecked(QuestId);
		if (Quest.ActivationCreatureSpawns.IsEmpty()
			|| !ShouldActivateQuestCreatureSpawns(QuestId))
		{
			continue;
		}
		FConfiguredCreatureWave& Wave = Waves.AddDefaulted_GetRef();
		Wave.Key = FName(*FString::Printf(
			TEXT("quest.%s"), *QuestId.ToString()));
		Wave.QuestId = QuestId;
		Wave.Spawns = Quest.ActivationCreatureSpawns;
	}

	for (const FConfiguredCreatureWave& Wave : Waves)
	{
		if (SpawnedCreatureWaves.Contains(Wave.Key)) continue;
		// TryResolveTerrainSpawnLocation reads the canonical heightfield and can
		// therefore return a valid Z even when the corresponding Chaos collision
		// chunk is not resident. Pin and wait for every authored marker before
		// constructing any member of the wave; otherwise distant bosses begin
		// simulating over empty physics space and fall out of the world.
		bool bWaveTerrainReady = true;
		for (const FMatterFluxCreatureSpawnDefinition& Spawn : Wave.Spawns)
		{
			FVector MarkerLocation;
			if (!PlayableWorld->TryGetCustomMapMarker(
				Spawn.MarkerId, MarkerLocation))
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Creature wave '%s' cannot pin missing marker '%s' on map '%s'"),
					*Wave.Key.ToString(),
					*Spawn.MarkerId.ToString(),
					*CurrentCustomMapId.ToString());
				bWaveTerrainReady = false;
				break;
			}
			const FName PinKey(*FString::Printf(
				TEXT("%s.%s"),
				*Wave.Key.ToString(),
				*Spawn.MarkerId.ToString()));
			FIntPoint* TerrainChunk =
				ConfiguredCreatureTerrainPins.Find(PinKey);
			if (!TerrainChunk)
			{
				FIntPoint RequestedChunk;
				if (!PlayableWorld->RequestPlayerSpawnRegion(
					MarkerLocation, RequestedChunk))
				{
					bWaveTerrainReady = false;
					continue;
				}
				TerrainChunk = &ConfiguredCreatureTerrainPins.Add(
					PinKey, RequestedChunk);
			}
			bWaveTerrainReady &=
				PlayableWorld->IsPlayerSpawnRegionTerrainReady(
					*TerrainChunk);
		}
		if (!bWaveTerrainReady)
		{
			continue;
		}
		TArray<AMatterFluxCreatureActor*> DeferredCreatures;
		TArray<FTransform> SpawnTransforms;
		const AMatterFluxCreatureActor* CreatureDefault =
			GetDefault<AMatterFluxCreatureActor>();
		const UCapsuleComponent* DefaultCapsule = CreatureDefault
			? CreatureDefault->GetCapsuleComponent()
			: nullptr;
		const float ConstructionRadius = DefaultCapsule
			? DefaultCapsule->GetScaledCapsuleRadius()
			: 0.0f;
		const float ConstructionHalfHeight = DefaultCapsule
			? DefaultCapsule->GetScaledCapsuleHalfHeight()
			: 0.0f;
		bool bWaveReady = true;
		for (const FMatterFluxCreatureSpawnDefinition& Spawn : Wave.Spawns)
		{
			const FMatterFluxCreatureDefinition* Definition =
				Registry->Creatures.Find(Spawn.CreatureId);
			if (!Definition)
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Creature wave '%s' references missing creature '%s'"),
					*Wave.Key.ToString(), *Spawn.CreatureId.ToString());
				bWaveReady = false;
				break;
			}
			FVector Location;
			if (!PlayableWorld->TryGetCustomMapMarker(Spawn.MarkerId, Location))
			{
				UE_LOG(LogMatterFlux, Error,
					TEXT("Creature wave '%s' cannot find marker '%s' on map '%s'"),
					*Wave.Key.ToString(),
					*Spawn.MarkerId.ToString(),
					*CurrentCustomMapId.ToString());
				bWaveReady = false;
				break;
			}
			const float FinalRadius = Definition->Width * 0.5f;
			// UE capsules clamp half-height to at least their radius. Wide, short
			// creatures (for example the slime) must use that effective value when
			// computing feet height or the post-initialize capsule can sink again.
			const float FinalHalfHeight = FMath::Max(
				FinalRadius, Definition->Height * 0.5f);
			FVector ConstructionLocation;
			FVector FinalLocation;
			const bool bHasConstructionLocation =
				PlayableWorld->TryResolveTerrainSpawnLocation(
					Location,
					FMath::Max(ConstructionRadius, FinalRadius),
					FMath::Max(
						ConstructionHalfHeight, FinalHalfHeight),
					4.0f,
					ConstructionLocation);
			const bool bHasFinalLocation =
				PlayableWorld->TryResolveTerrainSpawnLocation(
					Location,
					FinalRadius,
					FinalHalfHeight,
					4.0f,
					FinalLocation);
			if (!bHasConstructionLocation || !bHasFinalLocation)
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Could not resolve terrain-safe creature spawn '%s' at %s"),
					*Spawn.MarkerId.ToString(),
					*Location.ToCompactString());
				bWaveReady = false;
				break;
			}
			const FTransform ConstructionTransform(
				FRotator::ZeroRotator, ConstructionLocation);
			const FTransform FinalTransform(
				FRotator::ZeroRotator, FinalLocation);
			AMatterFluxCreatureActor* Creature =
				GetWorld()->SpawnActorDeferred<AMatterFluxCreatureActor>(
					AMatterFluxCreatureActor::StaticClass(),
					ConstructionTransform,
					nullptr,
					nullptr,
					ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
			if (!Creature)
			{
				bWaveReady = false;
				break;
			}
			Creature->InitializeCreature(Spawn.CreatureId);
			Creature->Tags.AddUnique(TEXT("MatterFluxConfiguredCreature"));
			DeferredCreatures.Add(Creature);
			SpawnTransforms.Add(FinalTransform);
		}
		if (!bWaveReady)
		{
			for (AMatterFluxCreatureActor* Existing : DeferredCreatures)
			{
				if (Existing) Existing->Destroy();
			}
			continue;
		}
		for (int32 Index = 0; Index < DeferredCreatures.Num(); ++Index)
		{
			DeferredCreatures[Index]->FinishSpawning(SpawnTransforms[Index]);
		}
		UE_LOG(LogMatterFlux, Display,
			TEXT("Spawned configured creature wave '%s' with %d creature(s) (quest='%s')"),
			*Wave.Key.ToString(), DeferredCreatures.Num(),
			*Wave.QuestId.ToString());
		SpawnedCreatureWaves.Add(Wave.Key);
	}
}
