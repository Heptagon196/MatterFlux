#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GA_PlayerCut.generated.h"

UCLASS()
class MATTERFLUX_API UGA_PlayerCut : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_PlayerCut();

	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	static int32 ExecuteForwardCut(
		AActor& Avatar,
		float Range,
		float TargetRadius,
		float CutThickness,
		float DamagePower,
		int32 EventSeed);

protected:
	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	float Range = 900.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	float TargetRadius = 160.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	float CutThickness = 45.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	float DamagePower = 1200.0f;

private:
	int32 ActivationSerial = 0;
};
