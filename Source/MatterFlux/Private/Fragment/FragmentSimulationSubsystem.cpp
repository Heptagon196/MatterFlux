#include "Fragment/FragmentSimulationSubsystem.h"

#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "HAL/IConsoleManager.h"
#include "MatterFluxLog.h"

namespace
{
	int32 GMatterFluxFragmentDebug = 0;
	TAutoConsoleVariable<int32> CVarMatterFluxRenderOnlyFragmentSpawnsPerFrame(
		TEXT("mf.Fragment.RenderOnlySpawnsPerFrame"),
		4,
		TEXT("Maximum render-only fragment actors materialized per world frame."),
		ECVF_Default);
	TAutoConsoleVariable<int32> CVarMatterFluxWorldCutsPerFrame(
		TEXT("mf.Fragment.WorldCutsPerFrame"),
		1,
		TEXT("Maximum accepted world-cut commands executed per server frame."),
		ECVF_Default);

	AFragment2DSourceActor* FindFirstSource(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}

		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			if (!It->IsHidden())
			{
				return *It;
			}
		}

		return nullptr;
	}

	void RequestDamageOnFirstSource(UWorld* World, const FFragmentDamageShape& Shape, const float DamagePower, const int32 EventSeed)
	{
		AFragment2DSourceActor* Source = FindFirstSource(World);
		if (!Source)
		{
			UE_LOG(LogMatterFlux, Warning, TEXT("No Fragment2DSourceActor found for debug damage command."));
			return;
		}

		UFragmentSimulationSubsystem* Subsystem = World->GetSubsystem<UFragmentSimulationSubsystem>();
		if (!Subsystem)
		{
			return;
		}

		FFragmentDamageEvent Event;
		Event.SourceId = Source->SourceId;
		Event.BaseRevision = Source->Revision;
		Event.DamageShape = Shape;
		Event.DamagePower = DamagePower;
		Event.EventSeed = EventSeed;
		Subsystem->RequestFragmentDamage(Source, Event);
	}

	FAutoConsoleCommandWithWorldAndArgs GFragmentDebugCommand(
		TEXT("mf.Fragment.Debug"),
		TEXT("Enable or disable MatterFlux fragment debug logging: mf.Fragment.Debug 0|1"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() > 0)
			{
				GMatterFluxFragmentDebug = FCString::Atoi(*Args[0]) != 0 ? 1 : 0;
			}
			UE_LOG(LogMatterFlux, Log, TEXT("Fragment debug: %d"), GMatterFluxFragmentDebug);
		}));

	FAutoConsoleCommandWithWorldAndArgs GForceDamageCircleCommand(
		TEXT("mf.Fragment.ForceDamageCircle"),
		TEXT("Apply a circle damage shape to the first Fragment2DSourceActor: mf.Fragment.ForceDamageCircle <radius>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AFragment2DSourceActor* Source = FindFirstSource(World);
			if (!Source)
			{
				UE_LOG(LogMatterFlux, Warning, TEXT("No Fragment2DSourceActor found."));
				return;
			}

			FFragmentDamageShape Shape;
			Shape.Type = EFragmentDamageShapeType::Circle;
			Shape.WorldTransform = Source->GetActorTransform();
			Shape.Radius = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 180.0f;
			RequestDamageOnFirstSource(World, Shape, 1200.0f, 1337);
		}));

	FAutoConsoleCommandWithWorldAndArgs GForceDamageLineCommand(
		TEXT("mf.Fragment.ForceDamageLine"),
		TEXT("Apply a line damage shape to the first Fragment2DSourceActor: mf.Fragment.ForceDamageLine <length> <thickness>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			AFragment2DSourceActor* Source = FindFirstSource(World);
			if (!Source)
			{
				UE_LOG(LogMatterFlux, Warning, TEXT("No Fragment2DSourceActor found."));
				return;
			}

			FFragmentDamageShape Shape;
			Shape.Type = EFragmentDamageShapeType::Line;
			Shape.WorldTransform = Source->GetActorTransform();
			Shape.Extents.X = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 2000.0f;
			Shape.Thickness = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 80.0f;
			RequestDamageOnFirstSource(World, Shape, 1200.0f, 1337);
		}));
}

void UFragmentSimulationSubsystem::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);
	const int32 CutBudget = FMath::Clamp(
		CVarMatterFluxWorldCutsPerFrame.GetValueOnGameThread(),
		1,
		4);
	int32 CutsProcessed = 0;
	while (NextQueuedWorldCut < QueuedWorldCuts.Num()
		&& CutsProcessed < CutBudget)
	{
		ExecuteWorldCut(GetWorld(), QueuedWorldCuts[NextQueuedWorldCut]);
		++NextQueuedWorldCut;
		++CutsProcessed;
	}
	if (NextQueuedWorldCut >= QueuedWorldCuts.Num())
	{
		QueuedWorldCuts.Reset();
		NextQueuedWorldCut = 0;
	}

	const int32 SpawnBudget = FMath::Clamp(
		CVarMatterFluxRenderOnlyFragmentSpawnsPerFrame.GetValueOnGameThread(),
		1,
		16);
	int32 SpawnedThisFrame = 0;
	while (NextQueuedFragmentSpawn < QueuedFragmentSpawns.Num()
		&& SpawnedThisFrame < SpawnBudget)
	{
		SpawnQueuedFragment(QueuedFragmentSpawns[NextQueuedFragmentSpawn]);
		++NextQueuedFragmentSpawn;
		++SpawnedThisFrame;
	}
	if (NextQueuedFragmentSpawn >= QueuedFragmentSpawns.Num())
	{
		QueuedFragmentSpawns.Reset();
		NextQueuedFragmentSpawn = 0;
	}
}

TStatId UFragmentSimulationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UFragmentSimulationSubsystem,
		STATGROUP_Tickables);
}

void UFragmentSimulationSubsystem::Deinitialize()
{
	QueuedFragmentSpawns.Reset();
	NextQueuedFragmentSpawn = 0;
	QueuedWorldCuts.Reset();
	NextQueuedWorldCut = 0;
	RegisteredSources.Reset();
	RegisteredSourceIndex.Reset();
	Super::Deinitialize();
}

void UFragmentSimulationSubsystem::EnqueueRenderOnlyFragments(
	const TSubclassOf<AFragment2DActor> ActorClass,
	const TArray<FFragmentSpawnPayload>& Payloads,
	UMaterialInterface* Material,
	const FLinearColor& Color)
{
	QueuedFragmentSpawns.Reserve(
		QueuedFragmentSpawns.Num() + Payloads.Num());
	for (const FFragmentSpawnPayload& Payload : Payloads)
	{
		FQueuedFragmentSpawn& Queued =
			QueuedFragmentSpawns.AddDefaulted_GetRef();
		Queued.ActorClass = ActorClass;
		Queued.Payload = Payload;
		Queued.Material = Material;
		Queued.Color = Color;
	}
}

bool UFragmentSimulationSubsystem::SpawnQueuedFragment(
	const FQueuedFragmentSpawn& QueuedSpawn)
{
	UWorld* World = GetWorld();
	if (!World || !QueuedSpawn.ActorClass)
	{
		return false;
	}
	AFragment2DActor* FragmentActor =
		World->SpawnActorDeferred<AFragment2DActor>(
			QueuedSpawn.ActorClass,
			QueuedSpawn.Payload.InitialTransform,
			nullptr,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!FragmentActor)
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Queued render-only fragment %s could not be spawned."),
			*QueuedSpawn.Payload.FragmentId.ToString(EGuidFormats::Digits));
		return false;
	}
	FragmentActor->SpawnPayload = QueuedSpawn.Payload;
	FragmentActor->FragmentMaterial = QueuedSpawn.Material.Get();
	FragmentActor->FragmentColor = QueuedSpawn.Color;
#if WITH_EDITOR
	FragmentActor->SetActorLabel(FString::Printf(
		TEXT("Fragment_%s"),
		*QueuedSpawn.Payload.FragmentId.ToString(EGuidFormats::Short)));
#endif
	FragmentActor->Tags.AddUnique(TEXT("MatterFluxFragment"));
	FragmentActor->FinishSpawning(QueuedSpawn.Payload.InitialTransform);
	if (!IsValid(FragmentActor)
		|| !FragmentActor->InitializeFromPayload(QueuedSpawn.Payload))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Queued render-only fragment %s could not initialize."),
			*QueuedSpawn.Payload.FragmentId.ToString(EGuidFormats::Digits));
		if (IsValid(FragmentActor))
		{
			World->DestroyActor(FragmentActor);
		}
		return false;
	}
	return true;
}

bool UFragmentSimulationSubsystem::RequestFragmentDamage(AFragment2DSourceActor* SourceActor, const FFragmentDamageEvent& DamageEvent)
{
	return ExecuteFragmentDamage(SourceActor, DamageEvent);
}

void UFragmentSimulationSubsystem::RegisterSourceActor(
	AFragment2DSourceActor& SourceActor)
{
	if (!SourceActor.SourceId.IsValid())
	{
		return;
	}
	const TWeakObjectPtr<AFragment2DSourceActor>* Existing =
		RegisteredSources.Find(SourceActor.SourceId);
	if (Existing && Existing->Get() == &SourceActor)
	{
		return;
	}
	RegisteredSources.Add(SourceActor.SourceId, &SourceActor);
	RegisteredSourceIndex.Upsert(
		SourceActor.SourceId,
		SourceActor.GetComponentsBoundingBox(true));
	SourcePresenceChanged.Broadcast(SourceActor.SourceId, true);
}

void UFragmentSimulationSubsystem::UnregisterSourceActor(
	AFragment2DSourceActor& SourceActor)
{
	const FGuid SourceId = SourceActor.RegisteredPresenceSourceId.IsValid()
		? SourceActor.RegisteredPresenceSourceId
		: SourceActor.SourceId;
	const TWeakObjectPtr<AFragment2DSourceActor>* Existing =
		RegisteredSources.Find(SourceId);
	if (!Existing || Existing->Get() != &SourceActor)
	{
		return;
	}
	RegisteredSources.Remove(SourceId);
	RegisteredSourceIndex.Remove(SourceId);
	SourcePresenceChanged.Broadcast(SourceId, false);
}

void UFragmentSimulationSubsystem::GatherSourcesInBounds(
	const FBox& WorldBounds,
	TArray<AFragment2DSourceActor*>& OutSources)
{
	GatherSourcesInBoundsMany(
		MakeArrayView(&WorldBounds, 1),
		OutSources);
}

void UFragmentSimulationSubsystem::GatherSourcesInBoundsMany(
	const TConstArrayView<FBox> WorldBounds,
	TArray<AFragment2DSourceActor*>& OutSources)
{
	OutSources.Reset();
	TArray<FBox> ValidBounds;
	ValidBounds.Reserve(WorldBounds.Num());
	for (const FBox& Bounds : WorldBounds)
	{
		if (Bounds.IsValid)
		{
			ValidBounds.Add(Bounds);
		}
	}
	if (ValidBounds.IsEmpty())
	{
		return;
	}

	TArray<FGuid> SourceIds;
	RegisteredSourceIndex.QueryMany(ValidBounds, SourceIds);
	OutSources.Reserve(SourceIds.Num());
	for (const FGuid& SourceId : SourceIds)
	{
		const TWeakObjectPtr<AFragment2DSourceActor>* Registered =
			RegisteredSources.Find(SourceId);
		AFragment2DSourceActor* Source = Registered
			? Registered->Get()
			: nullptr;
		if (!IsValid(Source) || Source->IsActorBeingDestroyed())
		{
			continue;
		}
		const FBox CurrentBounds = Source->GetComponentsBoundingBox(true);
		if (!CurrentBounds.IsValid)
		{
			continue;
		}
		for (const FBox& Bounds : ValidBounds)
		{
			if (CurrentBounds.Intersect(Bounds))
			{
				OutSources.Add(Source);
				break;
			}
		}
	}
}

int32 UFragmentSimulationSubsystem::RequestWorldCut(
	const FFragmentWorldCutRequest& Request)
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return ExecuteWorldCut(World, Request);
	}
	if (World->GetNetMode() == NM_Client
		|| !Request.CutShape.WorldTransform.IsValid()
		|| !FMath::IsFinite(Request.DamagePower)
		|| Request.DamagePower < 0.0f
		|| !FMath::IsFinite(Request.TargetPadding)
		|| Request.TargetPadding < 0.0f
		|| QueuedWorldCuts.Num() - NextQueuedWorldCut >= 64)
	{
		return 0;
	}
	const FFragmentDamageShape& Shape = Request.CutShape;
	const bool bShapeValid =
		(Shape.Type == EFragmentDamageShapeType::Circle
			&& FMath::IsFinite(Shape.Radius)
			&& Shape.Radius > 0.0f)
		|| (Shape.Type == EFragmentDamageShapeType::Box
			&& FMath::IsFinite(Shape.Extents.X)
			&& FMath::IsFinite(Shape.Extents.Y)
			&& Shape.Extents.X > 0.0
			&& Shape.Extents.Y > 0.0)
		|| (Shape.Type == EFragmentDamageShapeType::Line
			&& FMath::IsFinite(Shape.Extents.X)
			&& Shape.Extents.X > 0.0
			&& FMath::IsFinite(Shape.Thickness)
			&& Shape.Thickness > 0.0f);
	if (!bShapeValid)
	{
		return 0;
	}
	QueuedWorldCuts.Add(Request);
	// In game worlds this is an accepted-command count. The actual changed
	// source count is known only when the bounded queue executes the request.
	return 1;
}

int32 UFragmentSimulationSubsystem::ExecuteWorldCut(
	UWorld* World,
	const FFragmentWorldCutRequest& Request)
{
	const FFragmentDamageShape& Shape = Request.CutShape;
	if (!World
		|| !Shape.WorldTransform.IsValid()
		|| !FMath::IsFinite(Request.DamagePower)
		|| Request.DamagePower < 0.0f
		|| !FMath::IsFinite(Request.TargetPadding)
		|| Request.TargetPadding < 0.0f)
	{
		return 0;
	}

	double ShapeRadius = 0.0;
	switch (Shape.Type)
	{
	case EFragmentDamageShapeType::Circle:
		ShapeRadius = Shape.Radius;
		break;
	case EFragmentDamageShapeType::Box:
		ShapeRadius = Shape.Extents.Size();
		break;
	case EFragmentDamageShapeType::Line:
		ShapeRadius = FVector2D(
			Shape.Extents.X * 0.5,
			Shape.Thickness * 0.5).Size();
		break;
	default:
		return 0;
	}
	if (!FMath::IsFinite(ShapeRadius) || ShapeRadius <= 0.0)
	{
		return 0;
	}
	ShapeRadius += Request.TargetPadding;

	const FVector ShapeCenter =
		Shape.WorldTransform.GetLocation();
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		It->MaterializeFragmentSourcesForDamage(Shape);
	}
	const FBox QueryBounds = FBox::BuildAABB(
		ShapeCenter,
		FVector(ShapeRadius));
	TArray<AFragment2DSourceActor*> CandidateSources;
	if (UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>())
	{
		Subsystem->GatherSourcesInBounds(
			QueryBounds,
			CandidateSources);
	}
	int32 AcceptedCuts = 0;
	for (AFragment2DSourceActor* Source : CandidateSources)
	{
		if (!IsValid(Source)
			|| Source->IsActorBeingDestroyed()
			|| Source->bBroken
			|| !Source->SourceId.IsValid())
		{
			continue;
		}

		const FBox Bounds =
			Source->GetComponentsBoundingBox(true);
		if (!Bounds.IsValid)
		{
			continue;
		}
		FFragmentDamageEvent Event;
		Event.SourceId = Source->SourceId;
		Event.BaseRevision = Source->Revision;
		Event.DamageShape = Shape;
		Event.DamagePower = Request.DamagePower;
		Event.EventSeed = Request.EventSeed
			^ static_cast<int32>(
				GetTypeHash(Source->SourceId));
		AcceptedCuts += ExecuteFragmentDamage(Source, Event)
			? 1
			: 0;
	}
	return AcceptedCuts;
}

bool UFragmentSimulationSubsystem::ExecuteFragmentDamage(AFragment2DSourceActor* SourceActor, const FFragmentDamageEvent& DamageEvent)
{
	if (!SourceActor)
	{
		return false;
	}

	UWorld* World = SourceActor->GetWorld();
	if (!World)
	{
		return false;
	}

	if (World->IsGameWorld() && !SourceActor->HasAuthority())
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Rejected non-authority fragment damage request on %s."), *SourceActor->GetName());
		return false;
	}

	AFragment2DSourceActor::FPreparedFragmentDamage Transaction;
	if (!SourceActor->PrepareDamageEvent(DamageEvent, Transaction))
	{
		return false;
	}

	TArray<AFragment2DActor*> DeferredFragments;
	DeferredFragments.Reserve(Transaction.Payloads.Num());
	auto RollBackDeferredSpawns = [&DeferredFragments, World]()
	{
		for (AFragment2DActor* Fragment : DeferredFragments)
		{
			if (IsValid(Fragment))
			{
				World->DestroyActor(Fragment);
			}
		}
		DeferredFragments.Reset();
	};

	if (!Transaction.Payloads.IsEmpty() && !SourceActor->FragmentActorClass)
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Fragment actor class is invalid on %s; damage transaction was rolled back."), *SourceActor->GetName());
		return false;
	}

	const bool bCanDeferRenderOnlyFragments =
		World->IsGameWorld()
		&& !SourceActor->bEnableSourceCollision
		// The stock actor has already passed complete payload validation.
		// Custom actor classes may add fallible initialization and therefore
		// retain the strict spawn-before-commit transaction below.
		&& SourceActor->FragmentActorClass == AFragment2DActor::StaticClass()
		&& !Transaction.Payloads.IsEmpty();
	if (bCanDeferRenderOnlyFragments)
	{
		UFragmentSimulationSubsystem* Subsystem =
			World->GetSubsystem<UFragmentSimulationSubsystem>();
		if (!Subsystem || !SourceActor->CommitPreparedDamage(Transaction))
		{
			return false;
		}
		Subsystem->EnqueueRenderOnlyFragments(
			SourceActor->FragmentActorClass,
			Transaction.Payloads,
			SourceActor->FragmentMaterial,
			SourceActor->FragmentColor);
		const bool bReturnedToBatch =
			Cast<AMatterFluxPlayableWorldActor>(SourceActor->GetOwner())
				? CastChecked<AMatterFluxPlayableWorldActor>(
					SourceActor->GetOwner())
					->DematerializeFragmentSource(SourceActor->SourceId)
				: false;
		if (!bReturnedToBatch
			&& !SourceActor->GetRuntimeMask().Contains(1))
		{
			SourceActor->MarkBroken();
		}
		if (GMatterFluxFragmentDebug != 0)
		{
			UE_LOG(
				LogMatterFlux,
				Log,
				TEXT("Fragment damage queued %d render-only fragment actors."),
				Transaction.Payloads.Num());
		}
		return true;
	}

	for (const FFragmentSpawnPayload& Payload : Transaction.Payloads)
	{
		AFragment2DActor* FragmentActor = World->SpawnActorDeferred<AFragment2DActor>(
			SourceActor->FragmentActorClass, Payload.InitialTransform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!FragmentActor)
		{
			UE_LOG(LogMatterFlux, Error, TEXT("Fragment %s could not be materialized; damage transaction was rolled back."), *Payload.FragmentId.ToString(EGuidFormats::Digits));
			RollBackDeferredSpawns();
			return false;
		}
		FragmentActor->SpawnPayload = Payload;
		FragmentActor->FragmentMaterial = SourceActor->FragmentMaterial;
		FragmentActor->FragmentColor = SourceActor->FragmentColor;
#if WITH_EDITOR
		FragmentActor->SetActorLabel(FString::Printf(TEXT("Fragment_%s"), *Payload.FragmentId.ToString(EGuidFormats::Short)));
#endif
		FragmentActor->Tags.AddUnique(TEXT("MatterFluxFragment"));
		DeferredFragments.Add(FragmentActor);
	}

	for (int32 Index = 0; Index < DeferredFragments.Num(); ++Index)
	{
		AFragment2DActor* FragmentActor = DeferredFragments[Index];
		const FFragmentSpawnPayload& Payload = Transaction.Payloads[Index];
		FragmentActor->FinishSpawning(Payload.InitialTransform);
		if (!IsValid(FragmentActor) || !FragmentActor->InitializeFromPayload(Payload))
		{
			UE_LOG(LogMatterFlux, Error, TEXT("Fragment %s could not initialize from its payload; damage transaction was rolled back."),
				*Payload.FragmentId.ToString(EGuidFormats::Digits));
			RollBackDeferredSpawns();
			return false;
		}
	}

	if (!SourceActor->CommitPreparedDamage(Transaction))
	{
		RollBackDeferredSpawns();
		return false;
	}

	if (!DeferredFragments.IsEmpty())
	{
		if (SourceActor->AggregateId.IsValid()
			&& SourceActor->bAggregateRoot)
		{
			for (TActorIterator<AMatterFluxPlayableWorldActor> It(
				SourceActor->GetWorld());
				It;
				++It)
			{
				It->MaterializeFragmentAggregate(
					SourceActor->AggregateId);
			}
		}
		SourceActor->TransferAggregateMembersTo(
			*DeferredFragments[0]);
	}

	const bool bReturnedToBatch =
		Cast<AMatterFluxPlayableWorldActor>(SourceActor->GetOwner())
			? CastChecked<AMatterFluxPlayableWorldActor>(
				SourceActor->GetOwner())
				->DematerializeFragmentSource(SourceActor->SourceId)
			: false;
	if (!bReturnedToBatch
		&& !SourceActor->GetRuntimeMask().Contains(1))
	{
		SourceActor->MarkBroken();
	}

	if (GMatterFluxFragmentDebug != 0)
	{
		UE_LOG(LogMatterFlux, Log, TEXT("Fragment damage spawned %d fragment actors."), Transaction.Payloads.Num());
	}

	return true;
}
