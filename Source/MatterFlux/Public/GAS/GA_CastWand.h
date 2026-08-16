#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "Magic/MatterFluxWandProgram.h"
#include "GA_CastWand.generated.h"

UCLASS()
class MATTERFLUX_API UGA_CastWand : public UGameplayAbility
{
	GENERATED_BODY()

public:
	static constexpr int32 EquipmentSlotCount = 4;
	UGA_CastWand();

	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	static bool SpawnCastPlan(
		AActor& Avatar,
		const FMatterFluxWandCastPlan& Plan,
		int32 EventSeed);

private:
	int32 ActivationSerial = 0;
};
