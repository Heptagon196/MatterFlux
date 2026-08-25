#include "Creatures/MatterFluxCreatureActor.h"

#include "Creatures/MatterFluxCreatureAIController.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxCharacterPhysicsInteraction.h"
#include "Game/MatterFluxCharacterMovementComponent.h"
#include "Game/MatterFluxPlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "IMatterFluxScriptRuntime.h"
#include "Magic/MatterFluxMagicProjectile.h"
#include "Material/MatterFluxBuoyancyComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

AMatterFluxCreatureActor::AMatterFluxCreatureActor(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<
		UMatterFluxCharacterMovementComponent>(
			ACharacter::CharacterMovementComponentName))
{
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(15.0f);
	SetMinNetUpdateFrequency(3.0f);
	SetNetCullDistanceSquared(FMath::Square(9000.0f));
	AIControllerClass = AMatterFluxCreatureAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	GetCapsuleComponent()->InitCapsuleSize(38.0f, 80.0f);
	GetCharacterMovement()->MaxWalkSpeed = 240.0f;
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 480.0f, 0.0f);
	GetCharacterMovement()->bUseFlatBaseForFloorChecks = true;
	GetCharacterMovement()->MaxStepHeight = 55.0f;
	GetCharacterMovement()->SetWalkableFloorAngle(52.0f);
	MatterFlux::CharacterPhysics::ConfigurePhysicsInteraction(
		*GetCharacterMovement());
	GetMesh()->SetVisibility(false);
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BuoyancyComponent = CreateDefaultSubobject<UMatterFluxBuoyancyComponent>(
		TEXT("BuoyancyComponent"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> VoxelMaterial(
		TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette"));
	UStaticMesh* CubeAsset = CubeMesh.Succeeded() ? CubeMesh.Object : nullptr;
	UMaterialInterface* VoxelMaterialAsset =
		VoxelMaterial.Succeeded() ? VoxelMaterial.Object : nullptr;
	const auto CreatePart = [this, CubeAsset, VoxelMaterialAsset](
		const FName Name,
		const FVector& Location,
		const FVector& Scale)
	{
		UStaticMeshComponent* Part =
			CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Part->SetupAttachment(GetCapsuleComponent());
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCanEverAffectNavigation(false);
		Part->SetRelativeLocation(Location);
		Part->SetRelativeScale3D(Scale);
		Part->SetCastShadow(true);
		if (CubeAsset) Part->SetStaticMesh(CubeAsset);
		if (VoxelMaterialAsset) Part->SetMaterial(0, VoxelMaterialAsset);
		return Part;
	};
	BodyVisual = CreatePart(
		TEXT("BodyVisual"), FVector(0.0f, 0.0f, 0.0f), FVector(0.7f, 0.55f, 0.85f));
	HeadVisual = CreatePart(
		TEXT("HeadVisual"), FVector(0.0f, 0.0f, 65.0f), FVector(0.62f, 0.60f, 0.52f));
	AccentVisual = CreatePart(
		TEXT("AccentVisual"), FVector(31.0f, 0.0f, 70.0f), FVector(0.10f, 0.42f, 0.12f));
}

void AMatterFluxCreatureActor::BeginPlay()
{
	Super::BeginPlay();
	ApplyDefinitionPresentation();
}

void AMatterFluxCreatureActor::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(
		AMatterFluxCreatureActor, DefinitionId, COND_InitialOnly);
	DOREPLIFETIME(AMatterFluxCreatureActor, CurrentHealth);
	DOREPLIFETIME(AMatterFluxCreatureActor, RuntimeState);
}

void AMatterFluxCreatureActor::InitializeCreature(const FName InDefinitionId)
{
	check(HasAuthority());
	DefinitionId = InDefinitionId;
	const FMatterFluxCreatureDefinition* Definition = ResolveDefinition();
	if (Definition)
	{
		CurrentHealth = Definition->MaxHealth;
		// Deferred creatures must use their authored capsule before
		// FinishSpawning places them on terrain. Waiting for BeginPlay leaves the
		// constructor's 80 cm half-height capsule embedded for short creatures.
		ApplyDefinitionRuntimeProperties(*Definition);
		FMatterFluxCreatureAIDecisionContext InitialContext;
		FString BehaviorError;
		if (!FMatterFluxCreatureBehaviorTreeEvaluator::Evaluate(
			Definition->BehaviorProgram,
			InitialContext,
			RuntimeState,
			BehaviorError))
		{
			RuntimeState = EMatterFluxCreatureRuntimeState::Passive;
		}
		if (Definition->bWaitForFirstSight)
		{
			RuntimeState = EMatterFluxCreatureRuntimeState::Passive;
		}
		// SpawnActor 后才初始化时 BeginPlay 已经错过 DefinitionId，需要
		// 立即刷新；deferred spawn 则交给随后的 BeginPlay，避免重复创建 MID。
		if (HasActorBegunPlay())
		{
			ApplyDefinitionPresentation();
		}
	}
}

const FMatterFluxCreatureDefinition*
AMatterFluxCreatureActor::ResolveDefinition() const
{
	if (DefinitionId.IsNone() || !IMatterFluxScriptRuntime::IsAvailable())
	{
		return nullptr;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	return Registry.IsValid() ? Registry->Creatures.Find(DefinitionId) : nullptr;
}

bool AMatterFluxCreatureActor::CanInteract(const APawn& Interactor) const
{
	const FMatterFluxCreatureDefinition* Definition = ResolveDefinition();
	return Definition
		&& Definition->Faction == EMatterFluxCreatureFaction::Friendly
		&& !Definition->DialogueId.IsNone()
		&& RuntimeState != EMatterFluxCreatureRuntimeState::Dead
		&& FVector::DistSquared(
			Interactor.GetActorLocation(), GetActorLocation())
			<= FMath::Square(360.0f);
}

bool AMatterFluxCreatureActor::PurchaseOfferAuthority(
	AMatterFluxPlayerState& Buyer,
	const int32 OfferIndex,
	const int32 ExpectedProgressionRevision,
	int32& OutRemainingPurchases,
	FString& OutError)
{
	OutRemainingPurchases = INDEX_NONE;
	OutError.Reset();
	if (!HasAuthority())
	{
		OutError = TEXT("shop purchases require authority");
		return false;
	}
	const FMatterFluxCreatureDefinition* Creature = ResolveDefinition();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxShopDefinition* Shop = Creature && Registry.IsValid()
		? Registry->Shops.Find(Creature->ShopId) : nullptr;
	if (!Shop || !Shop->Offers.IsValidIndex(OfferIndex))
	{
		OutError = TEXT("shop offer does not exist");
		return false;
	}

	UMatterFluxProgressionComponent* Progression = Buyer.GetProgression();
	const FName OfferKey(*FString::Printf(
		TEXT("%s.offer.%d"), *Shop->Id.ToString(), OfferIndex));
	if (!Progression || !Progression->PurchaseOfferAuthority(
		Shop->Offers[OfferIndex], OfferKey, ExpectedProgressionRevision,
		OutRemainingPurchases, OutError))
	{
		return false;
	}
	return true;
}

bool AMatterFluxCreatureActor::ApplyDamageAuthority(
	const float Damage,
	AActor* DamageSource)
{
	if (!HasAuthority()
		|| RuntimeState == EMatterFluxCreatureRuntimeState::Dead
		|| !FMath::IsFinite(Damage)
		|| Damage <= 0.0f)
	{
		return false;
	}
	const FMatterFluxCreatureDefinition* Definition = ResolveDefinition();
	if (!Definition || Definition->Faction == EMatterFluxCreatureFaction::Friendly)
	{
		return false;
	}
	CurrentHealth = FMath::Max(0.0f, CurrentHealth - Damage);
	ForceNetUpdate();
	if (CurrentHealth <= 0.0f)
	{
		HandleDeathAuthority(DamageSource);
	}
	return true;
}

bool AMatterFluxCreatureActor::CastConfiguredSpellAuthority(
	AActor& Target,
	const bool bUseSkill,
	const int32 EventSeed)
{
	if (!HasAuthority() || bCastSequenceActive) return false;
	const FMatterFluxCreatureDefinition* Creature = ResolveDefinition();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Creature || !Registry.IsValid()) return false;
	const FMatterFluxCreatureCastProgramDefinition& Program = bUseSkill
		? Creature->SkillProgram : Creature->AttackProgram;
	const FMatterFluxSpellDefinition* Spell =
		Registry->Spells.Find(Program.SpellId);
	if (!Spell
		|| (Spell->Kind != EMatterFluxSpellKind::Projectile
			&& Spell->Kind != EMatterFluxSpellKind::Trigger))
	{
		return false;
	}

	FVector Aim = Target.GetActorLocation() - GetActorLocation();
	Aim.Z = 0.0f;
	Aim = Aim.GetSafeNormal(UE_SMALL_NUMBER, GetActorForwardVector());
	TArray<FMatterFluxCreatureCastShot> Shots;
	FString Error;
	if (!FMatterFluxCreatureCastPlanner::Build(
		Program, Aim, EventSeed, Shots, Error)
		|| Shots.IsEmpty())
	{
		return false;
	}
	if (Program.HorizontalImpulse > 0.0f || Program.VerticalImpulse > 0.0f)
	{
		LaunchCharacter(
			Aim * Program.HorizontalImpulse
				+ FVector::UpVector * Program.VerticalImpulse,
			true,
			true);
	}
	if (!SpawnCastShotAuthority(Program, Shots[0])) return false;

	const bool bHasTimedSequence = Shots.Num() > 1
		&& Program.ProjectileInterval > 0.0f;
	const bool bHasRecovery = Program.RecoverySeconds > 0.0f
		|| Program.ProjectileInterval > 0.0f;
	if (bHasTimedSequence)
	{
		bCastSequenceActive = true;
		PendingCastProgram = Program;
		PendingCastShots = MoveTemp(Shots);
		PendingCastShotIndex = 1;
		GetWorldTimerManager().SetTimer(
			PendingCastTimer,
			this,
			&AMatterFluxCreatureActor::SpawnNextPendingCastShotAuthority,
			Program.ProjectileInterval,
			true);
	}
	else if (bHasRecovery)
	{
		bCastSequenceActive = true;
		GetWorldTimerManager().SetTimer(
			CastRecoveryTimer,
			this,
			&AMatterFluxCreatureActor::FinishCastSequenceAuthority,
			Program.ProjectileInterval + Program.RecoverySeconds,
			false);
	}
	else
	{
		for (int32 Index = 1; Index < Shots.Num(); ++Index)
		{
			if (!SpawnCastShotAuthority(Program, Shots[Index])) return false;
		}
	}
	return true;
}

bool AMatterFluxCreatureActor::SpawnCastShotAuthority(
	const FMatterFluxCreatureCastProgramDefinition& Program,
	const FMatterFluxCreatureCastShot& Shot)
{
	if (!HasAuthority() || !GetWorld()) return false;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxSpellDefinition* Spell = Registry.IsValid()
		? Registry->Spells.Find(Program.SpellId) : nullptr;
	if (!Spell) return false;

	FMatterFluxMagicProjectilePlan Plan;
	Plan.SpellId = Spell->Id;
	Plan.Damage = Spell->Damage;
	Plan.Speed = Spell->Speed;
	Plan.Lifetime = Spell->Lifetime;
	Plan.Radius = Spell->Radius;
	Plan.GravityScale = Spell->GravityScale;
	Plan.BodyMaterial = Spell->BodyMaterial;
	Plan.MaterialAmount = Spell->MaterialAmount;
	Plan.bUsePlaneVisual = Spell->bUsePlaneVisual;
	Plan.bUseVerticalPlaneVisual = Spell->bUseVerticalPlaneVisual;
	Plan.bOverrideColor = Program.bOverrideColor || Spell->bOverrideColor;
	Plan.Color = Program.bOverrideColor ? Program.Color : Spell->Color;
	const FTransform Transform(
		Shot.Direction.Rotation(),
		GetActorLocation() + FVector(0.0f, 0.0f, 35.0f)
			+ Shot.Direction * 70.0f);
	AMatterFluxMagicProjectile* Projectile =
		GetWorld()->SpawnActorDeferred<AMatterFluxMagicProjectile>(
			AMatterFluxMagicProjectile::StaticClass(),
			Transform,
			this,
			this,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Projectile) return false;
	Projectile->InitializeProjectile(Plan, Shot.EventSeed);
	Projectile->FinishSpawning(Transform);
	return true;
}

void AMatterFluxCreatureActor::SpawnNextPendingCastShotAuthority()
{
	if (!HasAuthority()
		|| !PendingCastShots.IsValidIndex(PendingCastShotIndex)
		|| !SpawnCastShotAuthority(
			PendingCastProgram, PendingCastShots[PendingCastShotIndex]))
	{
		FinishCastSequenceAuthority();
		return;
	}
	++PendingCastShotIndex;
	if (PendingCastShotIndex >= PendingCastShots.Num())
	{
		GetWorldTimerManager().ClearTimer(PendingCastTimer);
		GetWorldTimerManager().SetTimer(
			CastRecoveryTimer,
			this,
			&AMatterFluxCreatureActor::FinishCastSequenceAuthority,
			PendingCastProgram.ProjectileInterval
				+ PendingCastProgram.RecoverySeconds,
			false);
	}
}

void AMatterFluxCreatureActor::FinishCastSequenceAuthority()
{
	GetWorldTimerManager().ClearTimer(PendingCastTimer);
	GetWorldTimerManager().ClearTimer(CastRecoveryTimer);
	PendingCastShots.Reset();
	PendingCastShotIndex = 0;
	bCastSequenceActive = false;
}

void AMatterFluxCreatureActor::SetRuntimeStateAuthority(
	const EMatterFluxCreatureRuntimeState NewState)
{
	if (HasAuthority() && RuntimeState != EMatterFluxCreatureRuntimeState::Dead)
	{
		RuntimeState = NewState;
	}
}

void AMatterFluxCreatureActor::ApplyDefinitionPresentation()
{
	const FMatterFluxCreatureDefinition* Definition = ResolveDefinition();
	if (!Definition) return;
	ApplyDefinitionRuntimeProperties(*Definition);
	const float WidthScale = Definition->Width / 100.0f;
	const float HeightScale = Definition->Height / 100.0f;
	BodyVisual->SetRelativeScale3D(FVector(
		WidthScale * 0.72f, WidthScale * 0.62f, HeightScale * 0.62f));
	HeadVisual->SetRelativeLocation(FVector(
		0.0f, 0.0f, Definition->Height * 0.30f));
	HeadVisual->SetRelativeScale3D(FVector(
		WidthScale * 0.60f, WidthScale * 0.60f, HeightScale * 0.30f));
	AccentVisual->SetRelativeLocation(FVector(
		Definition->Width * 0.38f, 0.0f, Definition->Height * 0.31f));
	AccentVisual->SetVisibility(
		Definition->Level != EMatterFluxCreatureLevel::Normal);

	VisualMaterials.Reset();
	for (UStaticMeshComponent* Part : { BodyVisual.Get(), HeadVisual.Get(), AccentVisual.Get() })
	{
		UMaterialInterface* Base = Part
			? MatterFlux::Rendering::ResolveDynamicMaterialParent(
				Part->GetMaterial(0))
			: nullptr;
		if (!Part || !Base) continue;
		UMaterialInstanceDynamic* Material =
			UMaterialInstanceDynamic::Create(Base, this);
		Material->SetVectorParameterValue(TEXT("Color"), Definition->Color);
		Material->SetScalarParameterValue(TEXT("PixelSize"), 7.0f);
		Material->SetScalarParameterValue(TEXT("FaceContrast"), 0.78f);
		Material->SetScalarParameterValue(TEXT("Roughness"), 0.82f);
		Part->SetMaterial(0, Material);
		VisualMaterials.Add(Material);
	}
}

void AMatterFluxCreatureActor::ApplyDefinitionRuntimeProperties(
	const FMatterFluxCreatureDefinition& Definition)
{
	GetCapsuleComponent()->SetCapsuleSize(
		Definition.Width * 0.5f,
		Definition.Height * 0.5f);
	GetCharacterMovement()->MaxWalkSpeed = Definition.MoveSpeed;
	BuoyancyComponent->SetBodyDensity(Definition.Density);
}

AMatterFluxPlayerState* AMatterFluxCreatureActor::ResolveKillerPlayerState(
	AActor* DamageSource) const
{
	if (const AMatterFluxCharacter* Character =
		Cast<AMatterFluxCharacter>(DamageSource))
	{
		return Character->GetPlayerState<AMatterFluxPlayerState>();
	}
	return DamageSource
		? Cast<AMatterFluxPlayerState>(DamageSource->GetOwner())
		: nullptr;
}

void AMatterFluxCreatureActor::HandleDeathAuthority(AActor* DamageSource)
{
	if (bDeathHandled) return;
	bDeathHandled = true;
	FinishCastSequenceAuthority();
	RuntimeState = EMatterFluxCreatureRuntimeState::Dead;
	GetCharacterMovement()->StopMovementImmediately();
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (AAIController* AI = Cast<AAIController>(GetController()))
	{
		AI->StopMovement();
	}

	const FMatterFluxCreatureDefinition* Definition = ResolveDefinition();
	AMatterFluxPlayerState* Killer = ResolveKillerPlayerState(DamageSource);
	if (Killer)
	{
		if (UMatterFluxProgressionComponent* Progression = Killer->GetProgression())
		{
			FString Error;
			if (Definition && !Definition->DropItemId.IsNone()
				&& Definition->DropItemCount > 0)
			{
				Progression->AddItemAuthority(
					Definition->DropItemId, Definition->DropItemCount, Error);
			}
		}
	}

	// A kill objective describes a world fact: the target enemy died. It must
	// not disappear merely because another hostile delivered the final hit.
	// Loot ownership remains killer-specific above, while every participating
	// player's active objective receives the authoritative death event once.
	FMatterFluxQuestEvent Event;
	Event.Type = EMatterFluxQuestEventType::EnemyKilled;
	Event.SubjectId = DefinitionId;
	Event.SubjectLevel = Definition
		? static_cast<int32>(Definition->Level) : INDEX_NONE;
	TSet<UMatterFluxProgressionComponent*> QuestRecipients;
	if (const AGameStateBase* GameState = GetWorld()
		? GetWorld()->GetGameState() : nullptr)
	{
		for (APlayerState* QuestPlayerState : GameState->PlayerArray)
		{
			if (AMatterFluxPlayerState* MatterFluxState =
				Cast<AMatterFluxPlayerState>(QuestPlayerState))
			{
				if (UMatterFluxProgressionComponent* Progression =
					MatterFluxState->GetProgression())
				{
					QuestRecipients.Add(Progression);
				}
			}
		}
	}
	if (Killer && Killer->GetProgression())
	{
		// Preserve standalone/unit-test worlds which do not install a GameState.
		QuestRecipients.Add(Killer->GetProgression());
	}
	for (UMatterFluxProgressionComponent* Progression : QuestRecipients)
	{
		FString Error;
		Progression->NotifyQuestEventAuthority(Event, Error);
	}
	ForceNetUpdate();
	SetLifeSpan(0.75f);
}

void AMatterFluxCreatureActor::OnRep_Definition()
{
	ApplyDefinitionPresentation();
}

void AMatterFluxCreatureActor::OnRep_Health()
{
	if (CurrentHealth <= 0.0f)
	{
		RuntimeState = EMatterFluxCreatureRuntimeState::Dead;
	}
}
