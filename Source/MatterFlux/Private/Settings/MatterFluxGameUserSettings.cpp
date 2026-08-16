#include "Settings/MatterFluxGameUserSettings.h"

#include "AudioDevice.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/App.h"

UMatterFluxGameUserSettings* UMatterFluxGameUserSettings::Get()
{
	return GEngine
		? Cast<UMatterFluxGameUserSettings>(GEngine->GetGameUserSettings())
		: nullptr;
}

void UMatterFluxGameUserSettings::SetToDefaults()
{
	Super::SetToDefaults();
	MasterVolume = 0.8f;
	InterfaceScale = 1.0f;
	SetVSyncEnabled(true);
	SetOverallScalabilityLevel(3);
}

void UMatterFluxGameUserSettings::ValidateSettings()
{
	MasterVolume = FMath::Clamp(
		FMath::IsFinite(MasterVolume) ? MasterVolume : 0.8f,
		0.0f,
		1.0f);
	InterfaceScale = FMath::Clamp(
		FMath::IsFinite(InterfaceScale) ? InterfaceScale : 1.0f,
		0.8f,
		1.25f);
	Super::ValidateSettings();
}

void UMatterFluxGameUserSettings::ApplyNonResolutionSettings()
{
	ValidateSettings();
	Super::ApplyNonResolutionSettings();
	if (GEngine)
	{
		if (FAudioDeviceHandle AudioDevice = GEngine->GetMainAudioDevice())
		{
			AudioDevice->SetTransientPrimaryVolume(MasterVolume);
		}
	}
	// Application scale is global to the Slate process.  Apply it only when
	// this process is actually running the game so PIE does not resize the
	// surrounding editor UI.
	if (FApp::IsGame() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetApplicationScale(InterfaceScale);
	}
}

void UMatterFluxGameUserSettings::SetMasterVolume(const float InVolume)
{
	MasterVolume = FMath::Clamp(
		FMath::IsFinite(InVolume) ? InVolume : 0.8f,
		0.0f,
		1.0f);
}

void UMatterFluxGameUserSettings::SetInterfaceScale(const float InScale)
{
	InterfaceScale = FMath::Clamp(
		FMath::IsFinite(InScale) ? InScale : 1.0f,
		0.8f,
		1.25f);
}

void UMatterFluxGameUserSettings::ApplyAndSave()
{
	ValidateSettings();
	ApplySettings(false);
	SaveSettings();
}
