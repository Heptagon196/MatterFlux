#include "Fragment/Fragment2DActor.h"

#include "Fragment/FragmentGeometry.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "Material/MatterFluxSourceCombustionRuntime.h"
#include "Material/MatterFluxBuoyancyComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "Rendering/MatterFluxWholeObjectGeometry.h"

namespace
{
	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool IsPayloadStateValid(const FFragmentSpawnPayload& Payload)
	{
		const bool bHasDetachedVoxelMask =
			Payload.DetachedVoxelMask.Width != 0
			|| Payload.DetachedVoxelMask.Height != 0
			|| !Payload.DetachedVoxelMask.SolidMask.IsEmpty();
		return MatterFlux::FragmentGeometry::IsSpawnPayloadWithinReplicationBudget(Payload)
			&& Payload.InitialTransform.IsValid()
			&& IsFiniteVector(Payload.InitialLinearVelocity)
			&& IsFiniteVector(Payload.InitialAngularVelocity)
			&& FMath::IsFinite(Payload.Mass)
			&& Payload.Mass > 0.0f
			&& FMath::IsFinite(Payload.FadeOutDuration)
			&& Payload.FadeOutDuration >= 0.0f
			&& (!bHasDetachedVoxelMask
				|| (!Payload.MaterialId.IsNone()
					&& Payload.DetachedVoxelMask.IsValid()
					&& Payload.DetachedVoxelMask.GeometryStyle
						== EFragmentSourceGeometryStyle::VoxelBlocks));
	}

	float ComputeVisualDepthOffset(const FGuid& FragmentId)
	{
		const uint32 Hash = HashCombineFast(
			HashCombineFast(FragmentId.A, FragmentId.B),
			HashCombineFast(FragmentId.C, FragmentId.D));
		const uint32 LaneIndex = Hash % 16u;
		const int32 SignedLane = LaneIndex < 8u
			? static_cast<int32>(LaneIndex) - 8
			: static_cast<int32>(LaneIndex) - 7;
		return static_cast<float>(SignedLane) * 0.2f;
	}

	FBox BuildMaskWorldBounds(
		const FFragmentSourceMask& Mask,
		const FTransform& WorldTransform)
	{
		if (!Mask.IsValid() || !WorldTransform.IsValid())
		{
			return FBox(ForceInit);
		}
		const FVector HalfExtent(
			Mask.Width * Mask.CellSize * 0.5f,
			Mask.CellSize * 0.5f,
			Mask.Height * Mask.CellSize * 0.5f);
		return FBox(-HalfExtent, HalfExtent).TransformBy(
			WorldTransform.ToMatrixWithScale());
	}

	const FMatterFluxReactionDefinition* FindCombustionRule(
		const FMatterFluxContentRegistry& Registry,
		const FName FuelMaterial,
		const FName FlameMaterial)
	{
		return MatterFlux::Reaction::FMaterialReactionEngine::
			FindPropagatingRule(
				Registry, FuelMaterial, FlameMaterial);
	}

	int32 CountSetCells(const TArray<uint8>& Mask)
	{
		int32 Count = 0;
		for (const uint8 Value : Mask)
		{
			Count += Value != 0 ? 1 : 0;
		}
		return Count;
	}

	double Cross2D(
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& C)
	{
		return FVector2D::CrossProduct(B - A, C - A);
	}

	bool IsPointInTriangle(
		const FVector2D& Point,
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& C)
	{
		const double AB = Cross2D(A, B, Point);
		const double BC = Cross2D(B, C, Point);
		const double CA = Cross2D(C, A, Point);
		constexpr double Epsilon = 0.001;
		return (AB >= -Epsilon && BC >= -Epsilon && CA >= -Epsilon)
			|| (AB <= Epsilon && BC <= Epsilon && CA <= Epsilon);
	}

	bool DoSegmentsIntersect(
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& C,
		const FVector2D& D)
	{
		const double AB_C = Cross2D(A, B, C);
		const double AB_D = Cross2D(A, B, D);
		const double CD_A = Cross2D(C, D, A);
		const double CD_B = Cross2D(C, D, B);
		constexpr double Epsilon = 0.001;
		const bool bProperCrossing =
			((AB_C > Epsilon && AB_D < -Epsilon)
				|| (AB_C < -Epsilon && AB_D > Epsilon))
			&& ((CD_A > Epsilon && CD_B < -Epsilon)
				|| (CD_A < -Epsilon && CD_B > Epsilon));
		if (bProperCrossing)
		{
			return true;
		}
		const auto IsOnSegment = [](const FVector2D& Point,
			const FVector2D& Start, const FVector2D& End)
		{
			constexpr double BoundsEpsilon = 0.001;
			return FMath::Abs(Cross2D(Start, End, Point))
					<= BoundsEpsilon
				&& Point.X >= FMath::Min(Start.X, End.X) - BoundsEpsilon
				&& Point.X <= FMath::Max(Start.X, End.X) + BoundsEpsilon
				&& Point.Y >= FMath::Min(Start.Y, End.Y) - BoundsEpsilon
				&& Point.Y <= FMath::Max(Start.Y, End.Y) + BoundsEpsilon;
		};
		return IsOnSegment(C, A, B)
			|| IsOnSegment(D, A, B)
			|| IsOnSegment(A, C, D)
			|| IsOnSegment(B, C, D);
	}

	double PointSegmentDistanceSquared(
		const FVector2D& Point,
		const FVector2D& A,
		const FVector2D& B)
	{
		const FVector2D Segment = B - A;
		const double SegmentLengthSquared = Segment.SizeSquared();
		if (SegmentLengthSquared <= UE_DOUBLE_SMALL_NUMBER)
		{
			return FVector2D::DistSquared(Point, A);
		}
		const double Along = FMath::Clamp(
			FVector2D::DotProduct(Point - A, Segment)
				/ SegmentLengthSquared,
			0.0,
			1.0);
		return FVector2D::DistSquared(Point, A + Segment * Along);
	}

	bool DoesTriangleIntersectDamageShape(
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& C,
		const FFragmentDamageShape& Shape)
	{
		if (Shape.Type == EFragmentDamageShapeType::Circle)
		{
			const double RadiusSquared = FMath::Square(
				static_cast<double>(Shape.Radius));
			const FVector2D Center = FVector2D::ZeroVector;
			return A.SizeSquared() <= RadiusSquared
				|| B.SizeSquared() <= RadiusSquared
				|| C.SizeSquared() <= RadiusSquared
				|| IsPointInTriangle(Center, A, B, C)
				|| PointSegmentDistanceSquared(Center, A, B)
					<= RadiusSquared
				|| PointSegmentDistanceSquared(Center, B, C)
					<= RadiusSquared
				|| PointSegmentDistanceSquared(Center, C, A)
					<= RadiusSquared;
		}

		const FVector2D HalfExtent = Shape.Type
			== EFragmentDamageShapeType::Line
			? FVector2D(Shape.Extents.X * 0.5, Shape.Thickness * 0.5)
			: Shape.Extents;
		const auto IsInsideRect = [&HalfExtent](const FVector2D& Point)
		{
			return FMath::Abs(Point.X) <= HalfExtent.X
				&& FMath::Abs(Point.Y) <= HalfExtent.Y;
		};
		if (IsInsideRect(A) || IsInsideRect(B) || IsInsideRect(C))
		{
			return true;
		}
		const FVector2D Corners[] =
		{
			FVector2D(-HalfExtent.X, -HalfExtent.Y),
			FVector2D(HalfExtent.X, -HalfExtent.Y),
			FVector2D(HalfExtent.X, HalfExtent.Y),
			FVector2D(-HalfExtent.X, HalfExtent.Y)
		};
		for (const FVector2D& Corner : Corners)
		{
			if (IsPointInTriangle(Corner, A, B, C))
			{
				return true;
			}
		}
		const FVector2D Triangle[] = {A, B, C};
		for (int32 TriangleEdge = 0; TriangleEdge < 3; ++TriangleEdge)
		{
			for (int32 RectangleEdge = 0; RectangleEdge < 4; ++RectangleEdge)
			{
				if (DoSegmentsIntersect(
					Triangle[TriangleEdge],
					Triangle[(TriangleEdge + 1) % 3],
					Corners[RectangleEdge],
					Corners[(RectangleEdge + 1) % 4]))
				{
					return true;
				}
			}
		}
		return false;
	}
}

bool FFragmentAggregateSourceState::IsValid() const
{
	return SourceId.IsValid()
		&& (DefinitionSourceId.IsValid() || bOwnsLogicalSource)
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
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
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
	MeshComponent->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
	// Detached material is a movable body, not a terrain step. Allowing
	// CharacterMovement to StepUp makes tall creatures climb over a voxel hull
	// after one contact frame instead of continuously pushing it sideways.
	MeshComponent->CanCharacterStepUpOn = ECB_No;
	MeshComponent->SetNotifyRigidBodyCollision(true);
	MeshComponent->bUseComplexAsSimpleCollision = false;
	MeshComponent->SetLinearDamping(1.25f);
	MeshComponent->SetAngularDamping(4.0f);
	MeshComponent->BodyInstance.bUseCCD = true;
	MeshComponent->BodyInstance.SleepFamily = ESleepFamily::Custom;
	MeshComponent->BodyInstance.CustomSleepThresholdMultiplier = 2.5f;
	MeshComponent->BodyInstance.StabilizationThresholdMultiplier = 2.0f;
	// Gameplay takes place on the horizontal XY ground plane. Locking detached
	// material to XZ was a legacy 2D assumption: it made a fragment immovable
	// whenever a player approached along world Y and disagreed with the same
	// canonical object's visible 3D orientation after a rotated cut.
	MeshComponent->SetConstraintMode(EDOFMode::None);
	FragmentPhysicalMaterial = CreateDefaultSubobject<UPhysicalMaterial>(
		TEXT("FragmentPhysicalMaterial"));
	FragmentPhysicalMaterial->Friction = 0.45f;
	FragmentPhysicalMaterial->StaticFriction = 0.45f;
	FragmentPhysicalMaterial->bOverrideFrictionCombineMode = true;
	FragmentPhysicalMaterial->FrictionCombineMode = EFrictionCombineMode::Min;
	FragmentPhysicalMaterial->Restitution = 0.0f;
	BuoyancyComponent = CreateDefaultSubobject<UMatterFluxBuoyancyComponent>(
		TEXT("BuoyancyComponent"));
	BuoyancyComponent->SetTargetPrimitive(MeshComponent);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface>
		FadeMaterialFinder(
			TEXT("/Game/MatterFlux/Materials/M_VoxelGas.M_VoxelGas"));
	TransientFadeMaterial = FadeMaterialFinder.Object;
}

AFragment2DActor::~AFragment2DActor() = default;

void AFragment2DActor::BeginPlay()
{
	Super::BeginPlay();
	if (FragmentPhysicalMaterial)
	{
		MeshComponent->SetPhysMaterialOverride(FragmentPhysicalMaterial);
	}
	ConfigureTransientFade();
}

void AFragment2DActor::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	AdvanceRootCombustion(DeltaSeconds);
	AdvanceDetachedAggregateCombustion(DeltaSeconds);
	if (SpawnPayload.FadeOutDuration <= 0.0f)
	{
		SetActorTickEnabled(
			IsRootCombusting()
			|| !DetachedAggregateCombustionRuntimes.IsEmpty());
		return;
	}

	TransientFadeElapsed = FMath::Clamp(
		TransientFadeElapsed + FMath::Max(DeltaSeconds, 0.0f),
		0.0f,
		SpawnPayload.FadeOutDuration);
	const float LinearAlpha = 1.0f
		- TransientFadeElapsed / SpawnPayload.FadeOutDuration;
	TransientFadeAlpha = FMath::SmoothStep(0.0f, 1.0f, LinearAlpha);
	ApplyTransientFadeAlpha();
	if (TransientFadeElapsed >= SpawnPayload.FadeOutDuration)
	{
		SetActorTickEnabled(false);
		Destroy();
	}
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
	AcceptedCutCount = 0;
	ActiveCutFadeDuration = 0.0f;
	InitializeRootCombustionState();
	RefreshBuoyancyDensity();
	const bool bReady = RebuildMeshFromPayload();
	if (bReady)
	{
		SetActorTransform(Payload.InitialTransform);
		ConfigureTransientFade();
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
	DOREPLIFETIME(AFragment2DActor, RootCombustionState);
	DOREPLIFETIME(AFragment2DActor, AcceptedCutCount);
	DOREPLIFETIME_CONDITION(
		AFragment2DActor,
		CutsBeforeFade,
		COND_InitialOnly);
	DOREPLIFETIME_CONDITION(
		AFragment2DActor,
		CutExhaustionFadeDuration,
		COND_InitialOnly);
	DOREPLIFETIME(AFragment2DActor, ActiveCutFadeDuration);
}

void AFragment2DActor::OnRep_SpawnPayload()
{
	InitializeRootCombustionState();
	RefreshBuoyancyDensity();
	RebuildMeshFromPayload();
	SynchronizeCutFadeState();
	if (!IsCutFadeActive())
	{
		ConfigureTransientFade();
	}
}

void AFragment2DActor::OnRep_FragmentMaterial()
{
	if (RootCombustionState.SourceId == SpawnPayload.FragmentId)
	{
		RootCombustionState.Material = FragmentMaterial;
		RootCombustionState.Color = FragmentColor;
	}
	RebuildMeshFromPayload();
}

void AFragment2DActor::OnRep_AggregateSources()
{
	RefreshBuoyancyDensity();
	RebuildMeshFromPayload();
	NotifyWorldOfAggregateSources();
	MarkCombustionVisualizationDirty();
}

void AFragment2DActor::OnRep_RootCombustionState()
{
	RebuildMeshFromPayload();
	SetActorTickEnabled(
		SpawnPayload.FadeOutDuration > 0.0f || IsRootCombusting());
	MarkCombustionVisualizationDirty();
}

void AFragment2DActor::OnRep_CutState()
{
	SynchronizeCutFadeState();
}

bool AFragment2DActor::DoesCutShapeIntersect(
	const FFragmentDamageShape& CutShape) const
{
	const auto IntersectsMask = [&CutShape](
		const FFragmentSourceMask& Mask,
		const FTransform& MaskWorldTransform)
	{
		if (!Mask.IsValid() || !MaskWorldTransform.IsValid())
		{
			return false;
		}
		FFragmentDamageShape LocalShape = CutShape;
		LocalShape.WorldTransform = CutShape.WorldTransform.GetRelativeTransform(
			MaskWorldTransform);
		TArray<uint8> CandidateMask = Mask.SolidMask;
		return MatterFlux::FragmentGeometry::ApplyDamageShape(
			CandidateMask,
			Mask.Width,
			Mask.Height,
			Mask.CellSize,
			LocalShape);
	};

	const FFragmentSourceMask* RootMask = nullptr;
	if (RootCombustionState.IsValid())
	{
		RootMask = &RootCombustionState.SourceMask;
	}
	else if (SpawnPayload.DetachedVoxelMask.IsValid())
	{
		RootMask = &SpawnPayload.DetachedVoxelMask;
	}
	if (RootMask && IntersectsMask(*RootMask, GetActorTransform()))
	{
		return true;
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (Source.IsValid()
			&& IntersectsMask(
				Source.SourceMask,
				Source.LocalTransform * GetActorTransform()))
		{
			return true;
		}
	}

	// Non-voxel fragments already retain their canonical triangulated face in
	// the spawn payload. Test that face directly so generic debris does not need
	// a second raster mask merely to become cuttable.
	for (int32 Index = 0;
		Index + 2 < SpawnPayload.TriangleIndices.Num();
		Index += 3)
	{
		const int32 Indices[] =
		{
			SpawnPayload.TriangleIndices[Index],
			SpawnPayload.TriangleIndices[Index + 1],
			SpawnPayload.TriangleIndices[Index + 2]
		};
		FVector2D ShapePoints[3];
		bool bValidTriangle = true;
		for (int32 PointIndex = 0; PointIndex < 3; ++PointIndex)
		{
			if (!SpawnPayload.Vertices2D.IsValidIndex(Indices[PointIndex]))
			{
				bValidTriangle = false;
				break;
			}
			const FVector2D& Vertex =
				SpawnPayload.Vertices2D[Indices[PointIndex]];
			const FVector ShapeLocal = CutShape.WorldTransform
				.InverseTransformPosition(
					GetActorTransform().TransformPosition(
						FVector(Vertex.X, 0.0f, Vertex.Y)));
			ShapePoints[PointIndex] = FVector2D(ShapeLocal.X, ShapeLocal.Z);
		}
		if (bValidTriangle
			&& DoesTriangleIntersectDamageShape(
				ShapePoints[0],
				ShapePoints[1],
				ShapePoints[2],
				CutShape))
		{
			return true;
		}
	}
	return false;
}

bool AFragment2DActor::TryAcceptWorldCut(
	const FFragmentDamageShape& CutShape)
{
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| IsActorBeingDestroyed()
		|| IsCutFadeActive()
		|| CutsBeforeFade <= 0
		|| !FMath::IsFinite(CutExhaustionFadeDuration)
		|| CutExhaustionFadeDuration <= 0.0f
		|| !DoesCutShapeIntersect(CutShape))
	{
		return false;
	}

	AcceptedCutCount = FMath::Min(AcceptedCutCount + 1, CutsBeforeFade);
	if (AcceptedCutCount >= CutsBeforeFade)
	{
		ActiveCutFadeDuration = CutExhaustionFadeDuration;
		SynchronizeCutFadeState();
	}
	ForceNetUpdate();
	return true;
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

bool AFragment2DActor::GetAggregateSourceState(
	const FGuid& SourceId,
	FFragmentAggregateSourceState& OutState) const
{
	const FFragmentAggregateSourceState* Source =
		AggregateSources.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	if (!Source || !Source->IsValid())
	{
		return false;
	}
	OutState = *Source;
	return true;
}

bool AFragment2DActor::IsRootCombusting() const
{
	return RootCombustionState.bHasCombustionState
		&& RootCombustionState.BurningMask.SolidMask.Contains(1);
}

bool AFragment2DActor::IsAggregateSourceCombusting(
	const FGuid& SourceId) const
{
	const FFragmentAggregateSourceState* Source =
		AggregateSources.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	return Source
		&& Source->bHasCombustionState
		&& Source->BurningMask.SolidMask.Contains(1);
}

bool AFragment2DActor::IsAnyAggregateMaterialCombusting(
	const FName MaterialId) const
{
	return AggregateSources.ContainsByPredicate(
		[MaterialId](const FFragmentAggregateSourceState& Source)
		{
			return Source.MaterialId == MaterialId
				&& Source.bHasCombustionState
				&& Source.BurningMask.SolidMask.Contains(1);
		});
}

int32 AFragment2DActor::GetRootCombustionResidueCellCount() const
{
	return RootCombustionState.bHasCombustionState
		? CountSetCells(RootCombustionState.ResidueMask.SolidMask)
		: 0;
}

FBox AFragment2DActor::GetCombustibleWorldBounds() const
{
	FBox Bounds(ForceInit);
	if (RootCombustionState.IsValid())
	{
		Bounds += BuildMaskWorldBounds(
			RootCombustionState.SourceMask,
			GetActorTransform());
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (Source.IsValid())
		{
			Bounds += BuildMaskWorldBounds(
				Source.SourceMask,
				Source.LocalTransform * GetActorTransform());
		}
	}
	return Bounds;
}

void AFragment2DActor::InitializeRootCombustionState()
{
	const bool bSupportsCombustion = SpawnPayload.FragmentId.IsValid()
		&& !SpawnPayload.MaterialId.IsNone()
		&& SpawnPayload.DetachedVoxelMask.IsValid()
		&& SpawnPayload.DetachedVoxelMask.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks;
	if (!bSupportsCombustion)
	{
		RootCombustionState = FFragmentAggregateSourceState();
		RootCombustionRuntime.Reset();
		return;
	}
	if (RootCombustionState.SourceId == SpawnPayload.FragmentId
		&& RootCombustionState.SourceMask.HasValidLayout())
	{
		RootCombustionState.Material = FragmentMaterial;
		RootCombustionState.Color = FragmentColor;
		return;
	}

	RootCombustionState = FFragmentAggregateSourceState();
	RootCombustionState.SourceId = SpawnPayload.FragmentId;
	RootCombustionState.DefinitionSourceId = SpawnPayload.FragmentId;
	RootCombustionState.bOwnsLogicalSource = false;
	RootCombustionState.Revision = SpawnPayload.Revision;
	RootCombustionState.SourceMask = SpawnPayload.DetachedVoxelMask;
	RootCombustionState.LocalTransform = FTransform::Identity;
	RootCombustionState.Material = FragmentMaterial;
	RootCombustionState.MaterialId = SpawnPayload.MaterialId;
	RootCombustionState.Color = FragmentColor;
	RootCombustionState.bEnableCollision = SpawnPayload.bEnableCollision;
	RootCombustionState.ResidueMask = SpawnPayload.DetachedVoxelMask;
	RootCombustionState.ResidueMask.SolidMask.Init(
		0,
		SpawnPayload.DetachedVoxelMask.SolidMask.Num());
	RootCombustionState.BurningMask = SpawnPayload.DetachedVoxelMask;
	RootCombustionState.BurningMask.SolidMask.Init(
		0,
		SpawnPayload.DetachedVoxelMask.SolidMask.Num());
	RootCombustionRuntime.Reset();
}

bool AFragment2DActor::IgniteRootAtWorldLocation(
	const FVector& WorldLocation,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| WorldLocation.ContainsNaN()
		|| FlameMaterial.IsNone()
		|| !RootCombustionState.IsValid()
		|| IsRootCombusting())
	{
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (!Registry.IsValid())
	{
		return false;
	}
	const FMatterFluxReactionDefinition* Rule = FindCombustionRule(
		*Registry,
		RootCombustionState.MaterialId,
		FlameMaterial);
	if (!Rule)
	{
		return false;
	}

	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime> Candidate =
		MakeUnique<MatterFlux::Combustion::FSourceCombustionRuntime>();
	FString Error;
	if (!Candidate->Initialize(
		MatterFlux::Combustion::FSourceRuntimeSettings(),
		RootCombustionState.SourceMask,
		*Rule,
		EventSeed,
		Error))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Detached fragment combustion initialization failed: %s"),
			*Error);
		return false;
	}
	const FVector Local = GetActorTransform().InverseTransformPosition(
		WorldLocation);
	const FFragmentSourceMask& Mask = RootCombustionState.SourceMask;
	const FIntPoint RequestedCell(
		FMath::FloorToInt(
			Local.X / Mask.CellSize
				+ static_cast<double>(Mask.Width) * 0.5),
		FMath::FloorToInt(
			Local.Z / Mask.CellSize
				+ static_cast<double>(Mask.Height) * 0.5));
	if (!Candidate->IgniteNearest(RequestedCell, FlameMaterial))
	{
		return false;
	}
	RootCombustionRuntime = MoveTemp(Candidate);
	if (!SynchronizeRootCombustionState())
	{
		RootCombustionRuntime.Reset();
		return false;
	}
	RootCombustionPropagationAccumulator = 0.0f;
	SetActorTickEnabled(true);
	ForceNetUpdate();
	MarkCombustionVisualizationDirty();
	return true;
}

bool AFragment2DActor::IgniteAtWorldLocation(
	const FVector& WorldLocation,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| WorldLocation.ContainsNaN()
		|| FlameMaterial.IsNone())
	{
		return false;
	}
	struct FCandidate
	{
		FGuid SourceId;
		double DistanceSquared = TNumericLimits<double>::Max();
		bool bRoot = false;
	};
	TArray<FCandidate, TInlineAllocator<17>> Candidates;
	if (RootCombustionState.IsValid()
		&& RootCombustionState.SourceMask.SolidMask.Contains(1)
		&& !IsRootCombusting())
	{
		const FBox Bounds = BuildMaskWorldBounds(
			RootCombustionState.SourceMask,
			GetActorTransform());
		if (Bounds.IsValid)
		{
			Candidates.Add({
				RootCombustionState.SourceId,
				Bounds.ComputeSquaredDistanceToPoint(WorldLocation),
				true});
		}
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.IsValid()
			|| !Source.SourceMask.SolidMask.Contains(1)
			|| IsAggregateSourceCombusting(Source.SourceId))
		{
			continue;
		}
		const FBox Bounds = BuildMaskWorldBounds(
			Source.SourceMask,
			Source.LocalTransform * GetActorTransform());
		if (Bounds.IsValid)
		{
			Candidates.Add({
				Source.SourceId,
				Bounds.ComputeSquaredDistanceToPoint(WorldLocation),
				false});
		}
	}
	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
	{
		if (!FMath::IsNearlyEqual(A.DistanceSquared, B.DistanceSquared))
		{
			return A.DistanceSquared < B.DistanceSquared;
		}
		if (A.bRoot != B.bRoot)
		{
			return A.bRoot;
		}
		return A.SourceId.ToString(EGuidFormats::Digits)
			< B.SourceId.ToString(EGuidFormats::Digits);
	});
	if (Candidates.IsEmpty())
	{
		return false;
	}
	if (Candidates[0].bRoot)
	{
		return IgniteRootAtWorldLocation(
			WorldLocation,
			FlameMaterial,
			EventSeed);
	}
	return IgniteAggregateSourceAtWorldLocation(
		Candidates[0].SourceId,
		WorldLocation,
		FlameMaterial,
		EventSeed);
}

bool AFragment2DActor::IgniteAggregateSourceAtWorldLocation(
	const FGuid& SourceId,
	const FVector& WorldLocation,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !SourceId.IsValid()
		|| WorldLocation.ContainsNaN()
		|| FlameMaterial.IsNone())
	{
		return false;
	}
	const FFragmentAggregateSourceState* Layer =
		AggregateSources.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	if (!Layer || !Layer->IsValid())
	{
		return false;
	}
	if (!Layer->bOwnsLogicalSource)
	{
		return IgniteDetachedAggregateAtWorldLocation(
			SourceId,
			WorldLocation,
			FlameMaterial,
			EventSeed);
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (AMatterFluxPlayableWorldActor* OwnerWorld =
		Cast<AMatterFluxPlayableWorldActor>(GetOwner()))
	{
		return OwnerWorld->IgniteDynamicAggregateSource(
			*this,
			SourceId,
			WorldLocation,
			FlameMaterial,
			EventSeed);
	}
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		if (It->IgniteDynamicAggregateSource(
			*this,
			SourceId,
			WorldLocation,
			FlameMaterial,
			EventSeed))
		{
			return true;
		}
	}
	return false;
}

bool AFragment2DActor::IgniteInCone(
	const FVector& Start,
	const FVector& Direction,
	const float Range,
	const float StartRadius,
	const float EndRadius,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	const FVector Forward = Direction.GetSafeNormal();
	if (!HasAuthority()
		|| Start.ContainsNaN()
		|| Forward.IsNearlyZero()
		|| !FMath::IsFinite(Range)
		|| !FMath::IsFinite(StartRadius)
		|| !FMath::IsFinite(EndRadius)
		|| Range <= 0.0f
		|| StartRadius <= 0.0f
		|| EndRadius < StartRadius
		|| FlameMaterial.IsNone())
	{
		return false;
	}
	struct FConeCandidate
	{
		FGuid SourceId;
		FVector Contact = FVector::ZeroVector;
		double Score = TNumericLimits<double>::Max();
		bool bRoot = false;
	};
	TArray<FConeCandidate, TInlineAllocator<17>> Candidates;
	const auto AddCandidate = [
		&Candidates,
		&Start,
		&Forward,
		Range,
		StartRadius,
		EndRadius](
			const FGuid& SourceId,
			const FFragmentSourceMask& Mask,
			const FTransform& WorldTransform,
			const bool bRoot)
	{
		const FBox Bounds = BuildMaskWorldBounds(Mask, WorldTransform);
		if (!Bounds.IsValid)
		{
			return;
		}
		constexpr int32 SampleCount = 24;
		double BestScore = TNumericLimits<double>::Max();
		FVector BestContact = FVector::ZeroVector;
		for (int32 SampleIndex = 0;
			SampleIndex <= SampleCount;
			++SampleIndex)
		{
			const float Alpha = static_cast<float>(SampleIndex)
				/ static_cast<float>(SampleCount);
			const FVector Centerline = Start + Forward * (Range * Alpha);
			const float Radius = FMath::Lerp(StartRadius, EndRadius, Alpha);
			const double DistanceSquared =
				Bounds.ComputeSquaredDistanceToPoint(Centerline);
			const double Score = DistanceSquared
				/ FMath::Max(
					static_cast<double>(Radius) * Radius,
					UE_DOUBLE_SMALL_NUMBER);
			if (Score < BestScore)
			{
				BestScore = Score;
				BestContact = Bounds.GetClosestPointTo(Centerline);
			}
		}
		if (BestScore <= 1.0 + UE_DOUBLE_SMALL_NUMBER)
		{
			Candidates.Add({SourceId, BestContact, BestScore, bRoot});
		}
	};
	if (RootCombustionState.IsValid()
		&& RootCombustionState.SourceMask.SolidMask.Contains(1)
		&& !IsRootCombusting())
	{
		AddCandidate(
			RootCombustionState.SourceId,
			RootCombustionState.SourceMask,
			GetActorTransform(),
			true);
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (Source.IsValid()
			&& Source.SourceMask.SolidMask.Contains(1)
			&& !IsAggregateSourceCombusting(Source.SourceId))
		{
			AddCandidate(
				Source.SourceId,
				Source.SourceMask,
				Source.LocalTransform * GetActorTransform(),
				false);
		}
	}
	Candidates.Sort([](const FConeCandidate& A, const FConeCandidate& B)
	{
		if (!FMath::IsNearlyEqual(A.Score, B.Score))
		{
			return A.Score < B.Score;
		}
		if (A.bRoot != B.bRoot)
		{
			return A.bRoot;
		}
		return A.SourceId.ToString(EGuidFormats::Digits)
			< B.SourceId.ToString(EGuidFormats::Digits);
	});
	if (Candidates.IsEmpty())
	{
		return false;
	}
	const FConeCandidate& Best = Candidates[0];
	if (Best.bRoot)
	{
		return IgniteRootAtWorldLocation(
			Best.Contact,
			FlameMaterial,
			EventSeed);
	}
	if (const FFragmentAggregateSourceState* DetachedLayer =
		AggregateSources.FindByPredicate(
			[&Best](const FFragmentAggregateSourceState& Source)
			{
				return Source.SourceId == Best.SourceId;
			});
		DetachedLayer && !DetachedLayer->bOwnsLogicalSource)
	{
		return IgniteDetachedAggregateAtWorldLocation(
			DetachedLayer->SourceId,
			Best.Contact,
			FlameMaterial,
			EventSeed);
	}
	if (AMatterFluxPlayableWorldActor* OwnerWorld =
		Cast<AMatterFluxPlayableWorldActor>(GetOwner()))
	{
		return OwnerWorld->IgniteDynamicAggregateSource(
			*this,
			Best.SourceId,
			Best.Contact,
			FlameMaterial,
			EventSeed);
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			if (It->IgniteDynamicAggregateSource(
				*this,
				Best.SourceId,
				Best.Contact,
				FlameMaterial,
				EventSeed))
			{
				return true;
			}
		}
	}
	return false;
}

bool AFragment2DActor::ApplyAggregateSourceStreamingState(
	const FGuid& SourceId,
	const FFragment2DSourceStreamingState& State,
	const FName InResidueMaterialId,
	const FLinearColor& InResidueColor)
{
	if (!SourceId.IsValid())
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
	MarkCombustionVisualizationDirty();
	return true;
}

bool AFragment2DActor::SynchronizeRootCombustionState()
{
	if (!RootCombustionRuntime || !RootCombustionRuntime->IsInitialized())
	{
		return false;
	}
	MatterFlux::Combustion::FSourceRuntimeSnapshot Snapshot;
	if (!RootCombustionRuntime->CaptureState(Snapshot))
	{
		return false;
	}
	RootCombustionState.SourceMask.SolidMask =
		MoveTemp(Snapshot.CombustionState.FuelMask);
	RootCombustionState.ResidueMask = RootCombustionState.SourceMask;
	RootCombustionState.ResidueMask.SolidMask =
		MoveTemp(Snapshot.CombustionState.ResidueMask);
	RootCombustionState.BurningMask = RootCombustionState.SourceMask;
	RootCombustionState.BurningMask.SolidMask =
		MoveTemp(Snapshot.CombustionState.BurningMask);
	for (uint8& Value : RootCombustionState.BurningMask.SolidMask)
	{
		Value = Value != 0 ? 1 : 0;
	}
	RootCombustionState.bHasCombustionState = true;
	RootCombustionState.CombustionRuleId = Snapshot.CombustionState.RuleId;
	RootCombustionState.CombustionSeed = Snapshot.CombustionState.Seed;
	RootCombustionState.CombustionTick = Snapshot.CombustionState.Tick;
	RootCombustionState.CombustionAccumulator =
		Snapshot.CombustionAccumulator;
	RootCombustionState.TotalSmokeEmissionCount =
		Snapshot.TotalSmokeEmissionCount;
	if (const FMatterFluxReactionDefinition* Rule =
		RootCombustionRuntime->GetRule())
	{
		RootCombustionState.ResidueMaterialId = Rule->OutputA;
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (Registry.IsValid())
		{
			if (const FMatterFluxMaterialDefinition* Residue =
				Registry->Materials.Find(Rule->OutputA))
			{
				RootCombustionState.ResidueColor = Residue->Color;
			}
		}
	}
	if (!RootCombustionState.IsValid() || !RebuildMeshFromPayload())
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Detached fragment rejected its combustion mesh update"));
		return false;
	}
	ForceNetUpdate();
	MarkCombustionVisualizationDirty();
	return true;
}

void AFragment2DActor::AdvanceRootCombustion(const float DeltaSeconds)
{
	if (!HasAuthority()
		|| !RootCombustionRuntime
		|| !RootCombustionRuntime->IsBurning())
	{
		return;
	}
	const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	const MatterFlux::Combustion::FSourceAdvanceResult Result =
		RootCombustionRuntime->AdvanceAuthority(ClampedDelta);
	if (Result.Steps > 0)
	{
		SynchronizeRootCombustionState();
	}

	RootCombustionPropagationAccumulator += ClampedDelta;
	if (RootCombustionRuntime->IsBurning()
		&& RootCombustionPropagationAccumulator >= 0.2f)
	{
		RootCombustionPropagationAccumulator = FMath::Fmod(
			RootCombustionPropagationAccumulator,
			0.2f);
		const FMatterFluxReactionDefinition* Rule =
			RootCombustionRuntime->GetRule();
		if (Rule)
		{
			const int32 Seed = RootCombustionState.CombustionSeed
				^ static_cast<int32>(RootCombustionState.CombustionTick)
				^ static_cast<int32>(GetTypeHash(SpawnPayload.FragmentId));
			PropagateCombustionToAdjacentLayer(
				RootCombustionState,
				GetActorTransform(),
				Rule->InputB,
				Seed);
		}
	}
	if (!RootCombustionRuntime->IsBurning())
	{
		RootCombustionRuntime.Reset();
		SetActorTickEnabled(SpawnPayload.FadeOutDuration > 0.0f);
	}
}

bool AFragment2DActor::IgniteDetachedAggregateAtWorldLocation(
	const FGuid& SourceId,
	const FVector& WorldLocation,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	FFragmentAggregateSourceState* Source = AggregateSources.FindByPredicate(
		[&SourceId](const FFragmentAggregateSourceState& Candidate)
		{
			return Candidate.SourceId == SourceId;
		});
	if (!HasAuthority()
		|| WorldLocation.ContainsNaN()
		|| FlameMaterial.IsNone()
		|| !Source
		|| Source->bOwnsLogicalSource
		|| !Source->IsValid()
		|| IsAggregateSourceCombusting(SourceId))
	{
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	if (!Registry.IsValid())
	{
		return false;
	}
	const FMatterFluxReactionDefinition* Rule = FindCombustionRule(
		*Registry,
		Source->MaterialId,
		FlameMaterial);
	if (!Rule)
	{
		return false;
	}

	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime> Runtime =
		MakeUnique<MatterFlux::Combustion::FSourceCombustionRuntime>();
	FString Error;
	if (!Runtime->Initialize(
		MatterFlux::Combustion::FSourceRuntimeSettings(),
		Source->SourceMask,
		*Rule,
		EventSeed,
		Error))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Detached aggregate layer combustion initialization failed: %s"),
			*Error);
		return false;
	}
	const FTransform WorldTransform =
		Source->LocalTransform * GetActorTransform();
	const FVector Local = WorldTransform.InverseTransformPosition(
		WorldLocation);
	const FIntPoint RequestedCell(
		FMath::FloorToInt(
			Local.X / Source->SourceMask.CellSize
				+ static_cast<double>(Source->SourceMask.Width) * 0.5),
		FMath::FloorToInt(
			Local.Z / Source->SourceMask.CellSize
				+ static_cast<double>(Source->SourceMask.Height) * 0.5));
	if (!Runtime->IgniteNearest(RequestedCell, FlameMaterial))
	{
		return false;
	}
	DetachedAggregateCombustionRuntimes.Add(
		SourceId,
		MoveTemp(Runtime));
	if (!SynchronizeDetachedAggregateCombustionState(SourceId))
	{
		DetachedAggregateCombustionRuntimes.Remove(SourceId);
		return false;
	}
	DetachedAggregateCombustionPropagationAccumulator = 0.0f;
	SetActorTickEnabled(true);
	return true;
}

bool AFragment2DActor::SynchronizeDetachedAggregateCombustionState(
	const FGuid& SourceId)
{
	TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime>* RuntimePtr =
		DetachedAggregateCombustionRuntimes.Find(SourceId);
	FFragmentAggregateSourceState* Source = AggregateSources.FindByPredicate(
		[&SourceId](const FFragmentAggregateSourceState& Candidate)
		{
			return Candidate.SourceId == SourceId;
		});
	if (!RuntimePtr || !RuntimePtr->Get() || !Source)
	{
		return false;
	}
	MatterFlux::Combustion::FSourceRuntimeSnapshot Snapshot;
	if (!(*RuntimePtr)->CaptureState(Snapshot))
	{
		return false;
	}
	Source->SourceMask.SolidMask = MoveTemp(
		Snapshot.CombustionState.FuelMask);
	Source->ResidueMask = Source->SourceMask;
	Source->ResidueMask.SolidMask = MoveTemp(
		Snapshot.CombustionState.ResidueMask);
	Source->BurningMask = Source->SourceMask;
	Source->BurningMask.SolidMask = MoveTemp(
		Snapshot.CombustionState.BurningMask);
	for (uint8& Value : Source->BurningMask.SolidMask)
	{
		Value = Value != 0 ? 1 : 0;
	}
	Source->bHasCombustionState = true;
	Source->CombustionRuleId = Snapshot.CombustionState.RuleId;
	Source->CombustionSeed = Snapshot.CombustionState.Seed;
	Source->CombustionTick = Snapshot.CombustionState.Tick;
	Source->CombustionAccumulator = Snapshot.CombustionAccumulator;
	Source->TotalSmokeEmissionCount = Snapshot.TotalSmokeEmissionCount;
	if (const FMatterFluxReactionDefinition* Rule =
		(*RuntimePtr)->GetRule())
	{
		Source->ResidueMaterialId = Rule->OutputA;
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (Registry.IsValid())
		{
			if (const FMatterFluxMaterialDefinition* Residue =
				Registry->Materials.Find(Rule->OutputA))
			{
				Source->ResidueColor = Residue->Color;
			}
		}
	}
	if (!Source->IsValid() || !RebuildMeshFromPayload())
	{
		return false;
	}
	ForceNetUpdate();
	MarkCombustionVisualizationDirty();
	return true;
}

void AFragment2DActor::AdvanceDetachedAggregateCombustion(
	const float DeltaSeconds)
{
	if (!HasAuthority() || DetachedAggregateCombustionRuntimes.IsEmpty())
	{
		return;
	}
	const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	TArray<FGuid> Completed;
	TArray<FGuid> BurningIds;
	DetachedAggregateCombustionRuntimes.GetKeys(BurningIds);
	BurningIds.Sort([](const FGuid& A, const FGuid& B)
	{
		return A.ToString(EGuidFormats::Digits)
			< B.ToString(EGuidFormats::Digits);
	});
	for (const FGuid& SourceId : BurningIds)
	{
		TUniquePtr<MatterFlux::Combustion::FSourceCombustionRuntime>* Runtime =
			DetachedAggregateCombustionRuntimes.Find(SourceId);
		if (!Runtime || !Runtime->Get())
		{
			Completed.Add(SourceId);
			continue;
		}
		const MatterFlux::Combustion::FSourceAdvanceResult Result =
			(*Runtime)->AdvanceAuthority(ClampedDelta);
		if (Result.Steps > 0)
		{
			SynchronizeDetachedAggregateCombustionState(SourceId);
		}
		if (!(*Runtime)->IsBurning())
		{
			Completed.Add(SourceId);
		}
	}
	for (const FGuid& SourceId : Completed)
	{
		DetachedAggregateCombustionRuntimes.Remove(SourceId);
	}

	DetachedAggregateCombustionPropagationAccumulator += ClampedDelta;
	if (!DetachedAggregateCombustionRuntimes.IsEmpty()
		&& DetachedAggregateCombustionPropagationAccumulator >= 0.2f)
	{
		DetachedAggregateCombustionPropagationAccumulator = FMath::Fmod(
			DetachedAggregateCombustionPropagationAccumulator,
			0.2f);
		for (const FGuid& SourceId : BurningIds)
		{
			const TUniquePtr<
				MatterFlux::Combustion::FSourceCombustionRuntime>* Runtime =
				DetachedAggregateCombustionRuntimes.Find(SourceId);
			const FFragmentAggregateSourceState* Source =
				AggregateSources.FindByPredicate(
					[&SourceId](const FFragmentAggregateSourceState& Candidate)
					{
						return Candidate.SourceId == SourceId;
					});
			if (!Runtime || !Runtime->Get() || !Source)
			{
				continue;
			}
			const FMatterFluxReactionDefinition* Rule = (*Runtime)->GetRule();
			if (Rule)
			{
				PropagateCombustionToAdjacentLayer(
					*Source,
					Source->LocalTransform * GetActorTransform(),
					Rule->InputB,
					Source->CombustionSeed
						^ static_cast<int32>(Source->CombustionTick));
				break;
			}
		}
	}
}

bool AFragment2DActor::PropagateCombustionToAdjacentLayer(
	const FFragmentAggregateSourceState& BurningSource,
	const FTransform& BurningWorldTransform,
	const FName FlameMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !BurningSource.IsValid()
		|| !BurningSource.bHasCombustionState
		|| FlameMaterial.IsNone()
		|| !BurningWorldTransform.IsValid())
	{
		return false;
	}
	TArray<FVector, TInlineAllocator<64>> BurningCellCenters;
	for (int32 Index = 0;
		Index < BurningSource.BurningMask.SolidMask.Num();
		++Index)
	{
		if (BurningSource.BurningMask.SolidMask[Index] == 0)
		{
			continue;
		}
		const int32 X = Index % BurningSource.SourceMask.Width;
		const int32 Y = Index / BurningSource.SourceMask.Width;
		BurningCellCenters.Add(BurningWorldTransform.TransformPosition(FVector(
			(static_cast<float>(X) + 0.5f
				- BurningSource.SourceMask.Width * 0.5f)
				* BurningSource.SourceMask.CellSize,
			0.0f,
			(static_cast<float>(Y) + 0.5f
				- BurningSource.SourceMask.Height * 0.5f)
				* BurningSource.SourceMask.CellSize)));
	}
	if (BurningCellCenters.IsEmpty())
	{
		return false;
	}

	struct FAdjacentLayerContact
	{
		const FFragmentAggregateSourceState* Source = nullptr;
		FTransform WorldTransform = FTransform::Identity;
		FVector WorldLocation = FVector::ZeroVector;
		double DistanceSquared = TNumericLimits<double>::Max();
		int32 CellIndex = INDEX_NONE;
		bool bRoot = false;
	};
	FAdjacentLayerContact Best;
	const auto ConsiderLayer = [
		&Best,
		&BurningCellCenters,
		&BurningSource](
			const FFragmentAggregateSourceState& Candidate,
			const FTransform& CandidateWorldTransform,
			const bool bRoot)
	{
		if (Candidate.SourceId == BurningSource.SourceId
			|| !Candidate.IsValid()
			|| Candidate.bHasCombustionState
				&& Candidate.BurningMask.SolidMask.Contains(1))
		{
			return;
		}
		const double MaximumDistanceSquared = FMath::Square(
			FMath::Max(
				BurningSource.SourceMask.CellSize,
				Candidate.SourceMask.CellSize) * 1.05);
		for (int32 Index = 0;
			Index < Candidate.SourceMask.SolidMask.Num();
			++Index)
		{
			if (Candidate.SourceMask.SolidMask[Index] == 0)
			{
				continue;
			}
			const int32 X = Index % Candidate.SourceMask.Width;
			const int32 Y = Index / Candidate.SourceMask.Width;
			const FVector CandidateCenter =
				CandidateWorldTransform.TransformPosition(FVector(
					(static_cast<float>(X) + 0.5f
						- Candidate.SourceMask.Width * 0.5f)
						* Candidate.SourceMask.CellSize,
					0.0f,
					(static_cast<float>(Y) + 0.5f
						- Candidate.SourceMask.Height * 0.5f)
						* Candidate.SourceMask.CellSize));
			for (const FVector& BurningCenter : BurningCellCenters)
			{
				const double DistanceSquared = FVector::DistSquared(
					BurningCenter,
					CandidateCenter);
				if (DistanceSquared > MaximumDistanceSquared)
				{
					continue;
				}
				const bool bStableTieBreak = FMath::IsNearlyEqual(
					DistanceSquared,
					Best.DistanceSquared)
					&& (!Best.Source
						|| Candidate.SourceId.ToString(EGuidFormats::Digits)
							< Best.Source->SourceId.ToString(EGuidFormats::Digits)
						|| (Candidate.SourceId == Best.Source->SourceId
							&& Index < Best.CellIndex));
				if (!Best.Source
					|| DistanceSquared < Best.DistanceSquared
					|| bStableTieBreak)
				{
					Best.Source = &Candidate;
					Best.WorldTransform = CandidateWorldTransform;
					Best.WorldLocation = CandidateCenter;
					Best.DistanceSquared = DistanceSquared;
					Best.CellIndex = Index;
					Best.bRoot = bRoot;
				}
			}
		}
	};
	if (RootCombustionState.IsValid())
	{
		ConsiderLayer(
			RootCombustionState,
			GetActorTransform(),
			true);
	}
	for (const FFragmentAggregateSourceState& Candidate : AggregateSources)
	{
		ConsiderLayer(
			Candidate,
			Candidate.LocalTransform * GetActorTransform(),
			false);
	}
	if (!Best.Source)
	{
		return false;
	}
	if (Best.bRoot)
	{
		return IgniteRootAtWorldLocation(
			Best.WorldLocation,
			FlameMaterial,
			EventSeed);
	}
	if (!Best.Source->bOwnsLogicalSource)
	{
		return IgniteDetachedAggregateAtWorldLocation(
			Best.Source->SourceId,
			Best.WorldLocation,
			FlameMaterial,
			EventSeed);
	}
	if (AMatterFluxPlayableWorldActor* WorldOwner =
		Cast<AMatterFluxPlayableWorldActor>(GetOwner()))
	{
		return WorldOwner->IgniteDynamicAggregateSource(
			*this,
			Best.Source->SourceId,
			Best.WorldLocation,
			FlameMaterial,
			EventSeed);
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			if (It->IgniteDynamicAggregateSource(
				*this,
				Best.Source->SourceId,
				Best.WorldLocation,
				FlameMaterial,
				EventSeed))
			{
				return true;
			}
		}
	}
	return false;
}

void AFragment2DActor::GatherRootCombustionVisualTransforms(
	TArray<FTransform>& OutFlameTransforms,
	TArray<MatterFlux::Rendering::FSmokeEmissionAnchor>& OutSmokeAnchors,
	const int32 MaxVisualInstances) const
{
	if (MaxVisualInstances <= 0)
	{
		return;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	const auto AppendState = [
		this,
		&OutFlameTransforms,
		&OutSmokeAnchors,
		&Registry,
		MaxVisualInstances](
			const FFragmentAggregateSourceState& State,
			const FTransform& VisualTransform)
	{
		if (!State.IsValid()
			|| !State.bHasCombustionState
			|| !State.BurningMask.SolidMask.Contains(1))
		{
			return;
		}
		TArray<uint8> Occupied = State.SourceMask.SolidMask;
		if (State.ResidueMask.SolidMask.Num() == Occupied.Num())
		{
			for (int32 Index = 0; Index < Occupied.Num(); ++Index)
			{
				Occupied[Index] = Occupied[Index] != 0
					|| State.ResidueMask.SolidMask[Index] != 0;
			}
		}
		TArray<int32> VisibleBurningCells;
		for (int32 Index = 0;
			Index < State.BurningMask.SolidMask.Num();
			++Index)
		{
			if (State.BurningMask.SolidMask[Index] != 0)
			{
				VisibleBurningCells.Add(Index);
			}
		}
		TArray<int32> SmokeSourceCells;
		MatterFlux::FragmentGeometry::GatherTopExposedBurningMaskCells(
			Occupied,
			State.BurningMask.SolidMask,
			State.SourceMask.Width,
			State.SourceMask.Height,
			SmokeSourceCells);
		TSet<int32> SmokeSourceCellSet;
		for (const int32 SmokeSourceCell : SmokeSourceCells)
		{
			SmokeSourceCellSet.Add(SmokeSourceCell);
		}
		const FFragmentSourceMask& Mask = State.SourceMask;
		for (const int32 Index : VisibleBurningCells)
		{
			if (OutFlameTransforms.Num() >= MaxVisualInstances)
			{
				break;
			}
			const int32 X = Index % Mask.Width;
			const int32 Y = Index / Mask.Width;
			const FVector LocalPosition(
				(static_cast<float>(X) + 0.5f
					- static_cast<float>(Mask.Width) * 0.5f)
					* Mask.CellSize,
				VisualDepthOffset,
				(static_cast<float>(Y) + 0.62f
					- static_cast<float>(Mask.Height) * 0.5f)
					* Mask.CellSize);
			const FVector Position = VisualTransform.TransformPosition(
				LocalPosition);
			const float BaseScale = Mask.CellSize / 100.0f;
			const FVector FlameScale(
				BaseScale * 1.06f,
				BaseScale * 1.06f,
				BaseScale * 1.18f);
			OutFlameTransforms.Emplace(
				VisualTransform.Rotator(),
				Position,
				FlameScale);
			if (SmokeSourceCellSet.Contains(Index)
				&& OutSmokeAnchors.Num() < MaxVisualInstances)
			{
				float SmokeProbability = 0.7f;
				if (Registry.IsValid())
				{
					if (const FMatterFluxReactionDefinition* Rule =
						Registry->Reactions.Find(State.CombustionRuleId))
					{
						SmokeProbability = FMath::Clamp(
							static_cast<float>(Rule->EmissionChancePermille)
								/ 1000.0f,
							0.0f,
							1.0f);
					}
				}
				MatterFlux::Rendering::FSmokeEmissionAnchor& Anchor =
					OutSmokeAnchors.AddDefaulted_GetRef();
				Anchor.WorldPosition = Position + FVector(
					0.0f,
					0.0f,
					Mask.CellSize * 1.05f);
				Anchor.CellSize = Mask.CellSize;
				Anchor.EmissionProbability = SmokeProbability;
				Anchor.Seed = GetTypeHash(State.SourceId)
					^ static_cast<uint32>(Index) * 0x9e3779b9u;
			}
		}
	};
	if (IsRootCombusting())
	{
		AppendState(RootCombustionState, GetActorTransform());
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.bOwnsLogicalSource)
		{
			AppendState(
				Source,
				Source.LocalTransform * GetActorTransform());
		}
	}
}

void AFragment2DActor::MarkCombustionVisualizationDirty() const
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			It->MarkSourceCombustionVisualizationDirty();
		}
	}
}

bool AFragment2DActor::RebuildAggregateVisualSectionsOnly()
{
	if (!MeshComponent)
	{
		return false;
	}
	// 体素根碎片与枝叶必须共用一套占用。这里只清 aggregate section
	// 会保留旧的独立树干网格，再次造成木材穿叶，因此统一重建。
	if (SpawnPayload.DetachedVoxelMask.IsValid())
	{
		return RebuildMeshFromPayload();
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
	Candidate.DefinitionSourceId = SourceActor.SourceId;
	Candidate.bOwnsLogicalSource = true;
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
			if (const FMatterFluxReactionDefinition* Rule =
				Registry->Reactions.Find(Candidate.CombustionRuleId))
			{
				Candidate.ResidueMaterialId = Rule->OutputA;
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(Rule->OutputA))
				{
					Candidate.ResidueColor = Material->Color;
				}
			}
		}
	}
	return AddAggregateSourceState(MoveTemp(Candidate), &SourceActor);
}

bool AFragment2DActor::AbsorbAggregateSourceFragment(
	const AFragment2DSourceActor& SourceActor,
	const FFragmentSpawnPayload& Payload)
{
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| SourceActor.GetWorld() != GetWorld()
		|| !Payload.FragmentId.IsValid()
		|| !Payload.DetachedVoxelMask.IsValid()
		|| Payload.MaterialId.IsNone()
		|| ContainsAggregateSource(Payload.FragmentId)
		|| AggregateSources.Num() >= 16)
	{
		return false;
	}

	FFragmentAggregateSourceState Candidate;
	Candidate.SourceId = Payload.FragmentId;
	Candidate.DefinitionSourceId = SourceActor.SourceId;
	Candidate.bOwnsLogicalSource = false;
	Candidate.Revision = Payload.Revision;
	Candidate.SourceMask = Payload.DetachedVoxelMask;
	Candidate.LocalTransform = Payload.InitialTransform
		.GetRelativeTransform(GetActorTransform());
	Candidate.Material = SourceActor.FragmentMaterial;
	Candidate.MaterialId = Payload.MaterialId;
	Candidate.Color = SourceActor.FragmentColor;
	Candidate.bEnableCollision = Payload.bEnableCollision;
	Candidate.ResidueMask = Candidate.SourceMask;
	Candidate.ResidueMask.SolidMask.Init(
		0,
		Candidate.SourceMask.SolidMask.Num());
	Candidate.BurningMask = Candidate.SourceMask;
	Candidate.BurningMask.SolidMask.Init(
		0,
		Candidate.SourceMask.SolidMask.Num());
	return AddAggregateSourceState(MoveTemp(Candidate), nullptr);
}

bool AFragment2DActor::AddAggregateSourceState(
	FFragmentAggregateSourceState&& Candidate,
	AFragment2DSourceActor* RetiredSource)
{
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

	const FGuid CandidateId = Candidate.SourceId;
	const bool bOwnsLogicalSource = Candidate.bOwnsLogicalSource;
	const int32 AddedSolidCells = CountSetCells(
		Candidate.SourceMask.SolidMask);
	AggregateSources.Add(MoveTemp(Candidate));
	RefreshBuoyancyDensity();
	if (!RebuildMeshFromPayload())
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Aggregate carrier mesh rejected source %s"),
			*CandidateId.ToString());
		AggregateSources.Pop(EAllowShrinking::No);
		RefreshBuoyancyDensity();
		RebuildMeshFromPayload();
		return false;
	}

	if (RetiredSource)
	{
		if (AMatterFluxPlayableWorldActor* WorldOwner =
			Cast<AMatterFluxPlayableWorldActor>(RetiredSource->GetOwner());
			WorldOwner
			&& !WorldOwner->RetireFragmentSourceIntoDynamicAggregate(
				*RetiredSource,
				*this))
		{
			UE_LOG(
				LogMatterFlux,
				Error,
				TEXT("Playable world rejected aggregate source handoff %s"),
				*CandidateId.ToString());
			AggregateSources.Pop(EAllowShrinking::No);
			RefreshBuoyancyDensity();
			RebuildMeshFromPayload();
			return false;
		}
	}

	SpawnPayload.Mass = FMath::Clamp(
		PreviousMass + static_cast<float>(AddedSolidCells) * 0.05f,
		0.5f,
		800.0f);
	// Aggregate members were already parts of the same static object before the
	// handoff. Absorbing their render/collision layers must not repeatedly divide
	// the felling velocity by the growing mass, otherwise a full tree loses almost
	// all sideways motion before the first physics frame and restacks upright.
	const FVector PreservedMomentumVelocity = PreservedLinearVelocity;
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

	if (RetiredSource)
	{
		RetiredSource->Destroy();
	}
	ForceNetUpdate();
	if (bOwnsLogicalSource)
	{
		NotifyWorldOfAggregateSources();
	}
	return true;
}

void AFragment2DActor::RefreshBuoyancyDensity()
{
	if (!BuoyancyComponent || !IMatterFluxScriptRuntime::IsAvailable())
	{
		return;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!Registry.IsValid())
	{
		return;
	}

	double WeightedDensity = 0.0;
	double TotalWeight = 0.0;
	const auto AddMaterial = [&Registry, &WeightedDensity, &TotalWeight](
		const FName MaterialId,
		const double Volume)
	{
		const FMatterFluxMaterialDefinition* Material =
			Registry->Materials.Find(MaterialId);
		if (!Material
			|| !FMath::IsFinite(Material->Density)
			|| Material->Density <= 0.0f
			|| !FMath::IsFinite(Volume)
			|| Volume <= UE_SMALL_NUMBER)
		{
			return;
		}
		WeightedDensity += static_cast<double>(Material->Density) * Volume;
		TotalWeight += Volume;
	};
	const auto MaskVolume = [](const FFragmentSourceMask& Mask)
	{
		if (!Mask.HasValidLayout())
		{
			return 0.0;
		}
		const double CellSize = static_cast<double>(Mask.CellSize);
		return static_cast<double>(CountSetCells(Mask.SolidMask))
			* CellSize * CellSize * CellSize;
	};
	const auto PayloadVolume = [&MaskVolume](
		const FFragmentSpawnPayload& Payload)
	{
		const double VoxelVolume = MaskVolume(Payload.DetachedVoxelMask);
		if (VoxelVolume > UE_SMALL_NUMBER)
		{
			return VoxelVolume;
		}
		double FaceArea = 0.0;
		for (int32 Index = 0;
			Index + 2 < Payload.TriangleIndices.Num();
			Index += 3)
		{
			const int32 A = Payload.TriangleIndices[Index];
			const int32 B = Payload.TriangleIndices[Index + 1];
			const int32 C = Payload.TriangleIndices[Index + 2];
			if (!Payload.Vertices2D.IsValidIndex(A)
				|| !Payload.Vertices2D.IsValidIndex(B)
				|| !Payload.Vertices2D.IsValidIndex(C))
			{
				continue;
			}
			const FVector2D AB =
				Payload.Vertices2D[B] - Payload.Vertices2D[A];
			const FVector2D AC =
				Payload.Vertices2D[C] - Payload.Vertices2D[A];
			FaceArea += FMath::Abs(
				static_cast<double>(AB.X) * AC.Y
					- static_cast<double>(AB.Y) * AC.X) * 0.5;
		}
		return FaceArea * FMath::Max(
			static_cast<double>(Payload.Thickness), 0.0);
	};
	AddMaterial(
		SpawnPayload.MaterialId,
		PayloadVolume(SpawnPayload));
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		AddMaterial(
			Source.MaterialId,
			MaskVolume(Source.SourceMask));
	}
	if (TotalWeight > UE_SMALL_NUMBER)
	{
		BuoyancyComponent->SetBodyDensity(
			static_cast<float>(WeightedDensity / TotalWeight));
	}
}

void AFragment2DActor::ApplyFragmentMaterial()
{
	UMaterialInterface* ParentMaterial = SpawnPayload.FadeOutDuration > 0.0f
		&& TransientFadeMaterial
		? TransientFadeMaterial.Get()
		: FragmentMaterial.Get();
	ParentMaterial = MatterFlux::Rendering::ResolveDynamicMaterialParent(
		ParentMaterial);
	if (!ParentMaterial)
	{
		DynamicFragmentMaterial = nullptr;
		DynamicFragmentSideMaterial = nullptr;
		MeshComponent->SetMaterial(0, nullptr);
		MeshComponent->SetMaterial(1, nullptr);
		return;
	}

	const float CellSize = SpawnPayload.DetachedVoxelMask.CellSize > 0.0f
		? SpawnPayload.DetachedVoxelMask.CellSize
		: 12.0f;

	DynamicFragmentMaterial =
		UMaterialInstanceDynamic::Create(ParentMaterial, this);
	MatterFlux::Rendering::ApplyVoxelMaterialProjection(
		*DynamicFragmentMaterial,
		MatterFlux::Rendering::ResolveVoxelMaterialProjection(
			FragmentColor,
			SpawnPayload.MaterialId,
			CellSize,
			MatterFlux::Rendering::EVoxelMaterialFaceRole::Primary),
		TransientFadeAlpha);
	MeshComponent->SetMaterial(0, DynamicFragmentMaterial);
	DynamicFragmentSideMaterial =
		UMaterialInstanceDynamic::Create(ParentMaterial, this);
	MatterFlux::Rendering::ApplyVoxelMaterialProjection(
		*DynamicFragmentSideMaterial,
		MatterFlux::Rendering::ResolveVoxelMaterialProjection(
			FragmentColor,
			SpawnPayload.MaterialId,
			CellSize,
			MatterFlux::Rendering::EVoxelMaterialFaceRole::Side),
		TransientFadeAlpha);
	MeshComponent->SetMaterial(1, DynamicFragmentSideMaterial);
}

void AFragment2DActor::ConfigureTransientFade()
{
	if (!FMath::IsFinite(SpawnPayload.FadeOutDuration)
		|| SpawnPayload.FadeOutDuration <= 0.0f)
	{
		TransientFadeElapsed = 0.0f;
		TransientFadeAlpha = 1.0f;
		SetActorTickEnabled(IsRootCombusting());
		return;
	}

	TransientFadeElapsed = 0.0f;
	TransientFadeAlpha = 1.0f;
	MeshComponent->SetSimulatePhysics(false);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetActorTickEnabled(true);
	SetLifeSpan(SpawnPayload.FadeOutDuration + 0.1f);
	ApplyTransientFadeAlpha();
}

void AFragment2DActor::SynchronizeCutFadeState()
{
	if (!FMath::IsFinite(ActiveCutFadeDuration)
		|| ActiveCutFadeDuration <= 0.0f)
	{
		return;
	}
	SpawnPayload.FadeOutDuration = ActiveCutFadeDuration;
	// Switching an already-visible rigid item to the translucent fade parent is
	// a projection rebuild only; the canonical masks and aggregate layers remain
	// untouched until the carrier retires at the end of the fade.
	RebuildMeshFromPayload();
	ConfigureTransientFade();
}

void AFragment2DActor::ApplyTransientFadeAlpha()
{
	if (DynamicFragmentMaterial)
	{
		DynamicFragmentMaterial->SetVectorParameterValue(
			TEXT("Color"),
			FLinearColor(
				FragmentColor.R,
				FragmentColor.G,
				FragmentColor.B,
				TransientFadeAlpha));
		DynamicFragmentMaterial->SetScalarParameterValue(
			TEXT("Opacity"),
			TransientFadeAlpha);
	}
	if (DynamicFragmentSideMaterial)
	{
		DynamicFragmentSideMaterial->SetVectorParameterValue(
			TEXT("Color"),
			FLinearColor(
				FragmentColor.R * 0.72f,
				FragmentColor.G * 0.72f,
				FragmentColor.B * 0.72f,
				TransientFadeAlpha));
		DynamicFragmentSideMaterial->SetScalarParameterValue(
			TEXT("Opacity"),
			TransientFadeAlpha);
	}
	for (UMaterialInstanceDynamic* Dynamic : AggregateDynamicMaterials)
	{
		if (Dynamic)
		{
			Dynamic->SetScalarParameterValue(
				TEXT("Opacity"),
				TransientFadeAlpha);
		}
	}
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
	VisualDepthOffset = bMeshValid
		? ComputeVisualDepthOffset(SpawnPayload.FragmentId)
		: 0.0f;
	if (bMeshValid)
	{
		for (FVector& Vertex : Vertices)
		{
			Vertex.Y += VisualDepthOffset;
		}
	}

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

	const bool bRenderRootAsWholeVoxel =
		SpawnPayload.DetachedVoxelMask.IsValid()
		&& SpawnPayload.DetachedVoxelMask.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks
		&& !SpawnPayload.MaterialId.IsNone();
	const int32 FaceIndexCount = SpawnPayload.TriangleIndices.Num() * 2;
	if (FaceIndexCount <= 0 || FaceIndexCount >= Triangles.Num())
	{
		MeshComponent->ClearCollisionConvexMeshes();
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return false;
	}
	if (!bRenderRootAsWholeVoxel)
	{
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
	}
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
		float CellSize = 1.0f;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FColor> VertexColors;
	};

	TMap<FString, FRenderGroup> Groups;
	TSet<FGuid> WholeRenderedSourceIds;

	// 第二个真实 Adapter：动态 aggregate 与静态 chunk proxy 调用同一
	// WholeObject 编译器。Actor 只负责承载 replicated movement 和物理，
	// 不再逐 source 重新生成一套不同的可视网格。
	struct FWholeMaterial
	{
		FString StableKey;
		TObjectPtr<UMaterialInterface> Material;
		FName MaterialId = NAME_None;
		FLinearColor Color = FLinearColor::White;
		float CellSize = 1.0f;
	};
	TArray<FWholeMaterial> WholeMaterials;
	const bool bIncludeRootVoxel =
		SpawnPayload.DetachedVoxelMask.IsValid()
		&& SpawnPayload.DetachedVoxelMask.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks
		&& !SpawnPayload.MaterialId.IsNone();
	const bool bUseRootCombustionState = bIncludeRootVoxel
		&& RootCombustionState.SourceId == SpawnPayload.FragmentId
		&& RootCombustionState.IsValid();
	const FFragmentSourceMask& RootRenderMask = bUseRootCombustionState
		? RootCombustionState.SourceMask
		: SpawnPayload.DetachedVoxelMask;
	if (bIncludeRootVoxel)
	{
		if (RootRenderMask.SolidMask.Contains(1))
		{
			WholeMaterials.Add({
				FString::Printf(
					TEXT("%s|%08x|%08x"),
					*SpawnPayload.MaterialId.ToString(),
					FragmentColor.ToFColor(false).DWColor(),
					GetTypeHash(RootRenderMask.CellSize)),
				FragmentMaterial,
				SpawnPayload.MaterialId,
				FragmentColor,
				RootRenderMask.CellSize});
		}
		if (bUseRootCombustionState
			&& RootCombustionState.bHasCombustionState
			&& RootCombustionState.ResidueMask.SolidMask.Contains(1))
		{
			const FName ResidueMaterialId =
				RootCombustionState.ResidueMaterialId.IsNone()
					? FName(TEXT("residue"))
					: RootCombustionState.ResidueMaterialId;
			WholeMaterials.Add({
				FString::Printf(
					TEXT("%s|%08x|%08x"),
					*ResidueMaterialId.ToString(),
					RootCombustionState.ResidueColor.ToFColor(false).DWColor(),
					GetTypeHash(RootRenderMask.CellSize)),
				FragmentMaterial,
				ResidueMaterialId,
				RootCombustionState.ResidueColor,
				RootRenderMask.CellSize});
		}
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.IsValid()
			|| Source.SourceMask.GeometryStyle
				!= EFragmentSourceGeometryStyle::VoxelBlocks)
		{
			continue;
		}
		const auto AddMaterial = [&WholeMaterials, &Source](
			const FName MaterialId,
			const FLinearColor& Color)
		{
			const FString StableKey = FString::Printf(
				TEXT("%s|%08x|%08x"),
				*MaterialId.ToString(),
				Color.ToFColor(false).DWColor(),
				GetTypeHash(Source.SourceMask.CellSize));
			if (!WholeMaterials.ContainsByPredicate(
				[&StableKey](const FWholeMaterial& Existing)
				{
					return Existing.StableKey == StableKey;
				}))
			{
				WholeMaterials.Add({
					StableKey,
					Source.Material,
					MaterialId,
					Color,
					Source.SourceMask.CellSize});
			}
		};
		if (Source.SourceMask.SolidMask.Contains(1))
		{
			AddMaterial(Source.MaterialId, Source.Color);
		}
		if (Source.bHasCombustionState
			&& Source.ResidueMask.SolidMask.Contains(1))
		{
			AddMaterial(
				Source.ResidueMaterialId.IsNone()
					? FName(TEXT("residue"))
					: Source.ResidueMaterialId,
				Source.ResidueColor);
		}
	}
	WholeMaterials.Sort([](const FWholeMaterial& A, const FWholeMaterial& B)
	{
		return A.StableKey < B.StableKey;
	});
	TArray<MatterFlux::WholeObject::FLayer> WholeLayers;
	if (bIncludeRootVoxel)
	{
		const auto AddRootLayer = [
			&WholeLayers,
			&WholeMaterials,
			&RootRenderMask,
			this](
				const TArray<uint8>& Mask,
				const FName MaterialId,
				const FLinearColor& Color,
				const int32 Priority)
		{
			if (!Mask.Contains(1))
			{
				return;
			}
			const FString StableKey = FString::Printf(
				TEXT("%s|%08x|%08x"),
				*MaterialId.ToString(),
				Color.ToFColor(false).DWColor(),
				GetTypeHash(RootRenderMask.CellSize));
			const int32 MaterialIndex = WholeMaterials.IndexOfByPredicate(
				[&StableKey](const FWholeMaterial& Existing)
				{
					return Existing.StableKey == StableKey;
				});
			if (MaterialIndex == INDEX_NONE)
			{
				return;
			}
			MatterFlux::WholeObject::FLayer& RootLayer =
				WholeLayers.AddDefaulted_GetRef();
			RootLayer.MaterialIndex = MaterialIndex;
			RootLayer.Priority = Priority;
			RootLayer.bEnableCollision = SpawnPayload.bEnableCollision;
			RootLayer.Width = RootRenderMask.Width;
			RootLayer.Height = RootRenderMask.Height;
			RootLayer.CellSize = RootRenderMask.CellSize;
			RootLayer.LocalTransform = FTransform(
				FQuat::Identity,
				FVector(0.0f, VisualDepthOffset, 0.0f));
			RootLayer.SolidMask = Mask;
		};
		AddRootLayer(
			RootRenderMask.SolidMask,
			SpawnPayload.MaterialId,
			FragmentColor,
			SpawnPayload.MaterialId == TEXT("leaf") ? 100 : 10);
		if (bUseRootCombustionState
			&& RootCombustionState.bHasCombustionState)
		{
			AddRootLayer(
				RootCombustionState.ResidueMask.SolidMask,
				RootCombustionState.ResidueMaterialId.IsNone()
					? FName(TEXT("residue"))
					: RootCombustionState.ResidueMaterialId,
				RootCombustionState.ResidueColor,
				200);
		}
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.IsValid()
			|| Source.SourceMask.GeometryStyle
				!= EFragmentSourceGeometryStyle::VoxelBlocks)
		{
			continue;
		}
		const auto AddLayer = [
			&WholeLayers,
			&WholeMaterials,
			&Source,
			this](
				const TArray<uint8>& Mask,
				const FName MaterialId,
				const FLinearColor& Color,
				const int32 Priority)
		{
			if (!Mask.Contains(1))
			{
				return;
			}
			const FString StableKey = FString::Printf(
				TEXT("%s|%08x|%08x"),
				*MaterialId.ToString(),
				Color.ToFColor(false).DWColor(),
				GetTypeHash(Source.SourceMask.CellSize));
			const int32 MaterialIndex = WholeMaterials.IndexOfByPredicate(
				[&StableKey](const FWholeMaterial& Existing)
				{
					return Existing.StableKey == StableKey;
				});
			if (MaterialIndex == INDEX_NONE)
			{
				return;
			}
			MatterFlux::WholeObject::FLayer& Layer =
				WholeLayers.AddDefaulted_GetRef();
			Layer.MaterialIndex = MaterialIndex;
			Layer.Priority = Priority;
			Layer.bEnableCollision = Source.bEnableCollision;
			Layer.Width = Source.SourceMask.Width;
			Layer.Height = Source.SourceMask.Height;
			Layer.CellSize = Source.SourceMask.CellSize;
			Layer.LocalTransform = Source.LocalTransform;
			Layer.LocalTransform.AddToTranslation(
				FVector(0.0f, VisualDepthOffset, 0.0f));
			Layer.SolidMask = Mask;
		};
		AddLayer(
			Source.SourceMask.SolidMask,
			Source.MaterialId,
			Source.Color,
			Source.MaterialId == TEXT("leaf") ? 100 : 10);
		if (Source.bHasCombustionState)
		{
			AddLayer(
				Source.ResidueMask.SolidMask,
				Source.ResidueMaterialId.IsNone()
					? FName(TEXT("residue"))
					: Source.ResidueMaterialId,
				Source.ResidueColor,
				200);
		}
	}
	WholeLayers.RemoveAll(
		[](const MatterFlux::WholeObject::FLayer& Layer)
		{
			return !Layer.SolidMask.Contains(1);
		});
	MatterFlux::WholeObject::FBuildResult WholeMesh;
	if (!WholeLayers.IsEmpty()
		&& MatterFlux::WholeObject::BuildMesh(WholeLayers, WholeMesh))
	{
		for (const MatterFlux::WholeObject::FMeshSection& Section
			: WholeMesh.Sections)
		{
			if (!WholeMaterials.IsValidIndex(Section.MaterialIndex))
			{
				continue;
			}
			const FWholeMaterial& WholeMaterial =
				WholeMaterials[Section.MaterialIndex];
			// 与静态 proxy 使用完全相同的面角色：顶面不属于侧面。
			// 否则树刚脱离地形就会再次出现暗色“凹顶”。
			const bool bSide = Section.FaceRole
				== MatterFlux::WholeObject::EFaceRole::Side;
			const FString Key = FString::Printf(
				TEXT("%s|%s|%08x|%08x|%08x|%08x|%d"),
				*GetPathNameSafe(WholeMaterial.Material.Get()),
				*WholeMaterial.MaterialId.ToString(),
				FPlatformMath::AsUInt(WholeMaterial.Color.R),
				FPlatformMath::AsUInt(WholeMaterial.Color.G),
				FPlatformMath::AsUInt(WholeMaterial.Color.B),
				FPlatformMath::AsUInt(WholeMaterial.Color.A),
				bSide ? 1 : 0);
			FRenderGroup& Group = Groups.FindOrAdd(Key);
			Group.Material = WholeMaterial.Material;
			Group.MaterialId = WholeMaterial.MaterialId;
			Group.Color = WholeMaterial.Color;
			Group.bSide = bSide;
			Group.CellSize = WholeMaterial.CellSize;
			const int32 VertexOffset = Group.Vertices.Num();
			Group.Vertices.Append(Section.Vertices);
			Group.Normals.Append(Section.Normals);
			Group.UVs.Append(Section.UVs);
			Group.VertexColors.Append(Section.VertexColors);
			for (const int32 TriangleIndex : Section.Triangles)
			{
				Group.Triangles.Add(TriangleIndex + VertexOffset);
			}
		}
		for (const FFragmentAggregateSourceState& Source : AggregateSources)
		{
			if (Source.SourceMask.GeometryStyle
				== EFragmentSourceGeometryStyle::VoxelBlocks)
			{
				WholeRenderedSourceIds.Add(Source.SourceId);
			}
		}
	}

	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.IsValid())
		{
			return false;
		}
		if (WholeRenderedSourceIds.Contains(Source.SourceId))
		{
			continue;
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
			int32 FaceIndexCount = 0;
			const bool bBuilt = Source.SourceMask.GeometryStyle
				== EFragmentSourceGeometryStyle::VoxelBlocks
				? MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromMask(
					*Layer.Mask,
					Source.SourceMask.Width,
					Source.SourceMask.Height,
					Source.SourceMask.CellSize,
					SourceVertices,
					SourceTriangles,
					SourceNormals,
					SourceUVs,
					FaceIndexCount)
				: MatterFlux::FragmentGeometry::BuildExtrudedMesh(
					Geometry.Vertices2D,
					Geometry.TriangleIndices,
					Geometry.OuterContours,
					Geometry.HoleContours,
					Source.SourceMask.CellSize,
					SourceVertices,
					SourceTriangles,
					SourceNormals,
					SourceUVs);
			if (!bBuilt)
			{
				return false;
			}
			if (Source.SourceMask.GeometryStyle
				!= EFragmentSourceGeometryStyle::VoxelBlocks)
			{
				FaceIndexCount = Geometry.TriangleIndices.Num() * 2;
			}
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
				Group.CellSize = Source.SourceMask.CellSize;
				const int32 VertexOffset = Group.Vertices.Num();
				Group.Vertices.Reserve(VertexOffset + SourceVertices.Num());
				Group.Normals.Reserve(VertexOffset + SourceNormals.Num());
				Group.UVs.Append(SourceUVs);
				Group.VertexColors.AddDefaulted(SourceVertices.Num());
				for (int32 ColorIndex =
					Group.VertexColors.Num() - SourceVertices.Num();
					ColorIndex < Group.VertexColors.Num();
					++ColorIndex)
				{
					Group.VertexColors[ColorIndex] = FColor::White;
				}
				for (int32 Index = 0; Index < SourceVertices.Num(); ++Index)
				{
					FVector RenderPosition =
						Source.LocalTransform.TransformPosition(
							SourceVertices[Index]);
					RenderPosition.Y += VisualDepthOffset;
					Group.Vertices.Add(RenderPosition);
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
	int32 SectionIndex = bIncludeRootVoxel ? 0 : 2;
	for (const FString& Key : Keys)
	{
		FRenderGroup& Group = Groups.FindChecked(Key);
		MeshComponent->CreateMeshSection(
			SectionIndex,
			Group.Vertices,
			Group.Triangles,
			Group.Normals,
			Group.UVs,
			Group.VertexColors,
			TArray<FProcMeshTangent>(),
			false);
		UMaterialInterface* Parent = SpawnPayload.FadeOutDuration > 0.0f
			&& TransientFadeMaterial
			? TransientFadeMaterial.Get()
			: Group.Material
				? Group.Material.Get()
				: FragmentMaterial.Get();
		if (Parent)
		{
			UMaterialInstanceDynamic* Dynamic =
				UMaterialInstanceDynamic::Create(Parent, this);
			MatterFlux::Rendering::ApplyVoxelMaterialProjection(
				*Dynamic,
				MatterFlux::Rendering::ResolveVoxelMaterialProjection(
					Group.Color,
					Group.MaterialId,
					Group.CellSize,
					Group.bSide
						? MatterFlux::Rendering::EVoxelMaterialFaceRole::Side
						: MatterFlux::Rendering::EVoxelMaterialFaceRole::Primary),
				TransientFadeAlpha);
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
			if (Source.bOwnsLogicalSource)
			{
				It->NotifyDynamicAggregateOwnsSource(Source.SourceId, this);
			}
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
