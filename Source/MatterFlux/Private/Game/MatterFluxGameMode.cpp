#include "Game/MatterFluxGameMode.h"

#include "Game/MatterFluxCharacter.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Game/MatterFluxGameState.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
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
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			GetWorld()->SpawnActor<AMatterFluxPlayableWorldActor>(
				PlayableWorldClass,
				FTransform::Identity,
				SpawnParameters);
		}
	}

	Super::StartPlay();
	if (HasAuthority() && GetWorld())
	{
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
		GetWorld()->GetTimerManager().ClearTimer(CreatureSpawnTimer);
	}
	Super::EndPlay(EndPlayReason);
}

bool AMatterFluxGameMode::ShouldSpawnCreature(
	const FName SpawnQuestId) const
{
	if (SpawnQuestId.IsNone()) return true;
	const AGameStateBase* CurrentGameState = GetGameState<AGameStateBase>();
	if (!CurrentGameState) return false;
	for (APlayerState* PlayerState : CurrentGameState->PlayerArray)
	{
		const AMatterFluxPlayerState* MatterFluxState =
			Cast<AMatterFluxPlayerState>(PlayerState);
		const UMatterFluxProgressionComponent* Progression =
			MatterFluxState ? MatterFluxState->GetProgression() : nullptr;
		const FMatterFluxQuestState* Quest = Progression
			? Progression->FindQuestState(SpawnQuestId) : nullptr;
		if (Quest && (Quest->Status == EMatterFluxQuestRuntimeStatus::Active
			|| Quest->Status == EMatterFluxQuestRuntimeStatus::Completed))
		{
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

	FVector Anchor = FVector::ZeroVector;
	bool bHasPlayerAnchor = false;
	for (TActorIterator<AMatterFluxCharacter> It(GetWorld()); It; ++It)
	{
		Anchor = It->GetActorLocation();
		bHasPlayerAnchor = true;
		break;
	}
	if (!bHasPlayerAnchor) return;
	TArray<FName> CreatureIds;
	Registry->Creatures.GetKeys(CreatureIds);
	CreatureIds.Sort(FNameLexicalLess());
	for (const FName CreatureId : CreatureIds)
	{
		if (SpawnedCreatureDefinitions.Contains(CreatureId)) continue;
		const FMatterFluxCreatureDefinition& Definition =
			Registry->Creatures.FindChecked(CreatureId);
		if (Definition.SpawnCount <= 0
			|| !ShouldSpawnCreature(Definition.SpawnQuestId))
		{
			continue;
		}
		TArray<AMatterFluxCreatureActor*> DeferredCreatures;
		TArray<FTransform> SpawnTransforms;
		const uint32 IdHash = FCrc::StrCrc32(*CreatureId.ToString());
		for (int32 Index = 0; Index < Definition.SpawnCount; ++Index)
		{
			const float Angle = static_cast<float>(IdHash % 360u)
				+ 360.0f * static_cast<float>(Index)
					/ static_cast<float>(Definition.SpawnCount);
			const float Radius = Definition.SpawnDistance
				+ static_cast<float>(Index) * 90.0f;
			FVector Location = Anchor + FVector(
				FMath::Cos(FMath::DegreesToRadians(Angle)) * Radius,
				FMath::Sin(FMath::DegreesToRadians(Angle)) * Radius,
				0.0f);
			float TerrainHeight = 0.0f;
			if (!PlayableWorld->TrySampleTerrainHeightAtWorldLocation(
				Location, TerrainHeight))
			{
				return;
			}
			Location.Z = TerrainHeight + Definition.Height * 0.55f + 20.0f;
			const FTransform Transform(FRotator::ZeroRotator, Location);
			AMatterFluxCreatureActor* Creature =
				GetWorld()->SpawnActorDeferred<AMatterFluxCreatureActor>(
					AMatterFluxCreatureActor::StaticClass(),
					Transform,
					nullptr,
					nullptr,
					ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
			if (!Creature)
			{
				for (AMatterFluxCreatureActor* Existing : DeferredCreatures)
				{
					if (Existing) Existing->Destroy();
				}
				return;
			}
			Creature->InitializeCreature(CreatureId);
			DeferredCreatures.Add(Creature);
			SpawnTransforms.Add(Transform);
		}
		for (int32 Index = 0; Index < DeferredCreatures.Num(); ++Index)
		{
			DeferredCreatures[Index]->FinishSpawning(SpawnTransforms[Index]);
		}
		UE_LOG(LogMatterFlux, Display,
			TEXT("Spawned %d Lua creature(s) for '%s' (quest='%s')"),
			DeferredCreatures.Num(), *CreatureId.ToString(),
			*Definition.SpawnQuestId.ToString());
		SpawnedCreatureDefinitions.Add(CreatureId);
	}
}
