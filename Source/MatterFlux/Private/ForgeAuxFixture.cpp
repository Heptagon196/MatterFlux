#include "ForgeAuxFixture.h"

namespace ForgeAuxFixture
{
FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE A1F12EAE91660732 */ float CrossModuleValue(float Value)
{
	return Value + 3.0f;
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 4136A308C9F1BECF */ TArray<int32> AdvanceProjectValues(const TArray<int32>& Values)
{
	TArray<int32> Result = Values;
	for (int32& Value : Result)
	{
		Value += 4;
	}
	return Result;
}

FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE A25E2C89331BCEEE */ int32 ApplyProjectTemplate(int32 Base, int32 Value)
{
	const TProjectAccumulator<int32, 7> Accumulator(Base);
	return Accumulator.Add(Value);
}
}
