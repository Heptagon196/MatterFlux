#include "Fragment/Fragment2DActor.h"

#include "Fragment/FragmentGeometry.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"

namespace
{
	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool IsPayloadStateValid(const FFragmentSpawnPayload& Payload)
	{
		return MatterFlux::FragmentGeometry::IsSpawnPayloadWithinReplicationBudget(Payload)
			&& Payload.InitialTransform.IsValid()
			&& IsFiniteVector(Payload.InitialLinearVelocity)
			&& IsFiniteVector(Payload.InitialAngularVelocity)
			&& FMath::IsFinite(Payload.Mass)
			&& Payload.Mass > 0.0f;
	}
}

bool FFragmentAggregateSourceState::IsValid() const
{
	return SourceId.IsValid()
		&& Revision >= 0
		&& SourceMask.HasValidLayout()
		&& (SourceMask.SolidMask.Contains(1)
			|| (bHasCombustionState
				&& ResidueMask.SolidMask.Contains(1)))
		&& LocalTransform.IsValid()
		&& !MaterialId.IsNone()
		&& FMath::IsFinite(Color.R)
		&& FMath::IsFinite(Color.G)
		&& FMath::IsFinite(Color.B)
		&& FMath::IsFinite(Color.A)
		&& ResidueMask.HasValidLayout()
		&& BurningMask.HasValidLayout()
		&& ResidueMask.Width == SourceMask.Width
		&& ResidueMask.Height == SourceMask.Height
		&& BurningMask.Width == SourceMask.Width
		&& BurningMask.Height == SourceMask.Height
		&& (!bHasCombustionState
			|| (!CombustionRuleId.IsNone()
				&& FMath::IsFinite(CombustionAccumulator)
				&& CombustionAccumulator >= 0.0f
				&& TotalSmokeEmissionCount >= 0
				&& FMath::IsFinite(ResidueColor.R)
				&& FMath::IsFinite(ResidueColor.G)
				&& FMath::IsFinite(ResidueColor.B)
				&& FMath::IsFinite(ResidueColor.A)));
}

AFragment2DActor::AFragment2DActor()
{
	bReplicates = true;
	bAlwaysRelevant = false;
	SetNetCullDistanceSquared(FMath::Square(1400.0f));
	InitialLifeSpan = 30.0f;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30.0f);
	SetMinNetUpdateFrequency(5.0f);

	MeshComponent = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FragmentMesh"));
	SetRootComponent(MeshComponent);
	MeshComponent->SetCanEverAffectNavigation(false);

	MeshComponent->SetCollisionObjectType(ECC_PhysicsBody);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComponent->SetCollisionResponseToAllChannels(ECR_Block);
	MeshComponent->SetNotifyRigidBodyCollision(true);
	MeshComponent->bUseComplexAsSimpleCollision = false;
	MeshComponent->SetLinearDamping(1.25f);
	MeshComponent->SetAngularDamping(6.0f);
	MeshComponent->BodyInstance.bUseCCD = true;
}

void AFragment2DActor::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			It->ReleaseDynamicAggregateCarrier(*this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

bool AFragment2DActor::InitializeFromPayload(const FFragmentSpawnPayload& Payload)
{
	SpawnPayload = Payload;
	const bool bReady = RebuildMeshFromPayload();
	if (bReady)
	{
		SetActorTransform(Payload.InitialTransform);
	}

	if (bReady
		&& SpawnPayload.bEnableCollision
		&& HasAuthority()
		&& GetWorld()
		&& GetWorld()->IsGameWorld())
	{
		MeshComponent->SetMassOverrideInKg(NAME_None, FMath::Max(SpawnPayload.Mass, 0.5f), true);
		MeshComponent->SetSimulatePhysics(true);
		MeshComponent->SetPhysicsMaxAngularVelocityInDegrees(
			360.0f,
			false);
		MeshComponent->SetPhysicsLinearVelocity(SpawnPayload.InitialLinearVelocity);
		MeshComponent->SetPhysicsAngularVelocityInDegrees(SpawnPayload.InitialAngularVelocity);
		ForceNetUpdate();
	}
	return bReady;
}

void AFragment2DActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(AFragment2DActor, SpawnPayload, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AFragment2DActor, FragmentMaterial, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AFragment2DActor, FragmentColor, COND_InitialOnly);
	DOREPLIFETIME(AFragment2DActor, AggregateSources);
}

void AFragment2DActor::OnRep_SpawnPayload()
{
	RebuildMeshFromPayload();
}

void AFragment2DActor::OnRep_FragmentMaterial()
{
	RebuildMeshFromPayload();
}

void AFragment2DActor::OnRep_AggregateSources()
{
	RebuildMeshFromPayload();
	NotifyWorldOfAggregateSources();
}

bool AFragment2DActor::ContainsAggregateSource(const FGuid& SourceId) const
{
	return AggregateSources.ContainsByPredicate(
		[&SourceId](const FFragmentAggregateSourceState& Source)
		{
			return Source.SourceId == SourceId;
		});
}

FName AFragment2DActor::GetAggregateSourceMaterialId(
	const FGuid& SourceId) const
{
	const FFragmentAggregateSourceState* Source =
		AggregateSources.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	return Source ? Source->MaterialId : NAME_None;
}

bool AFragment2DActor::GetAggregateSourceWorldTransform(
	const FGuid& SourceId,
	FTransform& OutWorldTransform) const
{
	const FFragmentAggregateSourceState* Source =
		AggregateSources.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	if (!Source)
	{
		return false;
	}
	OutWorldTransform = Source->LocalTransform * GetActorTransform();
	return OutWorldTransform.IsValid();
}

bool AFragment2DActor::ApplyAggregateSourceStreamingState(
	const FGuid& SourceId,
	const FFragment2DSourceStreamingState& State,
	const FName InResidueMaterialId,
	const FLinearColor& InResidueColor)
{
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| !SourceId.IsValid())
	{
		return false;
	}
	FFragmentAggregateSourceState* Source =
		AggregateSources.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	const int32 CellCount = Source
		? Source->SourceMask.Width * Source->SourceMask.Height
		: 0;
	if (!Source
		|| State.GetRuntimeMask().Num() != CellCount
		|| !State.bHasCombustionState
		|| State.CombustionState.ResidueMask.Num() != CellCount
		|| State.CombustionState.BurningMask.Num() != CellCount)
	{
		return false;
	}

	const FFragmentAggregateSourceState Previous = *Source;
	Source->Revision = State.Revision;
	Source->SourceMask.SolidMask = State.GetRuntimeMask();
	Source->ResidueMask = Source->SourceMask;
	Source->ResidueMask.SolidMask = State.CombustionState.ResidueMask;
	Source->BurningMask = Source->SourceMask;
	Source->BurningMask.SolidMask = State.CombustionState.BurningMask;
	for (uint8& Value : Source->BurningMask.SolidMask)
	{
		Value = Value != 0 ? 1 : 0;
	}
	Source->bHasCombustionState = true;
	Source->CombustionRuleId = State.CombustionState.RuleId;
	Source->ResidueMaterialId = InResidueMaterialId;
	Source->ResidueColor = InResidueColor;
	Source->CombustionSeed = State.CombustionState.Seed;
	Source->CombustionTick = State.CombustionState.Tick;
	Source->CombustionAccumulator = State.CombustionAccumulator;
	Source->TotalSmokeEmissionCount = State.TotalSmokeEmissionCount;
	const bool bUpdated = Source->IsValid()
		&& (Source->bEnableCollision
			? RebuildMeshFromPayload()
			: RebuildAggregateVisualSectionsOnly());
	if (!bUpdated)
	{
		*Source = Previous;
		RebuildMeshFromPayload();
		return false;
	}
	ForceNetUpdate();
	return true;
}

bool AFragment2DActor::RebuildAggregateVisualSectionsOnly()
{
	if (!MeshComponent)
	{
		return false;
	}
	const int32 PreviousSectionCount = MeshComponent->GetNumSections();
	for (int32 SectionIndex = 2;
		SectionIndex < PreviousSectionCount;
		++SectionIndex)
	{
		MeshComponent->ClearMeshSection(SectionIndex);
	}
	return RebuildAggregateSourceSections();
}

bool AFragment2DActor::AbsorbAggregateSource(
	AFragment2DSourceActor& SourceActor)
{
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| SourceActor.GetWorld() != GetWorld()
		|| SourceActor.IsActorBeingDestroyed()
		|| SourceActor.bBroken
		|| SourceActor.bAggregateRoot
		|| !SourceActor.SourceId.IsValid()
		|| ContainsAggregateSource(SourceActor.SourceId)
		|| AggregateSources.Num() >= 16)
	{
		UE_LOG(
			LogMatterFlux,
			Warning,
			TEXT("Aggregate source %s rejected by carrier preconditions (broken=%d root=%d id=%d duplicate=%d count=%d)"),
			*SourceActor.SourceId.ToString(),
			SourceActor.bBroken ? 1 : 0,
			SourceActor.bAggregateRoot ? 1 : 0,
			SourceActor.SourceId.IsValid() ? 1 : 0,
			ContainsAggregateSource(SourceActor.SourceId) ? 1 : 0,
			AggregateSources.Num());
		return false;
	}

	FFragment2DSourceStreamingState StreamingState;
	FString StreamingError;
	if (!SourceActor.CaptureStreamingState(
		StreamingState,
		StreamingError))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Aggregate carrier could not capture source %s: %s"),
			*SourceActor.SourceId.ToString(),
			*StreamingError);
		return false;
	}

	FFragmentAggregateSourceState Candidate;
	Candidate.SourceId = SourceActor.SourceId;
	Candidate.Revision = SourceActor.Revision;
	Candidate.SourceMask = SourceActor.ProceduralSource;
	Candidate.SourceMask.SolidMask = StreamingState.GetRuntimeMask();
	Candidate.LocalTransform = SourceActor.GetActorTransform()
		.GetRelativeTransform(GetActorTransform());
	Candidate.Material = SourceActor.FragmentMaterial;
	Candidate.MaterialId = SourceActor.SourceMaterialId;
	Candidate.Color = SourceActor.FragmentColor;
	Candidate.bEnableCollision = SourceActor.bEnableSourceCollision;
	Candidate.ResidueMask = Candidate.SourceMask;
	Candidate.ResidueMask.SolidMask.Init(
		0,
		Candidate.SourceMask.SolidMask.Num());
	Candidate.BurningMask = Candidate.SourceMask;
	Candidate.BurningMask.SolidMask.Init(
		0,
		Candidate.SourceMask.SolidMask.Num());
	if (StreamingState.bHasCombustionState)
	{
		Candidate.ResidueMask.SolidMask =
			StreamingState.CombustionState.ResidueMask;
		Candidate.BurningMask.SolidMask =
			StreamingState.CombustionState.BurningMask;
		for (uint8& Value : Candidate.BurningMask.SolidMask)
		{
			Value = Value != 0 ? 1 : 0;
		}
		Candidate.bHasCombustionState = true;
		Candidate.CombustionRuleId =
			StreamingState.CombustionState.RuleId;
		Candidate.CombustionSeed =
			StreamingState.CombustionState.Seed;
		Candidate.CombustionTick =
			StreamingState.CombustionState.Tick;
		Candidate.CombustionAccumulator =
			StreamingState.CombustionAccumulator;
		Candidate.TotalSmokeEmissionCount =
			StreamingState.TotalSmokeEmissionCount;
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (Registry.IsValid())
		{
			if (const FMatterFluxCombustionDefinition* Rule =
				Registry->Combustions.Find(Candidate.CombustionRuleId))
			{
				Candidate.ResidueMaterialId = Rule->ResidueMaterial;
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(Rule->ResidueMaterial))
				{
					Candidate.ResidueColor = Material->Color;
				}
			}
		}
	}
	if (!Candidate.IsValid())
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Aggregate source state %s is invalid before handoff"),
			*Candidate.SourceId.ToString());
		return false;
	}

	int64 TotalCells = Candidate.SourceMask.SolidMask.Num();
	for (const FFragmentAggregateSourceState& Existing : AggregateSources)
	{
		TotalCells += Existing.SourceMask.SolidMask.Num();
	}
	if (TotalCells > 65536)
	{
		return false;
	}

	const bool bWasSimulating = MeshComponent->IsSimulatingPhysics();
	const FTransform PreservedBodyTransform = MeshComponent->GetComponentTransform();
	const FVector PreservedLinearVelocity = bWasSimulating
		? MeshComponent->GetPhysicsLinearVelocity()
		: SpawnPayload.InitialLinearVelocity;
	const FVector PreservedAngularVelocity = bWasSimulating
		? MeshComponent->GetPhysicsAngularVelocityInDegrees()
		: SpawnPayload.InitialAngularVelocity;
	const float PreviousMass = FMath::Max(SpawnPayload.Mass, 0.5f);

	AggregateSources.Add(Candidate);
	if (!RebuildMeshFromPayload())
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Aggregate carrier mesh rejected source %s"),
			*Candidate.SourceId.ToString());
		AggregateSources.Pop(EAllowShrinking::No);
		RebuildMeshFromPayload();
		return false;
	}

	if (AMatterFluxPlayableWorldActor* WorldOwner =
		Cast<AMatterFluxPlayableWorldActor>(SourceActor.GetOwner()))
	{
		if (!WorldOwner->RetireFragmentSourceIntoDynamicAggregate(
			SourceActor,
			*this))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Playable world rejected aggregate source handoff %s"),
				*Candidate.SourceId.ToString());
			AggregateSources.Pop(EAllowShrinking::No);
			RebuildMeshFromPayload();
			return false;
		}
	}

	int32 AddedSolidCells = 0;
	for (const uint8 Cell : Candidate.SourceMask.SolidMask)
	{
		AddedSolidCells += Cell != 0 ? 1 : 0;
	}
	SpawnPayload.Mass = FMath::Clamp(
		PreviousMass + static_cast<float>(AddedSolidCells) * 0.05f,
		0.5f,
		800.0f);
	const FVector PreservedMomentumVelocity =
		PreservedLinearVelocity * PreviousMass / SpawnPayload.Mass;
	SpawnPayload.InitialLinearVelocity = PreservedMomentumVelocity;
	SpawnPayload.InitialAngularVelocity = PreservedAngularVelocity;
	if (HasAuthority()
		&& GetWorld()
		&& GetWorld()->IsGameWorld()
		&& SpawnPayload.bEnableCollision)
	{
		MeshComponent->SetMassOverrideInKg(
			NAME_None,
			SpawnPayload.Mass,
			true);
		MeshComponent->SetWorldTransform(
			PreservedBodyTransform,
			false,
			nullptr,
			ETeleportType::TeleportPhysics);
		if (bWasSimulating && !MeshComponent->IsSimulatingPhysics())
		{
			MeshComponent->SetSimulatePhysics(true);
		}
		MeshComponent->SetPhysicsLinearVelocity(
			PreservedMomentumVelocity);
		MeshComponent->SetPhysicsAngularVelocityInDegrees(
			PreservedAngularVelocity);
		MeshComponent->SetPhysicsMaxAngularVelocityInDegrees(
			360.0f,
			false);
	}

	SourceActor.Destroy();
	ForceNetUpdate();
	NotifyWorldOfAggregateSources();
	return true;
}

void AFragment2DActor::ApplyFragmentMaterial()
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
		[](UMaterialInstanceDynamic* Material)
		{
			if (!Material)
			{
				return;
			}
			Material->SetScalarParameterValue(TEXT("FaceContrast"), 0.88f);
			Material->SetScalarParameterValue(TEXT("ColorVariation"), 0.035f);
			Material->SetScalarParameterValue(TEXT("PixelSize"), 12.0f);
			Material->SetScalarParameterValue(TEXT("Roughness"), 0.86f);
			Material->SetScalarParameterValue(TEXT("ShadowLift"), 0.18f);
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
	const FLinearColor SideColor(
		FragmentColor.R * 0.72f,
		FragmentColor.G * 0.72f,
		FragmentColor.B * 0.72f,
		FragmentColor.A);
	DynamicFragmentSideMaterial->SetVectorParameterValue(
		TEXT("Color"),
		SideColor);
	ConfigureVoxelMaterial(DynamicFragmentSideMaterial);
	MeshComponent->SetMaterial(1, DynamicFragmentSideMaterial);
}

bool AFragment2DActor::RebuildMeshFromPayload()
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;

	const bool bMeshValid = IsPayloadStateValid(SpawnPayload)
		&& MatterFlux::FragmentGeometry::BuildExtrudedMesh(
			SpawnPayload.Vertices2D,
			SpawnPayload.TriangleIndices,
			SpawnPayload.OuterContours,
			SpawnPayload.HoleContours,
			SpawnPayload.Thickness,
			Vertices,
			Triangles,
			Normals,
			UVs);

	// Authority needs a body immediately so mass and launch velocity are
	// committed in the same transaction. Simulated client copies receive their
	// motion from replication, so making their local query collision cook async
	// removes a large game-thread hitch when several fragment payloads arrive in
	// one network frame without changing authoritative physics.
	MeshComponent->bUseAsyncCooking = GetWorld()
		&& GetWorld()->IsGameWorld()
		&& !HasAuthority();

	MeshComponent->ClearAllMeshSections();
	if (!bMeshValid)
	{
		MeshComponent->ClearCollisionConvexMeshes();
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComponent->SetSimulatePhysics(false);
		return false;
	}

	const int32 FaceIndexCount = SpawnPayload.TriangleIndices.Num() * 2;
	if (FaceIndexCount <= 0 || FaceIndexCount >= Triangles.Num())
	{
		MeshComponent->ClearCollisionConvexMeshes();
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
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
		false);
	MeshComponent->CreateMeshSection(
		1,
		Vertices,
		SideTriangles,
		Normals,
		UVs,
		TArray<FColor>(),
		TArray<FProcMeshTangent>(),
		false);
	ApplyFragmentMaterial();
	if (!RebuildAggregateSourceSections())
	{
		MeshComponent->ClearAllMeshSections();
		MeshComponent->ClearCollisionConvexMeshes();
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return false;
	}
	const bool bNeedsCollision = SpawnPayload.bEnableCollision
		|| AggregateSources.ContainsByPredicate(
			[](const FFragmentAggregateSourceState& Source)
			{
				return Source.bEnableCollision;
			});
	if (!bNeedsCollision)
	{
		MeshComponent->ClearCollisionConvexMeshes();
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComponent->SetSimulatePhysics(false);
		return true;
	}
	return RebuildSimpleCollision();
}

bool AFragment2DActor::RebuildAggregateSourceSections()
{
	struct FRenderGroup
	{
		TObjectPtr<UMaterialInterface> Material;
		FName MaterialId = NAME_None;
		FLinearColor Color = FLinearColor::White;
		bool bSide = false;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
	};

	TMap<FString, FRenderGroup> Groups;
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.IsValid())
		{
			return false;
		}
		struct FRenderLayer
		{
			const TArray<uint8>* Mask = nullptr;
			FName MaterialId = NAME_None;
			FLinearColor Color = FLinearColor::White;
		};
		TArray<FRenderLayer, TInlineAllocator<2>> RenderLayers;
		if (Source.SourceMask.SolidMask.Contains(1))
		{
			RenderLayers.Add({
				&Source.SourceMask.SolidMask,
				Source.MaterialId,
				Source.Color});
		}
		if (Source.bHasCombustionState
			&& Source.ResidueMask.SolidMask.Contains(1))
		{
			RenderLayers.Add({
				&Source.ResidueMask.SolidMask,
				Source.ResidueMaterialId.IsNone()
					? FName(TEXT("residue"))
					: Source.ResidueMaterialId,
				Source.ResidueColor});
		}

		for (const FRenderLayer& Layer : RenderLayers)
		{
			MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
			if (!Layer.Mask
				|| !MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
					*Layer.Mask,
					Source.SourceMask.Width,
					Source.SourceMask.Height,
					Source.SourceMask.CellSize,
					Geometry))
			{
				return false;
			}
			TArray<FVector> SourceVertices;
			TArray<int32> SourceTriangles;
			TArray<FVector> SourceNormals;
			TArray<FVector2D> SourceUVs;
			if (!MatterFlux::FragmentGeometry::BuildExtrudedMesh(
				Geometry.Vertices2D,
				Geometry.TriangleIndices,
				Geometry.OuterContours,
				Geometry.HoleContours,
				Source.SourceMask.CellSize,
				SourceVertices,
				SourceTriangles,
				SourceNormals,
				SourceUVs))
			{
				return false;
			}
			const int32 FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
			if (FaceIndexCount <= 0 || FaceIndexCount >= SourceTriangles.Num())
			{
				return false;
			}

			for (const bool bSide : {false, true})
			{
				const FString Key = FString::Printf(
					TEXT("%s|%s|%08x|%08x|%08x|%08x|%d"),
					*GetPathNameSafe(Source.Material.Get()),
					*Layer.MaterialId.ToString(),
					FPlatformMath::AsUInt(Layer.Color.R),
					FPlatformMath::AsUInt(Layer.Color.G),
					FPlatformMath::AsUInt(Layer.Color.B),
					FPlatformMath::AsUInt(Layer.Color.A),
					bSide ? 1 : 0);
				FRenderGroup& Group = Groups.FindOrAdd(Key);
				Group.Material = Source.Material;
				Group.MaterialId = Layer.MaterialId;
				Group.Color = Layer.Color;
				Group.bSide = bSide;
				const int32 VertexOffset = Group.Vertices.Num();
				Group.Vertices.Reserve(VertexOffset + SourceVertices.Num());
				Group.Normals.Reserve(VertexOffset + SourceNormals.Num());
				Group.UVs.Append(SourceUVs);
				for (int32 Index = 0; Index < SourceVertices.Num(); ++Index)
				{
					Group.Vertices.Add(Source.LocalTransform.TransformPosition(
						SourceVertices[Index]));
					Group.Normals.Add(Source.LocalTransform.TransformVectorNoScale(
						SourceNormals[Index]).GetSafeNormal());
				}
				const int32 FirstTriangle = bSide ? FaceIndexCount : 0;
				const int32 EndTriangle = bSide
					? SourceTriangles.Num()
					: FaceIndexCount;
				Group.Triangles.Reserve(
					Group.Triangles.Num() + EndTriangle - FirstTriangle);
				for (int32 Index = FirstTriangle; Index < EndTriangle; ++Index)
				{
					Group.Triangles.Add(
						SourceTriangles[Index] + VertexOffset);
				}
			}
		}
	}

	TArray<FString> Keys;
	Groups.GenerateKeyArray(Keys);
	Keys.Sort();
	AggregateDynamicMaterials.Reset();
	int32 SectionIndex = 2;
	for (const FString& Key : Keys)
	{
		FRenderGroup& Group = Groups.FindChecked(Key);
		MeshComponent->CreateMeshSection(
			SectionIndex,
			Group.Vertices,
			Group.Triangles,
			Group.Normals,
			Group.UVs,
			TArray<FColor>(),
			TArray<FProcMeshTangent>(),
			false);
		UMaterialInterface* Parent = Group.Material
			? Group.Material.Get()
			: FragmentMaterial.Get();
		if (Parent)
		{
			UMaterialInstanceDynamic* Dynamic =
				UMaterialInstanceDynamic::Create(Parent, this);
			const float Brightness = Group.bSide ? 0.72f : 1.0f;
			Dynamic->SetVectorParameterValue(
				TEXT("Color"),
				FLinearColor(
					Group.Color.R * Brightness,
					Group.Color.G * Brightness,
					Group.Color.B * Brightness,
					Group.Color.A));
			Dynamic->SetScalarParameterValue(TEXT("FaceContrast"), 0.56f);
			Dynamic->SetScalarParameterValue(TEXT("ColorVariation"), 0.035f);
			Dynamic->SetScalarParameterValue(TEXT("PixelSize"), 12.0f);
			Dynamic->SetScalarParameterValue(TEXT("Roughness"), 0.86f);
			Dynamic->SetScalarParameterValue(TEXT("ShadowLift"), 0.18f);
			AggregateDynamicMaterials.Add(Dynamic);
			MeshComponent->SetMaterial(SectionIndex, Dynamic);
		}
		++SectionIndex;
	}
	return true;
}

void AFragment2DActor::NotifyWorldOfAggregateSources()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		for (const FFragmentAggregateSourceState& Source : AggregateSources)
		{
			It->NotifyDynamicAggregateOwnsSource(Source.SourceId, this);
		}
	}
}

bool AFragment2DActor::RebuildSimpleCollision()
{
	MeshComponent->ClearCollisionConvexMeshes();
	if (!FMath::IsFinite(SpawnPayload.Thickness) || SpawnPayload.Thickness <= 0.0f)
	{
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return false;
	}
	bool bAddedCollision = false;
	const auto AddContour =
		[this, &bAddedCollision](
			const FFragmentContour& Contour,
			const float Thickness,
			const FTransform& LocalTransform)
	{
		if (Contour.Vertices.Num() < 3
			|| !FMath::IsFinite(Thickness)
			|| Thickness <= 0.0f
			|| !LocalTransform.IsValid())
		{
			return;
		}
		const float HalfY = FMath::Max(Thickness, 1.0f) * 0.5f;
		TArray<FVector> ConvexVerts;
		bool bContourValid = true;
		double TwiceArea = 0.0;
		for (const FVector2D& Point : Contour.Vertices)
		{
			bContourValid &= FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y);
			ConvexVerts.Add(LocalTransform.TransformPosition(
				FVector(Point.X, -HalfY, Point.Y)));
		}
		for (int32 Index = 0; Index < Contour.Vertices.Num(); ++Index)
		{
			const FVector2D& A = Contour.Vertices[Index];
			const FVector2D& B = Contour.Vertices[(Index + 1) % Contour.Vertices.Num()];
			TwiceArea += A.X * B.Y - B.X * A.Y;
		}
		for (const FVector2D& Point : Contour.Vertices)
		{
			ConvexVerts.Add(LocalTransform.TransformPosition(
				FVector(Point.X, HalfY, Point.Y)));
		}
		if (!bContourValid
			|| !FMath::IsFinite(TwiceArea)
			|| FMath::Abs(TwiceArea) <= UE_SMALL_NUMBER)
		{
			return;
		}
		MeshComponent->AddCollisionConvexMesh(ConvexVerts);
		bAddedCollision = true;
	};

	if (SpawnPayload.bEnableCollision)
	{
		for (const FFragmentContour& Contour : SpawnPayload.CollisionContours)
		{
			AddContour(Contour, SpawnPayload.Thickness, FTransform::Identity);
		}
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.bEnableCollision)
		{
			continue;
		}
		TArray<uint8> CollisionMask = Source.SourceMask.SolidMask;
		if (Source.bHasCombustionState
			&& Source.ResidueMask.SolidMask.Num() == CollisionMask.Num())
		{
			for (int32 Index = 0; Index < CollisionMask.Num(); ++Index)
			{
				CollisionMask[Index] = CollisionMask[Index] != 0
					|| Source.ResidueMask.SolidMask[Index] != 0
					? 1
					: 0;
			}
		}
		MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
		if (!Source.IsValid()
			|| !MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
				CollisionMask,
				Source.SourceMask.Width,
				Source.SourceMask.Height,
				Source.SourceMask.CellSize,
				Geometry))
		{
			MeshComponent->ClearCollisionConvexMeshes();
			MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			return false;
		}
		for (const FFragmentContour& Contour : Geometry.CollisionContours)
		{
			AddContour(
				Contour,
				Source.SourceMask.CellSize,
				Source.LocalTransform);
		}
	}
	MeshComponent->SetCollisionEnabled(bAddedCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	return bAddedCollision;
}
