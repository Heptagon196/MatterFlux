#include "Game/MatterFluxPlayerState.h"

#include "AbilitySystemComponent.h"
#include "GAS/GA_CastWand.h"
#include "GAS/MatterFluxPlayerAttributeSet.h"
#include "MatterFluxLog.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Progression/MatterFluxProgressionComponent.h"

AMatterFluxPlayerState::AMatterFluxPlayerState()
{
	SetNetUpdateFrequency(100.0f);

	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
	MagicInventory = CreateDefaultSubobject<
		UMatterFluxMagicInventoryComponent>(TEXT("MagicInventory"));
	Progression = CreateDefaultSubobject<
		UMatterFluxProgressionComponent>(TEXT("Progression"));
	PlayerAttributes = CreateDefaultSubobject<
		UMatterFluxPlayerAttributeSet>(TEXT("PlayerAttributes"));
	AbilitySystemComponent->AddAttributeSetSubobject(PlayerAttributes.Get());
	DefaultAbilities.Add(UGA_CastWand::StaticClass());
}

UAbilitySystemComponent* AMatterFluxPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AMatterFluxPlayerState::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		GrantDefaultAbilities();
	}
}

void AMatterFluxPlayerState::GrantDefaultAbilities()
{
	if (!HasAuthority() || bDefaultAbilitiesGranted || !AbilitySystemComponent)
	{
		return;
	}

	int32 GrantedSpecCount = 0;
	for (const TSubclassOf<UGameplayAbility> AbilityClass : DefaultAbilities)
	{
		if (*AbilityClass)
		{
			const int32 Copies = AbilityClass == UGA_CastWand::StaticClass()
				? UGA_CastWand::EquipmentSlotCount
				: 1;
			for (int32 InputId = 0; InputId < Copies; ++InputId)
			{
				AbilitySystemComponent->GiveAbility(
					FGameplayAbilitySpec(
						AbilityClass,
						1,
						Copies > 1 ? InputId : INDEX_NONE,
						this));
				++GrantedSpecCount;
			}
		}
	}

	bDefaultAbilitiesGranted = true;
	ForceNetUpdate();

	UE_LOG(LogMatterFlux, Log, TEXT("Granted %d default GAS ability specs to %s."), GrantedSpecCount, *GetName());
}
