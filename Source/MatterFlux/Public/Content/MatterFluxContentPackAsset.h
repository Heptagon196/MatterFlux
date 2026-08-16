#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MatterFluxContentPackAsset.generated.h"

/**
 * Asset Manager metadata for a versioned MatterFlux content pack.
 *
 * The Lua file contains data-registration calls. Expensive UE assets remain
 * soft references so a content pack does not force-load all of its visuals.
 */
UCLASS(BlueprintType)
class MATTERFLUX_API UMatterFluxContentPackAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Content Pack")
	FName PackId = TEXT("matterflux.default");

	UPROPERTY(
		VisibleAnywhere,
		BlueprintReadOnly,
		Category = "Content Pack",
		meta = (ClampMin = "1"))
	int32 SchemaVersion = 1;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Content Pack",
		meta = (ClampMin = "0"))
	int32 Revision = 1;

	/** Path relative to the project's Content directory. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Content Pack")
	FString ScriptRelativePath = TEXT("Lua/MatterFluxContent.lua");

	/** Optional visuals/audio referenced by definitions in this pack. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Content Pack")
	TArray<TSoftObjectPtr<UObject>> ReferencedAssets;
};

