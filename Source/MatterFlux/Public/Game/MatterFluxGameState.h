#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "MatterFluxGameState.generated.h"

UCLASS()
class MATTERFLUX_API AMatterFluxGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AMatterFluxGameState();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void SetAuthoritativeContentIdentity(
		const FString& InPackId,
		int32 InRevision,
		const FString& InVersionHash);

	UPROPERTY(
		BlueprintReadOnly,
		ReplicatedUsing = OnRep_ContentIdentity,
		Category = "Content")
	FString ContentPackId;

	UPROPERTY(
		BlueprintReadOnly,
		ReplicatedUsing = OnRep_ContentIdentity,
		Category = "Content")
	int32 ContentRevision = INDEX_NONE;

	UPROPERTY(
		BlueprintReadOnly,
		ReplicatedUsing = OnRep_ContentIdentity,
		Category = "Content")
	FString ContentVersionHash;

protected:
	UFUNCTION()
	void OnRep_ContentIdentity();

private:
	void ValidateLocalContentIdentity() const;

	FDelegateHandle ContentReloadHandle;
};
