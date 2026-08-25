#include "Fragment/FragmentSimulationSubsystem.h"

#include "Algo/Count.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "HAL/IConsoleManager.h"
#include "MatterFluxLog.h"

namespace
{
	int32 GMatterFluxFragmentDebug = 0;
	int32 GMatterFluxFragmentCutLog = 0;
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

	FAutoConsoleCommandWithWorldAndArgs GFragmentCutLogCommand(
		TEXT("mf.Fragment.CutLog"),
		TEXT("Enable or disable one-shot world-cut diagnostics: mf.Fragment.CutLog 0|1"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			if (Args.Num() > 0)
			{
				GMatterFluxFragmentCutLog = FCString::Atoi(*Args[0]) != 0 ? 1 : 0;
			}
			UE_LOG(
				LogMatterFlux,
				Display,
				TEXT("[FragmentCut] logging=%s"),
				GMatterFluxFragmentCutLog != 0 ? TEXT("on") : TEXT("off"));
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

bool UFragmentSimulationSubsystem::IsCutLoggingEnabled()
{
	return GMatterFluxFragmentCutLog != 0;
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

bool UFragmentSimulationSubsystem::RegisterSourceActor(
	AFragment2DSourceActor& SourceActor)
{
	if (!SourceActor.SourceId.IsValid())
	{
		return false;
	}
	const TWeakObjectPtr<AFragment2DSourceActor>* Existing =
		RegisteredSources.Find(SourceActor.SourceId);
	const bool bAlreadyRegistered =
		Existing && Existing->Get() == &SourceActor;
	if (!RegisteredSourceIndex.Upsert(
		SourceActor.SourceId,
		SourceActor.GetCanonicalWorldBounds()))
	{
		return false;
	}
	RegisteredSources.Add(SourceActor.SourceId, &SourceActor);
	if (!bAlreadyRegistered)
	{
		SourcePresenceChanged.Broadcast(SourceActor.SourceId, true);
	}
	return true;
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
		const FBox CurrentBounds = Source->GetCanonicalWorldBounds();
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
		|| Request.MaxAffectedSources < 0
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
		|| Request.TargetPadding < 0.0f
		|| Request.MaxAffectedSources < 0)
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
	if (IsCutLoggingEnabled())
	{
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("[FragmentCut] begin seed=%d shape=%d center=(%.1f,%.1f,%.1f) length=%.1f thickness=%.1f radius=%.1f power=%.1f maxTargets=%d"),
			Request.EventSeed,
			static_cast<int32>(Shape.Type),
			ShapeCenter.X,
			ShapeCenter.Y,
			ShapeCenter.Z,
			Shape.Extents.X,
			Shape.Thickness,
			Shape.Radius,
			Request.DamagePower,
			Request.MaxAffectedSources);
	}
	int32 AcceptedTerrainCuts = 0;
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		It->MaterializeFragmentSourcesForDamage(Shape);
		AcceptedTerrainCuts +=
			It->ApplyTerrainDamage(Shape, Request.DamagePower) > 0
				? 1
				: 0;
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
	const auto GuidLess = [](const FGuid& Left, const FGuid& Right)
	{
		return Left.A != Right.A ? Left.A < Right.A
			: Left.B != Right.B ? Left.B < Right.B
			: Left.C != Right.C ? Left.C < Right.C
			: Left.D < Right.D;
	};

	// 世界切割的预算单位是“逻辑物体”，不是组成物体的二维 source
	// 层。一棵体素树有多层树干、枝条和树叶；旧实现按 actor 距离直接
	// 截断候选数组，最近的非根层会先耗掉预算，最终把横刀错误地显示成
	// 几块纵向木板。先按 AggregateId 分组，再由根 source 发起事务；根
	// 成功后会把同一刀口应用到所有成员并交给同一个物理 carrier。
	TMap<FGuid, AFragment2DSourceActor*> AggregateRoots;
	for (AFragment2DSourceActor* Source : CandidateSources)
	{
		if (IsValid(Source)
			&& !Source->IsActorBeingDestroyed()
			&& !Source->bBroken
			&& Source->AggregateId.IsValid()
			&& Source->bAggregateRoot)
		{
			AggregateRoots.FindOrAdd(Source->AggregateId) = Source;
		}
	}

	struct FCutTargetGroup
	{
		FGuid StableId;
		TObjectPtr<AFragment2DSourceActor> Root;
		TObjectPtr<AFragment2DActor> DetachedItem;
		TArray<TObjectPtr<AFragment2DSourceActor>> Sources;
		double DistanceSquared = TNumericLimits<double>::Max();
	};
	TArray<FCutTargetGroup> TargetGroups;
	TMap<FGuid, int32> TargetGroupIndices;
	for (AFragment2DSourceActor* Source : CandidateSources)
	{
		if (!IsValid(Source)
			|| Source->IsActorBeingDestroyed()
			|| Source->bBroken
			|| !Source->SourceId.IsValid())
		{
			continue;
		}
		const FBox Bounds = Source->GetCanonicalWorldBounds();
		if (!Bounds.IsValid)
		{
			continue;
		}

		const FGuid StableId = Source->AggregateId.IsValid()
			? Source->AggregateId
			: Source->SourceId;
		int32* GroupIndex = TargetGroupIndices.Find(StableId);
		if (!GroupIndex)
		{
			FCutTargetGroup& NewGroup = TargetGroups.AddDefaulted_GetRef();
			NewGroup.StableId = StableId;
			if (Source->AggregateId.IsValid())
			{
				NewGroup.Root = AggregateRoots.FindRef(Source->AggregateId);
			}
			TargetGroupIndices.Add(StableId, TargetGroups.Num() - 1);
			GroupIndex = TargetGroupIndices.Find(StableId);
		}
		FCutTargetGroup& Group = TargetGroups[*GroupIndex];
		Group.Sources.AddUnique(Source);
		Group.DistanceSquared = FMath::Min(
			Group.DistanceSquared,
			Bounds.ComputeSquaredDistanceToPoint(ShapeCenter));
	}
	// Detached rigid carriers are the moving projection of the same material
	// facts, not terminal debris. Include them in the same deterministic target
	// budget as static sources so a cut remains a generic world interaction after
	// an object has changed projection.
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		AFragment2DActor* Item = *It;
		if (!IsValid(Item)
			|| Item->IsActorBeingDestroyed()
			|| Item->IsCutFadeActive()
			|| !Item->SpawnPayload.FragmentId.IsValid())
		{
			continue;
		}
		const FBox Bounds = Item->GetComponentsBoundingBox(true);
		if (!Bounds.IsValid || !Bounds.Intersect(QueryBounds))
		{
			continue;
		}
		const FGuid StableId = Item->SpawnPayload.FragmentId;
		int32* GroupIndex = TargetGroupIndices.Find(StableId);
		if (!GroupIndex)
		{
			FCutTargetGroup& NewGroup = TargetGroups.AddDefaulted_GetRef();
			NewGroup.StableId = StableId;
			TargetGroupIndices.Add(StableId, TargetGroups.Num() - 1);
			GroupIndex = TargetGroupIndices.Find(StableId);
		}
		FCutTargetGroup& Group = TargetGroups[*GroupIndex];
		Group.DetachedItem = Item;
		Group.DistanceSquared = FMath::Min(
			Group.DistanceSquared,
			Bounds.ComputeSquaredDistanceToPoint(ShapeCenter));
	}
	for (FCutTargetGroup& Group : TargetGroups)
	{
		if (IsValid(Group.Root))
		{
			Group.Sources.AddUnique(Group.Root);
			// 根只决定事务提交顺序，不代表整个逻辑物体的位置。全局
			// 目标排序保留上面由所有相交成员包围盒算出的最近表面距离；
			// 否则贴着树根的一刀会因为树干中心较远而被旁边小草抢走。
		}
		Group.Sources.Sort(
			[&GuidLess, &Group, &ShapeCenter](
				const AFragment2DSourceActor& Left,
				const AFragment2DSourceActor& Right)
			{
				if (&Left == Group.Root.Get() || &Right == Group.Root.Get())
				{
					return &Left == Group.Root.Get()
						&& &Right != Group.Root.Get();
				}
				const double LeftDistance = FVector::DistSquared(
					Left.GetActorLocation(), ShapeCenter);
				const double RightDistance = FVector::DistSquared(
					Right.GetActorLocation(), ShapeCenter);
				if (LeftDistance != RightDistance)
				{
					return LeftDistance < RightDistance;
				}
				return GuidLess(Left.SourceId, Right.SourceId);
			});
	}
	TargetGroups.Sort(
		[&GuidLess](const FCutTargetGroup& Left, const FCutTargetGroup& Right)
		{
			if (Left.DistanceSquared != Right.DistanceSquared)
			{
				return Left.DistanceSquared < Right.DistanceSquared;
			}
			return GuidLess(Left.StableId, Right.StableId);
		});
	int32 AcceptedCuts = 0;
	int32 AttemptedTargets = 0;
	if (IsCutLoggingEnabled())
	{
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("[FragmentCut] candidates=%d logicalTargets=%d terrainAccepted=%d"),
			CandidateSources.Num(),
			TargetGroups.Num(),
			AcceptedTerrainCuts);
	}
	for (FCutTargetGroup& Group : TargetGroups)
	{
		if (Request.MaxAffectedSources > 0
			&& AttemptedTargets >= Request.MaxAffectedSources)
		{
			break;
		}
		++AttemptedTargets;
		bool bTargetAccepted = false;
		if (IsCutLoggingEnabled())
		{
			UE_LOG(
				LogMatterFlux,
				Display,
				TEXT("[FragmentCut] target=%s sources=%d root=%s detachedItem=%s distance=%.1f"),
				*Group.StableId.ToString(EGuidFormats::Short),
				Group.Sources.Num(),
				IsValid(Group.Root) ? *Group.Root->SourceId.ToString(EGuidFormats::Short) : TEXT("none"),
				IsValid(Group.DetachedItem) ? *Group.DetachedItem->GetName() : TEXT("none"),
				FMath::Sqrt(Group.DistanceSquared));
		}
		if (IsValid(Group.DetachedItem))
		{
			const int32 CutsBefore = Group.DetachedItem->GetAcceptedCutCount();
			bTargetAccepted = Group.DetachedItem->TryAcceptWorldCut(Shape);
			if (IsCutLoggingEnabled())
			{
				UE_LOG(
					LogMatterFlux,
					Display,
					TEXT("[FragmentCut] item=%s accepted=%s cuts=%d->%d cutFade=%s transientFade=%.2f"),
					*Group.DetachedItem->GetName(),
					bTargetAccepted ? TEXT("true") : TEXT("false"),
					CutsBefore,
					Group.DetachedItem->GetAcceptedCutCount(),
					Group.DetachedItem->IsCutFadeActive() ? TEXT("true") : TEXT("false"),
					Group.DetachedItem->SpawnPayload.FadeOutDuration);
			}
			AcceptedCuts += bTargetAccepted ? 1 : 0;
			continue;
		}
		TMap<FGuid, int32> InitialRevisions;
		for (AFragment2DSourceActor* Source : Group.Sources)
		{
			if (IsValid(Source) && Source->SourceId.IsValid())
			{
				InitialRevisions.Add(Source->SourceId, Source->Revision);
			}
		}
		FFragmentDamageEvent AggregateRootEvent;
		bool bHasAggregateRootEvent = false;
		bool bAggregateMembersTransferred = false;
		for (AFragment2DSourceActor* Source : Group.Sources)
		{
			if (!IsValid(Source)
				|| Source->IsActorBeingDestroyed()
				|| Source->bBroken
				|| !Source->SourceId.IsValid())
			{
				continue;
			}
			const int32* InitialRevision =
				InitialRevisions.Find(Source->SourceId);
			if (!InitialRevision || Source->Revision != *InitialRevision)
			{
				// 根层若已经生成 carrier，TransferAggregateMembersTo 会
				// 先提交各成员的同一刀口；这里不能再重复伤害它们。
				continue;
			}
			FFragmentDamageEvent Event;
			Event.SourceId = Source->SourceId;
			Event.BaseRevision = Source->Revision;
			Event.DamageShape = Shape;
			Event.DamagePower = Request.DamagePower;
			Event.EventSeed = Request.EventSeed
				^ static_cast<int32>(GetTypeHash(Source->SourceId));
			if (Source == Group.Root.Get())
			{
				AggregateRootEvent = Event;
				bHasAggregateRootEvent = true;
			}
			AFragment2DActor* PrimaryCarrier = nullptr;
			if (ExecuteFragmentDamage(Source, Event, &PrimaryCarrier))
			{
				bTargetAccepted = true;
				if (PrimaryCarrier && Source == Group.Root.Get())
				{
					bAggregateMembersTransferred = true;
				}
				else if (PrimaryCarrier
					&& !bAggregateMembersTransferred
					&& IsValid(Group.Root)
					&& bHasAggregateRootEvent)
				{
					const int32* RootInitialRevision =
						InitialRevisions.Find(Group.Root->SourceId);
					if (RootInitialRevision
						&& Group.Root->Revision > *RootInitialRevision)
					{
						// A wide vertical band can remove the root's entire upper
						// segment, leaving only a stump and therefore no root payload
						// to act as carrier. Promote the first persistent member body
						// and transfer every untouched canopy slice into it.
						Group.Root->TransferAggregateMembersTo(
							*PrimaryCarrier,
							&AggregateRootEvent);
						Group.Root->BeginAggregateSeparationGracePeriod(
							*PrimaryCarrier);
						if (IsCutLoggingEnabled())
						{
							UE_LOG(
								LogMatterFlux,
								Display,
								TEXT("[FragmentCut] promoted member carrier=%s aggregateRoot=%s layers=%d"),
								*GetNameSafe(PrimaryCarrier),
								*GetNameSafe(Group.Root.Get()),
								PrimaryCarrier->GetAggregateMemberCount());
						}
						bAggregateMembersTransferred = true;
					}
				}
				// 不提前终止：没有产生 carrier 的表面刻痕仍需穿过木叶
				// 多材质层。已由根事务接管的成员会在循环顶部按 revision
				// 或对象有效性跳过，整棵树仍只计作一个法术目标。
			}
		}
		AcceptedCuts += bTargetAccepted ? 1 : 0;
	}
	if (IsCutLoggingEnabled())
	{
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("[FragmentCut] end seed=%d attempted=%d objectAccepted=%d terrainAccepted=%d total=%d"),
			Request.EventSeed,
			AttemptedTargets,
			AcceptedCuts,
			AcceptedTerrainCuts,
			AcceptedCuts + AcceptedTerrainCuts);
	}
	return AcceptedCuts + AcceptedTerrainCuts;
}

bool UFragmentSimulationSubsystem::ExecuteFragmentDamage(
	AFragment2DSourceActor* SourceActor,
	const FFragmentDamageEvent& DamageEvent)
{
	return ExecuteFragmentDamage(SourceActor, DamageEvent, nullptr);
}

bool UFragmentSimulationSubsystem::ExecuteFragmentDamage(
	AFragment2DSourceActor* SourceActor,
	const FFragmentDamageEvent& DamageEvent,
	AFragment2DActor** OutPrimaryCarrier)
{
	if (OutPrimaryCarrier)
	{
		*OutPrimaryCarrier = nullptr;
	}
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
	const int32 SolidCellsBefore = Algo::Count(
		SourceActor->GetRuntimeMask(),
		static_cast<uint8>(1));
	// A non-root aggregate layer is normally collision-free because the intact
	// root owns the compound body's collision. When a vertical cut leaves the
	// root terrain-supported, however, detached branches/leaves are processed
	// independently and need their own physical bodies instead of render-only
	// actors that remain suspended in place.
	const bool bDetachedAggregateMemberNeedsPhysics =
		SourceActor->AggregateId.IsValid()
		&& !SourceActor->bAggregateRoot;
	if (!SourceActor->PrepareDamageEvent(
		DamageEvent,
		Transaction,
		bDetachedAggregateMemberNeedsPhysics))
	{
		return false;
	}
	if (IsCutLoggingEnabled())
	{
		int32 PersistentPayloads = 0;
		int32 FadingPayloads = 0;
		int32 CollisionPayloads = 0;
		for (const FFragmentSpawnPayload& Payload : Transaction.Payloads)
		{
			PersistentPayloads += Payload.FadeOutDuration <= 0.0f ? 1 : 0;
			FadingPayloads += Payload.FadeOutDuration > 0.0f ? 1 : 0;
			CollisionPayloads += Payload.bEnableCollision ? 1 : 0;
		}
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("[FragmentCut] prepared actor=%s source=%s aggregate=%s root=%s material=%s revision=%d cellsBefore=%d cellsSupported=%d payloads=%d persistent=%d fading=%d collision=%d forcePhysics=%s"),
			*SourceActor->GetName(),
			*SourceActor->SourceId.ToString(EGuidFormats::Short),
			SourceActor->AggregateId.IsValid() ? *SourceActor->AggregateId.ToString(EGuidFormats::Short) : TEXT("none"),
			SourceActor->bAggregateRoot ? TEXT("true") : TEXT("false"),
			*SourceActor->SourceMaterialId.ToString(),
			SourceActor->Revision,
			SolidCellsBefore,
			Algo::Count(Transaction.SupportedMask, static_cast<uint8>(1)),
			Transaction.Payloads.Num(),
			PersistentPayloads,
			FadingPayloads,
			CollisionPayloads,
			bDetachedAggregateMemberNeedsPhysics ? TEXT("true") : TEXT("false"));
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
		&& !bDetachedAggregateMemberNeedsPhysics
		// Aggregate roots must synchronously create their carrier so every
		// member can transfer into one authoritative falling object before the
		// root source returns to batched rendering.
		&& !(SourceActor->AggregateId.IsValid()
			&& SourceActor->bAggregateRoot)
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
			*DeferredFragments[0],
			&DamageEvent);
		if (SourceActor->AggregateId.IsValid()
			&& SourceActor->bAggregateRoot)
		{
			SourceActor->BeginAggregateSeparationGracePeriod(
				*DeferredFragments[0]);
		}
		if (OutPrimaryCarrier)
		{
			*OutPrimaryCarrier = DeferredFragments[0];
		}
	}
	if (IsCutLoggingEnabled())
	{
		UE_LOG(
			LogMatterFlux,
			Display,
			TEXT("[FragmentCut] committed actor=%s revision=%d cells=%d spawned=%d aggregateLayers=%d"),
			*SourceActor->GetName(),
			SourceActor->Revision,
			Algo::Count(SourceActor->GetRuntimeMask(), static_cast<uint8>(1)),
			DeferredFragments.Num(),
			!DeferredFragments.IsEmpty()
				? DeferredFragments[0]->GetAggregateMemberCount()
				: 0);
	}

	const bool bReturnedToBatch =
		!SourceActor->IsAggregateSeparationCollisionSuppressed()
		&& Cast<AMatterFluxPlayableWorldActor>(SourceActor->GetOwner())
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
