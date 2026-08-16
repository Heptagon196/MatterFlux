#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/PlayerState.h"
#include "MatterFluxPlayerState.generated.h"

class UAbilitySystemComponent;
class UGameplayAbility;
class UMatterFluxMagicInventoryComponent;
class UMatterFluxPlayerAttributeSet;
class UMatterFluxProgressionComponent;

UCLASS()
class MATTERFLUX_API AMatterFluxPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AMatterFluxPlayerState();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual void BeginPlay() override;

	void GrantDefaultAbilities();
	bool HasGrantedDefaultAbilities() const { return bDefaultAbilitiesGranted; }
	const TArray<TSubclassOf<UGameplayAbility>>& GetDefaultAbilities() const { return DefaultAbilities; }
	UMatterFluxMagicInventoryComponent* GetMagicInventory() const
	{
		return MagicInventory;
	}
	UMatterFluxProgressionComponent* GetProgression() const
	{
		return Progression;
	}
	UMatterFluxPlayerAttributeSet* GetPlayerAttributes() const
	{
		return PlayerAttributes;
	}

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GAS")
	TArray<TSubclassOf<UGameplayAbility>> DefaultAbilities;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Magic")
	TObjectPtr<UMatterFluxMagicInventoryComponent> MagicInventory;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Progression")
	TObjectPtr<UMatterFluxProgressionComponent> Progression;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS")
	TObjectPtr<UMatterFluxPlayerAttributeSet> PlayerAttributes;

private:
	bool bDefaultAbilitiesGranted = false;
};
