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
		int32 EventSeed,
		int32 MaxAffectedSources = 4);

protected:
	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	float Range = 520.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	float TargetRadius = 60.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	float CutThickness = 30.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	int32 MaxAffectedSources = 4;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Cut")
	float DamagePower = 1200.0f;

private:
	int32 ActivationSerial = 0;
};
