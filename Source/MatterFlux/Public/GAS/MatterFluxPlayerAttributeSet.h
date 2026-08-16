#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "MatterFluxPlayerAttributeSet.generated.h"

#define MATTERFLUX_ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/** Minimal replicated player vitality model used by item capabilities and GAS. */
UCLASS()
class MATTERFLUX_API UMatterFluxPlayerAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UMatterFluxPlayerAttributeSet();

	virtual void PreAttributeChange(
		const FGameplayAttribute& Attribute,
		float& NewValue) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "Vitals")
	FGameplayAttributeData Health;
	MATTERFLUX_ATTRIBUTE_ACCESSORS(UMatterFluxPlayerAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "Vitals")
	FGameplayAttributeData MaxHealth;
	MATTERFLUX_ATTRIBUTE_ACCESSORS(UMatterFluxPlayerAttributeSet, MaxHealth)

private:
	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& Previous);
};

#undef MATTERFLUX_ATTRIBUTE_ACCESSORS
