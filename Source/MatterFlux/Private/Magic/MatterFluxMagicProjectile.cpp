#include "Magic/MatterFluxMagicProjectile.h"

#include "Components/SphereComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxPlayerState.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GAS/MatterFluxPlayerAttributeSet.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "IMatterFluxScriptRuntime.h"
#include "Misc/Crc.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"
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

	MaterialBody = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
		TEXT("MaterialBody"));
	MaterialBody->SetupAttachment(Collision);
	MaterialBody->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MaterialBody->SetCastShadow(false);
	MaterialBody->SetVisibility(false);
	if (VoxelMesh.Succeeded())
	{
		MaterialBody->SetStaticMesh(VoxelMesh.Object);
	}
	if (VoxelMaterial.Succeeded())
	{
		MaterialBody->SetMaterial(0, VoxelMaterial.Object);
	}

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(
		TEXT("ProjectileMovement"));
	ProjectileMovement->UpdatedComponent = Collision;
	ProjectileMovement->InitialSpeed = 1000.0f;
	ProjectileMovement->MaxSpeed = 1000.0f;
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	ProjectileMovement->bSweepCollision = true;
	ProjectileMovement->bForceSubStepping = true;
	ProjectileMovement->MaxSimulationTimeStep = 0.025f;
	ProjectileMovement->MaxSimulationIterations = 8;
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->bShouldBounce = false;
	ProjectileMovement->OnProjectileStop.AddDynamic(
		this,
		&AMatterFluxMagicProjectile::OnProjectileStopped);
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
	MaterialSweepOriginLocation = GetActorLocation();
	PreviousMaterialSweepLocation = GetActorLocation();
	bHasPreviousMaterialSweepLocation = true;
	// Every projectile is a moving particle in the material world, even when
	// it does not carry a body material of its own. Authority keeps the sweep
	// active so sand, liquid, and other simulated entities can block it.
	SetActorTickEnabled(HasAuthority());
}

void AMatterFluxMagicProjectile::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || bImpactHandled)
	{
		return;
	}

	const FVector CurrentLocation = GetActorLocation();
	if (bHasPreviousMaterialSweepLocation
		&& !CurrentLocation.Equals(
			PreviousMaterialSweepLocation,
			UE_SMALL_NUMBER))
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				FVector ImpactLocation;
				FName ContactMaterial;
				if (!It->SweepSimulatedMaterial(
					PreviousMaterialSweepLocation,
					CurrentLocation,
					FMath::Clamp(Presentation.Radius, 2.0f, 100.0f),
					ImpactLocation,
					ContactMaterial))
				{
					continue;
				}
				FHitResult MaterialHit;
				MaterialHit.Location = ImpactLocation;
				MaterialHit.ImpactPoint = ImpactLocation;
				MaterialHit.TraceStart = PreviousMaterialSweepLocation;
				MaterialHit.TraceEnd = CurrentLocation;
				ResolveImpactAuthority(MaterialHit);
				return;
			}
		}
	}
	PreviousMaterialSweepLocation = CurrentLocation;
	bHasPreviousMaterialSweepLocation = true;

	if (Presentation.OrbitRadius <= UE_SMALL_NUMBER || !ProjectileMovement)
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
		ReleaseMaterialBodyAtWorldLocation(GetActorLocation());
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
	Presentation.GravityScale = Plan.GravityScale;
	Presentation.bOverrideColor = Plan.bOverrideColor;
	Presentation.Color = Plan.Color;
	Presentation.OrbitRadius = Plan.OrbitRadius;
	Presentation.BodyMaterial = Plan.BodyMaterial;
	Presentation.MaterialAmount = Plan.MaterialAmount;
	Presentation.bUsePlaneVisual = Plan.bUsePlaneVisual;
	Presentation.bUseVerticalPlaneVisual = Plan.bUseVerticalPlaneVisual;
	ProjectileMovement->ProjectileGravityScale = FMath::Clamp(
		Plan.GravityScale,
		0.0f,
		4.0f);
	MaterialSweepOriginLocation = GetActorLocation();
	PreviousMaterialSweepLocation = GetActorLocation();
	bHasPreviousMaterialSweepLocation = true;
	// InitializeProjectile is also used by deferred/editor-world spawns where
	// BeginPlay has not run yet. Arm the authority sweep immediately so the
	// first movement segment cannot escape material contact testing.
	SetActorTickEnabled(true);
	// Deferred-spawned actors may not have entered BeginPlay yet (notably in
	// editor worlds), but their immutable material body is already complete.
	BuildMaterialBodyPresentation(
		FMath::Clamp(Presentation.Radius, 2.0f, 100.0f));
}

void AMatterFluxMagicProjectile::ApplyPresentation()
{
	const float Radius = FMath::Clamp(Presentation.Radius, 2.0f, 100.0f);
	Collision->SetSphereRadius(Radius);
	const bool bUsesMaterialBody = !Presentation.BodyMaterial.IsNone();
	Visual->SetVisibility(!bUsesMaterialBody);
	MaterialBody->SetVisibility(bUsesMaterialBody);
	Visual->SetRelativeScale3D(Presentation.bUsePlaneVisual
		? Presentation.bUseVerticalPlaneVisual
			? FVector(
				Radius / 50.0f,
				0.02f,
				Radius / 50.0f)
			: FVector(
				Radius / 50.0f,
				Radius / 50.0f,
				0.02f)
		: FVector(
			Radius / 50.0f,
			FMath::Max(0.08f, Radius / 80.0f),
			Radius / 50.0f));
	ProjectileMovement->InitialSpeed = Presentation.Speed;
	ProjectileMovement->MaxSpeed = Presentation.Speed;
	ProjectileMovement->ProjectileGravityScale = FMath::Clamp(
		Presentation.GravityScale,
		0.0f,
		4.0f);
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
		FLinearColor MaterialColor = FLinearColor::White;
		bool bHasMaterialColor = false;
		const FName PresentationMaterial = Presentation.BodyMaterial;
		if (!PresentationMaterial.IsNone()
			&& IMatterFluxScriptRuntime::IsAvailable())
		{
			const FMatterFluxContentRegistryPtr Registry =
				IMatterFluxScriptRuntime::Get().GetActiveRegistry();
			if (Registry.IsValid())
			{
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(PresentationMaterial))
				{
					MaterialColor = Material->Color;
					bHasMaterialColor = true;
				}
			}
		}
		const FLinearColor Color = Presentation.bOverrideColor
			? Presentation.Color
			: bHasMaterialColor
				? MaterialColor
				: FLinearColor(
						0.25f
							+ static_cast<float>(Hash & 0xff) / 510.0f,
						0.45f
							+ static_cast<float>((Hash >> 8) & 0xff) / 510.0f,
						0.70f
							+ static_cast<float>((Hash >> 16) & 0xff) / 850.0f);
		VisualMaterial->SetVectorParameterValue(TEXT("Color"), Color);
		VisualMaterial->SetScalarParameterValue(TEXT("PixelSize"), 5.0f);
		VisualMaterial->SetScalarParameterValue(TEXT("FaceContrast"), 0.72f);
		VisualMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.60f);
		if (Visual->GetMaterial(0) != VisualMaterial)
		{
			Visual->SetMaterial(0, VisualMaterial);
		}
		if (MaterialBody->GetMaterial(0) != VisualMaterial)
		{
			MaterialBody->SetMaterial(0, VisualMaterial);
		}
	}
	BuildMaterialBodyPresentation(Radius);
}

void AMatterFluxMagicProjectile::BuildMaterialBodyPresentation(
	const float Radius)
{
	MaterialBody->ClearInstances();
	if (Presentation.BodyMaterial.IsNone())
	{
		return;
	}

	// Keep one compact deterministic material body per projectile. The body is
	// an in-flight projection; collision is still owned by the single sphere,
	// and collision or lifetime expiry hands its conserved payload back to the
	// falling-sand world.
	const float Spacing = FMath::Max(5.0f, Radius * 0.45f);
	const float BodyRadius = Radius * 0.92f;
	const int32 StepCount = FMath::Max(
		1,
		FMath::CeilToInt(BodyRadius / Spacing));
	const float VoxelScale = Spacing * 0.72f / 100.0f;
	struct FBodyVoxel
	{
		FVector Position = FVector::ZeroVector;
		float DistanceSquared = 0.0f;
	};
	TArray<FBodyVoxel> Voxels;
	for (int32 X = -StepCount; X <= StepCount; ++X)
	{
		for (int32 Y = -StepCount; Y <= StepCount; ++Y)
		{
			for (int32 Z = -StepCount; Z <= StepCount; ++Z)
			{
				const FVector LocalPosition = FVector(X, Y, Z) * Spacing;
				if (LocalPosition.SizeSquared()
					> FMath::Square(BodyRadius))
				{
					continue;
				}
				Voxels.Add({
					LocalPosition,
					static_cast<float>(LocalPosition.SizeSquared()) });
			}
		}
	}
	Voxels.Sort([](const FBodyVoxel& Left, const FBodyVoxel& Right)
	{
		if (!FMath::IsNearlyEqual(
			Left.DistanceSquared, Right.DistanceSquared))
		{
			return Left.DistanceSquared < Right.DistanceSquared;
		}
		if (!FMath::IsNearlyEqual(Left.Position.Z, Right.Position.Z))
		{
			return Left.Position.Z < Right.Position.Z;
		}
		if (!FMath::IsNearlyEqual(Left.Position.Y, Right.Position.Y))
		{
			return Left.Position.Y < Right.Position.Y;
		}
		return Left.Position.X < Right.Position.X;
	});
	// One presentation cube stands for several conserved powder/liquid units.
	// Small spray payloads therefore no longer masquerade as large material balls.
	const int32 MaximumVisibleVoxels = FMath::Clamp(
		FMath::CeilToInt(
			static_cast<float>(FMath::Max(Presentation.MaterialAmount, 1)) / 8.0f),
		1,
		96);
	for (int32 Index = 0;
		Index < FMath::Min(Voxels.Num(), MaximumVisibleVoxels);
		++Index)
	{
		MaterialBody->AddInstance(FTransform(
			FQuat::Identity,
			Voxels[Index].Position,
			FVector(VoxelScale)));
	}
}

int32 AMatterFluxMagicProjectile::GetMaterialBodyVoxelCount() const
{
	return MaterialBody ? MaterialBody->GetInstanceCount() : 0;
}

void AMatterFluxMagicProjectile::ReleaseMaterialBodyAtWorldLocation(
	const FVector& WorldLocation,
	AActor* ImpactActor)
{
	if (ServerPlan.BodyMaterial.IsNone())
	{
		return;
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			It->DepositSimulatedMaterialFromImpact(
				WorldLocation,
				ServerPlan.BodyMaterial,
				ServerPlan.MaterialAmount,
				ImpactActor,
				Presentation.Radius);
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
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (Subsystem
		&& ServerPlan.bUsePlaneVisual
		&& ServerPlan.Damage > 0.0f)
	{
		FFragmentWorldCutRequest Request;
		Request.CutShape = BuildImpactCutShape(
			ServerPlan,
			GetActorForwardVector(),
			Hit.ImpactPoint);
		Request.DamagePower = ServerPlan.Damage * 100.0f;
		Request.EventSeed = ServerEventSeed;
		Request.TargetPadding = ServerPlan.Radius;
		Subsystem->RequestWorldCut(Request);
	}

	AActor* ImpactActor = Hit.GetActor();
	if (!IsValid(ImpactActor))
	{
		if (const UPrimitiveComponent* ImpactComponent = Hit.GetComponent())
		{
			ImpactActor = ImpactComponent->GetOwner();
		}
	}
	ReleaseMaterialBodyAtWorldLocation(Hit.ImpactPoint, ImpactActor);
}

FFragmentDamageShape AMatterFluxMagicProjectile::BuildImpactCutShape(
	const FMatterFluxMagicProjectilePlan& Plan,
	const FVector& ProjectileForward,
	const FVector& ImpactPoint)
{
	// Keep a conservative query width here; the canonical material operation
	// below is rasterized as one cell in each target Source's own resolution.
	constexpr float SingleLineQueryThickness = 10.0f;
	if (!Plan.bUsePlaneVisual)
	{
		FFragmentDamageShape Shape;
		Shape.Type = EFragmentDamageShapeType::Circle;
		Shape.WorldTransform = FTransform(ImpactPoint);
		Shape.Radius = FMath::Max(2.0f, Plan.Radius);
		return Shape;
	}

	FVector PlanarForward = ProjectileForward;
	PlanarForward.Z = 0.0f;
	if (!PlanarForward.Normalize())
	{
		PlanarForward = FVector::ForwardVector;
	}
	FVector CutRight = FVector::CrossProduct(
		FVector::UpVector,
		PlanarForward).GetSafeNormal();
	if (CutRight.IsNearlyZero())
	{
		CutRight = FVector::RightVector;
	}

	// A plane is only the projectile presentation. The canonical material
	// operation is its one-cell intersection line: horizontal cuts run across
	// world right, vertical cuts run along world up. Circle used to erase a
	// full radius-sized disk and also ignored distance along its plane normal.
	const FVector LineDirection = Plan.bUseVerticalPlaneVisual
		? FVector::UpVector
		: CutRight;
	const FVector ThicknessDirection = Plan.bUseVerticalPlaneVisual
		? CutRight
		: FVector::UpVector;
	FFragmentDamageShape Shape;
	Shape.Type = EFragmentDamageShapeType::Line;
	Shape.WorldTransform = FTransform(
		FRotationMatrix::MakeFromXZ(
			LineDirection,
			ThicknessDirection).ToQuat(),
		ImpactPoint);
	Shape.Extents.X = FMath::Max(
		SingleLineQueryThickness,
		Plan.Radius * 2.0f);
	Shape.Thickness = SingleLineQueryThickness;
	Shape.bSingleCellLine = true;
	return Shape;
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

void AMatterFluxMagicProjectile::OnProjectileStopped(
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
	FHitResult ResolvedHit = Hit;
	AFragment2DSourceActor* FixedSourceHit = nullptr;
	if (!ServerPlan.BodyMaterial.IsNone())
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
			{
				FVector FixedImpactLocation;
				FVector FixedImpactNormal;
				if (!It->SweepFixedFragmentSource(
						MaterialSweepOriginLocation,
						Hit.ImpactPoint,
						FMath::Clamp(Presentation.Radius, 2.0f, 100.0f),
						FixedImpactLocation,
						FixedImpactNormal,
						FixedSourceHit))
				{
					continue;
				}
				ResolvedHit.Location = FixedImpactLocation;
				ResolvedHit.ImpactPoint = FixedImpactLocation;
				ResolvedHit.Normal = FixedImpactNormal;
				ResolvedHit.ImpactNormal = FixedImpactNormal;
				ResolvedHit.Component = FixedSourceHit->MeshComponent.Get();
				ResolvedHit.HitObjectHandle = FActorInstanceHandle(
					FixedSourceHit,
					FixedSourceHit->MeshComponent.Get(),
					INDEX_NONE);
				break;
			}
		}
	}
	bImpactHandled = true;
	AActor* HitActor = FixedSourceHit
		? FixedSourceHit
		: ResolvedHit.GetActor();
	if (AMatterFluxCreatureActor* Creature =
		Cast<AMatterFluxCreatureActor>(HitActor))
	{
		Creature->ApplyDamageAuthority(ServerPlan.Damage, GetOwner());
	}
	else if (AMatterFluxCharacter* Character =
		Cast<AMatterFluxCharacter>(HitActor))
	{
		// Creature projectiles damage players; player-owned projectiles cannot
		// damage their owner through a trigger payload.
		if (Cast<AMatterFluxCreatureActor>(GetOwner()))
		{
			if (AMatterFluxPlayerState* PlayerState =
				Character->GetPlayerState<AMatterFluxPlayerState>())
			{
				if (UMatterFluxPlayerAttributeSet* Attributes =
					PlayerState->GetPlayerAttributes())
				{
					Attributes->SetHealth(FMath::Max(
						0.0f, Attributes->GetHealth() - ServerPlan.Damage));
				}
			}
		}
	}
	ApplyWorldImpact(ResolvedHit);
	SpawnTriggerPayload(
		ServerPlan.OnImpactProjectiles,
		ResolvedHit.ImpactPoint,
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
