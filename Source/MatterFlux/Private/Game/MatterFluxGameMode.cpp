#include "Game/MatterFluxGameMode.h"

#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxGameState.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerController.h"
#include "Game/MatterFluxPlayerState.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "EngineUtils.h"

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
}
