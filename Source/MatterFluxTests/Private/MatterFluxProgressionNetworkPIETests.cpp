#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Game/MatterFluxPlayerState.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

namespace MatterFluxProgressionNetworkTests
{
	void FindPIEWorlds(UWorld*& OutServer, TArray<UWorld*>& OutClients)
	{
		OutServer = nullptr;
		OutClients.Reset();
		if (!GEngine) return;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || Context.WorldType != EWorldType::PIE) continue;
			if (World->GetNetMode() == NM_DedicatedServer) OutServer = World;
			else if (World->GetNetMode() == NM_Client) OutClients.Add(World);
		}
		OutClients.Sort([](const UWorld& A, const UWorld& B)
		{
			return A.GetPackage()->GetPIEInstanceID()
				< B.GetPackage()->GetPIEInstanceID();
		});
	}

	APlayerController* FindLocalController(UWorld* World)
	{
		for (TActorIterator<APlayerController> It(World); It; ++It)
		{
			if (It->IsLocalController()) return *It;
		}
		return nullptr;
	}

	AMatterFluxPlayerState* FindPlayerState(UWorld* World, const int32 PlayerId)
	{
		for (TActorIterator<AMatterFluxPlayerState> It(World); It; ++It)
		{
			if (It->GetPlayerId() == PlayerId) return *It;
		}
		return nullptr;
	}

	bool SameOwnerState(
		const UMatterFluxProgressionComponent& A,
		const UMatterFluxProgressionComponent& B)
	{
		if (A.GetRevision() != B.GetRevision()
			|| A.GetSelectedQuest() != B.GetSelectedQuest()
			|| A.GetItems().Num() != B.GetItems().Num()
			|| A.GetQuests().Num() != B.GetQuests().Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.GetItems().Num(); ++Index)
		{
			if (A.GetItems()[Index].ItemId != B.GetItems()[Index].ItemId
				|| A.GetItems()[Index].Quantity != B.GetItems()[Index].Quantity)
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < A.GetQuests().Num(); ++Index)
		{
			const FMatterFluxQuestState& Left = A.GetQuests()[Index];
			const FMatterFluxQuestState& Right = B.GetQuests()[Index];
			if (Left.QuestId != Right.QuestId || Left.Status != Right.Status
				|| Left.Progress != Right.Progress
				|| Left.bActivationRewardsGranted != Right.bActivationRewardsGranted
				|| Left.bCompletionRewardsGranted != Right.bCompletionRewardsGranted)
			{
				return false;
			}
		}
		return true;
	}

	class FVerifyProgressionNetworkCommand final : public IAutomationLatentCommand
	{
	public:
		explicit FVerifyProgressionNetworkCommand(FAutomationTestBase* InTest)
			: Test(InTest), PhaseStart(FPlatformTime::Seconds()) {}

		virtual bool Update() override
		{
			UWorld* Server = nullptr;
			TArray<UWorld*> Clients;
			FindPIEWorlds(Server, Clients);
			if (!Server || Clients.Num() != 2)
			{
				return FailOnTimeout(TEXT("Progression PIE did not create one dedicated server and two clients."));
			}
			APlayerController* ClientController = FindLocalController(Clients[0]);
			APlayerController* OtherController = FindLocalController(Clients[1]);
			AMatterFluxPlayerState* ClientState = ClientController
				? ClientController->GetPlayerState<AMatterFluxPlayerState>() : nullptr;
			AMatterFluxPlayerState* OtherState = OtherController
				? OtherController->GetPlayerState<AMatterFluxPlayerState>() : nullptr;
			AMatterFluxPlayerState* ServerState = ClientState
				? FindPlayerState(Server, ClientState->GetPlayerId()) : nullptr;
			UMatterFluxProgressionComponent* ClientProgression = ClientState
				? ClientState->GetProgression() : nullptr;
			UMatterFluxProgressionComponent* ServerProgression = ServerState
				? ServerState->GetProgression() : nullptr;
			UMatterFluxProgressionComponent* OtherProgression = OtherState
				? OtherState->GetProgression() : nullptr;
			if (!ClientProgression || !ServerProgression || !OtherProgression
				|| ClientProgression->GetRevision() <= 0
				|| !SameOwnerState(*ClientProgression, *ServerProgression))
			{
				if (FPlatformTime::Seconds() - PhaseStart >= 30.0
					&& ClientProgression && ServerProgression)
				{
					Test->AddError(FString::Printf(
						TEXT("Starter progression mismatch: client rev=%d items=%d quests=%d selected=%s; server rev=%d items=%d quests=%d selected=%s"),
						ClientProgression->GetRevision(), ClientProgression->GetItems().Num(),
						ClientProgression->GetQuests().Num(), *ClientProgression->GetSelectedQuest().ToString(),
						ServerProgression->GetRevision(), ServerProgression->GetItems().Num(),
						ServerProgression->GetQuests().Num(), *ServerProgression->GetSelectedQuest().ToString()));
					for (int32 Index = 0; Index < FMath::Min(
						ClientProgression->GetQuests().Num(),
						ServerProgression->GetQuests().Num()); ++Index)
					{
						const FMatterFluxQuestState& C = ClientProgression->GetQuests()[Index];
						const FMatterFluxQuestState& S = ServerProgression->GetQuests()[Index];
						if (C.QuestId != S.QuestId || C.Status != S.Status
							|| C.Progress != S.Progress
							|| C.bActivationRewardsGranted != S.bActivationRewardsGranted
							|| C.bCompletionRewardsGranted != S.bCompletionRewardsGranted)
						{
							Test->AddError(FString::Printf(
								TEXT("Quest[%d] differs: client %s status=%d progress=%d activation=%d completion=%d; server %s status=%d progress=%d activation=%d completion=%d"),
								Index, *C.QuestId.ToString(), static_cast<int32>(C.Status), C.Progress,
								C.bActivationRewardsGranted, C.bCompletionRewardsGranted,
								*S.QuestId.ToString(), static_cast<int32>(S.Status), S.Progress,
								S.bActivationRewardsGranted, S.bCompletionRewardsGranted));
							break;
						}
					}
					return true;
				}
				return FailOnTimeout(TEXT("Starter progression did not replicate coherently to its owner."));
			}

			if (!bServerEventApplied)
			{
				const int32 ClientRevision = ClientProgression->GetRevision();
				const int32 ClientCoins = ClientProgression->GetItemQuantity(TEXT("std.coin"));
				FString Error;
				if (ClientProgression->AddItemAuthority(TEXT("std.coin"), 99, Error)
					|| ClientProgression->GetRevision() != ClientRevision
					|| ClientProgression->GetItemQuantity(TEXT("std.coin")) != ClientCoins)
				{
					Test->AddError(TEXT("Owning client mutated authoritative item state directly."));
					return true;
				}
				const FMatterFluxQuestState* Kill =
					ServerProgression->FindQuestState(TEXT("std.init_quest.kill_enemy"));
				if (!Kill || Kill->Status != EMatterFluxQuestRuntimeStatus::Active)
				{
					return FailOnTimeout(TEXT("Migrated tutorial kill objective did not become active."));
				}
				FMatterFluxQuestEvent Event;
				Event.Type = EMatterFluxQuestEventType::EnemyKilled;
				Event.Amount = 3;
				if (!ServerProgression->NotifyQuestEventAuthority(Event, Error))
				{
					Test->AddError(FString::Printf(
						TEXT("Server quest event failed: %s"), *Error));
					return true;
				}
				ExpectedRevision = ServerProgression->GetRevision();
				bServerEventApplied = true;
				PhaseStart = FPlatformTime::Seconds();
				return false;
			}

			if (!SameOwnerState(*ClientProgression, *ServerProgression)
				|| ClientProgression->GetRevision() != ExpectedRevision)
			{
				return FailOnTimeout(TEXT("Committed quest event did not round-trip to the owning client."));
			}
			const FMatterFluxQuestState* Tutorial =
				ClientProgression->FindQuestState(TEXT("std.init_quest"));
			if (!Tutorial || Tutorial->Status != EMatterFluxQuestRuntimeStatus::Completed
				|| ClientProgression->GetSelectedQuest()
					!= FName(TEXT("std.init_quest_buy")))
			{
				Test->AddError(TEXT("Migrated tutorial completion or next-quest focus differs on client."));
			}
		AMatterFluxPlayerState* ForeignState = FindPlayerState(
			Clients[1], ClientState->GetPlayerId());
		UMatterFluxProgressionComponent* Foreign = ForeignState
			? ForeignState->GetProgression() : nullptr;
		if (Foreign && Foreign->GetRevision() != 0)
		{
			Test->AddError(TEXT("Owner-only quest/item state leaked to the other client."));
		}
		return true;
		}

	private:
		bool FailOnTimeout(const TCHAR* Message)
		{
			if (FPlatformTime::Seconds() - PhaseStart < 30.0) return false;
			Test->AddError(Message);
			return true;
		}

		FAutomationTestBase* Test = nullptr;
		double PhaseStart = 0.0;
		int32 ExpectedRevision = 0;
		bool bServerEventApplied = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProgressionNetworkPIETest,
	"MatterFlux.Progression.Network.DedicatedServerTwoClients",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxProgressionNetworkPIETest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Isolated progression network map created"),
		FAutomationEditorCommonUtils::CreateNewMap()))
	{
		return false;
	}
	AddExpectedError(TEXT("FNetGUIDCache::SupportsObject: Level /Temp/"),
		EAutomationExpectedErrorFlags::Contains, -1, false);
	AddExpectedError(TEXT("RegisterNetGUID_Client: Guid with pathname"),
		EAutomationExpectedErrorFlags::Contains, -1, false);
	FRequestPlaySessionParams RequestParams;
	ULevelEditorPlaySettings* PlaySettings = NewObject<ULevelEditorPlaySettings>();
	PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Client);
	PlaySettings->SetRunUnderOneProcess(true);
	PlaySettings->SetPlayNumberOfClients(2);
	PlaySettings->bLaunchSeparateServer = false;
	RequestParams.EditorPlaySettings = PlaySettings;
	FAutomationEditorCommonUtils::SetPlaySessionStartToActiveViewport(RequestParams);
	PlaySettings->AddToRoot();
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIEForAutomationCommand(RequestParams));
	ADD_LATENT_AUTOMATION_COMMAND(
		MatterFluxProgressionNetworkTests::FVerifyProgressionNetworkCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	return true;
}
