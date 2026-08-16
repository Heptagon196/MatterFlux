#include "ForgeMvpFixture.h"

namespace ForgeMvpFixture
{
FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 900D6BC06D4C3EB1 */ static float InternalBias(float Value)
{
	return Value + 2.0f;
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE CAC09EC538D360B3 */ float CallInternalBias(float Value)
{
	return InternalBias(Value);
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 99EB31EBBB910D86 */ float CalculateDamage(float BaseDamage, float Distance)
{
	return BaseDamage - Distance;
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 4D2141DCFE25CE23 */ FString DescribeImpact(const FString& Label, const FName& Category, const FVector& Position)
{
	return FString::Printf(
		TEXT("%s:%s:%.0f"),
		*Label,
		*Category.ToString(),
		Position.X);
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 0EE947E2535CD0B3 */ FVector OffsetPosition(const FVector& Position)
{
	return Position + FVector(1.0, 2.0, 3.0);
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE C3086EFBE4AA3306 */ FImpactData BoostImpact(const FImpactData& Value)
{
	return FImpactData{ Value.Amount + 1.0f, Value.Kind };
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE DE31F1E71A339BE7 */ EImpactKind OppositeImpact(EImpactKind Value)
{
	return Value == EImpactKind::Light ? EImpactKind::Heavy : EImpactKind::Light;
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 18B286874F26CA78 */ float InvokeReflectedFloat(UObject* Object, const FName& FunctionName, float Value)
{
	return Object != nullptr && !FunctionName.IsNone() ? Value : -1.0f;
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE FA641992B9223CA0 */ float FDamageCalculator::Scale(float Value) const
{
	return Value * Multiplier;
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE FFC993EE2D71F368 */ int FDamageCalculator::ClampToInt(double Value)
{
	const int IntegerValue = static_cast<int>(Value);
	return IntegerValue < 0 ? 0 : (IntegerValue > 100 ? 100 : IntegerValue);
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 3D7C51D2492F7F0D */ float FDamageCalculator::SecretScale(float Value) const
{
	return Value * Multiplier + 1.0f;
}
}

// BEGIN UNREAL ANGELSCRIPT FORGE INTERNAL BRIDGE
#include "ForgeGenerated/MatterFluxTests.ForgeMvpFixture.7785ad6d5cb7.forge.internal.inl"
// END UNREAL ANGELSCRIPT FORGE INTERNAL BRIDGE
