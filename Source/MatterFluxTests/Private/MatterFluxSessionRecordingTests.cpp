#include "Testing/MatterFluxSessionRecordingTypes.h"
#include "Testing/MatterFluxReplayRuntime.h"

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRecordingLaunchOptionsTest,
	"MatterFlux.Recording.LaunchOptions",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxRecordingLaunchOptionsTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxRecordingLaunchOptions Options;
	FString Error;
	const bool bParsed = MatterFlux::Recording::ParseLaunchOptions(
		TEXT("-MFRecord -MFRecordDir=\"D:/Matter Flux/Runs\" ")
		TEXT("-MFRecordName=ForestCut -MFSeed=1337 ")
		TEXT("-MFRecordDuration=5 -MFRecordQuit ")
		TEXT("-MFRecordStateHz=20 ")
		TEXT("-MFRecordScreenshots=0.5:Spawn,1.25:AfterCut"),
		Options,
		Error);
	if (!TestTrue(TEXT("Valid recording options parse"), bParsed))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Recording is enabled"), Options.bRecord);
	TestFalse(TEXT("Replay is disabled"), Options.bReplay);
	TestEqual(TEXT("Requested seed"), Options.RequestedSeed, 1337);
	TestEqual(
		TEXT("Bounded recording duration"),
		Options.RecordDurationSeconds,
		5.0);
	TestTrue(
		TEXT("Bounded recording requests a clean exit"),
		Options.bQuitAfterRecord);
	TestEqual(
		TEXT("State interval"),
		Options.StateIntervalSeconds,
		0.05,
		0.000001);
	TestEqual(
		TEXT("Two screenshot markers parse"),
		Options.ScheduledScreenshots.Num(),
		2);
	TestEqual(
		TEXT("First screenshot label"),
		Options.ScheduledScreenshots[0].Label,
		FString(TEXT("Spawn")));
	TestEqual(
		TEXT("Quoted output directory survives parsing"),
		Options.RecordDirectory,
		FString(TEXT("D:/Matter Flux/Runs")));

	const bool bConflictParsed =
		MatterFlux::Recording::ParseLaunchOptions(
			TEXT("-MFRecord -MFReplay=Run.mfrecord.json"),
			Options,
			Error);
	TestFalse(
		TEXT("Recording and replay cannot be enabled together"),
		bConflictParsed);
	TestTrue(
		TEXT("Conflict returns a useful error"),
		Error.Contains(TEXT("mutually exclusive")));
	TestFalse(
		TEXT("Unbounded recording durations are rejected"),
		MatterFlux::Recording::ParseLaunchOptions(
			TEXT("-MFRecord -MFRecordDuration=1e100"),
			Options,
			Error));
	TestFalse(
		TEXT("Screenshots cannot outlive a bounded recording"),
		MatterFlux::Recording::ParseLaunchOptions(
			TEXT("-MFRecord -MFRecordDuration=5 ")
			TEXT("-MFRecordScreenshots=6:TooLate"),
			Options,
			Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRecordingJsonRoundTripTest,
	"MatterFlux.Recording.JsonRoundTrip",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxRecordingJsonRoundTripTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxSessionRecording Source;
	Source.CreatedUtc = TEXT("2026-07-27T01:02:03Z");
	Source.EngineVersion = TEXT("5.8");
	Source.MapName = TEXT("Default");
	Source.WorldSeed = 1337;
	Source.DurationSeconds = 3.5;
	Source.StateIntervalSeconds = 0.1;

	FMatterFluxRecordedPlayer& Host =
		Source.Players.AddDefaulted_GetRef();
	Host.PlayerId = 7;
	Host.PlayerName = TEXT("Host");
	FMatterFluxRecordedPlayer& Client =
		Source.Players.AddDefaulted_GetRef();
	Client.PlayerId = 11;
	Client.PlayerName = TEXT("Client");

	FMatterFluxRecordedOperation& Move =
		Source.Operations.AddDefaulted_GetRef();
	Move.TimeSeconds = 0.25;
	Move.PlayerId = 7;
	Move.Operation = EMatterFluxRecordedOperation::Move;
	Move.Value = FVector2D(1.0, -0.5);
	FMatterFluxRecordedOperation& Regenerate =
		Source.Operations.AddDefaulted_GetRef();
	Regenerate.TimeSeconds = 2.0;
	Regenerate.PlayerId = 11;
	Regenerate.Operation =
		EMatterFluxRecordedOperation::Regenerate;
	Regenerate.IntegerValue = 24681357;
	FMatterFluxRecordedOperation& CastWand =
		Source.Operations.AddDefaulted_GetRef();
	CastWand.TimeSeconds = 2.5;
	CastWand.PlayerId = 7;
	CastWand.Operation = EMatterFluxRecordedOperation::CastWand;
	CastWand.IntegerValue = 3;

	FMatterFluxRecordedPlayerState& State =
		Source.States.AddDefaulted_GetRef();
	State.TimeSeconds = 0.3;
	State.PlayerId = 7;
	State.Location = FVector(10.0, 20.0, 30.0);
	State.Rotation = FRotator(1.0, 2.0, 3.0);
	State.Velocity = FVector(4.0, 5.0, 6.0);
	State.MovementMode = 1;

	FMatterFluxRecordedScreenshot& Screenshot =
		Source.Screenshots.AddDefaulted_GetRef();
	Screenshot.TimeSeconds = 1.5;
	Screenshot.Label = TEXT("Tree_Cut");

	FString Json;
	FString Error;
	if (!TestTrue(
		TEXT("Recording serializes"),
		MatterFlux::Recording::SaveToJson(Source, Json, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("JSON identifies its schema"),
		Json.Contains(TEXT("MatterFluxSessionRecording")));

	FMatterFluxSessionRecording Loaded;
	if (!TestTrue(
		TEXT("Recording deserializes"),
		MatterFlux::Recording::LoadFromJson(Json, Loaded, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Seed round-trips"), Loaded.WorldSeed, 1337);
	TestEqual(TEXT("All players round-trip"), Loaded.Players.Num(), 2);
	TestEqual(
		TEXT("All operations round-trip"),
		Loaded.Operations.Num(),
		3);
	TestEqual(
		TEXT("Regeneration seed round-trips without float loss"),
		Loaded.Operations[1].IntegerValue,
		24681357);
	TestEqual(
		TEXT("Wand equipment slot round-trips"),
		Loaded.Operations[2].IntegerValue,
		3);
	TestEqual(
		TEXT("State location round-trips"),
		Loaded.States[0].Location,
		State.Location);
	TestEqual(
		TEXT("Screenshot label round-trips"),
		Loaded.Screenshots[0].Label,
		Screenshot.Label);

	FMatterFluxSessionRecording Invalid;
	TestFalse(
		TEXT("Unknown schemas are rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT("{\"schema\":\"Other\",\"version\":1}"),
			Invalid,
			Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRecordingCommandsTest,
	"MatterFlux.Recording.CommandsRegistered",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxRecordingCommandsTest::RunTest(
	const FString& Parameters)
{
	TestNotNull(
		TEXT("Timestamped screenshot command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Record.Screenshot")));
	TestNotNull(
		TEXT("Manual atomic flush command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Record.Flush")));
	TestNotNull(
		TEXT("Semantic operation injection command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Record.Inject")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRecordingRejectsInvalidNumbersTest,
	"MatterFlux.Recording.RejectsInvalidNumbersTransactionally",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxRecordingRejectsInvalidNumbersTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxSessionRecording Loaded;
	FString Error;
	TestFalse(
		TEXT("Fractional integer fields are rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"world_seed\":1.5}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Out-of-range integer fields are rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"players\":[{\"id\":1e100}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Unreasonably long replay timelines are rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"duration_seconds\":1e100}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Malformed trailing data is rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"players\":[{\"id\":7}],"
				"\"operations\":[{\"time\":0,\"player\":7,"
				"\"operation\":\"Unknown\",\"value\":[0,0]}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Replay movement outside the RPC payload bounds is rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"operations\":[{\"time\":0,"
				"\"player\":7,\"operation\":\"Move\","
				"\"value\":[101,0]}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Negative regeneration seeds are rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"operations\":[{\"time\":0,"
				"\"player\":7,\"operation\":\"Regenerate\","
				"\"value\":[0,0],\"integer_value\":-1}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Out-of-range wand equipment slots are rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"operations\":[{\"time\":0,"
				"\"player\":7,\"operation\":\"CastWand\","
				"\"value\":[0,0],\"integer_value\":4}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Duplicate player identities are rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"players\":["
				"{\"id\":7,\"name\":\"Host\"},"
				"{\"id\":7,\"name\":\"Duplicate\"}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("The unresolved player identity sentinel is rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"players\":[{\"id\":-1}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Operations cannot reference an unknown player"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"operations\":[{\"time\":0,"
				"\"player\":7,\"operation\":\"Move\","
				"\"value\":[1,0]}]}"),
			Loaded,
			Error));

	const FString OversizedLabel =
		FString::ChrN(129, TEXT('x'));
	TestFalse(
		TEXT("Oversized screenshot labels are rejected"),
		MatterFlux::Recording::LoadFromJson(
			FString::Printf(
				TEXT(
					"{\"schema\":\"MatterFluxSessionRecording\","
					"\"version\":1,\"screenshots\":["
					"{\"time\":0,\"label\":\"%s\"}]}"),
				*OversizedLabel),
			Loaded,
			Error));

	TestFalse(
		TEXT("Operations cannot extend the declared replay duration"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"duration_seconds\":1,"
				"\"players\":[{\"id\":7}],"
				"\"operations\":[{\"time\":2,\"player\":7,"
				"\"operation\":\"Move\",\"value\":[1,0]}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("States cannot extend the declared replay duration"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"duration_seconds\":1,"
				"\"players\":[{\"id\":7}],"
				"\"states\":[{\"time\":2,\"player\":7,"
				"\"location\":[0,0,0],\"rotation\":[0,0,0],"
				"\"velocity\":[0,0,0],\"movement_mode\":0}]}"),
			Loaded,
			Error));

	TestFalse(
		TEXT("Screenshots cannot extend the declared replay duration"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"duration_seconds\":1,"
				"\"screenshots\":[{\"time\":2,\"label\":\"Late\"}]}"),
			Loaded,
			Error));

	FMatterFluxSessionRecording InvalidForSave;
	FMatterFluxRecordedOperation& InvalidOperation =
		InvalidForSave.Operations.AddDefaulted_GetRef();
	InvalidOperation.Value = FVector2D(101.0, 0.0);
	FString Json;
	TestFalse(
		TEXT("Invalid runtime operation payloads are not serialized"),
		MatterFlux::Recording::SaveToJson(
			InvalidForSave,
			Json,
			Error));
	TestTrue(
		TEXT("Failed serialization does not expose partial JSON"),
		Json.IsEmpty());

	FMatterFluxSessionRecording InvalidStateForSave;
	FMatterFluxRecordedPlayerState& InvalidState =
		InvalidStateForSave.States.AddDefaulted_GetRef();
	InvalidState.Location.X =
		std::numeric_limits<double>::quiet_NaN();
	TestFalse(
		TEXT("Non-finite runtime player states are not serialized"),
		MatterFlux::Recording::SaveToJson(
			InvalidStateForSave,
			Json,
			Error));
	TestTrue(
		TEXT("Invalid state serialization exposes no partial JSON"),
		Json.IsEmpty());

	FMatterFluxSessionRecording InvalidMetadataForSave;
	InvalidMetadataForSave.DurationSeconds = -1.0;
	TestFalse(
		TEXT("Invalid runtime recording metadata is not serialized"),
		MatterFlux::Recording::SaveToJson(
			InvalidMetadataForSave,
			Json,
			Error));
	TestTrue(
		TEXT("Invalid metadata serialization exposes no partial JSON"),
		Json.IsEmpty());

	FMatterFluxSessionRecording DuplicatePlayersForSave;
	FMatterFluxRecordedPlayer& FirstDuplicatePlayer =
		DuplicatePlayersForSave.Players.AddDefaulted_GetRef();
	FirstDuplicatePlayer.PlayerId = 7;
	FirstDuplicatePlayer.PlayerName = TEXT("Host");
	FMatterFluxRecordedPlayer& SecondDuplicatePlayer =
		DuplicatePlayersForSave.Players.AddDefaulted_GetRef();
	SecondDuplicatePlayer.PlayerId = 7;
	SecondDuplicatePlayer.PlayerName = TEXT("Duplicate");
	TestFalse(
		TEXT("Duplicate runtime player identities are not serialized"),
		MatterFlux::Recording::SaveToJson(
			DuplicatePlayersForSave,
			Json,
			Error));
	TestTrue(
		TEXT("Duplicate player serialization exposes no partial JSON"),
		Json.IsEmpty());

	TestEqual(
		TEXT("Failed loads do not expose partially parsed players"),
		Loaded.Players.Num(),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRecordingFailedDecodeIsAtomicTest,
	"MatterFlux.Recording.FailedDecodePreservesDestination",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxRecordingFailedDecodeIsAtomicTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxSessionRecording Existing;
	Existing.CreatedUtc = TEXT("ExistingTimestamp");
	Existing.MapName = TEXT("ExistingMap");
	Existing.WorldSeed = 2468;
	Existing.DurationSeconds = 12.5;
	FMatterFluxRecordedPlayer& ExistingPlayer =
		Existing.Players.AddDefaulted_GetRef();
	ExistingPlayer.PlayerId = 99;
	ExistingPlayer.PlayerName = TEXT("ExistingPlayer");

	FString Error;
	TestFalse(
		TEXT("A recording with a malformed operation is rejected"),
		MatterFlux::Recording::LoadFromJson(
			TEXT(
				"{\"schema\":\"MatterFluxSessionRecording\","
				"\"version\":1,\"duration_seconds\":10,"
				"\"players\":[{\"id\":7,\"name\":\"ParsedFirst\"}],"
				"\"operations\":[{\"time\":1,\"player\":7,"
				"\"operation\":\"Unknown\",\"value\":[0,0]}]}"),
			Existing,
			Error));
	TestTrue(
		TEXT("Decode failure explains the invalid operation"),
		!Error.IsEmpty());
	TestEqual(
		TEXT("Failed decode preserves timestamp"),
		Existing.CreatedUtc,
		FString(TEXT("ExistingTimestamp")));
	TestEqual(
		TEXT("Failed decode preserves map"),
		Existing.MapName,
		FString(TEXT("ExistingMap")));
	TestEqual(TEXT("Failed decode preserves seed"), Existing.WorldSeed, 2468);
	TestEqual(
		TEXT("Failed decode preserves duration"),
		Existing.DurationSeconds,
		12.5);
	TestEqual(
		TEXT("Failed decode preserves player count"),
		Existing.Players.Num(),
		1);
	TestTrue(
		TEXT("Failed decode preserves player identity"),
		Existing.Players.Num() == 1
			&& Existing.Players[0].PlayerId == 99);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReplayRuntimeStableFrameTest,
	"MatterFlux.Recording.ReplayRuntimeProducesStableBatchedFrame",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxReplayRuntimeStableFrameTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxSessionRecording Recording;
	Recording.DurationSeconds = 2.0;
	for (const int32 PlayerId : { 3, 7 })
	{
		FMatterFluxRecordedPlayer& Player =
			Recording.Players.AddDefaulted_GetRef();
		Player.PlayerId = PlayerId;
	}
	auto AddOperation =
		[&Recording](
			const int32 PlayerId,
			const EMatterFluxRecordedOperation Operation,
			const FVector2D& Value)
		{
			FMatterFluxRecordedOperation& Entry =
				Recording.Operations.AddDefaulted_GetRef();
			Entry.TimeSeconds = 0.25;
			Entry.PlayerId = PlayerId;
			Entry.Operation = Operation;
			Entry.Value = Value;
		};
	AddOperation(7, EMatterFluxRecordedOperation::Move, FVector2D(1.0, 0.0));
	AddOperation(3, EMatterFluxRecordedOperation::JumpStarted, FVector2D::ZeroVector);
	AddOperation(3, EMatterFluxRecordedOperation::Move, FVector2D(-1.0, 0.0));
	AddOperation(7, EMatterFluxRecordedOperation::Cut, FVector2D::ZeroVector);
	auto AddState =
		[&Recording](
			const double TimeSeconds,
			const int32 PlayerId,
			const double LocationX)
		{
			FMatterFluxRecordedPlayerState& State =
				Recording.States.AddDefaulted_GetRef();
			State.TimeSeconds = TimeSeconds;
			State.PlayerId = PlayerId;
			State.Location = FVector(LocationX, 0.0, 0.0);
		};
	AddState(0.10, 7, 10.0);
	AddState(0.10, 3, 30.0);
	AddState(0.20, 7, 20.0);
	AddState(0.40, 7, 40.0);
	AddState(0.50, 3, 50.0);
	for (const TCHAR* Label : { TEXT("First"), TEXT("Second") })
	{
		FMatterFluxRecordedScreenshot& Screenshot =
			Recording.Screenshots.AddDefaulted_GetRef();
		Screenshot.TimeSeconds = 0.20;
		Screenshot.Label = Label;
	}

	MatterFlux::Recording::FReplayRuntime Runtime;
	MatterFlux::Recording::FReplayRuntimeSettings Settings;
	Settings.bVerifyStates = true;
	FString Error;
	if (!TestTrue(
		TEXT("Replay timeline initializes"),
		Runtime.Initialize(Recording, Settings, Error)))
	{
		AddError(Error);
		return false;
	}
	MatterFlux::Recording::FReplayFrame Frame;
	if (!TestTrue(
		TEXT("A replay frame advances"),
		Runtime.Advance(0.25, Frame, Error)))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("Only one-shot operations are emitted"),
		Frame.Operations.Num(), 2);
	TestTrue(
		TEXT("One-shot operations preserve file order"),
		Frame.Operations.Num() == 2
			&& Frame.Operations[0].Operation
				== EMatterFluxRecordedOperation::JumpStarted
			&& Frame.Operations[1].Operation
				== EMatterFluxRecordedOperation::Cut);
	TestTrue(
		TEXT("Active movement is sorted by player id"),
		Frame.Movements.Num() == 2
			&& Frame.Movements[0].PlayerId == 3
			&& Frame.Movements[0].Value == FVector2D(-1.0, 0.0)
			&& Frame.Movements[1].PlayerId == 7
			&& Frame.Movements[1].Value == FVector2D(1.0, 0.0));
	TestTrue(
		TEXT("Expected states are sorted and interpolated"),
		Frame.ExpectedStates.Num() == 2
			&& Frame.ExpectedStates[0].PlayerId == 3
			&& FMath::IsNearlyEqual(
				Frame.ExpectedStates[0].Location.X,
				37.5)
			&& Frame.ExpectedStates[1].PlayerId == 7
			&& FMath::IsNearlyEqual(
				Frame.ExpectedStates[1].Location.X,
				25.0));
	TestTrue(
		TEXT("Same-time screenshots preserve file order"),
		Frame.Screenshots.Num() == 2
			&& Frame.Screenshots[0].Label == TEXT("First")
			&& Frame.Screenshots[1].Label == TEXT("Second"));
	TestFalse(TEXT("The early frame is not complete"), Frame.bComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReplayRuntimeCompletionEdgeTest,
	"MatterFlux.Recording.ReplayRuntimeReportsCompletionOnce",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxReplayRuntimeCompletionEdgeTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxSessionRecording Recording;
	Recording.DurationSeconds = 0.5;
	MatterFlux::Recording::FReplayRuntime Runtime;
	MatterFlux::Recording::FReplayRuntimeSettings Settings;
	Settings.bVerifyStates = false;
	Settings.CompletionGraceSeconds = 0.1;
	FString Error;
	if (!TestTrue(
		TEXT("Empty replay timeline initializes"),
		Runtime.Initialize(Recording, Settings, Error)))
	{
		AddError(Error);
		return false;
	}

	MatterFlux::Recording::FReplayFrame FirstFrame;
	MatterFlux::Recording::FReplayFrame RepeatedFrame;
	TestTrue(
		TEXT("Replay reaches its completion edge"),
		Runtime.Advance(0.6, FirstFrame, Error)
			&& FirstFrame.bComplete);
	TestTrue(
		TEXT("Advancing at the same completed time remains valid"),
		Runtime.Advance(0.6, RepeatedFrame, Error));
	TestFalse(
		TEXT("Completion is emitted only once"),
		RepeatedFrame.bComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReplayRuntimeTransactionalAdvanceTest,
	"MatterFlux.Recording.ReplayRuntimeRepeatAndRollbackAreTransactional",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxReplayRuntimeTransactionalAdvanceTest::RunTest(
	const FString& Parameters)
{
	FMatterFluxSessionRecording Recording;
	Recording.DurationSeconds = 1.0;
	auto AddOperation =
		[&Recording](
			const double TimeSeconds,
			const EMatterFluxRecordedOperation Operation,
			const FVector2D& Value = FVector2D::ZeroVector)
		{
			FMatterFluxRecordedOperation& Entry =
				Recording.Operations.AddDefaulted_GetRef();
			Entry.TimeSeconds = TimeSeconds;
			Entry.PlayerId = 4;
			Entry.Operation = Operation;
			Entry.Value = Value;
		};
	AddOperation(
		0.10,
		EMatterFluxRecordedOperation::Move,
		FVector2D(0.25, -0.75));
	AddOperation(0.20, EMatterFluxRecordedOperation::JumpStarted);
	AddOperation(0.25, EMatterFluxRecordedOperation::Cut);
	FMatterFluxRecordedScreenshot& Screenshot =
		Recording.Screenshots.AddDefaulted_GetRef();
	Screenshot.TimeSeconds = 0.25;
	Screenshot.Label = TEXT("AfterRollback");

	MatterFlux::Recording::FReplayRuntime Runtime;
	MatterFlux::Recording::FReplayRuntimeSettings Settings;
	Settings.bVerifyStates = false;
	FString Error;
	if (!TestTrue(
		TEXT("Transactional replay initializes"),
		Runtime.Initialize(Recording, Settings, Error)))
	{
		AddError(Error);
		return false;
	}

	MatterFlux::Recording::FReplayFrame FirstFrame;
	TestTrue(
		TEXT("Initial replay frame advances"),
		Runtime.Advance(0.20, FirstFrame, Error));
	TestTrue(
		TEXT("Initial frame emits the due event and movement"),
		FirstFrame.Operations.Num() == 1
			&& FirstFrame.Operations[0].Operation
				== EMatterFluxRecordedOperation::JumpStarted
			&& FirstFrame.Movements.Num() == 1
			&& FirstFrame.Movements[0].PlayerId == 4
			&& FirstFrame.Movements[0].Value
				== FVector2D(0.25, -0.75));

	MatterFlux::Recording::FReplayFrame RepeatedFrame;
	TestTrue(
		TEXT("Repeated replay time remains valid"),
		Runtime.Advance(0.20, RepeatedFrame, Error));
	TestTrue(
		TEXT("Repeated time keeps movement without duplicating events"),
		RepeatedFrame.Operations.IsEmpty()
			&& RepeatedFrame.Screenshots.IsEmpty()
			&& RepeatedFrame.Movements.Num() == 1
			&& RepeatedFrame.Movements[0].Value
				== FVector2D(0.25, -0.75));

	MatterFlux::Recording::FReplayFrame RejectedFrame;
	RejectedFrame.Operations.AddDefaulted();
	RejectedFrame.bComplete = true;
	TestFalse(
		TEXT("Replay time moving backwards is rejected"),
		Runtime.Advance(0.15, RejectedFrame, Error));
	TestTrue(
		TEXT("Rejected advance clears output and reports an error"),
		!Error.IsEmpty()
			&& RejectedFrame.Operations.IsEmpty()
			&& RejectedFrame.Movements.IsEmpty()
			&& !RejectedFrame.bComplete);

	MatterFlux::Recording::FReplayFrame RecoveredFrame;
	TestTrue(
		TEXT("A valid time can continue after rejection"),
		Runtime.Advance(0.25, RecoveredFrame, Error));
	TestTrue(
		TEXT("Rejected advance did not consume future timeline entries"),
		RecoveredFrame.Operations.Num() == 1
			&& RecoveredFrame.Operations[0].Operation
				== EMatterFluxRecordedOperation::Cut
			&& RecoveredFrame.Screenshots.Num() == 1
			&& RecoveredFrame.Screenshots[0].Label
				== TEXT("AfterRollback")
			&& RecoveredFrame.Movements.Num() == 1);
	return true;
}
