#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GAS/GA_CastWand.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayerState.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Magic/MatterFluxMagicProjectile.h"
#include "MatterFluxGameplayTags.h"
#include "Misc/AutomationTest.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

namespace MatterFluxMagicNetworkTests
{
	constexpr int32 NetworkCastEquipmentSlot = 3;

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

	AMatterFluxPlayerState* FindPlayerState(UWorld* World, const int32 PlayerId)
	{
		for (TActorIterator<AMatterFluxPlayerState> It(World); It; ++It)
		{
			if (It->GetPlayerId() == PlayerId)
			{
				return *It;
			}
		}
		return nullptr;
	}

	AMatterFluxMagicProjectile* FindProjectile(UWorld* World)
	{
		for (TActorIterator<AMatterFluxMagicProjectile> It(World); It; ++It)
		{
			return *It;
		}
		return nullptr;
	}

	int32 GetEquippedWandCastSerial(
		const UMatterFluxMagicInventoryComponent* Inventory,
		const int32 EquipmentSlot)
	{
		if (!Inventory)
		{
			return INDEX_NONE;
		}
		const FGuid WandId = Inventory->GetEquippedWandId(EquipmentSlot);
		for (const FMatterFluxOwnedWand& Wand : Inventory->GetOwnedWands())
		{
			if (Wand.InstanceId == WandId)
			{
				return Wand.CastSerial;
			}
		}
		return INDEX_NONE;
	}

	FGameplayAbilitySpecHandle FindWandAbilityHandle(
		UAbilitySystemComponent* AbilitySystem,
		const int32 InputId)
	{
		if (!AbilitySystem)
		{
			return FGameplayAbilitySpecHandle();
		}
		for (const FGameplayAbilitySpec& Spec
			: AbilitySystem->GetActivatableAbilities())
		{
			if (Spec.InputID == InputId
				&& Spec.Ability
				&& Spec.Ability->IsA<UGA_CastWand>())
			{
				return Spec.Handle;
			}
		}
		return FGameplayAbilitySpecHandle();
	}

	class FVerifyMagicNetworkCommand final : public IAutomationLatentCommand
	{
	public:
		explicit FVerifyMagicNetworkCommand(FAutomationTestBase* InTest)
			: Test(InTest)
			, PhaseStart(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			UWorld* Server = nullptr;
			TArray<UWorld*> Clients;
			FindPIEWorlds(Server, Clients);
			if (!Server || Clients.Num() != 2)
			{
				return FailOnTimeout(TEXT("Magic PIE did not create one dedicated server and two clients."));
			}

			APlayerController* ClientController = FindLocalController(Clients[0]);
			APlayerController* OtherController = FindLocalController(Clients[1]);
			AMatterFluxPlayerState* ClientState = ClientController
				? ClientController->GetPlayerState<AMatterFluxPlayerState>()
				: nullptr;
			AMatterFluxPlayerState* OtherState = OtherController
				? OtherController->GetPlayerState<AMatterFluxPlayerState>()
				: nullptr;
			AMatterFluxPlayerState* ServerState = ClientState
				? FindPlayerState(Server, ClientState->GetPlayerId())
				: nullptr;
			UMatterFluxMagicInventoryComponent* ClientInventory = ClientState
				? ClientState->GetMagicInventory()
				: nullptr;
			UMatterFluxMagicInventoryComponent* OtherInventory = OtherState
				? OtherState->GetMagicInventory()
				: nullptr;
			UMatterFluxMagicInventoryComponent* ServerInventory = ServerState
				? ServerState->GetMagicInventory()
				: nullptr;

			if (!bRequestedEdit)
			{
				if (!ClientInventory || !OtherInventory || !ServerInventory
					|| ClientInventory->GetInventoryRevision() <= 0
					|| ClientInventory->GetInventoryRevision()
						!= ServerInventory->GetInventoryRevision()
					|| ClientInventory->GetOwnedWands().Num() != 6
					|| ClientInventory->GetEquippedWands().Num() != 4
					|| !ClientInventory->GetActiveWandId().IsValid())
				{
					return FailOnTimeout(TEXT("Starter wand inventory did not replicate coherently to its owning client."));
				}

				InitialRevision = ClientInventory->GetInventoryRevision();
				OtherInitialRevision = OtherInventory->GetInventoryRevision();
				OtherPlayerId = OtherState->GetPlayerId();
				FMatterFluxMagicEdit Edit;
				Edit.Type = EMatterFluxMagicEditType::SelectEquipmentSlot;
				Edit.ExpectedRevision = InitialRevision;
				Edit.EquipmentSlot = 1;
				ClientInventory->RequestEdit(Edit);
				bRequestedEdit = true;
				BeginNextPhase();
				return false;
			}

			if (!bEditReplicated)
			{
				AMatterFluxPlayerState* ForeignCopy =
					FindPlayerState(Clients[1], ClientState->GetPlayerId());
				UMatterFluxMagicInventoryComponent* ForeignInventory = ForeignCopy
					? ForeignCopy->GetMagicInventory()
					: nullptr;
				if (ClientInventory->GetInventoryRevision() != InitialRevision + 1
					|| ServerInventory->GetInventoryRevision() != InitialRevision + 1
					|| ClientInventory->GetActiveEquipmentSlot() != 1
					|| ServerInventory->GetActiveEquipmentSlot() != 1)
				{
					return FailOnTimeout(TEXT("Server-authoritative equipment edit did not round-trip to the owner."));
				}
				if (OtherState->GetPlayerId() != OtherPlayerId
					|| OtherInventory->GetInventoryRevision() != OtherInitialRevision
					|| (ForeignInventory
						&& ForeignInventory->GetInventoryRevision() != 0))
				{
					Test->AddError(TEXT("Owner-only inventory state leaked to or mutated the second client."));
					return true;
				}

				FMatterFluxMagicEdit SelectWand;
				SelectWand.Type = EMatterFluxMagicEditType::SelectEquipmentSlot;
				SelectWand.ExpectedRevision = InitialRevision + 1;
				SelectWand.EquipmentSlot = NetworkCastEquipmentSlot;
				ClientInventory->RequestEdit(SelectWand);
				bEditReplicated = true;
				BeginNextPhase();
				return false;
			}

			if (!bRequestedCast)
			{
				UAbilitySystemComponent* ASC = ClientState
					? ClientState->GetAbilitySystemComponent()
					: nullptr;
				UAbilitySystemComponent* ServerASC = ServerState
					? ServerState->GetAbilitySystemComponent()
					: nullptr;
				const FGameplayAbilitySpecHandle WandHandle =
					FindWandAbilityHandle(ASC, NetworkCastEquipmentSlot);
				const FGameplayAbilitySpecHandle ServerWandHandle =
					FindWandAbilityHandle(ServerASC, NetworkCastEquipmentSlot);
				if (!ASC
					|| !ServerASC
					|| ClientInventory->GetActiveEquipmentSlot()
						!= NetworkCastEquipmentSlot
					|| ServerInventory->GetActiveEquipmentSlot()
						!= NetworkCastEquipmentSlot
					|| ClientInventory->GetInventoryRevision() != InitialRevision + 2
					|| !ASC->AbilityActorInfo.IsValid()
					|| !ASC->AbilityActorInfo->AvatarActor.IsValid()
					|| !ServerASC->AbilityActorInfo.IsValid()
					|| !ServerASC->AbilityActorInfo->AvatarActor.IsValid()
					|| !WandHandle.IsValid()
					|| !ServerWandHandle.IsValid())
				{
					return FailOnTimeout(TEXT("Client/server wand ability actor info or restored active slot was not ready."));
				}
				AActor* ServerAvatar = ServerASC->AbilityActorInfo
					->AvatarActor.Get();
				ServerAvatar->SetActorLocation(
					FVector(0.0f, 0.0f, 2500.0f),
					false,
					nullptr,
					ETeleportType::TeleportPhysics);
				AMatterFluxCharacter* ClientAvatar = Cast<AMatterFluxCharacter>(
					ASC->AbilityActorInfo->AvatarActor.Get());
				if (!ClientAvatar)
				{
					return FailOnTimeout(TEXT("Owning client has no MatterFlux character avatar."));
				}
				ClientAvatar->ApplyPlayerOperation(
					EMatterFluxPlayerOperation::CastWand,
					FVector2D::ZeroVector,
					NetworkCastEquipmentSlot);
				bRequestedCast = true;
				BeginNextPhase();
				return false;
			}

			const int32 ServerCastSerial = GetEquippedWandCastSerial(
				ServerInventory,
				NetworkCastEquipmentSlot);
			if (!bFirstCastObserved)
			{
				const int32 ClientCastSerial = GetEquippedWandCastSerial(
					ClientInventory,
					NetworkCastEquipmentSlot);
				if (ServerCastSerial <= 0
					|| ClientCastSerial != ServerCastSerial)
				{
					return FailOnTimeout(TEXT("First wand cast serial did not commit and replicate to its owner."));
				}
				FirstCastSerial = ServerCastSerial;
				bFirstCastObserved = true;
				BeginNextPhase();
				return false;
			}

			AMatterFluxMagicProjectile* ServerProjectile = FindProjectile(Server);
			AMatterFluxMagicProjectile* ClientProjectile = FindProjectile(Clients[0]);
			AMatterFluxMagicProjectile* OtherProjectile = FindProjectile(Clients[1]);
			if (ServerProjectile && ClientProjectile && OtherProjectile)
			{
				const FMatterFluxMagicProjectilePresentation& ServerView =
					ServerProjectile->GetPresentation();
				const FMatterFluxMagicProjectilePresentation& ClientView =
					ClientProjectile->GetPresentation();
				const FMatterFluxMagicProjectilePresentation& OtherView =
					OtherProjectile->GetPresentation();
				if (ServerView.SpellId != ClientView.SpellId
					|| ServerView.SpellId != OtherView.SpellId
					|| !FMath::IsNearlyEqual(ServerView.Speed, ClientView.Speed)
					|| !FMath::IsNearlyEqual(ServerView.Speed, OtherView.Speed)
					|| !FMath::IsNearlyEqual(ServerView.Radius, ClientView.Radius)
					|| !FMath::IsNearlyEqual(ServerView.Radius, OtherView.Radius))
				{
					Test->AddError(TEXT("Replicated projectile presentation differs across peers."));
				}
			}

			UAbilitySystemComponent* ASC = ClientState
				? ClientState->GetAbilitySystemComponent()
				: nullptr;
			const FGameplayAbilitySpecHandle WandHandle =
				FindWandAbilityHandle(ASC, NetworkCastEquipmentSlot);
			if (!bRequestedSecondCast)
			{
				if (FPlatformTime::Seconds() - PhaseStart < 0.8)
				{
					return false;
				}
				AMatterFluxCharacter* ClientAvatar = ASC
					? Cast<AMatterFluxCharacter>(
						ASC->AbilityActorInfo->AvatarActor.Get())
					: nullptr;
				if (!ClientAvatar || !WandHandle.IsValid())
				{
					Test->AddError(TEXT("The same held wand cannot activate a second time after its recharge."));
					return true;
				}
				ClientAvatar->ApplyPlayerOperation(
					EMatterFluxPlayerOperation::CastWand,
					FVector2D::ZeroVector,
					NetworkCastEquipmentSlot);
				bRequestedSecondCast = true;
				BeginNextPhase();
				return false;
			}
			if (!bSecondCastObserved)
			{
				const int32 ClientCastSerial = GetEquippedWandCastSerial(
					ClientInventory,
					NetworkCastEquipmentSlot);
				if (ServerCastSerial < FirstCastSerial + 1
					|| ClientCastSerial != ServerCastSerial)
				{
					return FailOnTimeout(TEXT("Second consecutive wand cast did not commit and replicate to its owner."));
				}
				bSecondCastObserved = true;
				BeginNextPhase();
				return false;
			}
			if (!bRequestedThirdCast)
			{
				if (FPlatformTime::Seconds() - PhaseStart < 0.8)
				{
					return false;
				}
				AMatterFluxCharacter* ClientAvatar = ASC
					? Cast<AMatterFluxCharacter>(
						ASC->AbilityActorInfo->AvatarActor.Get())
					: nullptr;
				if (!ClientAvatar || !WandHandle.IsValid())
				{
					Test->AddError(TEXT("The same held wand cannot activate a third time after its recharge."));
					return true;
				}
				ClientAvatar->ApplyPlayerOperation(
					EMatterFluxPlayerOperation::CastWand,
					FVector2D::ZeroVector,
					NetworkCastEquipmentSlot);
				bRequestedThirdCast = true;
				BeginNextPhase();
				return false;
			}
			const int32 ClientCastSerial = GetEquippedWandCastSerial(
				ClientInventory,
				NetworkCastEquipmentSlot);
			if (ServerCastSerial < FirstCastSerial + 2
				|| ClientCastSerial != ServerCastSerial)
			{
				return FailOnTimeout(TEXT("Third consecutive wand cast did not commit and replicate to its owner."));
			}
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
		int32 InitialRevision = 0;
		int32 OtherInitialRevision = 0;
		int32 OtherPlayerId = INDEX_NONE;
		bool bRequestedEdit = false;
		bool bEditReplicated = false;
		bool bRequestedCast = false;
		int32 FirstCastSerial = INDEX_NONE;
		bool bFirstCastObserved = false;
		bool bRequestedSecondCast = false;
		bool bSecondCastObserved = false;
		bool bRequestedThirdCast = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMagicNetworkPIETest,
	"MatterFlux.Magic.Network.DedicatedServerTwoClients",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxMagicNetworkPIETest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(
		TEXT("Isolated magic network map created"),
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
	PlaySettings->SetPlayNumberOfClients(2);
	PlaySettings->bLaunchSeparateServer = false;
	RequestParams.EditorPlaySettings = PlaySettings;
	FAutomationEditorCommonUtils::SetPlaySessionStartToActiveViewport(
		RequestParams);
	PlaySettings->AddToRoot();
	ADD_LATENT_AUTOMATION_COMMAND(
		FStartPIEForAutomationCommand(RequestParams));
	ADD_LATENT_AUTOMATION_COMMAND(
		MatterFluxMagicNetworkTests::FVerifyMagicNetworkCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	return true;
}
