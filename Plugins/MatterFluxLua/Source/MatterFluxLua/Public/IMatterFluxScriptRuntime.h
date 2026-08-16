#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "MatterFluxContentTypes.h"

DECLARE_MULTICAST_DELEGATE_OneParam(
	FOnMatterFluxContentReloaded,
	FMatterFluxContentRegistryPtr);

class MATTERFLUXLUA_API IMatterFluxScriptRuntime : public IModuleInterface
{
public:
	static IMatterFluxScriptRuntime& Get()
	{
		return FModuleManager::LoadModuleChecked<IMatterFluxScriptRuntime>(
			TEXT("MatterFluxLua"));
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("MatterFluxLua"));
	}

	virtual bool LoadContentPackFromSource(
		const FString& Source,
		const FString& Origin,
		FString& OutError) = 0;

	virtual bool ReloadDefaultContentPack(FString& OutError) = 0;
	virtual FString GetDefaultContentPackPath() const = 0;
	virtual FString GetDefaultEngineConfigPath() const = 0;
	virtual FMatterFluxContentRegistryPtr GetActiveRegistry() const = 0;
	virtual FOnMatterFluxContentReloaded& OnContentReloaded() = 0;
};
