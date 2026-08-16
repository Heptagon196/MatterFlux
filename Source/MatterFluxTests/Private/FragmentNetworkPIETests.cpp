#include "AbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "FragmentTestActors.h"
#include "GAS/GA_FragmentDebugDamage.h"
#include "Game/MatterFluxPlayerState.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "IMatterFluxScriptRuntime.h"
#include "Materials/Material.h"
#include "MatterFluxGameplayTags.h"
#include "Misc/AutomationTest.h"
#include "ProceduralMeshComponent.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

namespace
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
		OutClients.Sort([](const UWorld& A, const UWorld& B) { return A.GetPackage()->GetPIEInstanceID() < B.GetPackage()->GetPIEInstanceID(); });
	}

	AFragment2DSourceActor* FindSource(UWorld* World, const FGuid& ExpectedSourceId)
	{
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			if (!ExpectedSourceId.IsValid() || It->SourceId == ExpectedSourceId)
			{
				return *It;
			}
		}
		return nullptr;
	}

	AMatterFluxPlayableWorldActor* FindPlayableWorldActor(
		UWorld* World)
	{
		TActorIterator<AMatterFluxPlayableWorldActor> It(World);
		return It ? *It : nullptr;
	}

	void FindFragments(UWorld* World, TArray<AFragment2DActor*>& OutFragments)
	{
		OutFragments.Reset();
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			if (It->SpawnPayload.FragmentId.IsValid()) OutFragments.Add(*It);
		}
		OutFragments.Sort([](const AFragment2DActor& A, const AFragment2DActor& B)
		{
			return A.SpawnPayload.FragmentId.ToString(EGuidFormats::Digits) < B.SpawnPayload.FragmentId.ToString(EGuidFormats::Digits);
		});
	}

	int32 CountFragmentActors(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	APlayerController* FindLocalPlayerController(UWorld* World)
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

	AFragment2DActor* FindFragmentById(const TArray<AFragment2DActor*>& Fragments, const FGuid& FragmentId)
	{
		for (AFragment2DActor* Fragment : Fragments)
		{
			if (Fragment && Fragment->SpawnPayload.FragmentId == FragmentId) return Fragment;
		}
		return nullptr;
	}

	bool PayloadGeometryEquals(const FFragmentSpawnPayload& A, const FFragmentSpawnPayload& B)
	{
		if (A.FragmentId != B.FragmentId || A.Revision != B.Revision || A.Vertices2D != B.Vertices2D
			|| A.TriangleIndices != B.TriangleIndices || A.OuterContours.Num() != B.OuterContours.Num()
			|| A.HoleContours.Num() != B.HoleContours.Num() || A.CollisionContours.Num() != B.CollisionContours.Num()
			|| A.bEnableCollision != B.bEnableCollision
			|| A.Thickness != B.Thickness) return false;
		for (int32 Index = 0; Index < A.OuterContours.Num(); ++Index)
		{
			if (A.OuterContours[Index].Vertices != B.OuterContours[Index].Vertices) return false;
		}
		for (int32 Index = 0; Index < A.HoleContours.Num(); ++Index)
		{
			if (A.HoleContours[Index].Vertices != B.HoleContours[Index].Vertices) return false;
		}
		for (int32 Index = 0; Index < A.CollisionContours.Num(); ++Index)
		{
			if (A.CollisionContours[Index].Vertices != B.CollisionContours[Index].Vertices) return false;
		}
		return true;
	}

	bool AggregateSourcesEqual(
		const AFragment2DActor& A,
		const AFragment2DActor& B)
	{
		if (A.AggregateSources.Num() != B.AggregateSources.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.AggregateSources.Num(); ++Index)
		{
			const FFragmentAggregateSourceState& Left = A.AggregateSources[Index];
			const FFragmentAggregateSourceState& Right = B.AggregateSources[Index];
			if (Left.SourceId != Right.SourceId
				|| Left.Revision != Right.Revision
				|| Left.SourceMask.Width != Right.SourceMask.Width
				|| Left.SourceMask.Height != Right.SourceMask.Height
				|| Left.SourceMask.CellSize != Right.SourceMask.CellSize
				|| Left.SourceMask.SolidMask != Right.SourceMask.SolidMask
				|| !Left.LocalTransform.Equals(Right.LocalTransform)
				|| Left.MaterialId != Right.MaterialId
				|| !Left.Color.Equals(Right.Color)
				|| Left.bEnableCollision != Right.bEnableCollision
				|| Left.bHasCombustionState != Right.bHasCombustionState
				|| Left.CombustionRuleId != Right.CombustionRuleId
				|| Left.ResidueMaterialId != Right.ResidueMaterialId
				|| !Left.ResidueColor.Equals(Right.ResidueColor)
				|| Left.CombustionSeed != Right.CombustionSeed
				|| Left.CombustionTick != Right.CombustionTick
				|| Left.CombustionAccumulator != Right.CombustionAccumulator
				|| Left.TotalSmokeEmissionCount != Right.TotalSmokeEmissionCount
				|| Left.ResidueMask.Width != Right.ResidueMask.Width
				|| Left.ResidueMask.Height != Right.ResidueMask.Height
				|| Left.ResidueMask.SolidMask != Right.ResidueMask.SolidMask
				|| Left.BurningMask.Width != Right.BurningMask.Width
				|| Left.BurningMask.Height != Right.BurningMask.Height
				|| Left.BurningMask.SolidMask != Right.BurningMask.SolidMask)
			{
				return false;
			}
		}
		return true;
	}

	class FVerifyMatterFluxNetworkPIECommand final : public IAutomationLatentCommand
	{
	public:
		explicit FVerifyMatterFluxNetworkPIECommand(FAutomationTestBase* InTest)
			: Test(InTest), PhaseStartTime(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			UWorld* Server = nullptr;
			TArray<UWorld*> Clients;
			FindPIEWorlds(Server, Clients);
			if (!Server || Clients.Num() != 2)
			{
				return FailOnTimeout(TEXT("Dedicated server and two client PIE worlds were not created."));
			}

			if (!bValidatedMaterialState)
			{
				AMatterFluxPlayableWorldActor* ServerWorldActor =
					FindPlayableWorldActor(Server);
				AMatterFluxPlayableWorldActor* ClientAWorldActor =
					FindPlayableWorldActor(Clients[0]);
				AMatterFluxPlayableWorldActor* ClientBWorldActor =
					FindPlayableWorldActor(Clients[1]);
				if (!ServerWorldActor
					|| !ClientAWorldActor
					|| !ClientBWorldActor
					|| ServerWorldActor->GetReplicatedMaterialStateByteCount()
						<= 0
					|| ClientAWorldActor->GetAppliedMaterialStateRevision()
						<= 0
					|| ClientAWorldActor->GetAppliedMaterialStateRevision()
						!= ClientBWorldActor->GetAppliedMaterialStateRevision()
					|| ClientAWorldActor->GetMaterialSimulationStep()
						!= ClientBWorldActor->GetMaterialSimulationStep()
					|| ClientBWorldActor->GetMaterialSimulationStep()
						> ServerWorldActor->GetMaterialSimulationStep())
				{
					return FailOnTimeout(
						TEXT("The authoritative material snapshot did not initialize both clients coherently."));
				}
				static const FName MaterialIds[] =
				{
					TEXT("water"),
					TEXT("lava"),
					TEXT("sand"),
					TEXT("steam"),
					TEXT("stone")
				};
				for (const FName MaterialId : MaterialIds)
				{
					const int32 ClientCount =
						ClientAWorldActor->GetSimulatedMaterialCount(
							MaterialId);
					if (ClientBWorldActor->GetSimulatedMaterialCount(
							MaterialId)
							!= ClientCount)
					{
						return FailOnTimeout(
							TEXT("Clients imported different active material cells from the server snapshot."));
					}
				}
				bValidatedMaterialState = true;
				BeginNextPhase();
				return false;
			}

			if (!bRequestedLogicalCombustion)
			{
				AMatterFluxPlayableWorldActor* ServerWorldActor =
					FindPlayableWorldActor(Server);
				if (!ServerWorldActor
					|| !ServerWorldActor->IgniteFirstGeneratedTree(7331))
				{
					return FailOnTimeout(
						TEXT("The server could not ignite a logical tree source."));
				}
				// Hold the deterministic cell state stable while the Fast Array add
				// reaches both clients; replication remains active when actor Tick is
				// disabled.
				ServerWorldActor->SetActorTickEnabled(false);
				ServerWorldActor->ForceNetUpdate();
				bRequestedLogicalCombustion = true;
				BeginNextPhase();
				return false;
			}

			if (!bValidatedLogicalCombustion)
			{
				AMatterFluxPlayableWorldActor* ServerWorldActor =
					FindPlayableWorldActor(Server);
				AMatterFluxPlayableWorldActor* ClientAWorldActor =
					FindPlayableWorldActor(Clients[0]);
				AMatterFluxPlayableWorldActor* ClientBWorldActor =
					FindPlayableWorldActor(Clients[1]);
				const int32 ServerFuel = ServerWorldActor
					? ServerWorldActor->GetLogicalCombustionFuelCellCount(TEXT("wood"))
					: 0;
				if (!ServerWorldActor
					|| !ClientAWorldActor
					|| !ClientBWorldActor
					|| ServerWorldActor->GetCombustingSourceCount() <= 0
					|| ClientAWorldActor->GetCombustingSourceCount() <= 0
					|| ClientBWorldActor->GetCombustingSourceCount() <= 0
					|| ServerFuel <= 0
					|| ClientAWorldActor->GetLogicalCombustionFuelCellCount(TEXT("wood"))
						!= ServerFuel
					|| ClientBWorldActor->GetLogicalCombustionFuelCellCount(TEXT("wood"))
						!= ServerFuel)
				{
					return FailOnTimeout(
						TEXT("Logical source combustion Fast Array state did not converge on both clients."));
				}
				for (UWorld* NetworkWorld : { Server, Clients[0], Clients[1] })
				{
					if (FindSource(NetworkWorld, FGuid()))
					{
						return FailOnTimeout(
							TEXT("Logical tree combustion allocated a Source Actor in a network world."));
					}
				}
				ServerWorldActor->SetActorTickEnabled(true);
				bValidatedLogicalCombustion = true;
				BeginNextPhase();
				return false;
			}

			if (!bRequestedAggregateFelling)
			{
				AMatterFluxPlayableWorldActor* ServerWorldActor =
					FindPlayableWorldActor(Server);
				if (!ServerWorldActor)
				{
					return FailOnTimeout(TEXT("Server playable world was not ready for aggregate felling."));
				}
				AFragment2DSourceActor* Root = nullptr;
				MatterFlux::PlayableLevel::FLevelLayout Layout;
				const FMatterFluxContentRegistryPtr Registry =
					IMatterFluxScriptRuntime::Get().GetActiveRegistry();
				if (!Registry.IsValid()
					|| !MatterFlux::PlayableLevel::BuildLevelLayout(
						ServerWorldActor->GetMapSeed(),
						Layout,
						Registry.Get()))
				{
					return FailOnTimeout(TEXT("Generated layout was unavailable for aggregate felling."));
				}
				for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Definition
					: Layout.FragmentSources)
				{
					if (!Definition.bAggregateRoot
						|| !Definition.AggregateId.IsValid()
						|| Definition.MaterialId != TEXT("wood"))
					{
						continue;
					}
					const FVector HalfExtent(
						Definition.Mask.Width * Definition.Mask.CellSize * 0.5f,
						Definition.Mask.CellSize,
						Definition.Mask.Height * Definition.Mask.CellSize * 0.5f);
					const FTransform SourceWorldTransform =
						Definition.Transform * ServerWorldActor->GetActorTransform();
					TArray<AFragment2DSourceActor*> Sources;
					ServerWorldActor->GatherFragmentSourcesInBounds(
						FBox(-HalfExtent, HalfExtent).TransformBy(
							SourceWorldTransform.ToMatrixWithScale()),
						Sources);
					AFragment2DSourceActor** Entry = Sources.FindByPredicate(
						[&Definition](const AFragment2DSourceActor* Source)
						{
							return Source
								&& Source->SourceId == Definition.SourceId;
						});
					if (Entry && *Entry && !(*Entry)->IsCombusting())
					{
						Root = *Entry;
						break;
					}
				}
				if (!Root)
				{
					return FailOnTimeout(TEXT("A non-burning generated tree root could not be materialized."));
				}

				FFragmentDamageEvent Event;
				Event.SourceId = Root->SourceId;
				Event.BaseRevision = Root->Revision;
				Event.EventSeed = 8817;
				Event.DamagePower = 500.0f;
				Event.DamageShape.Type = EFragmentDamageShapeType::Line;
				Event.DamageShape.WorldTransform = Root->GetActorTransform();
				const FVector LocalCutPoint(
					0.0f,
					0.0f,
					-static_cast<float>(Root->GetMaskHeight())
						* Root->GetCellSize() * 0.5f
						+ Root->GetCellSize() * 2.5f);
				Event.DamageShape.WorldTransform.SetLocation(
					Root->GetActorTransform().TransformPosition(LocalCutPoint));
				Event.DamageShape.Extents.X =
					Root->GetCellSize()
						* static_cast<float>(Root->GetMaskWidth() + 2);
				Event.DamageShape.Thickness = Root->GetCellSize() * 0.9f;
				UFragmentSimulationSubsystem* Subsystem =
					Server->GetSubsystem<UFragmentSimulationSubsystem>();
				if (!Subsystem
					|| !Subsystem->RequestFragmentDamage(Root, Event))
				{
					return FailOnTimeout(TEXT("Server could not fell a generated aggregate tree."));
				}

				for (TActorIterator<AFragment2DActor> It(Server); It; ++It)
				{
					if (It->GetAggregateMemberCount() == 3)
					{
						AggregateCarrierId = It->SpawnPayload.FragmentId;
						for (const FFragmentAggregateSourceState& Member
							: It->AggregateSources)
						{
							AggregateSourceIds.Add(Member.SourceId);
						}
						It->bAlwaysRelevant = true;
						It->ForceNetUpdate();
						break;
					}
				}
				if (!AggregateCarrierId.IsValid()
					|| AggregateSourceIds.Num() != 3)
				{
					return FailOnTimeout(TEXT("Generated tree did not collapse into one three-member carrier."));
				}
				bRequestedAggregateFelling = true;
				BeginNextPhase();
				return false;
			}

			if (!bValidatedAggregateCarrier)
			{
				TArray<AFragment2DActor*> ServerFragments;
				TArray<AFragment2DActor*> ClientAFragments;
				TArray<AFragment2DActor*> ClientBFragments;
				FindFragments(Server, ServerFragments);
				FindFragments(Clients[0], ClientAFragments);
				FindFragments(Clients[1], ClientBFragments);
				AFragment2DActor* ServerCarrier =
					FindFragmentById(ServerFragments, AggregateCarrierId);
				AFragment2DActor* ClientACarrier =
					FindFragmentById(ClientAFragments, AggregateCarrierId);
				AFragment2DActor* ClientBCarrier =
					FindFragmentById(ClientBFragments, AggregateCarrierId);
				if (!ServerCarrier || !ClientACarrier || !ClientBCarrier
					|| ServerCarrier->GetAggregateMemberCount() != 3
					|| !AggregateSourcesEqual(*ServerCarrier, *ClientACarrier)
					|| !AggregateSourcesEqual(*ServerCarrier, *ClientBCarrier)
					|| ClientACarrier->MeshComponent->GetNumSections() <= 2
					|| ClientBCarrier->MeshComponent->GetNumSections() <= 2)
				{
					return FailOnTimeout(TEXT("Aggregate member state or multi-material mesh did not converge on both clients."));
				}
				for (UWorld* NetworkWorld : {Server, Clients[0], Clients[1]})
				{
					for (const FGuid& SourceId : AggregateSourceIds)
					{
						if (FindSource(NetworkWorld, SourceId))
						{
							return FailOnTimeout(TEXT("A detached aggregate member retained a Source Actor."));
						}
					}
				}
				ServerCarrier->Destroy();
				bValidatedAggregateCarrier = true;
				BeginNextPhase();
				return false;
			}

			if (!bCleanedAggregateCarrier)
			{
				for (UWorld* NetworkWorld : {Server, Clients[0], Clients[1]})
				{
					TArray<AFragment2DActor*> Fragments;
					FindFragments(NetworkWorld, Fragments);
					if (FindFragmentById(Fragments, AggregateCarrierId))
					{
						return FailOnTimeout(TEXT("Aggregate test carrier did not leave every network world."));
					}
				}
				bCleanedAggregateCarrier = true;
				BeginNextPhase();
				return false;
			}

			if (!bSpawnedSource)
			{
				TArray<AFragment2DSourceActor*> ExistingSources;
				for (TActorIterator<AFragment2DSourceActor> It(Server); It; ++It)
				{
					ExistingSources.Add(*It);
				}
				for (AFragment2DSourceActor* ExistingSource : ExistingSources)
				{
					ExistingSource->Destroy();
				}

				AFragment2DSourceActor* ServerSource = Server->SpawnActorDeferred<AFragment2DSourceActor>(
					AFragment2DSourceActor::StaticClass(),
					FTransform::Identity,
					nullptr,
					nullptr,
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
				if (!ServerSource)
				{
					Test->AddError(TEXT("Dedicated server could not spawn the replicated fragment source."));
					return true;
				}
				ServerSource->Tags.AddUnique(TEXT("MF_Source_Test"));
				// This test verifies exact payload delivery to both clients. Spatial
				// relevancy is covered by the scale tests, so keep this synthetic
				// source visible regardless of the PIE player-start layout.
				ServerSource->bAlwaysRelevant = true;
				ServerSource->bDestroySourceOnFirstBreak = false;
				ServerSource->DefaultSupportMode =
					EFragmentSupportMode::None;
				ServerSource->FragmentActorClass =
					AMatterFluxNetworkTestFragmentActor::StaticClass();
				ServerSource->FragmentMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
				ServerSource->FinishSpawning(FTransform::Identity);
				TestSourceId = ServerSource->SourceId;
				if (!TestSourceId.IsValid())
				{
					Test->AddError(TEXT("Dedicated server source did not initialize a valid SourceId."));
					return true;
				}
				bSpawnedSource = true;
				BeginNextPhase();
				return false;
			}

			if (!bRequestedAbility)
			{
				AFragment2DSourceActor* ClientSource = FindSource(Clients[0], TestSourceId);
				AFragment2DSourceActor* ServerSource = FindSource(Server, TestSourceId);
				APlayerController* ClientController = FindLocalPlayerController(Clients[0]);
				AMatterFluxPlayerState* PlayerState = ClientController ? ClientController->GetPlayerState<AMatterFluxPlayerState>() : nullptr;
				UAbilitySystemComponent* ASC = PlayerState ? PlayerState->GetAbilitySystemComponent() : nullptr;
				if (PlayerState)
				{
					for (TActorIterator<AMatterFluxPlayerState> It(Server); It; ++It)
					{
						AMatterFluxPlayerState* ServerPlayerState = *It;
						if (ServerPlayerState->GetPlayerId() != PlayerState->GetPlayerId())
						{
							continue;
						}
						UAbilitySystemComponent* ServerASC = ServerPlayerState->GetAbilitySystemComponent();
						if (ServerASC && !ServerASC->FindAbilitySpecFromClass(UGA_FragmentDebugDamage::StaticClass()))
						{
							ServerASC->GiveAbility(FGameplayAbilitySpec(UGA_FragmentDebugDamage::StaticClass(), 1));
							ServerPlayerState->ForceNetUpdate();
						}
						break;
					}
				}
				const FGameplayAbilitySpec* DebugAbilitySpec = ASC
					? ASC->FindAbilitySpecFromClass(UGA_FragmentDebugDamage::StaticClass())
					: nullptr;
				const AActor* AvatarActor = ASC ? ASC->GetAvatarActor() : nullptr;
				const bool bActorInfoReady = ASC
					&& ASC->AbilityActorInfo.IsValid()
					&& ASC->AbilityActorInfo->OwnerActor.IsValid()
					&& ASC->AbilityActorInfo->AvatarActor.IsValid()
					&& ASC->AbilityActorInfo->IsLocallyControlled()
					&& AvatarActor
					&& AvatarActor->GetLocalRole() != ROLE_SimulatedProxy;
				if (!ClientSource || !ServerSource || !ASC || !ClientSource->SourceId.IsValid()
					|| !DebugAbilitySpec || !bActorInfoReady
					|| ClientSource->SourceId != ServerSource->SourceId || ClientSource->Revision != ServerSource->Revision)
				{
					return FailOnTimeout(*FString::Printf(
						TEXT("Replicated source identity, revision, or client ASC was not ready. ")
						TEXT("source=%d/%d asc=%d spec=%d actorInfo=%d local=%d avatarRole=%d."),
						ClientSource != nullptr,
						ServerSource != nullptr,
						ASC != nullptr,
						DebugAbilitySpec != nullptr,
						ASC && ASC->AbilityActorInfo.IsValid(),
						ASC && ASC->AbilityActorInfo.IsValid() && ASC->AbilityActorInfo->IsLocallyControlled(),
						AvatarActor ? static_cast<int32>(AvatarActor->GetLocalRole()) : INDEX_NONE));
				}

				if (!bValidatedClientAuthority)
				{
					FFragmentDamageEvent DirectEvent;
					DirectEvent.SourceId = ClientSource->SourceId;
					DirectEvent.BaseRevision = ClientSource->Revision;
					DirectEvent.DamageShape.Type = EFragmentDamageShapeType::Circle;
					DirectEvent.DamageShape.WorldTransform = ClientSource->GetActorTransform();
					DirectEvent.DamageShape.Radius = 20.0f;
					TArray<FFragmentSpawnPayload> ClientPayloads;
					const bool bClientActorAccepted = ClientSource->ApplyDamageEvent(DirectEvent, ClientPayloads);
					UFragmentSimulationSubsystem* ClientSubsystem = Clients[0]->GetSubsystem<UFragmentSimulationSubsystem>();
					if (!ClientSubsystem) return FailOnTimeout(TEXT("Client fragment subsystem was not ready."));
					const bool bClientAccepted = ClientSubsystem->RequestFragmentDamage(ClientSource, DirectEvent);
					if (bClientActorAccepted || bClientAccepted || ClientPayloads.Num() != 0 || ServerSource->Revision != 0)
					{
						Test->AddError(TEXT("A direct client actor or subsystem call modified authority state."));
						return true;
					}
					bValidatedClientAuthority = true;
				}

				FGameplayTagContainer AbilityTags;
				AbilityTags.AddTag(TAG_Ability_Fragment_DebugDamage);
				if (!ASC->TryActivateAbilitiesByTag(AbilityTags))
				{
					FGameplayTagContainer FailureTags;
					const bool bCanActivate = DebugAbilitySpec->Ability->CanActivateAbility(
						DebugAbilitySpec->Handle,
						ASC->AbilityActorInfo.Get(),
						nullptr,
						nullptr,
						&FailureTags);
					return FailOnTimeout(*FString::Printf(
						TEXT("Client GAS debug ability was not ready for activation. CanActivate=%d FailureTags=%s"),
						bCanActivate,
						*FailureTags.ToStringSimple()));
				}
				bRequestedAbility = true;
				BeginNextPhase();
				return false;
			}

			TArray<AFragment2DActor*> ServerFragments;
			TArray<AFragment2DActor*> ClientAFragments;
			TArray<AFragment2DActor*> ClientBFragments;
			FindFragments(Server, ServerFragments);
			FindFragments(Clients[0], ClientAFragments);
			FindFragments(Clients[1], ClientBFragments);
			const int32 ServerActorCount = CountFragmentActors(Server);
			const int32 ClientAActorCount = CountFragmentActors(Clients[0]);
			const int32 ClientBActorCount = CountFragmentActors(Clients[1]);
			MaxServerActorCount = FMath::Max(MaxServerActorCount, ServerActorCount);
			MaxClientAActorCount = FMath::Max(MaxClientAActorCount, ClientAActorCount);
			MaxClientBActorCount = FMath::Max(MaxClientBActorCount, ClientBActorCount);
			if (!bComparedPayloads)
			{
				if (ServerFragments.Num() == 0 || ClientAFragments.Num() != ServerFragments.Num() || ClientBFragments.Num() != ServerFragments.Num())
				{
					const AFragment2DSourceActor* ServerSource = FindSource(Server, TestSourceId);
					const AFragment2DSourceActor* ClientASource = FindSource(Clients[0], TestSourceId);
					const AFragment2DSourceActor* ClientBSource = FindSource(Clients[1], TestSourceId);
					return FailOnTimeout(*FString::Printf(
						TEXT("Fragments did not replicate to both clients. ")
						TEXT("valid fragments server/clientA/clientB=%d/%d/%d; ")
						TEXT("all fragment actors=%d/%d/%d (max=%d/%d/%d); ")
						TEXT("source revision=%d/%d/%d; broken=%d/%d/%d."),
						ServerFragments.Num(),
						ClientAFragments.Num(),
						ClientBFragments.Num(),
						ServerActorCount,
						ClientAActorCount,
						ClientBActorCount,
						MaxServerActorCount,
						MaxClientAActorCount,
						MaxClientBActorCount,
						ServerSource ? ServerSource->Revision : INDEX_NONE,
						ClientASource ? ClientASource->Revision : INDEX_NONE,
						ClientBSource ? ClientBSource->Revision : INDEX_NONE,
						ServerSource ? ServerSource->bBroken : false,
						ClientASource ? ClientASource->bBroken : false,
						ClientBSource ? ClientBSource->bBroken : false));
				}
				for (int32 Index = 0; Index < ServerFragments.Num(); ++Index)
				{
					if (!PayloadGeometryEquals(ServerFragments[Index]->SpawnPayload, ClientAFragments[Index]->SpawnPayload)
						|| !PayloadGeometryEquals(ServerFragments[Index]->SpawnPayload, ClientBFragments[Index]->SpawnPayload)
						|| ServerFragments[Index]->FragmentMaterial != ClientAFragments[Index]->FragmentMaterial
						|| ServerFragments[Index]->FragmentMaterial != ClientBFragments[Index]->FragmentMaterial
						|| ClientAFragments[Index]->MeshComponent->GetMaterial(0) != ClientAFragments[Index]->FragmentMaterial
						|| ClientBFragments[Index]->MeshComponent->GetMaterial(0) != ClientBFragments[Index]->FragmentMaterial)
					{
						Test->AddError(TEXT("Clients received different fragment ids, geometry, revisions, or materials."));
						return true;
					}
				}
				AFragment2DSourceActor* ServerSource = FindSource(Server, TestSourceId);
				AFragment2DSourceActor* ClientASource = FindSource(Clients[0], TestSourceId);
				AFragment2DSourceActor* ClientBSource = FindSource(Clients[1], TestSourceId);
				if (!ServerSource || !ClientASource || !ClientBSource || !ServerSource->bBroken
					|| !ClientASource->bBroken || !ClientBSource->bBroken || !ClientASource->IsHidden()
					|| !ClientBSource->IsHidden() || ClientASource->GetActorEnableCollision()
					|| ClientBSource->GetActorEnableCollision())
				{
					return FailOnTimeout(*FString::Printf(
						TEXT("Replicated broken state did not hide and disable collision on both clients. ")
						TEXT("revision=%d/%d/%d; broken=%d/%d/%d; hidden=%d/%d/%d; ")
						TEXT("actor collision=%d/%d/%d; always relevant=%d/%d/%d; ")
						TEXT("replicates=%d/%d/%d; local role=%d/%d/%d; remote role=%d/%d/%d; ")
						TEXT("dormancy=%d/%d/%d; net startup=%d/%d/%d."),
						ServerSource ? ServerSource->Revision : INDEX_NONE,
						ClientASource ? ClientASource->Revision : INDEX_NONE,
						ClientBSource ? ClientBSource->Revision : INDEX_NONE,
						ServerSource ? ServerSource->bBroken : false,
						ClientASource ? ClientASource->bBroken : false,
						ClientBSource ? ClientBSource->bBroken : false,
						ServerSource ? ServerSource->IsHidden() : false,
						ClientASource ? ClientASource->IsHidden() : false,
						ClientBSource ? ClientBSource->IsHidden() : false,
						ServerSource ? ServerSource->GetActorEnableCollision() : false,
						ClientASource ? ClientASource->GetActorEnableCollision() : false,
						ClientBSource ? ClientBSource->GetActorEnableCollision() : false,
						ServerSource ? ServerSource->bAlwaysRelevant : false,
						ClientASource ? ClientASource->bAlwaysRelevant : false,
						ClientBSource ? ClientBSource->bAlwaysRelevant : false,
						ServerSource ? ServerSource->GetIsReplicated() : false,
						ClientASource ? ClientASource->GetIsReplicated() : false,
						ClientBSource ? ClientBSource->GetIsReplicated() : false,
						ServerSource ? static_cast<int32>(ServerSource->GetLocalRole()) : INDEX_NONE,
						ClientASource ? static_cast<int32>(ClientASource->GetLocalRole()) : INDEX_NONE,
						ClientBSource ? static_cast<int32>(ClientBSource->GetLocalRole()) : INDEX_NONE,
						ServerSource ? static_cast<int32>(ServerSource->GetRemoteRole()) : INDEX_NONE,
						ClientASource ? static_cast<int32>(ClientASource->GetRemoteRole()) : INDEX_NONE,
						ClientBSource ? static_cast<int32>(ClientBSource->GetRemoteRole()) : INDEX_NONE,
						ServerSource ? static_cast<int32>(ServerSource->NetDormancy) : INDEX_NONE,
						ClientASource ? static_cast<int32>(ClientASource->NetDormancy) : INDEX_NONE,
						ClientBSource ? static_cast<int32>(ClientBSource->NetDormancy) : INDEX_NONE,
						ServerSource ? ServerSource->bNetStartup : false,
						ClientASource ? ClientASource->bNetStartup : false,
						ClientBSource ? ClientBSource->bNetStartup : false));
				}
				MovedFragmentId = ServerFragments[0]->SpawnPayload.FragmentId;
				InitialServerLocation = ServerFragments[0]->GetActorLocation();
				ServerFragments[0]->MeshComponent->SetPhysicsLinearVelocity(FVector(200.0, 0.0, 0.0));
				ServerFragments[0]->ForceNetUpdate();
				bComparedPayloads = true;
				BeginNextPhase();
				return false;
			}

			AFragment2DActor* ServerMoved = FindFragmentById(ServerFragments, MovedFragmentId);
			AFragment2DActor* ClientAMoved = FindFragmentById(ClientAFragments, MovedFragmentId);
			AFragment2DActor* ClientBMoved = FindFragmentById(ClientBFragments, MovedFragmentId);
			if (!ServerMoved || !ClientAMoved || !ClientBMoved) return FailOnTimeout(TEXT("Moved fragment disappeared before convergence."));
			const bool bServerMoved = FVector::Distance(ServerMoved->GetActorLocation(), InitialServerLocation) > 10.0;
			const bool bConverged = FVector::Distance(ServerMoved->GetActorLocation(), ClientAMoved->GetActorLocation()) < 75.0
				&& FVector::Distance(ServerMoved->GetActorLocation(), ClientBMoved->GetActorLocation()) < 75.0;
			if (bServerMoved && bConverged) return true;
			return FailOnTimeout(TEXT("Replicated fragment movement did not converge on both clients."));
		}

	private:
		void BeginNextPhase()
		{
			PhaseStartTime = FPlatformTime::Seconds();
		}

		bool FailOnTimeout(const TCHAR* Message)
		{
			constexpr double PhaseTimeoutSeconds = 30.0;
			if (FPlatformTime::Seconds() - PhaseStartTime < PhaseTimeoutSeconds) return false;
			Test->AddError(Message);
			return true;
		}

		FAutomationTestBase* Test = nullptr;
		double PhaseStartTime = 0.0;
		bool bSpawnedSource = false;
		bool bValidatedMaterialState = false;
		bool bRequestedLogicalCombustion = false;
		bool bValidatedLogicalCombustion = false;
		bool bRequestedAggregateFelling = false;
		bool bValidatedAggregateCarrier = false;
		bool bCleanedAggregateCarrier = false;
		bool bValidatedClientAuthority = false;
		bool bRequestedAbility = false;
		bool bComparedPayloads = false;
		int32 MaxServerActorCount = 0;
		int32 MaxClientAActorCount = 0;
		int32 MaxClientBActorCount = 0;
		FGuid TestSourceId;
		FGuid AggregateCarrierId;
		TArray<FGuid> AggregateSourceIds;
		FGuid MovedFragmentId;
		FVector InitialServerLocation = FVector::ZeroVector;
	};

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxNetworkPIETest, "MatterFlux.Fragment.Network.DedicatedServerTwoClients", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxNetworkPIETest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Isolated network test map created"), FAutomationEditorCommonUtils::CreateNewMap()))
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
	AddExpectedError(
		TEXT("Rejected non-authority damage transaction"),
		EAutomationExpectedErrorFlags::Contains,
		1,
		false);
	AddExpectedError(
		TEXT("Rejected non-authority fragment damage request"),
		EAutomationExpectedErrorFlags::Contains,
		1,
		false);

	FRequestPlaySessionParams RequestParams;
	ULevelEditorPlaySettings* PlaySettings = NewObject<ULevelEditorPlaySettings>();
	PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Client);
	PlaySettings->SetRunUnderOneProcess(true);
	PlaySettings->SetPlayNumberOfClients(2);
	PlaySettings->bLaunchSeparateServer = false;
	RequestParams.EditorPlaySettings = PlaySettings;
	FAutomationEditorCommonUtils::SetPlaySessionStartToActiveViewport(RequestParams);
	// FStartPIEForAutomationCommand owns the matching RemoveFromRoot in UE 5.8.
	PlaySettings->AddToRoot();
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIEForAutomationCommand(RequestParams));
	ADD_LATENT_AUTOMATION_COMMAND(FVerifyMatterFluxNetworkPIECommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	return true;
}
