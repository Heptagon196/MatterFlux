// Developer-only engine adapter for recording and replay automation.
#include "Testing/MatterFluxSessionRecorderSubsystem.h"

#include "Testing/MatterFluxSessionRecordingPolicy.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "MatterFluxLog.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

using namespace MatterFlux::Recording::Private;

void UMatterFluxSessionRecorderSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	FString Error;
	if (!MatterFlux::Recording::ParseLaunchOptions(
		FCommandLine::Get(),
		Options,
		Error))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Session recording arguments are invalid: %s"),
			*Error);
		Options = FMatterFluxRecordingLaunchOptions();
		return;
	}
	if (!IsActive())
	{
		return;
	}

	if (IsReplaying())
	{
		FString Json;
		bool bLoaded = false;
		const int64 ReplayFileSize =
			IFileManager::Get().FileSize(*Options.ReplayFile);
		if (ReplayFileSize < 0)
		{
			Error = TEXT("Replay file does not exist or cannot be inspected.");
		}
		else if (ReplayFileSize > MaxFileBytes)
		{
			Error = FString::Printf(
				TEXT("Replay file exceeds the %lld-byte size limit."),
				MaxFileBytes);
		}
		else if (!FFileHelper::LoadFileToString(
			Json,
			*Options.ReplayFile))
		{
			Error = TEXT("Replay file could not be read.");
		}
		else
		{
			bLoaded = MatterFlux::Recording::LoadFromJson(
				Json,
				Recording,
				Error);
			if (!bLoaded && Error.IsEmpty())
			{
				Error = TEXT("Replay JSON was rejected.");
			}
		}
		if (!bLoaded)
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Could not load replay '%s': %s"),
				*Options.ReplayFile,
				*Error);
			Options.bReplay = false;
			return;
		}
	}
	else
	{
		Recording.Version = FMatterFluxSessionRecording::LatestVersion;
		Recording.CreatedUtc = FDateTime::UtcNow().ToIso8601();
		Recording.EngineVersion = FEngineVersion::Current().ToString();
		Recording.StateIntervalSeconds = Options.StateIntervalSeconds;
		Recording.Screenshots = Options.ScheduledScreenshots;
	}

	PlayerOperationHandle =
		MatterFlux::PlayerOperations::OnApplied().AddUObject(
			this,
			&UMatterFluxSessionRecorderSubsystem::HandlePlayerOperation);
	PreExitHandle = FCoreDelegates::OnPreExit.AddUObject(
		this,
		&UMatterFluxSessionRecorderSubsystem::HandlePreExit);
	UE_LOG(
		LogMatterFlux,
		Display,
		TEXT("MatterFlux session %s enabled."),
		IsReplaying() ? TEXT("replay") : TEXT("recording"));
}

void UMatterFluxSessionRecorderSubsystem::Deinitialize()
{
	if (PlayerOperationHandle.IsValid())
	{
		MatterFlux::PlayerOperations::OnApplied().Remove(
			PlayerOperationHandle);
		PlayerOperationHandle.Reset();
	}
	if (PreExitHandle.IsValid())
	{
		FCoreDelegates::OnPreExit.Remove(PreExitHandle);
		PreExitHandle.Reset();
	}
	FlushRecording();
	Super::Deinitialize();
}

void UMatterFluxSessionRecorderSubsystem::HandlePreExit()
{
	FlushRecording();
}

void UMatterFluxSessionRecorderSubsystem::HandlePlayerOperation(
	AMatterFluxCharacter& Character,
	const EMatterFluxPlayerOperation Operation,
	const FVector2D Value,
	const int32 IntegerValue,
	const bool bRelayedFromClient)
{
	if (!IsRecording()
		|| Character.GetGameInstance() != GetGameInstance()
		|| (bRelayedFromClient && !Character.HasAuthority()))
	{
		return;
	}

	const bool bRecorded = RecordPlayerOperation(
		&Character,
		Operation,
		Value,
		IntegerValue);
	if (bRecorded && !bRelayedFromClient && !Character.HasAuthority())
	{
		Character.RelayPlayerOperationToServer(
			Operation,
			Value,
			IntegerValue);
	}
}

bool UMatterFluxSessionRecorderSubsystem::IsTickable() const
{
	return IsActive() && !HasAnyFlags(RF_ClassDefaultObject);
}

UWorld* UMatterFluxSessionRecorderSubsystem::GetTickableGameObjectWorld() const
{
	return GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
}

TStatId UMatterFluxSessionRecorderSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UMatterFluxSessionRecorderSubsystem,
		STATGROUP_Tickables);
}

void UMatterFluxSessionRecorderSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetTickableGameObjectWorld();
	if (!World || !World->IsGameWorld() || bReplayFinished)
	{
		return;
	}
	if (!bSessionStarted && !BeginSession(*World))
	{
		return;
	}

	const double ElapsedSeconds =
		FMath::Max(
			0.0,
			static_cast<double>(World->GetTimeSeconds())
				- SessionWorldStartSeconds);
	if (IsRecording())
	{
		TickRecording(*World, ElapsedSeconds);
	}
	else
	{
		TickReplay(*World, ElapsedSeconds);
	}
}

bool UMatterFluxSessionRecorderSubsystem::BeginSession(UWorld& World)
{
	AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(&World); It; ++It)
	{
		PlayableWorld = *It;
		break;
	}
	if (!PlayableWorld || PlayableWorld->GetMapSeed() == 0)
	{
		return false;
	}

	if (IsReplaying()
		&& Recording.WorldSeed > 0
		&& PlayableWorld->HasAuthority()
		&& PlayableWorld->GetMapSeed() != Recording.WorldSeed)
	{
		PlayableWorld->Regenerate(Recording.WorldSeed);
	}
	else if (IsRecording())
	{
		if (Options.RequestedSeed > 0
			&& PlayableWorld->HasAuthority()
			&& PlayableWorld->GetMapSeed() != Options.RequestedSeed)
		{
			PlayableWorld->Regenerate(Options.RequestedSeed);
		}
		Recording.WorldSeed = PlayableWorld->GetMapSeed();
		Recording.MapName = World.GetMapName();
		OutputFilePath = BuildOutputFilePath();
	}

	if (IsReplaying())
	{
		TArray<AMatterFluxCharacter*> Characters;
		for (TActorIterator<AMatterFluxCharacter> It(&World); It; ++It)
		{
			Characters.Add(*It);
		}
		Characters.Sort(
			[](const AMatterFluxCharacter& A, const AMatterFluxCharacter& B)
			{
				const APlayerState* AState = A.GetPlayerState();
				const APlayerState* BState = B.GetPlayerState();
				const int32 AId = AState ? AState->GetPlayerId() : INDEX_NONE;
				const int32 BId = BState ? BState->GetPlayerId() : INDEX_NONE;
				return AId < BId;
			});
		TArray<FMatterFluxRecordedPlayer> Players = Recording.Players;
		Players.Sort(
			[](const FMatterFluxRecordedPlayer& A,
				const FMatterFluxRecordedPlayer& B)
			{
				return A.PlayerId < B.PlayerId;
			});
		const int32 Count = FMath::Min(Characters.Num(), Players.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			ReplayPlayers.Add(Players[Index].PlayerId, Characters[Index]);
		}
		MatterFlux::Recording::FReplayRuntimeSettings ReplaySettings;
		ReplaySettings.bVerifyStates = Options.bVerifyReplayStates;
		FString ReplayError;
		if (!ReplayRuntime.Initialize(
			Recording,
			ReplaySettings,
			ReplayError))
		{
			bReplayPassed = false;
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Replay timeline initialization failed: %s"),
				*ReplayError);
			FinishReplay();
			return false;
		}
	}

	SessionWorldStartSeconds = World.GetTimeSeconds();
	NextStateSampleSeconds = 0.0;
	NextRecordingScreenshotIndex = 0;
	bSessionStarted = true;
	UE_LOG(
		LogMatterFlux,
		Display,
		TEXT("MatterFlux session %s started: seed=%d map=%s"),
		IsReplaying() ? TEXT("replay") : TEXT("recording"),
		Recording.WorldSeed,
		*World.GetMapName());
	return true;
}

void UMatterFluxSessionRecorderSubsystem::TickRecording(
	UWorld& World,
	const double ElapsedSeconds)
{
	Recording.DurationSeconds = ElapsedSeconds;
	if (ElapsedSeconds + KINDA_SMALL_NUMBER >= NextStateSampleSeconds)
	{
		SamplePlayerStates(World, ElapsedSeconds);
		// Missed samples cannot be reconstructed after a hitch. Scheduling
		// from the current timestamp avoids a burst of duplicate current-state
		// samples while an old fixed-rate deadline catches up.
		NextStateSampleSeconds =
			ElapsedSeconds + Recording.StateIntervalSeconds;
	}
	CaptureDueScreenshots(World, ElapsedSeconds);
	if (Options.RecordDurationSeconds > 0.0
		&& ElapsedSeconds >= Options.RecordDurationSeconds)
	{
		const bool bQuit = Options.bQuitAfterRecord;
		FString Error;
		const bool bSaved = FlushRecording(&Error);
		if (bQuit)
		{
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[bSaved](float)
					{
						FPlatformMisc::RequestExitWithStatus(
							false,
							bSaved ? 0 : 3);
						return false;
					}),
				0.5f);
		}
	}
}

void UMatterFluxSessionRecorderSubsystem::TickReplay(
	UWorld& World,
	const double ElapsedSeconds)
{
	MatterFlux::Recording::FReplayFrame Frame;
	FString Error;
	if (!ReplayRuntime.Advance(ElapsedSeconds, Frame, Error))
	{
		bReplayPassed = false;
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Replay timeline advance failed: %s"),
			*Error);
		FinishReplay();
		return;
	}
	for (const FMatterFluxRecordedOperation& Operation : Frame.Operations)
	{
		ApplyReplayOperation(World, Operation);
	}
	for (const MatterFlux::Recording::FReplayMovement& Movement
		: Frame.Movements)
	{
		if (AMatterFluxCharacter* Character = FindPlayerById(
			World,
			Movement.PlayerId))
		{
			Character->ApplyPlayerOperation(
				EMatterFluxRecordedOperation::Move,
				Movement.Value,
				0);
		}
		else
		{
			bReplayPassed = false;
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Replay movement could not find player %d."),
				Movement.PlayerId);
		}
	}
	for (const MatterFlux::Recording::FReplayExpectedState& Expected
		: Frame.ExpectedStates)
	{
		VerifyReplayExpectedState(World, Expected);
	}
	for (const FMatterFluxRecordedScreenshot& Screenshot : Frame.Screenshots)
	{
		CaptureScreenshot(World, Screenshot, true);
	}
	if (Frame.bComplete)
	{
		FinishReplay();
	}
}

bool UMatterFluxSessionRecorderSubsystem::RecordPlayerOperation(
	AMatterFluxCharacter* Character,
	const EMatterFluxPlayerOperation Operation,
	const FVector2D& Value,
	const int32 IntegerValue)
{
	UWorld* CharacterWorld =
		IsValid(Character) ? Character->GetWorld() : nullptr;
	FMatterFluxRecordedOperation Candidate;
	Candidate.TimeSeconds = CharacterWorld
		? FMath::Max(
			0.0,
			static_cast<double>(CharacterWorld->GetTimeSeconds())
				- SessionWorldStartSeconds)
		: -1.0;
	Candidate.Operation = Operation;
	Candidate.Value = Value;
	Candidate.IntegerValue = IntegerValue;
	if (!IsRecording()
		|| !bSessionStarted
		|| !CharacterWorld
		|| Recording.Operations.Num() >= MaxOperations
		|| !IsValidOperationPayload(Candidate))
	{
		return false;
	}
	const TOptional<int32> PlayerId = ResolvePlayerId(*Character);
	if (!PlayerId.IsSet())
	{
		return false;
	}
	if (Operation == EMatterFluxRecordedOperation::Move)
	{
		const FVector2D* Previous =
			LastRecordedMovementByPlayer.Find(PlayerId.GetValue());
		if (Previous && Previous->Equals(Value, 0.001))
		{
			return false;
		}
		LastRecordedMovementByPlayer.Add(PlayerId.GetValue(), Value);
	}

	FMatterFluxRecordedOperation& Entry =
		Recording.Operations.AddDefaulted_GetRef();
	Entry.TimeSeconds = Candidate.TimeSeconds;
	Entry.PlayerId = PlayerId.GetValue();
	Entry.Operation = Operation;
	Entry.Value = Value;
	Entry.IntegerValue = IntegerValue;
	return true;
}

TOptional<int32> UMatterFluxSessionRecorderSubsystem::ResolvePlayerId(
	AMatterFluxCharacter& Character)
{
	const APlayerState* PlayerState = Character.GetPlayerState();
	if (!PlayerState || PlayerState->GetPlayerId() == INDEX_NONE)
	{
		return TOptional<int32>();
	}
	const int32 PlayerId = PlayerState->GetPlayerId();
	const bool bKnownPlayer = Recording.Players.ContainsByPredicate(
		[PlayerId](const FMatterFluxRecordedPlayer& Player)
		{
			return Player.PlayerId == PlayerId;
		});
	if (!bKnownPlayer)
	{
		if (Recording.Players.Num() >= MaxPlayers)
		{
			return TOptional<int32>();
		}
		FMatterFluxRecordedPlayer& Player =
			Recording.Players.AddDefaulted_GetRef();
		Player.PlayerId = PlayerId;
		Player.PlayerName =
			PlayerState->GetPlayerName()
				.Left(MaxPlayerNameCharacters);
		for (int32 Index = 0; Index < Player.PlayerName.Len(); ++Index)
		{
			if (Player.PlayerName[Index] == TEXT('\0'))
			{
				Player.PlayerName[Index] = TEXT('_');
			}
		}
	}
	return PlayerId;
}

void UMatterFluxSessionRecorderSubsystem::SamplePlayerStates(
	UWorld& World,
	const double ElapsedSeconds)
{
	for (TActorIterator<AMatterFluxCharacter> It(&World); It; ++It)
	{
		AMatterFluxCharacter* Character = *It;
		if (Recording.States.Num() >= MaxStates)
		{
			break;
		}
		if (!IsValid(Character))
		{
			continue;
		}
		FMatterFluxRecordedPlayerState Candidate;
		Candidate.TimeSeconds = ElapsedSeconds;
		Candidate.Location = Character->GetActorLocation();
		Candidate.Rotation = Character->GetActorRotation();
		Candidate.Velocity = Character->GetVelocity();
		const UCharacterMovementComponent* Movement =
			Character->GetCharacterMovement();
		Candidate.MovementMode = Movement
			? static_cast<uint8>(Movement->MovementMode)
			: 0;
		if (!IsValidStatePayload(Candidate))
		{
			continue;
		}
		const TOptional<int32> PlayerId =
			ResolvePlayerId(*Character);
		if (!PlayerId.IsSet())
		{
			continue;
		}
		Candidate.PlayerId = PlayerId.GetValue();
		Recording.States.Add(Candidate);
	}
}

void UMatterFluxSessionRecorderSubsystem::RequestScreenshot(
	const FString& Label)
{
	if (!IsRecording()
		|| !bSessionStarted
		|| Recording.Screenshots.Num() >= MaxScreenshots)
	{
		return;
	}
	FMatterFluxRecordedScreenshot& Screenshot =
		Recording.Screenshots.AddDefaulted_GetRef();
	UWorld* World = GetTickableGameObjectWorld();
	Screenshot.TimeSeconds = World
		? FMath::Max(
			0.0,
			static_cast<double>(World->GetTimeSeconds())
				- SessionWorldStartSeconds)
		: Recording.DurationSeconds;
	Screenshot.Label = SanitizeScreenshotLabel(Label);
	SortByTime(Recording.Screenshots);
}

void UMatterFluxSessionRecorderSubsystem::CaptureDueScreenshots(
	UWorld& World,
	const double ElapsedSeconds)
{
	while (NextRecordingScreenshotIndex < Recording.Screenshots.Num()
		&& Recording.Screenshots[NextRecordingScreenshotIndex].TimeSeconds
			<= ElapsedSeconds + KINDA_SMALL_NUMBER)
	{
		CaptureScreenshot(
			World,
			Recording.Screenshots[NextRecordingScreenshotIndex],
			false);
		++NextRecordingScreenshotIndex;
	}
}

void UMatterFluxSessionRecorderSubsystem::CaptureScreenshot(
	UWorld& World,
	const FMatterFluxRecordedScreenshot& Screenshot,
	const bool bReplayCapture)
{
	if (!GEngine
		|| !GEngine->GameViewport
		|| GEngine->GameViewport->GetWorld() != &World)
	{
		UE_LOG(
			LogMatterFlux,
			Verbose,
			TEXT("Screenshot '%s' skipped because this process has no game viewport."),
			*Screenshot.Label);
		return;
	}

	FString Directory = bReplayCapture
		? Options.ReplayOutputDirectory
		: Options.RecordDirectory;
	if (Directory.IsEmpty())
	{
		Directory = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			bReplayCapture ? TEXT("Replays") : TEXT("Recordings"));
	}
	Directory = FPaths::Combine(Directory, TEXT("Screenshots"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Filename = FString::Printf(
		TEXT("%012lld_%s.png"),
		FMath::RoundToInt64(Screenshot.TimeSeconds * 1000.0),
		*SanitizeScreenshotLabel(Screenshot.Label));
	const FString Path = FPaths::Combine(Directory, Filename);
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
		TEXT("Session screenshot requested: %s"),
		*Path);
}

AMatterFluxCharacter*
UMatterFluxSessionRecorderSubsystem::FindPlayerById(
	UWorld& World,
	const int32 PlayerId) const
{
	if (const TWeakObjectPtr<AMatterFluxCharacter>* Character =
		ReplayPlayers.Find(PlayerId))
	{
		return Character->Get();
	}
	for (TActorIterator<AMatterFluxCharacter> It(&World); It; ++It)
	{
		const APlayerState* PlayerState = It->GetPlayerState();
		if (PlayerState && PlayerState->GetPlayerId() == PlayerId)
		{
			return *It;
		}
	}
	return nullptr;
}

void UMatterFluxSessionRecorderSubsystem::ApplyReplayOperation(
	UWorld& World,
	const FMatterFluxRecordedOperation& Operation)
{
	if (AMatterFluxCharacter* Character =
		FindPlayerById(World, Operation.PlayerId))
	{
		Character->ApplyPlayerOperation(
			Operation.Operation,
			Operation.Value,
			Operation.IntegerValue);
	}
	else
	{
		bReplayPassed = false;
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Replay operation could not find player %d."),
			Operation.PlayerId);
	}
}

void UMatterFluxSessionRecorderSubsystem::VerifyReplayExpectedState(
	UWorld& World,
	const MatterFlux::Recording::FReplayExpectedState& Expected)
{
	AMatterFluxCharacter* Character =
		FindPlayerById(World, Expected.PlayerId);
	if (!Character)
	{
		bReplayPassed = false;
		return;
	}
	const double LocationError = FVector::Distance(
		Character->GetActorLocation(),
		Expected.Location);
	if (LocationError > Options.ReplayLocationTolerance)
	{
		bReplayPassed = false;
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Replay state mismatch at %.3fs for player %d: location error %.2f cm exceeds %.2f cm."),
			Expected.TimeSeconds,
			Expected.PlayerId,
			LocationError,
			Options.ReplayLocationTolerance);
	}
}

void UMatterFluxSessionRecorderSubsystem::FinishReplay()
{
	bReplayFinished = true;
	if (bReplayPassed)
	{
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("MatterFlux replay complete: PASS"));
	}
	else
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("MatterFlux replay complete: FAIL"));
	}
	if (Options.bQuitAfterReplay)
	{
		FPlatformMisc::RequestExitWithStatus(
			false,
			bReplayPassed ? 0 : 2);
	}
}

FString UMatterFluxSessionRecorderSubsystem::BuildOutputFilePath() const
{
	FString Directory = Options.RecordDirectory;
	if (Directory.IsEmpty())
	{
		Directory = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Recordings"));
	}
	FString Name = Options.RecordName;
	if (Name.IsEmpty())
	{
		Name = FString::Printf(
			TEXT("MatterFlux_%s"),
			*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
	}
	Name = FPaths::MakeValidFileName(Name);
	if (!Name.EndsWith(TEXT(".mfrecord.json")))
	{
		Name += TEXT(".mfrecord.json");
	}
	return FPaths::Combine(Directory, Name);
}

bool UMatterFluxSessionRecorderSubsystem::FlushRecording(FString* OutError)
{
	if (OutError)
	{
		OutError->Reset();
	}
	if (!IsRecording() || bFlushed)
	{
		return true;
	}
	UWorld* World = GetTickableGameObjectWorld();
	if (World && World->GetNetMode() == NM_Client)
	{
		bFlushed = true;
		Options.bRecord = false;
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("Client recording was relayed to the server; no duplicate recording file was written."));
		return true;
	}
	if (OutputFilePath.IsEmpty())
	{
		OutputFilePath = BuildOutputFilePath();
	}
	if (bSessionStarted && World)
	{
		Recording.DurationSeconds = FMath::Clamp(
			static_cast<double>(World->GetTimeSeconds())
				- SessionWorldStartSeconds,
			0.0,
			MaxTimeSeconds);
	}
	Recording.Screenshots.RemoveAll(
		[this](const FMatterFluxRecordedScreenshot& Screenshot)
		{
			return Screenshot.TimeSeconds > Recording.DurationSeconds;
		});

	FString Json;
	FString Error;
	if (!MatterFlux::Recording::SaveToJson(Recording, Json, Error))
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}

	const FString Directory = FPaths::GetPath(OutputFilePath);
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString TemporaryPath = OutputFilePath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(
		Json,
		*TemporaryPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(
			*OutputFilePath,
			*TemporaryPath,
			true,
			true))
	{
		Error = FString::Printf(
			TEXT("Could not atomically write recording to %s"),
			*OutputFilePath);
		if (OutError)
		{
			*OutError = Error;
		}
		UE_LOG(LogMatterFlux, Error, TEXT("%s"), *Error);
		return false;
	}

	bFlushed = true;
	Options.bRecord = false;
	UE_LOG(
		LogMatterFlux,
		Display,
		TEXT("MatterFlux recording saved: %s (%d operations, %d states, %d screenshots)"),
		*OutputFilePath,
		Recording.Operations.Num(),
		Recording.States.Num(),
		Recording.Screenshots.Num());
	return true;
}

namespace
{
	TMap<int32, FVector2D> GInjectedMovementByPlayer;
	bool GInjectedMovementTickerActive = false;

	UMatterFluxSessionRecorderSubsystem* GetRecorder(UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance
			? GameInstance->GetSubsystem<
				UMatterFluxSessionRecorderSubsystem>()
			: nullptr;
	}

	void GetOrderedCharacters(
		UWorld& World,
		TArray<AMatterFluxCharacter*>& OutCharacters)
	{
		OutCharacters.Reset();
		for (TActorIterator<AMatterFluxCharacter> It(&World); It; ++It)
		{
			OutCharacters.Add(*It);
		}
		OutCharacters.Sort(
			[](const AMatterFluxCharacter& A,
				const AMatterFluxCharacter& B)
			{
				const APlayerState* AState = A.GetPlayerState();
				const APlayerState* BState = B.GetPlayerState();
				return (AState ? AState->GetPlayerId() : INDEX_NONE)
					< (BState ? BState->GetPlayerId() : INDEX_NONE);
			});
	}

	void EnsureInjectedMovementTicker()
	{
		if (GInjectedMovementTickerActive)
		{
			return;
		}
		GInjectedMovementTickerActive = true;
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[](float)
				{
					UWorld* World =
						GEngine && GEngine->GameViewport
							? GEngine->GameViewport->GetWorld()
							: nullptr;
					UMatterFluxSessionRecorderSubsystem* Recorder =
						GetRecorder(World);
					if (!World
						|| !World->IsGameWorld()
						|| !Recorder
						|| !Recorder->IsRecording()
						|| !Recorder->HasSessionStarted())
					{
						GInjectedMovementByPlayer.Reset();
						GInjectedMovementTickerActive = false;
						return false;
					}
					TArray<AMatterFluxCharacter*> Characters;
					GetOrderedCharacters(*World, Characters);
					for (const TPair<int32, FVector2D>& Pair
						: GInjectedMovementByPlayer)
					{
						if (Characters.IsValidIndex(Pair.Key))
						{
							Characters[Pair.Key]->ApplyPlayerOperation(
								EMatterFluxRecordedOperation::Move,
								Pair.Value);
						}
					}
					return true;
				}),
			0.0f);
	}

	void RequestRecordingScreenshot(
		const TArray<FString>& Args,
		UWorld* World)
	{
		if (UMatterFluxSessionRecorderSubsystem* Recorder =
			GetRecorder(World))
		{
			Recorder->RequestScreenshot(
				Args.Num() > 0 ? Args[0] : TEXT("Manual"));
		}
	}

	void FlushSessionRecording(
		const TArray<FString>&,
		UWorld* World)
	{
		if (UMatterFluxSessionRecorderSubsystem* Recorder =
			GetRecorder(World))
		{
			FString Error;
			if (!Recorder->FlushRecording(&Error))
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Manual recording flush failed: %s"),
					*Error);
			}
		}
	}

	void QueueRecordedOperationInjection(
		const TArray<FString>& Args,
		UWorld*)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Usage: mf.Record.Inject <delay-seconds> <operation> [x=0] [y=0] [integer=0] [player-index=0]"));
			return;
		}

		double DelaySeconds = 0.0;
		EMatterFluxRecordedOperation Operation;
		if (!LexTryParseString(DelaySeconds, *Args[0])
			|| !FMath::IsFinite(DelaySeconds)
			|| DelaySeconds < 0.0
			|| !MatterFlux::Recording::TryParseOperation(
				Args[1],
				Operation))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("mf.Record.Inject received an invalid delay or operation."));
			return;
		}

		FVector2D Value = FVector2D::ZeroVector;
		int32 IntegerValue = 0;
		int32 PlayerIndex = 0;
		const bool bPayloadParsed =
			(Args.Num() <= 2
				|| LexTryParseString(Value.X, *Args[2]))
			&& (Args.Num() <= 3
				|| LexTryParseString(Value.Y, *Args[3]))
			&& (Args.Num() <= 4
				|| LexTryParseString(IntegerValue, *Args[4]))
			&& (Args.Num() <= 5
				|| LexTryParseString(PlayerIndex, *Args[5]));
		if (!bPayloadParsed
			|| !FMath::IsFinite(Value.X)
			|| !FMath::IsFinite(Value.Y)
			|| FMath::Abs(Value.X) > 100.0
			|| FMath::Abs(Value.Y) > 100.0
			|| PlayerIndex < 0)
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("mf.Record.Inject received invalid payload values."));
			return;
		}

		const TSharedRef<double> WorldReadyAt = MakeShared<double>(-1.0);
		const double QueuedAt = FPlatformTime::Seconds();
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[DelaySeconds,
					Operation,
					Value,
					IntegerValue,
					PlayerIndex,
					WorldReadyAt,
					QueuedAt](float)
				{
					UWorld* World =
						GEngine && GEngine->GameViewport
							? GEngine->GameViewport->GetWorld()
							: nullptr;
					UMatterFluxSessionRecorderSubsystem* Recorder =
						GetRecorder(World);
					if (!World
						|| !World->IsGameWorld()
						|| !Recorder
						|| !Recorder->IsRecording()
						|| !Recorder->HasSessionStarted())
					{
						if (FPlatformTime::Seconds() - QueuedAt >= 30.0)
						{
							UE_LOG(
								LogMatterFlux,
								Error,
								TEXT("mf.Record.Inject timed out waiting for an active recording world."));
							return false;
						}
						return true;
					}
					const double Now = World->GetTimeSeconds();
					if (*WorldReadyAt < 0.0)
					{
						*WorldReadyAt = Now;
					}
					if (Now - *WorldReadyAt < DelaySeconds)
					{
						return true;
					}

					TArray<AMatterFluxCharacter*> Characters;
					GetOrderedCharacters(*World, Characters);
					if (!Characters.IsValidIndex(PlayerIndex))
					{
						UE_LOG(
							LogMatterFlux,
							Error,
							TEXT("mf.Record.Inject could not find player index %d."),
							PlayerIndex);
						return false;
					}
					if (Operation == EMatterFluxRecordedOperation::Move)
					{
						if (Value.IsNearlyZero())
						{
							GInjectedMovementByPlayer.Remove(PlayerIndex);
							Characters[PlayerIndex]->ApplyPlayerOperation(
								Operation,
								FVector2D::ZeroVector,
								IntegerValue);
						}
						else
						{
							GInjectedMovementByPlayer.Add(
								PlayerIndex,
								Value);
							EnsureInjectedMovementTicker();
						}
					}
					else
					{
						Characters[PlayerIndex]->ApplyPlayerOperation(
							Operation,
							Value,
							IntegerValue);
					}
					UE_LOG(
						LogMatterFlux,
						Display,
						TEXT("Injected recorded operation %s for player index %d."),
						*MatterFlux::Recording::OperationToString(
							Operation),
						PlayerIndex);
					return false;
				}),
			0.05f);
	}

	FAutoConsoleCommandWithWorldAndArgs GRecordingScreenshotCommand(
		TEXT("mf.Record.Screenshot"),
		TEXT("Add and capture a timestamped recording screenshot: mf.Record.Screenshot [label]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&RequestRecordingScreenshot));

	FAutoConsoleCommandWithWorldAndArgs GRecordingFlushCommand(
		TEXT("mf.Record.Flush"),
		TEXT("Atomically flush the active recording to disk."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&FlushSessionRecording));

	FAutoConsoleCommandWithWorldAndArgs GRecordingInjectCommand(
		TEXT("mf.Record.Inject"),
		TEXT("Schedule a semantic player operation for automation: mf.Record.Inject <delay> <operation> [x] [y] [integer] [player-index]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&QueueRecordedOperationInjection));
}
