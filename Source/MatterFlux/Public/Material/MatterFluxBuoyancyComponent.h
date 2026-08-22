#pragma once

#include "Components/ActorComponent.h"
#include "MatterFluxBuoyancyComponent.generated.h"

class AMatterFluxPlayableWorldActor;
class UPrimitiveComponent;

/**
 * 把 MatterFlux 的二维液体格转换成角色或 Chaos 刚体可用的浮力。
 *
 * 组件只负责“采样液柱并施加额外加速度”；重力仍由
 * CharacterMovement/Chaos 负责，因此不会产生双重重力。
 */
UCLASS(ClassGroup = (MatterFlux), meta = (BlueprintSpawnableComponent))
class MATTERFLUX_API UMatterFluxBuoyancyComponent final
	: public UActorComponent
{
	GENERATED_BODY()

public:
	UMatterFluxBuoyancyComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** 设置物体相对于水密度的体积密度；必须大于零。 */
	UFUNCTION(BlueprintCallable, Category = "MatterFlux|Liquid")
	void SetBodyDensity(float NewDensity);

	UFUNCTION(BlueprintPure, Category = "MatterFlux|Liquid")
	float GetBodyDensity() const { return BodyDensity; }

	/** 非角色物体可显式指定接受 Chaos 浮力的根图元。 */
	void SetTargetPrimitive(UPrimitiveComponent* NewTarget);

	UFUNCTION(BlueprintPure, Category = "MatterFlux|Liquid")
	float GetLastSubmergedFraction() const
	{
		return LastSubmergedFraction;
	}

	UFUNCTION(BlueprintPure, Category = "MatterFlux|Liquid")
	float GetLastLiquidDensity() const { return LastLiquidDensity; }

protected:
	/** 身体体积密度；小于液体密度时倾向上浮，反之下沉。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MatterFlux|Liquid",
		meta = (ClampMin = "0.05", ClampMax = "20.0"))
	float BodyDensity = 0.65f;

	/** 线速度相对液体流速的阻尼系数。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MatterFlux|Liquid",
		meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float LinearDrag = 2.4f;

	/** Chaos 刚体浸没后的角速度阻尼系数。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MatterFlux|Liquid",
		meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float AngularDrag = 1.8f;

	/** 四个边缘采样点相对碰撞半径/包围盒半径的比例。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MatterFlux|Liquid",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SampleRadiusScale = 0.65f;

private:
	AMatterFluxPlayableWorldActor* ResolvePlayableWorld(float DeltaTime);
	bool ShouldSimulateCharacter() const;
	void ApplyToCharacter(float DeltaTime);
	void ApplyToPhysicsBody(float DeltaTime);

	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> TargetPrimitive;

	TWeakObjectPtr<AMatterFluxPlayableWorldActor> CachedPlayableWorld;
	float WorldResolveCooldown = 0.0f;
	float LastSubmergedFraction = 0.0f;
	float LastLiquidDensity = 0.0f;
};
