#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "Save/MatterFluxSaveTypes.h"
#include "MatterFluxSaveSubsystem.generated.h"

class AMatterFluxPlayableWorldActor;
class APlayerController;
class UMatterFluxSaveGame;
class UMatterFluxSaveMetadata;
class USaveGame;

UENUM(BlueprintType)
enum class EMatterFluxSaveOperation : uint8
{
	Idle,
	Saving,
	Loading,
	Duplicating,
	GeneratingWorld,
	ApplyingWorld,
	Complete,
	Failed
};

DECLARE_MULTICAST_DELEGATE(FOnMatterFluxSaveOperationChanged);

UCLASS()
class MATTERFLUX_API UMatterFluxSaveSubsystem
	: public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool RequestNewGame(APlayerController* Controller, int32 Seed = 0);
	bool RequestStoryGame(
		APlayerController* Controller,
		FName CustomMapId,
		int32 Seed = 1);
	bool RequestSave(APlayerController* Controller, int32 SlotIndex);
	bool RequestLoad(APlayerController* Controller, int32 SlotIndex);
	bool RequestDuplicate(int32 SourceSlotIndex);
	bool RenameSlot(int32 SlotIndex, const FString& NewDisplayName,
		FString& OutError);
	bool DeleteSlot(int32 SlotIndex);
	void AcknowledgeResult();

	const TArray<FMatterFluxSaveSlotInfo>& GetSlots() const;
	const FMatterFluxSaveSlotInfo* FindSlot(int32 SlotIndex) const;
	int32 GetNextAvailableSlotIndex() const;
	int32 GetMostRecentSlotIndex() const;
	bool HasAnySave() const { return GetMostRecentSlotIndex() != INDEX_NONE; }
	bool IsBusy() const;
	EMatterFluxSaveOperation GetOperation() const { return Operation; }
	float GetOperationProgress() const;
	bool IsOperationProgressDeterminate() const;
	FText GetOperationTitle() const;
	FText GetOperationStatusText() const;
	const FString& GetLastResultMessage() const { return LastResultMessage; }
	FOnMatterFluxSaveOperationChanged& OnOperationChanged()
	{
		return OperationChanged;
	}

	static FString MakeSlotName(int32 SlotIndex);
	static float MapWorldGenerationProgress(
		float WorldProgress,
		bool bLoadingSave);
	static float MapWorldInitializationProgress(
		int32 PendingTerrainWork,
		int32 MaximumTerrainWork,
		int32 PendingPopulationWork,
		int32 MaximumPopulationWork,
		float PreviousProgress);
	static float GetApplyingWorldProgress();
	static bool IsDeterminateOperation(
		EMatterFluxSaveOperation InOperation);
	static constexpr int32 UserIndex = 0;

private:
	enum class EPendingGenerationPurpose : uint8
	{
		None,
		NewGame,
		StoryGame,
		LoadGame
	};

	void LoadMetadata();
	void SaveMetadata();
	AMatterFluxPlayableWorldActor* FindPlayableWorld() const;
	bool CanUseSharedWorldSave(
		APlayerController* Controller,
		FString& OutError) const;
	bool CaptureSaveData(
		APlayerController* Controller,
		UMatterFluxSaveGame& Save,
		FString& OutError) const;
	bool StartWorldGeneration(
		APlayerController* Controller,
		int32 Seed,
		EPendingGenerationPurpose Purpose,
		bool bForceExactSeed);
	bool StartCustomMapLoad(
		APlayerController* Controller,
		FName CustomMapId,
		int32 Seed,
		EPendingGenerationPurpose Purpose);
	void FinishOperation(bool bSuccess, const FString& Message);
	void HandleAsyncSaveComplete(
		const FString& SlotName,
		int32 InUserIndex,
		bool bSuccess);
	void HandleAsyncLoadComplete(
		const FString& SlotName,
		int32 InUserIndex,
		USaveGame* LoadedObject);
	void HandleAsyncDuplicateLoadComplete(
		const FString& SlotName,
		int32 InUserIndex,
		USaveGame* LoadedObject);
	void HandleAsyncDuplicateSaveComplete(
		const FString& SlotName,
		int32 InUserIndex,
		bool bSuccess);
	void HandleWorldGenerationFinished(
		bool bSuccess,
		const FString& Message);
	void BeginWaitingForWorldInitialization();
	void PollWorldInitialization();
	void ResetWorldInitializationProgress();

	UPROPERTY(Transient)
	TObjectPtr<UMatterFluxSaveMetadata> Metadata;

	UPROPERTY(Transient)
	TObjectPtr<UMatterFluxSaveGame> PendingLoadedSave;

	TWeakObjectPtr<APlayerController> PendingController;
	TWeakObjectPtr<AMatterFluxPlayableWorldActor> PendingWorld;
	FDelegateHandle WorldGenerationHandle;
	FTimerHandle WorldInitializationTimer;
	EMatterFluxSaveOperation Operation = EMatterFluxSaveOperation::Idle;
	EPendingGenerationPurpose PendingGenerationPurpose =
		EPendingGenerationPurpose::None;
	int32 ActiveSlotIndex = INDEX_NONE;
	int32 DuplicateDestinationSlotIndex = INDEX_NONE;
	FString PendingDuplicateDisplayName;
	float OperationProgress = 0.0f;
	int32 MaximumPendingTerrainInitializationWork = 0;
	int32 MaximumPendingPopulationInitializationWork = 0;
	FString OperationStatus;
	FString LastResultMessage;
	FOnMatterFluxSaveOperationChanged OperationChanged;
};
