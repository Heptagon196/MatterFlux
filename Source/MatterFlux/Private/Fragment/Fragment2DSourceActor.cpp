#include "Fragment/Fragment2DSourceActor.h"

#include "Algo/Count.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DAsset.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "Material/MatterFluxLocalMaterialReaction.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/MatterFluxInstanceVisuals.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "TimerManager.h"

namespace
{

}

AFragment2DSourceActor::AFragment2DSourceActor()
{
	bReplicates = true;
	PrimaryActorTick.bCanEverTick = false;
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
	DOREPLIFETIME(AFragment2DSourceActor, ReplicatedMaterialVolumeState);
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
	Revision = 0;
	bBroken = false;
	MaterialVolumeFields = FMaterialVolumeFields();
	RefreshMaterialVolumeTopology();
	if (!RebuildSourceMesh())
	{
		ProceduralSource = FFragmentSourceMask();
		RuntimeMask.Reset();
		SupportAnchorMask.Reset();
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
	Revision = 0;
	bBroken = false;
	MaterialVolumeFields = FMaterialVolumeFields();
	RefreshMaterialVolumeTopology();
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
		|| Revision < 0)
	{
		OutError =
			TEXT("Fragment source runtime state is invalid");
		return false;
	}

	OutState.Revision = Revision;
	OutState.SetRuntimeMask(RuntimeMask);
	if (MaterialVolumeTopology.IsSet())
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		FLocalMaterialReactionProgram Program;
		if (!Registry.IsValid() || !Program.Compile(*Registry, OutError))
		{
			OutError = TEXT("Fragment Volume content program could not be captured");
			return false;
		}
		OutState.VolumeTopologyRevision =
			MaterialVolumeTopology->TopologyRevision;
		OutState.VolumeFieldRevision = MaterialVolumeFields.FieldRevision;
		OutState.VolumeEnvironmentEnergy = MaterialVolumeFields.EnvironmentEnergy;
		for (int32 V = 0; V < GetMaskHeight(); ++V)
		{
			for (int32 U = 0; U < GetMaskWidth(); ++U)
			{
				const int32 Index = V * GetMaskWidth() + U;
				if (RuntimeMask[Index] == 0)
				{
					continue;
				}
				const FIntVector Cell(U, V, 0);
				uint16 MaterialIndex = 0;
				FName MaterialId = NAME_None;
				if (!FMaterialVolumeAlgorithms::TryGetCellMaterial(
						MaterialVolumeTopology.GetValue(), Cell, MaterialIndex)
					|| !Program.TryGetMaterialId(MaterialIndex, MaterialId))
				{
					OutError = TEXT("Fragment Volume contains an unknown material index");
					return false;
				}
				const uint16* EnergyOverride =
					MaterialVolumeFields.EnergyOverrides.Find(Cell);
				if (MaterialId != SourceMaterialId || EnergyOverride)
				{
					OutState.VolumeCellStates.Add({
						Cell,
						MaterialId,
						EnergyOverride
							? *EnergyOverride
							: MaterialVolumeFields.EnvironmentEnergy });
				}
			}
		}
	}
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
		|| State.VolumeTopologyRevision < 0
		|| State.VolumeFieldRevision < 0
		|| StateRuntimeMask.Num() != ExpectedCellCount
		|| StateRuntimeMask.ContainsByPredicate(
			[](const uint8 Value)
			{
				return Value > 1;
			}))
	{
		OutError =
			TEXT("Cached fragment source state is invalid");
		return false;
	}
	FLocalMaterialReactionProgram VolumeProgram;
	if (!State.VolumeCellStates.IsEmpty())
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (!Registry.IsValid() || !VolumeProgram.Compile(*Registry, OutError))
		{
			OutError = TEXT("Cached Fragment Volume program is unavailable");
			return false;
		}
		TSet<FIntVector> SeenCells;
		for (const FFragment2DMaterialVolumeCellState& CellState
			: State.VolumeCellStates)
		{
			const int32 Index =
				CellState.Cell.Y * GetMaskWidth() + CellState.Cell.X;
			uint16 MaterialIndex = 0;
			if (CellState.Cell.Z != 0
				|| CellState.Cell.X < 0 || CellState.Cell.Y < 0
				|| CellState.Cell.X >= GetMaskWidth()
				|| CellState.Cell.Y >= GetMaskHeight()
				|| !StateRuntimeMask.IsValidIndex(Index)
				|| StateRuntimeMask[Index] == 0
				|| SeenCells.Contains(CellState.Cell)
				|| !VolumeProgram.TryGetMaterialIndex(
					CellState.MaterialId, MaterialIndex)
				|| MaterialIndex == 0)
			{
				OutError = TEXT("Cached Fragment Volume cell state is invalid");
				return false;
			}
			SeenCells.Add(CellState.Cell);
		}
	}

	RuntimeMask = StateRuntimeMask;
	ProceduralSource.SolidMask = RuntimeMask;
	Revision = State.Revision;
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

	RefreshMaterialVolumeTopology();
	if (!State.VolumeCellStates.IsEmpty())
	{
		if (!MaterialVolumeTopology.IsSet())
		{
			OutError = TEXT("Cached Fragment Volume could not be constructed");
			return false;
		}
		FMaterialVolumeTopology Candidate = MaterialVolumeTopology.GetValue();
		for (const FFragment2DMaterialVolumeCellState& CellState
			: State.VolumeCellStates)
		{
			uint16 MaterialIndex = 0;
			VolumeProgram.TryGetMaterialIndex(CellState.MaterialId, MaterialIndex);
			FMaterialVolumeTopology Changed;
			if (!FMaterialVolumeAlgorithms::SetCellMaterial(
					Candidate,
					CellState.Cell,
					MaterialIndex,
					Changed,
					OutError))
			{
				return false;
			}
			Candidate = MoveTemp(Changed);
		}
		Candidate.TopologyRevision = State.VolumeTopologyRevision;
		MaterialVolumeTopology = MoveTemp(Candidate);
	}
	else if (MaterialVolumeTopology.IsSet())
	{
		MaterialVolumeTopology->TopologyRevision = State.VolumeTopologyRevision;
	}
	MaterialVolumeFields = FMaterialVolumeFields();
	MaterialVolumeFields.EnvironmentEnergy = State.VolumeEnvironmentEnergy;
	MaterialVolumeFields.FieldRevision = State.VolumeFieldRevision;
	for (const FFragment2DMaterialVolumeCellState& CellState
		: State.VolumeCellStates)
	{
		if (CellState.Energy != State.VolumeEnvironmentEnergy)
		{
			MaterialVolumeFields.EnergyOverrides.Add(
				CellState.Cell, CellState.Energy);
		}
	}
	RebuildMaterialVisualization();
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
	AMatterFluxPlayableWorldActor* WorldOwner =
		Cast<AMatterFluxPlayableWorldActor>(GetOwner());
	if (!WorldOwner && GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(GetWorld()); It; ++It)
		{
			WorldOwner = *It;
			break;
		}
	}
	return WorldOwner
		&& WorldOwner->ApplyMaterialStimulusAtWorldLocation(
			WorldLocation, StimulusMaterial, EventSeed, GetCellSize()) > 0;
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
	RefreshMaterialVolumeTopology();
	PublishReplicatedMaterialVolumeState();
	RebuildSourceMesh();
	RebuildMaterialVisualization();
	ForceNetUpdate();
	return true;
}

void AFragment2DSourceActor::RefreshMaterialVolumeTopology()
{
	bool bRemovedField = false;
	for (auto It = MaterialVolumeFields.EnergyOverrides.CreateIterator(); It; ++It)
	{
		const FIntVector Cell = It.Key();
		const int32 Index = Cell.Y * ProceduralSource.Width + Cell.X;
		if (Cell.Z != 0
			|| Cell.X < 0 || Cell.Y < 0
			|| Cell.X >= ProceduralSource.Width
			|| Cell.Y >= ProceduralSource.Height
			|| !RuntimeMask.IsValidIndex(Index)
			|| RuntimeMask[Index] == 0)
		{
			It.RemoveCurrent();
			bRemovedField = true;
		}
	}
	if (bRemovedField)
	{
		++MaterialVolumeFields.FieldRevision;
	}
	TOptional<FMaterialVolumeTopology> PreviousTopology =
		MoveTemp(MaterialVolumeTopology);
	MaterialVolumeTopology.Reset();
	if (!ProceduralSource.HasValidLayout()
		|| RuntimeMask.Num()
			!= ProceduralSource.Width * ProceduralSource.Height)
	{
		return;
	}
	FFragmentSourceMask TopologyInputMask = ProceduralSource;
	TopologyInputMask.SolidMask = RuntimeMask;
	uint16 SourceMaterialIndex = 1;
	uint16 SourceDefaultEnergy = 0;
	if (IMatterFluxScriptRuntime::IsAvailable())
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::Get().GetActiveRegistry();
		FLocalMaterialReactionProgram Program;
		FString ProgramError;
		if (Registry.IsValid()
			&& Program.Compile(*Registry, ProgramError))
		{
			uint16 ResolvedIndex = 0;
			if (Program.TryGetMaterialIndex(SourceMaterialId, ResolvedIndex)
				&& ResolvedIndex != 0)
			{
				SourceMaterialIndex = ResolvedIndex;
			}
			FMaterialElementState DefaultState;
			if (Program.MakeState(
					SourceMaterialId, 255, TOptional<uint16>(), DefaultState))
			{
				SourceDefaultEnergy = DefaultState.Energy;
			}
		}
	}
	FMaterialVolumeTopology Topology;
	FString Error;
	if (!FMaterialVolumeConverters::FromLegacyMaskXZY(
			TopologyInputMask, 0, 1, Topology, Error, SourceMaterialIndex))
	{
		UE_LOG(LogMatterFlux, Warning,
			TEXT("Fragment Volume topology failed for %s: %s"),
			*GetName(), *Error);
		return;
	}
	Topology.DefinitionId = SourceMaterialId.IsNone()
		? FName(TEXT("fragment.source"))
		: SourceMaterialId;
	if (MaterialVolumeFields.FieldRevision == 0
		&& MaterialVolumeFields.EnergyOverrides.IsEmpty())
	{
		MaterialVolumeFields.EnvironmentEnergy = SourceDefaultEnergy;
	}
	if (PreviousTopology.IsSet())
	{
		for (int32 V = 0; V < ProceduralSource.Height; ++V)
		{
			for (int32 U = 0; U < ProceduralSource.Width; ++U)
			{
				if (RuntimeMask[V * ProceduralSource.Width + U] == 0)
				{
					continue;
				}
				const FIntVector Cell(U, V, 0);
				uint16 PreviousMaterial = 0;
				if (!FMaterialVolumeAlgorithms::TryGetCellMaterial(
						PreviousTopology.GetValue(), Cell, PreviousMaterial)
					|| PreviousMaterial == SourceMaterialIndex)
				{
					continue;
				}
				FMaterialVolumeTopology WithMaterial;
				if (!FMaterialVolumeAlgorithms::SetCellMaterial(
						Topology, Cell, PreviousMaterial, WithMaterial, Error))
				{
					UE_LOG(LogMatterFlux, Warning,
						TEXT("Fragment Volume material restore failed for %s: %s"),
						*GetName(), *Error);
					return;
				}
				Topology = MoveTemp(WithMaterial);
			}
		}
		Topology.TopologyRevision =
			FMaterialVolumeAlgorithms::ComputeLogicalHash(Topology)
				== FMaterialVolumeAlgorithms::ComputeLogicalHash(
					PreviousTopology.GetValue())
			? PreviousTopology->TopologyRevision
			: PreviousTopology->TopologyRevision + 1;
	}
	else
	{
		Topology.TopologyRevision = 0;
	}
	if (ProceduralSource.SupportMode == EFragmentSupportMode::Bottom)
	{
		for (int32 X = 0; X < ProceduralSource.Width; ++X)
		{
			if (RuntimeMask[X] != 0)
			{
				Topology.StructuralAnchors.Add(FIntVector(X, 0, 0));
			}
		}
	}
	MaterialVolumeTopology.Emplace(MoveTemp(Topology));
}

bool AFragment2DSourceActor::BuildMaterialVolumeInstance(
	FMaterialVolumeInstance& OutInstance) const
{
	OutInstance = FMaterialVolumeInstance();
	if (!MaterialVolumeTopology.IsSet() || !SourceId.IsValid())
	{
		return false;
	}
	OutInstance.InstanceId = SourceId;
	OutInstance.ParentInstanceId = AggregateId;
	OutInstance.WorldTransform = GetActorTransform();
	OutInstance.LinearVelocity = GetVelocity();
	OutInstance.Topology = MaterialVolumeTopology.GetValue();
	OutInstance.Fields = MaterialVolumeFields;
	return true;
}

bool AFragment2DSourceActor::TryGetMaterialVolumeCellAtWorldLocation(
	const FVector& WorldLocation,
	FIntVector& OutVolumeCell) const
{
	OutVolumeCell = FIntVector::ZeroValue;
	if (!SourceId.IsValid()
		|| !ProceduralSource.HasValidLayout()
		|| RuntimeMask.Num() != ProceduralSource.Width * ProceduralSource.Height
		|| WorldLocation.ContainsNaN()
		|| !GetActorTransform().IsValid())
	{
		return false;
	}
	const FVector Local = GetActorTransform().InverseTransformPosition(WorldLocation);
	const double CellSize = ProceduralSource.CellSize;
	int32 U = FMath::FloorToInt(
		Local.X / CellSize + static_cast<double>(ProceduralSource.Width) * 0.5);
	int32 V = FMath::FloorToInt(
		Local.Z / CellSize + static_cast<double>(ProceduralSource.Height) * 0.5);
	const auto IsOccupied = [this](const int32 CandidateU, const int32 CandidateV)
	{
		return CandidateU >= 0 && CandidateV >= 0
			&& CandidateU < ProceduralSource.Width
			&& CandidateV < ProceduralSource.Height
			&& RuntimeMask[CandidateV * ProceduralSource.Width + CandidateU] != 0;
	};
	if (!IsOccupied(U, V))
	{
		// Canonical source bounds include empty cells around sparse silhouettes.
		// Resolve a surface contact to the nearest occupied cell. The caller uses
		// that exact cell center for its bounded topology edit, so empty silhouette
		// space cannot become a second, ambiguous reaction location. Index order is
		// the deterministic tie-break at exact corners.
		double BestDistanceSquared = TNumericLimits<double>::Max();
		int32 BestIndex = INDEX_NONE;
		for (int32 Index = 0; Index < RuntimeMask.Num(); ++Index)
		{
			if (RuntimeMask[Index] == 0)
			{
				continue;
			}
			const int32 CandidateU = Index % ProceduralSource.Width;
			const int32 CandidateV = Index / ProceduralSource.Width;
			const double CenterX =
				(static_cast<double>(CandidateU) + 0.5
					- static_cast<double>(ProceduralSource.Width) * 0.5) * CellSize;
			const double CenterZ =
				(static_cast<double>(CandidateV) + 0.5
					- static_cast<double>(ProceduralSource.Height) * 0.5) * CellSize;
			const double DeltaX = Local.X - CenterX;
			const double DeltaZ = Local.Z - CenterZ;
			const double DistanceSquared = DeltaX * DeltaX + DeltaZ * DeltaZ;
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				BestIndex = Index;
			}
		}
		if (BestIndex == INDEX_NONE)
		{
			return false;
		}
		U = BestIndex % ProceduralSource.Width;
		V = BestIndex / ProceduralSource.Width;
	}
	// Legacy sources have one centered extrusion cell along local Y.
	OutVolumeCell = FIntVector(U, V, 0);
	return true;
}

uint16 AFragment2DSourceActor::GetMaterialVolumeCellEnergy(
	const FIntVector& VolumeCell,
	const uint16 DefaultEnergy) const
{
	if (const uint16* Override =
		MaterialVolumeFields.EnergyOverrides.Find(VolumeCell))
	{
		return *Override;
	}
	return DefaultEnergy;
}

bool AFragment2DSourceActor::CommitMaterialVolumeCellEnergy(
	const FIntVector& VolumeCell,
	const uint16 DefaultEnergy,
	const uint16 ExpectedEnergy,
	const uint16 AfterEnergy)
{
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| VolumeCell.Z != 0
		|| VolumeCell.X < 0 || VolumeCell.Y < 0
		|| VolumeCell.X >= GetMaskWidth()
		|| VolumeCell.Y >= GetMaskHeight()
		|| !RuntimeMask.IsValidIndex(
			VolumeCell.Y * GetMaskWidth() + VolumeCell.X)
		|| RuntimeMask[VolumeCell.Y * GetMaskWidth() + VolumeCell.X] == 0
		|| GetMaterialVolumeCellEnergy(VolumeCell, DefaultEnergy)
			!= ExpectedEnergy)
	{
		return false;
	}
	MaterialVolumeFields.EnvironmentEnergy = DefaultEnergy;
	const bool bCommitted = MaterialVolumeFields.SetEnergy(VolumeCell, AfterEnergy)
		|| ExpectedEnergy == AfterEnergy;
	if (bCommitted)
	{
		PublishReplicatedMaterialVolumeState();
		RebuildMaterialVisualization();
		MarkSharedSmokeVisualizationDirty();
		ForceNetUpdate();
	}
	return bCommitted;
}

bool AFragment2DSourceActor::CommitMaterialVolumeCellState(
	const FIntVector& VolumeCell,
	const uint16 DefaultEnergy,
	const FMaterialElementState& ExpectedBefore,
	const FMaterialElementState& After,
	FString& OutError)
{
	OutError.Reset();
	if (!MaterialVolumeTopology.IsSet()
		|| ExpectedBefore.Amount == 0
		|| After.Amount != ExpectedBefore.Amount
		|| After.MaterialIndex == 0
		|| GetMaterialVolumeCellEnergy(VolumeCell, DefaultEnergy)
			!= ExpectedBefore.Energy)
	{
		OutError = TEXT("Volume cell state base does not match");
		return false;
	}
	uint16 CurrentMaterial = 0;
	if (!FMaterialVolumeAlgorithms::TryGetCellMaterial(
			MaterialVolumeTopology.GetValue(), VolumeCell, CurrentMaterial)
		|| CurrentMaterial != ExpectedBefore.MaterialIndex)
	{
		OutError = TEXT("Volume topology material base does not match");
		return false;
	}
	FMaterialVolumeTopology Candidate = MaterialVolumeTopology.GetValue();
	if (After.MaterialIndex != CurrentMaterial
		&& !FMaterialVolumeAlgorithms::SetCellMaterial(
			MaterialVolumeTopology.GetValue(),
			VolumeCell,
			After.MaterialIndex,
			Candidate,
			OutError))
	{
		return false;
	}
	MaterialVolumeFields.EnvironmentEnergy = DefaultEnergy;
	if (After.Energy != ExpectedBefore.Energy)
	{
		MaterialVolumeFields.SetEnergy(VolumeCell, After.Energy);
	}
	MaterialVolumeTopology = MoveTemp(Candidate);
	PublishReplicatedMaterialVolumeState();
	RebuildMaterialVisualization();
	MarkSharedSmokeVisualizationDirty();
	ForceNetUpdate();
	return true;
}

bool AFragment2DSourceActor::CommitMaterialVolumeElementBatch(
	const FMaterialDeltaBatch& Batch,
	FString& OutError)
{
	OutError.Reset();
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| !MaterialVolumeTopology.IsSet()
		|| !Batch.IsValid(OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Volume reaction batch cannot be committed");
		}
		return false;
	}
	FMaterialVolumeTopology CandidateTopology;
	FMaterialVolumeFields CandidateFields;
	if (!PrepareMaterialVolumeElementDeltas(
			Batch.ElementDeltas,
			CandidateTopology,
			CandidateFields,
			OutError))
	{
		return false;
	}
	CommitPreparedMaterialVolumeState(
		MoveTemp(CandidateTopology), MoveTemp(CandidateFields));
	return true;
}

bool AFragment2DSourceActor::PrepareMaterialVolumeElementDeltas(
	const TConstArrayView<FMaterialElementDelta> Deltas,
	FMaterialVolumeTopology& OutTopology,
	FMaterialVolumeFields& OutFields,
	FString& OutError) const
{
	OutError.Reset();
	if (!MaterialVolumeTopology.IsSet())
	{
		OutError = TEXT("Volume reaction target has no topology");
		return false;
	}

	FMaterialVolumeTopology CandidateTopology = MaterialVolumeTopology.GetValue();
	FMaterialVolumeFields CandidateFields = MaterialVolumeFields;
	for (const FMaterialElementDelta& Delta : Deltas)
	{
		if (Delta.Address.Kind != EMaterialElementAddressKind::VolumeCell
			|| Delta.Address.OwnerId != SourceId
			|| Delta.ExpectedBefore.Amount != 255
			|| Delta.After.Amount != 255
			|| Delta.After.MaterialIndex == 0)
		{
			OutError = TEXT("Volume reaction batch contains an unsupported element delta");
			return false;
		}
		uint16 CurrentMaterial = 0;
		if (!FMaterialVolumeAlgorithms::TryGetCellMaterial(
				CandidateTopology, Delta.Address.Cell, CurrentMaterial)
			|| CurrentMaterial != Delta.ExpectedBefore.MaterialIndex
			|| CandidateFields.GetEnergy(Delta.Address.Cell)
				!= Delta.ExpectedBefore.Energy)
		{
			OutError = TEXT("Volume reaction batch base does not match");
			return false;
		}
		if (Delta.After.MaterialIndex != CurrentMaterial)
		{
			FMaterialVolumeTopology Changed;
			if (!FMaterialVolumeAlgorithms::SetCellMaterial(
					CandidateTopology,
					Delta.Address.Cell,
					Delta.After.MaterialIndex,
					Changed,
					OutError))
			{
				return false;
			}
			CandidateTopology = MoveTemp(Changed);
		}
		CandidateFields.SetEnergy(Delta.Address.Cell, Delta.After.Energy);
	}
	if (!CandidateTopology.IsValid(&OutError) || !CandidateFields.IsValid())
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Volume reaction batch produced invalid state");
		}
		return false;
	}
	OutTopology = MoveTemp(CandidateTopology);
	OutFields = MoveTemp(CandidateFields);
	return true;
}

void AFragment2DSourceActor::CommitPreparedMaterialVolumeState(
	FMaterialVolumeTopology&& Topology,
	FMaterialVolumeFields&& Fields)
{
	check(Topology.IsValid() && Fields.IsValid());
	MaterialVolumeTopology = MoveTemp(Topology);
	MaterialVolumeFields = MoveTemp(Fields);
	PublishReplicatedMaterialVolumeState();
	RebuildMaterialVisualization();
	MarkSharedSmokeVisualizationDirty();
	ForceNetUpdate();
}

void AFragment2DSourceActor::MarkBroken()
{
	if (!HasAuthority() && GetWorld() && GetWorld()->IsGameWorld())
	{
		return;
	}
	bBroken = true;
	if (FlameInstances)
	{
		FlameInstances->ClearInstances();
	}
	if (FireLight)
	{
		FireLight->SetVisibility(false);
	}
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
	OnRep_MaterialVolumeState();
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
	RefreshMaterialVolumeTopology();
	OnRep_MaterialVolumeState();
	RebuildSourceMesh();
}

void AFragment2DSourceActor::PublishReplicatedMaterialVolumeState()
{
	if (!HasAuthority() || !SourceId.IsValid() || !MaterialVolumeTopology.IsSet())
	{
		return;
	}
	FFragment2DSourceStreamingState State;
	FString Error;
	if (!CaptureStreamingState(State, Error))
	{
		UE_LOG(LogMatterFlux, Error,
			TEXT("Cannot publish Volume state for %s: %s"),
			*GetName(), *Error);
		return;
	}
	FMatterFluxReplicatedMaterialVolumeState Candidate;
	Candidate.InstanceId = SourceId;
	Candidate.TopologyRevision = State.VolumeTopologyRevision;
	Candidate.FieldRevision = State.VolumeFieldRevision;
	Candidate.EnvironmentEnergy = State.VolumeEnvironmentEnergy;
	Candidate.Cells.Reserve(State.VolumeCellStates.Num());
	for (const FFragment2DMaterialVolumeCellState& Cell : State.VolumeCellStates)
	{
		Candidate.Cells.Add({ Cell.Cell, Cell.MaterialId, Cell.Energy });
	}
	Candidate.Cells.Sort([](
		const FMatterFluxReplicatedMaterialVolumeCell& A,
		const FMatterFluxReplicatedMaterialVolumeCell& B)
	{
		return A.Cell.X != B.Cell.X ? A.Cell.X < B.Cell.X
			: A.Cell.Y != B.Cell.Y ? A.Cell.Y < B.Cell.Y
			: A.Cell.Z < B.Cell.Z;
	});
	if (!(Candidate == ReplicatedMaterialVolumeState))
	{
		ReplicatedMaterialVolumeState = MoveTemp(Candidate);
		ForceNetUpdate();
	}
}

bool AFragment2DSourceActor::ApplyReplicatedMaterialVolumeState(
	FString& OutError)
{
	OutError.Reset();
	if (ReplicatedMaterialVolumeState.InstanceId != SourceId
		|| !SourceId.IsValid()
		|| !MaterialVolumeTopology.IsSet()
		|| ReplicatedMaterialVolumeState.TopologyRevision < 0
		|| ReplicatedMaterialVolumeState.FieldRevision < 0)
	{
		OutError = TEXT("replicated Volume snapshot prerequisites are unavailable");
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	FLocalMaterialReactionProgram Program;
	if (!Registry.IsValid() || !Program.Compile(*Registry, OutError))
	{
		OutError = TEXT("replicated Volume content program is unavailable");
		return false;
	}
	FMaterialVolumeTopology CandidateTopology = MaterialVolumeTopology.GetValue();
	uint16 SourceMaterialIndex = 0;
	if (!Program.TryGetMaterialIndex(SourceMaterialId, SourceMaterialIndex)
		|| SourceMaterialIndex == 0)
	{
		OutError = TEXT("replicated Volume source material is unknown");
		return false;
	}
	// This property is a full sparse snapshot. Reset every occupied cell to the
	// definition material first so a later snapshot can remove an old override.
	for (int32 V = 0; V < GetMaskHeight(); ++V)
	{
		for (int32 U = 0; U < GetMaskWidth(); ++U)
		{
			const int32 Index = V * GetMaskWidth() + U;
			if (!RuntimeMask.IsValidIndex(Index) || RuntimeMask[Index] == 0)
			{
				continue;
			}
			FMaterialVolumeTopology Changed;
			if (!FMaterialVolumeAlgorithms::SetCellMaterial(
					CandidateTopology,
					FIntVector(U, V, 0),
					SourceMaterialIndex,
					Changed,
					OutError))
			{
				return false;
			}
			CandidateTopology = MoveTemp(Changed);
		}
	}
	TSet<FIntVector> SeenCells;
	for (const FMatterFluxReplicatedMaterialVolumeCell& Cell
		: ReplicatedMaterialVolumeState.Cells)
	{
		uint16 MaterialIndex = 0;
		uint16 ExistingMaterial = 0;
		if (SeenCells.Contains(Cell.Cell)
			|| !Program.TryGetMaterialIndex(Cell.MaterialId, MaterialIndex)
			|| MaterialIndex == 0
			|| !FMaterialVolumeAlgorithms::TryGetCellMaterial(
				CandidateTopology, Cell.Cell, ExistingMaterial))
		{
			OutError = TEXT("replicated Volume cell is invalid");
			return false;
		}
		SeenCells.Add(Cell.Cell);
		FMaterialVolumeTopology Changed;
		if (!FMaterialVolumeAlgorithms::SetCellMaterial(
				CandidateTopology, Cell.Cell, MaterialIndex, Changed, OutError))
		{
			return false;
		}
		CandidateTopology = MoveTemp(Changed);
	}
	CandidateTopology.TopologyRevision =
		ReplicatedMaterialVolumeState.TopologyRevision;
	FMaterialVolumeFields CandidateFields;
	CandidateFields.EnvironmentEnergy =
		ReplicatedMaterialVolumeState.EnvironmentEnergy;
	CandidateFields.FieldRevision = ReplicatedMaterialVolumeState.FieldRevision;
	for (const FMatterFluxReplicatedMaterialVolumeCell& Cell
		: ReplicatedMaterialVolumeState.Cells)
	{
		if (Cell.Energy != CandidateFields.EnvironmentEnergy)
		{
			CandidateFields.EnergyOverrides.Add(Cell.Cell, Cell.Energy);
		}
	}
	if (!CandidateTopology.IsValid(&OutError) || !CandidateFields.IsValid())
	{
		return false;
	}
	MaterialVolumeTopology = MoveTemp(CandidateTopology);
	MaterialVolumeFields = MoveTemp(CandidateFields);
	RebuildMaterialVisualization();
	MarkSharedSmokeVisualizationDirty();
	return true;
}

void AFragment2DSourceActor::OnRep_MaterialVolumeState()
{
	if (HasAuthority() || !ReplicatedMaterialVolumeState.InstanceId.IsValid())
	{
		return;
	}
	FString Error;
	if (!ApplyReplicatedMaterialVolumeState(Error))
	{
		UE_LOG(LogMatterFlux, Warning,
			TEXT("Client could not apply Volume snapshot for %s: %s"),
			*GetName(), *Error);
	}
}

void AFragment2DSourceActor::OnRep_SourceAppearance()
{
	ApplySourceMaterial();
}

void AFragment2DSourceActor::OnRep_SourceCollision()
{
	ApplySourceCollisionState();
}

void AFragment2DSourceActor::EnsureReactionVisualComponents()
{
	if (!OutputMeshComponent)
	{
		OutputMeshComponent =
			NewObject<UProceduralMeshComponent>(
				this,
				TEXT("MaterialOverrideMesh"));
		OutputMeshComponent->SetupAttachment(MeshComponent);
		OutputMeshComponent->SetCollisionObjectType(ECC_WorldStatic);
		OutputMeshComponent->SetCollisionResponseToAllChannels(ECR_Block);
		OutputMeshComponent->SetCollisionEnabled(
			ECollisionEnabled::NoCollision);
		OutputMeshComponent->bUseComplexAsSimpleCollision = true;
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

	if (FragmentMaterial)
	{
		if (!StimulusMaterialInstance)
		{
			StimulusMaterialInstance =
				UMaterialInstanceDynamic::Create(
					FragmentMaterial,
					this);
			StimulusMaterialInstance->SetVectorParameterValue(
				TEXT("Color"),
				FLinearColor(1.0f, 0.22f, 0.01f));
			FlameInstances->SetMaterial(
				0,
				StimulusMaterialInstance);
		}
	}

	if (!FireLight)
	{
		FireLight = NewObject<UPointLightComponent>(
			this,
			TEXT("ReactionFireLight"));
		FireLight->SetupAttachment(MeshComponent);
		FireLight->SetLightColor(FLinearColor(1.0f, 0.20f, 0.01f));
		FireLight->SetIntensity(1800.0f);
		FireLight->SetAttenuationRadius(420.0f);
		FireLight->SetCastShadows(false);
		// Registration must not expose a one-frame fire flash before the first
		// material projection has derived whether any hot cells exist.
		FireLight->SetVisibility(false);
		AddInstanceComponent(FireLight);
		FireLight->RegisterComponent();
	}
}

void AFragment2DSourceActor::RebuildMaterialVisualization()
{
	EnsureReactionVisualComponents();
	TArray<uint8> OutputCells;
	TArray<uint8> HotCells;
	FName OutputMaterialId = NAME_None;
	uint16 IgnitionThreshold = 0;
	BuildMaterialProjection(
		OutputCells, HotCells, OutputMaterialId, IgnitionThreshold);
	// Material replacement changes a cell's material, not its occupancy. Render
	// the original and replacement materials as a disjoint partition of that
	// occupancy; drawing both complete masks at identical voxel coordinates
	// causes depth fighting (green/burned flicker) and can make charcoal look
	// like pristine wood indefinitely.
	TArray<uint8> BaseCells = RuntimeMask;
	for (int32 Index = 0; Index < BaseCells.Num(); ++Index)
	{
		if (OutputCells.IsValidIndex(Index) && OutputCells[Index] != 0)
		{
			BaseCells[Index] = 0;
		}
	}
	RebuildSourceMesh(&BaseCells);
	RebuildOutputMesh(OutputCells, OutputMaterialId);
	ApplySourceCollisionState();

	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const float CellSize = GetCellSize();
	TArray<FTransform> FlameTransforms;
	FVector FlameCenter = FVector::ZeroVector;
	for (int32 Index = 0;
		Index < HotCells.Num();
		++Index)
	{
		if (HotCells[Index] != 0)
		{
			const int32 X = Index % Width;
			const int32 Y = Index / Width;
			const FVector Position(
				(static_cast<float>(X) + 0.5f
					- static_cast<float>(Width) * 0.5f) * CellSize,
				0.0f,
				(static_cast<float>(Y) + 0.62f
					- static_cast<float>(Height) * 0.5f) * CellSize);
			FlameCenter += Position;
			FlameTransforms.Emplace(
				FRotator::ZeroRotator,
				Position,
				FVector(
					CellSize * 1.06f / 100.0f,
					CellSize * 1.06f / 100.0f,
					CellSize * 1.18f / 100.0f));
		}
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
}

void AFragment2DSourceActor::RebuildOutputMesh(
	const TArray<uint8>& OutputCells,
	const FName OutputMaterialId)
{
	if (!OutputMeshComponent)
	{
		return;
	}
	OutputMeshComponent->ClearAllMeshSections();
	OutputMeshComponent->ClearCollisionConvexMeshes();
	if (OutputMaterialId.IsNone()
		|| OutputMaterialId == TEXT("empty")
		|| OutputCells.IsEmpty()
		|| !OutputCells.ContainsByPredicate(
			[](const uint8 Value)
			{
				return Value != 0;
			}))
	{
		return;
	}

	MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
	if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		OutputCells,
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
			OutputCells,
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
			OutputCells,
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
		bEnableSourceCollision);
	OutputMeshComponent->CreateMeshSection(
		1,
		Vertices,
		SideTriangles,
		Normals,
		UVs,
		TArray<FColor>(),
		TArray<FProcMeshTangent>(),
		bEnableSourceCollision);
	if (FragmentMaterial)
	{
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		const FMatterFluxMaterialDefinition* Material = Registry.IsValid()
			? Registry->Materials.Find(OutputMaterialId) : nullptr;
		if (!OutputMaterialInstance)
		{
			OutputMaterialInstance = UMaterialInstanceDynamic::Create(
				FragmentMaterial, this);
		}
		OutputMaterialInstance->SetVectorParameterValue(
			TEXT("Color"),
			Material ? Material->Color : FLinearColor(0.08f, 0.07f, 0.06f));
		OutputMeshComponent->SetMaterial(0, OutputMaterialInstance);
		OutputMeshComponent->SetMaterial(1, OutputMaterialInstance);
	}
}

void AFragment2DSourceActor::MarkSharedSmokeVisualizationDirty() const
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			It->MarkSourceMaterialVisualizationDirty();
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

bool AFragment2DSourceActor::BuildMaterialProjection(
	TArray<uint8>& OutOutputCells,
	TArray<uint8>& OutHotCells,
	FName& OutOutputMaterialId,
	uint16& OutIgnitionThreshold) const
{
	OutOutputCells.Init(0, RuntimeMask.Num());
	OutHotCells.Init(0, RuntimeMask.Num());
	OutOutputMaterialId = NAME_None;
	OutIgnitionThreshold = 1000;
	if (!MaterialVolumeTopology.IsSet()
		|| !IMatterFluxScriptRuntime::IsAvailable())
	{
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	FLocalMaterialReactionProgram Program;
	FString Error;
	if (!Registry.IsValid() || !Program.Compile(*Registry, Error))
	{
		return false;
	}
	uint16 SourceMaterialIndex = 0;
	Program.TryGetMaterialIndex(SourceMaterialId, SourceMaterialIndex);
	const FMatterFluxMaterialDefinition* SourceDefinition =
		Registry->Materials.Find(SourceMaterialId);
	if (SourceDefinition)
	{
		if (SourceDefinition->IgnitionThreshold > 0)
		{
			OutIgnitionThreshold = SourceDefinition->IgnitionThreshold;
		}
	}
	for (int32 Index = 0; Index < RuntimeMask.Num(); ++Index)
	{
		if (RuntimeMask[Index] == 0)
		{
			continue;
		}
		const FIntVector Cell(Index % GetMaskWidth(), Index / GetMaskWidth(), 0);
		uint16 MaterialIndex = 0;
		if (!FMaterialVolumeAlgorithms::TryGetCellMaterial(
				MaterialVolumeTopology.GetValue(), Cell, MaterialIndex))
		{
			continue;
		}
		FName MaterialId = NAME_None;
		Program.TryGetMaterialId(MaterialIndex, MaterialId);
		const FMatterFluxMaterialDefinition* CurrentDefinition =
			Registry->Materials.Find(MaterialId);
		if (MaterialIndex != SourceMaterialIndex)
		{
			OutOutputCells[Index] = 1;
			if (OutOutputMaterialId.IsNone())
			{
				OutOutputMaterialId = MaterialId;
			}
		}
		const uint16 Energy = MaterialVolumeFields.GetEnergy(Cell);
		uint16 CellFlameThreshold = CurrentDefinition
			? CurrentDefinition->IgnitionThreshold : 0;
		// A hot, solid combustion residue (wood -> charcoal) is visibly burning
		// only near its authored combustion energy. The remaining lower heat can
		// still propagate without being rendered as a permanent flame. Powder ash
		// is inert and never inherits the original leaf threshold.
		if (CellFlameThreshold == 0
			&& SourceDefinition
			&& SourceDefinition->CombustionProduct == MaterialId
			&& CurrentDefinition
			&& CurrentDefinition->Phase
				== EMatterFluxMaterialPhase::StaticSolid)
		{
			CellFlameThreshold = FMath::Max<uint16>(
				SourceDefinition->IgnitionThreshold,
				static_cast<uint16>(FMath::Max(
					static_cast<int32>(SourceDefinition->CombustionEnergy) - 100,
					0)));
		}
		if (Energy > MaterialVolumeFields.EnvironmentEnergy
			&& CellFlameThreshold > 0
			&& Energy >= CellFlameThreshold)
		{
			OutHotCells[Index] = 1;
		}
	}
	return true;
}

bool AFragment2DSourceActor::HasLocalMaterialRule() const
{
	if (!IMatterFluxScriptRuntime::IsAvailable())
	{
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	FLocalMaterialReactionProgram Program;
	FString Error;
	uint16 SourceIndex = 0;
	if (!Registry.IsValid()
		|| !Program.Compile(*Registry, Error)
		|| !Program.TryGetMaterialIndex(SourceMaterialId, SourceIndex))
	{
		return false;
	}
	return Program.GetContactRules().ContainsByPredicate(
		[SourceIndex](const FLocalMaterialContactRule& Rule)
		{
			return Rule.InputA == SourceIndex || Rule.InputB == SourceIndex;
		});
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
	TArray<uint8> OutputCells;
	TArray<uint8> HotCells;
	FName OutputMaterial;
	uint16 Threshold = 0;
	BuildMaterialProjection(OutputCells, HotCells, OutputMaterial, Threshold);
	return Algo::Count(OutputCells, static_cast<uint8>(1));
}

int32 AFragment2DSourceActor::GetActiveCellCount() const
{
	TArray<uint8> OutputCells;
	TArray<uint8> HotCells;
	FName OutputMaterial;
	uint16 Threshold = 0;
	BuildMaterialProjection(OutputCells, HotCells, OutputMaterial, Threshold);
	return Algo::Count(HotCells, static_cast<uint8>(1));
}

int32 AFragment2DSourceActor::GetMaterialProjectionOverlapCellCount() const
{
	TArray<uint8> OutputCells;
	TArray<uint8> HotCells;
	FName OutputMaterial;
	uint16 Threshold = 0;
	BuildMaterialProjection(
		OutputCells, HotCells, OutputMaterial, Threshold);
	int32 Count = 0;
	for (int32 Index = 0;
		Index < RenderedSourceMask.Num() && Index < OutputCells.Num();
		++Index)
	{
		Count += RenderedSourceMask[Index] != 0
			&& OutputCells[Index] != 0 ? 1 : 0;
	}
	return Count;
}

bool AFragment2DSourceActor::IsMaterialHot() const
{
	return GetActiveCellCount() > 0;
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
	TArray<uint8> OutputCells;
	TArray<uint8> HotCells;
	FName OutputMaterial;
	uint16 Threshold = 0;
	BuildMaterialProjection(OutputCells, HotCells, OutputMaterial, Threshold);
	for (int32 Index = 0;
		Index < HotCells.Num();
		++Index)
	{
		if (HotCells[Index] == 0)
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
	if (!IMatterFluxScriptRuntime::IsAvailable())
	{
		return NAME_None;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	FLocalMaterialReactionProgram Program;
	FString Error;
	uint16 SourceIndex = 0;
	if (!Registry.IsValid()
		|| !Program.Compile(*Registry, Error)
		|| !Program.TryGetMaterialIndex(SourceMaterialId, SourceIndex))
	{
		return NAME_None;
	}
	for (const FLocalMaterialContactRule& Rule : Program.GetContactRules())
	{
		uint16 OtherIndex = 0;
		if (Rule.InputA == SourceIndex)
		{
			OtherIndex = Rule.InputB;
		}
		else if (Rule.InputB == SourceIndex)
		{
			OtherIndex = Rule.InputA;
		}
		FName MaterialId = NAME_None;
		if (OtherIndex != 0
			&& Program.TryGetMaterialId(OtherIndex, MaterialId))
		{
			return MaterialId;
		}
	}
	return NAME_None;
}

void AFragment2DSourceActor::GatherReactionVisualTransforms(
	TArray<FTransform>& OutFlameTransforms,
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>& OutSmokeAnchors,
	const int32 MaxVisualInstances) const
{
	TArray<uint8> OutputCells;
	TArray<uint8> HotCells;
	FName OutputMaterial;
	uint16 IgnitionThreshold = 0;
	if (MaxVisualInstances <= 0
		|| !BuildMaterialProjection(
			OutputCells, HotCells, OutputMaterial, IgnitionThreshold)
		|| !HotCells.Contains(1))
	{
		return;
	}

	const int32 Width = GetMaskWidth();
	const int32 Height = GetMaskHeight();
	const float CellSize = GetCellSize();
	const FTransform ActorTransform = GetActorTransform();
	const float BaseScale = CellSize / 100.0f;
	for (int32 Index = 0;
		Index < HotCells.Num()
			&& OutFlameTransforms.Num() < MaxVisualInstances;
		++Index)
	{
		if (HotCells[Index] == 0)
		{
			continue;
		}
		const int32 X = Index % Width;
		const int32 Y = Index / Width;
		const FVector WorldPosition = ActorTransform.TransformPosition(
			FVector(
				(static_cast<float>(X) + 0.5f
					- static_cast<float>(Width) * 0.5f) * CellSize,
				0.0f,
				(static_cast<float>(Y) + 0.62f
					- static_cast<float>(Height) * 0.5f) * CellSize));
		OutFlameTransforms.Emplace(
			ActorTransform.Rotator(),
			WorldPosition,
			FVector(
				BaseScale * 1.06f,
				BaseScale * 1.06f,
				BaseScale * 1.18f));
	}

	TArray<int32> SurfaceCells;
	MatterFlux::FragmentGeometry::GatherTopExposedMarkedMaskCells(
		RuntimeMask,
		HotCells,
		Width,
		Height,
		SurfaceCells);
	for (const int32 Index : SurfaceCells)
	{
		if (OutSmokeAnchors.Num() >= MaxVisualInstances)
		{
			break;
		}
		const int32 X = Index % Width;
		const int32 Y = Index / Width;
		MatterFlux::Rendering::FMaterialEmissionAnchor& Anchor =
			OutSmokeAnchors.AddDefaulted_GetRef();
		Anchor.WorldPosition = ActorTransform.TransformPosition(
			FVector(
				(static_cast<float>(X) + 0.5f
					- static_cast<float>(Width) * 0.5f)
					* CellSize,
				-CellSize * 0.55f,
				(static_cast<float>(Y) + 1.0f
					- static_cast<float>(Height) * 0.5f)
					* CellSize));
		Anchor.CellSize = CellSize;
		const FIntVector Cell(X, Y, 0);
		const uint16 Energy = MaterialVolumeFields.GetEnergy(Cell);
		Anchor.EmissionProbability = FMath::Clamp(
			static_cast<float>(Energy - IgnitionThreshold)
				/ static_cast<float>(FMath::Max<int32>(
					1, MAX_uint16 - IgnitionThreshold)),
			0.15f, 1.0f);
		Anchor.Seed = GetTypeHash(SourceId)
			^ static_cast<uint32>(Index) * 0x9e3779b9u;
	}
}

void AFragment2DSourceActor::GatherReactionSmokeAnchors(
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>& OutAnchors,
	const int32 MaxAnchors) const
{
	TArray<FTransform> IgnoredFlameTransforms;
	GatherReactionVisualTransforms(
		IgnoredFlameTransforms, OutAnchors, MaxAnchors);
}

void AFragment2DSourceActor::ApplyBrokenState()
{
	const bool bShouldHideSourceMesh =
		bBroken || !bSourceMeshProjectionEnabled;
	// Disabling the base projection means that the world proxy owns the
	// unchanged source geometry; it does not mean that this Actor stopped owning
	// derived reaction visuals. Hiding the whole Actor also hides hot-cell flames
	// and their light, making live local reactions appear to stop. Only a broken
	// source is globally hidden after its material state has moved to a carrier.
	SetActorHiddenInGame(bBroken);
	// Keep the component itself visible because reaction meshes and lights are
	// attached to it. Only the source geometry sections move between the Actor
	// and its world proxy; section visibility does not suppress child rendering
	// or collision. Never propagate this flag into state-derived visuals.
	MeshComponent->SetVisibility(true, false);
	for (int32 SectionIndex = 0;
		SectionIndex < MeshComponent->GetNumSections();
		++SectionIndex)
	{
		MeshComponent->SetMeshSectionVisible(
			SectionIndex, !bShouldHideSourceMesh);
	}
	ApplySourceCollisionState();
}

void AFragment2DSourceActor::ApplySourceCollisionState()
{
	const bool bHasSourceGeometry =
		(MeshComponent && MeshComponent->GetNumSections() > 0)
		|| (OutputMeshComponent
			&& OutputMeshComponent->GetNumSections() > 0);
	const bool bShouldEnable =
		bEnableSourceCollision
		&& !bBroken
		&& !bAggregateSeparationCollisionSuppressed
		&& bHasSourceGeometry;
	SetActorEnableCollision(bShouldEnable);
	if (MeshComponent)
	{
		MeshComponent->SetCollisionEnabled(
			bShouldEnable
				? ECollisionEnabled::QueryAndPhysics
				: ECollisionEnabled::NoCollision);
	}
	if (OutputMeshComponent)
	{
		OutputMeshComponent->SetCollisionEnabled(
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

bool AFragment2DSourceActor::RebuildSourceMesh(
	const TArray<uint8>* RenderMask)
{
	const TArray<uint8>& MeshMask = RenderMask
		&& RenderMask->Num() == RuntimeMask.Num()
		? *RenderMask
		: RuntimeMask;
	RenderedSourceMask = MeshMask;
	MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
	const bool bGeometryValid = MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		MeshMask, GetMaskWidth(), GetMaskHeight(), GetCellSize(), Geometry);

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
			MeshMask, GetMaskWidth(), GetMaskHeight(), GetCellSize(),
			Vertices, Triangles, Normals, UVs, FaceIndexCount)
		: bVoxelBlocks
		? MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromMask(
			MeshMask, GetMaskWidth(), GetMaskHeight(), GetCellSize(),
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
