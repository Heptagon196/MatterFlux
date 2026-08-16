#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ForgeReflectionFixtureObject.generated.h"

UCLASS()
class UForgeReflectionFixtureObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	float Multiplier = 1.0f;

	UFUNCTION()
	float Scale(float Value) const
	{
		return Value * Multiplier;
	}
};
