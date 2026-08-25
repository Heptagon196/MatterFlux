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
	struct FFallingMaterialPacket
	{
		FVector WorldPosition = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float LandingZ = 0.0f;
		float DelaySeconds = 0.0f;
		float VisualScale = 1.0f;
		int32 CellCount = 0;
		int32 ConservedMaterialAmount = 0;
		int32 VoxelIndex = INDEX_NONE;
		TWeakObjectPtr<AActor> ImpactActor;
	};

	void ApplyPresentation();
	void BuildMaterialBodyPresentation(float Radius);
	void RefreshMaterialBodyInstances();
	bool TryBeginAirborneMaterialProjection();
	void RefreshAirborneMaterialProjection();
	void FinishAirborneMaterialProjection();
	bool ShouldReleaseMaterialBodyProgressively() const;
	bool ShouldBeginGranularMaterialFall() const;
	void BeginGranularMaterialFall();
	void BeginProgressiveMaterialRelease(
		const FHitResult& Hit,
		AActor* ImpactActor);
	void TickProgressiveMaterialRelease(float DeltaSeconds);
	void QueueFallingMaterialPacketsForVoxel(
		int32 VoxelIndex,
		int32 VoxelCellCount);
	void AdvanceFallingMaterialPackets(float DeltaSeconds);
	void FinishProgressiveMaterialRelease();
	void ReleaseMaterialBodyAtWorldLocation(
		const FVector& WorldLocation,
		AActor* ImpactActor = nullptr);
	void ApplyWorldImpact(
		const FHitResult& Hit,
		bool bReleaseMaterialBody = true);
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

	UFUNCTION()
	void OnRep_MaterialBodyReleasedVoxelCount();

	UPROPERTY(ReplicatedUsing = OnRep_Presentation)
	FMatterFluxMagicProjectilePresentation Presentation;

	UPROPERTY(ReplicatedUsing = OnRep_MaterialBodyReleasedVoxelCount)
	uint16 MaterialBodyReleasedVoxelCount = 0;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> VisualMaterial;

	FMatterFluxMagicProjectilePlan ServerPlan;
	int32 ServerEventSeed = 0;
	bool bImpactHandled = false;
	bool bProjectsAirborneMaterialParticles = false;
	bool bProgressiveMaterialRelease = false;
	bool bGranularMaterialFall = false;
	bool bOrbitInitialized = false;
	bool bHasPreviousMaterialSweepLocation = false;
	float MaterialBodyVoxelSpacing = 0.0f;
	float AirborneMaterialProjectionAccumulator = 0.0f;
	float ProgressiveMaterialReleaseElapsed = 0.0f;
	float ProgressiveMaterialReleaseDuration = 1.6f;
	TArray<FVector> MaterialBodyVoxelPositions;
	FGuid MaterialParticleBatchId;
	FVector LastMaterialParticleCenter = FVector::ZeroVector;
	FVector LastMaterialParticleVelocity = FVector::ZeroVector;
	TArray<FFallingMaterialPacket> FallingMaterialPackets;
	FVector OrbitCenter = FVector::ZeroVector;
	FVector MaterialSweepOriginLocation = FVector::ZeroVector;
	FVector PreviousMaterialSweepLocation = FVector::ZeroVector;
	FVector ProgressiveReleaseStartLocation = FVector::ZeroVector;
	FVector ProgressiveReleaseEndLocation = FVector::ZeroVector;
	FVector ProgressiveReleaseImpactPoint = FVector::ZeroVector;
	FVector ProgressiveReleaseDirection = FVector::DownVector;
	TWeakObjectPtr<AActor> ProgressiveReleaseImpactActor;
};
