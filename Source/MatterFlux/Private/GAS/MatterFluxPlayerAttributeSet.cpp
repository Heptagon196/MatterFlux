#include "GAS/MatterFluxPlayerAttributeSet.h"

#include "Net/UnrealNetwork.h"

UMatterFluxPlayerAttributeSet::UMatterFluxPlayerAttributeSet()
{
	InitMaxHealth(100.0f);
	InitHealth(100.0f);
}

void UMatterFluxPlayerAttributeSet::PreAttributeChange(
	const FGameplayAttribute& Attribute,
	float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	if (Attribute == GetMaxHealthAttribute())
	{
		NewValue = FMath::Max(NewValue, 1.0f);
	}
	else if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
}

void UMatterFluxPlayerAttributeSet::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(
		UMatterFluxPlayerAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(
		UMatterFluxPlayerAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
}

void UMatterFluxPlayerAttributeSet::OnRep_Health(
	const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(
		UMatterFluxPlayerAttributeSet, Health, Previous);
}

void UMatterFluxPlayerAttributeSet::OnRep_MaxHealth(
	const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(
		UMatterFluxPlayerAttributeSet, MaxHealth, Previous);
}
