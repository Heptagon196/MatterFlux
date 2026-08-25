#include "Save/MatterFluxSaveSubsystem.h"

#include "Async/Async.h"
#include "EngineUtils.h"
#include "Game/MatterFluxGameMode.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerState.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "MatterFluxLog.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "Save/MatterFluxSaveGame.h"

namespace
{
	const FString MetadataSlotName = TEXT("MatterFlux_Save_Metadata");
	constexpr float InitialWorldProgress = 0.05f;
	constexpr float FinalGenerationProgress = 0.72f;
	constexpr float InitialLoadGenerationProgress = 0.15f;
	constexpr float ApplyingWorldProgress = FinalGenerationProgress;
	constexpr float FinalInitializationProgress = 0.995f;
	constexpr float TerrainInitializationWeight = 0.65f;

	const TArray<FMatterFluxSaveSlotInfo>& EmptySlots()
	{
		static const TArray<FMatterFluxSaveSlotInfo> Empty;
		return Empty;
	}
}

void UMatterFluxSaveSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LoadMetadata();
}

void UMatterFluxSaveSubsystem::Deinitialize()
{
	if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
	{
		World->GetTimerManager().ClearTimer(WorldInitializationTimer);
	}
	if (AMatterFluxPlayableWorldActor* World = PendingWorld.Get())
	{
		World->OnGenerationFinished().Remove(WorldGenerationHandle);
	}
	WorldGenerationHandle.Reset();
	WorldInitializationTimer.Invalidate();
	ResetWorldInitializationProgress();
	PendingWorld.Reset();
	PendingController.Reset();
	PendingLoadedSave = nullptr;
	DuplicateDestinationSlotIndex = INDEX_NONE;
	PendingDuplicateDisplayName.Reset();
	Metadata = nullptr;
	Super::Deinitialize();
}

FString UMatterFluxSaveSubsystem::MakeSlotName(const int32 SlotIndex)
{
	return FString::Printf(TEXT("MatterFlux_Save_%02d"), SlotIndex);
}

void UMatterFluxSaveSubsystem::LoadMetadata()
{
	Metadata = UGameplayStatics::DoesSaveGameExist(
		MetadataSlotName,
		UserIndex)
		? Cast<UMatterFluxSaveMetadata>(
			UGameplayStatics::LoadGameFromSlot(
				MetadataSlotName,
				UserIndex))
		: nullptr;
	if (!Metadata)
	{
		Metadata = Cast<UMatterFluxSaveMetadata>(
			UGameplayStatics::CreateSaveGameObject(
				UMatterFluxSaveMetadata::StaticClass()));
		if (Metadata)
		{
			Metadata->Initialize();
		}
	}
	if (!Metadata)
	{
		return;
	}
	const bool bMetadataWasValid = Metadata->ValidateAndRepair();
	const int32 RemovedMissingSlots = Metadata->Slots.RemoveAll(
		[](const FMatterFluxSaveSlotInfo& Slot)
		{
			return !UGameplayStatics::DoesSaveGameExist(
				MakeSlotName(Slot.SlotIndex),
				UserIndex);
		});
	if (!bMetadataWasValid || RemovedMissingSlots > 0)
	{
		SaveMetadata();
	}
}

void UMatterFluxSaveSubsystem::SaveMetadata()
{
	if (Metadata
		&& !UGameplayStatics::SaveGameToSlot(
			Metadata,
			MetadataSlotName,
			UserIndex))
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Failed to save MatterFlux slot metadata."));
	}
}

const TArray<FMatterFluxSaveSlotInfo>&
	UMatterFluxSaveSubsystem::GetSlots() const
{
	return Metadata ? Metadata->Slots : EmptySlots();
}

const FMatterFluxSaveSlotInfo* UMatterFluxSaveSubsystem::FindSlot(
	const int32 SlotIndex) const
{
	return GetSlots().FindByPredicate(
		[SlotIndex](const FMatterFluxSaveSlotInfo& Slot)
		{
			return Slot.SlotIndex == SlotIndex && Slot.bOccupied;
		});
}

int32 UMatterFluxSaveSubsystem::GetNextAvailableSlotIndex() const
{
	return Metadata ? Metadata->GetNextAvailableSlotIndex() : INDEX_NONE;
}

int32 UMatterFluxSaveSubsystem::GetMostRecentSlotIndex() const
{
	const FMatterFluxSaveSlotInfo* MostRecent = nullptr;
	for (const FMatterFluxSaveSlotInfo& Slot : GetSlots())
	{
		if (Slot.bOccupied
			&& (!MostRecent || Slot.SavedAtUtc > MostRecent->SavedAtUtc))
		{
			MostRecent = &Slot;
		}
	}
	return MostRecent ? MostRecent->SlotIndex : INDEX_NONE;
}

bool UMatterFluxSaveSubsystem::IsBusy() const
{
	return Operation == EMatterFluxSaveOperation::Saving
		|| Operation == EMatterFluxSaveOperation::Loading
		|| Operation == EMatterFluxSaveOperation::Duplicating
		|| Operation == EMatterFluxSaveOperation::GeneratingWorld
		|| Operation == EMatterFluxSaveOperation::ApplyingWorld;
}

float UMatterFluxSaveSubsystem::GetOperationProgress() const
{
	if (Operation == EMatterFluxSaveOperation::GeneratingWorld)
	{
		if (const AMatterFluxPlayableWorldActor* World = PendingWorld.Get())
		{
			return MapWorldGenerationProgress(
				World->GetGenerationProgress(),
				PendingGenerationPurpose
					== EPendingGenerationPurpose::LoadGame);
		}
	}
	return OperationProgress;
}

bool UMatterFluxSaveSubsystem::IsOperationProgressDeterminate() const
{
	return IsDeterminateOperation(Operation);
}

float UMatterFluxSaveSubsystem::MapWorldGenerationProgress(
	const float WorldProgress,
	const bool bLoadingSave)
{
	const float SafeWorldProgress = FMath::IsFinite(WorldProgress)
		? FMath::Clamp(WorldProgress, InitialWorldProgress, 1.0f)
		: InitialWorldProgress;
	const float NormalizedWorldProgress =
		(SafeWorldProgress - InitialWorldProgress)
		/ (1.0f - InitialWorldProgress);
	return FMath::Lerp(
		bLoadingSave
			? InitialLoadGenerationProgress
			: InitialWorldProgress,
		FinalGenerationProgress,
		NormalizedWorldProgress);
}

float UMatterFluxSaveSubsystem::MapWorldInitializationProgress(
	const int32 PendingTerrainWork,
	const int32 MaximumTerrainWork,
	const int32 PendingPopulationWork,
	const int32 MaximumPopulationWork,
	const float PreviousProgress)
{
	const int32 SafeMaximumTerrain = FMath::Max(
		MaximumTerrainWork, FMath::Max(PendingTerrainWork, 1));
	const int32 SafeMaximumPopulation = FMath::Max(
		MaximumPopulationWork, FMath::Max(PendingPopulationWork, 0));
	const float TerrainCompletion = 1.0f - static_cast<float>(
		FMath::Clamp(PendingTerrainWork, 0, SafeMaximumTerrain))
		/ static_cast<float>(SafeMaximumTerrain);
	const float PopulationCompletion = SafeMaximumPopulation > 0
		? 1.0f - static_cast<float>(FMath::Clamp(
			PendingPopulationWork, 0, SafeMaximumPopulation))
			/ static_cast<float>(SafeMaximumPopulation)
		: PendingTerrainWork <= 0 ? 1.0f : 0.0f;
	const float WorkCompletion =
		TerrainCompletion * TerrainInitializationWeight
		+ PopulationCompletion * (1.0f - TerrainInitializationWeight);
	const float CalculatedProgress = FMath::Lerp(
		FinalGenerationProgress,
		FinalInitializationProgress,
		FMath::Clamp(WorkCompletion, 0.0f, 1.0f));
	const float SafePreviousProgress = FMath::IsFinite(PreviousProgress)
		? PreviousProgress : FinalGenerationProgress;
	return FMath::Clamp(
		FMath::Max(SafePreviousProgress, CalculatedProgress),
		FinalGenerationProgress,
		FinalInitializationProgress);
}

float UMatterFluxSaveSubsystem::GetApplyingWorldProgress()
{
	return ApplyingWorldProgress;
}

bool UMatterFluxSaveSubsystem::IsDeterminateOperation(
	const EMatterFluxSaveOperation InOperation)
{
	return InOperation == EMatterFluxSaveOperation::GeneratingWorld
		|| InOperation == EMatterFluxSaveOperation::ApplyingWorld;
}

FText UMatterFluxSaveSubsystem::GetOperationTitle() const
{
	switch (Operation)
	{
	case EMatterFluxSaveOperation::Saving:
		return FText::FromString(TEXT("正在保存游戏"));
	case EMatterFluxSaveOperation::Loading:
		return FText::FromString(TEXT("正在读取存档"));
	case EMatterFluxSaveOperation::Duplicating:
		return FText::FromString(TEXT("正在复制存档"));
	case EMatterFluxSaveOperation::GeneratingWorld:
		return FText::FromString(
			PendingGenerationPurpose == EPendingGenerationPurpose::LoadGame
				? TEXT("正在加载世界")
				: TEXT("正在生成地图"));
	case EMatterFluxSaveOperation::ApplyingWorld:
		return FText::FromString(
			PendingGenerationPurpose == EPendingGenerationPurpose::NewGame
				? TEXT("正在初始化自由模式")
				: TEXT("正在恢复世界状态"));
	case EMatterFluxSaveOperation::Failed:
		return FText::FromString(TEXT("操作失败"));
	case EMatterFluxSaveOperation::Complete:
		return FText::FromString(TEXT("操作完成"));
	default:
		return FText::GetEmpty();
	}
}

FText UMatterFluxSaveSubsystem::GetOperationStatusText() const
{
	if (Operation == EMatterFluxSaveOperation::GeneratingWorld)
	{
		if (const AMatterFluxPlayableWorldActor* World = PendingWorld.Get())
		{
			return FText::FromString(World->GetGenerationStatusText());
		}
	}
	return FText::FromString(OperationStatus);
}

AMatterFluxPlayableWorldActor*
	UMatterFluxSaveSubsystem::FindPlayableWorld() const
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

bool UMatterFluxSaveSubsystem::CanUseSharedWorldSave(
	APlayerController* Controller,
	FString& OutError) const
{
	OutError.Reset();
	if (!IsValid(Controller)
		|| !Controller->IsLocalController()
		|| !Controller->HasAuthority()
		|| !Controller->GetWorld()
		|| Controller->GetNetMode() == NM_Client)
	{
		OutError = TEXT("共享世界只能由单机或主机玩家保存和加载");
		return false;
	}
	if (IsBusy())
	{
		OutError = TEXT("已有存档或地图操作正在进行");
		return false;
	}
	return true;
}

bool UMatterFluxSaveSubsystem::CaptureSaveData(
	APlayerController* Controller,
	UMatterFluxSaveGame& Save,
	FString& OutError) const
{
	AMatterFluxPlayableWorldActor* World = FindPlayableWorld();
	AMatterFluxPlayerState* PlayerState = Controller
		? Controller->GetPlayerState<AMatterFluxPlayerState>()
		: nullptr;
	UMatterFluxMagicInventoryComponent* Inventory = PlayerState
		? PlayerState->GetMagicInventory()
		: nullptr;
	UMatterFluxProgressionComponent* Progression = PlayerState
		? PlayerState->GetProgression()
		: nullptr;
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!World || !Inventory || !Progression || !Pawn
		|| World->IsGenerationInProgress())
	{
		OutError = TEXT("世界、玩家或法术背包尚未准备好");
		return false;
	}
	Save.InitializeNew(World->GetMapSeed());
	Save.CustomMapId = World->GetActiveCustomMapId();
	Save.PlayerTransform = Pawn->GetActorTransform();
	if (!Inventory->CaptureSaveState(Save.MagicInventory, OutError)
		|| !Progression->CaptureSaveState(Save.Progression, OutError)
		|| !World->CaptureSaveState(Save.WorldState, OutError))
	{
		return false;
	}
	return Save.ValidateAndMigrate(OutError);
}

bool UMatterFluxSaveSubsystem::RequestSave(
	APlayerController* Controller,
	const int32 SlotIndex)
{
	FString Error;
	if (!CanUseSharedWorldSave(Controller, Error)
		|| !Metadata
		|| SlotIndex < 0)
	{
		FinishOperation(false, Error.IsEmpty()
			? TEXT("存档槽无效") : Error);
		return false;
	}
	UMatterFluxSaveGame* Save = Cast<UMatterFluxSaveGame>(
		UGameplayStatics::CreateSaveGameObject(
			UMatterFluxSaveGame::StaticClass()));
	if (!Save || !CaptureSaveData(Controller, *Save, Error))
	{
		FinishOperation(false, Error.IsEmpty()
			? TEXT("无法创建存档快照") : Error);
		return false;
	}
	PendingLoadedSave = Save;
	PendingController = Controller;
	ActiveSlotIndex = SlotIndex;
	Operation = EMatterFluxSaveOperation::Saving;
	// The async save API does not expose byte-level progress. Keep the stored
	// value neutral as well as presenting this operation as indeterminate.
	OperationProgress = 0.0f;
	OperationStatus = TEXT("世界快照已完成，正在写入磁盘…");
	LastResultMessage.Reset();
	OperationChanged.Broadcast();
	FAsyncSaveGameToSlotDelegate Delegate;
	Delegate.BindUObject(
		this,
		&UMatterFluxSaveSubsystem::HandleAsyncSaveComplete);
	UGameplayStatics::AsyncSaveGameToSlot(
		Save,
		MakeSlotName(SlotIndex),
		UserIndex,
		Delegate);
	return true;
}

void UMatterFluxSaveSubsystem::HandleAsyncSaveComplete(
	const FString&,
	const int32,
	const bool bSuccess)
{
	if (bSuccess && Metadata && ActiveSlotIndex >= 0 && PendingLoadedSave)
	{
		FMatterFluxSaveSlotInfo* ExistingSlot = Metadata->Slots.FindByPredicate(
			[this](const FMatterFluxSaveSlotInfo& Slot)
			{
				return Slot.SlotIndex == ActiveSlotIndex;
			});
		FMatterFluxSaveSlotInfo& Slot = ExistingSlot
			? *ExistingSlot
			: Metadata->Slots.AddDefaulted_GetRef();
		Slot.SlotIndex = ActiveSlotIndex;
		Slot.bOccupied = true;
		Slot.MapSeed = PendingLoadedSave->MapSeed;
		Slot.SavedAtUtc = PendingLoadedSave->SavedAtUtc;
		Metadata->ValidateAndRepair();
		SaveMetadata();
	}
	PendingLoadedSave = nullptr;
	FinishOperation(
		bSuccess,
		bSuccess ? TEXT("游戏已保存") : TEXT("写入存档失败"));
}

bool UMatterFluxSaveSubsystem::RequestLoad(
	APlayerController* Controller,
	const int32 SlotIndex)
{
	FString Error;
	if (!CanUseSharedWorldSave(Controller, Error)
		|| !Metadata
		|| !FindSlot(SlotIndex)
		|| !UGameplayStatics::DoesSaveGameExist(
			MakeSlotName(SlotIndex), UserIndex))
	{
		FinishOperation(false, Error.IsEmpty()
			? TEXT("这个存档槽为空") : Error);
		return false;
	}
	PendingController = Controller;
	ActiveSlotIndex = SlotIndex;
	Operation = EMatterFluxSaveOperation::Loading;
	// The async load API does not expose byte-level progress. A fabricated
	// percentage would be misleading to non-UI callers of GetOperationProgress.
	OperationProgress = 0.0f;
	OperationStatus = TEXT("正在从磁盘读取并校验存档…");
	LastResultMessage.Reset();
	OperationChanged.Broadcast();
	FAsyncLoadGameFromSlotDelegate Delegate;
	Delegate.BindUObject(
		this,
		&UMatterFluxSaveSubsystem::HandleAsyncLoadComplete);
	UGameplayStatics::AsyncLoadGameFromSlot(
		MakeSlotName(SlotIndex),
		UserIndex,
		Delegate);
	return true;
}

void UMatterFluxSaveSubsystem::HandleAsyncLoadComplete(
	const FString&,
	const int32,
	USaveGame* LoadedObject)
{
	UMatterFluxSaveGame* Loaded = Cast<UMatterFluxSaveGame>(LoadedObject);
	FString Error;
	if (!Loaded || !Loaded->ValidateAndMigrate(Error))
	{
		FinishOperation(false, Error.IsEmpty()
			? TEXT("存档不存在或类型不正确") : Error);
		return;
	}
	PendingLoadedSave = Loaded;
	const bool bStarted = Loaded->CustomMapId.IsNone()
		? StartWorldGeneration(
			PendingController.Get(),
			Loaded->MapSeed,
			EPendingGenerationPurpose::LoadGame,
			true)
		: StartCustomMapLoad(
			PendingController.Get(),
			Loaded->CustomMapId,
			Loaded->MapSeed,
			EPendingGenerationPurpose::LoadGame);
	if (!bStarted)
	{
		return;
	}
}

bool UMatterFluxSaveSubsystem::RequestDuplicate(const int32 SourceSlotIndex)
{
	if (IsBusy())
	{
		return false;
	}
	const FMatterFluxSaveSlotInfo* SourceSlot = FindSlot(SourceSlotIndex);
	const int32 DestinationSlotIndex = GetNextAvailableSlotIndex();
	if (!Metadata || !SourceSlot
		|| DestinationSlotIndex == INDEX_NONE
		|| !UGameplayStatics::DoesSaveGameExist(
			MakeSlotName(SourceSlotIndex), UserIndex))
	{
		FinishOperation(false, TEXT("无法复制这个存档"));
		return false;
	}

	ActiveSlotIndex = SourceSlotIndex;
	DuplicateDestinationSlotIndex = DestinationSlotIndex;
	const FString SourceName = SourceSlot->DisplayName.IsEmpty()
		? FString::Printf(TEXT("存档 %d"), SourceSlotIndex + 1)
		: SourceSlot->DisplayName;
	PendingDuplicateDisplayName = UMatterFluxSaveMetadata::NormalizeDisplayName(
		SourceName + TEXT(" 副本"));
	Operation = EMatterFluxSaveOperation::Duplicating;
	OperationProgress = 0.0f;
	OperationStatus = TEXT("正在读取并校验源存档…");
	LastResultMessage.Reset();
	OperationChanged.Broadcast();

	FAsyncLoadGameFromSlotDelegate Delegate;
	Delegate.BindUObject(this,
		&UMatterFluxSaveSubsystem::HandleAsyncDuplicateLoadComplete);
	UGameplayStatics::AsyncLoadGameFromSlot(
		MakeSlotName(SourceSlotIndex), UserIndex, Delegate);
	return true;
}

void UMatterFluxSaveSubsystem::HandleAsyncDuplicateLoadComplete(
	const FString&,
	const int32,
	USaveGame* LoadedObject)
{
	UMatterFluxSaveGame* Loaded = Cast<UMatterFluxSaveGame>(LoadedObject);
	FString Error;
	if (!Loaded || !Loaded->ValidateAndMigrate(Error)
		|| DuplicateDestinationSlotIndex == INDEX_NONE)
	{
		FinishOperation(false, Error.IsEmpty()
			? TEXT("源存档不存在或内容无效") : Error);
		return;
	}
	Loaded->SavedAtUtc = FDateTime::UtcNow();
	PendingLoadedSave = Loaded;
	OperationStatus = TEXT("源存档有效，正在写入副本…");
	OperationChanged.Broadcast();

	FAsyncSaveGameToSlotDelegate Delegate;
	Delegate.BindUObject(this,
		&UMatterFluxSaveSubsystem::HandleAsyncDuplicateSaveComplete);
	UGameplayStatics::AsyncSaveGameToSlot(
		Loaded,
		MakeSlotName(DuplicateDestinationSlotIndex),
		UserIndex,
		Delegate);
}

void UMatterFluxSaveSubsystem::HandleAsyncDuplicateSaveComplete(
	const FString&,
	const int32,
	const bool bSuccess)
{
	if (!bSuccess || !Metadata || !PendingLoadedSave
		|| DuplicateDestinationSlotIndex == INDEX_NONE)
	{
		FinishOperation(false, TEXT("写入存档副本失败"));
		return;
	}
	FMatterFluxSaveSlotInfo& Slot = Metadata->Slots.AddDefaulted_GetRef();
	Slot.SlotIndex = DuplicateDestinationSlotIndex;
	Slot.bOccupied = true;
	Slot.MapSeed = PendingLoadedSave->MapSeed;
	Slot.SavedAtUtc = PendingLoadedSave->SavedAtUtc;
	Slot.DisplayName = PendingDuplicateDisplayName;
	Metadata->ValidateAndRepair();
	SaveMetadata();
	FinishOperation(true, TEXT("存档副本已创建"));
}

bool UMatterFluxSaveSubsystem::RenameSlot(
	const int32 SlotIndex,
	const FString& NewDisplayName,
	FString& OutError)
{
	OutError.Reset();
	if (IsBusy() || !Metadata)
	{
		OutError = TEXT("存档系统正忙");
		return false;
	}
	FMatterFluxSaveSlotInfo* Slot = Metadata->Slots.FindByPredicate(
		[SlotIndex](const FMatterFluxSaveSlotInfo& Candidate)
		{
			return Candidate.SlotIndex == SlotIndex && Candidate.bOccupied;
		});
	if (!Slot)
	{
		OutError = TEXT("找不到这个存档");
		return false;
	}
	Slot->DisplayName = UMatterFluxSaveMetadata::NormalizeDisplayName(
		NewDisplayName);
	SaveMetadata();
	OperationChanged.Broadcast();
	return true;
}

bool UMatterFluxSaveSubsystem::RequestNewGame(
	APlayerController* Controller,
	const int32 Seed)
{
	FString Error;
	if (!CanUseSharedWorldSave(Controller, Error))
	{
		FinishOperation(false, Error);
		return false;
	}
	PendingLoadedSave = nullptr;
	return StartWorldGeneration(
		Controller,
		Seed,
		EPendingGenerationPurpose::NewGame,
		false);
}

bool UMatterFluxSaveSubsystem::RequestStoryGame(
	APlayerController* Controller,
	const FName CustomMapId,
	const int32 Seed)
{
	FString Error;
	if (!CanUseSharedWorldSave(Controller, Error))
	{
		FinishOperation(false, Error);
		return false;
	}
	PendingLoadedSave = nullptr;
	return StartCustomMapLoad(
		Controller,
		CustomMapId,
		Seed,
		EPendingGenerationPurpose::StoryGame);
}

bool UMatterFluxSaveSubsystem::StartWorldGeneration(
	APlayerController* Controller,
	const int32 Seed,
	const EPendingGenerationPurpose Purpose,
	const bool bForceExactSeed)
{
	AMatterFluxPlayableWorldActor* World = FindPlayableWorld();
	if (!World || !Controller)
	{
		FinishOperation(false, TEXT("找不到可生成的游戏世界"));
		return false;
	}
	if (WorldGenerationHandle.IsValid())
	{
		World->OnGenerationFinished().Remove(WorldGenerationHandle);
		WorldGenerationHandle.Reset();
	}
	PendingController = Controller;
	PendingWorld = World;
	PendingGenerationPurpose = Purpose;
	Operation = EMatterFluxSaveOperation::GeneratingWorld;
	OperationProgress = Purpose == EPendingGenerationPurpose::LoadGame
		? InitialLoadGenerationProgress : InitialWorldProgress;
	OperationStatus = TEXT("正在准备地图生成任务…");
	LastResultMessage.Reset();
	WorldGenerationHandle = World->OnGenerationFinished().AddUObject(
		this,
		&UMatterFluxSaveSubsystem::HandleWorldGenerationFinished);
	if (!World->RequestRegenerateAsync(Seed, bForceExactSeed))
	{
		World->OnGenerationFinished().Remove(WorldGenerationHandle);
		WorldGenerationHandle.Reset();
		FinishOperation(false, TEXT("地图生成请求被拒绝"));
		return false;
	}
	if (Purpose == EPendingGenerationPurpose::NewGame)
	{
		// Returning to the title screen preserves the current world and pawn. A
		// subsequent "new free mode" must nevertheless behave like a new game:
		// remove the previous streaming focus now that generation was accepted, then
		// let the terrain-safe entry queue create a fresh pawn at PlayerStart.
		if (APawn* ExistingPawn = Controller->GetPawn())
		{
			Controller->UnPossess();
			ExistingPawn->Destroy();
		}
	}
	OperationChanged.Broadcast();
	return true;
}

bool UMatterFluxSaveSubsystem::StartCustomMapLoad(
	APlayerController* Controller,
	const FName CustomMapId,
	const int32 Seed,
	const EPendingGenerationPurpose Purpose)
{
	AMatterFluxPlayableWorldActor* World = FindPlayableWorld();
	if (!World || !Controller || CustomMapId.IsNone())
	{
		FinishOperation(false, TEXT("找不到故事地图或本地玩家"));
		return false;
	}
	PendingController = Controller;
	PendingWorld = World;
	PendingGenerationPurpose = Purpose;
	Operation = EMatterFluxSaveOperation::GeneratingWorld;
	OperationProgress = Purpose == EPendingGenerationPurpose::LoadGame
		? InitialLoadGenerationProgress : InitialWorldProgress;
	OperationStatus = TEXT("正在加载故事地图与场景标记…");
	LastResultMessage.Reset();
	OperationChanged.Broadcast();
	if (AMatterFluxGameMode* GameMode = World->GetWorld()
		? Cast<AMatterFluxGameMode>(World->GetWorld()->GetAuthGameMode())
		: nullptr)
	{
		// Close the entry gate before LoadCustomMap starts replacing terrain.
		// Its completion callback is queued for a later game-thread pass, so
		// closing it there would leave a window for configured creatures to spawn
		// against an only partially prepared story world.
		GameMode->PrepareForInitialWorldEntry();
	}
	FString Error;
	if (!World->LoadCustomMap(CustomMapId, Seed, Error))
	{
		FinishOperation(false, Error.IsEmpty()
			? TEXT("故事地图加载失败") : Error);
		return false;
	}
	TWeakObjectPtr<UMatterFluxSaveSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis]()
	{
		if (UMatterFluxSaveSubsystem* Self = WeakThis.Get())
		{
			Self->HandleWorldGenerationFinished(
				true,
				TEXT("故事地图加载完成"));
		}
	});
	return true;
}

void UMatterFluxSaveSubsystem::HandleWorldGenerationFinished(
	const bool bSuccess,
	const FString& Message)
{
	AMatterFluxPlayableWorldActor* World = PendingWorld.Get();
	if (World && WorldGenerationHandle.IsValid())
	{
		World->OnGenerationFinished().Remove(WorldGenerationHandle);
	}
	WorldGenerationHandle.Reset();
	if (!bSuccess || !World)
	{
		FinishOperation(false, Message.IsEmpty()
			? TEXT("地图生成失败") : Message);
		return;
	}

	Operation = EMatterFluxSaveOperation::ApplyingWorld;
	OperationProgress = GetApplyingWorldProgress();
	OperationStatus = PendingGenerationPurpose
		== EPendingGenerationPurpose::LoadGame
		? TEXT("正在恢复玩家、法术背包与世界变化…")
		: PendingGenerationPurpose == EPendingGenerationPurpose::StoryGame
			? TEXT("正在启动故事任务与角色…")
			: TEXT("正在准备自由模式…");
	OperationChanged.Broadcast();

	APlayerController* Controller = PendingController.Get();
	AMatterFluxPlayerState* PlayerState = Controller
		? Controller->GetPlayerState<AMatterFluxPlayerState>()
		: nullptr;
	UMatterFluxMagicInventoryComponent* Inventory = PlayerState
		? PlayerState->GetMagicInventory()
		: nullptr;
	UMatterFluxProgressionComponent* Progression = PlayerState
		? PlayerState->GetProgression()
		: nullptr;
	FString Error;
	if (!Controller || !Inventory || !Progression)
	{
		FinishOperation(false, TEXT("生成完成后找不到玩家法术背包"));
		return;
	}
	if (PendingGenerationPurpose == EPendingGenerationPurpose::LoadGame)
	{
		if (!PendingLoadedSave
			|| !World->RestoreSaveState(PendingLoadedSave->WorldState, Error)
			|| !Inventory->RestoreSaveStateAuthority(
				PendingLoadedSave->MagicInventory,
				Error)
			|| !Progression->RestoreSaveStateAuthority(
				PendingLoadedSave->Progression,
				Error)
			|| (PendingLoadedSave->CustomMapId.IsNone()
				&& !Progression->ClearStoryQuestsAuthority(Error)))
		{
			FinishOperation(false, Error.IsEmpty()
				? TEXT("存档状态恢复失败") : Error);
			return;
		}
		if (APawn* Pawn = Controller->GetPawn())
		{
			Pawn->SetActorTransform(
				PendingLoadedSave->PlayerTransform,
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
		}
	}
	else if ((PendingGenerationPurpose == EPendingGenerationPurpose::StoryGame
			? !Inventory->ResetToEmptyLoadoutAuthority(Error)
			: !Inventory->ResetToStarterLoadoutAuthority(Error))
		|| (PendingGenerationPurpose == EPendingGenerationPurpose::StoryGame
			? !Progression->ResetToStoryStateAuthority(Error)
			: !Progression->ResetToFreeModeStateAuthority(Error)))
	{
		FinishOperation(false, Error);
		return;
	}
	if (PendingGenerationPurpose == EPendingGenerationPurpose::NewGame
		|| PendingGenerationPurpose == EPendingGenerationPurpose::StoryGame)
	{
		AMatterFluxGameMode* GameMode = World->GetWorld()
			? Cast<AMatterFluxGameMode>(World->GetWorld()->GetAuthGameMode())
			: nullptr;
		if (!GameMode)
		{
			FinishOperation(false, TEXT("无法找到玩家生成规则"));
			return;
		}
		if (PendingGenerationPurpose == EPendingGenerationPurpose::StoryGame)
		{
			FVector PlayerStart;
			if (!World->TryGetCustomMapMarker(TEXT("player_start"), PlayerStart))
			{
				FinishOperation(false, TEXT("故事地图缺少主角出生点"));
				return;
			}
			if (APawn* ExistingPawn = Controller->GetPawn())
			{
				Controller->UnPossess();
				ExistingPawn->Destroy();
			}
		}
		GameMode->PrepareForInitialWorldEntry();
		// The title screen owns only a spectator view target. Keep the controller
		// in the terrain-safe entry queue. The save operation remains active until
		// that queue passes the full terrain/population/creature entry barrier.
		if (!Controller->GetPawn())
		{
			GameMode->HandleStartingNewPlayer_Implementation(Controller);
		}
		PendingLoadedSave = nullptr;
		BeginWaitingForWorldInitialization();
		return;
	}
	if (PendingGenerationPurpose == EPendingGenerationPurpose::LoadGame)
	{
		PendingLoadedSave = nullptr;
		BeginWaitingForWorldInitialization();
		return;
	}
	PendingLoadedSave = nullptr;
	FinishOperation(true,
		PendingGenerationPurpose == EPendingGenerationPurpose::LoadGame
			? TEXT("存档加载完成")
			: PendingGenerationPurpose == EPendingGenerationPurpose::StoryGame
				? TEXT("故事模式已开始")
				: TEXT("自由模式地图生成完成"));
}

void UMatterFluxSaveSubsystem::BeginWaitingForWorldInitialization()
{
	AMatterFluxPlayableWorldActor* World = PendingWorld.Get();
	UWorld* GameWorld = World ? World->GetWorld() : nullptr;
	if (!World || !GameWorld || !PendingController.IsValid())
	{
		FinishOperation(false, TEXT("无法继续世界初始化"));
		return;
	}
	ResetWorldInitializationProgress();
	MaximumPendingTerrainInitializationWork =
		World->GetPendingTerrainChunkPrefetchCount();
	MaximumPendingPopulationInitializationWork =
		World->GetPendingProceduralPopulationUpdateCount();
	OperationProgress = FinalGenerationProgress;
	OperationStatus = PendingGenerationPurpose
		== EPendingGenerationPurpose::StoryGame
		? TEXT("正在加载故事区域地形、装饰与水体…")
		: PendingGenerationPurpose == EPendingGenerationPurpose::LoadGame
			? TEXT("正在加载存档区域地形、装饰与碰撞…")
			: TEXT("正在加载出生区域地形与碰撞…");
	GameWorld->GetTimerManager().SetTimer(
		WorldInitializationTimer,
		this,
		&UMatterFluxSaveSubsystem::PollWorldInitialization,
		0.05f,
		true,
		0.0f);
}

void UMatterFluxSaveSubsystem::PollWorldInitialization()
{
	if (Operation != EMatterFluxSaveOperation::ApplyingWorld
		|| (PendingGenerationPurpose != EPendingGenerationPurpose::NewGame
			&& PendingGenerationPurpose
				!= EPendingGenerationPurpose::StoryGame
			&& PendingGenerationPurpose
				!= EPendingGenerationPurpose::LoadGame))
	{
		if (UWorld* World = GetGameInstance()
			? GetGameInstance()->GetWorld() : nullptr)
		{
			World->GetTimerManager().ClearTimer(WorldInitializationTimer);
		}
		return;
	}

	AMatterFluxPlayableWorldActor* World = PendingWorld.Get();
	APlayerController* Controller = PendingController.Get();
	if (!World || !Controller)
	{
		FinishOperation(false, TEXT("世界初始化期间丢失世界或玩家"));
		return;
	}

	const int32 PendingTerrainWork =
		World->GetPendingTerrainChunkPrefetchCount();
	const int32 PendingPopulationWork =
		World->GetPendingProceduralPopulationUpdateCount();
	MaximumPendingTerrainInitializationWork = FMath::Max(
		MaximumPendingTerrainInitializationWork,
		PendingTerrainWork);
	MaximumPendingPopulationInitializationWork = FMath::Max(
		MaximumPendingPopulationInitializationWork,
		PendingPopulationWork);
	OperationProgress = MapWorldInitializationProgress(
		PendingTerrainWork,
		MaximumPendingTerrainInitializationWork,
		PendingPopulationWork,
		MaximumPendingPopulationInitializationWork,
		OperationProgress);

	if (PendingTerrainWork > 0)
	{
		OperationStatus = FString::Printf(
			TEXT("正在加载地形与碰撞（剩余 %d 项）…"),
			PendingTerrainWork);
	}
	else if (PendingPopulationWork > 0)
	{
		OperationStatus = FString::Printf(
			TEXT("正在生成植被、装饰与河流表现（剩余 %d 项）…"),
			PendingPopulationWork);
	}
	else if (!World->IsInitialWorldEntryReady())
	{
		OperationStatus = TEXT("正在完成材质表现与物理初始化…");
	}
	else if (!Controller->GetPawn())
	{
		OperationStatus = TEXT("正在生成玩家角色…");
	}
	else
	{
		if (PendingGenerationPurpose == EPendingGenerationPurpose::LoadGame
			&& World->IsCustomMapActive())
		{
			AMatterFluxGameMode* GameMode = World->GetWorld()
				? Cast<AMatterFluxGameMode>(
					World->GetWorld()->GetAuthGameMode())
				: nullptr;
			if (!GameMode || !GameMode->CompleteExistingPlayerWorldLoad())
			{
				FinishOperation(false, TEXT("存档世界准备完成后无法恢复生物生成"));
				return;
			}
		}
		FinishOperation(
			true,
			PendingGenerationPurpose == EPendingGenerationPurpose::StoryGame
				? TEXT("故事模式已开始")
				: PendingGenerationPurpose == EPendingGenerationPurpose::LoadGame
					? TEXT("存档加载完成")
					: TEXT("自由模式初始化完成"));
	}
}

void UMatterFluxSaveSubsystem::ResetWorldInitializationProgress()
{
	MaximumPendingTerrainInitializationWork = 0;
	MaximumPendingPopulationInitializationWork = 0;
}

bool UMatterFluxSaveSubsystem::DeleteSlot(const int32 SlotIndex)
{
	if (IsBusy() || !Metadata || !FindSlot(SlotIndex))
	{
		return false;
	}
	const bool bDeleted = !UGameplayStatics::DoesSaveGameExist(
		MakeSlotName(SlotIndex), UserIndex)
		|| UGameplayStatics::DeleteGameInSlot(
			MakeSlotName(SlotIndex), UserIndex);
	if (bDeleted)
	{
		Metadata->Slots.RemoveAll(
			[SlotIndex](const FMatterFluxSaveSlotInfo& Slot)
			{
				return Slot.SlotIndex == SlotIndex;
			});
		SaveMetadata();
		OperationChanged.Broadcast();
	}
	return bDeleted;
}

void UMatterFluxSaveSubsystem::FinishOperation(
	const bool bSuccess,
	const FString& Message)
{
	if (UWorld* GameWorld = GetGameInstance()
		? GetGameInstance()->GetWorld() : nullptr)
	{
		GameWorld->GetTimerManager().ClearTimer(WorldInitializationTimer);
	}
	WorldInitializationTimer.Invalidate();
	ResetWorldInitializationProgress();
	if (AMatterFluxPlayableWorldActor* World = PendingWorld.Get();
		World && WorldGenerationHandle.IsValid())
	{
		World->OnGenerationFinished().Remove(WorldGenerationHandle);
	}
	WorldGenerationHandle.Reset();
	Operation = bSuccess
		? EMatterFluxSaveOperation::Complete
		: EMatterFluxSaveOperation::Failed;
	OperationProgress = bSuccess ? 1.0f : 0.0f;
	OperationStatus = Message;
	LastResultMessage = Message;
	PendingGenerationPurpose = EPendingGenerationPurpose::None;
	PendingWorld.Reset();
	PendingController.Reset();
	PendingLoadedSave = nullptr;
	ActiveSlotIndex = INDEX_NONE;
	DuplicateDestinationSlotIndex = INDEX_NONE;
	PendingDuplicateDisplayName.Reset();
	if (bSuccess)
	{
		UE_LOG(LogMatterFlux, Display,
			TEXT("Save operation completed: %s"),
			*Message);
	}
	else
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Save operation failed: %s"),
			*Message);
	}
	OperationChanged.Broadcast();
}

void UMatterFluxSaveSubsystem::AcknowledgeResult()
{
	if (Operation != EMatterFluxSaveOperation::Complete
		&& Operation != EMatterFluxSaveOperation::Failed)
	{
		return;
	}
	Operation = EMatterFluxSaveOperation::Idle;
	OperationProgress = 0.0f;
	OperationStatus.Reset();
	OperationChanged.Broadcast();
}
