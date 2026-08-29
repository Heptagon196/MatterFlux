#include "Fragment/Fragment2DActor.h"

#include "Fragment/FragmentGeometry.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "Material/MatterFluxLocalMaterialReaction.h"
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
	constexpr float FragmentInitialOverlapMaxDepenetrationSpeed = 400.0f;

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

	double ComputeClosestSolidCellDistanceSquared(
		const FFragmentSourceMask& Mask,
		const FTransform& WorldTransform,
		const FVector& WorldLocation)
	{
		if (!Mask.IsValid()
			|| !WorldTransform.IsValid()
			|| WorldLocation.ContainsNaN())
		{
			return TNumericLimits<double>::Max();
		}
		double BestDistanceSquared = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Mask.SolidMask.Num(); ++Index)
		{
			if (Mask.SolidMask[Index] == 0)
			{
				continue;
			}
			const int32 X = Index % Mask.Width;
			const int32 Z = Index / Mask.Width;
			const FVector LocalCellCenter(
				(static_cast<float>(X) + 0.5f
					- static_cast<float>(Mask.Width) * 0.5f)
					* Mask.CellSize,
				0.0f,
				(static_cast<float>(Z) + 0.5f
					- static_cast<float>(Mask.Height) * 0.5f)
					* Mask.CellSize);
			BestDistanceSquared = FMath::Min(
				BestDistanceSquared,
				FVector::DistSquared(
					WorldTransform.TransformPosition(LocalCellCenter),
					WorldLocation));
		}
		return BestDistanceSquared;
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

	bool HasNonEnvironmentEnergy(
		const FFragmentAggregateSourceState& State)
	{
		return State.VolumeCellStates.ContainsByPredicate(
			[&State](const FFragmentCarrierVolumeCellState& Cell)
			{
				return Cell.Energy != State.VolumeEnvironmentEnergy;
			});
	}

	int32 CountMaterialOverrideCells(
		const FFragmentAggregateSourceState& State)
	{
		int32 Count = 0;
		for (const FFragmentCarrierVolumeCellState& Cell
			: State.VolumeCellStates)
		{
			Count += Cell.MaterialId != State.MaterialId ? 1 : 0;
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
	TSet<FIntVector> SeenVolumeCells;
	const bool bVolumeStateValid = VolumeTopologyRevision >= 0
		&& VolumeFieldRevision >= 0
		&& !VolumeCellStates.ContainsByPredicate(
			[this, &SeenVolumeCells](
				const FFragmentCarrierVolumeCellState& CellState)
			{
				const int32 Index = CellState.Cell.Y * SourceMask.Width
					+ CellState.Cell.X;
				const bool bInvalid = CellState.Cell.Z != 0
					|| CellState.Cell.X < 0 || CellState.Cell.Y < 0
					|| CellState.Cell.X >= SourceMask.Width
					|| CellState.Cell.Y >= SourceMask.Height
					|| !SourceMask.SolidMask.IsValidIndex(Index)
					|| SourceMask.SolidMask[Index] == 0
					|| CellState.MaterialId.IsNone()
					|| CellState.MaterialId == TEXT("empty")
					|| SeenVolumeCells.Contains(CellState.Cell);
				SeenVolumeCells.Add(CellState.Cell);
				return bInvalid;
			});
	return bVolumeStateValid
		&& SourceId.IsValid()
		&& (DefinitionSourceId.IsValid() || bOwnsLogicalSource)
		&& Revision >= 0
		&& SourceMask.HasValidLayout()
		&& SourceMask.SolidMask.Contains(1)
		&& LocalTransform.IsValid()
		&& !MaterialId.IsNone()
		&& FMath::IsFinite(Color.R)
		&& FMath::IsFinite(Color.G)
		&& FMath::IsFinite(Color.B)
		&& FMath::IsFinite(Color.A);
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
	// Detached bodies must participate in Chaos contacts with one another. Ignoring
	// PhysicsBody lets independently cut items be pushed into the same space.
	MeshComponent->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	// Detached material is a movable body, not a terrain step. Allowing
	// CharacterMovement to StepUp makes tall creatures climb over a voxel hull
	// after one contact frame instead of continuously pushing it sideways.
	MeshComponent->CanCharacterStepUpOn = ECB_No;
	MeshComponent->SetNotifyRigidBodyCollision(true);
	MeshComponent->bUseComplexAsSimpleCollision = false;
	MeshComponent->SetLinearDamping(1.25f);
	MeshComponent->SetAngularDamping(4.0f);
	MeshComponent->BodyInstance.bUseCCD = true;
	// If terrain streaming or a cut briefly creates an initial overlap, Chaos
	// may otherwise convert the entire penetration depth into a single-frame
	// launch. This cap affects only initial/teleport overlap correction, not
	// normal collision response, forces, impulses, or character pushing.
	MeshComponent->SetMaxDepenetrationVelocity(
		NAME_None,
		FragmentInitialOverlapMaxDepenetrationSpeed);
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
	if (SpawnPayload.FadeOutDuration <= 0.0f)
	{
		SetActorTickEnabled(false);
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
	InitializeRootMaterialState();
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
	DOREPLIFETIME(AFragment2DActor, RootMaterialState);
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
	InitializeRootMaterialState();
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
	if (RootMaterialState.SourceId == SpawnPayload.FragmentId)
	{
		RootMaterialState.Material = FragmentMaterial;
		RootMaterialState.Color = FragmentColor;
	}
	RebuildMeshFromPayload();
}

void AFragment2DActor::OnRep_AggregateSources()
{
	RefreshBuoyancyDensity();
	RebuildMeshFromPayload();
	NotifyWorldOfAggregateSources();
	MarkReactionVisualizationDirty();
}

void AFragment2DActor::OnRep_RootMaterialState()
{
	RebuildMeshFromPayload();
	SetActorTickEnabled(SpawnPayload.FadeOutDuration > 0.0f);
	MarkReactionVisualizationDirty();
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
	if (RootMaterialState.IsValid())
	{
		RootMask = &RootMaterialState.SourceMask;
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
		|| !DoesCutShapeIntersect(CutShape))
	{
		return false;
	}

	const bool bWasSimulating = MeshComponent->IsSimulatingPhysics();
	const FTransform PreservedBodyTransform =
		MeshComponent->GetComponentTransform();
	const FVector PreservedLinearVelocity = bWasSimulating
		? MeshComponent->GetPhysicsLinearVelocity()
		: SpawnPayload.InitialLinearVelocity;
	const FVector PreservedAngularVelocity = bWasSimulating
		? MeshComponent->GetPhysicsAngularVelocityInDegrees()
		: SpawnPayload.InitialAngularVelocity;

	const FFragmentAggregateSourceState PreviousRootState =
		RootMaterialState;
	const TArray<FFragmentAggregateSourceState> PreviousAggregateSources =
		AggregateSources;
	const FFragmentSourceMask PreviousDetachedMask =
		SpawnPayload.DetachedVoxelMask;

	const auto ApplyCutToMask = [&CutShape](
		FFragmentSourceMask& Mask,
		const FTransform& MaskWorldTransform)
	{
		if (!Mask.IsValid() || !MaskWorldTransform.IsValid())
		{
			return false;
		}
		FFragmentDamageShape LocalShape = CutShape;
		LocalShape.WorldTransform = CutShape.WorldTransform.GetRelativeTransform(
			MaskWorldTransform);
		return MatterFlux::FragmentGeometry::ApplyDamageShape(
			Mask.SolidMask,
			Mask.Width,
			Mask.Height,
			Mask.CellSize,
			LocalShape);
	};
	const auto ApplyCutToState = [&ApplyCutToMask](
		FFragmentAggregateSourceState& State,
		const FTransform& StateWorldTransform)
	{
		if (!State.IsValid()
			|| !ApplyCutToMask(State.SourceMask, StateWorldTransform))
		{
			return false;
		}
		State.VolumeCellStates.RemoveAll(
			[&State](const FFragmentCarrierVolumeCellState& Cell)
			{
				const int32 Index = Cell.Cell.Y * State.SourceMask.Width
					+ Cell.Cell.X;
				return Cell.Cell.Z != 0
					|| !State.SourceMask.SolidMask.IsValidIndex(Index)
					|| State.SourceMask.SolidMask[Index] == 0;
			});
		++State.Revision;
		++State.VolumeTopologyRevision;
		return true;
	};

	bool bChanged = false;
	if (RootMaterialState.IsValid())
	{
		bChanged |= ApplyCutToState(
			RootMaterialState,
			GetActorTransform());
	}
	else if (SpawnPayload.DetachedVoxelMask.IsValid())
	{
		bChanged |= ApplyCutToMask(
			SpawnPayload.DetachedVoxelMask,
			GetActorTransform());
	}
	for (FFragmentAggregateSourceState& Source : AggregateSources)
	{
		bChanged |= ApplyCutToState(
			Source,
			Source.LocalTransform * GetActorTransform());
	}
	if (!bChanged)
	{
		// Legacy triangulated debris has no retained material mask to edit.
		// It must still react on the first exact hit instead of silently
		// accumulating invisible durability counters.
		if (!RootMaterialState.SourceMask.HasValidLayout()
			&& !SpawnPayload.DetachedVoxelMask.HasValidLayout()
			&& AggregateSources.IsEmpty())
		{
			AcceptedCutCount = FMath::Min(AcceptedCutCount + 1, MAX_int32);
			ActiveCutFadeDuration = FMath::Max(
				CutExhaustionFadeDuration,
				0.05f);
			SynchronizeCutFadeState();
			ForceNetUpdate();
			return true;
		}
		return false;
	}

	const auto StateHasMaterial = [](
		const FFragmentAggregateSourceState& State)
	{
		return State.SourceMask.SolidMask.Contains(1);
	};
	const bool bHasRemainingMaterial =
		StateHasMaterial(RootMaterialState)
		|| AggregateSources.ContainsByPredicate(StateHasMaterial)
		|| (!RootMaterialState.SourceMask.HasValidLayout()
			&& SpawnPayload.DetachedVoxelMask.SolidMask.Contains(1));
	if (!bHasRemainingMaterial)
	{
		AcceptedCutCount = FMath::Min(AcceptedCutCount + 1, MAX_int32);
		MeshComponent->SetSimulatePhysics(false);
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ForceNetUpdate();
		Destroy();
		return true;
	}

	bool bSplitIntoIndependentActors = false;
	if (!TrySplitDisconnectedRootMaterial(
		PreservedBodyTransform,
		PreservedLinearVelocity,
		PreservedAngularVelocity,
		FMath::Min(AcceptedCutCount + 1, MAX_int32),
		bSplitIntoIndependentActors))
	{
		RootMaterialState = PreviousRootState;
		AggregateSources = PreviousAggregateSources;
		SpawnPayload.DetachedVoxelMask = PreviousDetachedMask;
		RefreshBuoyancyDensity();
		RebuildMeshFromPayload();
		return false;
	}
	if (bSplitIntoIndependentActors)
	{
		return true;
	}

	RefreshBuoyancyDensity();
	if (!RebuildMeshFromPayload())
	{
		RootMaterialState = PreviousRootState;
		AggregateSources = PreviousAggregateSources;
		SpawnPayload.DetachedVoxelMask = PreviousDetachedMask;
		RefreshBuoyancyDensity();
		RebuildMeshFromPayload();
		return false;
	}

	AcceptedCutCount = FMath::Min(AcceptedCutCount + 1, MAX_int32);
	if (bWasSimulating
		&& MeshComponent->GetCollisionEnabled()
			!= ECollisionEnabled::NoCollision)
	{
		MeshComponent->SetWorldTransform(
			PreservedBodyTransform,
			false,
			nullptr,
			ETeleportType::TeleportPhysics);
		if (!MeshComponent->IsSimulatingPhysics())
		{
			MeshComponent->SetSimulatePhysics(true);
		}
		MeshComponent->SetPhysicsLinearVelocity(PreservedLinearVelocity);
		MeshComponent->SetPhysicsAngularVelocityInDegrees(
			PreservedAngularVelocity);
	}
	ForceNetUpdate();
	return true;
}

bool AFragment2DActor::TrySplitDisconnectedRootMaterial(
	const FTransform& ParentWorldTransform,
	const FVector& PreservedLinearVelocity,
	const FVector& PreservedAngularVelocity,
	const int32 NextAcceptedCutCount,
	bool& bOutSplit)
{
	bOutSplit = false;
	if (!AggregateSources.IsEmpty()
		|| !RootMaterialState.IsValid()
		|| !ParentWorldTransform.IsValid())
	{
		return true;
	}

	const FFragmentSourceMask& RootMask = RootMaterialState.SourceMask;
	TArray<MatterFlux::FragmentGeometry::FFragmentComponent> Components;
	MatterFlux::FragmentGeometry::ExtractConnectedComponents(
		RootMask.SolidMask,
		RootMask.Width,
		RootMask.Height,
		Components);
	if (Components.Num() <= 1)
	{
		return true;
	}

	const FGuid SplitSourceId = RootMaterialState.SourceId.IsValid()
		? RootMaterialState.SourceId
		: SpawnPayload.FragmentId;
	const int32 SplitSeed = static_cast<int32>(HashCombineFast(
		GetTypeHash(SplitSourceId),
		static_cast<uint32>(NextAcceptedCutCount)));
	TArray<FFragmentSpawnPayload> ChildPayloads;
	if (!MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
		Components,
		SplitSourceId,
		ParentWorldTransform,
		RootMask.Width,
		RootMask.Height,
		RootMaterialState.Revision,
		RootMask.CellSize,
		RootMask.MinFragmentAreaPixels,
		RootMask.MaxFragmentsPerBreak,
		ParentWorldTransform.GetLocation(),
		0.0f,
		SplitSeed,
		ChildPayloads,
		RootMask.GeometryStyle))
	{
		return false;
	}
	if (ChildPayloads.Num() <= 1)
	{
		return true;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	TArray<AFragment2DActor*> SpawnedChildren;
	SpawnedChildren.Reserve(ChildPayloads.Num());
	const auto RollBackChildren = [&SpawnedChildren, World]()
	{
		for (AFragment2DActor* Child : SpawnedChildren)
		{
			if (IsValid(Child))
			{
				World->DestroyActor(Child);
			}
		}
		SpawnedChildren.Reset();
	};

	for (FFragmentSpawnPayload& Payload : ChildPayloads)
	{
		Payload.MaterialId = RootMaterialState.MaterialId.IsNone()
			? SpawnPayload.MaterialId
			: RootMaterialState.MaterialId;
		Payload.bEnableCollision = RootMaterialState.bEnableCollision;
		Payload.FadeOutDuration = 0.0f;
		Payload.InitialLinearVelocity = PreservedLinearVelocity;
		Payload.InitialAngularVelocity = PreservedAngularVelocity;

		AFragment2DActor* Child =
			World->SpawnActorDeferred<AFragment2DActor>(
				GetClass(),
				Payload.InitialTransform,
				GetOwner(),
				GetInstigator(),
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Child)
		{
			RollBackChildren();
			return false;
		}
		Child->SpawnPayload = Payload;
		Child->FragmentMaterial = FragmentMaterial;
		Child->FragmentColor = FragmentColor;
		Child->CutsBeforeFade = CutsBeforeFade;
		Child->CutExhaustionFadeDuration = CutExhaustionFadeDuration;
		Child->Tags = Tags;
		Child->Tags.AddUnique(TEXT("MatterFluxFragment"));
		Child->FinishSpawning(Payload.InitialTransform);
		SpawnedChildren.Add(Child);
		if (!IsValid(Child) || !Child->InitializeFromPayload(Payload))
		{
			RollBackChildren();
			return false;
		}

		FFragmentAggregateSourceState ChildState = RootMaterialState;
		ChildState.SourceId = Payload.FragmentId;
		ChildState.bOwnsLogicalSource = false;
		ChildState.LocalTransform = FTransform::Identity;
		ChildState.SourceMask = Payload.DetachedVoxelMask;
		ChildState.VolumeTopologyRevision =
			RootMaterialState.VolumeTopologyRevision + 1;
		const FTransform ChildToParent =
			Payload.InitialTransform.GetRelativeTransform(
				ParentWorldTransform);
		ChildState.VolumeCellStates.Reset();
		for (int32 ChildY = 0;
			ChildY < ChildState.SourceMask.Height;
			++ChildY)
		{
			for (int32 ChildX = 0;
				ChildX < ChildState.SourceMask.Width;
				++ChildX)
			{
				const int32 ChildIndex = ChildY
					* ChildState.SourceMask.Width + ChildX;
				if (ChildState.SourceMask.SolidMask[ChildIndex] == 0)
				{
					continue;
				}
				const FVector ChildCellCenter(
					(static_cast<float>(ChildX) + 0.5f
						- ChildState.SourceMask.Width * 0.5f)
						* ChildState.SourceMask.CellSize,
					0.0f,
					(static_cast<float>(ChildY) + 0.5f
						- ChildState.SourceMask.Height * 0.5f)
						* ChildState.SourceMask.CellSize);
				const FVector ParentCellCenter =
					ChildToParent.TransformPosition(ChildCellCenter);
				const FIntVector ParentCell(
					FMath::RoundToInt(
						ParentCellCenter.X / RootMask.CellSize
							+ RootMask.Width * 0.5f - 0.5f),
					FMath::RoundToInt(
						ParentCellCenter.Z / RootMask.CellSize
							+ RootMask.Height * 0.5f - 0.5f),
					0);
				if (const FFragmentCarrierVolumeCellState* ParentState =
					RootMaterialState.VolumeCellStates.FindByPredicate(
						[&ParentCell](
							const FFragmentCarrierVolumeCellState& Cell)
						{
							return Cell.Cell == ParentCell;
						}))
				{
					ChildState.VolumeCellStates.Add({
						FIntVector(ChildX, ChildY, 0),
						ParentState->MaterialId,
						ParentState->Energy});
				}
			}
		}

		Child->RootMaterialState = MoveTemp(ChildState);
		Child->AcceptedCutCount = NextAcceptedCutCount;
		Child->RefreshBuoyancyDensity();
		if (!Child->RebuildMeshFromPayload())
		{
			RollBackChildren();
			return false;
		}
		Child->SetActorTickEnabled(
			Child->SpawnPayload.FadeOutDuration > 0.0f);
		Child->ForceNetUpdate();
	}

	MeshComponent->SetSimulatePhysics(false);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bOutSplit = true;
	Destroy();
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

bool AFragment2DActor::TryGetMaterialVolumeElementAtWorldLocation(
	const FVector& WorldLocation,
	const FLocalMaterialReactionProgram& Program,
	FMaterialElementAddress& OutAddress,
	FMaterialElementState& OutState,
	uint16& OutDefaultEnergy,
	FVector& OutWorldCellCenter) const
{
	OutAddress = FMaterialElementAddress();
	OutState = FMaterialElementState();
	OutDefaultEnergy = 0;
	OutWorldCellCenter = FVector::ZeroVector;
	if (WorldLocation.ContainsNaN() || !Program.IsCompiled())
	{
		return false;
	}

	struct FCandidate
	{
		const FFragmentAggregateSourceState* Source = nullptr;
		FIntVector Cell = FIntVector::ZeroValue;
		FVector WorldCenter = FVector::ZeroVector;
		double DistanceSquared = TNumericLimits<double>::Max();
		bool bRoot = false;
	};
	FCandidate Best;
	const auto ConsiderSource = [this, &WorldLocation, &Best](
		const FFragmentAggregateSourceState& Source,
		const FTransform& WorldTransform,
		const bool bRoot)
	{
		if (!Source.IsValid() || !WorldTransform.IsValid())
		{
			return;
		}
		for (int32 Index = 0; Index < Source.SourceMask.SolidMask.Num(); ++Index)
		{
			if (Source.SourceMask.SolidMask[Index] == 0)
			{
				continue;
			}
			const int32 U = Index % Source.SourceMask.Width;
			const int32 V = Index / Source.SourceMask.Width;
			const FVector LocalCenter(
				(static_cast<double>(U) + 0.5
					- static_cast<double>(Source.SourceMask.Width) * 0.5)
					* Source.SourceMask.CellSize,
				0.0,
				(static_cast<double>(V) + 0.5
					- static_cast<double>(Source.SourceMask.Height) * 0.5)
					* Source.SourceMask.CellSize);
			const FVector WorldCenter =
				WorldTransform.TransformPosition(LocalCenter);
			const double DistanceSquared =
				FVector::DistSquared(WorldCenter, WorldLocation);
			const bool bStableTie = FMath::IsNearlyEqual(
				DistanceSquared, Best.DistanceSquared)
				&& (bRoot != Best.bRoot
					? bRoot
					: Source.SourceId.ToString(EGuidFormats::Digits)
						< (Best.Source
							? Best.Source->SourceId.ToString(EGuidFormats::Digits)
							: FString()));
			if (DistanceSquared < Best.DistanceSquared || bStableTie)
			{
				Best.Source = &Source;
				Best.Cell = FIntVector(U, V, 0);
				Best.WorldCenter = WorldCenter;
				Best.DistanceSquared = DistanceSquared;
				Best.bRoot = bRoot;
			}
		}
	};
	if (RootMaterialState.IsValid())
	{
		ConsiderSource(RootMaterialState, GetActorTransform(), true);
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		ConsiderSource(
			Source, Source.LocalTransform * GetActorTransform(), false);
	}
	if (!Best.Source)
	{
		return false;
	}

	FName MaterialId = Best.Source->MaterialId;
	const FFragmentCarrierVolumeCellState* Override =
		Best.Source->VolumeCellStates.FindByPredicate(
			[&Best](const FFragmentCarrierVolumeCellState& CellState)
			{
				return CellState.Cell == Best.Cell;
			});
	if (Override)
	{
		MaterialId = Override->MaterialId;
	}
	if (!Program.MakeState(
			MaterialId, 255, TOptional<uint16>(), OutState))
	{
		return false;
	}
	OutDefaultEnergy = OutState.Energy;
	if (Override)
	{
		OutState.Energy = Override->Energy;
	}
	else if (Best.Source->VolumeFieldRevision > 0
		|| Best.Source->VolumeEnvironmentEnergy != 0)
	{
		OutState.Energy = Best.Source->VolumeEnvironmentEnergy;
	}
	OutAddress = FMaterialElementAddress::MakeVolumeCell(
		Best.Source->SourceId, Best.Cell);
	OutWorldCellCenter = Best.WorldCenter;
	return OutAddress.IsValid() && OutState.IsValid();
}

bool AFragment2DActor::CommitMaterialVolumeCellState(
	const FMaterialElementAddress& Address,
	const uint16 DefaultEnergy,
	const FMaterialElementState& ExpectedBefore,
	const FMaterialElementState& After,
	const FLocalMaterialReactionProgram& Program,
	FString& OutError)
{
	OutError.Reset();
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| Address.Kind != EMaterialElementAddressKind::VolumeCell
		|| !Address.OwnerId.IsValid()
		|| After.Amount != ExpectedBefore.Amount
		|| ExpectedBefore.Amount != 255
		|| After.MaterialIndex == 0)
	{
		OutError = TEXT("carrier Volume cell state base is invalid");
		return false;
	}
	const FFragmentAggregateSourceState* Target =
		RootMaterialState.SourceId == Address.OwnerId
			? &RootMaterialState
			: AggregateSources.FindByPredicate(
				[&Address](const FFragmentAggregateSourceState& Candidate)
				{
					return Candidate.SourceId == Address.OwnerId;
				});
	if (!Target || !Target->IsValid())
	{
		OutError = TEXT("carrier Volume address is unavailable");
		return false;
	}
	const FFragmentCarrierVolumeCellState* ExistingOverride =
		Target->VolumeCellStates.FindByPredicate(
			[&Address](const FFragmentCarrierVolumeCellState& CellState)
			{
				return CellState.Cell == Address.Cell;
			});
	const FName CurrentMaterialId = ExistingOverride
		? ExistingOverride->MaterialId : Target->MaterialId;
	FMaterialElementState Canonical;
	if (!Program.MakeState(
		CurrentMaterialId, 255, TOptional<uint16>(), Canonical))
	{
		OutError = TEXT("carrier Volume material is not in the local program");
		return false;
	}
	if (DefaultEnergy != Canonical.Energy)
	{
		OutError = TEXT("carrier Volume default energy does not match the program");
		return false;
	}
	FMaterialDeltaBatch Batch;
	Batch.BaseStoreRevision = 0;
	Batch.TargetStoreRevision = After == ExpectedBefore ? 0 : 1;
	Batch.ExplicitAmountDelta =
		static_cast<int64>(After.Amount) - ExpectedBefore.Amount;
	Batch.ExplicitEnergyDelta =
		After.GetTotalEnergy() - ExpectedBefore.GetTotalEnergy();
	if (!(After == ExpectedBefore))
	{
		Batch.ElementDeltas.Add({Address, ExpectedBefore, After});
	}
	return CommitMaterialVolumeElementBatch(Batch, Program, OutError);
}

bool AFragment2DActor::CommitMaterialVolumeElementBatch(
	const FMaterialDeltaBatch& Batch,
	const FLocalMaterialReactionProgram& Program,
	FString& OutError)
{
	OutError.Reset();
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| !Program.IsCompiled() || !Batch.IsValid(OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("carrier Volume batch cannot be committed");
		}
		return false;
	}

	FFragmentAggregateSourceState CandidateRoot = RootMaterialState;
	TArray<FFragmentAggregateSourceState> CandidateAggregates = AggregateSources;
	TSet<FGuid> TopologyChanged;
	TSet<FGuid> FieldsChanged;
	TSet<FGuid> TouchedSources;
	const auto FindCandidate = [
		&CandidateRoot,
		&CandidateAggregates](const FGuid& SourceId)
		-> FFragmentAggregateSourceState*
	{
		if (CandidateRoot.SourceId == SourceId)
		{
			return &CandidateRoot;
		}
		return CandidateAggregates.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	};
	for (const FMaterialElementDelta& Delta : Batch.ElementDeltas)
	{
		if (Delta.Address.Kind != EMaterialElementAddressKind::VolumeCell
			|| Delta.Address.Cell.Z != 0
			|| Delta.ExpectedBefore.Amount != 255
			|| Delta.After.Amount != 255
			|| Delta.After.MaterialIndex == 0)
		{
			OutError = TEXT("carrier Volume batch contains an unsupported delta");
			return false;
		}
		FFragmentAggregateSourceState* Target =
			FindCandidate(Delta.Address.OwnerId);
		if (!Target || !Target->IsValid()
			|| Delta.Address.Cell.X < 0 || Delta.Address.Cell.Y < 0
			|| Delta.Address.Cell.X >= Target->SourceMask.Width
			|| Delta.Address.Cell.Y >= Target->SourceMask.Height)
		{
			OutError = TEXT("carrier Volume batch references an unavailable layer");
			return false;
		}
		const int32 CellIndex = Delta.Address.Cell.Y * Target->SourceMask.Width
			+ Delta.Address.Cell.X;
		if (!Target->SourceMask.SolidMask.IsValidIndex(CellIndex)
			|| Target->SourceMask.SolidMask[CellIndex] == 0)
		{
			OutError = TEXT("carrier Volume batch references an empty cell");
			return false;
		}

		FFragmentCarrierVolumeCellState* ExistingOverride =
			Target->VolumeCellStates.FindByPredicate(
				[&Delta](const FFragmentCarrierVolumeCellState& CellState)
				{
					return CellState.Cell == Delta.Address.Cell;
				});
		const FName CurrentMaterialId = ExistingOverride
			? ExistingOverride->MaterialId : Target->MaterialId;
		FMaterialElementState Current;
		if (!Program.MakeState(
				CurrentMaterialId, 255, TOptional<uint16>(), Current))
		{
			OutError = TEXT("carrier Volume batch material is not compiled");
			return false;
		}
		if (ExistingOverride)
		{
			Current.Energy = ExistingOverride->Energy;
		}
		else if (Target->VolumeFieldRevision > 0
			|| Target->VolumeEnvironmentEnergy != 0)
		{
			Current.Energy = Target->VolumeEnvironmentEnergy;
		}
		if (!(Current == Delta.ExpectedBefore))
		{
			OutError = TEXT("carrier Volume batch base is stale");
			return false;
		}

		FName AfterMaterialId = NAME_None;
		if (!Program.TryGetMaterialId(
				Delta.After.MaterialIndex, AfterMaterialId)
			|| AfterMaterialId.IsNone() || AfterMaterialId == TEXT("empty"))
		{
			OutError = TEXT("carrier Volume batch output material is invalid");
			return false;
		}
		FMaterialElementState BaseState;
		if (!Program.MakeState(
				Target->MaterialId, 255, TOptional<uint16>(), BaseState))
		{
			OutError = TEXT("carrier Volume base material is not compiled");
			return false;
		}
		if (Target->VolumeFieldRevision == 0
			&& Target->VolumeEnvironmentEnergy == 0)
		{
			Target->VolumeEnvironmentEnergy = BaseState.Energy;
		}
		const uint16 EnvironmentEnergy = Target->VolumeFieldRevision > 0
			|| Target->VolumeEnvironmentEnergy != 0
			? Target->VolumeEnvironmentEnergy : BaseState.Energy;
		Target->VolumeCellStates.RemoveAll(
			[&Delta](const FFragmentCarrierVolumeCellState& CellState)
			{
				return CellState.Cell == Delta.Address.Cell;
			});
		if (AfterMaterialId != Target->MaterialId
			|| Delta.After.Energy != EnvironmentEnergy)
		{
			Target->VolumeCellStates.Add({
				Delta.Address.Cell, AfterMaterialId, Delta.After.Energy});
		}
		TouchedSources.Add(Target->SourceId);
		if (Delta.After.MaterialIndex != Delta.ExpectedBefore.MaterialIndex)
		{
			TopologyChanged.Add(Target->SourceId);
		}
		if (Delta.After.Energy != Delta.ExpectedBefore.Energy)
		{
			FieldsChanged.Add(Target->SourceId);
		}
	}

	for (const FGuid& SourceId : TouchedSources)
	{
		FFragmentAggregateSourceState* Target = FindCandidate(SourceId);
		check(Target);
		if (TopologyChanged.Contains(SourceId))
		{
			++Target->VolumeTopologyRevision;
		}
		if (FieldsChanged.Contains(SourceId))
		{
			++Target->VolumeFieldRevision;
		}
		Target->VolumeCellStates.Sort([](
			const FFragmentCarrierVolumeCellState& Left,
			const FFragmentCarrierVolumeCellState& Right)
		{
			return Left.Cell.X != Right.Cell.X
				? Left.Cell.X < Right.Cell.X
				: Left.Cell.Y != Right.Cell.Y
					? Left.Cell.Y < Right.Cell.Y
					: Left.Cell.Z < Right.Cell.Z;
		});
		if (!Target->IsValid())
		{
			OutError = TEXT("carrier Volume batch produced invalid state");
			return false;
		}
	}

	if (Batch.ElementDeltas.IsEmpty())
	{
		return true;
	}
	const FFragmentAggregateSourceState PreviousRoot = RootMaterialState;
	TArray<FFragmentAggregateSourceState> PreviousAggregates = AggregateSources;
	RootMaterialState = MoveTemp(CandidateRoot);
	AggregateSources = MoveTemp(CandidateAggregates);
	if (!RebuildMeshFromPayload())
	{
		RootMaterialState = PreviousRoot;
		AggregateSources = MoveTemp(PreviousAggregates);
		RebuildMeshFromPayload();
		OutError = TEXT("carrier Volume batch mesh rebuild failed");
		return false;
	}
	RefreshBuoyancyDensity();
	ForceNetUpdate();
	MarkReactionVisualizationDirty();
	return true;
}

bool AFragment2DActor::CommitMaterialVolumePairBatch(
	AFragment2DActor& CarrierA,
	AFragment2DActor& CarrierB,
	const FMaterialDeltaBatch& Batch,
	const FLocalMaterialReactionProgram& Program,
	FString& OutError)
{
	OutError.Reset();
	if (&CarrierA == &CarrierB
		|| !Batch.IsValid(OutError)
		|| !Program.IsCompiled()
		|| (CarrierA.GetWorld() && CarrierA.GetWorld()->IsGameWorld()
			&& !CarrierA.HasAuthority())
		|| (CarrierB.GetWorld() && CarrierB.GetWorld()->IsGameWorld()
			&& !CarrierB.HasAuthority()))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("cross-carrier Volume batch is invalid");
		}
		return false;
	}
	const auto OwnsAddress = [](const AFragment2DActor& Carrier,
		const FMaterialElementAddress& Address)
	{
		return Address.Kind == EMaterialElementAddressKind::VolumeCell
			&& (Carrier.RootMaterialState.SourceId == Address.OwnerId
				|| Carrier.AggregateSources.ContainsByPredicate(
					[&Address](const FFragmentAggregateSourceState& Source)
					{
						return Source.SourceId == Address.OwnerId;
					}));
	};
	FMaterialDeltaBatch BatchA;
	FMaterialDeltaBatch BatchB;
	BatchA.BaseStoreRevision = Batch.BaseStoreRevision;
	BatchB.BaseStoreRevision = Batch.BaseStoreRevision;
	for (const FMaterialElementDelta& Delta : Batch.ElementDeltas)
	{
		const bool bOwnedByA = OwnsAddress(CarrierA, Delta.Address);
		const bool bOwnedByB = OwnsAddress(CarrierB, Delta.Address);
		if (bOwnedByA == bOwnedByB)
		{
			OutError = TEXT("cross-carrier batch address ownership is ambiguous");
			return false;
		}
		(bOwnedByA ? BatchA.ElementDeltas : BatchB.ElementDeltas).Add(Delta);
	}
	const auto FinishLocalBatch = [](FMaterialDeltaBatch& LocalBatch)
	{
		int64 BeforeAmount = 0;
		int64 AfterAmount = 0;
		int64 BeforeEnergy = 0;
		int64 AfterEnergy = 0;
		for (const FMaterialElementDelta& Delta : LocalBatch.ElementDeltas)
		{
			BeforeAmount += Delta.ExpectedBefore.Amount;
			AfterAmount += Delta.After.Amount;
			BeforeEnergy += Delta.ExpectedBefore.GetTotalEnergy();
			AfterEnergy += Delta.After.GetTotalEnergy();
		}
		LocalBatch.ExplicitAmountDelta = AfterAmount - BeforeAmount;
		LocalBatch.ExplicitEnergyDelta = AfterEnergy - BeforeEnergy;
		LocalBatch.TargetStoreRevision = LocalBatch.ElementDeltas.IsEmpty()
			? LocalBatch.BaseStoreRevision
			: LocalBatch.BaseStoreRevision + 1;
	};
	FinishLocalBatch(BatchA);
	FinishLocalBatch(BatchB);

	const FFragmentAggregateSourceState PreviousRootA =
		CarrierA.RootMaterialState;
	const FFragmentAggregateSourceState PreviousRootB =
		CarrierB.RootMaterialState;
	const TArray<FFragmentAggregateSourceState> PreviousAggregatesA =
		CarrierA.AggregateSources;
	const TArray<FFragmentAggregateSourceState> PreviousAggregatesB =
		CarrierB.AggregateSources;
	if (!CarrierA.CommitMaterialVolumeElementBatch(
			BatchA, Program, OutError)
		|| !CarrierB.CommitMaterialVolumeElementBatch(
			BatchB, Program, OutError))
	{
		CarrierA.RootMaterialState = PreviousRootA;
		CarrierB.RootMaterialState = PreviousRootB;
		CarrierA.AggregateSources = PreviousAggregatesA;
		CarrierB.AggregateSources = PreviousAggregatesB;
		CarrierA.RebuildMeshFromPayload();
		CarrierB.RebuildMeshFromPayload();
		CarrierA.RefreshBuoyancyDensity();
		CarrierB.RefreshBuoyancyDensity();
		CarrierA.ForceNetUpdate();
		CarrierB.ForceNetUpdate();
		return false;
	}
	return true;
}

bool AFragment2DActor::GatherMaterialVolumeElements(
	const FLocalMaterialReactionProgram& Program,
	TArray<FFragmentCarrierMaterialElement>& OutElements,
	FString& OutError) const
{
	OutElements.Reset();
	OutError.Reset();
	if (!Program.IsCompiled())
	{
		OutError = TEXT("carrier material program is not compiled");
		return false;
	}
	const auto AppendSource = [&Program, &OutElements, &OutError](
		const FFragmentAggregateSourceState& Source,
		const FTransform& WorldTransform)
	{
		if (!Source.IsValid() || !WorldTransform.IsValid())
		{
			return true;
		}
		for (int32 Index = 0; Index < Source.SourceMask.SolidMask.Num(); ++Index)
		{
			if (Source.SourceMask.SolidMask[Index] == 0)
			{
				continue;
			}
			const FIntVector Cell(
				Index % Source.SourceMask.Width,
				Index / Source.SourceMask.Width,
				0);
			const FFragmentCarrierVolumeCellState* Override =
				Source.VolumeCellStates.FindByPredicate(
					[&Cell](const FFragmentCarrierVolumeCellState& Candidate)
					{
						return Candidate.Cell == Cell;
					});
			FMaterialElementState State;
			if (!Program.MakeState(
					Override ? Override->MaterialId : Source.MaterialId,
					255,
					TOptional<uint16>(),
					State))
			{
				OutError = FString::Printf(
					TEXT("carrier material '%s' is not compiled"),
					*(Override ? Override->MaterialId : Source.MaterialId)
						.ToString());
				return false;
			}
			const uint16 DefaultEnergy = State.Energy;
			if (Override)
			{
				State.Energy = Override->Energy;
			}
			else if (Source.VolumeFieldRevision > 0
				|| Source.VolumeEnvironmentEnergy != 0)
			{
				State.Energy = Source.VolumeEnvironmentEnergy;
			}
			FFragmentCarrierMaterialElement& Element =
				OutElements.AddDefaulted_GetRef();
			Element.Address = FMaterialElementAddress::MakeVolumeCell(
				Source.SourceId, Cell);
			Element.State = State;
			Element.DefaultEnergy = DefaultEnergy;
			Element.CellSize = Source.SourceMask.CellSize;
			Element.bNonEnvironmentEnergy = Override
				&& Override->Energy > Source.VolumeEnvironmentEnergy;
			Element.WorldCenter = WorldTransform.TransformPosition(FVector(
				(static_cast<double>(Cell.X) + 0.5
					- static_cast<double>(Source.SourceMask.Width) * 0.5)
					* Source.SourceMask.CellSize,
				0.0,
				(static_cast<double>(Cell.Y) + 0.5
					- static_cast<double>(Source.SourceMask.Height) * 0.5)
					* Source.SourceMask.CellSize));
		}
		return true;
	};
	if (RootMaterialState.IsValid()
		&& !AppendSource(RootMaterialState, GetActorTransform()))
	{
		OutElements.Reset();
		return false;
	}
	TArray<const FFragmentAggregateSourceState*> OrderedSources;
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		OrderedSources.Add(&Source);
	}
	OrderedSources.Sort([](
		const FFragmentAggregateSourceState& Left,
		const FFragmentAggregateSourceState& Right)
	{
		return Left.SourceId.ToString(EGuidFormats::Digits)
			< Right.SourceId.ToString(EGuidFormats::Digits);
	});
	for (const FFragmentAggregateSourceState* Source : OrderedSources)
	{
		if (!AppendSource(
				*Source, Source->LocalTransform * GetActorTransform()))
		{
			OutElements.Reset();
			return false;
		}
	}
	OutElements.Sort([](
		const FFragmentCarrierMaterialElement& Left,
		const FFragmentCarrierMaterialElement& Right)
	{
		return MaterialElementAddressLess(Left.Address, Right.Address);
	});
	return true;
}

bool AFragment2DActor::HasLocalMaterialVolumeReactionWork() const
{
	struct FLayer
	{
		const FFragmentAggregateSourceState* Source = nullptr;
		FTransform WorldTransform = FTransform::Identity;
	};
	TArray<FLayer> CarrierLayers;
	if (RootMaterialState.IsValid())
	{
		CarrierLayers.Add({&RootMaterialState, GetActorTransform()});
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (Source.IsValid())
		{
			CarrierLayers.Add({
				&Source, Source.LocalTransform * GetActorTransform()});
		}
	}
	const auto IsOccupied = [](
		const FFragmentAggregateSourceState& Source,
		const FIntVector& Cell)
	{
		const int32 Index = Cell.Y * Source.SourceMask.Width + Cell.X;
		return Cell.Z == 0 && Cell.X >= 0 && Cell.Y >= 0
			&& Cell.X < Source.SourceMask.Width
			&& Cell.Y < Source.SourceMask.Height
			&& Source.SourceMask.SolidMask.IsValidIndex(Index)
			&& Source.SourceMask.SolidMask[Index] != 0;
	};
	const auto CellWorldCenter = [](
		const FFragmentAggregateSourceState& Source,
		const FTransform& WorldTransform,
		const FIntVector& Cell)
	{
		return WorldTransform.TransformPosition(FVector(
			(static_cast<double>(Cell.X) + 0.5
				- static_cast<double>(Source.SourceMask.Width) * 0.5)
				* Source.SourceMask.CellSize,
			0.0,
			(static_cast<double>(Cell.Y) + 0.5
				- static_cast<double>(Source.SourceMask.Height) * 0.5)
				* Source.SourceMask.CellSize));
	};
	static const FIntVector Neighbours[] = {
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0) };
	for (int32 LayerIndex = 0;
		LayerIndex < CarrierLayers.Num(); ++LayerIndex)
	{
		const FLayer& Layer = CarrierLayers[LayerIndex];
		for (const FFragmentCarrierVolumeCellState& Hot
			: Layer.Source->VolumeCellStates)
		{
			if (Hot.Energy <= Layer.Source->VolumeEnvironmentEnergy)
			{
				continue;
			}
			for (const FIntVector& Offset : Neighbours)
			{
				if (IsOccupied(*Layer.Source, Hot.Cell + Offset))
				{
					return true;
				}
			}
			const FVector HotCenter = CellWorldCenter(
				*Layer.Source, Layer.WorldTransform, Hot.Cell);
			for (int32 OtherLayerIndex = 0;
				OtherLayerIndex < CarrierLayers.Num(); ++OtherLayerIndex)
			{
				if (OtherLayerIndex == LayerIndex)
				{
					continue;
				}
				const FLayer& Other = CarrierLayers[OtherLayerIndex];
				const double MaximumDistance = FMath::Max(
					Layer.Source->SourceMask.CellSize,
					Other.Source->SourceMask.CellSize) * 1.05;
				for (int32 Index = 0;
					Index < Other.Source->SourceMask.SolidMask.Num(); ++Index)
				{
					if (Other.Source->SourceMask.SolidMask[Index] == 0)
					{
						continue;
					}
					const FIntVector OtherCell(
						Index % Other.Source->SourceMask.Width,
						Index / Other.Source->SourceMask.Width,
						0);
					if (FVector::DistSquared(
							HotCenter,
							CellWorldCenter(
								*Other.Source,
								Other.WorldTransform,
								OtherCell))
						<= FMath::Square(MaximumDistance))
					{
						return true;
					}
				}
			}
		}
	}
	return false;
}

bool AFragment2DActor::HasNonEnvironmentMaterialVolumeEnergy() const
{
	const auto HasNonEnvironmentEnergy = [](
		const FFragmentAggregateSourceState& Source)
	{
		return Source.IsValid()
			&& Source.VolumeCellStates.ContainsByPredicate(
				[&Source](const FFragmentCarrierVolumeCellState& Cell)
				{
					return Cell.Energy > Source.VolumeEnvironmentEnergy;
				});
	};
	return HasNonEnvironmentEnergy(RootMaterialState)
		|| AggregateSources.ContainsByPredicate(HasNonEnvironmentEnergy);
}

bool AFragment2DActor::TryGetMaterialVolumeElementWorldLocation(
	const FMaterialElementAddress& Address,
	FVector& OutWorldLocation) const
{
	OutWorldLocation = FVector::ZeroVector;
	if (Address.Kind != EMaterialElementAddressKind::VolumeCell
		|| Address.Cell.Z != 0)
	{
		return false;
	}
	const FFragmentAggregateSourceState* Source =
		RootMaterialState.SourceId == Address.OwnerId
			? &RootMaterialState
			: AggregateSources.FindByPredicate(
				[&Address](const FFragmentAggregateSourceState& Candidate)
				{
					return Candidate.SourceId == Address.OwnerId;
				});
	if (!Source || !Source->IsValid()
		|| Address.Cell.X < 0 || Address.Cell.Y < 0
		|| Address.Cell.X >= Source->SourceMask.Width
		|| Address.Cell.Y >= Source->SourceMask.Height)
	{
		return false;
	}
	const int32 Index = Address.Cell.Y * Source->SourceMask.Width
		+ Address.Cell.X;
	if (!Source->SourceMask.SolidMask.IsValidIndex(Index)
		|| Source->SourceMask.SolidMask[Index] == 0)
	{
		return false;
	}
	const FVector LocalCenter(
		(static_cast<double>(Address.Cell.X) + 0.5
			- static_cast<double>(Source->SourceMask.Width) * 0.5)
			* Source->SourceMask.CellSize,
		0.0,
		(static_cast<double>(Address.Cell.Y) + 0.5
			- static_cast<double>(Source->SourceMask.Height) * 0.5)
			* Source->SourceMask.CellSize);
	const FTransform WorldTransform = Source == &RootMaterialState
		? GetActorTransform() : Source->LocalTransform * GetActorTransform();
	OutWorldLocation = WorldTransform.TransformPosition(LocalCenter);
	return !OutWorldLocation.ContainsNaN();
}

bool AFragment2DActor::AdvanceLocalMaterialVolumeReactions(
	const FLocalMaterialReactionProgram& Program,
	const uint32 Seed,
	const int32 LogicalStep,
	const int32 MaxContacts,
	TArray<FMaterialParticleEmission>& OutEmissions,
	int32& OutProcessedContacts,
	FString& OutError)
{
	OutEmissions.Reset();
	OutProcessedContacts = 0;
	OutError.Reset();
	if ((GetWorld() && GetWorld()->IsGameWorld() && !HasAuthority())
		|| !Program.IsCompiled() || LogicalStep < 0 || MaxContacts <= 0)
	{
		OutError = TEXT("carrier local reaction step input is invalid");
		return false;
	}

	struct FCachedCell
	{
		FMaterialElementAddress Address;
		FMaterialElementState State;
		FVector WorldCenter = FVector::ZeroVector;
		float CellSize = 0.0f;
	};
	FMaterialElementStore Store;
	TArray<FCachedCell> Cells;
	const auto AppendSource = [this, &Program, &Store, &Cells](
		const FFragmentAggregateSourceState& Source,
		const FTransform& WorldTransform)
	{
		if (!Source.IsValid() || !WorldTransform.IsValid())
		{
			return;
		}
		for (int32 Index = 0; Index < Source.SourceMask.SolidMask.Num(); ++Index)
		{
			if (Source.SourceMask.SolidMask[Index] == 0)
			{
				continue;
			}
			const FIntVector Cell(
				Index % Source.SourceMask.Width,
				Index / Source.SourceMask.Width,
				0);
			const FFragmentCarrierVolumeCellState* Override =
				Source.VolumeCellStates.FindByPredicate(
					[&Cell](const FFragmentCarrierVolumeCellState& Candidate)
					{
						return Candidate.Cell == Cell;
					});
			FMaterialElementState State;
			if (!Program.MakeState(
					Override ? Override->MaterialId : Source.MaterialId,
					255,
					TOptional<uint16>(),
					State))
			{
				continue;
			}
			if (Override)
			{
				State.Energy = Override->Energy;
			}
			else if (Source.VolumeFieldRevision > 0
				|| Source.VolumeEnvironmentEnergy != 0)
			{
				State.Energy = Source.VolumeEnvironmentEnergy;
			}
			FCachedCell& Cached = Cells.AddDefaulted_GetRef();
			Cached.Address = FMaterialElementAddress::MakeVolumeCell(
				Source.SourceId, Cell);
			Cached.State = State;
			Cached.CellSize = Source.SourceMask.CellSize;
			Cached.WorldCenter = WorldTransform.TransformPosition(FVector(
				(static_cast<double>(Cell.X) + 0.5
					- static_cast<double>(Source.SourceMask.Width) * 0.5)
					* Source.SourceMask.CellSize,
				0.0,
				(static_cast<double>(Cell.Y) + 0.5
					- static_cast<double>(Source.SourceMask.Height) * 0.5)
					* Source.SourceMask.CellSize));
			Store.SetInitialState(Cached.Address, Cached.State);
		}
	};
	if (RootMaterialState.IsValid())
	{
		AppendSource(RootMaterialState, GetActorTransform());
	}
	TArray<const FFragmentAggregateSourceState*> OrderedSources;
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		OrderedSources.Add(&Source);
	}
	OrderedSources.Sort([](
		const FFragmentAggregateSourceState& Left,
		const FFragmentAggregateSourceState& Right)
	{
		return Left.SourceId.ToString(EGuidFormats::Digits)
			< Right.SourceId.ToString(EGuidFormats::Digits);
	});
	for (const FFragmentAggregateSourceState* Source : OrderedSources)
	{
		AppendSource(*Source, Source->LocalTransform * GetActorTransform());
	}
	Cells.Sort([](const FCachedCell& Left, const FCachedCell& Right)
	{
		return MaterialElementAddressLess(Left.Address, Right.Address);
	});

	TArray<FMaterialContact> Contacts;
	for (int32 LeftIndex = 0; LeftIndex < Cells.Num(); ++LeftIndex)
	{
		const FCachedCell& Left = Cells[LeftIndex];
		for (int32 RightIndex = LeftIndex + 1;
			RightIndex < Cells.Num(); ++RightIndex)
		{
			const FCachedCell& Right = Cells[RightIndex];
			if (Left.State.Energy == 0 && Right.State.Energy == 0)
			{
				continue;
			}
			bool bAdjacent = false;
			if (Left.Address.OwnerId == Right.Address.OwnerId)
			{
				const FIntVector Difference = Left.Address.Cell - Right.Address.Cell;
				bAdjacent = FMath::Abs(Difference.X)
					+ FMath::Abs(Difference.Y)
					+ FMath::Abs(Difference.Z) == 1;
			}
			else
			{
				const double MaximumDistance = FMath::Max(
					Left.CellSize, Right.CellSize) * 1.05;
				bAdjacent = FVector::DistSquared(
					Left.WorldCenter, Right.WorldCenter)
					<= FMath::Square(MaximumDistance);
			}
			if (bAdjacent)
			{
				Contacts.Emplace(Left.Address, Right.Address, 1);
			}
		}
	}
	if (Contacts.IsEmpty())
	{
		return true;
	}
	Contacts.Sort([](
		const FMaterialContact& Left,
		const FMaterialContact& Right)
	{
		if (!(Left.ElementA == Right.ElementA))
		{
			return MaterialElementAddressLess(Left.ElementA, Right.ElementA);
		}
		return MaterialElementAddressLess(Left.ElementB, Right.ElementB);
	});
	TArray<FMaterialContact> SelectedContacts;
	const int32 ContactCount = FMath::Min(MaxContacts, Contacts.Num());
	SelectedContacts.Reserve(ContactCount);
	const int32 StartIndex = LogicalStep % Contacts.Num();
	for (int32 Offset = 0; Offset < ContactCount; ++Offset)
	{
		SelectedContacts.Add(
			Contacts[(StartIndex + Offset) % Contacts.Num()]);
	}
	OutProcessedContacts = SelectedContacts.Num();
	FLocalMaterialReactionContext Context;
	Context.Seed = Seed;
	Context.LogicalStep = LogicalStep;
	Context.MaxContacts = SelectedContacts.Num();
	Context.MaxElementDeltas = SelectedContacts.Num() * 2;
	Context.MaxEmissions = SelectedContacts.Num() * 2;
	FMaterialDeltaBatch Batch;
	if (!FLocalMaterialReactionKernel::Evaluate(
			Store,
			Program.GetThermalDefinitions(),
			Program.GetContactRules(),
			SelectedContacts,
			Context,
			Batch,
			OutError)
		|| !CommitMaterialVolumeElementBatch(Batch, Program, OutError))
	{
		return false;
	}
	OutEmissions = Batch.ParticleEmissions;
	return true;
}

bool AFragment2DActor::IsRootMaterialHot() const
{
	return RootMaterialState.IsValid()
		&& HasNonEnvironmentEnergy(RootMaterialState);
}

bool AFragment2DActor::IsAggregateSourceMaterialHot(
	const FGuid& SourceId) const
{
	const FFragmentAggregateSourceState* Source =
		AggregateSources.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	return Source && HasNonEnvironmentEnergy(*Source);
}

bool AFragment2DActor::IsAnyAggregateMaterialHot(
	const FName MaterialId) const
{
	return AggregateSources.ContainsByPredicate(
		[MaterialId](const FFragmentAggregateSourceState& Source)
		{
			return Source.MaterialId == MaterialId
				&& HasNonEnvironmentEnergy(Source);
		});
}

int32 AFragment2DActor::GetRootMaterialOverrideCellCount() const
{
	return CountMaterialOverrideCells(RootMaterialState);
}

FBox AFragment2DActor::GetReactiveWorldBounds() const
{
	FBox Bounds(ForceInit);
	if (RootMaterialState.IsValid())
	{
		Bounds += BuildMaskWorldBounds(
			RootMaterialState.SourceMask,
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

void AFragment2DActor::InitializeRootMaterialState()
{
	const bool bSupportsReaction = SpawnPayload.FragmentId.IsValid()
		&& !SpawnPayload.MaterialId.IsNone()
		&& SpawnPayload.DetachedVoxelMask.IsValid()
		&& SpawnPayload.DetachedVoxelMask.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks;
	if (!bSupportsReaction)
	{
		RootMaterialState = FFragmentAggregateSourceState();
		return;
	}
	if (RootMaterialState.SourceId == SpawnPayload.FragmentId
		&& RootMaterialState.SourceMask.HasValidLayout())
	{
		RootMaterialState.Material = FragmentMaterial;
		RootMaterialState.Color = FragmentColor;
		return;
	}

	RootMaterialState = FFragmentAggregateSourceState();
	RootMaterialState.SourceId = SpawnPayload.FragmentId;
	RootMaterialState.DefinitionSourceId = SpawnPayload.FragmentId;
	RootMaterialState.bOwnsLogicalSource = false;
	RootMaterialState.Revision = SpawnPayload.Revision;
	RootMaterialState.SourceMask = SpawnPayload.DetachedVoxelMask;
	RootMaterialState.LocalTransform = FTransform::Identity;
	RootMaterialState.Material = FragmentMaterial;
	RootMaterialState.MaterialId = SpawnPayload.MaterialId;
	RootMaterialState.Color = FragmentColor;
	RootMaterialState.bEnableCollision = SpawnPayload.bEnableCollision;
}

bool AFragment2DActor::ApplyMaterialStimulusToRootAtWorldLocation(
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	// Object-level propagation was removed. WorldActor authors an ordinary
	// material element and the fixed-step contact adapter owns all reactions.
	return false;
}

bool AFragment2DActor::ApplyMaterialStimulusAtWorldLocation(
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| WorldLocation.ContainsNaN()
		|| StimulusMaterial.IsNone())
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
	if (RootMaterialState.IsValid()
		&& RootMaterialState.SourceMask.SolidMask.Contains(1)
		&& !IsRootMaterialHot())
	{
		const double DistanceSquared =
			ComputeClosestSolidCellDistanceSquared(
				RootMaterialState.SourceMask,
				GetActorTransform(),
				WorldLocation);
		if (FMath::IsFinite(DistanceSquared))
		{
			Candidates.Add({
				RootMaterialState.SourceId,
				DistanceSquared,
				true});
		}
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.IsValid()
			|| !Source.SourceMask.SolidMask.Contains(1)
			|| IsAggregateSourceMaterialHot(Source.SourceId))
		{
			continue;
		}
		const double DistanceSquared =
			ComputeClosestSolidCellDistanceSquared(
				Source.SourceMask,
				Source.LocalTransform * GetActorTransform(),
				WorldLocation);
		if (FMath::IsFinite(DistanceSquared))
		{
			Candidates.Add({
				Source.SourceId,
				DistanceSquared,
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
		return ApplyMaterialStimulusToRootAtWorldLocation(
			WorldLocation,
			StimulusMaterial,
			EventSeed);
	}
	return ApplyMaterialStimulusToAggregateAtWorldLocation(
		Candidates[0].SourceId,
		WorldLocation,
		StimulusMaterial,
		EventSeed);
}

bool AFragment2DActor::ApplyMaterialStimulusToAggregateAtWorldLocation(
	const FGuid& SourceId,
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !SourceId.IsValid()
		|| WorldLocation.ContainsNaN()
		|| StimulusMaterial.IsNone())
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
		return ApplyMaterialStimulusToDetachedAggregateAtWorldLocation(
			SourceId,
			WorldLocation,
			StimulusMaterial,
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
		return OwnerWorld->ApplyMaterialStimulusToDynamicAggregateSource(
			*this,
			SourceId,
			WorldLocation,
			StimulusMaterial,
			EventSeed);
	}
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		if (It->ApplyMaterialStimulusToDynamicAggregateSource(
			*this,
			SourceId,
			WorldLocation,
			StimulusMaterial,
			EventSeed))
		{
			return true;
		}
	}
	return false;
}

bool AFragment2DActor::ApplyMaterialStimulusInCone(
	const FVector& Start,
	const FVector& Direction,
	const float Range,
	const float StartRadius,
	const float EndRadius,
	const FName StimulusMaterial,
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
		|| StimulusMaterial.IsNone())
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
	if (RootMaterialState.IsValid()
		&& RootMaterialState.SourceMask.SolidMask.Contains(1)
		&& !IsRootMaterialHot())
	{
		AddCandidate(
			RootMaterialState.SourceId,
			RootMaterialState.SourceMask,
			GetActorTransform(),
			true);
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (Source.IsValid()
			&& Source.SourceMask.SolidMask.Contains(1)
			&& !IsAggregateSourceMaterialHot(Source.SourceId))
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
		return ApplyMaterialStimulusToRootAtWorldLocation(
			Best.Contact,
			StimulusMaterial,
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
		return ApplyMaterialStimulusToDetachedAggregateAtWorldLocation(
			DetachedLayer->SourceId,
			Best.Contact,
			StimulusMaterial,
			EventSeed);
	}
	if (AMatterFluxPlayableWorldActor* OwnerWorld =
		Cast<AMatterFluxPlayableWorldActor>(GetOwner()))
	{
		return OwnerWorld->ApplyMaterialStimulusToDynamicAggregateSource(
			*this,
			Best.SourceId,
			Best.Contact,
			StimulusMaterial,
			EventSeed);
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			if (It->ApplyMaterialStimulusToDynamicAggregateSource(
				*this,
				Best.SourceId,
				Best.Contact,
				StimulusMaterial,
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
	const FFragment2DSourceStreamingState& State)
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
		|| State.GetRuntimeMask().Num() != CellCount)
	{
		return false;
	}

	const FFragmentAggregateSourceState Previous = *Source;
	Source->Revision = State.Revision;
	Source->SourceMask.SolidMask = State.GetRuntimeMask();
	Source->VolumeTopologyRevision = State.VolumeTopologyRevision;
	Source->VolumeFieldRevision = State.VolumeFieldRevision;
	Source->VolumeEnvironmentEnergy = State.VolumeEnvironmentEnergy;
	Source->VolumeCellStates.Reset(State.VolumeCellStates.Num());
	for (const FFragment2DMaterialVolumeCellState& Cell : State.VolumeCellStates)
	{
		Source->VolumeCellStates.Add({ Cell.Cell, Cell.MaterialId, Cell.Energy });
	}
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
	MarkReactionVisualizationDirty();
	return true;
}

bool AFragment2DActor::ApplyMaterialStimulusToDetachedAggregateAtWorldLocation(
	const FGuid& SourceId,
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	// The world material-contact adapter owns stimulus deposition for every
	// carrier cell. Detached layers intentionally have no object-level runtime.
	return false;
}

void AFragment2DActor::GatherRootMaterialVisualTransforms(
	TArray<FTransform>& OutFlameTransforms,
	TArray<MatterFlux::Rendering::FMaterialEmissionAnchor>& OutSmokeAnchors,
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
	const auto AppendVolumeFields = [
		this,
		&OutFlameTransforms,
		&OutSmokeAnchors,
		&Registry,
		MaxVisualInstances](
			const FFragmentAggregateSourceState& State,
			const FTransform& VisualTransform)
	{
		if (!State.IsValid() || !VisualTransform.IsValid())
		{
			return;
		}
		const FMatterFluxMaterialDefinition* Base = Registry.IsValid()
			? Registry->Materials.Find(State.MaterialId) : nullptr;
		for (const FFragmentCarrierVolumeCellState& Cell
			: State.VolumeCellStates)
		{
			const FMatterFluxMaterialDefinition* Current = Registry.IsValid()
				? Registry->Materials.Find(Cell.MaterialId) : nullptr;
			uint16 BurningThreshold = Current
				? Current->IgnitionThreshold : 0;
			if (BurningThreshold == 0
				&& Base
				&& Base->CombustionProduct == Cell.MaterialId
				&& Current
				&& Current->Phase
					== EMatterFluxMaterialPhase::StaticSolid)
			{
				BurningThreshold = FMath::Max<uint16>(
					Base->IgnitionThreshold,
					static_cast<uint16>(FMath::Max(
						static_cast<int32>(Base->CombustionEnergy) - 100,
						0)));
			}
			if (BurningThreshold == 0
				|| Cell.Energy < BurningThreshold
				|| Cell.Energy <= State.VolumeEnvironmentEnergy
				|| (OutFlameTransforms.Num() >= MaxVisualInstances
					&& OutSmokeAnchors.Num() >= MaxVisualInstances))
			{
				continue;
			}
			const FVector LocalPosition(
				(static_cast<double>(Cell.Cell.X) + 0.5
					- static_cast<double>(State.SourceMask.Width) * 0.5)
					* State.SourceMask.CellSize,
				VisualDepthOffset,
				(static_cast<double>(Cell.Cell.Y) + 0.62
					- static_cast<double>(State.SourceMask.Height) * 0.5)
					* State.SourceMask.CellSize);
			const FVector WorldPosition =
				VisualTransform.TransformPosition(LocalPosition);
			const float BaseScale = State.SourceMask.CellSize / 100.0f;
			if (OutFlameTransforms.Num() < MaxVisualInstances)
			{
				OutFlameTransforms.Emplace(
					VisualTransform.Rotator(),
					WorldPosition,
					FVector(
						BaseScale * 1.06f,
						BaseScale * 1.06f,
						BaseScale * 1.18f));
			}
			if (OutSmokeAnchors.Num() < MaxVisualInstances)
			{
				MatterFlux::Rendering::FMaterialEmissionAnchor& Anchor =
					OutSmokeAnchors.AddDefaulted_GetRef();
				Anchor.WorldPosition = WorldPosition + FVector(
					0.0f, 0.0f, State.SourceMask.CellSize * 1.05f);
				Anchor.CellSize = State.SourceMask.CellSize;
				Anchor.EmissionProbability = FMath::Clamp(
					static_cast<float>(Cell.Energy - BurningThreshold)
						/ static_cast<float>(FMath::Max<int32>(
							1, MAX_uint16 - BurningThreshold)),
					0.15f,
					1.0f);
				Anchor.Seed = GetTypeHash(State.SourceId)
					^ GetTypeHash(Cell.Cell);
			}
		}
	};
	if (RootMaterialState.IsValid())
	{
		AppendVolumeFields(RootMaterialState, GetActorTransform());
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		AppendVolumeFields(
			Source, Source.LocalTransform * GetActorTransform());
	}
}

void AFragment2DActor::MarkReactionVisualizationDirty() const
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			It->MarkSourceMaterialVisualizationDirty();
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
	Candidate.VolumeTopologyRevision =
		StreamingState.VolumeTopologyRevision;
	Candidate.VolumeFieldRevision = StreamingState.VolumeFieldRevision;
	Candidate.VolumeEnvironmentEnergy =
		StreamingState.VolumeEnvironmentEnergy;
	Candidate.VolumeCellStates.Reserve(StreamingState.VolumeCellStates.Num());
	for (const FFragment2DMaterialVolumeCellState& Cell
		: StreamingState.VolumeCellStates)
	{
		Candidate.VolumeCellStates.Add({ Cell.Cell, Cell.MaterialId, Cell.Energy });
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
		SetActorTickEnabled(IsRootMaterialHot());
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
	const FMatterFluxContentRegistryPtr ContentRegistry =
		IMatterFluxScriptRuntime::IsAvailable()
			? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
			: nullptr;
	const auto ResolveVolumeColor = [&ContentRegistry](
		const FName MaterialId,
		const FLinearColor& Fallback)
	{
		if (ContentRegistry.IsValid())
		{
			if (const FMatterFluxMaterialDefinition* Definition =
				ContentRegistry->Materials.Find(MaterialId))
			{
				return Definition->Color;
			}
		}
		return Fallback;
	};
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
	const bool bUseRootMaterialState = bIncludeRootVoxel
		&& RootMaterialState.SourceId == SpawnPayload.FragmentId
		&& RootMaterialState.SourceMask.HasValidLayout();
	const FFragmentSourceMask& RootRenderMask = bUseRootMaterialState
		? RootMaterialState.SourceMask
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
		if (bUseRootMaterialState)
		{
			for (const FFragmentCarrierVolumeCellState& Cell
				: RootMaterialState.VolumeCellStates)
			{
				if (Cell.MaterialId == SpawnPayload.MaterialId)
				{
					continue;
				}
				const FLinearColor Color = ResolveVolumeColor(
					Cell.MaterialId, FragmentColor);
				const FString StableKey = FString::Printf(
					TEXT("%s|%08x|%08x"),
					*Cell.MaterialId.ToString(),
					Color.ToFColor(false).DWColor(),
					GetTypeHash(RootRenderMask.CellSize));
				if (!WholeMaterials.ContainsByPredicate(
					[&StableKey](const FWholeMaterial& Existing)
					{
						return Existing.StableKey == StableKey;
					}))
				{
					WholeMaterials.Add({
						StableKey,
						FragmentMaterial,
						Cell.MaterialId,
						Color,
						RootRenderMask.CellSize});
				}
			}
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
		for (const FFragmentCarrierVolumeCellState& Cell
			: Source.VolumeCellStates)
		{
			if (Cell.MaterialId != Source.MaterialId)
			{
				AddMaterial(
					Cell.MaterialId,
					ResolveVolumeColor(Cell.MaterialId, Source.Color));
			}
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
		if (bUseRootMaterialState)
		{
			TMap<FName, TArray<uint8>> OverrideMasks;
			for (const FFragmentCarrierVolumeCellState& Cell
				: RootMaterialState.VolumeCellStates)
			{
				if (Cell.MaterialId == SpawnPayload.MaterialId)
				{
					continue;
				}
				TArray<uint8>& Mask = OverrideMasks.FindOrAdd(
					Cell.MaterialId);
				if (Mask.IsEmpty())
				{
					Mask.Init(0, RootRenderMask.SolidMask.Num());
				}
				const int32 Index = Cell.Cell.Y * RootRenderMask.Width
					+ Cell.Cell.X;
				if (Mask.IsValidIndex(Index)
					&& RootRenderMask.SolidMask[Index] != 0)
				{
					Mask[Index] = 1;
				}
			}
			TArray<FName> MaterialIds;
			OverrideMasks.GetKeys(MaterialIds);
			MaterialIds.Sort(FNameLexicalLess());
			for (const FName MaterialId : MaterialIds)
			{
				AddRootLayer(
					OverrideMasks.FindChecked(MaterialId),
					MaterialId,
					ResolveVolumeColor(MaterialId, FragmentColor),
					300);
			}
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
		TMap<FName, TArray<uint8>> OverrideMasks;
		for (const FFragmentCarrierVolumeCellState& Cell
			: Source.VolumeCellStates)
		{
			if (Cell.MaterialId == Source.MaterialId)
			{
				continue;
			}
			TArray<uint8>& Mask = OverrideMasks.FindOrAdd(Cell.MaterialId);
			if (Mask.IsEmpty())
			{
				Mask.Init(0, Source.SourceMask.SolidMask.Num());
			}
			const int32 Index = Cell.Cell.Y * Source.SourceMask.Width
				+ Cell.Cell.X;
			if (Mask.IsValidIndex(Index)
				&& Source.SourceMask.SolidMask[Index] != 0)
			{
				Mask[Index] = 1;
			}
		}
		TArray<FName> MaterialIds;
		OverrideMasks.GetKeys(MaterialIds);
		MaterialIds.Sort(FNameLexicalLess());
		for (const FName MaterialId : MaterialIds)
		{
			AddLayer(
				OverrideMasks.FindChecked(MaterialId),
				MaterialId,
				ResolveVolumeColor(MaterialId, Source.Color),
				300);
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
		TArray<FRenderLayer> RenderLayers;
		if (Source.SourceMask.SolidMask.Contains(1))
		{
			RenderLayers.Add({
				&Source.SourceMask.SolidMask,
				Source.MaterialId,
				Source.Color});
		}
		TMap<FName, TArray<uint8>> OverrideMasks;
		for (const FFragmentCarrierVolumeCellState& Cell
			: Source.VolumeCellStates)
		{
			if (Cell.MaterialId == Source.MaterialId)
			{
				continue;
			}
			TArray<uint8>& Mask = OverrideMasks.FindOrAdd(Cell.MaterialId);
			if (Mask.IsEmpty())
			{
				Mask.Init(0, Source.SourceMask.SolidMask.Num());
			}
			const int32 Index = Cell.Cell.Y * Source.SourceMask.Width
				+ Cell.Cell.X;
			if (Mask.IsValidIndex(Index)
				&& Source.SourceMask.SolidMask[Index] != 0)
			{
				Mask[Index] = 1;
			}
		}
		TArray<FName> OverrideMaterialIds;
		OverrideMasks.GetKeys(OverrideMaterialIds);
		OverrideMaterialIds.Sort(FNameLexicalLess());
		for (const FName OverrideMaterialId : OverrideMaterialIds)
		{
			RenderLayers.Add({
				&OverrideMasks.FindChecked(OverrideMaterialId),
				OverrideMaterialId,
				ResolveVolumeColor(OverrideMaterialId, Source.Color)});
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
		const bool bUseCutRootMask =
			RootMaterialState.SourceMask.HasValidLayout()
			&& SpawnPayload.DetachedVoxelMask.GeometryStyle
				== EFragmentSourceGeometryStyle::VoxelBlocks;
		if (bUseCutRootMask)
		{
			const TArray<uint8>& CollisionMask =
				RootMaterialState.SourceMask.SolidMask;
			if (CollisionMask.Contains(1))
			{
				MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
				if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
					CollisionMask,
					RootMaterialState.SourceMask.Width,
					RootMaterialState.SourceMask.Height,
					RootMaterialState.SourceMask.CellSize,
					Geometry))
				{
					MeshComponent->SetCollisionEnabled(
						ECollisionEnabled::NoCollision);
					return false;
				}
				for (const FFragmentContour& Contour : Geometry.CollisionContours)
				{
					AddContour(
						Contour,
						RootMaterialState.SourceMask.CellSize,
						FTransform::Identity);
				}
			}
		}
		else
		{
			for (const FFragmentContour& Contour : SpawnPayload.CollisionContours)
			{
				AddContour(
					Contour,
					SpawnPayload.Thickness,
					FTransform::Identity);
			}
		}
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.bEnableCollision
			|| !Source.SourceMask.HasValidLayout()
			|| !Source.SourceMask.SolidMask.Contains(1))
		{
			continue;
		}
		const TArray<uint8>& CollisionMask = Source.SourceMask.SolidMask;
		MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
		if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
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
