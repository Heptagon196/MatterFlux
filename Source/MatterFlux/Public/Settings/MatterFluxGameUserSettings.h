#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameUserSettings.h"
#include "MatterFluxGameUserSettings.generated.h"

UCLASS(Config = GameUserSettings)
class MATTERFLUX_API UMatterFluxGameUserSettings : public UGameUserSettings
{
	GENERATED_BODY()

public:
	static UMatterFluxGameUserSettings* Get();

	virtual void SetToDefaults() override;
	virtual void ValidateSettings() override;
	virtual void ApplyNonResolutionSettings() override;

	void SetMasterVolume(float InVolume);
	float GetMasterVolume() const { return MasterVolume; }
	void SetInterfaceScale(float InScale);
	float GetInterfaceScale() const { return InterfaceScale; }
	void ApplyAndSave();

private:
	UPROPERTY(Config)
	float MasterVolume = 0.8f;

	UPROPERTY(Config)
	float InterfaceScale = 1.0f;
};
