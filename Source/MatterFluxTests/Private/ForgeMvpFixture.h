#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

namespace ForgeMvpFixture
{
float CalculateDamage(float BaseDamage, float Distance);
FString DescribeImpact(const FString& Label, const FName& Category, const FVector& Position);
FVector OffsetPosition(const FVector& Position);

enum class EImpactKind : int32
{
	Light = 1,
	Heavy = 4,
};

struct FImpactData
{
	float Amount;
	EImpactKind Kind;
};

FImpactData BoostImpact(const FImpactData& Value);
EImpactKind OppositeImpact(EImpactKind Value);
float InvokeReflectedFloat(UObject* Object, const FName& FunctionName, float Value);
float CallInternalBias(float Value);

class FDamageCalculator
{
public:
	explicit FDamageCalculator(float InMultiplier)
		: Multiplier(InMultiplier)
	{
	}

	float Scale(float Value) const;
	static int ClampToInt(double Value);
	FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 186B74744D673BEC */ float CallSecret(float Value) const { return SecretScale(Value); }

private:
	float SecretScale(float Value) const;
	float Multiplier = 1.0f;
};
}
