#include "Magic/MatterFluxMagicProjectile.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Crc.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AMatterFluxMagicProjectile::AMatterFluxMagicProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30.0f);
	SetMinNetUpdateFrequency(10.0f);
	SetNetCullDistanceSquared(FMath::Square(12000.0f));

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetRootComponent(Collision);
	Collision->InitSphereRadius(8.0f);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Collision->SetCollisionResponseToAllChannels(ECR_Block);
	Collision->SetNotifyRigidBodyCollision(true);
	Collision->OnComponentHit.AddDynamic(
		this,
		&AMatterFluxMagicProjectile::OnProjectileHit);

	Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Visual"));
	Visual->SetupAttachment(Collision);
	Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Visual->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> VoxelMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> VoxelMaterial(
		TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette"));
	if (VoxelMesh.Succeeded())
	{
		Visual->SetStaticMesh(VoxelMesh.Object);
	}
	if (VoxelMaterial.Succeeded())
	{
		Visual->SetMaterial(0, VoxelMaterial.Object);
	}

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(
		TEXT("ProjectileMovement"));
	ProjectileMovement->UpdatedComponent = Collision;
	ProjectileMovement->InitialSpeed = 1000.0f;
	ProjectileMovement->MaxSpeed = 1000.0f;
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->bShouldBounce = false;
}

void AMatterFluxMagicProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (AActor* OwnerActor = GetOwner())
	{
		Collision->IgnoreActorWhenMoving(OwnerActor, true);
	}
	if (APawn* InstigatorPawn = GetInstigator())
	{
		Collision->IgnoreActorWhenMoving(InstigatorPawn, true);
	}
	ApplyPresentation();
	SetActorTickEnabled(
		HasAuthority() && Presentation.OrbitRadius > UE_SMALL_NUMBER);
}

void AMatterFluxMagicProjectile::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority()
		|| Presentation.OrbitRadius <= UE_SMALL_NUMBER
		|| !ProjectileMovement)
	{
		return;
	}
	const FVector Direction = ProjectileMovement->Velocity.GetSafeNormal(
		UE_SMALL_NUMBER,
		GetActorForwardVector());
	if (!bOrbitInitialized)
	{
		const FVector CenterDirection = FVector::CrossProduct(
			FVector::UpVector,
			Direction).GetSafeNormal();
		OrbitCenter = GetActorLocation()
			+ CenterDirection * Presentation.OrbitRadius;
		bOrbitInitialized = true;
	}
	const FVector Radial = (GetActorLocation() - OrbitCenter)
		.GetSafeNormal(UE_SMALL_NUMBER, FVector::RightVector);
	const FVector Tangent = FVector::CrossProduct(
		Radial,
		FVector::UpVector).GetSafeNormal(
			UE_SMALL_NUMBER,
			Direction);
	ProjectileMovement->Velocity = Tangent * Presentation.Speed;
}

void AMatterFluxMagicProjectile::LifeSpanExpired()
{
	if (HasAuthority() && !bImpactHandled)
	{
		bImpactHandled = true;
		SpawnTriggerPayload(
			ServerPlan.OnExpireProjectiles,
			GetActorLocation(),
			GetVelocity().GetSafeNormal(
				UE_SMALL_NUMBER,
				GetActorForwardVector()),
			ServerPlan.bTriggerRandomDirection);
	}
	Super::LifeSpanExpired();
}

void AMatterFluxMagicProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(
		AMatterFluxMagicProjectile,
		Presentation,
		COND_InitialOnly);
}

void AMatterFluxMagicProjectile::InitializeProjectile(
	const FMatterFluxMagicProjectilePlan& Plan,
	const int32 EventSeed)
{
	check(HasAuthority());
	ServerPlan = Plan;
	ServerEventSeed = EventSeed;
	Presentation.SpellId = Plan.SpellId;
	Presentation.Speed = Plan.Speed;
	Presentation.Lifetime = Plan.Lifetime;
	Presentation.Radius = Plan.Radius;
	Presentation.bOverrideColor = Plan.bOverrideColor;
	Presentation.Color = Plan.Color;
	Presentation.OrbitRadius = Plan.OrbitRadius;
	Presentation.ImpactMaterial = Plan.ImpactMaterial;
}

void AMatterFluxMagicProjectile::ApplyPresentation()
{
	const float Radius = FMath::Clamp(Presentation.Radius, 2.0f, 100.0f);
	Collision->SetSphereRadius(Radius);
	Visual->SetRelativeScale3D(FVector(
		Radius / 50.0f,
		FMath::Max(0.08f, Radius / 80.0f),
		Radius / 50.0f));
	ProjectileMovement->InitialSpeed = Presentation.Speed;
	ProjectileMovement->MaxSpeed = Presentation.Speed;
	ProjectileMovement->Velocity =
		GetActorForwardVector() * Presentation.Speed;
	SetLifeSpan(FMath::Clamp(Presentation.Lifetime, 0.05f, 30.0f));

	if (!VisualMaterial)
	{
		UMaterialInterface* BaseMaterial = Visual->GetMaterial(0);
		VisualMaterial = Cast<UMaterialInstanceDynamic>(BaseMaterial);
		if (!VisualMaterial && BaseMaterial)
		{
			VisualMaterial = UMaterialInstanceDynamic::Create(
				BaseMaterial,
				this);
		}
	}
	if (VisualMaterial)
	{
		const uint32 Hash = FCrc::StrCrc32(
			*Presentation.SpellId.ToString());
		const FLinearColor Color = Presentation.bOverrideColor
			? Presentation.Color
			: Presentation.ImpactMaterial == TEXT("fire")
				? FLinearColor(1.0f, 0.12f, 0.015f)
				: FLinearColor(
				0.25f + static_cast<float>(Hash & 0xff) / 510.0f,
				0.45f + static_cast<float>((Hash >> 8) & 0xff) / 510.0f,
				0.70f + static_cast<float>((Hash >> 16) & 0xff) / 850.0f);
		VisualMaterial->SetVectorParameterValue(TEXT("Color"), Color);
		VisualMaterial->SetScalarParameterValue(TEXT("PixelSize"), 5.0f);
		VisualMaterial->SetScalarParameterValue(TEXT("FaceContrast"), 0.72f);
		VisualMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.60f);
		if (Visual->GetMaterial(0) != VisualMaterial)
		{
			Visual->SetMaterial(0, VisualMaterial);
		}
	}
}

void AMatterFluxMagicProjectile::ApplyWorldImpact(const FHitResult& Hit)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FFragmentWorldCutRequest Request;
	Request.CutShape.Type = EFragmentDamageShapeType::Circle;
	Request.CutShape.WorldTransform = FTransform(Hit.ImpactPoint);
	Request.CutShape.Radius = FMath::Max(2.0f, ServerPlan.Radius);
	Request.DamagePower = FMath::Max(0.0f, ServerPlan.Damage * 100.0f);
	Request.EventSeed = ServerEventSeed;
	Request.TargetPadding = ServerPlan.Radius;
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (Subsystem)
	{
		Subsystem->RequestWorldCut(Request);
	}

	if (ServerPlan.ImpactMaterial == TEXT("fire"))
	{
		const FBox IgnitionBounds(
			Hit.ImpactPoint - FVector(ServerPlan.Radius),
			Hit.ImpactPoint + FVector(ServerPlan.Radius));
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World);
			It;
			++It)
		{
			It->IgniteLogicalFragmentSourcesInBounds(
				IgnitionBounds,
				Hit.ImpactPoint,
				TEXT("fire"),
				ServerEventSeed);
		}
		TArray<AFragment2DSourceActor*> CandidateSources;
		if (Subsystem)
		{
			Subsystem->GatherSourcesInBounds(
				IgnitionBounds,
				CandidateSources);
		}
		for (AFragment2DSourceActor* Source : CandidateSources)
		{
			if (IsValid(Source)
				&& Source->HasCombustionRule()
				&& Source->GetComponentsBoundingBox(true)
					.ExpandBy(ServerPlan.Radius)
					.IsInsideOrOn(Hit.ImpactPoint))
			{
				Source->IgniteAtWorldLocation(
					Hit.ImpactPoint,
					TEXT("fire"),
					ServerEventSeed);
			}
		}
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World);
			It;
			++It)
		{
			It->IgniteGroundAtWorldLocation(
				Hit.ImpactPoint,
				ServerEventSeed);
		}
	}
}

void AMatterFluxMagicProjectile::SpawnTriggerPayload(
	const TConstArrayView<FMatterFluxMagicProjectilePlan> Payload,
	const FVector& Origin,
	const FVector& ParentDirection,
	const bool bRandomDirection)
{
	UWorld* World = GetWorld();
	if (!World || Payload.IsEmpty())
	{
		return;
	}
	for (int32 Index = 0;
		Index < Payload.Num();
		++Index)
	{
		const FMatterFluxMagicProjectilePlan& ChildPlan =
			Payload[Index];
		float AngleDegrees = ChildPlan.SpawnAngleDegrees;
		if (bRandomDirection)
		{
			FRandomStream Random(
				ServerEventSeed ^ (Index * 0x2f91 + 0x6c31));
			AngleDegrees += Random.FRandRange(-180.0f, 180.0f);
		}
		const FVector Direction = ParentDirection.RotateAngleAxis(
			AngleDegrees,
			FVector::UpVector);
		const FTransform Transform(
			Direction.Rotation(),
			Origin + Direction * (ChildPlan.Radius + 4.0f));
		AMatterFluxMagicProjectile* Child =
			World->SpawnActorDeferred<AMatterFluxMagicProjectile>(
				StaticClass(),
				Transform,
				GetOwner(),
				GetInstigator(),
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Child)
		{
			Child->InitializeProjectile(
				ChildPlan,
				ServerEventSeed ^ (Index + 0x51f2));
			UGameplayStatics::FinishSpawningActor(Child, Transform);
		}
	}
}

void AMatterFluxMagicProjectile::OnProjectileHit(
	UPrimitiveComponent* HitComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	FVector NormalImpulse,
	const FHitResult& Hit)
{
	ResolveImpactAuthority(Hit);
}

bool AMatterFluxMagicProjectile::ResolveImpactAuthority(
	const FHitResult& Hit)
{
	if (!HasAuthority() || bImpactHandled)
	{
		return false;
	}
	bImpactHandled = true;
	ApplyWorldImpact(Hit);
	SpawnTriggerPayload(
		ServerPlan.OnImpactProjectiles,
		Hit.ImpactPoint,
		GetVelocity().GetSafeNormal(
			UE_SMALL_NUMBER,
			GetActorForwardVector()),
		ServerPlan.bTriggerRandomDirection);
	Destroy();
	return true;
}

void AMatterFluxMagicProjectile::OnRep_Presentation()
{
	ApplyPresentation();
}
