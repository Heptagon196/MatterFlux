// Developer-only command-line and JSON recording codec.
#include "Testing/MatterFluxSessionRecordingTypes.h"

#include "Testing/MatterFluxSessionRecordingPolicy.h"

#include "Dom/JsonObject.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace MatterFlux::Recording::Private;

namespace
{
	bool IsBoundedText(
		const FString& Value,
		const int32 MaximumCharacters)
	{
		if (Value.Len() > MaximumCharacters)
		{
			return false;
		}
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (Value[Index] == TEXT('\0'))
			{
				return false;
			}
		}
		return true;
	}

	bool TryConvertInt32(const double Value, int32& OutValue)
	{
		if (!FMath::IsFinite(Value)
			|| Value < static_cast<double>(MIN_int32)
			|| Value > static_cast<double>(MAX_int32)
			|| FMath::FloorToDouble(Value) != Value)
		{
			return false;
		}
		OutValue = static_cast<int32>(Value);
		return true;
	}

	bool TryConvertUint8(const double Value, uint8& OutValue)
	{
		if (!FMath::IsFinite(Value)
			|| Value < 0.0
			|| Value > static_cast<double>(MAX_uint8)
			|| FMath::FloorToDouble(Value) != Value)
		{
			return false;
		}
		OutValue = static_cast<uint8>(Value);
		return true;
	}

	bool IsValidRecordedScreenshotPayload(
		const FMatterFluxRecordedScreenshot& Screenshot)
	{
		return FMath::IsFinite(Screenshot.TimeSeconds)
			&& Screenshot.TimeSeconds >= 0.0
			&& Screenshot.TimeSeconds <= MaxTimeSeconds
			&& IsBoundedText(
				Screenshot.Label,
				MaxScreenshotLabelCharacters);
	}

	TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& Value)
	{
		return {
			MakeShared<FJsonValueNumber>(Value.X),
			MakeShared<FJsonValueNumber>(Value.Y),
			MakeShared<FJsonValueNumber>(Value.Z)
		};
	}

	TArray<TSharedPtr<FJsonValue>> Vector2DToJson(const FVector2D& Value)
	{
		return {
			MakeShared<FJsonValueNumber>(Value.X),
			MakeShared<FJsonValueNumber>(Value.Y)
		};
	}

	TArray<TSharedPtr<FJsonValue>> RotatorToJson(const FRotator& Value)
	{
		return {
			MakeShared<FJsonValueNumber>(Value.Pitch),
			MakeShared<FJsonValueNumber>(Value.Yaw),
			MakeShared<FJsonValueNumber>(Value.Roll)
		};
	}

	bool JsonToVector(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		FVector& OutValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid()
			|| !Object->TryGetArrayField(Field, Values)
			|| !Values
			|| Values->Num() != 3)
		{
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!(*Values)[0].IsValid()
			|| !(*Values)[1].IsValid()
			|| !(*Values)[2].IsValid()
			|| !(*Values)[0]->TryGetNumber(X)
			|| !(*Values)[1]->TryGetNumber(Y)
			|| !(*Values)[2]->TryGetNumber(Z))
		{
			return false;
		}
		OutValue = FVector(X, Y, Z);
		return IsFiniteVector(OutValue);
	}

	bool JsonToVector2D(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		FVector2D& OutValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid()
			|| !Object->TryGetArrayField(Field, Values)
			|| !Values
			|| Values->Num() != 2)
		{
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		if (!(*Values)[0].IsValid()
			|| !(*Values)[1].IsValid()
			|| !(*Values)[0]->TryGetNumber(X)
			|| !(*Values)[1]->TryGetNumber(Y))
		{
			return false;
		}
		OutValue = FVector2D(X, Y);
		return IsFiniteVector(OutValue);
	}

	bool JsonToRotator(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		FRotator& OutValue)
	{
		FVector Values;
		if (!JsonToVector(Object, Field, Values))
		{
			return false;
		}
		OutValue = FRotator(Values.X, Values.Y, Values.Z);
		return true;
	}

	bool ParseRawCommandLineValue(
		const TCHAR* CommandLine,
		const TCHAR* Key,
		FString& OutValue)
	{
		const TCHAR* Start =
			CommandLine ? FCString::Strifind(CommandLine, Key) : nullptr;
		if (!Start)
		{
			return false;
		}
		Start += FCString::Strlen(Key);
		if (*Start == TEXT('"'))
		{
			++Start;
			const TCHAR* End = FCString::Strchr(Start, TEXT('"'));
			OutValue = End
				? FString(static_cast<int32>(End - Start), Start)
				: FString(Start);
			return true;
		}
		const TCHAR* End = Start;
		while (*End && !FChar::IsWhitespace(*End))
		{
			++End;
		}
		OutValue = FString(static_cast<int32>(End - Start), Start);
		return true;
	}
}

namespace MatterFlux::Recording
{
	FString OperationToString(const EMatterFluxRecordedOperation Operation)
	{
		switch (Operation)
		{
		case EMatterFluxRecordedOperation::Move:
			return TEXT("Move");
		case EMatterFluxRecordedOperation::JumpStarted:
			return TEXT("JumpStarted");
		case EMatterFluxRecordedOperation::JumpCompleted:
			return TEXT("JumpCompleted");
		case EMatterFluxRecordedOperation::CameraZoom:
			return TEXT("CameraZoom");
		case EMatterFluxRecordedOperation::Cut:
			return TEXT("Cut");
		case EMatterFluxRecordedOperation::Flame:
			return TEXT("Flame");
		case EMatterFluxRecordedOperation::Regenerate:
			return TEXT("Regenerate");
		case EMatterFluxRecordedOperation::CastWand:
			return TEXT("CastWand");
		default:
			return TEXT("Unknown");
		}
	}

	bool TryParseOperation(
		const FString& Text,
		EMatterFluxRecordedOperation& OutOperation)
	{
		for (uint8 Value = 0;
			Value <= static_cast<uint8>(
				EMatterFluxRecordedOperation::CastWand);
			++Value)
		{
			const EMatterFluxRecordedOperation Candidate =
				static_cast<EMatterFluxRecordedOperation>(Value);
			if (OperationToString(Candidate).Equals(
				Text,
				ESearchCase::IgnoreCase))
			{
				OutOperation = Candidate;
				return true;
			}
		}
		return false;
	}

	bool ParseLaunchOptions(
		const TCHAR* CommandLine,
		FMatterFluxRecordingLaunchOptions& OutOptions,
		FString& OutError)
	{
		OutOptions = FMatterFluxRecordingLaunchOptions();
		OutError.Reset();
		if (!CommandLine)
		{
			OutError = TEXT("Command line is null.");
			return false;
		}

		OutOptions.bRecord = FParse::Param(CommandLine, TEXT("MFRecord"));
		OutOptions.bQuitAfterRecord =
			FParse::Param(CommandLine, TEXT("MFRecordQuit"));
		OutOptions.bQuitAfterReplay =
			FParse::Param(CommandLine, TEXT("MFReplayQuit"));
		OutOptions.bVerifyReplayStates =
			!FParse::Param(CommandLine, TEXT("MFReplayNoVerify"));
		FParse::Value(
			CommandLine,
			TEXT("MFRecordDir="),
			OutOptions.RecordDirectory);
		FParse::Value(
			CommandLine,
			TEXT("MFRecordName="),
			OutOptions.RecordName);
		OutOptions.bReplay = FParse::Value(
			CommandLine,
			TEXT("MFReplay="),
			OutOptions.ReplayFile);
		FParse::Value(
			CommandLine,
			TEXT("MFReplayOutputDir="),
			OutOptions.ReplayOutputDirectory);
		FParse::Value(
			CommandLine,
			TEXT("MFSeed="),
			OutOptions.RequestedSeed);
		if (OutOptions.RequestedSeed < 0)
		{
			OutError = TEXT("MFSeed must be zero or positive.");
			return false;
		}
		FParse::Value(
			CommandLine,
			TEXT("MFRecordDuration="),
			OutOptions.RecordDurationSeconds);
		if (!FMath::IsFinite(OutOptions.RecordDurationSeconds)
			|| OutOptions.RecordDurationSeconds < 0.0
			|| OutOptions.RecordDurationSeconds
				> MaxTimeSeconds)
		{
			OutError =
				TEXT("MFRecordDuration must be between zero and 366 days.");
			return false;
		}

		double StateHz = 10.0;
		FParse::Value(CommandLine, TEXT("MFRecordStateHz="), StateHz);
		if (!FMath::IsFinite(StateHz) || StateHz <= 0.0 || StateHz > 120.0)
		{
			OutError = TEXT("MFRecordStateHz must be in (0, 120].");
			return false;
		}
		OutOptions.StateIntervalSeconds = 1.0 / StateHz;
		FParse::Value(
			CommandLine,
			TEXT("MFReplayLocationTolerance="),
			OutOptions.ReplayLocationTolerance);
		if (!FMath::IsFinite(OutOptions.ReplayLocationTolerance)
			|| OutOptions.ReplayLocationTolerance < 0.0)
		{
			OutError =
				TEXT("MFReplayLocationTolerance must be finite and non-negative.");
			return false;
		}

		FString ScreenshotList;
		if (ParseRawCommandLineValue(
			CommandLine,
			TEXT("MFRecordScreenshots="),
			ScreenshotList))
		{
			TArray<FString> Entries;
			ScreenshotList.ParseIntoArray(Entries, TEXT(","), true);
			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				if (OutOptions.ScheduledScreenshots.Num()
					>= MaxScreenshots)
				{
					OutError =
						TEXT("MFRecordScreenshots exceeds the screenshot limit.");
					return false;
				}
				FString TimeText;
				FString Label;
				if (!Entries[Index].Split(TEXT(":"), &TimeText, &Label))
				{
					TimeText = Entries[Index];
					Label = FString::Printf(TEXT("Shot_%02d"), Index + 1);
				}
				double TimeSeconds = 0.0;
				if (!LexTryParseString(TimeSeconds, *TimeText)
					|| !FMath::IsFinite(TimeSeconds)
					|| TimeSeconds < 0.0
					|| TimeSeconds > MaxTimeSeconds
					|| (OutOptions.RecordDurationSeconds > 0.0
						&& TimeSeconds
							> OutOptions.RecordDurationSeconds))
				{
					OutError = FString::Printf(
						TEXT("Invalid MFRecordScreenshots entry: %s"),
						*Entries[Index]);
					return false;
				}
				FMatterFluxRecordedScreenshot& Screenshot =
					OutOptions.ScheduledScreenshots.AddDefaulted_GetRef();
				Screenshot.TimeSeconds = TimeSeconds;
				Screenshot.Label = SanitizeScreenshotLabel(Label);
			}
			SortByTime(OutOptions.ScheduledScreenshots);
		}

		if (OutOptions.bRecord && OutOptions.bReplay)
		{
			OutError =
				TEXT("MFRecord and MFReplay are mutually exclusive.");
			return false;
		}
		if (OutOptions.bReplay && OutOptions.ReplayFile.IsEmpty())
		{
			OutError = TEXT("MFReplay requires a recording file path.");
			return false;
		}
		return true;
	}

	bool SaveToJson(
		const FMatterFluxSessionRecording& Recording,
		FString& OutJson,
		FString& OutError)
	{
		OutJson.Reset();
		OutError.Reset();
		if (Recording.Version <= 0
			|| Recording.Version > FMatterFluxSessionRecording::LatestVersion)
		{
			OutError = TEXT("Unsupported recording version.");
			return false;
		}
		if (Recording.WorldSeed < 0
			|| !FMath::IsFinite(Recording.DurationSeconds)
			|| Recording.DurationSeconds < 0.0
			|| Recording.DurationSeconds > MaxTimeSeconds
			|| !FMath::IsFinite(Recording.StateIntervalSeconds)
			|| Recording.StateIntervalSeconds <= 0.0
			|| Recording.StateIntervalSeconds > MaxTimeSeconds
			|| !IsBoundedText(
				Recording.CreatedUtc,
				MaxMetadataCharacters)
			|| !IsBoundedText(
				Recording.EngineVersion,
				MaxMetadataCharacters)
			|| !IsBoundedText(
				Recording.MapName,
				MaxMetadataCharacters))
		{
			OutError = TEXT("Recording metadata is invalid.");
			return false;
		}
		if (Recording.Players.Num() > MaxPlayers
			|| Recording.Operations.Num() > MaxOperations
			|| Recording.States.Num() > MaxStates
			|| Recording.Screenshots.Num() > MaxScreenshots)
		{
			OutError = TEXT("Recording exceeds serialization limits.");
			return false;
		}

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), Schema);
		Root->SetNumberField(TEXT("version"), Recording.Version);
		Root->SetStringField(TEXT("created_utc"), Recording.CreatedUtc);
		Root->SetStringField(TEXT("engine_version"), Recording.EngineVersion);
		Root->SetStringField(TEXT("map"), Recording.MapName);
		Root->SetNumberField(TEXT("world_seed"), Recording.WorldSeed);
		Root->SetNumberField(TEXT("duration_seconds"), Recording.DurationSeconds);
		Root->SetNumberField(
			TEXT("state_interval_seconds"),
			Recording.StateIntervalSeconds);

		TArray<TSharedPtr<FJsonValue>> Players;
		TSet<int32> PlayerIds;
		for (const FMatterFluxRecordedPlayer& Player : Recording.Players)
		{
			if (Player.PlayerId == INDEX_NONE
				|| PlayerIds.Contains(Player.PlayerId)
				|| !IsBoundedText(
					Player.PlayerName,
					MaxPlayerNameCharacters))
			{
				OutError =
					TEXT("Recording contains an invalid or duplicate player.");
				return false;
			}
			PlayerIds.Add(Player.PlayerId);
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("id"), Player.PlayerId);
			Object->SetStringField(TEXT("name"), Player.PlayerName);
			Players.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("players"), Players);

		TArray<TSharedPtr<FJsonValue>> Operations;
		for (const FMatterFluxRecordedOperation& Operation : Recording.Operations)
		{
			if (!IsValidOperationPayload(Operation)
				|| Operation.TimeSeconds > Recording.DurationSeconds
				|| !PlayerIds.Contains(Operation.PlayerId))
			{
				OutError = TEXT("Recording contains an invalid operation payload.");
				return false;
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("time"), Operation.TimeSeconds);
			Object->SetNumberField(TEXT("player"), Operation.PlayerId);
			Object->SetStringField(
				TEXT("operation"),
				OperationToString(Operation.Operation));
			Object->SetArrayField(TEXT("value"), Vector2DToJson(Operation.Value));
			Object->SetNumberField(
				TEXT("integer_value"),
				Operation.IntegerValue);
			Operations.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("operations"), Operations);

		TArray<TSharedPtr<FJsonValue>> States;
		for (const FMatterFluxRecordedPlayerState& State : Recording.States)
		{
			if (!IsValidStatePayload(State)
				|| State.TimeSeconds > Recording.DurationSeconds
				|| !PlayerIds.Contains(State.PlayerId))
			{
				OutError = TEXT("Recording contains an invalid player state.");
				return false;
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("time"), State.TimeSeconds);
			Object->SetNumberField(TEXT("player"), State.PlayerId);
			Object->SetArrayField(
				TEXT("location"),
				VectorToJson(State.Location));
			Object->SetArrayField(
				TEXT("rotation"),
				RotatorToJson(State.Rotation));
			Object->SetArrayField(
				TEXT("velocity"),
				VectorToJson(State.Velocity));
			Object->SetNumberField(TEXT("movement_mode"), State.MovementMode);
			States.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("states"), States);

		TArray<TSharedPtr<FJsonValue>> Screenshots;
		for (const FMatterFluxRecordedScreenshot& Screenshot
			: Recording.Screenshots)
		{
			if (!IsValidRecordedScreenshotPayload(Screenshot))
			{
				OutError = TEXT("Recording contains an invalid screenshot.");
				return false;
			}
			if (Screenshot.TimeSeconds > Recording.DurationSeconds)
			{
				OutError =
					TEXT("Recording contains a screenshot after its duration.");
				return false;
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("time"), Screenshot.TimeSeconds);
			Object->SetStringField(TEXT("label"), Screenshot.Label);
			Screenshots.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("screenshots"), Screenshots);

		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<
				TCHAR,
				TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			OutError = TEXT("Failed to serialize recording JSON.");
			return false;
		}
		if (OutJson.Len() > MaxJsonCharacters)
		{
			OutJson.Reset();
			OutError = TEXT("Recording JSON exceeds the size limit.");
			return false;
		}
		return true;
	}

	bool LoadFromJson(
		const FString& Json,
		FMatterFluxSessionRecording& OutRecording,
		FString& OutError)
	{
		OutError.Reset();
		if (Json.Len() > MaxJsonCharacters)
		{
			OutError = TEXT("Recording JSON exceeds the size limit.");
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("Recording is not valid JSON.");
			return false;
		}
		FString SchemaName;
		if (!Root->TryGetStringField(TEXT("schema"), SchemaName)
			|| SchemaName != Schema)
		{
			OutError = TEXT("Recording schema is missing or invalid.");
			return false;
		}
		double Version = 0.0;
		if (!Root->TryGetNumberField(TEXT("version"), Version)
			|| FMath::FloorToDouble(Version) != Version
			|| Version < 1.0
			|| Version > FMatterFluxSessionRecording::LatestVersion)
		{
			OutError = TEXT("Recording version is unsupported.");
			return false;
		}
		FMatterFluxSessionRecording Candidate;
		Candidate.Version = static_cast<int32>(Version);
		Root->TryGetStringField(TEXT("created_utc"), Candidate.CreatedUtc);
		Root->TryGetStringField(
			TEXT("engine_version"),
			Candidate.EngineVersion);
		Root->TryGetStringField(TEXT("map"), Candidate.MapName);
		if (!IsBoundedText(
				Candidate.CreatedUtc,
				MaxMetadataCharacters)
			|| !IsBoundedText(
				Candidate.EngineVersion,
				MaxMetadataCharacters)
			|| !IsBoundedText(
				Candidate.MapName,
				MaxMetadataCharacters))
		{
			OutError = TEXT("Recording metadata text is invalid.");
			return false;
		}
		double Number = 0.0;
		if (Root->TryGetNumberField(TEXT("world_seed"), Number))
		{
			if (!TryConvertInt32(Number, Candidate.WorldSeed)
				|| Candidate.WorldSeed < 0)
			{
				OutError = TEXT("Recording world seed is invalid.");
				return false;
			}
		}
		else if (Root->HasField(TEXT("world_seed")))
		{
			OutError = TEXT("Recording world seed is invalid.");
			return false;
		}
		if (Root->TryGetNumberField(
			TEXT("duration_seconds"),
			Candidate.DurationSeconds))
		{
			if (!FMath::IsFinite(Candidate.DurationSeconds)
				|| Candidate.DurationSeconds < 0.0
				|| Candidate.DurationSeconds
					> MaxTimeSeconds)
			{
				OutError = TEXT("Recording duration is invalid.");
				return false;
			}
		}
		else if (Root->HasField(TEXT("duration_seconds")))
		{
			OutError = TEXT("Recording duration is invalid.");
			return false;
		}
		if (Root->TryGetNumberField(
			TEXT("state_interval_seconds"),
			Candidate.StateIntervalSeconds))
		{
			if (!FMath::IsFinite(Candidate.StateIntervalSeconds)
				|| Candidate.StateIntervalSeconds <= 0.0
				|| Candidate.StateIntervalSeconds
					> MaxTimeSeconds)
			{
				OutError = TEXT("Recording state interval is invalid.");
				return false;
			}
		}
		else if (Root->HasField(TEXT("state_interval_seconds")))
		{
			OutError = TEXT("Recording state interval is invalid.");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Root->TryGetArrayField(TEXT("players"), Values) && Values)
		{
			if (Values->Num() > MaxPlayers)
			{
				OutError = TEXT("Recording contains too many players.");
				return false;
			}
			TSet<int32> PlayerIds;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				if (!Object.IsValid()
					|| !Object->TryGetNumberField(TEXT("id"), Number))
				{
					OutError = TEXT("A player entry is invalid.");
					return false;
				}
				FMatterFluxRecordedPlayer& Player =
					Candidate.Players.AddDefaulted_GetRef();
				if (!TryConvertInt32(Number, Player.PlayerId))
				{
					OutError = TEXT("A player entry is invalid.");
					return false;
				}
				Object->TryGetStringField(TEXT("name"), Player.PlayerName);
				if (Player.PlayerId == INDEX_NONE
					|| PlayerIds.Contains(Player.PlayerId)
					|| !IsBoundedText(
						Player.PlayerName,
						MaxPlayerNameCharacters))
				{
					OutError =
						TEXT("A player entry is invalid or duplicated.");
					return false;
				}
				PlayerIds.Add(Player.PlayerId);
			}
		}
		else if (Root->HasField(TEXT("players")))
		{
			OutError = TEXT("Recording players field is invalid.");
			return false;
		}

		if (Root->TryGetArrayField(TEXT("operations"), Values) && Values)
		{
			if (Values->Num() > MaxOperations)
			{
				OutError = TEXT("Recording contains too many operations.");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				FString OperationText;
				FMatterFluxRecordedOperation Operation;
				double Player = 0.0;
				if (!Object.IsValid()
					|| !Object->TryGetNumberField(
						TEXT("time"),
						Operation.TimeSeconds)
					|| !Object->TryGetNumberField(TEXT("player"), Player)
					|| !Object->TryGetStringField(
						TEXT("operation"),
						OperationText)
					|| !TryParseOperation(
						OperationText,
						Operation.Operation)
					|| !JsonToVector2D(
						Object,
						TEXT("value"),
						Operation.Value))
				{
					OutError = TEXT("An operation entry is invalid.");
					return false;
				}
				if (!FMath::IsFinite(Operation.TimeSeconds)
					|| Operation.TimeSeconds < 0.0
					|| !TryConvertInt32(Player, Operation.PlayerId))
				{
					OutError = TEXT("An operation entry is invalid.");
					return false;
				}
				if (Object->TryGetNumberField(TEXT("integer_value"), Number))
				{
					if (!TryConvertInt32(Number, Operation.IntegerValue))
					{
						OutError = TEXT("An operation entry is invalid.");
						return false;
					}
				}
				else if (Object->HasField(TEXT("integer_value")))
				{
					OutError = TEXT("An operation entry is invalid.");
					return false;
				}
				if (!IsValidOperationPayload(Operation))
				{
					OutError = TEXT("An operation entry is invalid.");
					return false;
				}
				if (Operation.TimeSeconds > Candidate.DurationSeconds)
				{
					OutError =
						TEXT("An operation occurs after the recording duration.");
					return false;
				}
				if (!Candidate.Players.ContainsByPredicate(
					[&Operation](const FMatterFluxRecordedPlayer& PlayerEntry)
					{
						return PlayerEntry.PlayerId == Operation.PlayerId;
					}))
				{
					OutError =
						TEXT("An operation references an unknown player.");
					return false;
				}
				Candidate.Operations.Add(Operation);
			}
		}
		else if (Root->HasField(TEXT("operations")))
		{
			OutError = TEXT("Recording operations field is invalid.");
			return false;
		}

		if (Root->TryGetArrayField(TEXT("states"), Values) && Values)
		{
			if (Values->Num() > MaxStates)
			{
				OutError = TEXT("Recording contains too many states.");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				FMatterFluxRecordedPlayerState State;
				double Player = 0.0;
				double MovementMode = 0.0;
				if (!Object.IsValid()
					|| !Object->TryGetNumberField(
						TEXT("time"),
						State.TimeSeconds)
					|| !Object->TryGetNumberField(TEXT("player"), Player)
					|| !JsonToVector(
						Object,
						TEXT("location"),
						State.Location)
					|| !JsonToRotator(
						Object,
						TEXT("rotation"),
						State.Rotation)
					|| !JsonToVector(
						Object,
						TEXT("velocity"),
						State.Velocity)
					|| !Object->TryGetNumberField(
						TEXT("movement_mode"),
						MovementMode))
				{
					OutError = TEXT("A state entry is invalid.");
					return false;
				}
				if (!TryConvertInt32(Player, State.PlayerId)
					|| !TryConvertUint8(
						MovementMode,
						State.MovementMode)
					|| !IsValidStatePayload(State)
					|| State.TimeSeconds > Candidate.DurationSeconds
					|| !Candidate.Players.ContainsByPredicate(
						[&State](
							const FMatterFluxRecordedPlayer& PlayerEntry)
						{
							return PlayerEntry.PlayerId == State.PlayerId;
						}))
				{
					OutError = TEXT("A state entry is invalid.");
					return false;
				}
				Candidate.States.Add(State);
			}
		}
		else if (Root->HasField(TEXT("states")))
		{
			OutError = TEXT("Recording states field is invalid.");
			return false;
		}

		if (Root->TryGetArrayField(TEXT("screenshots"), Values) && Values)
		{
			if (Values->Num() > MaxScreenshots)
			{
				OutError = TEXT("Recording contains too many screenshots.");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				FMatterFluxRecordedScreenshot Screenshot;
				if (!Object.IsValid()
					|| !Object->TryGetNumberField(
						TEXT("time"),
						Screenshot.TimeSeconds)
					|| !Object->TryGetStringField(
						TEXT("label"),
						Screenshot.Label))
				{
					OutError = TEXT("A screenshot entry is invalid.");
					return false;
				}
				if (!IsValidRecordedScreenshotPayload(Screenshot))
				{
					OutError = TEXT("A screenshot entry is invalid.");
					return false;
				}
				if (Screenshot.TimeSeconds > Candidate.DurationSeconds)
				{
					OutError =
						TEXT("A screenshot occurs after the recording duration.");
					return false;
				}
				Screenshot.Label = SanitizeScreenshotLabel(Screenshot.Label);
				Candidate.Screenshots.Add(Screenshot);
			}
		}
		else if (Root->HasField(TEXT("screenshots")))
		{
			OutError = TEXT("Recording screenshots field is invalid.");
			return false;
		}

		SortByTime(Candidate.Operations);
		SortByTime(Candidate.States);
		SortByTime(Candidate.Screenshots);
		OutRecording = MoveTemp(Candidate);
		return true;
	}
}
