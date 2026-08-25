#pragma once

#include "CoreMinimal.h"
#include "Fragment/FragmentTypes.h"
#include "GameFramework/Actor.h"
#include "Magic/MatterFluxWandProgram.h"
#include "MatterFluxMagicProjectile.generated.h"

class UProjectileMovementComponent;
class UInstancedStaticMeshComponent;
class USphereComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

USTRUCT()
struct MATTERFLUX_API FMatterFluxMagicProjectilePresentation
{
	GENERATED_BODY()

	UPROPERTY()
	FName SpellId;

	UPROPERTY()
	float Speed = 0.0f;

	UPROPERTY()
	float Lifetime = 1.0f;

	UPROPERTY()
	float Radius = 8.0f;

	UPROPERTY()
	float GravityScale = 0.0f;

	UPROPERTY()
	bool bOverrideColor = false;

	UPROPERTY()
	FLinearColor Color = FLinearColor::White;

	UPROPERTY()
	float OrbitRadius = 0.0f;

	UPROPERTY()
	FName BodyMaterial;

	UPROPERTY()
	int32 MaterialAmount = 1;

	UPROPERTY()
	bool bUsePlaneVisual = false;

	UPROPERTY()
	bool bUseVerticalPlaneVisual = false;
};

/** Server-authoritative replicated projectile produced by a wand cast plan. */
UCLASS()
class MATTERFLUX_API AMatterFluxMagicProjectile : public AActor
{
	GENERATED_BODY()

public:
	AMatterFluxMagicProjectile();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void LifeSpanExpired() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void InitializeProjectile(
		const FMatterFluxMagicProjectilePlan& Plan,
		int32 EventSeed);
	static FFragmentDamageShape BuildImpactCutShape(
		const FMatterFluxMagicProjectilePlan& Plan,
		const FVector& ProjectileForward,
		const FVector& ImpactPoint);
	bool ResolveImpactAuthority(const FHitResult& Hit);
	const FMatterFluxMagicProjectilePresentation& GetPresentation() const
	{
		return Presentation;
	}
	int32 GetMaterialBodyVoxelCount() const;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Magic")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Magic")
	TObjectPtr<UStaticMeshComponent> Visual;

	/** Deterministic voxel projection used when the projectile has a body material. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Magic")
	TObjectPtr<UInstancedStaticMeshComponent> MaterialBody;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Magic")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

private:
	void ApplyPresentation();
	void BuildMaterialBodyPresentation(float Radius);
	void ReleaseMaterialBodyAtWorldLocation(
		const FVector& WorldLocation,
		AActor* ImpactActor = nullptr);
	void ApplyWorldImpact(const FHitResult& Hit);
	void SpawnTriggerPayload(
		TConstArrayView<FMatterFluxMagicProjectilePlan> Payload,
		const FVector& Origin,
		const FVector& ParentDirection,
		bool bRandomDirection);

	UFUNCTION()
	void OnProjectileHit(
		UPrimitiveComponent* HitComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		FVector NormalImpulse,
		const FHitResult& Hit);

	UFUNCTION()
	void OnProjectileStopped(const FHitResult& Hit);

	UFUNCTION()
	void OnRep_Presentation();

	UPROPERTY(ReplicatedUsing = OnRep_Presentation)
	FMatterFluxMagicProjectilePresentation Presentation;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> VisualMaterial;

	FMatterFluxMagicProjectilePlan ServerPlan;
	int32 ServerEventSeed = 0;
	bool bImpactHandled = false;
	bool bOrbitInitialized = false;
	bool bHasPreviousMaterialSweepLocation = false;
	FVector OrbitCenter = FVector::ZeroVector;
	FVector MaterialSweepOriginLocation = FVector::ZeroVector;
	FVector PreviousMaterialSweepLocation = FVector::ZeroVector;
};
