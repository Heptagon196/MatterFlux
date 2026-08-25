#include "Material/MatterFluxBuoyancyComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "EngineUtils.h"
#include "Game/MatterFluxCharacterMovementComponent.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Material/MatterFluxLiquidBuoyancy.h"

namespace
{
	constexpr int32 BuoyancySampleCount = 5;

	struct FBodyBounds
	{
		FVector Center = FVector::ZeroVector;
		FVector HorizontalExtent = FVector::ZeroVector;
		float BottomZ = 0.0f;
		float TopZ = 0.0f;
	};

	bool MakeCharacterBounds(const ACharacter& Character, FBodyBounds& OutBounds)
	{
		const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
		if (!IsValid(Capsule))
		{
			return false;
		}
		const float Radius = Capsule->GetScaledCapsuleRadius();
		const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		OutBounds.Center = Capsule->GetComponentLocation();
		OutBounds.HorizontalExtent = FVector(Radius, Radius, 0.0f);
		OutBounds.BottomZ = OutBounds.Center.Z - HalfHeight;
		OutBounds.TopZ = OutBounds.Center.Z + HalfHeight;
		return Radius > UE_SMALL_NUMBER && HalfHeight > UE_SMALL_NUMBER;
	}

	bool MakePrimitiveBounds(
		const UPrimitiveComponent& Primitive,
		FBodyBounds& OutBounds)
	{
		const FBoxSphereBounds Bounds = Primitive.Bounds;
		if (Bounds.BoxExtent.ContainsNaN()
			|| Bounds.Origin.ContainsNaN()
			|| Bounds.BoxExtent.Z <= UE_SMALL_NUMBER)
		{
			return false;
		}
		OutBounds.Center = Bounds.Origin;
		OutBounds.HorizontalExtent = FVector(
			Bounds.BoxExtent.X,
			Bounds.BoxExtent.Y,
			0.0f);
		OutBounds.BottomZ = Bounds.Origin.Z - Bounds.BoxExtent.Z;
		OutBounds.TopZ = Bounds.Origin.Z + Bounds.BoxExtent.Z;
		return true;
	}

	bool EvaluateBody(
		const AMatterFluxPlayableWorldActor& PlayableWorld,
		const FBodyBounds& Bounds,
		const float SampleRadiusScale,
		const MatterFlux::Liquid::FBodyState& Body,
		FVector& OutAcceleration,
		float& OutSubmergedFraction,
		float& OutLiquidDensity)
	{
		OutAcceleration = FVector::ZeroVector;
		OutSubmergedFraction = 0.0f;
		OutLiquidDensity = 0.0f;
		const FVector Offset(
			Bounds.HorizontalExtent.X * SampleRadiusScale,
			Bounds.HorizontalExtent.Y * SampleRadiusScale,
			0.0f);
		const FVector Samples[BuoyancySampleCount] = {
			Bounds.Center,
			Bounds.Center + FVector(Offset.X, 0.0f, 0.0f),
			Bounds.Center - FVector(Offset.X, 0.0f, 0.0f),
			Bounds.Center + FVector(0.0f, Offset.Y, 0.0f),
			Bounds.Center - FVector(0.0f, Offset.Y, 0.0f)
		};

		float DensityWeight = 0.0f;
		for (const FVector& Sample : Samples)
		{
			MatterFlux::Liquid::FLiquidColumn Liquid;
			if (!PlayableWorld.TrySampleAmbientLiquidColumnAtWorldLocation(
					Sample, Liquid))
			{
				continue;
			}
			MatterFlux::Liquid::FBuoyancyResult Result;
			if (!MatterFlux::Liquid::FLiquidBuoyancySolver::Evaluate(
					Body, Liquid, Result)
				|| !Result.bSubmerged)
			{
				continue;
			}
			OutAcceleration += Result.Acceleration
				/ static_cast<float>(BuoyancySampleCount);
			OutSubmergedFraction += Result.SubmergedFraction
				/ static_cast<float>(BuoyancySampleCount);
			OutLiquidDensity += Result.LiquidDensity
				* Result.SubmergedFraction;
			DensityWeight += Result.SubmergedFraction;
		}
		if (DensityWeight > UE_SMALL_NUMBER)
		{
			OutLiquidDensity /= DensityWeight;
		}
		return OutSubmergedFraction > UE_SMALL_NUMBER;
	}

	bool EvaluateMovementMedium(
		const AMatterFluxPlayableWorldActor& PlayableWorld,
		const FBodyBounds& Bounds,
		const float SampleRadiusScale,
		float& OutMediumFraction,
		float& OutAverageResistance,
		float& OutWeightedResistance)
	{
		OutMediumFraction = 0.0f;
		OutAverageResistance = 0.0f;
		OutWeightedResistance = 0.0f;
		const FVector Offset(
			Bounds.HorizontalExtent.X * SampleRadiusScale,
			Bounds.HorizontalExtent.Y * SampleRadiusScale,
			0.0f);
		const FVector Samples[BuoyancySampleCount] = {
			Bounds.Center,
			Bounds.Center + FVector(Offset.X, 0.0f, 0.0f),
			Bounds.Center - FVector(Offset.X, 0.0f, 0.0f),
			Bounds.Center + FVector(0.0f, Offset.Y, 0.0f),
			Bounds.Center - FVector(0.0f, Offset.Y, 0.0f)
		};
		const float BodyHeight = Bounds.TopZ - Bounds.BottomZ;
		for (const FVector& Sample : Samples)
		{
			FMatterFluxMovementMediumColumn Medium;
			if (!PlayableWorld.TrySampleMovementMediumColumnAtWorldLocation(
					Sample, Medium))
			{
				continue;
			}
			const float OverlapHeight = FMath::Max(
				0.0f,
				FMath::Min(Bounds.TopZ, Medium.SurfaceZ)
					- FMath::Max(Bounds.BottomZ, Medium.BottomZ));
			const float Fraction = FMath::Clamp(
				OverlapHeight / BodyHeight, 0.0f, 1.0f);
			OutMediumFraction += Fraction
				/ static_cast<float>(BuoyancySampleCount);
			OutWeightedResistance += Medium.MovementResistance * Fraction
				/ static_cast<float>(BuoyancySampleCount);
		}
		if (OutMediumFraction > UE_SMALL_NUMBER)
		{
			OutAverageResistance =
				OutWeightedResistance / OutMediumFraction;
		}
		return OutWeightedResistance > UE_SMALL_NUMBER;
	}
}

UMatterFluxBuoyancyComponent::UMatterFluxBuoyancyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UMatterFluxBuoyancyComponent::BeginPlay()
{
	Super::BeginPlay();
	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		if (UCharacterMovementComponent* Movement =
			Character->GetCharacterMovement())
		{
			Movement->AddTickPrerequisiteComponent(this);
		}
	}
}

void UMatterFluxBuoyancyComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (CachedPlayableWorld.IsValid())
	{
		CachedPlayableWorld->RemoveTickPrerequisiteComponent(this);
	}
	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		if (UCharacterMovementComponent* Movement =
			Character->GetCharacterMovement())
		{
			Movement->RemoveTickPrerequisiteComponent(this);
		}
	}
	CachedPlayableWorld.Reset();
	Super::EndPlay(EndPlayReason);
}

void UMatterFluxBuoyancyComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	LastSubmergedFraction = 0.0f;
	LastLiquidDensity = 0.0f;
	LastMovementMediumFraction = 0.0f;
	LastMovementResistance = 0.0f;
	if (!FMath::IsFinite(DeltaTime) || DeltaTime <= 0.0f)
	{
		return;
	}
	if (Cast<ACharacter>(GetOwner()))
	{
		ApplyToCharacter(DeltaTime);
	}
	else
	{
		ApplyToPhysicsBody(DeltaTime);
	}
}

void UMatterFluxBuoyancyComponent::SetBodyDensity(const float NewDensity)
{
	if (FMath::IsFinite(NewDensity) && NewDensity > 0.0f)
	{
		BodyDensity = FMath::Clamp(NewDensity, 0.05f, 20.0f);
	}
}

void UMatterFluxBuoyancyComponent::SetTargetPrimitive(
	UPrimitiveComponent* NewTarget)
{
	TargetPrimitive = NewTarget;
}

AMatterFluxPlayableWorldActor*
	UMatterFluxBuoyancyComponent::ResolvePlayableWorld(const float DeltaTime)
{
	if (CachedPlayableWorld.IsValid())
	{
		return CachedPlayableWorld.Get();
	}
	WorldResolveCooldown -= DeltaTime;
	if (WorldResolveCooldown > 0.0f || !GetWorld())
	{
		return nullptr;
	}
	WorldResolveCooldown = 1.0f;
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(GetWorld()); It; ++It)
	{
		CachedPlayableWorld = *It;
		(*It)->AddTickPrerequisiteComponent(this);
		return *It;
	}
	return nullptr;
}

bool UMatterFluxBuoyancyComponent::ShouldSimulateCharacter() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	return Character
		&& (Character->HasAuthority() || Character->IsLocallyControlled());
}

void UMatterFluxBuoyancyComponent::ApplyToCharacter(const float DeltaTime)
{
	if (!ShouldSimulateCharacter())
	{
		return;
	}
	ACharacter* Character = CastChecked<ACharacter>(GetOwner());
	UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	UMatterFluxCharacterMovementComponent* MatterFluxMovement =
		Cast<UMatterFluxCharacterMovementComponent>(Movement);
	if (MatterFluxMovement)
	{
		MatterFluxMovement->SetMaterialMovementResistance(0.0f);
	}
	AMatterFluxPlayableWorldActor* PlayableWorld =
		ResolvePlayableWorld(DeltaTime);
	FBodyBounds Bounds;
	if (!Movement
		|| !PlayableWorld
		|| !MakeCharacterBounds(*Character, Bounds))
	{
		return;
	}

	MatterFlux::Liquid::FBodyState Body;
	Body.Density = BodyDensity;
	Body.BottomZ = Bounds.BottomZ;
	Body.TopZ = Bounds.TopZ;
	Body.GravityZ = Movement->GetGravityZ();
	// Material-specific drag is evaluated for both liquid and powder below;
	// the liquid solver remains responsible only for Archimedes buoyancy here.
	Body.LinearDrag = 0.0f;
	Body.Velocity = Movement->Velocity;
	FVector Acceleration;
	const bool bSubmerged = EvaluateBody(
			*PlayableWorld,
			Bounds,
			SampleRadiusScale,
			Body,
			Acceleration,
			LastSubmergedFraction,
			LastLiquidDensity);
	float WeightedResistance = 0.0f;
	EvaluateMovementMedium(
		*PlayableWorld,
		Bounds,
		SampleRadiusScale,
		LastMovementMediumFraction,
		LastMovementResistance,
		WeightedResistance);
	if (MatterFluxMovement)
	{
		MatterFluxMovement->SetMaterialMovementResistance(WeightedResistance);
	}
	PlayableWorld->DisplaceMaterialInWorldBounds(
		Bounds.Center,
		Bounds.HorizontalExtent,
		Bounds.BottomZ,
		Bounds.TopZ,
		true,
		true);
	if (WeightedResistance > UE_SMALL_NUMBER)
	{
		// Exponential damping is stable across frame rates and cannot reverse a
		// velocity on a long frame. Input is then integrated normally by CMC.
		Movement->Velocity *= FMath::Exp(
			-LinearDrag * WeightedResistance * DeltaTime);
	}
	if (!bSubmerged)
	{
		return;
	}

	Movement->Velocity += Acceleration * DeltaTime;
	if (Movement->IsMovingOnGround()
		&& Acceleration.Z > -Body.GravityZ)
	{
		Movement->SetMovementMode(MOVE_Falling);
	}
}

void UMatterFluxBuoyancyComponent::ApplyToPhysicsBody(const float DeltaTime)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}
	UPrimitiveComponent* Primitive = TargetPrimitive;
	if (!IsValid(Primitive))
	{
		Primitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
	}
	AMatterFluxPlayableWorldActor* PlayableWorld =
		ResolvePlayableWorld(DeltaTime);
	FBodyBounds Bounds;
	if (!Primitive
		|| !Primitive->IsSimulatingPhysics()
		|| !PlayableWorld
		|| !MakePrimitiveBounds(*Primitive, Bounds))
	{
		return;
	}

	MatterFlux::Liquid::FBodyState Body;
	Body.Density = BodyDensity;
	Body.BottomZ = Bounds.BottomZ;
	Body.TopZ = Bounds.TopZ;
	Body.GravityZ = GetWorld() ? GetWorld()->GetGravityZ() : -980.0f;
	Body.LinearDrag = 0.0f;
	Body.Velocity = Primitive->GetPhysicsLinearVelocity();
	FVector Acceleration;
	const bool bSubmerged = EvaluateBody(
			*PlayableWorld,
			Bounds,
			SampleRadiusScale,
			Body,
			Acceleration,
			LastSubmergedFraction,
			LastLiquidDensity);
	float WeightedResistance = 0.0f;
	EvaluateMovementMedium(
		*PlayableWorld,
		Bounds,
		SampleRadiusScale,
		LastMovementMediumFraction,
		LastMovementResistance,
		WeightedResistance);
	PlayableWorld->DisplaceMaterialInWorldBounds(
		Bounds.Center,
		Bounds.HorizontalExtent,
		Bounds.BottomZ,
		Bounds.TopZ,
		false,
		true);
	if (WeightedResistance > UE_SMALL_NUMBER)
	{
		Primitive->AddForce(
			-Primitive->GetPhysicsLinearVelocity()
				* Primitive->GetMass()
				* LinearDrag
				* WeightedResistance,
			NAME_None,
			false);
	}
	if (!bSubmerged)
	{
		return;
	}

	Primitive->AddForce(
		Acceleration * Primitive->GetMass(),
		NAME_None,
		false);
	Primitive->AddTorqueInRadians(
		-Primitive->GetPhysicsAngularVelocityInRadians()
			* AngularDrag
			* LastSubmergedFraction,
		NAME_None,
		true);
}
