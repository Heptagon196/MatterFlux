#include "Game/MatterFluxGameState.h"

#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "Net/UnrealNetwork.h"

AMatterFluxGameState::AMatterFluxGameState()
{
	bReplicates = true;
}

void AMatterFluxGameState::BeginPlay()
{
	Super::BeginPlay();

	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	ContentReloadHandle = Runtime.OnContentReloaded().AddWeakLambda(
		this,
		[this](const FMatterFluxContentRegistryPtr Registry)
		{
			if (HasAuthority())
			{
				if (Registry.IsValid())
				{
					SetAuthoritativeContentIdentity(
						Registry->Manifest.PackId,
						Registry->Manifest.Revision,
						Registry->Manifest.VersionHash);
				}
			}
			else
			{
				ValidateLocalContentIdentity();
			}
		});

	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	if (HasAuthority() && Registry.IsValid())
	{
		SetAuthoritativeContentIdentity(
			Registry->Manifest.PackId,
			Registry->Manifest.Revision,
			Registry->Manifest.VersionHash);
	}
	else if (!HasAuthority() && !ContentVersionHash.IsEmpty())
	{
		ValidateLocalContentIdentity();
	}
}

void AMatterFluxGameState::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IMatterFluxScriptRuntime::IsAvailable())
	{
		IMatterFluxScriptRuntime::Get()
			.OnContentReloaded()
			.Remove(ContentReloadHandle);
	}
	ContentReloadHandle.Reset();
	Super::EndPlay(EndPlayReason);
}

void AMatterFluxGameState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AMatterFluxGameState, ContentPackId);
	DOREPLIFETIME(AMatterFluxGameState, ContentRevision);
	DOREPLIFETIME(AMatterFluxGameState, ContentVersionHash);
}

void AMatterFluxGameState::SetAuthoritativeContentIdentity(
	const FString& InPackId,
	const int32 InRevision,
	const FString& InVersionHash)
{
	check(HasAuthority());

	ContentPackId = InPackId;
	ContentRevision = InRevision;
	ContentVersionHash = InVersionHash;
	ForceNetUpdate();
}

void AMatterFluxGameState::OnRep_ContentIdentity()
{
	ValidateLocalContentIdentity();
}

void AMatterFluxGameState::ValidateLocalContentIdentity() const
{
	if (!IMatterFluxScriptRuntime::IsAvailable())
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Content identity arrived before MatterFluxLua was available"));
		return;
	}

	const FMatterFluxContentRegistryPtr LocalRegistry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!LocalRegistry.IsValid())
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Server requires content '%s' revision %d (%s), but no local pack is loaded"),
			*ContentPackId,
			ContentRevision,
			*ContentVersionHash);
		return;
	}

	const FMatterFluxContentManifest& Local = LocalRegistry->Manifest;
	if (Local.PackId != ContentPackId
		|| Local.Revision != ContentRevision
		|| Local.VersionHash != ContentVersionHash)
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Content version mismatch. Server=%s:%d:%s Local=%s:%d:%s"),
			*ContentPackId,
			ContentRevision,
			*ContentVersionHash,
			*Local.PackId,
			Local.Revision,
			*Local.VersionHash);
	}
}
