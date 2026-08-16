#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GA_FragmentDebugDamage.generated.h"

class AFragment2DSourceActor;

UCLASS()
class MATTERFLUX_API UGA_FragmentDebugDamage : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_FragmentDebugDamage();

	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	static AFragment2DSourceActor* FindDebugSourceActor(UWorld* World);

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment")
	float LineLength = 2000.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment")
	float LineThickness = 120.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fragment")
	float DamagePower = 1200.0f;
};
