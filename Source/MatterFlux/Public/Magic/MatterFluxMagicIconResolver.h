#pragma once

#include "CoreMinimal.h"

/** Resolves Lua-configured magic icon keys inside Content/Lua/Icons. */
class MATTERFLUX_API FMatterFluxMagicIconResolver
{
public:
	/** Absolute root shared by editor and staged NonUFS builds. */
	static FString GetIconRoot();

	/**
	 * Resolves an extensionless key such as "paper/default" to a PNG.
	 * Absolute paths, traversal and non-PNG extensions are rejected.
	 */
	static bool TryResolveIconPath(
		const FString& IconKey,
		FString& OutIconPath);
};
