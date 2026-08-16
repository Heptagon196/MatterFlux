#include "Fragment/Fragment2DSourceActor.h"

#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DAsset.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
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
	AdvanceCombustion(DeltaSeconds);
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
	DOREPLIFETIME_CONDITION(AFragment2DSourceActor, FragmentMaterial, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AFragment2DSourceActor, FragmentColor, COND_InitialOnly);
	DOREPLIFETIME(AFragment2DSourceActor, ProceduralSource);
	DOREPLIFETIME(AFragment2DSourceActor, bEnableSourceCollision);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		ReplicatedCombustionFuelMask);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		ReplicatedCombustionResidueMask);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		ReplicatedCombustionBurningMask);
	DOREPLIFETIME(
		AFragment2DSourceActor,
		CombustionRevision);
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
	const FName InMaterialId)
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
		RefreshPresenceRegistration();
		return false;
	}
	ResidueMask.Init(0, RuntimeMask.Num());
	VisibleBurningMask.Init(0, RuntimeMask.Num());
	CombustionSimulation.Reset();
	SmokeParticles.Reset();
	CombustionAccumulator = 0.0f;
	CombustionVisualAccumulator = 0.0f;
	TotalSmokeEmissionCount = 0;
	bCombustionVisualDirty = false;
	bCombustionGeometryDirty = false;
	CombustionRevision = 0;
	ReplicatedCombustionFuelMask.Reset();
	ReplicatedCombustionResidueMask.Reset();
	ReplicatedCombustionBurningMask.Reset();
	Revision = 0;
	bBroken = false;
	if (!RebuildSourceMesh())
	{
		ProceduralSource = FFragmentSourceMask();
		RuntimeMask.Reset();
		SupportAnchorMask.Reset();
		ResidueMask.Reset();
		VisibleBurningMask.Reset();
		SourceId.Invalidate();
		SourceMaterialId = NAME_None;
		FragmentColor = FLinearColor::White;
		RefreshPresenceRegistration();
		return false;
	}
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
		|| !FMath::IsFinite(CombustionAccumulator)
		|| CombustionAccumulator < 0.0f
		|| TotalSmokeEmissionCount < 0)
	{
		OutError =
			TEXT("Fragment source runtime state is invalid");
		return false;
	}

	OutState.Revision = Revision;
	OutState.CombustionAccumulator =
		CombustionAccumulator;
	OutState.TotalSmokeEmissionCount =
		TotalSmokeEmissionCount;
	if (CombustionSimulation)
	{
		if (!CombustionSimulation->CaptureState(
			OutState.CombustionState))
		{
			OutError =
				TEXT("Fragment source combustion state could not be captured");
			return false;
		}
		OutState.bHasCombustionState = true;
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
		|| !FMath::IsFinite(State.CombustionAccumulator)
		|| State.CombustionAccumulator < 0.0f
		|| State.CombustionAccumulator > 0.3f
		|| State.TotalSmokeEmissionCount < 0)
	{
		OutError =
			TEXT("Cached fragment source state is invalid");
		return false;
	}

	TUniquePtr<MatterFlux::Combustion::FMaskCombustion>
		RestoredCombustion;
	if (State.bHasCombustionState)
	{
		const FMatterFluxCombustionDefinition* Rule =
			FindCombustionRule();
		if (!Rule)
		{
			OutError =
				TEXT("Cached combustion state does not match the source material or fuel mask");
			return false;
		}
		RestoredCombustion =
			MakeUnique<MatterFlux::Combustion::FMaskCombustion>();
		if (!RestoredCombustion->RestoreState(
			State.CombustionState,
			*Rule,
			OutError))
		{
			return false;
		}
	}

	RuntimeMask = StateRuntimeMask;
	ProceduralSource.SolidMask = RuntimeMask;
	Revision = State.Revision;
	CombustionSimulation = MoveTemp(RestoredCombustion);
	CombustionAccumulator =
		State.CombustionAccumulator;
	TotalSmokeEmissionCount =
		State.TotalSmokeEmissionCount;
	SmokeParticles.Reset();
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

	if (CombustionSimulation)
	{
		ResidueMask =
			CombustionSimulation->GetResidueMask();
		VisibleBurningMask =
			CombustionSimulation->GetBurningMask();
		bCombustionGeometryDirty = true;
		bCombustionVisualDirty = true;
		EnsureCombustionVisualComponents();
		RebuildCombustionVisualization();
		PublishCombustionState();
	}
	else
	{
		ResidueMask.Init(0, RuntimeMask.Num());
		VisibleBurningMask.Init(0, RuntimeMask.Num());
	}
	SetActorTickEnabled(
		CombustionSimulation
		&& CombustionSimulation->IsBurning());
	ForceNetUpdate();
	return true;
}

bool AFragment2DSourceActor::IgniteAtWorldLocation(
	const FVector& WorldLocation,
	const FName IgnitionMaterial,
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
	const FMatterFluxCombustionDefinition* Rule =
		FindCombustionRule();
	if (!Rule || Rule->FlameMaterial != IgnitionMaterial)
	{
		return false;
	}

	if (!CombustionSimulation)
	{
		FFragmentSourceMask CurrentMask = ProceduralSource;
		CurrentMask.SolidMask = RuntimeMask;
		CombustionSimulation =
			MakeUnique<MatterFlux::Combustion::FMaskCombustion>();
		if (!CombustionSimulation->Initialize(
			CurrentMask,
			*Rule,
			EventSeed))
		{
			CombustionSimulation.Reset();
			return false;
		}
	}

	const FIntPoint RequestedCell = WorldToMaskCell(WorldLocation);
	bool bIgnited = CombustionSimulation->Ignite(
		RequestedCell,
		IgnitionMaterial);
	for (int32 Radius = 1;
		!bIgnited
			&& Radius <= FMath::Max(GetMaskWidth(), GetMaskHeight());
		++Radius)
	{
		for (int32 Y = 0; Y < GetMaskHeight() && !bIgnited; ++Y)
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
					if (CombustionSimulation->Ignite(
						FIntPoint(X, Y),
						IgnitionMaterial))
					{
						bIgnited = true;
						break;
					}
				}
				continue;
			}

			if (CombustionSimulation->Ignite(
				FIntPoint(MinimumX, Y),
				IgnitionMaterial))
			{
				bIgnited = true;
				break;
			}
			if (MaximumX != MinimumX
				&& CombustionSimulation->Ignite(
					FIntPoint(MaximumX, Y),
					IgnitionMaterial))
			{
				bIgnited = true;
				break;
			}
		}
	}
	if (!bIgnited)
	{
		return false;
	}

	VisibleBurningMask =
		CombustionSimulation->GetBurningMask();
	bCombustionVisualDirty = true;
	EnsureCombustionVisualComponents();
	PublishCombustionState();
	SetActorTickEnabled(true);
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

void AFragment2DSourceActor::TransferAggregateMembersTo(
	AActor& CarrierActor)
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
		if (FragmentCarrier
			&& FragmentCarrier->AbsorbAggregateSource(*Member))
		{
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

bool AFragment2DSourceActor::PrepareDamageEvent(
	const FFragmentDamageEvent& DamageEvent,
	FPreparedFragmentDamage& OutTransaction) const
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
		OutTransaction.Payloads))
	{
		OutTransaction = FPreparedFragmentDamage();
		UE_LOG(LogMatterFlux, Error, TEXT("Fragment geometry failed on %s; damage transaction was rolled back."), *GetName());
		return false;
	}
	for (FFragmentSpawnPayload& Payload : OutTransaction.Payloads)
	{
		Payload.bEnableCollision = bEnableSourceCollision;
		if (!Payload.bEnableCollision)
		{
			// Collision hulls can be regenerated from the visual geometry if a
			// future gameplay rule promotes this debris. They are unnecessary in
			// the replicated initial payload for render-only decoration fragments.
			Payload.CollisionContours.Reset();
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

	if (CombustionSimulation
		&& !CombustionSimulation->ConstrainFuelMask(
			Transaction.SupportedMask))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Combustion mask could not follow committed damage on %s."),
			*GetName());
		return false;
	}
	if (CombustionSimulation)
	{
		ResidueMask =
			CombustionSimulation->GetResidueMask();
		VisibleBurningMask =
			CombustionSimulation->GetBurningMask();
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
	bCombustionGeometryDirty = true;
	bCombustionVisualDirty = true;
	RebuildSourceMesh();
	if (CombustionSimulation)
	{
		RebuildCombustionVisualization();
		PublishCombustionState();
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
	CombustionSimulation.Reset();
	SmokeParticles.Reset();
	VisibleBurningMask.Init(0, RuntimeMask.Num());
	SetActorTickEnabled(false);
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

void AFragment2DSourceActor::OnRep_SourceId()
{
	RefreshPresenceRegistration();
}

void AFragment2DSourceActor::RefreshPresenceRegistration()
{
	if (RegisteredPresenceSourceId == SourceId)
	{
		return;
	}
	UFragmentSimulationSubsystem* Subsystem =
		GetWorld()
			? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
			: nullptr;
	if (!Subsystem)
	{
		return;
	}
	if (RegisteredPresenceSourceId.IsValid())
	{
		Subsystem->UnregisterSourceActor(*this);
		RegisteredPresenceSourceId.Invalidate();
	}
	if (SourceId.IsValid())
	{
		RegisteredPresenceSourceId = SourceId;
		Subsystem->RegisterSourceActor(*this);
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

void AFragment2DSourceActor::OnRep_CombustionState()
{
	const int32 ExpectedNum = GetMaskWidth() * GetMaskHeight();
	TArray<uint8> FuelMask;
	TArray<uint8> NewResidueMask;
	TArray<uint8> BurningMask;
	if (!UnpackBinaryMask(
			ReplicatedCombustionFuelMask,
			ExpectedNum,
			FuelMask)
		|| !UnpackBinaryMask(
			ReplicatedCombustionResidueMask,
			ExpectedNum,
			NewResidueMask)
		|| !UnpackBinaryMask(
			ReplicatedCombustionBurningMask,
			ExpectedNum,
			BurningMask))
	{
		return;
	}
	RuntimeMask = MoveTemp(FuelMask);
	ResidueMask = MoveTemp(NewResidueMask);
	VisibleBurningMask = MoveTemp(BurningMask);
	TArray<FIntPoint> SmokeSources;
	for (int32 Index = 0; Index < VisibleBurningMask.Num(); ++Index)
	{
		if (VisibleBurningMask[Index] != 0)
		{
			SmokeSources.Emplace(
				Index % GetMaskWidth(),
				Index / GetMaskWidth());
		}
	}
	AddSmokeEmissions(SmokeSources);
	bCombustionGeometryDirty = true;
	bCombustionVisualDirty = true;
	EnsureCombustionVisualComponents();
	RebuildCombustionVisualization();
	SetActorTickEnabled(
		!SmokeSources.IsEmpty()
		|| !SmokeParticles.IsEmpty());
}

void AFragment2DSourceActor::AdvanceCombustion(
	const float DeltaSeconds)
{
	const float ClampedDelta =
		FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	bool bStateChanged = false;
	if (HasAuthority()
		&& CombustionSimulation
		&& CombustionSimulation->IsBurning())
	{
		CombustionAccumulator += ClampedDelta;
		int32 StepsThisFrame = 0;
		while (CombustionAccumulator >= 0.1f
			&& StepsThisFrame < 3)
		{
			CombustionAccumulator -= 0.1f;
			const MatterFlux::Combustion::FStepStats Stats =
				CombustionSimulation->Step();
			AddSmokeEmissions(Stats.SmokeEmissionCells);
			RuntimeMask = CombustionSimulation->GetFuelMask();
			ResidueMask = CombustionSimulation->GetResidueMask();
			VisibleBurningMask =
				CombustionSimulation->GetBurningMask();
			bCombustionGeometryDirty |=
				Stats.ConsumedFuelCells > 0;
			bCombustionVisualDirty = true;
			bStateChanged = true;
			++StepsThisFrame;
		}
	}

	for (int32 Index = SmokeParticles.Num() - 1;
		Index >= 0;
		--Index)
	{
		FSmokeParticle& Particle = SmokeParticles[Index];
		Particle.LocalPosition +=
			Particle.LocalVelocity * ClampedDelta;
		Particle.RemainingLife -= ClampedDelta;
		if (Particle.RemainingLife <= 0.0f)
		{
			SmokeParticles.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		}
		bCombustionVisualDirty = true;
	}

	CombustionVisualAccumulator += ClampedDelta;
	if (bCombustionVisualDirty
		&& CombustionVisualAccumulator >= 0.1f)
	{
		CombustionVisualAccumulator = 0.0f;
		RebuildCombustionVisualization();
	}
	if (bStateChanged)
	{
		PublishCombustionState();
	}

	const bool bStillBurning =
		HasAuthority()
			? CombustionSimulation
				&& CombustionSimulation->IsBurning()
			: VisibleBurningMask.ContainsByPredicate(
				[](const uint8 Value)
				{
					return Value != 0;
				});
	if (!bStillBurning && SmokeParticles.IsEmpty())
	{
		if (FireLight)
		{
			FireLight->SetVisibility(false);
		}
		SetActorTickEnabled(false);
	}
}

void AFragment2DSourceActor::EnsureCombustionVisualComponents()
{
	if (!ResidueMeshComponent)
	{
		ResidueMeshComponent =
			NewObject<UProceduralMeshComponent>(
				this,
				TEXT("CombustionResidueMesh"));
		ResidueMeshComponent->SetupAttachment(MeshComponent);
		ResidueMeshComponent->SetCollisionEnabled(
			ECollisionEnabled::NoCollision);
		ResidueMeshComponent->SetCanEverAffectNavigation(false);
		ResidueMeshComponent->SetCastShadow(false);
		AddInstanceComponent(ResidueMeshComponent);
		ResidueMeshComponent->RegisterComponent();
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
	CreateParticles(FlameInstances, TEXT("CombustionFlames"));
	CreateParticles(SmokeInstances, TEXT("CombustionSmoke"));

	const FMatterFluxCombustionDefinition* Rule =
		FindCombustionRule();
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
		if (!ResidueMaterialInstance)
		{
			ResidueMaterialInstance =
				UMaterialInstanceDynamic::Create(
					FragmentMaterial,
					this);
			ResidueMaterialInstance->SetVectorParameterValue(
				TEXT("Color"),
				ResolveColor(
					Rule->ResidueMaterial,
					FLinearColor(0.08f, 0.07f, 0.06f)));
			ResidueMeshComponent->SetMaterial(
				0,
				ResidueMaterialInstance);
			ResidueMeshComponent->SetMaterial(
				1,
				ResidueMaterialInstance);
		}
		if (!FlameMaterialInstance)
		{
			FlameMaterialInstance =
				UMaterialInstanceDynamic::Create(
					FragmentMaterial,
					this);
			FlameMaterialInstance->SetVectorParameterValue(
				TEXT("Color"),
				ResolveColor(
					Rule->FlameMaterial,
					FLinearColor(1.0f, 0.22f, 0.01f)));
			FlameInstances->SetMaterial(
				0,
				FlameMaterialInstance);
		}
		if (!SmokeMaterialInstance)
		{
			SmokeMaterialInstance =
				UMaterialInstanceDynamic::Create(
					FragmentMaterial,
					this);
			SmokeMaterialInstance->SetVectorParameterValue(
				TEXT("Color"),
				ResolveColor(
					Rule->SmokeMaterial,
					FLinearColor(0.15f, 0.16f, 0.18f, 0.66f)));
			SmokeInstances->SetMaterial(
				0,
				SmokeMaterialInstance);
		}
	}

	if (!FireLight && SourceMaterialId == TEXT("wood"))
	{
		FireLight = NewObject<UPointLightComponent>(
			this,
			TEXT("CombustionFireLight"));
		FireLight->SetupAttachment(MeshComponent);
		FireLight->SetLightColor(FLinearColor(1.0f, 0.20f, 0.01f));
		FireLight->SetIntensity(1800.0f);
		FireLight->SetAttenuationRadius(420.0f);
		FireLight->SetCastShadows(false);
		AddInstanceComponent(FireLight);
		FireLight->RegisterComponent();
	}
}

void AFragment2DSourceActor::RebuildCombustionVisualization()
{
	EnsureCombustionVisualComponents();
	if (bCombustionGeometryDirty)
	{
		RebuildSourceMesh();
		RebuildResidueMesh();
		bCombustionGeometryDirty = false;
	}

	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const float CellSize = GetCellSize();
	const float ParticleScale = CellSize * 0.58f / 100.0f;
	TArray<FTransform> FlameTransforms;
	FVector FlameCenter = FVector::ZeroVector;
	for (int32 Index = 0;
		Index < VisibleBurningMask.Num();
		++Index)
	{
		if (VisibleBurningMask[Index] == 0)
		{
			continue;
		}
		const int32 X = Index % Width;
		const int32 Y = Index / Width;
		const FVector Position(
			(static_cast<float>(X) + 0.5f
				- static_cast<float>(Width) * 0.5f)
				* CellSize,
			-CellSize * 0.82f,
			(static_cast<float>(Y) + 0.65f
				- static_cast<float>(Height) * 0.5f)
				* CellSize);
		FlameCenter += Position;
		FlameTransforms.Emplace(
			FRotator::ZeroRotator,
			Position,
			FVector(ParticleScale));
	}
	MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
		*FlameInstances,
		FlameTransforms);

	TArray<FTransform> SmokeTransforms;
	SmokeTransforms.Reserve(SmokeParticles.Num());
	for (const FSmokeParticle& Particle : SmokeParticles)
	{
		const float LifeAlpha =
			FMath::Clamp(Particle.RemainingLife / 1.8f, 0.0f, 1.0f);
		const float Scale =
			ParticleScale * FMath::Lerp(1.15f, 0.55f, LifeAlpha);
		SmokeTransforms.Emplace(
			FRotator::ZeroRotator,
			Particle.LocalPosition,
			FVector(Scale));
	}
	MatterFlux::Rendering::SynchronizeInstancesWithoutClearing(
		*SmokeInstances,
		SmokeTransforms);

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
	bCombustionVisualDirty = false;
}

void AFragment2DSourceActor::RebuildResidueMesh()
{
	if (!ResidueMeshComponent)
	{
		return;
	}
	ResidueMeshComponent->ClearAllMeshSections();
	if (ResidueMask.IsEmpty()
		|| !ResidueMask.ContainsByPredicate(
			[](const uint8 Value)
			{
				return Value != 0;
			}))
	{
		return;
	}

	MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
	if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		ResidueMask,
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
	if (!MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		Geometry.Vertices2D,
		Geometry.TriangleIndices,
		Geometry.OuterContours,
		Geometry.HoleContours,
		GetCellSize(),
		Vertices,
		Triangles,
		Normals,
		UVs))
	{
		return;
	}
	const int32 FaceIndexCount =
		Geometry.TriangleIndices.Num() * 2;
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
	ResidueMeshComponent->CreateMeshSection(
		0,
		Vertices,
		FaceTriangles,
		Normals,
		UVs,
		TArray<FColor>(),
		TArray<FProcMeshTangent>(),
		false);
	ResidueMeshComponent->CreateMeshSection(
		1,
		Vertices,
		SideTriangles,
		Normals,
		UVs,
		TArray<FColor>(),
		TArray<FProcMeshTangent>(),
		false);
}

void AFragment2DSourceActor::AddSmokeEmissions(
	const TArray<FIntPoint>& Cells)
{
	const int32 MaxParticles = 128;
	const float CellSize = GetCellSize();
	for (const FIntPoint Cell : Cells)
	{
		++TotalSmokeEmissionCount;
		if (SmokeParticles.Num() >= MaxParticles)
		{
			continue;
		}
		const uint32 Hash =
			GetTypeHash(Cell)
				^ static_cast<uint32>(TotalSmokeEmissionCount)
					* 0x9e3779b9u;
		FSmokeParticle& Particle =
			SmokeParticles.AddDefaulted_GetRef();
		Particle.LocalPosition = FVector(
			(static_cast<float>(Cell.X) + 0.5f
				- static_cast<float>(GetMaskWidth()) * 0.5f)
				* CellSize,
			-CellSize,
			(static_cast<float>(Cell.Y) + 1.0f
				- static_cast<float>(GetMaskHeight()) * 0.5f)
				* CellSize);
		Particle.LocalVelocity = FVector(
			(static_cast<int32>(Hash & 7u) - 3) * 1.6f,
			-3.0f,
			24.0f + static_cast<float>((Hash >> 4u) & 15u));
		Particle.RemainingLife =
			1.25f + static_cast<float>((Hash >> 8u) & 7u) * 0.08f;
	}
	bCombustionVisualDirty |= !Cells.IsEmpty();
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

void AFragment2DSourceActor::PublishCombustionState()
{
	if (!HasAuthority() || !CombustionSimulation)
	{
		return;
	}
	TArray<uint8> PackedFuel;
	TArray<uint8> PackedResidue;
	TArray<uint8> PackedBurning;
	if (!PackPresenceMask(
			CombustionSimulation->GetFuelMask(),
			PackedFuel)
		|| !PackPresenceMask(
			CombustionSimulation->GetResidueMask(),
			PackedResidue)
		|| !PackPresenceMask(
			CombustionSimulation->GetBurningMask(),
			PackedBurning))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Combustion state on %s contains an invalid binary mask."),
			*GetName());
		return;
	}
	ReplicatedCombustionFuelMask = MoveTemp(PackedFuel);
	ReplicatedCombustionResidueMask = MoveTemp(PackedResidue);
	ReplicatedCombustionBurningMask = MoveTemp(PackedBurning);
	CombustionRevision =
		CombustionRevision == MAX_int32
			? 0
			: CombustionRevision + 1;
	ForceNetUpdate();
}

const FMatterFluxCombustionDefinition*
AFragment2DSourceActor::FindCombustionRule() const
{
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
	for (const TPair<FName, FMatterFluxCombustionDefinition>& Pair
		: Registry->Combustions)
	{
		if (Pair.Value.FuelMaterial == SourceMaterialId)
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

int32 AFragment2DSourceActor::GetRemainingFuelCellCount() const
{
	int32 Count = 0;
	for (const uint8 Value : RuntimeMask)
	{
		Count += Value != 0 ? 1 : 0;
	}
	return Count;
}

int32 AFragment2DSourceActor::GetResidueCellCount() const
{
	int32 Count = 0;
	for (const uint8 Value : ResidueMask)
	{
		Count += Value != 0 ? 1 : 0;
	}
	return Count;
}

int32 AFragment2DSourceActor::GetBurningCellCount() const
{
	int32 Count = 0;
	for (const uint8 Value : VisibleBurningMask)
	{
		Count += Value != 0 ? 1 : 0;
	}
	return Count;
}

bool AFragment2DSourceActor::IsCombusting() const
{
	return CombustionSimulation
		? CombustionSimulation->IsBurning()
		: GetBurningCellCount() > 0;
}

FBox AFragment2DSourceActor::GetBurningWorldBounds() const
{
	FBox Bounds(ForceInit);
	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const float CellSize = GetCellSize();
	for (int32 Index = 0;
		Index < VisibleBurningMask.Num();
		++Index)
	{
		if (VisibleBurningMask[Index] == 0)
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

FName AFragment2DSourceActor::GetCombustionFlameMaterial() const
{
	const FMatterFluxCombustionDefinition* Rule =
		FindCombustionRule();
	return Rule ? Rule->FlameMaterial : NAME_None;
}

void AFragment2DSourceActor::ApplyBrokenState()
{
	SetActorHiddenInGame(bBroken);
	MeshComponent->SetVisibility(!bBroken, true);
	ApplySourceCollisionState();
}

void AFragment2DSourceActor::ApplySourceCollisionState()
{
	const bool bShouldEnable =
		bEnableSourceCollision
		&& !bBroken
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
	const bool bMeshValid = bGeometryValid && MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		Geometry.Vertices2D, Geometry.TriangleIndices, Geometry.OuterContours, Geometry.HoleContours,
		GetCellSize(), Vertices, Triangles, Normals, UVs);

	MeshComponent->ClearAllMeshSections();
	MeshComponent->ClearCollisionConvexMeshes();
	if (bMeshValid)
	{
		const int32 FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
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

	const auto ConfigureVoxelMaterial =
		[this](UMaterialInstanceDynamic* Material)
		{
			if (!Material)
			{
				return;
			}
			const bool bLeaf = SourceMaterialId == TEXT("leaf");
			const bool bGrass = SourceMaterialId == TEXT("grass");
			const bool bFlower =
				SourceMaterialId == TEXT("flower_pink")
				|| SourceMaterialId == TEXT("flower_gold")
				|| SourceMaterialId == TEXT("flower_blue");
			const bool bStone = SourceMaterialId == TEXT("stone");
			Material->SetScalarParameterValue(
				TEXT("FaceContrast"),
				bFlower ? 0.42f
					: (bLeaf ? 0.56f
						: (bGrass ? 0.52f
							: (bStone ? 0.72f : 0.70f))));
			Material->SetScalarParameterValue(
				TEXT("ColorVariation"),
				bFlower ? 0.012f : (bLeaf ? 0.022f : 0.03f));
			Material->SetScalarParameterValue(
				TEXT("PixelSize"),
				FMath::Max(GetCellSize(), 4.0f));
			const float Roughness =
				SourceMaterialId == TEXT("stone") ? 0.96f
				: (SourceMaterialId == TEXT("leaf") ? 0.88f : 0.82f);
			Material->SetScalarParameterValue(TEXT("Roughness"), Roughness);
			Material->SetScalarParameterValue(
				TEXT("ShadowLift"),
				bFlower ? 0.38f
					: (bLeaf ? 0.32f
						: (bGrass ? 0.28f
							: (bStone ? 0.12f : 0.18f))));
		};

	if (FragmentColor.Equals(FLinearColor::White))
	{
		DynamicFragmentMaterial = nullptr;
		MeshComponent->SetMaterial(0, FragmentMaterial);
	}
	else
	{
		DynamicFragmentMaterial =
			UMaterialInstanceDynamic::Create(FragmentMaterial, this);
		DynamicFragmentMaterial->SetVectorParameterValue(
			TEXT("Color"),
			FragmentColor);
		ConfigureVoxelMaterial(DynamicFragmentMaterial);
		MeshComponent->SetMaterial(0, DynamicFragmentMaterial);
	}
	DynamicFragmentSideMaterial =
		UMaterialInstanceDynamic::Create(FragmentMaterial, this);
	const bool bSoftDecoration =
		SourceMaterialId == TEXT("leaf")
		|| SourceMaterialId == TEXT("grass")
		|| SourceMaterialId == TEXT("flower_pink")
		|| SourceMaterialId == TEXT("flower_gold")
		|| SourceMaterialId == TEXT("flower_blue");
	const float SideBrightness = bSoftDecoration ? 0.88f : 0.78f;
	const FLinearColor SideColor(
		FragmentColor.R * SideBrightness,
		FragmentColor.G * SideBrightness,
		FragmentColor.B * SideBrightness,
		FragmentColor.A);
	DynamicFragmentSideMaterial->SetVectorParameterValue(
		TEXT("Color"),
		SideColor);
	ConfigureVoxelMaterial(DynamicFragmentSideMaterial);
	MeshComponent->SetMaterial(1, DynamicFragmentSideMaterial);
}

void AFragment2DSourceActor::BuildDefaultMask(TArray<uint8>& OutMask) const
{
	const int32 ExpectedNum = GetMaskWidth() * GetMaskHeight();
	OutMask.Reset(ExpectedNum);
	OutMask.Init(1, ExpectedNum);
}
