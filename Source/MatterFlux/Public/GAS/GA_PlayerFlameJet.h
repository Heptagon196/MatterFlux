#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GA_PlayerFlameJet.generated.h"

UCLASS()
class MATTERFLUX_API UGA_PlayerFlameJet : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_PlayerFlameJet();

	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	static int32 ExecuteFlameJet(
		AActor& Avatar,
		float Range,
		float StartRadius,
		float EndRadius,
		FName FlameMaterial,
		int32 EventSeed);

protected:
	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Flame")
	float Range = 800.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Flame")
	float StartRadius = 45.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Flame")
	float EndRadius = 180.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Player Ability|Flame")
	FName FlameMaterial = TEXT("fire");

private:
	int32 ActivationSerial = 0;
};
