#pragma once

#include "CoreMinimal.h"

namespace ForgeAuxFixture
{
template <typename T, int32 Bias>
class TProjectAccumulator
{
public:
	explicit TProjectAccumulator(T InBase)
		: Base(InBase)
	{
	}

	FORCENOINLINE /* UNREAL ANGELSCRIPT FORGE NOINLINE 168BB1C2275CBE71 */ T Add(T Value) const
	{
		return Base + Value + static_cast<T>(Bias);
	}

private:
	T Base;
};

MATTERFLUX_API float CrossModuleValue(float Value);
MATTERFLUX_API TArray<int32> AdvanceProjectValues(const TArray<int32>& Values);
MATTERFLUX_API int32 ApplyProjectTemplate(int32 Base, int32 Value);
}
