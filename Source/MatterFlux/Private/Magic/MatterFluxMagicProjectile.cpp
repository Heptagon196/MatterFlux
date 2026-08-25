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
#include "Rendering/MatterFluxInstanceVisuals.h"
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
	if (HasAuthority() && !Presentation.BodyMaterial.IsNone())
	{
		TryBeginAirborneMaterialProjection();
	}
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
	if (!Presentation.BodyMaterial.IsNone()
		&& !bProjectsAirborneMaterialParticles
		&& TryBeginAirborneMaterialProjection())
	{
		return;
	}
	if (bProjectsAirborneMaterialParticles)
	{
		AirborneMaterialProjectionAccumulator += DeltaSeconds;
		const float ProjectionInterval = Presentation.MaterialAmount > 256
			? 1.0f / 30.0f
			: 0.0f;
		if (ProjectionInterval > 0.0f
			&& AirborneMaterialProjectionAccumulator < ProjectionInterval)
		{
			return;
		}
		AirborneMaterialProjectionAccumulator = 0.0f;
		RefreshAirborneMaterialProjection();
		return;
	}
	if (bProgressiveMaterialRelease)
	{
		TickProgressiveMaterialRelease(DeltaSeconds);
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
	if (bProjectsAirborneMaterialParticles)
	{
		SetLifeSpan(0.0f);
		return;
	}
	if (HasAuthority() && !Presentation.BodyMaterial.IsNone())
	{
		if (!TryBeginAirborneMaterialProjection())
		{
			SetLifeSpan(0.05f);
		}
		return;
	}
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
	DOREPLIFETIME(
		AMatterFluxMagicProjectile,
		MaterialBodyReleasedVoxelCount);
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
	if (HasActorBegunPlay() && !Presentation.BodyMaterial.IsNone())
	{
		TryBeginAirborneMaterialProjection();
	}
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
	MaterialBodyVoxelPositions.Reset();
	MaterialBodyVoxelSpacing = 0.0f;
	if (Presentation.BodyMaterial.IsNone())
	{
		MaterialBody->ClearInstances();
		return;
	}

	// One visible point is one canonical material particle. A deterministic
	// low-discrepancy volume fill keeps both five-particle sprays and the 2048
	// particle sand sphere free of lattice spikes or hidden aggregate payloads.
	const int32 ParticleCount = FMath::Clamp(
		Presentation.MaterialAmount,
		1,
		4096);
	const float BodyRadius = Radius * 0.92f;
	MaterialBodyVoxelSpacing = FMath::Clamp(
		BodyRadius * 1.45f
			/ FMath::Max(FMath::Pow(static_cast<float>(ParticleCount), 1.0f / 3.0f), 1.0f),
		3.0f,
		12.0f);
	MaterialBodyVoxelPositions.Reserve(ParticleCount);
	constexpr double GoldenRatioConjugate = 0.6180339887498948482;
	constexpr double PlasticConjugate = 0.7548776662466927600;
	for (int32 Index = 0; Index < ParticleCount; ++Index)
	{
		const double RadialFraction =
			(static_cast<double>(Index) + 0.5) / ParticleCount;
		const double RadiusFraction = FMath::Pow(RadialFraction, 1.0 / 3.0);
		const double Z = 1.0 - 2.0 * FMath::Frac(
			(static_cast<double>(Index) + 0.5) * GoldenRatioConjugate);
		const double Azimuth = 2.0 * PI * FMath::Frac(
			(static_cast<double>(Index) + 0.5) * PlasticConjugate);
		const double Ring = FMath::Sqrt(FMath::Max(1.0 - Z * Z, 0.0));
		MaterialBodyVoxelPositions.Add(
			FVector(
				Ring * FMath::Cos(Azimuth),
				Ring * FMath::Sin(Azimuth),
				Z)
			* BodyRadius * RadiusFraction);
	}
	RefreshMaterialBodyInstances();
}

void AMatterFluxMagicProjectile::RefreshMaterialBodyInstances()
{
	if (!MaterialBody)
	{
		return;
	}
	TArray<FTransform> Transforms;
	Transforms.Reserve(
		MaterialBodyVoxelPositions.Num() + FallingMaterialPackets.Num());
	const float VoxelScale = MaterialBodyVoxelSpacing * 0.72f / 100.0f;
	const int32 FirstBodyVoxel = bGranularMaterialFall
		? MaterialBodyVoxelPositions.Num()
		: MaterialBodyReleasedVoxelCount;
	for (int32 Index = FirstBodyVoxel;
		Index < MaterialBodyVoxelPositions.Num();
		++Index)
	{
		Transforms.Add(FTransform(
			FQuat::Identity,
			MaterialBodyVoxelPositions[Index],
			FVector(VoxelScale)));
	}
	const float GrainScale = FMath::Clamp(
		MaterialBodyVoxelSpacing * 0.18f,
		4.0f,
		7.0f) / 100.0f;
	const FTransform ActorTransform = GetActorTransform();
	for (const FFallingMaterialPacket& Packet : FallingMaterialPackets)
	{
		Transforms.Add(FTransform(
			FQuat::Identity,
			ActorTransform.InverseTransformPosition(Packet.WorldPosition),
			FVector(GrainScale * Packet.VisualScale)));
	}
	MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
		*MaterialBody,
		Transforms);
}

bool AMatterFluxMagicProjectile::TryBeginAirborneMaterialProjection()
{
	if (!HasAuthority()
		|| bProjectsAirborneMaterialParticles
		|| Presentation.BodyMaterial.IsNone()
		|| MaterialBodyVoxelPositions.IsEmpty())
	{
		return bProjectsAirborneMaterialParticles;
	}
	AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			PlayableWorld = *It;
			break;
		}
	}
	if (!PlayableWorld)
	{
		return false;
	}

	TArray<FVector> WorldPositions;
	TArray<FVector> InitialVelocities;
	WorldPositions.Reserve(MaterialBodyVoxelPositions.Num());
	InitialVelocities.Reserve(MaterialBodyVoxelPositions.Num());
	const FTransform ProjectionTransform = GetActorTransform();
	const FVector BaseVelocity = ProjectileMovement
		? ProjectileMovement->Velocity
		: GetActorForwardVector() * Presentation.Speed;
	const float VelocityJitter = Presentation.Speed > UE_SMALL_NUMBER
		? FMath::Clamp(Presentation.Speed * 0.035f, 4.0f, 45.0f)
		: 3.0f;
	FRandomStream Random(ServerEventSeed ^ 0x4d415454);
	for (const FVector& LocalPosition : MaterialBodyVoxelPositions)
	{
		WorldPositions.Add(ProjectionTransform.TransformPosition(LocalPosition));
		InitialVelocities.Add(
			BaseVelocity + Random.VRand() * VelocityJitter);
	}
	MaterialParticleBatchId = PlayableWorld->SpawnAirborneSimulatedMaterial(
		Presentation.BodyMaterial,
		Presentation.MaterialAmount,
		WorldPositions,
		InitialVelocities,
		FMath::Clamp(MaterialBodyVoxelSpacing * 0.28f, 1.5f, 5.0f),
		Presentation.GravityScale,
		Presentation.Lifetime,
		ServerEventSeed);
	if (!MaterialParticleBatchId.IsValid())
	{
		return false;
	}

	bProjectsAirborneMaterialParticles = true;
	AirborneMaterialProjectionAccumulator = 0.0f;
	bProgressiveMaterialRelease = false;
	bGranularMaterialFall = false;
	FallingMaterialPackets.Reset();
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Collision->SetGenerateOverlapEvents(false);
	if (ProjectileMovement)
	{
		ProjectileMovement->StopMovementImmediately();
		ProjectileMovement->Deactivate();
	}
	SetLifeSpan(0.0f);
	LastMaterialParticleCenter = GetActorLocation();
	LastMaterialParticleVelocity = BaseVelocity;
	RefreshAirborneMaterialProjection();
	ForceNetUpdate();
	return true;
}

void AMatterFluxMagicProjectile::RefreshAirborneMaterialProjection()
{
	if (!bProjectsAirborneMaterialParticles || !MaterialParticleBatchId.IsValid())
	{
		return;
	}
	AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			PlayableWorld = *It;
			break;
		}
	}
	if (!PlayableWorld)
	{
		return;
	}
	TArray<MatterFlux::Material::FAirborneParticle> Particles;
	PlayableWorld->GetAirborneSimulatedMaterialParticles(
		MaterialParticleBatchId,
		Particles);
	if (Particles.IsEmpty())
	{
		FinishAirborneMaterialProjection();
		return;
	}

	FVector Center = FVector::ZeroVector;
	FVector AverageVelocity = FVector::ZeroVector;
	for (const MatterFlux::Material::FAirborneParticle& Particle : Particles)
	{
		Center += Particle.WorldPosition;
		AverageVelocity += Particle.Velocity;
	}
	Center /= static_cast<double>(Particles.Num());
	AverageVelocity /= static_cast<double>(Particles.Num());
	SetActorLocation(Center, false, nullptr, ETeleportType::TeleportPhysics);
	LastMaterialParticleCenter = Center;
	LastMaterialParticleVelocity = AverageVelocity;

	TArray<FTransform> Transforms;
	Transforms.Reserve(Particles.Num());
	const FTransform ActorTransform = GetActorTransform();
	for (const MatterFlux::Material::FAirborneParticle& Particle : Particles)
	{
		const float Scale = Particle.Radius * 1.5f / 100.0f;
		Transforms.Add(FTransform(
			FQuat::Identity,
			ActorTransform.InverseTransformPosition(Particle.WorldPosition),
			FVector(Scale)));
	}
	MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
		*MaterialBody,
		Transforms);
}

void AMatterFluxMagicProjectile::FinishAirborneMaterialProjection()
{
	if (bImpactHandled)
	{
		return;
	}
	bImpactHandled = true;
	SpawnTriggerPayload(
		ServerPlan.OnImpactProjectiles,
		LastMaterialParticleCenter,
		LastMaterialParticleVelocity.GetSafeNormal(
			UE_SMALL_NUMBER,
			GetActorForwardVector()),
		ServerPlan.bTriggerRandomDirection);
	Destroy();
}

bool AMatterFluxMagicProjectile::ShouldReleaseMaterialBodyProgressively() const
{
	// A large material-bodied projectile must not depend on the script runtime
	// still being available at impact time. Its authoritative plan already says
	// that the payload is a material body; releasing that body over multiple
	// frames preserves the visible shape until it has actually entered the
	// material simulation. Small spray particles keep their immediate handoff.
	return !ServerPlan.BodyMaterial.IsNone()
		&& ServerPlan.MaterialAmount > 64
		&& MaterialBodyVoxelPositions.Num() >= 4;
}

bool AMatterFluxMagicProjectile::ShouldBeginGranularMaterialFall() const
{
	if (!ShouldReleaseMaterialBodyProgressively()
		|| !IMatterFluxScriptRuntime::IsAvailable())
	{
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxMaterialDefinition* Material = Registry.IsValid()
		? Registry->Materials.Find(ServerPlan.BodyMaterial)
		: nullptr;
	return Material
		&& Material->Phase == EMatterFluxMaterialPhase::Powder;
}

void AMatterFluxMagicProjectile::BeginGranularMaterialFall()
{
	bProgressiveMaterialRelease = true;
	bGranularMaterialFall = true;
	ProgressiveMaterialReleaseElapsed = 0.0f;
	MaterialBodyReleasedVoxelCount = 0;
	FallingMaterialPackets.Reset();
	ProgressiveReleaseStartLocation = GetActorLocation();
	ProgressiveReleaseEndLocation = ProgressiveReleaseStartLocation;
	ProgressiveReleaseImpactPoint = ProgressiveReleaseStartLocation;
	ProgressiveReleaseDirection = GetVelocity().GetSafeNormal(
		UE_SMALL_NUMBER,
		FVector::DownVector);
	ProgressiveReleaseImpactActor.Reset();
	for (int32 VoxelIndex = 0;
		VoxelIndex < MaterialBodyVoxelPositions.Num();
		++VoxelIndex)
	{
		const int32 VoxelCount = MaterialBodyVoxelPositions.Num();
		const int32 BaseCellCount = ServerPlan.MaterialAmount / VoxelCount;
		const int32 Remainder = ServerPlan.MaterialAmount % VoxelCount;
		QueueFallingMaterialPacketsForVoxel(
			VoxelIndex,
			BaseCellCount + (VoxelIndex < Remainder ? 1 : 0));
	}
	if (FallingMaterialPackets.IsEmpty())
	{
		bProgressiveMaterialRelease = false;
		bGranularMaterialFall = false;
		return;
	}
	if (ProjectileMovement)
	{
		ProjectileMovement->StopMovementImmediately();
		ProjectileMovement->Deactivate();
	}
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Collision->SetGenerateOverlapEvents(false);
	SetLifeSpan(0.0f);
	RefreshMaterialBodyInstances();
	ForceNetUpdate();
}

void AMatterFluxMagicProjectile::BeginProgressiveMaterialRelease(
	const FHitResult& Hit,
	AActor* ImpactActor)
{
	bProgressiveMaterialRelease = true;
	ProgressiveMaterialReleaseElapsed = 0.0f;
	MaterialBodyReleasedVoxelCount = 0;
	FallingMaterialPackets.Reset();
	ProgressiveReleaseStartLocation = GetActorLocation();
	ProgressiveReleaseEndLocation = ProgressiveReleaseStartLocation
		- FVector::UpVector * FMath::Max(Presentation.Radius * 2.10f, 30.0f);
	ProgressiveReleaseImpactPoint = Hit.ImpactPoint;
	ProgressiveReleaseDirection = GetVelocity().GetSafeNormal(
		UE_SMALL_NUMBER,
		FVector::DownVector);
	ProgressiveReleaseImpactActor = ImpactActor;
	if (ProjectileMovement)
	{
		ProjectileMovement->StopMovementImmediately();
		ProjectileMovement->Deactivate();
	}
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Collision->SetGenerateOverlapEvents(false);
	SetLifeSpan(0.0f);
	ForceNetUpdate();
}

void AMatterFluxMagicProjectile::QueueFallingMaterialPacketsForVoxel(
	const int32 VoxelIndex,
	const int32 VoxelCellCount)
{
	if (!MaterialBodyVoxelPositions.IsValidIndex(VoxelIndex)
		|| VoxelCellCount <= 0)
	{
		return;
	}

	AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			PlayableWorld = *It;
			break;
		}
	}
	if (!PlayableWorld)
	{
		return;
	}

	FRandomStream Random(ServerEventSeed
		^ VoxelIndex * 0x45d9f3b
		^ VoxelCellCount * 0x27d4eb2d);
	AFragment2DSourceActor* CandidateSource =
		Cast<AFragment2DSourceActor>(ProgressiveReleaseImpactActor.Get());
	const float JitterRadius = MaterialBodyVoxelSpacing * 0.34f;
	const float GrainRadius = FMath::Clamp(
		MaterialBodyVoxelSpacing * 0.09f,
		2.0f,
		4.0f);
	const FVector LocalVoxel = MaterialBodyVoxelPositions[VoxelIndex];
	const FVector InheritedVelocity = ProjectileMovement
		? ProjectileMovement->Velocity
		: FVector::ZeroVector;
	// Keep the presentation one-to-one with the authored payload. Partial
	// visual grains used to double a 2048-unit body into 4096 instances while
	// the canonical material amount remained unchanged, so the falling body no
	// longer represented what would actually be deposited.
	const int32 SubGrainsPerCell = 1;
	const int32 PacketCount = VoxelCellCount * SubGrainsPerCell;
	const int32 BaseCellCount = VoxelCellCount / PacketCount;
	const int32 Remainder = VoxelCellCount % PacketCount;
	const int32 TotalConservedAmount = VoxelCellCount * 255;
	const int32 BaseConservedAmount = TotalConservedAmount / PacketCount;
	const int32 ConservedRemainder = TotalConservedAmount % PacketCount;
	for (int32 PacketIndex = 0; PacketIndex < PacketCount; ++PacketIndex)
	{
		const FVector LocalJitter(
			Random.FRandRange(-JitterRadius, JitterRadius),
			Random.FRandRange(-JitterRadius, JitterRadius),
			Random.FRandRange(-MaterialBodyVoxelSpacing * 0.48f,
				MaterialBodyVoxelSpacing * 0.48f));
		FVector WorldPosition = GetActorTransform().TransformPosition(
			LocalVoxel + LocalJitter);
		float LandingZ = ProgressiveReleaseImpactPoint.Z;
		PlayableWorld->TrySampleTerrainHeightAtWorldLocation(
			WorldPosition,
			LandingZ);
		AActor* PacketImpactActor = nullptr;
		if (IsValid(CandidateSource) && !CandidateSource->bBroken)
		{
			const FVector TraceStart =
				WorldPosition + FVector::UpVector * 12.0f;
			const FVector TraceEnd(
				WorldPosition.X,
				WorldPosition.Y,
				LandingZ - 12.0f);
			FVector SupportLocation;
			FVector SupportNormal;
			if (CandidateSource->SweepRuntimeMask(
				TraceStart,
				TraceEnd,
				GrainRadius,
				SupportLocation,
				SupportNormal))
			{
				LandingZ = FMath::Max(LandingZ, SupportLocation.Z);
				PacketImpactActor = CandidateSource;
			}
		}
		WorldPosition.Z = FMath::Max(
			WorldPosition.Z,
			LandingZ + GrainRadius * 2.0f);

		FFallingMaterialPacket& Packet =
			FallingMaterialPackets.AddDefaulted_GetRef();
		Packet.WorldPosition = WorldPosition;
		Packet.Velocity = bGranularMaterialFall
			? InheritedVelocity + FVector(
				Random.FRandRange(-12.0f, 12.0f),
				Random.FRandRange(-12.0f, 12.0f),
				Random.FRandRange(-9.0f, 9.0f))
			: FVector(
				Random.FRandRange(-28.0f, 28.0f),
				Random.FRandRange(-28.0f, 28.0f),
				-Random.FRandRange(35.0f, 95.0f));
		Packet.LandingZ = LandingZ + GrainRadius;
		Packet.DelaySeconds = bGranularMaterialFall
			? 0.0f
			: Random.FRandRange(0.0f, 0.12f);
		Packet.CellCount = FMath::Max(
			BaseCellCount + (PacketIndex < Remainder ? 1 : 0),
			1);
		Packet.ConservedMaterialAmount = BaseConservedAmount
			+ (PacketIndex < ConservedRemainder ? 1 : 0);
		Packet.VisualScale = FMath::Pow(
			static_cast<float>(Packet.ConservedMaterialAmount) / 255.0f,
			1.0f / 3.0f);
		Packet.VoxelIndex = VoxelIndex;
		Packet.ImpactActor = PacketImpactActor;
	}
}

void AMatterFluxMagicProjectile::AdvanceFallingMaterialPackets(
	const float DeltaSeconds)
{
	if (FallingMaterialPackets.IsEmpty())
	{
		return;
	}
	AMatterFluxPlayableWorldActor* PlayableWorld = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			PlayableWorld = *It;
			break;
		}
	}
	if (!PlayableWorld)
	{
		return;
	}

	const float StepSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 0.10f);
	// Keep the handoff into the canonical 2.5D simulation granular as well as
	// the visible fall. Without a time-based budget, many independently falling
	// grains can cross their support plane during the same frame and still turn
	// into one large, visibly stepped material update.
	const int32 MaximumDepositedCellsThisFrame = FMath::Max(
		1,
		FMath::FloorToInt(12000.0f * StepSeconds));
	int32 DepositedCellsThisFrame = 0;
	bool bReplicatedReleaseChanged = false;
	for (int32 Index = FallingMaterialPackets.Num() - 1; Index >= 0; --Index)
	{
		FFallingMaterialPacket& Packet = FallingMaterialPackets[Index];
		if (Packet.DelaySeconds > 0.0f)
		{
			Packet.DelaySeconds -= StepSeconds;
			continue;
		}
		const FVector PreviousPosition = Packet.WorldPosition;
		const FVector Gravity(0.0f, 0.0f, -720.0f);
		const FVector NextPosition = Packet.WorldPosition
			+ Packet.Velocity * StepSeconds
			+ Gravity * (0.5f * StepSeconds * StepSeconds);
		Packet.Velocity += Gravity * StepSeconds;
		const float HorizontalDamping = FMath::Exp(-0.8f * StepSeconds);
		Packet.Velocity.X *= HorizontalDamping;
		Packet.Velocity.Y *= HorizontalDamping;

		const float GrainRadius = FMath::Clamp(
			MaterialBodyVoxelSpacing * 0.09f,
			2.0f,
			4.0f);
		bool bHasImpact = false;
		float BestImpactDistanceSquared = MAX_flt;
		FVector DepositLocation = NextPosition;
		AActor* DepositImpactActor = nullptr;
		const auto ConsiderImpact = [
			&bHasImpact,
			&BestImpactDistanceSquared,
			&DepositLocation,
			&DepositImpactActor,
			&PreviousPosition](
				const FVector& CandidateLocation,
				AActor* CandidateActor)
		{
			const float DistanceSquared = FVector::DistSquared(
				PreviousPosition,
				CandidateLocation);
			if (!bHasImpact || DistanceSquared < BestImpactDistanceSquared)
			{
				bHasImpact = true;
				BestImpactDistanceSquared = DistanceSquared;
				DepositLocation = CandidateLocation;
				DepositImpactActor = CandidateActor;
			}
		};

		FVector FixedImpactLocation;
		FVector FixedImpactNormal;
		AFragment2DSourceActor* FixedImpactSource = nullptr;
		if (PlayableWorld->SweepFixedFragmentSource(
				PreviousPosition,
				NextPosition,
				GrainRadius,
				FixedImpactLocation,
				FixedImpactNormal,
				FixedImpactSource))
		{
			ConsiderImpact(FixedImpactLocation, FixedImpactSource);
		}
		FVector MaterialImpactLocation;
		FName ContactMaterial;
		if (PlayableWorld->SweepSimulatedMaterial(
				PreviousPosition,
				NextPosition,
				GrainRadius,
				MaterialImpactLocation,
				ContactMaterial))
		{
			ConsiderImpact(MaterialImpactLocation, nullptr);
		}
		float TerrainZ = Packet.LandingZ - GrainRadius;
		PlayableWorld->TrySampleTerrainHeightAtWorldLocation(
			NextPosition,
			TerrainZ);
		if (NextPosition.Z <= TerrainZ + GrainRadius)
		{
			ConsiderImpact(
				FVector(NextPosition.X, NextPosition.Y, TerrainZ),
				nullptr);
		}
		if (!bHasImpact)
		{
			Packet.WorldPosition = NextPosition;
			continue;
		}
		if (DepositedCellsThisFrame + Packet.CellCount
			> MaximumDepositedCellsThisFrame)
		{
			Packet.WorldPosition = DepositLocation
				+ FVector::UpVector * GrainRadius * 2.0f;
			Packet.Velocity = FVector::ZeroVector;
			Packet.DelaySeconds = 0.01f;
			continue;
		}

		const int32 Deposited = PlayableWorld->DepositSimulatedMaterialFromImpact(
			DepositLocation,
			ServerPlan.BodyMaterial,
			Packet.CellCount,
			DepositImpactActor
				? DepositImpactActor
				: Packet.ImpactActor.Get(),
			GrainRadius,
			GrainRadius,
			64,
			Packet.ConservedMaterialAmount);
		if (Deposited > 0)
		{
			DepositedCellsThisFrame += Packet.CellCount;
			ProgressiveReleaseImpactPoint = DepositLocation;
			ProgressiveReleaseImpactActor = DepositImpactActor;
			const int32 LandedVoxelIndex = Packet.VoxelIndex;
			FallingMaterialPackets.RemoveAtSwap(
				Index,
				1,
				EAllowShrinking::No);
			if (bGranularMaterialFall
				&& !FallingMaterialPackets.ContainsByPredicate(
					[LandedVoxelIndex](const FFallingMaterialPacket& Candidate)
					{
						return Candidate.VoxelIndex == LandedVoxelIndex;
					}))
			{
				MaterialBodyReleasedVoxelCount = FMath::Min<uint16>(
					static_cast<uint16>(MaterialBodyReleasedVoxelCount + 1),
					static_cast<uint16>(MaterialBodyVoxelPositions.Num()));
				bReplicatedReleaseChanged = true;
			}
		}
		else
		{
			Packet.WorldPosition = DepositLocation
				+ FVector::UpVector * GrainRadius * 2.0f;
			Packet.WorldPosition.X += 4.0f;
			Packet.DelaySeconds = 0.03f;
		}
	}
	if (bReplicatedReleaseChanged)
	{
		ForceNetUpdate();
	}
	if (bGranularMaterialFall && !FallingMaterialPackets.IsEmpty())
	{
		FVector Center = FVector::ZeroVector;
		for (const FFallingMaterialPacket& Packet : FallingMaterialPackets)
		{
			Center += Packet.WorldPosition;
		}
		Center /= static_cast<double>(FallingMaterialPackets.Num());
		Center.Z = FMath::Min(Center.Z, GetActorLocation().Z);
		SetActorLocation(
			Center,
			false,
			nullptr,
			ETeleportType::TeleportPhysics);
	}
}

void AMatterFluxMagicProjectile::FinishProgressiveMaterialRelease()
{
	bImpactHandled = true;
	SpawnTriggerPayload(
		ServerPlan.OnImpactProjectiles,
		ProgressiveReleaseImpactPoint,
		ProgressiveReleaseDirection,
		ServerPlan.bTriggerRandomDirection);
	Destroy();
}

void AMatterFluxMagicProjectile::TickProgressiveMaterialRelease(
	const float DeltaSeconds)
{
	if (MaterialBodyVoxelPositions.IsEmpty())
	{
		bImpactHandled = true;
		Destroy();
		return;
	}
	if (bGranularMaterialFall)
	{
		AdvanceFallingMaterialPackets(DeltaSeconds);
		RefreshMaterialBodyInstances();
		if (FallingMaterialPackets.IsEmpty())
		{
			FinishProgressiveMaterialRelease();
		}
		return;
	}
	ProgressiveMaterialReleaseElapsed += FMath::Max(DeltaSeconds, 0.0f);
	const float Alpha = FMath::Clamp(
		ProgressiveMaterialReleaseElapsed
			/ FMath::Max(ProgressiveMaterialReleaseDuration, 0.05f),
		0.0f,
		1.0f);
	SetActorLocation(
		FMath::Lerp(
			ProgressiveReleaseStartLocation,
			ProgressiveReleaseEndLocation,
			FMath::InterpEaseInOut(0.0f, 1.0f, Alpha, 1.5f)),
		false,
		nullptr,
		ETeleportType::TeleportPhysics);

	const int32 VoxelCount = MaterialBodyVoxelPositions.Num();
	const int32 TargetReleasedCount = FMath::Clamp(
		FMath::FloorToInt(Alpha * static_cast<float>(VoxelCount)),
		0,
		VoxelCount);
	const int32 BaseCellCount = ServerPlan.MaterialAmount / VoxelCount;
	const int32 Remainder = ServerPlan.MaterialAmount % VoxelCount;
	for (int32 Index = MaterialBodyReleasedVoxelCount;
		Index < TargetReleasedCount;
		++Index)
	{
		const int32 VoxelCellCount = BaseCellCount
			+ (Index < Remainder ? 1 : 0);
		if (VoxelCellCount <= 0)
		{
			continue;
		}
		QueueFallingMaterialPacketsForVoxel(Index, VoxelCellCount);
	}
	if (TargetReleasedCount != MaterialBodyReleasedVoxelCount)
	{
		MaterialBodyReleasedVoxelCount = static_cast<uint16>(
			TargetReleasedCount);
		ForceNetUpdate();
	}
	AdvanceFallingMaterialPackets(DeltaSeconds);
	RefreshMaterialBodyInstances();
	if (TargetReleasedCount < VoxelCount
		|| !FallingMaterialPackets.IsEmpty())
	{
		return;
	}
	FinishProgressiveMaterialRelease();
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

void AMatterFluxMagicProjectile::ApplyWorldImpact(
	const FHitResult& Hit,
	const bool bReleaseMaterialBody)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (ServerPlan.bUsePlaneVisual && ServerPlan.Damage > 0.0f)
	{
		const FFragmentDamageShape CutShape = BuildImpactCutShape(
			ServerPlan,
			GetActorForwardVector(),
			Hit.ImpactPoint);
		FFragmentWorldCutRequest Request;
		Request.CutShape = CutShape;
		Request.DamagePower = ServerPlan.Damage * 100.0f;
		Request.EventSeed = ServerEventSeed;
		Request.TargetPadding = ServerPlan.Radius;
		if (UFragmentSimulationSubsystem* Subsystem =
			World->GetSubsystem<UFragmentSimulationSubsystem>())
		{
			Subsystem->RequestWorldCut(Request);
		}

		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			const float CutHalfThickness = CutShape.Thickness * 0.5f;
			It->RemoveSimulatedMaterialInOrientedBox(
				CutShape.WorldTransform,
				FVector(
					CutShape.Extents.X * 0.5f,
					CutHalfThickness,
					CutHalfThickness));
		}
	}

	AActor* ImpactActor = Hit.GetActor();
	if (!IsValid(ImpactActor))
	{
		if (const UPrimitiveComponent* ImpactComponent = Hit.GetComponent())
		{
			ImpactActor = ImpactComponent->GetOwner();
		}
	}
	if (bReleaseMaterialBody)
	{
		ReleaseMaterialBodyAtWorldLocation(Hit.ImpactPoint, ImpactActor);
	}
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
	if (!HasAuthority()
		|| bImpactHandled
		|| bProgressiveMaterialRelease
		|| bProjectsAirborneMaterialParticles)
	{
		return false;
	}
	FHitResult ResolvedHit = Hit;
	AFragment2DSourceActor* FixedSourceHit = nullptr;
	AActor* DirectHitActor = Hit.GetActor();
	if (!IsValid(DirectHitActor))
	{
		if (const UPrimitiveComponent* HitComponent = Hit.GetComponent())
		{
			DirectHitActor = HitComponent->GetOwner();
		}
	}
	// A blocking hit that already identifies its target is authoritative. The
	// fixed-source sweep exists only for synthetic material-column contacts that
	// have no actor identity. Re-sweeping the entire flight path after a real hit
	// can select an unrelated earlier tree and move the payload back toward the
	// caster.
	if (!ServerPlan.BodyMaterial.IsNone() && !IsValid(DirectHitActor))
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
	if (ShouldReleaseMaterialBodyProgressively())
	{
		ApplyWorldImpact(ResolvedHit, false);
		BeginProgressiveMaterialRelease(ResolvedHit, HitActor);
		return true;
	}

	bImpactHandled = true;
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

void AMatterFluxMagicProjectile::OnRep_MaterialBodyReleasedVoxelCount()
{
	RefreshMaterialBodyInstances();
}
