#include "Fragment/Fragment2DSourceActor.h"

#include "Algo/Count.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DAsset.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/MatterFluxInstanceVisuals.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "TimerManager.h"

namespace
{
	bool PackPresenceMask(
		const TArray<uint8>& Mask,
		TArray<uint8>& OutPackedMask)
	{
		OutPackedMask.Reset();
		if (Mask.IsEmpty())
		{
			return false;
		}
		OutPackedMask.Init(
			0,
			FMath::DivideAndRoundUp(Mask.Num(), 8));
		for (int32 Index = 0; Index < Mask.Num(); ++Index)
		{
			OutPackedMask[Index >> 3] |=
				static_cast<uint8>(Mask[Index] != 0)
				<< (Index & 7);
		}
		return true;
	}

	bool UnpackBinaryMask(
		const TArray<uint8>& PackedMask,
		const int32 CellCount,
		TArray<uint8>& OutMask)
	{
		OutMask.Reset();
		if (CellCount <= 0
			|| PackedMask.Num()
				!= FMath::DivideAndRoundUp(CellCount, 8))
		{
			return false;
		}
		const int32 UsedBitsInLastByte = CellCount & 7;
		if (UsedBitsInLastByte != 0
			&& (PackedMask.Last()
				>> UsedBitsInLastByte) != 0)
		{
			return false;
		}

		OutMask.SetNumUninitialized(CellCount);
		for (int32 Index = 0; Index < CellCount; ++Index)
		{
			OutMask[Index] =
				(PackedMask[Index >> 3] >> (Index & 7))
				& 1u;
		}
		return true;
	}
}

AFragment2DSourceActor::AFragment2DSourceActor()
{
	bReplicates = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bAlwaysRelevant = false;
	// One client needs the sources around its own streamed 3x3 chunk window,
	// not the union around every player on the server. Keep a little margin
	// beyond that window for fast movement and fragment hand-off.
	SetNetCullDistanceSquared(FMath::Square(1100.0f));
	FragmentActorClass = AFragment2DActor::StaticClass();

	MeshComponent = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("SourceMesh"));
	SetRootComponent(MeshComponent);
	MeshComponent->SetCanEverAffectNavigation(false);
	MeshComponent->SetCollisionObjectType(ECC_WorldStatic);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComponent->SetCollisionResponseToAllChannels(ECR_Block);
	MeshComponent->bUseComplexAsSimpleCollision = true;
}

void AFragment2DSourceActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	EnsureInitialized();
	RebuildSourceMesh();
}

void AFragment2DSourceActor::BeginPlay()
{
	Super::BeginPlay();
	EnsureInitialized();
	RebuildSourceMesh();
	RefreshPresenceRegistration();
}

void AFragment2DSourceActor::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(
			AggregateSeparationTimerHandle);
	}
	AggregateSeparationCarrier.Reset();
	if (UFragmentSimulationSubsystem* Subsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr)
	{
		Subsystem->UnregisterSourceActor(*this);
	}
	RegisteredPresenceSourceId.Invalidate();
	Super::EndPlay(EndPlayReason);
}

void AFragment2DSourceActor::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	AdvanceReaction(DeltaSeconds);
}

bool AFragment2DSourceActor::IsNetRelevantFor(
	const AActor* RealViewer,
	const AActor* ViewTarget,
	const FVector& SrcLocation) const
{
	if (bAlwaysRelevant)
	{
		return true;
	}

	// Relevancy follows the possessed pawn rather than the offset isometric
	// camera. SrcLocation is the connection's view origin, not this Source's
	// location; comparing it to ViewTarget made broken-state delivery depend on
	// camera-arm length and did not spatially cull Sources at all.
	const AActor* ViewerActor = ViewTarget ? ViewTarget : RealViewer;
	return ViewerActor
		&& FVector::DistSquared(
			GetActorLocation(),
			ViewerActor->GetActorLocation())
			<= GetNetCullDistanceSquared();
}

void AFragment2DSourceActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AFragment2DSourceActor, SourceId);
	DOREPLIFETIME_CONDITION(
		AFragment2DSourceActor,
		SourceMaterialId,
		COND_InitialOnly);
	DOREPLIFETIME_CONDITION(
		AFragment2DSourceActor,
		StructuralRole,
		COND_InitialOnly);
	DOREPLIFETIME(AFragment2DSourceActor, Revision);
	DOREPLIFETIME(AFragment2DSourceActor, bBroken);
	DOREPLIFETIME_CONDITION(
		AFragment2DSourceActor,
		AggregateId,
		COND_InitialOnly);
	DOREPLIFETIME_CONDITION(
		AFragment2DSourceActor,
		bAggregateRoot,
		COND_InitialOnly);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		bDetachedFromTerrain);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		bAggregateSeparationCollisionSuppressed);
	DOREPLIFETIME_CONDITION(AFragment2DSourceActor, FragmentMaterial, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AFragment2DSourceActor, FragmentColor, COND_InitialOnly);
	DOREPLIFETIME(AFragment2DSourceActor, ProceduralSource);
	DOREPLIFETIME(AFragment2DSourceActor, bEnableSourceCollision);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		ReplicatedReactionInputMask);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		ReplicatedReactionOutputMask);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		ReplicatedReactionActiveMask);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		ReactionRevision);
}

bool AFragment2DSourceActor::ApplyDamageEvent(const FFragmentDamageEvent& DamageEvent, TArray<FFragmentSpawnPayload>& OutPayloads)
{
	OutPayloads.Reset();
	FPreparedFragmentDamage Transaction;
	if (!PrepareDamageEvent(DamageEvent, Transaction) || !CommitPreparedDamage(Transaction))
	{
		return false;
	}
	OutPayloads = MoveTemp(Transaction.Payloads);
	return true;
}

bool AFragment2DSourceActor::InitializeFromProceduralMask(
	const FFragmentSourceMask& InMask,
	const FGuid& InSourceId,
	const FLinearColor& InColor,
	const FName InMaterialId,
	const EMatterFluxMaterialStructuralRole InStructuralRole)
{
	const bool bColorValid =
		FMath::IsFinite(InColor.R)
		&& FMath::IsFinite(InColor.G)
		&& FMath::IsFinite(InColor.B)
		&& FMath::IsFinite(InColor.A);
	if (!InMask.IsValid() || !InSourceId.IsValid() || !bColorValid
		|| (GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority()))
	{
		return false;
	}

	FragmentAsset = nullptr;
	ProceduralSource = InMask;
	SourceId = InSourceId;
	SourceMaterialId = InMaterialId;
	StructuralRole = InStructuralRole;
	FragmentColor = InColor;
	RuntimeMask = ProceduralSource.SolidMask;
	if (!MatterFlux::FragmentGeometry::BuildSupportAnchorMask(
		RuntimeMask,
		ProceduralSource.Width,
		ProceduralSource.Height,
		ProceduralSource.SupportMode,
		SupportAnchorMask))
	{
		ProceduralSource = FFragmentSourceMask();
		RuntimeMask.Reset();
		SupportAnchorMask.Reset();
		SourceId.Invalidate();
		SourceMaterialId = NAME_None;
		StructuralRole = EMatterFluxMaterialStructuralRole::None;
		RefreshPresenceRegistration();
		return false;
	}
	OutputMask.Init(0, RuntimeMask.Num());
	VisibleActiveMask.Init(0, RuntimeMask.Num());
	ReactionSimulation.Reset();
	ReactionAccumulator = 0.0f;
	ReactionVisualAccumulator = 0.0f;
	TotalMaterialEmissionCount = 0;
	bReactionVisualDirty = false;
	bReactionGeometryDirty = false;
	ReactionRevision = 0;
	ReplicatedReactionInputMask.Reset();
	ReplicatedReactionOutputMask.Reset();
	ReplicatedReactionActiveMask.Reset();
	Revision = 0;
	bBroken = false;
	if (!RebuildSourceMesh())
	{
		ProceduralSource = FFragmentSourceMask();
		RuntimeMask.Reset();
		SupportAnchorMask.Reset();
		OutputMask.Reset();
		VisibleActiveMask.Reset();
		SourceId.Invalidate();
		SourceMaterialId = NAME_None;
		StructuralRole = EMatterFluxMaterialStructuralRole::None;
		FragmentColor = FLinearColor::White;
		RefreshPresenceRegistration();
		return false;
	}
	ApplyBrokenState();
	ForceNetUpdate();
	RefreshPresenceRegistration();
	return true;
}

bool AFragment2DSourceActor::ResetForStreamingReuse(
	const FGuid& InSourceId)
{
	if (!InSourceId.IsValid()
		|| !ProceduralSource.HasValidLayout()
		|| (GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority()))
	{
		return false;
	}
	SourceId = InSourceId;
	RuntimeMask = ProceduralSource.SolidMask;
	if (!MatterFlux::FragmentGeometry::BuildSupportAnchorMask(
		RuntimeMask,
		ProceduralSource.Width,
		ProceduralSource.Height,
		ProceduralSource.SupportMode,
		SupportAnchorMask))
	{
		return false;
	}
	OutputMask.Init(0, RuntimeMask.Num());
	VisibleActiveMask.Init(0, RuntimeMask.Num());
	ReactionSimulation.Reset();
	ReactionAccumulator = 0.0f;
	ReactionVisualAccumulator = 0.0f;
	TotalMaterialEmissionCount = 0;
	bReactionVisualDirty = false;
	bReactionGeometryDirty = false;
	ReactionRevision = 0;
	ReplicatedReactionInputMask.Reset();
	ReplicatedReactionOutputMask.Reset();
	ReplicatedReactionActiveMask.Reset();
	Revision = 0;
	bBroken = false;
	AggregateId.Invalidate();
	bAggregateRoot = false;
	bDetachedFromTerrain = false;
	bAggregateSeparationCollisionSuppressed = false;
	ApplyBrokenState();
	ForceNetUpdate();
	RefreshPresenceRegistration();
	return true;
}

bool AFragment2DSourceActor::CaptureStreamingState(
	FFragment2DSourceStreamingState& OutState,
	FString& OutError) const
{
	OutState = FFragment2DSourceStreamingState();
	OutError.Reset();
	const int32 ExpectedCellCount =
		GetMaskWidth() * GetMaskHeight();
	if (!SourceId.IsValid()
		|| RuntimeMask.Num() != ExpectedCellCount
		|| RuntimeMask.ContainsByPredicate(
			[](const uint8 Value)
			{
				return Value > 1;
			})
		|| Revision < 0
		|| !FMath::IsFinite(ReactionAccumulator)
		|| ReactionAccumulator < 0.0f
		|| TotalMaterialEmissionCount < 0)
	{
		OutError =
			TEXT("Fragment source runtime state is invalid");
		return false;
	}

	OutState.Revision = Revision;
	OutState.ReactionAccumulator =
		ReactionAccumulator;
	OutState.TotalMaterialEmissionCount =
		TotalMaterialEmissionCount;
	if (ReactionSimulation)
	{
		if (!ReactionSimulation->CaptureState(
			OutState.ReactionState))
		{
			OutError =
				TEXT("Fragment source reaction state could not be captured");
			return false;
		}
		OutState.bHasReactionState = true;
	}
	OutState.SetRuntimeMask(RuntimeMask);
	return true;
}

bool AFragment2DSourceActor::RestoreStreamingState(
	const FFragment2DSourceStreamingState& State,
	FString& OutError)
{
	OutError.Reset();
	const int32 ExpectedCellCount =
		GetMaskWidth() * GetMaskHeight();
	const TArray<uint8>& StateRuntimeMask = State.GetRuntimeMask();
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| State.Revision < 0
		|| StateRuntimeMask.Num() != ExpectedCellCount
		|| StateRuntimeMask.ContainsByPredicate(
			[](const uint8 Value)
			{
				return Value > 1;
			})
		|| !FMath::IsFinite(State.ReactionAccumulator)
		|| State.ReactionAccumulator < 0.0f
		|| State.ReactionAccumulator > 0.3f
		|| State.TotalMaterialEmissionCount < 0)
	{
		OutError =
			TEXT("Cached fragment source state is invalid");
		return false;
	}

	TUniquePtr<MatterFlux::Reaction::FMaskReaction>
		RestoredReaction;
	if (State.bHasReactionState)
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		const FMatterFluxReactionDefinition* Rule = Registry.IsValid()
			? Registry->Reactions.Find(State.ReactionState.RuleId)
			: nullptr;
		if (!Rule
			|| Rule->Kind
				!= FMatterFluxReactionDefinition::EKind::Propagating
			|| Rule->InputA != SourceMaterialId)
		{
			OutError =
				TEXT("Cached reaction state does not match the source material or input mask");
			return false;
		}
		RestoredReaction =
			MakeUnique<MatterFlux::Reaction::FMaskReaction>();
		if (!RestoredReaction->RestoreState(
			State.ReactionState,
			*Rule,
			OutError))
		{
			return false;
		}
	}

	RuntimeMask = StateRuntimeMask;
	ProceduralSource.SolidMask = RuntimeMask;
	Revision = State.Revision;
	ReactionSimulation = MoveTemp(RestoredReaction);
	ReactionAccumulator =
		State.ReactionAccumulator;
	TotalMaterialEmissionCount =
		State.TotalMaterialEmissionCount;
	if (RuntimeMask.Contains(1))
	{
		if (!MatterFlux::FragmentGeometry::BuildSupportAnchorMask(
			RuntimeMask,
			GetMaskWidth(),
			GetMaskHeight(),
			ProceduralSource.SupportMode,
			SupportAnchorMask)
			|| !RebuildSourceMesh())
		{
			OutError =
				TEXT("Cached fragment source geometry could not be restored");
			return false;
		}
	}
	else
	{
		SupportAnchorMask.Init(0, RuntimeMask.Num());
		MeshComponent->ClearAllMeshSections();
		MeshComponent->ClearCollisionConvexMeshes();
		ApplySourceCollisionState();
	}

	if (ReactionSimulation)
	{
		OutputMask =
			ReactionSimulation->GetOutputMask();
		VisibleActiveMask =
			ReactionSimulation->GetActiveMask();
		bReactionGeometryDirty = true;
		bReactionVisualDirty = true;
		EnsureReactionVisualComponents();
		RebuildReactionVisualization();
		PublishReactionState();
	}
	else
	{
		OutputMask.Init(0, RuntimeMask.Num());
		VisibleActiveMask.Init(0, RuntimeMask.Num());
	}
	SetActorTickEnabled(
		ReactionSimulation
		&& ReactionSimulation->IsActive());
	ForceNetUpdate();
	return true;
}

bool AFragment2DSourceActor::ApplyMaterialStimulusAtWorldLocation(
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| bBroken
		|| SourceMaterialId.IsNone()
		|| WorldLocation.ContainsNaN()
		|| !GetActorTransform().IsValid())
	{
		return false;
	}
	const FMatterFluxReactionDefinition* Rule =
		FindReactionRule(StimulusMaterial);
	if (!Rule)
	{
		return false;
	}

	if (ReactionSimulation
		&& ReactionSimulation->GetRule().Id != Rule->Id)
	{
		if (ReactionSimulation->IsActive())
		{
			return false;
		}
		ReactionSimulation.Reset();
	}
	if (!ReactionSimulation)
	{
		FFragmentSourceMask CurrentMask = ProceduralSource;
		CurrentMask.SolidMask = RuntimeMask;
		ReactionSimulation =
			MakeUnique<MatterFlux::Reaction::FMaskReaction>();
		if (!ReactionSimulation->Initialize(
			CurrentMask,
			*Rule,
			EventSeed))
		{
			ReactionSimulation.Reset();
			return false;
		}
	}

	const FIntPoint RequestedCell = WorldToMaskCell(WorldLocation);
	bool bActivated = ReactionSimulation->Activate(
		RequestedCell,
		StimulusMaterial);
	for (int32 Radius = 1;
		!bActivated
			&& Radius <= FMath::Max(GetMaskWidth(), GetMaskHeight());
		++Radius)
	{
		for (int32 Y = 0; Y < GetMaskHeight() && !bActivated; ++Y)
		{
			const int32 DeltaY = FMath::Abs(Y - RequestedCell.Y);
			if (DeltaY > Radius)
			{
				continue;
			}

			const int32 MinimumX = FMath::Max(
				RequestedCell.X - Radius,
				0);
			const int32 MaximumX = FMath::Min(
				RequestedCell.X + Radius,
				GetMaskWidth() - 1);
			if (MinimumX > MaximumX)
			{
				continue;
			}

			if (DeltaY == Radius)
			{
				for (int32 X = MinimumX; X <= MaximumX; ++X)
				{
					if (ReactionSimulation->Activate(
						FIntPoint(X, Y),
						StimulusMaterial))
					{
						bActivated = true;
						break;
					}
				}
				continue;
			}

			if (ReactionSimulation->Activate(
				FIntPoint(MinimumX, Y),
				StimulusMaterial))
			{
				bActivated = true;
				break;
			}
			if (MaximumX != MinimumX
				&& ReactionSimulation->Activate(
					FIntPoint(MaximumX, Y),
					StimulusMaterial))
			{
				bActivated = true;
				break;
			}
		}
	}
	if (!bActivated)
	{
		return false;
	}

	VisibleActiveMask =
		ReactionSimulation->GetActiveMask();
	bReactionVisualDirty = true;
	EnsureReactionVisualComponents();
	PublishReactionState();
	SetActorTickEnabled(true);
	MarkSharedSmokeVisualizationDirty();
	return true;
}

void AFragment2DSourceActor::SetSourceCollisionEnabled(const bool bEnabled)
{
	if (bEnableSourceCollision == bEnabled)
	{
		ApplySourceCollisionState();
		return;
	}

	bEnableSourceCollision = bEnabled;
	ApplySourceCollisionState();
	ForceNetUpdate();
}

void AFragment2DSourceActor::SetSourceMeshProjectionEnabled(
	const bool bEnabled)
{
	bSourceMeshProjectionEnabled = bEnabled;
	ApplyBrokenState();
}

void AFragment2DSourceActor::ConfigureAggregate(
	const FGuid& InAggregateId,
	const bool bInAggregateRoot)
{
	if (GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
	{
		return;
	}
	AggregateId = InAggregateId;
	bAggregateRoot = AggregateId.IsValid() && bInAggregateRoot;
	ForceNetUpdate();
}

bool AFragment2DSourceActor::BuildSynchronizedDamageEventFrom(
	const AFragment2DSourceActor& ReferenceSource,
	const FFragmentDamageEvent& ReferenceEvent,
	FFragmentDamageEvent& OutEvent) const
{
	const EFragmentSourceGeometryStyle GeometryStyle =
		ProceduralSource.HasValidLayout()
			? ProceduralSource.GeometryStyle
			: EFragmentSourceGeometryStyle::ExtrudedMask;
	const EFragmentSourceGeometryStyle ReferenceGeometryStyle =
		ReferenceSource.ProceduralSource.HasValidLayout()
			? ReferenceSource.ProceduralSource.GeometryStyle
			: EFragmentSourceGeometryStyle::ExtrudedMask;
	const bool bParallelSlice =
		AggregateId.IsValid()
		&& AggregateId == ReferenceSource.AggregateId
		&& SourceMaterialId == ReferenceSource.SourceMaterialId
		&& GetMaskWidth() == ReferenceSource.GetMaskWidth()
		&& GetMaskHeight() == ReferenceSource.GetMaskHeight()
		&& FMath::IsNearlyEqual(
			GetCellSize(),
			ReferenceSource.GetCellSize())
		&& GeometryStyle == ReferenceGeometryStyle
		&& GetActorQuat().AngularDistance(
			ReferenceSource.GetActorQuat()) <= KINDA_SMALL_NUMBER;
	if (!bParallelSlice)
	{
		return false;
	}

	OutEvent = ReferenceEvent;
	OutEvent.SourceId = SourceId;
	OutEvent.BaseRevision = Revision;
	OutEvent.EventSeed ^= static_cast<int32>(GetTypeHash(SourceId));
	const FTransform ShapeInReferenceSpace =
		ReferenceEvent.DamageShape.WorldTransform.GetRelativeTransform(
			ReferenceSource.GetActorTransform());
	OutEvent.DamageShape.WorldTransform =
		ShapeInReferenceSpace * GetActorTransform();
	return OutEvent.DamageShape.WorldTransform.IsValid();
}

void AFragment2DSourceActor::TransferAggregateMembersTo(
	AActor& CarrierActor,
	const FFragmentDamageEvent* SharedDamageEvent)
{
	if (!HasAuthority()
		|| !AggregateId.IsValid()
		|| !bAggregateRoot
		|| CarrierActor.GetWorld() != GetWorld())
	{
		return;
	}

	const float CarrierLifeSpan = CarrierActor.GetLifeSpan();
	AFragment2DActor* FragmentCarrier =
		Cast<AFragment2DActor>(&CarrierActor);
	TArray<AFragment2DSourceActor*> Members;
	for (TActorIterator<AFragment2DSourceActor> It(GetWorld()); It; ++It)
	{
		AFragment2DSourceActor* Member = *It;
		if (Member == this
			|| Member->AggregateId != AggregateId
			|| Member->bAggregateRoot
			|| Member->bBroken)
		{
			continue;
		}
		Members.Add(Member);
	}
	Members.Sort(
		[](const AFragment2DSourceActor& A,
			const AFragment2DSourceActor& B)
		{
			return A.SourceId.ToString(EGuidFormats::Digits)
				< B.SourceId.ToString(EGuidFormats::Digits);
		});
	for (AFragment2DSourceActor* Member : Members)
	{
		if (FragmentCarrier && SharedDamageEvent)
		{
			FFragmentDamageEvent MemberEvent;
			const bool bMappedSynchronizedDamage =
				Member->BuildSynchronizedDamageEventFrom(
				*this,
				*SharedDamageEvent,
				MemberEvent);
			if (!bMappedSynchronizedDamage)
			{
				MemberEvent = *SharedDamageEvent;
				MemberEvent.SourceId = Member->SourceId;
				MemberEvent.BaseRevision = Member->Revision;
				MemberEvent.EventSeed ^= static_cast<int32>(
					GetTypeHash(Member->SourceId));
			}
			FPreparedFragmentDamage MemberTransaction;
			if (Member->PrepareDamageEvent(
				MemberEvent,
				MemberTransaction))
			{
				const int32 AvailableLayers = 16
					- FragmentCarrier->GetAggregateMemberCount();
				if (UFragmentSimulationSubsystem::IsCutLoggingEnabled())
				{
					int32 FadingPayloads = 0;
					for (const FFragmentSpawnPayload& Payload
						: MemberTransaction.Payloads)
					{
						FadingPayloads += Payload.FadeOutDuration > 0.0f ? 1 : 0;
					}
					UE_LOG(
						LogMatterFlux,
						Display,
						TEXT("[FragmentCut] transfer member=%s source=%s material=%s mapped=%s revision=%d cellsBefore=%d cellsSupported=%d payloads=%d fading=%d availableLayers=%d"),
						*Member->GetName(),
						*Member->SourceId.ToString(EGuidFormats::Short),
						*Member->SourceMaterialId.ToString(),
						bMappedSynchronizedDamage ? TEXT("true") : TEXT("false"),
						Member->Revision,
						Algo::Count(Member->GetRuntimeMask(), static_cast<uint8>(1)),
						Algo::Count(MemberTransaction.SupportedMask, static_cast<uint8>(1)),
						MemberTransaction.Payloads.Num(),
						FadingPayloads,
						AvailableLayers);
				}
				if (MemberTransaction.Payloads.Num() > AvailableLayers)
				{
					if (UFragmentSimulationSubsystem::IsCutLoggingEnabled())
					{
						UE_LOG(
							LogMatterFlux,
							Display,
							TEXT("[FragmentCut] transfer rejected source=%s reason=layer-budget needed=%d available=%d"),
							*Member->SourceId.ToString(EGuidFormats::Short),
							MemberTransaction.Payloads.Num(),
							AvailableLayers);
					}
					UE_LOG(
						LogMatterFlux,
						Error,
						TEXT("Aggregate cut of source %s exceeds the carrier layer budget"),
						*Member->SourceId.ToString());
					continue;
				}
				if (!Member->CommitPreparedDamage(MemberTransaction))
				{
					continue;
				}
				bool bAllDetachedPartsTransferred = true;
				for (const FFragmentSpawnPayload& Payload
					: MemberTransaction.Payloads)
				{
					bAllDetachedPartsTransferred &=
						FragmentCarrier->AbsorbAggregateSourceFragment(
							*Member,
							Payload);
				}
				if (!bAllDetachedPartsTransferred)
				{
					UE_LOG(
						LogMatterFlux,
						Error,
						TEXT("Aggregate carrier rejected a cut portion of source %s"),
						*Member->SourceId.ToString());
					continue;
				}
				if (UFragmentSimulationSubsystem::IsCutLoggingEnabled())
				{
					UE_LOG(
						LogMatterFlux,
						Display,
						TEXT("[FragmentCut] transfer committed source=%s revision=%d residualCells=%d carrierLayers=%d"),
						*Member->SourceId.ToString(EGuidFormats::Short),
						Member->Revision,
						Algo::Count(Member->GetRuntimeMask(), static_cast<uint8>(1)),
						FragmentCarrier->GetAggregateMemberCount());
				}
				if (Member->GetRuntimeMask().Contains(1))
				{
					// Only layout-compatible parallel slices own a static stump
					// remainder. A differently shaped/materialed member (for
					// example the canopy) is supported by the aggregate root, not
					// by terrain. If its synchronized vertical cut leaves a
					// locally "supported" mask, that remainder still belongs to
					// the falling carrier and must not remain as a floating Source.
					if (!bMappedSynchronizedDamage
						&& FragmentCarrier->AbsorbAggregateSource(*Member))
					{
						continue;
					}
					// 方柱树干由多个深度切片组成。每个留下残余的切片都必须
					// 与动态树冠完成分离后再回到区块批次；只保护 aggregate
					// root 会让背面的树桩切片仍从树叶中露出来。
					Member->BeginAggregateSeparationGracePeriod(
						CarrierActor);
				}
				else if (AMatterFluxPlayableWorldActor* WorldOwner =
					Cast<AMatterFluxPlayableWorldActor>(Member->GetOwner()))
				{
					WorldOwner->DematerializeFragmentSource(
						Member->SourceId);
				}
				else
				{
					Member->MarkBroken();
				}
				continue;
			}
		}
		if (FragmentCarrier
			&& FragmentCarrier->AbsorbAggregateSource(*Member))
		{
			if (UFragmentSimulationSubsystem::IsCutLoggingEnabled())
			{
				UE_LOG(
					LogMatterFlux,
					Display,
					TEXT("[FragmentCut] transfer whole-source source=%s material=%s carrierLayers=%d"),
					*Member->SourceId.ToString(EGuidFormats::Short),
					*Member->SourceMaterialId.ToString(),
					FragmentCarrier->GetAggregateMemberCount());
			}
			continue;
		}
		Member->AttachToActor(
			&CarrierActor,
			FAttachmentTransformRules::KeepWorldTransform);
		Member->bDetachedFromTerrain = true;
		Member->Tags.AddUnique(TEXT("MatterFluxDetachedAggregate"));
		if (CarrierLifeSpan > 0.0f)
		{
			Member->SetLifeSpan(CarrierLifeSpan);
		}
		Member->ForceNetUpdate();
	}
}

void AFragment2DSourceActor::BeginAggregateSeparationGracePeriod(
	AActor& CarrierActor,
	const float MaxDurationSeconds)
{
	if (!HasAuthority()
		|| CarrierActor.GetWorld() != GetWorld()
		|| bBroken)
	{
		return;
	}

	AggregateSeparationCarrier = &CarrierActor;
	const double Now = FPlatformTime::Seconds();
	// 仅以初始包围盒判定会在树冠刚切开、尚未开始下落时过早结束。
	// 先给物理解算一个短暂窗口，再用 X/Z 平面的实际分离状态收尾。
	AggregateSeparationEarliestEndSeconds = Now + 0.75;
	AggregateSeparationDeadlineSeconds = Now
		+ FMath::Clamp(MaxDurationSeconds, 0.8f, 5.0f);
	bAggregateSeparationCollisionSuppressed = true;
	ApplyBrokenState();
	ForceNetUpdate();

	GetWorldTimerManager().SetTimer(
		AggregateSeparationTimerHandle,
		this,
		&AFragment2DSourceActor::UpdateAggregateSeparationGracePeriod,
		0.05f,
		true);
}

void AFragment2DSourceActor::UpdateAggregateSeparationGracePeriod()
{
	AActor* CarrierActor = AggregateSeparationCarrier.Get();
	if (!HasAuthority()
		|| !IsValid(CarrierActor)
		|| FPlatformTime::Seconds() >= AggregateSeparationDeadlineSeconds)
	{
		EndAggregateSeparationGracePeriod();
		return;
	}
	if (FPlatformTime::Seconds() < AggregateSeparationEarliestEndSeconds)
	{
		return;
	}

	const FBox RootBounds = MeshComponent
		? MeshComponent->Bounds.GetBox()
		: FBox(ForceInit);
	const FBox CarrierBounds = CarrierActor->GetComponentsBoundingBox(true);
	if (!RootBounds.IsValid || !CarrierBounds.IsValid)
	{
		EndAggregateSeparationGracePeriod();
		return;
	}

	// MatterFlux 的可破坏平面位于 X/Z；挤出深度 Y 必然重叠，不能参与
	// 分离判定。留出半个像素余量，接触切面不被误判为穿透。
	const float Margin = FMath::Max(1.0f, GetCellSize() * 0.5f);
	const bool bOverlapsInDestructiblePlane =
		CarrierBounds.Min.X < RootBounds.Max.X - Margin
		&& CarrierBounds.Max.X > RootBounds.Min.X + Margin
		&& CarrierBounds.Min.Z < RootBounds.Max.Z - Margin
		&& CarrierBounds.Max.Z > RootBounds.Min.Z + Margin;
	if (!bOverlapsInDestructiblePlane)
	{
		EndAggregateSeparationGracePeriod();
	}
}

void AFragment2DSourceActor::EndAggregateSeparationGracePeriod()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(
			AggregateSeparationTimerHandle);
	}
	AggregateSeparationCarrier.Reset();
	AggregateSeparationEarliestEndSeconds = 0.0;
	AggregateSeparationDeadlineSeconds = 0.0;
	if (!bAggregateSeparationCollisionSuppressed)
	{
		return;
	}
	bAggregateSeparationCollisionSuppressed = false;
	ApplyBrokenState();
	ForceNetUpdate();

	// 保护期内根部保持为独立 Actor，以便复制碰撞抑制状态；完成分离后
	// 立即回到区块合并渲染，避免长距离探索留下大量树桩 Actor。
	if (AMatterFluxPlayableWorldActor* WorldOwner =
		Cast<AMatterFluxPlayableWorldActor>(GetOwner()))
	{
		WorldOwner->DematerializeFragmentSource(SourceId);
	}
}

bool AFragment2DSourceActor::PrepareDamageEvent(
	const FFragmentDamageEvent& DamageEvent,
	FPreparedFragmentDamage& OutTransaction,
	const bool bForceDetachedPhysics) const
{
	OutTransaction = FPreparedFragmentDamage();
	if (GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Rejected non-authority damage transaction on %s."), *GetName());
		return false;
	}

	if (bBroken || !SourceId.IsValid() || !DamageEvent.SourceId.IsValid() || DamageEvent.SourceId != SourceId)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Rejected fragment damage on %s: source id mismatch or source already broken."), *GetName());
		return false;
	}

	if (DamageEvent.BaseRevision != Revision)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Rejected fragment damage on %s: base revision %d != current revision %d"), *GetName(), DamageEvent.BaseRevision, Revision);
		return false;
	}
	if (Revision < 0 || Revision == MAX_int32)
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Rejected fragment damage on %s: revision %d cannot be incremented safely."), *GetName(), Revision);
		return false;
	}
	if (static_cast<int64>(RuntimeMask.Num()) != static_cast<int64>(GetMaskWidth()) * static_cast<int64>(GetMaskHeight()))
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Rejected fragment damage on %s: runtime mask dimensions are invalid."), *GetName());
		return false;
	}
	const FFragmentDamageShape& Shape = DamageEvent.DamageShape;
	const bool bValidShape = Shape.WorldTransform.IsValid()
		&& FMath::IsFinite(DamageEvent.DamagePower) && DamageEvent.DamagePower >= 0.0f
		&& ((Shape.Type == EFragmentDamageShapeType::Circle && FMath::IsFinite(Shape.Radius) && Shape.Radius > 0.0f)
			|| (Shape.Type == EFragmentDamageShapeType::Box && FMath::IsFinite(Shape.Extents.X) && FMath::IsFinite(Shape.Extents.Y)
				&& Shape.Extents.X > 0.0 && Shape.Extents.Y > 0.0)
			|| (Shape.Type == EFragmentDamageShapeType::Line && FMath::IsFinite(Shape.Extents.X) && Shape.Extents.X > 0.0
				&& FMath::IsFinite(Shape.Thickness) && Shape.Thickness > 0.0f));
	if (!bValidShape)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("Rejected fragment damage on %s: invalid damage shape."), *GetName());
		return false;
	}

	FFragmentDamageShape LocalDamageShape = DamageEvent.DamageShape;
	LocalDamageShape.WorldTransform = DamageEvent.DamageShape.WorldTransform.GetRelativeTransform(GetActorTransform());

	TArray<uint8> CandidateMask = RuntimeMask;
	const bool bChanged = MatterFlux::FragmentGeometry::ApplyDamageShape(CandidateMask, GetMaskWidth(), GetMaskHeight(), GetCellSize(), LocalDamageShape);
	if (!bChanged)
	{
		return false;
	}

	MatterFlux::FragmentGeometry::FFragmentSupportResult SupportResult;
	const EFragmentSupportMode SupportMode =
		ProceduralSource.HasValidLayout()
			? ProceduralSource.SupportMode
			: (FragmentAsset
				? FragmentAsset->SupportMode
				: DefaultSupportMode);
	if (!MatterFlux::FragmentGeometry::ClassifyMaskBySupport(
		CandidateMask,
		SupportAnchorMask,
		GetMaskWidth(),
		GetMaskHeight(),
		SupportMode,
		SupportResult))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Support classification failed on %s; damage transaction was rolled back."),
			*GetName());
		return false;
	}

	const int32 NewRevision = Revision + 1;
	if (!MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
		SupportResult.DetachedComponents,
		SourceId,
		GetActorTransform(),
		GetMaskWidth(),
		GetMaskHeight(),
		NewRevision,
		GetCellSize(),
		GetMinFragmentAreaPixels(),
		GetMaxFragmentsPerBreak(),
		DamageEvent.DamageShape.WorldTransform.GetLocation(),
		DamageEvent.DamagePower,
		DamageEvent.EventSeed,
		OutTransaction.Payloads,
		ProceduralSource.GeometryStyle))
	{
		OutTransaction = FPreparedFragmentDamage();
		UE_LOG(LogMatterFlux, Error, TEXT("Fragment geometry failed on %s; damage transaction was rolled back."), *GetName());
		return false;
	}
	if (DamageEvent.bDissolveDetachedFragments)
	{
		// 腐蚀已经通过 mask 缺口和反应产物表现。若再把溶解部分生成
		// 成木块，持续接触会不断把新 Actor 叠到正在倾倒的树冠上。
		OutTransaction.Payloads.Reset();
	}
	for (FFragmentSpawnPayload& Payload : OutTransaction.Payloads)
	{
		Payload.MaterialId = SourceMaterialId;
		// An aggregate member has lost the structural root that kept it aloft.
		// Even a sub-threshold remainder is now persistent physical material;
		// the generic 0.45 s decoration-debris fade is not the repeated-cut
		// exhaustion rule and must never retire tree canopy on its first cut.
		if (bForceDetachedPhysics && Payload.FadeOutDuration > 0.0f)
		{
			Payload.FadeOutDuration = 0.0f;
		}
		Payload.bEnableCollision = (bEnableSourceCollision
			|| bForceDetachedPhysics)
			&& Payload.FadeOutDuration <= 0.0f;
		if (!Payload.bEnableCollision)
		{
			// Collision hulls can be regenerated from the visual geometry if a
			// future gameplay rule promotes this debris. They are unnecessary in
			// the replicated initial payload for render-only decoration fragments.
			Payload.CollisionContours.Reset();
		}

		// 根部被水平截断的高树不能只获得向上的径向速度，否则上半棵树
		// 会原样落回木桩顶面，视觉上就像两根木桩和悬空树冠。这里仅对
		// aggregate root 的细长木质体施加确定性的侧向倾倒速度；普通碎片、
		// 房屋和非木质装饰仍沿用通用的径向碎片速度。
		const bool bTallFelledWood = bAggregateRoot
			&& AggregateId.IsValid()
			&& SourceMaterialId == TEXT("wood")
			&& Payload.FadeOutDuration <= 0.0f
			&& Payload.DetachedVoxelMask.IsValid()
			&& Payload.DetachedVoxelMask.Height
				>= Payload.DetachedVoxelMask.Width * 2
			&& Payload.InitialTransform.GetLocation().Z
				> DamageEvent.DamageShape.WorldTransform.GetLocation().Z
					+ GetCellSize();
		if (bTallFelledWood)
		{
			// Dynamic material is a full-3D projection of the same source state.
			// Preserve the tree's authored horizontal orientation instead of
			// collapsing every 45-degree tree onto an obsolete world-X lane.
			const FVector FallAxis = FVector(
				GetActorTransform().GetUnitAxis(EAxis::X).X,
				GetActorTransform().GetUnitAxis(EAxis::X).Y,
				0.0f).GetSafeNormal();
			const FVector RotationAxis = FVector::CrossProduct(
				FVector::UpVector,
				FallAxis).GetSafeNormal();
			const uint32 FallHash = HashCombineFast(
				GetTypeHash(SourceId),
				static_cast<uint32>(DamageEvent.EventSeed));
			const float FallSign = (FallHash & 1u) != 0u
				? 1.0f
				: -1.0f;
			const float SideSpeed = FMath::Clamp(
				DamageEvent.DamagePower * 0.80f,
				340.0f,
				420.0f);
			Payload.InitialLinearVelocity +=
				FallAxis * FallSign * SideSpeed;

			Payload.InitialAngularVelocity = RotationAxis
				* (-FallSign * 360.0f);
		}
	}

	OutTransaction.SourceId = SourceId;
	OutTransaction.BaseRevision = Revision;
	OutTransaction.NewRevision = NewRevision;
	OutTransaction.SupportedMask = MoveTemp(SupportResult.SupportedMask);
	return true;
}

bool AFragment2DSourceActor::CommitPreparedDamage(FPreparedFragmentDamage& Transaction)
{
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| bBroken
		|| Transaction.SourceId != SourceId
		|| Transaction.BaseRevision != Revision
		|| Revision < 0
		|| Revision == MAX_int32
		|| Transaction.NewRevision != Revision + 1
		|| Transaction.SupportedMask.Num() != RuntimeMask.Num())
	{
		UE_LOG(LogMatterFlux, Error, TEXT("Prepared fragment damage became stale before commit on %s."), *GetName());
		return false;
	}

	if (ReactionSimulation
		&& !ReactionSimulation->ConstrainInputMask(
			Transaction.SupportedMask))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Reaction mask could not follow committed damage on %s."),
			*GetName());
		return false;
	}
	if (ReactionSimulation)
	{
		OutputMask =
			ReactionSimulation->GetOutputMask();
		VisibleActiveMask =
			ReactionSimulation->GetActiveMask();
	}
	const int32 MaskWidth = GetMaskWidth();
	const int32 MaskHeight = GetMaskHeight();
	const float CellSize = GetCellSize();
	const int32 SourceMinimumArea = ProceduralSource.HasValidLayout()
		? ProceduralSource.MinFragmentAreaPixels
		: (FragmentAsset
			? FMath::Max(
				FragmentAsset->MinFragmentAreaPixels,
				1)
			: 16);
	const int32 MaxFragments = GetMaxFragmentsPerBreak();
	const EFragmentSupportMode SupportMode =
		ProceduralSource.HasValidLayout()
			? ProceduralSource.SupportMode
			: (FragmentAsset
				? FragmentAsset->SupportMode
				: DefaultSupportMode);
	RuntimeMask = MoveTemp(Transaction.SupportedMask);
	if (!ProceduralSource.HasValidLayout())
	{
		ProceduralSource.Width = MaskWidth;
		ProceduralSource.Height = MaskHeight;
		ProceduralSource.CellSize = CellSize;
		ProceduralSource.MinFragmentAreaPixels =
			SourceMinimumArea;
		ProceduralSource.MaxFragmentsPerBreak =
			MaxFragments;
		ProceduralSource.SupportMode = SupportMode;
	}
	ProceduralSource.SolidMask = RuntimeMask;
	Revision = Transaction.NewRevision;
	bReactionGeometryDirty = true;
	bReactionVisualDirty = true;
	RebuildSourceMesh();
	if (ReactionSimulation)
	{
		RebuildReactionVisualization();
		PublishReactionState();
	}
	ForceNetUpdate();
	return true;
}

void AFragment2DSourceActor::MarkBroken()
{
	if (!HasAuthority() && GetWorld() && GetWorld()->IsGameWorld())
	{
		return;
	}
	bBroken = true;
	ReactionSimulation.Reset();
	VisibleActiveMask.Init(0, RuntimeMask.Num());
	if (FlameInstances)
	{
		FlameInstances->ClearInstances();
	}
	if (FireLight)
	{
		FireLight->SetVisibility(false);
	}
	bReactionVisualDirty = false;
	SetActorTickEnabled(false);
	MarkSharedSmokeVisualizationDirty();
	ForceNetUpdate();
	if (bDestroySourceOnFirstBreak && GetWorld() && GetWorld()->IsGameWorld())
	{
		Destroy();
		return;
	}

	ApplyBrokenState();
}

void AFragment2DSourceActor::OnRep_Broken()
{
	ApplyBrokenState();
}

void AFragment2DSourceActor::OnRep_AggregateSeparationCollisionSuppressed()
{
	ApplyBrokenState();
}

void AFragment2DSourceActor::OnRep_SourceId()
{
	RefreshPresenceRegistration();
}

void AFragment2DSourceActor::RefreshPresenceRegistration()
{
	UFragmentSimulationSubsystem* Subsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr;
	if (!Subsystem)
	{
		return;
	}
	if (RegisteredPresenceSourceId.IsValid()
		&& RegisteredPresenceSourceId != SourceId)
	{
		Subsystem->UnregisterSourceActor(*this);
		RegisteredPresenceSourceId.Invalidate();
	}
	if (SourceId.IsValid())
	{
		// InitializeFromProceduralMask can run before deferred spawning has
		// finished. Re-register the same ID at BeginPlay so its spatial entry
		// reflects the final world transform instead of the pre-spawn bounds.
		if (Subsystem->RegisterSourceActor(*this))
		{
			RegisteredPresenceSourceId = SourceId;
		}
	}
}

void AFragment2DSourceActor::OnRep_ProceduralSource()
{
	if (!ProceduralSource.HasValidLayout())
	{
		return;
	}
	RuntimeMask = ProceduralSource.SolidMask;
	RebuildSourceMesh();
}

void AFragment2DSourceActor::OnRep_SourceAppearance()
{
	ApplySourceMaterial();
}

void AFragment2DSourceActor::OnRep_SourceCollision()
{
	ApplySourceCollisionState();
}

void AFragment2DSourceActor::OnRep_ReactionState()
{
	const int32 ExpectedNum = GetMaskWidth() * GetMaskHeight();
	TArray<uint8> InputMask;
	TArray<uint8> NewOutputMask;
	TArray<uint8> ActiveMask;
	if (!UnpackBinaryMask(
			ReplicatedReactionInputMask,
			ExpectedNum,
			InputMask)
		|| !UnpackBinaryMask(
			ReplicatedReactionOutputMask,
			ExpectedNum,
			NewOutputMask)
		|| !UnpackBinaryMask(
			ReplicatedReactionActiveMask,
			ExpectedNum,
			ActiveMask))
	{
		return;
	}
	RuntimeMask = MoveTemp(InputMask);
	OutputMask = MoveTemp(NewOutputMask);
	VisibleActiveMask = MoveTemp(ActiveMask);
	bReactionGeometryDirty = true;
	bReactionVisualDirty = true;
	EnsureReactionVisualComponents();
	RebuildReactionVisualization();
	SetActorTickEnabled(VisibleActiveMask.Contains(1));
	MarkSharedSmokeVisualizationDirty();
}

void AFragment2DSourceActor::AdvanceReaction(
	const float DeltaSeconds)
{
	const float ClampedDelta =
		FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	bool bStateChanged = false;
	if (HasAuthority()
		&& ReactionSimulation
		&& ReactionSimulation->IsActive())
	{
		ReactionAccumulator += ClampedDelta;
		int32 StepsThisFrame = 0;
		while (ReactionAccumulator >= 0.1f
			&& StepsThisFrame < 3)
		{
			ReactionAccumulator -= 0.1f;
			const MatterFlux::Reaction::FStepStats Stats =
				ReactionSimulation->Step();
			AddMaterialEmissions(Stats.MaterialEmissionCells);
			RuntimeMask = ReactionSimulation->GetInputMask();
			OutputMask = ReactionSimulation->GetOutputMask();
			VisibleActiveMask =
				ReactionSimulation->GetActiveMask();
			bReactionGeometryDirty |=
				Stats.ConsumedInputCells > 0;
			bReactionVisualDirty = true;
			bStateChanged = true;
			++StepsThisFrame;
		}
	}

	ReactionVisualAccumulator += ClampedDelta;
	const bool bReactionFinishedThisFrame =
		bStateChanged
		&& HasAuthority()
		&& ReactionSimulation
		&& !ReactionSimulation->IsActive();
	if (bReactionVisualDirty
		&& (ReactionVisualAccumulator >= 0.1f
			|| bReactionFinishedThisFrame))
	{
		ReactionVisualAccumulator = 0.0f;
		RebuildReactionVisualization();
	}
	if (bStateChanged)
	{
		PublishReactionState();
		MarkSharedSmokeVisualizationDirty();
	}

	const bool bStillActive =
		HasAuthority()
			? ReactionSimulation
				&& ReactionSimulation->IsActive()
			: VisibleActiveMask.ContainsByPredicate(
				[](const uint8 Value)
				{
					return Value != 0;
				});
	if (!bStillActive)
	{
		if (FireLight)
		{
			FireLight->SetVisibility(false);
		}
		SetActorTickEnabled(false);
	}
}

void AFragment2DSourceActor::EnsureReactionVisualComponents()
{
	if (!OutputMeshComponent)
	{
		OutputMeshComponent =
			NewObject<UProceduralMeshComponent>(
				this,
				TEXT("ReactionOutputMesh"));
		OutputMeshComponent->SetupAttachment(MeshComponent);
		OutputMeshComponent->SetCollisionEnabled(
			ECollisionEnabled::NoCollision);
		OutputMeshComponent->SetCanEverAffectNavigation(false);
		OutputMeshComponent->SetCastShadow(false);
		AddInstanceComponent(OutputMeshComponent);
		OutputMeshComponent->RegisterComponent();
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	const auto CreateParticles =
		[this, CubeMesh](
			TObjectPtr<UInstancedStaticMeshComponent>& Component,
			const FName Name)
		{
			if (Component)
			{
				return;
			}
			Component = NewObject<UInstancedStaticMeshComponent>(
				this,
				Name);
			Component->SetupAttachment(MeshComponent);
			Component->SetStaticMesh(CubeMesh);
			Component->SetCollisionEnabled(
				ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
			AddInstanceComponent(Component);
			Component->RegisterComponent();
	};
	CreateParticles(FlameInstances, TEXT("ReactionFlames"));

	const FMatterFluxReactionDefinition* Rule =
		FindReactionRule();
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	const auto ResolveColor =
		[&Registry](const FName MaterialId, const FLinearColor Fallback)
		{
			if (Registry.IsValid())
			{
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(MaterialId))
				{
					return Material->Color;
				}
			}
			return Fallback;
		};
	if (FragmentMaterial && Rule)
	{
		if (!OutputMaterialInstance)
		{
			OutputMaterialInstance =
				UMaterialInstanceDynamic::Create(
					FragmentMaterial,
					this);
			OutputMaterialInstance->SetVectorParameterValue(
				TEXT("Color"),
				ResolveColor(
					Rule->OutputA,
					FLinearColor(0.08f, 0.07f, 0.06f)));
			OutputMeshComponent->SetMaterial(
				0,
				OutputMaterialInstance);
			OutputMeshComponent->SetMaterial(
				1,
				OutputMaterialInstance);
		}
		if (!StimulusMaterialInstance)
		{
			StimulusMaterialInstance =
				UMaterialInstanceDynamic::Create(
					FragmentMaterial,
					this);
			StimulusMaterialInstance->SetVectorParameterValue(
				TEXT("Color"),
				ResolveColor(
					Rule->InputB,
					FLinearColor(1.0f, 0.22f, 0.01f)));
			FlameInstances->SetMaterial(
				0,
				StimulusMaterialInstance);
		}
	}

	if (!FireLight
		&& SourceMaterialId == TEXT("wood")
		&& Rule
		&& MatterFlux::Reaction::UsesFlamePresentation(*Rule))
	{
		FireLight = NewObject<UPointLightComponent>(
			this,
			TEXT("ReactionFireLight"));
		FireLight->SetupAttachment(MeshComponent);
		FireLight->SetLightColor(FLinearColor(1.0f, 0.20f, 0.01f));
		FireLight->SetIntensity(1800.0f);
		FireLight->SetAttenuationRadius(420.0f);
		FireLight->SetCastShadows(false);
		AddInstanceComponent(FireLight);
		FireLight->RegisterComponent();
	}
}

void AFragment2DSourceActor::RebuildReactionVisualization()
{
	EnsureReactionVisualComponents();
	if (bReactionGeometryDirty)
	{
		RebuildSourceMesh();
		RebuildOutputMesh();
		bReactionGeometryDirty = false;
	}

	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const float CellSize = GetCellSize();
	TArray<FTransform> FlameTransforms;
	FVector FlameCenter = FVector::ZeroVector;
	TArray<int32> VisibleActiveCells;
	const FMatterFluxReactionDefinition* Rule = FindReactionRule();
	const bool bRenderFlames = Rule
		&& MatterFlux::Reaction::UsesFlamePresentation(*Rule);
	for (int32 Index = 0;
		bRenderFlames && Index < VisibleActiveMask.Num();
		++Index)
	{
		if (VisibleActiveMask[Index] != 0)
		{
			VisibleActiveCells.Add(Index);
		}
	}
	for (const int32 Index : VisibleActiveCells)
	{
		const int32 X = Index % Width;
		const int32 Y = Index / Width;
		const FVector Position(
			(static_cast<float>(X) + 0.5f
				- static_cast<float>(Width) * 0.5f)
				* CellSize,
			0.0f,
			(static_cast<float>(Y) + 0.62f
				- static_cast<float>(Height) * 0.5f)
				* CellSize);
		FlameCenter += Position;
		FlameTransforms.Emplace(
			FRotator::ZeroRotator,
			Position,
			FVector(
				CellSize * 1.06f / 100.0f,
				CellSize * 1.06f / 100.0f,
				CellSize * 1.18f / 100.0f));
	}
	MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
		*FlameInstances,
		FlameTransforms);

	const bool bHasFlames = !FlameTransforms.IsEmpty();
	if (FireLight)
	{
		FireLight->SetVisibility(bHasFlames);
		if (bHasFlames)
		{
			FireLight->SetRelativeLocation(
				FlameCenter
					/ static_cast<float>(FlameTransforms.Num()));
		}
	}
	bReactionVisualDirty = false;
}

void AFragment2DSourceActor::RebuildOutputMesh()
{
	if (!OutputMeshComponent)
	{
		return;
	}
	OutputMeshComponent->ClearAllMeshSections();
	const FMatterFluxReactionDefinition* Rule = FindReactionRule();
	if (!Rule
		|| Rule->OutputA == TEXT("empty")
		|| OutputMask.IsEmpty()
		|| !OutputMask.ContainsByPredicate(
			[](const uint8 Value)
			{
				return Value != 0;
			}))
	{
		return;
	}

	MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
	if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		OutputMask,
		GetMaskWidth(),
		GetMaskHeight(),
		GetCellSize(),
		Geometry))
	{
		return;
	}
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	int32 FaceIndexCount = 0;
	const bool bBuilt = ProceduralSource.GeometryStyle
		== EFragmentSourceGeometryStyle::RadialColumn
		? MatterFlux::FragmentGeometry::BuildRadialColumnMeshFromMask(
			OutputMask,
			GetMaskWidth(),
			GetMaskHeight(),
			GetCellSize(),
			Vertices,
			Triangles,
			Normals,
			UVs,
			FaceIndexCount)
		: ProceduralSource.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks
		? MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromMask(
			OutputMask,
			GetMaskWidth(),
			GetMaskHeight(),
			GetCellSize(),
			Vertices,
			Triangles,
			Normals,
			UVs,
			FaceIndexCount)
		: MatterFlux::FragmentGeometry::BuildExtrudedMesh(
			Geometry.Vertices2D,
			Geometry.TriangleIndices,
			Geometry.OuterContours,
			Geometry.HoleContours,
			GetCellSize(),
			Vertices,
			Triangles,
			Normals,
			UVs);
	if (!bBuilt)
	{
		return;
	}
	if (ProceduralSource.GeometryStyle
		== EFragmentSourceGeometryStyle::ExtrudedMask)
	{
		FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
	}
	if (FaceIndexCount <= 0
		|| FaceIndexCount >= Triangles.Num())
	{
		return;
	}
	TArray<int32> FaceTriangles;
	FaceTriangles.Append(
		Triangles.GetData(),
		FaceIndexCount);
	TArray<int32> SideTriangles;
	SideTriangles.Append(
		Triangles.GetData() + FaceIndexCount,
		Triangles.Num() - FaceIndexCount);
	OutputMeshComponent->CreateMeshSection(
		0,
		Vertices,
		FaceTriangles,
		Normals,
		UVs,
		TArray<FColor>(),
		TArray<FProcMeshTangent>(),
		false);
	OutputMeshComponent->CreateMeshSection(
		1,
		Vertices,
		SideTriangles,
		Normals,
		UVs,
		TArray<FColor>(),
		TArray<FProcMeshTangent>(),
		false);
}

void AFragment2DSourceActor::AddMaterialEmissions(
	const TArray<FIntPoint>& Cells)
{
	TotalMaterialEmissionCount += Cells.Num();
	bReactionVisualDirty |= !Cells.IsEmpty();
	if (!Cells.IsEmpty())
	{
		MarkSharedSmokeVisualizationDirty();
	}
}

void AFragment2DSourceActor::MarkSharedSmokeVisualizationDirty() const
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			It->MarkSourceReactionVisualizationDirty();
		}
	}
}

FIntPoint AFragment2DSourceActor::WorldToMaskCell(
	const FVector& WorldLocation) const
{
	const FVector Local =
		GetActorTransform().InverseTransformPosition(
			WorldLocation);
	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const int32 MaximumSearchRadius = FMath::Max(Width, Height);
	const auto ToBoundedCellCoordinate =
		[MaximumSearchRadius](
			const double Coordinate,
			const int32 Dimension)
		{
			const double MinimumSearchCoordinate =
				-static_cast<double>(MaximumSearchRadius);
			const double MaximumSearchCoordinate =
				static_cast<double>(
					Dimension - 1 + MaximumSearchRadius);
			if (!FMath::IsFinite(Coordinate)
				|| Coordinate < MinimumSearchCoordinate)
			{
				return -MaximumSearchRadius - 1;
			}
			if (Coordinate > MaximumSearchCoordinate)
			{
				return Dimension + MaximumSearchRadius;
			}
			return static_cast<int32>(
				FMath::FloorToInt(Coordinate));
		};
	return FIntPoint(
		ToBoundedCellCoordinate(
			Local.X / GetCellSize()
				+ static_cast<double>(Width) * 0.5,
			Width),
		ToBoundedCellCoordinate(
			Local.Z / GetCellSize()
				+ static_cast<double>(Height) * 0.5,
			Height));
}

void AFragment2DSourceActor::PublishReactionState()
{
	if (!HasAuthority() || !ReactionSimulation)
	{
		return;
	}
	TArray<uint8> PackedInput;
	TArray<uint8> PackedOutput;
	TArray<uint8> PackedActive;
	if (!PackPresenceMask(
			ReactionSimulation->GetInputMask(),
			PackedInput)
		|| !PackPresenceMask(
			ReactionSimulation->GetOutputMask(),
			PackedOutput)
		|| !PackPresenceMask(
			ReactionSimulation->GetActiveMask(),
			PackedActive))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Reaction state on %s contains an invalid binary mask."),
			*GetName());
		return;
	}
	ReplicatedReactionInputMask = MoveTemp(PackedInput);
	ReplicatedReactionOutputMask = MoveTemp(PackedOutput);
	ReplicatedReactionActiveMask = MoveTemp(PackedActive);
	ReactionRevision =
		ReactionRevision == MAX_int32
			? 0
			: ReactionRevision + 1;
	ForceNetUpdate();
}

const FMatterFluxReactionDefinition*
AFragment2DSourceActor::FindReactionRule() const
{
	return FindReactionRule(NAME_None);
}

const FMatterFluxReactionDefinition*
AFragment2DSourceActor::FindReactionRule(
	const FName StimulusMaterial) const
{
	if (StimulusMaterial.IsNone()
		&& ReactionSimulation
		&& ReactionSimulation->IsInitialized())
	{
		return &ReactionSimulation->GetRule();
	}
	if (!IMatterFluxScriptRuntime::IsAvailable()
		|| SourceMaterialId.IsNone())
	{
		return nullptr;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		return nullptr;
	}
	return MatterFlux::Reaction::FMaterialReactionEngine::
		FindPropagatingRule(
			*Registry,
			SourceMaterialId,
			StimulusMaterial);
}

int32 AFragment2DSourceActor::GetRemainingInputCellCount() const
{
	int32 Count = 0;
	for (const uint8 Value : RuntimeMask)
	{
		Count += Value != 0 ? 1 : 0;
	}
	return Count;
}

int32 AFragment2DSourceActor::GetOutputCellCount() const
{
	int32 Count = 0;
	for (const uint8 Value : OutputMask)
	{
		Count += Value != 0 ? 1 : 0;
	}
	return Count;
}

int32 AFragment2DSourceActor::GetActiveCellCount() const
{
	int32 Count = 0;
	for (const uint8 Value : VisibleActiveMask)
	{
		Count += Value != 0 ? 1 : 0;
	}
	return Count;
}

bool AFragment2DSourceActor::IsReacting() const
{
	return ReactionSimulation
		? ReactionSimulation->IsActive()
		: GetActiveCellCount() > 0;
}

FBox AFragment2DSourceActor::GetCanonicalWorldBounds() const
{
	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const float CellSize = GetCellSize();
	const FTransform& WorldTransform = GetActorTransform();
	if (Width <= 0
		|| Height <= 0
		|| !FMath::IsFinite(CellSize)
		|| CellSize <= 0.0f
		|| !WorldTransform.IsValid())
	{
		return FBox(ForceInit);
	}

	// Fragment masks live in local XZ and are extruded by one cell on local Y.
	// Actor scale carries the authored wall thickness, so transforming this
	// logical box produces stable bounds even when the projection is hidden.
	const FVector HalfExtent(
		static_cast<double>(Width) * CellSize * 0.5,
		CellSize * 0.5,
		static_cast<double>(Height) * CellSize * 0.5);
	return FBox(-HalfExtent, HalfExtent).TransformBy(
		WorldTransform.ToMatrixWithScale());
}

FBox AFragment2DSourceActor::GetActiveWorldBounds() const
{
	FBox Bounds(ForceInit);
	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const float CellSize = GetCellSize();
	for (int32 Index = 0;
		Index < VisibleActiveMask.Num();
		++Index)
	{
		if (VisibleActiveMask[Index] == 0)
		{
			continue;
		}
		const int32 X = Index % Width;
		const int32 Y = Index / Width;
		const FVector LocalCenter(
			(static_cast<float>(X) + 0.5f
				- static_cast<float>(Width) * 0.5f)
				* CellSize,
			0.0f,
			(static_cast<float>(Y) + 0.5f
				- static_cast<float>(Height) * 0.5f)
				* CellSize);
		const FVector WorldCenter =
			GetActorTransform().TransformPosition(
				LocalCenter);
		Bounds += FBox::BuildAABB(
			WorldCenter,
			FVector(CellSize));
	}
	return Bounds;
}

bool AFragment2DSourceActor::SweepRuntimeMask(
	const FVector& Start,
	const FVector& End,
	const float Radius,
	FVector& OutImpactLocation,
	FVector& OutImpactNormal) const
{
	OutImpactLocation = End;
	OutImpactNormal = FVector::ZeroVector;
	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const float CellSize = GetCellSize();
	const FTransform WorldTransform = GetActorTransform();
	if (bBroken
		|| RuntimeMask.Num() != Width * Height
		|| !RuntimeMask.Contains(1)
		|| Width <= 0
		|| Height <= 0
		|| !FMath::IsFinite(CellSize)
		|| CellSize <= UE_SMALL_NUMBER
		|| !FMath::IsFinite(Radius)
		|| Radius < 0.0f
		|| Start.ContainsNaN()
		|| End.ContainsNaN()
		|| Start.Equals(End, UE_SMALL_NUMBER)
		|| !WorldTransform.IsValid())
	{
		return false;
	}

	const FVector Scale = WorldTransform.GetScale3D().GetAbs();
	if (Scale.GetMin() <= UE_SMALL_NUMBER)
	{
		return false;
	}
	const FVector LocalStart =
		WorldTransform.InverseTransformPosition(Start);
	const FVector LocalEnd =
		WorldTransform.InverseTransformPosition(End);
	const FVector LocalDirection = LocalEnd - LocalStart;
	const FVector LocalRadius(
		Radius / Scale.X,
		Radius / Scale.Y,
		Radius / Scale.Z);
	const FVector QueryMinimum = LocalStart.ComponentMin(LocalEnd)
		- LocalRadius;
	const FVector QueryMaximum = LocalStart.ComponentMax(LocalEnd)
		+ LocalRadius;
	const int32 MinimumX = FMath::Clamp(
		FMath::FloorToInt(
			QueryMinimum.X / CellSize
				+ static_cast<double>(Width) * 0.5),
		0,
		Width - 1);
	const int32 MaximumX = FMath::Clamp(
		FMath::FloorToInt(
			QueryMaximum.X / CellSize
				+ static_cast<double>(Width) * 0.5),
		0,
		Width - 1);
	const int32 MinimumY = FMath::Clamp(
		FMath::FloorToInt(
			QueryMinimum.Z / CellSize
				+ static_cast<double>(Height) * 0.5),
		0,
		Height - 1);
	const int32 MaximumY = FMath::Clamp(
		FMath::FloorToInt(
			QueryMaximum.Z / CellSize
				+ static_cast<double>(Height) * 0.5),
		0,
		Height - 1);
	if (MinimumX > MaximumX || MinimumY > MaximumY)
	{
		return false;
	}

	const auto FindEntry = [
		&LocalStart,
		&LocalDirection](
			const FBox& Box,
			double& OutEntry,
			FVector& OutNormal)
	{
		double Entry = 0.0;
		double Exit = 1.0;
		FVector EntryNormal = FVector::ZeroVector;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Origin = LocalStart[Axis];
			const double Direction = LocalDirection[Axis];
			if (FMath::Abs(Direction) <= UE_DOUBLE_SMALL_NUMBER)
			{
				if (Origin < Box.Min[Axis] || Origin > Box.Max[Axis])
				{
					return false;
				}
				continue;
			}
			double First = (Box.Min[Axis] - Origin) / Direction;
			double Last = (Box.Max[Axis] - Origin) / Direction;
			FVector FirstNormal = FVector::ZeroVector;
			FirstNormal[Axis] = Direction > 0.0 ? -1.0 : 1.0;
			if (First > Last)
			{
				Swap(First, Last);
			}
			if (First > Entry)
			{
				Entry = First;
				EntryNormal = FirstNormal;
			}
			Exit = FMath::Min(Exit, Last);
			if (Entry > Exit)
			{
				return false;
			}
		}
		if (Exit < 0.0 || Entry > 1.0)
		{
			return false;
		}
		OutEntry = FMath::Clamp(Entry, 0.0, 1.0);
		OutNormal = EntryNormal;
		return true;
	};

	double BestEntry = TNumericLimits<double>::Max();
	FVector BestLocalNormal = FVector::ZeroVector;
	int32 BestIndex = MAX_int32;
	for (int32 MaskY = MinimumY; MaskY <= MaximumY; ++MaskY)
	{
		for (int32 MaskX = MinimumX; MaskX <= MaximumX; ++MaskX)
		{
			const int32 Index = MaskY * Width + MaskX;
			if (!RuntimeMask.IsValidIndex(Index) || RuntimeMask[Index] == 0)
			{
				continue;
			}
			const double CellMinimumX =
				(static_cast<double>(MaskX)
					- static_cast<double>(Width) * 0.5) * CellSize;
			const double CellMinimumZ =
				(static_cast<double>(MaskY)
					- static_cast<double>(Height) * 0.5) * CellSize;
			const FBox ExpandedCell(
				FVector(
					CellMinimumX,
					-CellSize * 0.5,
					CellMinimumZ) - LocalRadius,
				FVector(
					CellMinimumX + CellSize,
					CellSize * 0.5,
					CellMinimumZ + CellSize) + LocalRadius);
			double Entry = 0.0;
			FVector LocalNormal;
			if (!FindEntry(ExpandedCell, Entry, LocalNormal)
				|| Entry > BestEntry
				|| (FMath::IsNearlyEqual(Entry, BestEntry)
					&& Index >= BestIndex))
			{
				continue;
			}
			BestEntry = Entry;
			BestLocalNormal = LocalNormal;
			BestIndex = Index;
		}
	}
	if (BestIndex == MAX_int32)
	{
		return false;
	}
	OutImpactLocation = WorldTransform.TransformPosition(
		LocalStart + LocalDirection * BestEntry);
	OutImpactNormal = WorldTransform.TransformVectorNoScale(
		BestLocalNormal).GetSafeNormal();
	return true;
}

FName AFragment2DSourceActor::GetReactionStimulusMaterial() const
{
	const FMatterFluxReactionDefinition* Rule =
		FindReactionRule();
	return Rule ? Rule->InputB : NAME_None;
}

void AFragment2DSourceActor::GatherReactionSmokeAnchors(
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>& OutAnchors,
	const int32 MaxAnchors) const
{
	const FMatterFluxReactionDefinition* Rule = FindReactionRule();
	if (MaxAnchors <= 0
		|| !Rule
		|| !IsReacting()
		|| RuntimeMask.Num() != VisibleActiveMask.Num())
	{
		return;
	}
	TArray<uint8> Occupied = RuntimeMask;
	if (OutputMask.Num() == Occupied.Num())
	{
		for (int32 Index = 0; Index < Occupied.Num(); ++Index)
		{
			Occupied[Index] = Occupied[Index] != 0
				|| OutputMask[Index] != 0;
		}
	}
	TArray<int32> SurfaceCells;
	MatterFlux::FragmentGeometry::GatherTopExposedActiveMaskCells(
		Occupied,
		VisibleActiveMask,
		GetMaskWidth(),
		GetMaskHeight(),
		SurfaceCells);
	const float CellSize = GetCellSize();
	for (const int32 Index : SurfaceCells)
	{
		if (OutAnchors.Num() >= MaxAnchors)
		{
			break;
		}
		const int32 X = Index % GetMaskWidth();
		const int32 Y = Index / GetMaskWidth();
		MatterFlux::Rendering::FMaterialEmissionAnchor& Anchor =
			OutAnchors.AddDefaulted_GetRef();
		Anchor.WorldPosition = GetActorTransform().TransformPosition(
			FVector(
				(static_cast<float>(X) + 0.5f
					- static_cast<float>(GetMaskWidth()) * 0.5f)
					* CellSize,
				-CellSize * 0.55f,
				(static_cast<float>(Y) + 1.0f
					- static_cast<float>(GetMaskHeight()) * 0.5f)
					* CellSize));
		Anchor.CellSize = CellSize;
		Anchor.EmissionProbability = FMath::Clamp(
			static_cast<float>(Rule->EmissionChancePermille) / 1000.0f,
			0.0f,
			1.0f);
		Anchor.Seed = GetTypeHash(SourceId)
			^ static_cast<uint32>(Index) * 0x9e3779b9u;
	}
}

void AFragment2DSourceActor::ApplyBrokenState()
{
	const bool bShouldHide = bBroken || !bSourceMeshProjectionEnabled;
	SetActorHiddenInGame(bShouldHide);
	MeshComponent->SetVisibility(!bShouldHide, true);
	ApplySourceCollisionState();
}

void AFragment2DSourceActor::ApplySourceCollisionState()
{
	const bool bShouldEnable =
		bEnableSourceCollision
		&& !bBroken
		&& !bAggregateSeparationCollisionSuppressed
		&& MeshComponent
		&& MeshComponent->GetNumSections() > 0;
	SetActorEnableCollision(bShouldEnable);
	if (MeshComponent)
	{
		MeshComponent->SetCollisionEnabled(
			bShouldEnable
				? ECollisionEnabled::QueryAndPhysics
				: ECollisionEnabled::NoCollision);
	}
}

int32 AFragment2DSourceActor::GetMaskWidth() const
{
	return ProceduralSource.HasValidLayout()
		? ProceduralSource.Width
		: (FragmentAsset ? FragmentAsset->GetClampedWidth() : 128);
}

int32 AFragment2DSourceActor::GetMaskHeight() const
{
	return ProceduralSource.HasValidLayout()
		? ProceduralSource.Height
		: (FragmentAsset ? FragmentAsset->GetClampedHeight() : 128);
}

float AFragment2DSourceActor::GetCellSize() const
{
	return ProceduralSource.HasValidLayout()
		? ProceduralSource.CellSize
		: (FragmentAsset ? FragmentAsset->GetClampedCellSize() : 10.0f);
}

int32 AFragment2DSourceActor::GetMinFragmentAreaPixels() const
{
	const int32 SourceMinimum = ProceduralSource.HasValidLayout()
		? ProceduralSource.MinFragmentAreaPixels
		: (FragmentAsset ? FMath::Max(FragmentAsset->MinFragmentAreaPixels, 1) : 16);
	if (!IMatterFluxScriptRuntime::IsAvailable())
	{
		return SourceMinimum;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	return Registry.IsValid()
		? FMath::Max(
			SourceMinimum,
			Registry->Fragmentation.MinDetachedAreaPixels)
		: SourceMinimum;
}

int32 AFragment2DSourceActor::GetMaxFragmentsPerBreak() const
{
	return ProceduralSource.HasValidLayout()
		? ProceduralSource.MaxFragmentsPerBreak
		: FragmentAsset
		? FMath::Clamp(FragmentAsset->MaxFragmentsPerBreak, 1, MatterFlux::FragmentGeometry::MaximumFragmentCount)
		: MatterFlux::FragmentGeometry::MaximumFragmentCount;
}

void AFragment2DSourceActor::EnsureInitialized()
{
	if (!SourceId.IsValid() && (!GetWorld() || !GetWorld()->IsGameWorld() || HasAuthority()))
	{
		SourceId = FGuid::NewGuid();
	}

	const bool bWaitingForReplicatedProceduralSource =
		GetWorld()
		&& GetWorld()->IsGameWorld()
		&& !HasAuthority()
		&& !ProceduralSource.HasValidLayout()
		&& !FragmentAsset;
	if (bWaitingForReplicatedProceduralSource)
	{
		RuntimeMask.Reset();
		SupportAnchorMask.Reset();
		return;
	}

	const int32 ExpectedNum = GetMaskWidth() * GetMaskHeight();
	const bool bEditorPreview = !GetWorld() || !GetWorld()->IsGameWorld();
	if (bEditorPreview || RuntimeMask.Num() != ExpectedNum)
	{
		if (ProceduralSource.HasValidLayout())
		{
			RuntimeMask = ProceduralSource.SolidMask;
		}
		else if (FragmentAsset)
		{
			FragmentAsset->BuildInitialMask(RuntimeMask);
		}
		else
		{
			BuildDefaultMask(RuntimeMask);
		}
	}
	if ((bEditorPreview || SupportAnchorMask.Num() != ExpectedNum)
		&& RuntimeMask.Contains(1))
	{
		const EFragmentSupportMode SupportMode =
			ProceduralSource.HasValidLayout()
				? ProceduralSource.SupportMode
				: (FragmentAsset
					? FragmentAsset->SupportMode
					: DefaultSupportMode);
		if (!MatterFlux::FragmentGeometry::BuildSupportAnchorMask(
			RuntimeMask,
			GetMaskWidth(),
			GetMaskHeight(),
			SupportMode,
			SupportAnchorMask))
		{
			SupportAnchorMask.Reset();
		}
	}
}

bool AFragment2DSourceActor::RebuildSourceMesh()
{
	MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
	const bool bGeometryValid = MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		RuntimeMask, GetMaskWidth(), GetMaskHeight(), GetCellSize(), Geometry);

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	int32 FaceIndexCount = 0;
	const bool bRadial = ProceduralSource.HasValidLayout()
		&& ProceduralSource.GeometryStyle
			== EFragmentSourceGeometryStyle::RadialColumn;
	const bool bVoxelBlocks = ProceduralSource.HasValidLayout()
		&& ProceduralSource.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks;
	const bool bMeshValid = bGeometryValid && (bRadial
		? MatterFlux::FragmentGeometry::BuildRadialColumnMeshFromMask(
			RuntimeMask, GetMaskWidth(), GetMaskHeight(), GetCellSize(),
			Vertices, Triangles, Normals, UVs, FaceIndexCount)
		: bVoxelBlocks
		? MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromMask(
			RuntimeMask, GetMaskWidth(), GetMaskHeight(), GetCellSize(),
			Vertices, Triangles, Normals, UVs, FaceIndexCount)
		: MatterFlux::FragmentGeometry::BuildExtrudedMesh(
			Geometry.Vertices2D, Geometry.TriangleIndices,
			Geometry.OuterContours, Geometry.HoleContours,
			GetCellSize(), Vertices, Triangles, Normals, UVs));

	MeshComponent->ClearAllMeshSections();
	MeshComponent->ClearCollisionConvexMeshes();
	if (bMeshValid)
	{
		if (!bRadial && !bVoxelBlocks)
		{
			FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
		}
		if (FaceIndexCount <= 0 || FaceIndexCount >= Triangles.Num())
		{
			ApplySourceCollisionState();
			return false;
		}
		TArray<int32> FaceTriangles;
		FaceTriangles.Append(Triangles.GetData(), FaceIndexCount);
		TArray<int32> SideTriangles;
		SideTriangles.Append(
			Triangles.GetData() + FaceIndexCount,
			Triangles.Num() - FaceIndexCount);
		MeshComponent->CreateMeshSection(
			0,
			Vertices,
			FaceTriangles,
			Normals,
			UVs,
			TArray<FColor>(),
			TArray<FProcMeshTangent>(),
			bEnableSourceCollision);
		MeshComponent->CreateMeshSection(
			1,
			Vertices,
			SideTriangles,
			Normals,
			UVs,
			TArray<FColor>(),
			TArray<FProcMeshTangent>(),
			bEnableSourceCollision);
	}
	ApplySourceCollisionState();

	ApplySourceMaterial();
	return bMeshValid;
}

void AFragment2DSourceActor::ApplySourceMaterial()
{
	if (!FragmentMaterial)
	{
		DynamicFragmentMaterial = nullptr;
		DynamicFragmentSideMaterial = nullptr;
		MeshComponent->SetMaterial(0, nullptr);
		MeshComponent->SetMaterial(1, nullptr);
		return;
	}
	UMaterialInterface* ParentMaterial =
		MatterFlux::Rendering::ResolveDynamicMaterialParent(FragmentMaterial);
	if (!ParentMaterial)
	{
		DynamicFragmentMaterial = nullptr;
		DynamicFragmentSideMaterial = nullptr;
		MeshComponent->SetMaterial(0, nullptr);
		MeshComponent->SetMaterial(1, nullptr);
		return;
	}

	DynamicFragmentMaterial =
		UMaterialInstanceDynamic::Create(ParentMaterial, this);
	MatterFlux::Rendering::ApplyVoxelMaterialProjection(
		*DynamicFragmentMaterial,
		MatterFlux::Rendering::ResolveVoxelMaterialProjection(
			FragmentColor,
			SourceMaterialId,
			GetCellSize(),
			MatterFlux::Rendering::EVoxelMaterialFaceRole::Primary));
	MeshComponent->SetMaterial(0, DynamicFragmentMaterial);
	DynamicFragmentSideMaterial =
		UMaterialInstanceDynamic::Create(ParentMaterial, this);
	MatterFlux::Rendering::ApplyVoxelMaterialProjection(
		*DynamicFragmentSideMaterial,
		MatterFlux::Rendering::ResolveVoxelMaterialProjection(
			FragmentColor,
			SourceMaterialId,
			GetCellSize(),
			MatterFlux::Rendering::EVoxelMaterialFaceRole::Side));
	MeshComponent->SetMaterial(1, DynamicFragmentSideMaterial);
}

void AFragment2DSourceActor::BuildDefaultMask(TArray<uint8>& OutMask) const
{
	const int32 ExpectedNum = GetMaskWidth() * GetMaskHeight();
	OutMask.Reset(ExpectedNum);
	OutMask.Init(1, ExpectedNum);
}
