#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "FragmentTestActors.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayerOperation.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Testing/MatterFluxSessionRecorderSubsystem.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

namespace
{
	void FindListenPIEWorlds(
		UWorld*& OutHost,
		TArray<UWorld*>& OutClients)
	{
		OutHost = nullptr;
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
			if (World->GetNetMode() == NM_ListenServer)
			{
				OutHost = World;
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

	APlayerController* FindLocalController(UWorld* World)
	{
		for (TActorIterator<APlayerController> It(World); It; ++It)
		{
			if (It->IsLocalController())
			{
				return *It;
			}
		}
		return nullptr;
	}

	AFragment2DSourceActor* FindListenSource(
		UWorld* World,
		const FGuid& SourceId)
	{
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			if (It->SourceId == SourceId)
			{
				return *It;
			}
		}
		return nullptr;
	}

	void FindTestFragments(
		UWorld* World,
		TArray<AMatterFluxNetworkTestFragmentActor*>& OutFragments)
	{
		OutFragments.Reset();
		for (TActorIterator<AMatterFluxNetworkTestFragmentActor> It(World);
			It;
			++It)
		{
			if (It->SpawnPayload.FragmentId.IsValid())
			{
				OutFragments.Add(*It);
			}
		}
		OutFragments.Sort(
			[](const AMatterFluxNetworkTestFragmentActor& A,
				const AMatterFluxNetworkTestFragmentActor& B)
			{
				return A.SpawnPayload.FragmentId.ToString(EGuidFormats::Digits)
					< B.SpawnPayload.FragmentId.ToString(EGuidFormats::Digits);
			});
	}

	bool ContoursEqual(
		const TArray<FFragmentContour>& A,
		const TArray<FFragmentContour>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index].Vertices != B[Index].Vertices)
			{
				return false;
			}
		}
		return true;
	}

	bool PayloadEquals(
		const FFragmentSpawnPayload& A,
		const FFragmentSpawnPayload& B)
	{
		return A.FragmentId == B.FragmentId
			&& A.Revision == B.Revision
			&& A.Vertices2D == B.Vertices2D
			&& A.TriangleIndices == B.TriangleIndices
			&& ContoursEqual(A.OuterContours, B.OuterContours)
			&& ContoursEqual(A.HoleContours, B.HoleContours)
			&& ContoursEqual(A.CollisionContours, B.CollisionContours)
			&& A.bEnableCollision == B.bEnableCollision
			&& A.Thickness == B.Thickness;
	}

	AMatterFluxNetworkTestFragmentActor* FindFragmentById(
		const TArray<AMatterFluxNetworkTestFragmentActor*>& Fragments,
		const FGuid& FragmentId)
	{
		for (AMatterFluxNetworkTestFragmentActor* Fragment : Fragments)
		{
			if (Fragment
				&& Fragment->SpawnPayload.FragmentId == FragmentId)
			{
				return Fragment;
			}
		}
		return nullptr;
	}

	AFragment2DActor* FindAnyFragmentById(
		UWorld* World,
		const FGuid& FragmentId)
	{
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			if (It->SpawnPayload.FragmentId == FragmentId)
			{
				return *It;
			}
		}
		return nullptr;
	}

	FFragmentSpawnPayload MakeFallenTreePayload(
		const FVector& WorldLocation)
	{
		FFragmentSpawnPayload Payload;
		Payload.FragmentId = FGuid::NewDeterministicGuid(
			TEXT("MatterFlux.FallenTreeCharacterContact"),
			1);
		Payload.Revision = 1;
		Payload.Vertices2D = {
			FVector2D(-260.0, -30.0),
			FVector2D(260.0, -30.0),
			FVector2D(260.0, 30.0),
			FVector2D(-260.0, 30.0)
		};
		Payload.TriangleIndices = { 0, 1, 2, 0, 2, 3 };
		FFragmentContour& Contour = Payload.OuterContours.AddDefaulted_GetRef();
		Contour.Vertices = Payload.Vertices2D;
		Payload.CollisionContours.Add(Contour);
		Payload.Thickness = 60.0f;
		Payload.Mass = 48.0f;
		Payload.bEnableCollision = true;
		Payload.InitialTransform = FTransform(WorldLocation);
		return Payload;
	}

	class FVerifyFallenTreeCharacterContactCommand final
		: public IAutomationLatentCommand
	{
	public:
		explicit FVerifyFallenTreeCharacterContactCommand(
			FAutomationTestBase* InTest)
			: Test(InTest)
			, PhaseStart(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			UWorld* Host = nullptr;
			TArray<UWorld*> Clients;
			FindListenPIEWorlds(Host, Clients);
			APlayerController* HostController = Host
				? FindLocalController(Host)
				: nullptr;
			AMatterFluxCharacter* Character = HostController
				? Cast<AMatterFluxCharacter>(HostController->GetPawn())
				: nullptr;
			if (!Host || Clients.Num() != 1 || !Character)
			{
				return FailOnTimeout(
					TEXT("Fallen-tree contact test did not create a listen host, client, and host character."));
			}

			if (!TreeId.IsValid())
			{
				const FVector TreeLocation = Character->GetActorLocation()
					+ FVector(380.0f, 0.0f, -55.0f);
				AFragment2DActor* Tree = Host->SpawnActor<AFragment2DActor>(
					TreeLocation,
					FRotator::ZeroRotator);
				const FFragmentSpawnPayload Payload =
					MakeFallenTreePayload(TreeLocation);
				if (!Tree || !Tree->InitializeFromPayload(Payload))
				{
					Test->AddError(
						TEXT("Listen host could not initialize the fallen-tree rigid body."));
					return true;
				}
				Tree->bAlwaysRelevant = true;
				Tree->ForceNetUpdate();
				TreeId = Payload.FragmentId;
				PreviousTreeLocation = Tree->GetActorLocation();
				BeginNextPhase();
				return false;
			}

			AFragment2DActor* HostTree = FindAnyFragmentById(Host, TreeId);
			AFragment2DActor* ClientTree =
				FindAnyFragmentById(Clients[0], TreeId);
			if (!HostTree || !ClientTree)
			{
				return FailOnTimeout(
					TEXT("Fallen tree did not replicate to the listen client."));
			}
			if (!bPushing)
			{
				if (FPlatformTime::Seconds() - PhaseStart < 1.0)
				{
					PreviousTreeLocation = HostTree->GetActorLocation();
					return false;
				}
				bPushing = true;
				BeginNextPhase();
			}

			if (!bFinishedPushing)
			{
				Character->AddMovementInput(FVector::XAxisVector, 1.0f);
				const FVector TreeLocation = HostTree->GetActorLocation();
				MaximumFrameDisplacement = FMath::Max(
					MaximumFrameDisplacement,
					static_cast<float>(FVector::Distance(
						TreeLocation,
						PreviousTreeLocation)));
				PreviousTreeLocation = TreeLocation;
				MaximumLinearSpeed = FMath::Max(
					MaximumLinearSpeed,
					static_cast<float>(HostTree->MeshComponent
						->GetPhysicsLinearVelocity().Size()));
				MaximumAngularSpeed = FMath::Max(
					MaximumAngularSpeed,
					static_cast<float>(HostTree->MeshComponent
						->GetPhysicsAngularVelocityInDegrees().Size()));
				if (FPlatformTime::Seconds() - PhaseStart < 2.0)
				{
					return false;
				}
				bFinishedPushing = true;
				BeginNextPhase();
				return false;
			}

			const float ClientDistance = FVector::Distance(
				HostTree->GetActorLocation(),
				ClientTree->GetActorLocation());
			if (FPlatformTime::Seconds() - PhaseStart < 1.0
				&& ClientDistance >= 100.0f)
			{
				return false;
			}
			Test->TestTrue(
				TEXT("Character contact never teleports the fallen tree between frames"),
				MaximumFrameDisplacement < 100.0f);
			Test->TestTrue(
				TEXT("Sustained character contact actually pushes the fallen tree"),
				MaximumLinearSpeed > 5.0f);
			Test->TestTrue(
				TEXT("Character contact keeps fallen-tree linear speed bounded"),
				MaximumLinearSpeed < 1200.0f);
			Test->TestTrue(
				TEXT("Character contact respects the fallen-tree angular speed cap"),
				MaximumAngularSpeed <= 365.0f);
			Test->TestTrue(
				TEXT("Listen client converges after sustained character/tree contact"),
				ClientDistance < 100.0f);
			return true;
		}

	private:
		void BeginNextPhase()
		{
			PhaseStart = FPlatformTime::Seconds();
		}

		bool FailOnTimeout(const TCHAR* Message)
		{
			if (FPlatformTime::Seconds() - PhaseStart < 30.0)
			{
				return false;
			}
			Test->AddError(Message);
			return true;
		}

		FAutomationTestBase* Test = nullptr;
		double PhaseStart = 0.0;
		FGuid TreeId;
		FVector PreviousTreeLocation = FVector::ZeroVector;
		float MaximumFrameDisplacement = 0.0f;
		float MaximumLinearSpeed = 0.0f;
		float MaximumAngularSpeed = 0.0f;
		bool bPushing = false;
		bool bFinishedPushing = false;
	};

	class FVerifyMatterFluxListenServerPIECommand final
		: public IAutomationLatentCommand
	{
	public:
		explicit FVerifyMatterFluxListenServerPIECommand(
			FAutomationTestBase* InTest,
			FString InOriginalCommandLine,
			const int32 InExpectedPlayerCount)
			: Test(InTest)
			, OriginalCommandLine(MoveTemp(InOriginalCommandLine))
			, ExpectedPlayerCount(InExpectedPlayerCount)
			, PhaseStartTime(FPlatformTime::Seconds())
		{
		}

		virtual ~FVerifyMatterFluxListenServerPIECommand() override
		{
			RestoreCommandLine();
		}

		virtual bool Update() override
		{
			UWorld* Host = nullptr;
			TArray<UWorld*> Clients;
			FindListenPIEWorlds(Host, Clients);
			if (!Host || Clients.Num() != ExpectedPlayerCount - 1)
			{
				return FailOnTimeout(
					TEXT("The listen-host world and expected remote client worlds were not created."));
			}
			UWorld* Client = Clients[0];

			if (!bValidatedPlayers)
			{
				APlayerController* HostController = FindLocalController(Host);
				APlayerController* ClientController = FindLocalController(Client);
				UMatterFluxSessionRecorderSubsystem* HostRecorder =
					Host->GetGameInstance()
						? Host->GetGameInstance()->GetSubsystem<
							UMatterFluxSessionRecorderSubsystem>()
						: nullptr;
				UMatterFluxSessionRecorderSubsystem* ClientRecorder =
					Client->GetGameInstance()
						? Client->GetGameInstance()->GetSubsystem<
							UMatterFluxSessionRecorderSubsystem>()
						: nullptr;
				const AGameStateBase* HostGameState = Host->GetGameState();
				bool bAllClientsReady = true;
				for (UWorld* ClientWorld : Clients)
				{
					const APlayerController* LocalController =
						FindLocalController(ClientWorld);
					const AGameStateBase* GameState =
						ClientWorld ? ClientWorld->GetGameState() : nullptr;
					bAllClientsReady &= LocalController
						&& !LocalController->HasAuthority()
						&& LocalController->GetPawn()
						&& GameState
						&& GameState->PlayerArray.Num() == ExpectedPlayerCount;
				}
				if (!HostController
					|| !ClientController
					|| !HostController->HasAuthority()
					|| ClientController->HasAuthority()
					|| !HostController->GetPawn()
					|| !ClientController->GetPawn()
					|| !HostGameState
					|| HostGameState->PlayerArray.Num() != ExpectedPlayerCount
					|| !bAllClientsReady
					|| !HostRecorder
					|| !ClientRecorder
					|| !HostRecorder->IsRecording()
					|| !ClientRecorder->IsRecording()
					|| !HostRecorder->HasSessionStarted()
					|| !ClientRecorder->HasSessionStarted())
				{
					return FailOnTimeout(
						TEXT("Listen host and remote clients did not all receive playable local pawns, complete player state, and active primary recording sessions."));
				}

				AMatterFluxCharacter* ClientCharacter =
					Cast<AMatterFluxCharacter>(ClientController->GetPawn());
				const APlayerState* ClientPlayerState =
					ClientController->GetPlayerState<APlayerState>();
				if (!ClientCharacter || !ClientPlayerState)
				{
					Test->AddError(
						TEXT("Remote client pawn was not a MatterFlux character with PlayerState."));
					return true;
				}
				InitialHostOperationCount =
					HostRecorder->GetRecording().Operations.Num();
				InitialClientOperationCount =
					ClientRecorder->GetRecording().Operations.Num();
				RecordedClientPlayerId =
					ClientPlayerState->GetPlayerId();
				RestoreCommandLine();
				ClientCharacter->ApplyPlayerOperation(
					EMatterFluxPlayerOperation::CameraZoom,
					FVector2D(1.0, 0.0),
					0);
				bValidatedPlayers = true;
				BeginNextPhase();
				return false;
			}

			if (!bValidatedRecordedRelay)
			{
				const UMatterFluxSessionRecorderSubsystem* HostRecorder =
					Host->GetGameInstance()
						? Host->GetGameInstance()->GetSubsystem<
							UMatterFluxSessionRecorderSubsystem>()
						: nullptr;
				const UMatterFluxSessionRecorderSubsystem* ClientRecorder =
					Client->GetGameInstance()
						? Client->GetGameInstance()->GetSubsystem<
							UMatterFluxSessionRecorderSubsystem>()
						: nullptr;
				if (!HostRecorder || !ClientRecorder)
				{
					return FailOnTimeout(
						TEXT("Recording subsystem disappeared during client relay verification."));
				}

				const TArray<FMatterFluxRecordedOperation>& HostOperations =
					HostRecorder->GetRecording().Operations;
				const TArray<FMatterFluxRecordedOperation>& ClientOperations =
					ClientRecorder->GetRecording().Operations;
				if (HostOperations.Num() < InitialHostOperationCount + 1
					|| ClientOperations.Num() < InitialClientOperationCount + 1)
				{
					return FailOnTimeout(
						TEXT("Remote client semantic operation did not reach both recording adapters."));
				}

				const FMatterFluxRecordedOperation& HostOperation =
					HostOperations.Last();
				const FMatterFluxRecordedOperation& ClientOperation =
					ClientOperations.Last();
				if (HostOperations.Num() != InitialHostOperationCount + 1
					|| ClientOperations.Num() != InitialClientOperationCount + 1
					|| HostOperation.PlayerId != RecordedClientPlayerId
					|| ClientOperation.PlayerId != RecordedClientPlayerId
					|| HostOperation.Operation
						!= EMatterFluxRecordedOperation::CameraZoom
					|| ClientOperation.Operation
						!= EMatterFluxRecordedOperation::CameraZoom
					|| HostOperation.Value != FVector2D(1.0, 0.0)
					|| ClientOperation.Value != FVector2D(1.0, 0.0))
				{
					Test->AddError(FString::Printf(
						TEXT("Client recording relay was duplicated or changed: host %d->%d, client %d->%d, player=%d/%d expected=%d."),
						InitialHostOperationCount,
						HostOperations.Num(),
						InitialClientOperationCount,
						ClientOperations.Num(),
						HostOperation.PlayerId,
						ClientOperation.PlayerId,
						RecordedClientPlayerId));
					return true;
				}
				bValidatedRecordedRelay = true;
				BeginNextPhase();
				return false;
			}

			if (!bSpawnedSource)
			{
				APlayerController* HostController = FindLocalController(Host);
				const FVector Location = HostController->GetPawn()->GetActorLocation()
					+ FVector(300.0, 0.0, 300.0);
				const FTransform Transform(FRotator::ZeroRotator, Location);
				AFragment2DSourceActor* Source =
					Host->SpawnActorDeferred<AFragment2DSourceActor>(
						AFragment2DSourceActor::StaticClass(),
						Transform,
						nullptr,
						nullptr,
						ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
				if (!Source)
				{
					Test->AddError(
						TEXT("Listen host could not spawn the replicated source."));
					return true;
				}

				FFragmentSourceMask Mask;
				Mask.Width = 12;
				Mask.Height = 12;
				Mask.CellSize = 10.0f;
				Mask.MinFragmentAreaPixels = 1;
				Mask.MaxFragmentsPerBreak = 4;
				Mask.SupportMode = EFragmentSupportMode::None;
				Mask.SolidMask.Init(1, Mask.Width * Mask.Height);
				TestSourceId = FGuid::NewDeterministicGuid(
					TEXT("MatterFlux.ListenHostClient.Source"),
					1);
				Source->bAlwaysRelevant = true;
				Source->bDestroySourceOnFirstBreak = false;
				Source->DefaultSupportMode = EFragmentSupportMode::None;
				Source->FragmentActorClass =
					AMatterFluxNetworkTestFragmentActor::StaticClass();
				Source->FragmentMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
				if (!Source->InitializeFromProceduralMask(
					Mask,
					TestSourceId,
					FLinearColor(0.42f, 0.18f, 0.06f),
					TEXT("wood")))
				{
					Source->Destroy();
					Test->AddError(
						TEXT("Listen host could not initialize the deterministic source mask."));
					return true;
				}
				Source->FinishSpawning(Transform);
				bSpawnedSource = true;
				BeginNextPhase();
				return false;
			}

			if (!bAppliedHostDamage)
			{
				AFragment2DSourceActor* HostSource = FindListenSource(Host, TestSourceId);
				if (!HostSource)
				{
					return FailOnTimeout(
						TEXT("The listen host source disappeared before damage."));
				}
				bool bAllClientsReceivedSource = HostSource != nullptr;
				for (UWorld* ClientWorld : Clients)
				{
					const AFragment2DSourceActor* ClientSource =
						FindListenSource(ClientWorld, TestSourceId);
					bAllClientsReceivedSource &= ClientSource
						&& ClientSource->Revision == 0
						&& ClientSource->ProceduralSource.Width == 12
						&& ClientSource->ProceduralSource.Height == 12
						&& ClientSource->ProceduralSource.SolidMask
							== HostSource->ProceduralSource.SolidMask;
				}
				if (!bAllClientsReceivedSource)
				{
					return FailOnTimeout(
						TEXT("Every client did not receive the listen host's source identity and mask."));
				}

				FFragmentDamageEvent Event;
				Event.SourceId = TestSourceId;
				Event.BaseRevision = HostSource->Revision;
				Event.DamageShape.Type = EFragmentDamageShapeType::Circle;
				Event.DamageShape.WorldTransform = HostSource->GetActorTransform();
				Event.DamageShape.Radius = 18.0f;
				Event.DamagePower = 1200.0f;
				Event.EventSeed = 20260809;
				UFragmentSimulationSubsystem* Subsystem =
					Host->GetSubsystem<UFragmentSimulationSubsystem>();
				if (!Subsystem
					|| !Subsystem->RequestFragmentDamage(HostSource, Event))
				{
					Test->AddError(
						TEXT("The local listen host could not commit authoritative damage."));
					return true;
				}
				bAppliedHostDamage = true;
				BeginNextPhase();
				return false;
			}

			TArray<AMatterFluxNetworkTestFragmentActor*> HostFragments;
			FindTestFragments(Host, HostFragments);
			if (!bComparedPayloads)
			{
				AFragment2DSourceActor* HostSource = FindListenSource(Host, TestSourceId);
				if (!HostSource
					|| HostSource->Revision != 1
					|| !HostSource->bBroken
					|| HostFragments.IsEmpty())
				{
					return FailOnTimeout(
						TEXT("The committed host cut did not replicate its revision, broken state, and fragments."));
				}

				for (UWorld* ClientWorld : Clients)
				{
					AFragment2DSourceActor* ClientSource =
						FindListenSource(ClientWorld, TestSourceId);
					TArray<AMatterFluxNetworkTestFragmentActor*> ClientFragments;
					FindTestFragments(ClientWorld, ClientFragments);
					if (!ClientSource
						|| ClientSource->Revision != 1
						|| !ClientSource->bBroken
						|| ClientFragments.Num() != HostFragments.Num())
					{
						return FailOnTimeout(
							TEXT("The committed host cut did not converge on every client."));
					}
					for (int32 Index = 0; Index < HostFragments.Num(); ++Index)
					{
						const FFragmentSpawnPayload& HostPayload =
							HostFragments[Index]->SpawnPayload;
						const FFragmentSpawnPayload& ClientPayload =
							ClientFragments[Index]->SpawnPayload;
						const bool bPayloadEqual = PayloadEquals(
							HostPayload,
							ClientPayload);
						const bool bAppearanceEqual =
							HostFragments[Index]->FragmentMaterial
								== ClientFragments[Index]->FragmentMaterial
							&& HostFragments[Index]->FragmentColor.Equals(
								ClientFragments[Index]->FragmentColor);
						const bool bMeshMaterialsApplied =
							ClientFragments[Index]->MeshComponent->GetMaterial(0)
								!= nullptr
							&& ClientFragments[Index]->MeshComponent->GetMaterial(1)
								!= nullptr;
						if (!bPayloadEqual
							|| !bAppearanceEqual
							|| !bMeshMaterialsApplied)
						{
							Test->AddError(
								TEXT("A listen client received a different fragment payload, appearance, or material assignment."));
							return true;
						}
					}
				}

				MovedFragmentId = HostFragments[0]->SpawnPayload.FragmentId;
				InitialHostLocation = HostFragments[0]->GetActorLocation();
				HostFragments[0]->MeshComponent->SetPhysicsLinearVelocity(
					FVector(250.0, 0.0, 100.0));
				HostFragments[0]->ForceNetUpdate();
				bComparedPayloads = true;
				BeginNextPhase();
				return false;
			}

			AMatterFluxNetworkTestFragmentActor* HostMoved =
				FindFragmentById(HostFragments, MovedFragmentId);
			if (!HostMoved)
			{
				return FailOnTimeout(
					TEXT("The moving replicated fragment disappeared."));
			}
			const bool bHostMoved = FVector::Distance(
				HostMoved->GetActorLocation(),
				InitialHostLocation) > 10.0;
			bool bAllClientsConverged = true;
			for (UWorld* ClientWorld : Clients)
			{
				TArray<AMatterFluxNetworkTestFragmentActor*> ClientFragments;
				FindTestFragments(ClientWorld, ClientFragments);
				const AMatterFluxNetworkTestFragmentActor* ClientMoved =
					FindFragmentById(ClientFragments, MovedFragmentId);
				bAllClientsConverged &= ClientMoved
					&& FVector::Distance(
						HostMoved->GetActorLocation(),
						ClientMoved->GetActorLocation()) < 75.0;
			}
			if (bHostMoved && bAllClientsConverged)
			{
				return true;
			}
			return FailOnTimeout(
				TEXT("Listen-host fragment movement did not converge on the client."));
		}

	private:
		void RestoreCommandLine()
		{
			if (!bCommandLineRestored)
			{
				FCommandLine::Set(*OriginalCommandLine);
				bCommandLineRestored = true;
			}
		}

		void BeginNextPhase()
		{
			PhaseStartTime = FPlatformTime::Seconds();
		}

		bool FailOnTimeout(const TCHAR* Message)
		{
			constexpr double PhaseTimeoutSeconds = 30.0;
			if (FPlatformTime::Seconds() - PhaseStartTime < PhaseTimeoutSeconds)
			{
				return false;
			}
			Test->AddError(Message);
			return true;
		}

		FAutomationTestBase* Test = nullptr;
		FString OriginalCommandLine;
		int32 ExpectedPlayerCount = 2;
		double PhaseStartTime = 0.0;
		int32 InitialHostOperationCount = 0;
		int32 InitialClientOperationCount = 0;
		int32 RecordedClientPlayerId = INDEX_NONE;
		bool bCommandLineRestored = false;
		bool bValidatedPlayers = false;
		bool bValidatedRecordedRelay = false;
		bool bSpawnedSource = false;
		bool bAppliedHostDamage = false;
		bool bComparedPayloads = false;
		FGuid TestSourceId;
		FGuid MovedFragmentId;
		FVector InitialHostLocation = FVector::ZeroVector;
	};

	bool RunListenServerScenario(
		FAutomationTestBase& Test,
		const int32 ExpectedPlayerCount,
		const TCHAR* RecordingName)
	{
		if (!Test.TestNotNull(
			TEXT("Isolated listen-server network map created"),
			FAutomationEditorCommonUtils::CreateNewMap()))
		{
			return false;
		}

		Test.AddExpectedError(
			TEXT("FNetGUIDCache::SupportsObject: Level /Temp/"),
			EAutomationExpectedErrorFlags::Contains,
			-1,
			false);

		const FString OriginalCommandLine = FCommandLine::Get();
		const FString RecordingDirectory = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("RecordE2E"));
		FCommandLine::Set(*FString::Printf(
			TEXT("%s -MFRecord -MFRecordDir=\"%s\" -MFRecordName=%s"),
			*OriginalCommandLine,
			*RecordingDirectory,
			RecordingName));
		Test.AddExpectedError(
			TEXT("RegisterNetGUID_Client: Guid with pathname"),
			EAutomationExpectedErrorFlags::Contains,
			-1,
			false);

		FRequestPlaySessionParams RequestParams;
		ULevelEditorPlaySettings* PlaySettings =
			NewObject<ULevelEditorPlaySettings>();
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
		PlaySettings->SetRunUnderOneProcess(true);
		// UE includes the listen host in this count.
		PlaySettings->SetPlayNumberOfClients(ExpectedPlayerCount);
		PlaySettings->bLaunchSeparateServer = false;
		RequestParams.EditorPlaySettings = PlaySettings;
		FAutomationEditorCommonUtils::SetPlaySessionStartToActiveViewport(
			RequestParams);
		// FStartPIEForAutomationCommand owns the matching RemoveFromRoot in UE 5.8.
		PlaySettings->AddToRoot();
		ADD_LATENT_AUTOMATION_COMMAND(
			FStartPIEForAutomationCommand(RequestParams));
		ADD_LATENT_AUTOMATION_COMMAND(
			FVerifyMatterFluxListenServerPIECommand(
				&Test,
				OriginalCommandLine,
				ExpectedPlayerCount));
		ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
		return true;
	}

	bool RunFallenTreeContactScenario(FAutomationTestBase& Test)
	{
		if (!Test.TestNotNull(
			TEXT("Isolated fallen-tree contact map created"),
			FAutomationEditorCommonUtils::CreateNewMap()))
		{
			return false;
		}

		Test.AddExpectedError(
			TEXT("FNetGUIDCache::SupportsObject: Level /Temp/"),
			EAutomationExpectedErrorFlags::Contains,
			-1,
			false);
		Test.AddExpectedError(
			TEXT("RegisterNetGUID_Client: Guid with pathname"),
			EAutomationExpectedErrorFlags::Contains,
			-1,
			false);

		FRequestPlaySessionParams RequestParams;
		ULevelEditorPlaySettings* PlaySettings =
			NewObject<ULevelEditorPlaySettings>();
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
		PlaySettings->SetRunUnderOneProcess(true);
		PlaySettings->SetPlayNumberOfClients(2);
		PlaySettings->bLaunchSeparateServer = false;
		RequestParams.EditorPlaySettings = PlaySettings;
		FAutomationEditorCommonUtils::SetPlaySessionStartToActiveViewport(
			RequestParams);
		PlaySettings->AddToRoot();
		ADD_LATENT_AUTOMATION_COMMAND(
			FStartPIEForAutomationCommand(RequestParams));
		ADD_LATENT_AUTOMATION_COMMAND(
			FVerifyFallenTreeCharacterContactCommand(&Test));
		ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxListenServerPIETest,
	"MatterFlux.Fragment.Network.ListenHostAndClient",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxListenServerPIETest::RunTest(const FString& Parameters)
{
	return RunListenServerScenario(
		*this,
		2,
		TEXT("ListenHostClientRelayAutomation"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFourPlayerListenServerPIETest,
	"MatterFlux.Fragment.Network.ListenHostAndThreeClients",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFourPlayerListenServerPIETest::RunTest(
	const FString& Parameters)
{
	return RunListenServerScenario(
		*this,
		4,
		TEXT("ListenHostThreeClientsAutomation"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFallenTreeCharacterContactPIETest,
	"MatterFlux.Fragment.Physics.FallenTreeCharacterContactIsStable",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFallenTreeCharacterContactPIETest::RunTest(
	const FString& Parameters)
{
	return RunFallenTreeContactScenario(*this);
}
