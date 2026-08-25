#include "Fragment/Fragment2DActor.h"

#include "Fragment/FragmentGeometry.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/Fragment2DSourceStreamingState.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "IMatterFluxScriptRuntime.h"
#include "MatterFluxLog.h"
#include "Material/MatterFluxSourceReactionRuntime.h"
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

	const FMatterFluxReactionDefinition* FindReactionRule(
		const FMatterFluxContentRegistry& Registry,
		const FName InputMaterial,
		const FName StimulusMaterial)
	{
		return MatterFlux::Reaction::FMaterialReactionEngine::
			FindPropagatingRule(
				Registry, InputMaterial, StimulusMaterial);
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

	bool CopyMaskToChildFrame(
		const FFragmentSourceMask& ParentMask,
		const FFragmentSourceMask& ChildTemplate,
		const FTransform& ChildToParent,
		FFragmentSourceMask& OutChildMask)
	{
		if (!ParentMask.HasValidLayout()
			|| !ChildTemplate.HasValidLayout()
			|| !ChildToParent.IsValid())
		{
			return false;
		}
		OutChildMask = ChildTemplate;
		OutChildMask.SolidMask.Init(
			0,
			ChildTemplate.Width * ChildTemplate.Height);
		for (int32 ChildY = 0; ChildY < ChildTemplate.Height; ++ChildY)
		{
			for (int32 ChildX = 0; ChildX < ChildTemplate.Width; ++ChildX)
			{
				const FVector ChildCellCenter(
					(static_cast<float>(ChildX) + 0.5f
						- static_cast<float>(ChildTemplate.Width) * 0.5f)
						* ChildTemplate.CellSize,
					0.0f,
					(static_cast<float>(ChildY) + 0.5f
						- static_cast<float>(ChildTemplate.Height) * 0.5f)
						* ChildTemplate.CellSize);
				const FVector ParentCellCenter =
					ChildToParent.TransformPosition(ChildCellCenter);
				const int32 ParentX = FMath::RoundToInt(
					ParentCellCenter.X / ParentMask.CellSize
						+ static_cast<float>(ParentMask.Width) * 0.5f
						- 0.5f);
				const int32 ParentY = FMath::RoundToInt(
					ParentCellCenter.Z / ParentMask.CellSize
						+ static_cast<float>(ParentMask.Height) * 0.5f
						- 0.5f);
				if (ParentX < 0 || ParentX >= ParentMask.Width
					|| ParentY < 0 || ParentY >= ParentMask.Height)
				{
					return false;
				}
				OutChildMask.SolidMask[
					ChildY * ChildTemplate.Width + ChildX] =
					ParentMask.SolidMask[
						ParentY * ParentMask.Width + ParentX];
			}
		}
		return true;
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
			|| (bHasReactionState
				&& OutputMask.SolidMask.Contains(1)))
		&& LocalTransform.IsValid()
		&& !MaterialId.IsNone()
		&& FMath::IsFinite(Color.R)
		&& FMath::IsFinite(Color.G)
		&& FMath::IsFinite(Color.B)
		&& FMath::IsFinite(Color.A)
		&& OutputMask.HasValidLayout()
		&& ActiveMask.HasValidLayout()
		&& OutputMask.Width == SourceMask.Width
		&& OutputMask.Height == SourceMask.Height
		&& ActiveMask.Width == SourceMask.Width
		&& ActiveMask.Height == SourceMask.Height
		&& (!bHasReactionState
			|| (!ReactionRuleId.IsNone()
				&& FMath::IsFinite(ReactionAccumulator)
				&& ReactionAccumulator >= 0.0f
				&& TotalMaterialEmissionCount >= 0
				&& FMath::IsFinite(OutputColor.R)
				&& FMath::IsFinite(OutputColor.G)
				&& FMath::IsFinite(OutputColor.B)
				&& FMath::IsFinite(OutputColor.A)));
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
	AdvanceRootReaction(DeltaSeconds);
	AdvanceDetachedAggregateReaction(DeltaSeconds);
	if (SpawnPayload.FadeOutDuration <= 0.0f)
	{
		SetActorTickEnabled(
			IsRootReacting()
			|| !DetachedAggregateReactionRuntimes.IsEmpty());
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
	InitializeRootReactionState();
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
	DOREPLIFETIME(AFragment2DActor, RootReactionState);
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
	InitializeRootReactionState();
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
	if (RootReactionState.SourceId == SpawnPayload.FragmentId)
	{
		RootReactionState.Material = FragmentMaterial;
		RootReactionState.Color = FragmentColor;
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

void AFragment2DActor::OnRep_RootReactionState()
{
	RebuildMeshFromPayload();
	SetActorTickEnabled(
		SpawnPayload.FadeOutDuration > 0.0f || IsRootReacting());
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
	if (RootReactionState.IsValid())
	{
		RootMask = &RootReactionState.SourceMask;
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
		RootReactionState;
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
		ApplyCutToMask(State.OutputMask, StateWorldTransform);
		ApplyCutToMask(State.ActiveMask, StateWorldTransform);
		++State.Revision;
		return true;
	};

	bool bChanged = false;
	if (RootReactionState.IsValid())
	{
		bChanged |= ApplyCutToState(
			RootReactionState,
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
		if (!RootReactionState.SourceMask.HasValidLayout()
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
		return State.SourceMask.SolidMask.Contains(1)
			|| (State.bHasReactionState
				&& State.OutputMask.SolidMask.Contains(1));
	};
	const bool bHasRemainingMaterial =
		StateHasMaterial(RootReactionState)
		|| AggregateSources.ContainsByPredicate(StateHasMaterial)
		|| (!RootReactionState.SourceMask.HasValidLayout()
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
		RootReactionState = PreviousRootState;
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
		RootReactionState = PreviousRootState;
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
		|| !RootReactionState.IsValid()
		|| !ParentWorldTransform.IsValid())
	{
		return true;
	}

	const FFragmentSourceMask& RootMask = RootReactionState.SourceMask;
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

	const FGuid SplitSourceId = RootReactionState.SourceId.IsValid()
		? RootReactionState.SourceId
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
		RootReactionState.Revision,
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
		Payload.MaterialId = RootReactionState.MaterialId.IsNone()
			? SpawnPayload.MaterialId
			: RootReactionState.MaterialId;
		Payload.bEnableCollision = RootReactionState.bEnableCollision;
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

		FFragmentAggregateSourceState ChildState = RootReactionState;
		ChildState.SourceId = Payload.FragmentId;
		ChildState.bOwnsLogicalSource = false;
		ChildState.LocalTransform = FTransform::Identity;
		ChildState.SourceMask = Payload.DetachedVoxelMask;
		const FTransform ChildToParent =
			Payload.InitialTransform.GetRelativeTransform(
				ParentWorldTransform);
		if (RootReactionState.OutputMask.HasValidLayout())
		{
			if (!CopyMaskToChildFrame(
					RootReactionState.OutputMask,
					Payload.DetachedVoxelMask,
					ChildToParent,
					ChildState.OutputMask))
			{
				RollBackChildren();
				return false;
			}
		}
		else
		{
			ChildState.OutputMask = Payload.DetachedVoxelMask;
			ChildState.OutputMask.SolidMask.Init(
				0,
				ChildState.OutputMask.Width
					* ChildState.OutputMask.Height);
		}
		if (RootReactionState.ActiveMask.HasValidLayout())
		{
			if (!CopyMaskToChildFrame(
					RootReactionState.ActiveMask,
					Payload.DetachedVoxelMask,
					ChildToParent,
					ChildState.ActiveMask))
			{
				RollBackChildren();
				return false;
			}
		}
		else
		{
			ChildState.ActiveMask = Payload.DetachedVoxelMask;
			ChildState.ActiveMask.SolidMask.Init(
				0,
				ChildState.ActiveMask.Width
					* ChildState.ActiveMask.Height);
		}

		Child->RootReactionState = MoveTemp(ChildState);
		Child->AcceptedCutCount = NextAcceptedCutCount;
		Child->RefreshBuoyancyDensity();
		if (!Child->RebuildMeshFromPayload())
		{
			RollBackChildren();
			return false;
		}
		Child->SetActorTickEnabled(
			Child->SpawnPayload.FadeOutDuration > 0.0f
				|| Child->IsRootReacting());
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

bool AFragment2DActor::IsRootReacting() const
{
	return RootReactionState.bHasReactionState
		&& RootReactionState.ActiveMask.SolidMask.Contains(1);
}

bool AFragment2DActor::IsAggregateSourceReacting(
	const FGuid& SourceId) const
{
	const FFragmentAggregateSourceState* Source =
		AggregateSources.FindByPredicate(
			[&SourceId](const FFragmentAggregateSourceState& Candidate)
			{
				return Candidate.SourceId == SourceId;
			});
	return Source
		&& Source->bHasReactionState
		&& Source->ActiveMask.SolidMask.Contains(1);
}

bool AFragment2DActor::IsAnyAggregateMaterialReacting(
	const FName MaterialId) const
{
	return AggregateSources.ContainsByPredicate(
		[MaterialId](const FFragmentAggregateSourceState& Source)
		{
			return Source.MaterialId == MaterialId
				&& Source.bHasReactionState
				&& Source.ActiveMask.SolidMask.Contains(1);
		});
}

int32 AFragment2DActor::GetRootReactionOutputCellCount() const
{
	return RootReactionState.bHasReactionState
		? CountSetCells(RootReactionState.OutputMask.SolidMask)
		: 0;
}

FBox AFragment2DActor::GetReactiveWorldBounds() const
{
	FBox Bounds(ForceInit);
	if (RootReactionState.IsValid())
	{
		Bounds += BuildMaskWorldBounds(
			RootReactionState.SourceMask,
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

void AFragment2DActor::InitializeRootReactionState()
{
	const bool bSupportsReaction = SpawnPayload.FragmentId.IsValid()
		&& !SpawnPayload.MaterialId.IsNone()
		&& SpawnPayload.DetachedVoxelMask.IsValid()
		&& SpawnPayload.DetachedVoxelMask.GeometryStyle
			== EFragmentSourceGeometryStyle::VoxelBlocks;
	if (!bSupportsReaction)
	{
		RootReactionState = FFragmentAggregateSourceState();
		RootReactionRuntime.Reset();
		return;
	}
	if (RootReactionState.SourceId == SpawnPayload.FragmentId
		&& RootReactionState.SourceMask.HasValidLayout())
	{
		RootReactionState.Material = FragmentMaterial;
		RootReactionState.Color = FragmentColor;
		return;
	}

	RootReactionState = FFragmentAggregateSourceState();
	RootReactionState.SourceId = SpawnPayload.FragmentId;
	RootReactionState.DefinitionSourceId = SpawnPayload.FragmentId;
	RootReactionState.bOwnsLogicalSource = false;
	RootReactionState.Revision = SpawnPayload.Revision;
	RootReactionState.SourceMask = SpawnPayload.DetachedVoxelMask;
	RootReactionState.LocalTransform = FTransform::Identity;
	RootReactionState.Material = FragmentMaterial;
	RootReactionState.MaterialId = SpawnPayload.MaterialId;
	RootReactionState.Color = FragmentColor;
	RootReactionState.bEnableCollision = SpawnPayload.bEnableCollision;
	RootReactionState.OutputMask = SpawnPayload.DetachedVoxelMask;
	RootReactionState.OutputMask.SolidMask.Init(
		0,
		SpawnPayload.DetachedVoxelMask.SolidMask.Num());
	RootReactionState.ActiveMask = SpawnPayload.DetachedVoxelMask;
	RootReactionState.ActiveMask.SolidMask.Init(
		0,
		SpawnPayload.DetachedVoxelMask.SolidMask.Num());
	RootReactionRuntime.Reset();
}

bool AFragment2DActor::ApplyMaterialStimulusToRootAtWorldLocation(
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| WorldLocation.ContainsNaN()
		|| StimulusMaterial.IsNone()
		|| !RootReactionState.IsValid()
		|| IsRootReacting())
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
	const FMatterFluxReactionDefinition* Rule = FindReactionRule(
		*Registry,
		RootReactionState.MaterialId,
		StimulusMaterial);
	if (!Rule)
	{
		return false;
	}

	TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime> Candidate =
		MakeUnique<MatterFlux::Reaction::FSourceReactionRuntime>();
	FString Error;
	if (!Candidate->Initialize(
		MatterFlux::Reaction::FSourceRuntimeSettings(),
		RootReactionState.SourceMask,
		*Rule,
		EventSeed,
		Error))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Detached fragment reaction initialization failed: %s"),
			*Error);
		return false;
	}
	const FVector Local = GetActorTransform().InverseTransformPosition(
		WorldLocation);
	const FFragmentSourceMask& Mask = RootReactionState.SourceMask;
	const FIntPoint RequestedCell(
		FMath::FloorToInt(
			Local.X / Mask.CellSize
				+ static_cast<double>(Mask.Width) * 0.5),
		FMath::FloorToInt(
			Local.Z / Mask.CellSize
				+ static_cast<double>(Mask.Height) * 0.5));
	if (!Candidate->ActivateNearest(RequestedCell, StimulusMaterial))
	{
		return false;
	}
	RootReactionRuntime = MoveTemp(Candidate);
	if (!SynchronizeRootReactionState())
	{
		RootReactionRuntime.Reset();
		return false;
	}
	RootReactionPropagationAccumulator = 0.0f;
	SetActorTickEnabled(true);
	ForceNetUpdate();
	MarkReactionVisualizationDirty();
	return true;
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
	if (RootReactionState.IsValid()
		&& RootReactionState.SourceMask.SolidMask.Contains(1)
		&& !IsRootReacting())
	{
		const double DistanceSquared =
			ComputeClosestSolidCellDistanceSquared(
				RootReactionState.SourceMask,
				GetActorTransform(),
				WorldLocation);
		if (FMath::IsFinite(DistanceSquared))
		{
			Candidates.Add({
				RootReactionState.SourceId,
				DistanceSquared,
				true});
		}
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (!Source.IsValid()
			|| !Source.SourceMask.SolidMask.Contains(1)
			|| IsAggregateSourceReacting(Source.SourceId))
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
	if (RootReactionState.IsValid()
		&& RootReactionState.SourceMask.SolidMask.Contains(1)
		&& !IsRootReacting())
	{
		AddCandidate(
			RootReactionState.SourceId,
			RootReactionState.SourceMask,
			GetActorTransform(),
			true);
	}
	for (const FFragmentAggregateSourceState& Source : AggregateSources)
	{
		if (Source.IsValid()
			&& Source.SourceMask.SolidMask.Contains(1)
			&& !IsAggregateSourceReacting(Source.SourceId))
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
	const FFragment2DSourceStreamingState& State,
	const FName InOutputMaterialId,
	const FLinearColor& InOutputColor)
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
		|| !State.bHasReactionState
		|| State.ReactionState.OutputMask.Num() != CellCount
		|| State.ReactionState.ActiveMask.Num() != CellCount)
	{
		return false;
	}

	const FFragmentAggregateSourceState Previous = *Source;
	Source->Revision = State.Revision;
	Source->SourceMask.SolidMask = State.GetRuntimeMask();
	Source->OutputMask = Source->SourceMask;
	Source->OutputMask.SolidMask = State.ReactionState.OutputMask;
	Source->ActiveMask = Source->SourceMask;
	Source->ActiveMask.SolidMask = State.ReactionState.ActiveMask;
	for (uint8& Value : Source->ActiveMask.SolidMask)
	{
		Value = Value != 0 ? 1 : 0;
	}
	Source->bHasReactionState = true;
	Source->ReactionRuleId = State.ReactionState.RuleId;
	Source->OutputMaterialId = InOutputMaterialId;
	Source->OutputColor = InOutputColor;
	Source->ReactionSeed = State.ReactionState.Seed;
	Source->ReactionTick = State.ReactionState.Tick;
	Source->ReactionAccumulator = State.ReactionAccumulator;
	Source->TotalMaterialEmissionCount = State.TotalMaterialEmissionCount;
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

bool AFragment2DActor::SynchronizeRootReactionState()
{
	if (!RootReactionRuntime || !RootReactionRuntime->IsInitialized())
	{
		return false;
	}
	MatterFlux::Reaction::FSourceRuntimeSnapshot Snapshot;
	if (!RootReactionRuntime->CaptureState(Snapshot))
	{
		return false;
	}
	RootReactionState.SourceMask.SolidMask =
		MoveTemp(Snapshot.ReactionState.InputMask);
	RootReactionState.OutputMask = RootReactionState.SourceMask;
	RootReactionState.OutputMask.SolidMask =
		MoveTemp(Snapshot.ReactionState.OutputMask);
	RootReactionState.ActiveMask = RootReactionState.SourceMask;
	RootReactionState.ActiveMask.SolidMask =
		MoveTemp(Snapshot.ReactionState.ActiveMask);
	for (uint8& Value : RootReactionState.ActiveMask.SolidMask)
	{
		Value = Value != 0 ? 1 : 0;
	}
	RootReactionState.bHasReactionState = true;
	RootReactionState.ReactionRuleId = Snapshot.ReactionState.RuleId;
	RootReactionState.ReactionSeed = Snapshot.ReactionState.Seed;
	RootReactionState.ReactionTick = Snapshot.ReactionState.Tick;
	RootReactionState.ReactionAccumulator =
		Snapshot.ReactionAccumulator;
	RootReactionState.TotalMaterialEmissionCount =
		Snapshot.TotalMaterialEmissionCount;
	if (const FMatterFluxReactionDefinition* Rule =
		RootReactionRuntime->GetRule())
	{
		RootReactionState.OutputMaterialId = Rule->OutputA;
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (Registry.IsValid())
		{
			if (const FMatterFluxMaterialDefinition* Output =
				Registry->Materials.Find(Rule->OutputA))
			{
				RootReactionState.OutputColor = Output->Color;
			}
		}
	}
	if (!RootReactionState.IsValid() || !RebuildMeshFromPayload())
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Detached fragment rejected its reaction mesh update"));
		return false;
	}
	ForceNetUpdate();
	MarkReactionVisualizationDirty();
	return true;
}

void AFragment2DActor::AdvanceRootReaction(const float DeltaSeconds)
{
	if (!HasAuthority()
		|| !RootReactionRuntime
		|| !RootReactionRuntime->IsActive())
	{
		return;
	}
	const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	const MatterFlux::Reaction::FSourceAdvanceResult Result =
		RootReactionRuntime->AdvanceAuthority(ClampedDelta);
	if (Result.Steps > 0)
	{
		SynchronizeRootReactionState();
	}

	RootReactionPropagationAccumulator += ClampedDelta;
	if (RootReactionRuntime->IsActive()
		&& RootReactionPropagationAccumulator >= 0.2f)
	{
		RootReactionPropagationAccumulator = FMath::Fmod(
			RootReactionPropagationAccumulator,
			0.2f);
		const FMatterFluxReactionDefinition* Rule =
			RootReactionRuntime->GetRule();
		if (Rule)
		{
			const int32 Seed = RootReactionState.ReactionSeed
				^ static_cast<int32>(RootReactionState.ReactionTick)
				^ static_cast<int32>(GetTypeHash(SpawnPayload.FragmentId));
			EmitReactionParticleToAdjacentLayer(
				RootReactionState,
				GetActorTransform(),
				Rule->InputB,
				Seed);
		}
	}
	if (!RootReactionRuntime->IsActive())
	{
		RootReactionRuntime.Reset();
		SetActorTickEnabled(SpawnPayload.FadeOutDuration > 0.0f);
	}
}

bool AFragment2DActor::ApplyMaterialStimulusToDetachedAggregateAtWorldLocation(
	const FGuid& SourceId,
	const FVector& WorldLocation,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	FFragmentAggregateSourceState* Source = AggregateSources.FindByPredicate(
		[&SourceId](const FFragmentAggregateSourceState& Candidate)
		{
			return Candidate.SourceId == SourceId;
		});
	if (!HasAuthority()
		|| WorldLocation.ContainsNaN()
		|| StimulusMaterial.IsNone()
		|| !Source
		|| Source->bOwnsLogicalSource
		|| !Source->IsValid()
		|| IsAggregateSourceReacting(SourceId))
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
	const FMatterFluxReactionDefinition* Rule = FindReactionRule(
		*Registry,
		Source->MaterialId,
		StimulusMaterial);
	if (!Rule)
	{
		return false;
	}

	TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime> Runtime =
		MakeUnique<MatterFlux::Reaction::FSourceReactionRuntime>();
	FString Error;
	if (!Runtime->Initialize(
		MatterFlux::Reaction::FSourceRuntimeSettings(),
		Source->SourceMask,
		*Rule,
		EventSeed,
		Error))
	{
		UE_LOG(
			LogMatterFlux,
			Error,
			TEXT("Detached aggregate layer reaction initialization failed: %s"),
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
	if (!Runtime->ActivateNearest(RequestedCell, StimulusMaterial))
	{
		return false;
	}
	DetachedAggregateReactionRuntimes.Add(
		SourceId,
		MoveTemp(Runtime));
	if (!SynchronizeDetachedAggregateReactionState(SourceId))
	{
		DetachedAggregateReactionRuntimes.Remove(SourceId);
		return false;
	}
	DetachedAggregateReactionPropagationAccumulator = 0.0f;
	SetActorTickEnabled(true);
	return true;
}

bool AFragment2DActor::SynchronizeDetachedAggregateReactionState(
	const FGuid& SourceId)
{
	TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime>* RuntimePtr =
		DetachedAggregateReactionRuntimes.Find(SourceId);
	FFragmentAggregateSourceState* Source = AggregateSources.FindByPredicate(
		[&SourceId](const FFragmentAggregateSourceState& Candidate)
		{
			return Candidate.SourceId == SourceId;
		});
	if (!RuntimePtr || !RuntimePtr->Get() || !Source)
	{
		return false;
	}
	MatterFlux::Reaction::FSourceRuntimeSnapshot Snapshot;
	if (!(*RuntimePtr)->CaptureState(Snapshot))
	{
		return false;
	}
	Source->SourceMask.SolidMask = MoveTemp(
		Snapshot.ReactionState.InputMask);
	Source->OutputMask = Source->SourceMask;
	Source->OutputMask.SolidMask = MoveTemp(
		Snapshot.ReactionState.OutputMask);
	Source->ActiveMask = Source->SourceMask;
	Source->ActiveMask.SolidMask = MoveTemp(
		Snapshot.ReactionState.ActiveMask);
	for (uint8& Value : Source->ActiveMask.SolidMask)
	{
		Value = Value != 0 ? 1 : 0;
	}
	Source->bHasReactionState = true;
	Source->ReactionRuleId = Snapshot.ReactionState.RuleId;
	Source->ReactionSeed = Snapshot.ReactionState.Seed;
	Source->ReactionTick = Snapshot.ReactionState.Tick;
	Source->ReactionAccumulator = Snapshot.ReactionAccumulator;
	Source->TotalMaterialEmissionCount = Snapshot.TotalMaterialEmissionCount;
	if (const FMatterFluxReactionDefinition* Rule =
		(*RuntimePtr)->GetRule())
	{
		Source->OutputMaterialId = Rule->OutputA;
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (Registry.IsValid())
		{
			if (const FMatterFluxMaterialDefinition* Output =
				Registry->Materials.Find(Rule->OutputA))
			{
				Source->OutputColor = Output->Color;
			}
		}
	}
	if (!Source->IsValid() || !RebuildMeshFromPayload())
	{
		return false;
	}
	ForceNetUpdate();
	MarkReactionVisualizationDirty();
	return true;
}

void AFragment2DActor::AdvanceDetachedAggregateReaction(
	const float DeltaSeconds)
{
	if (!HasAuthority() || DetachedAggregateReactionRuntimes.IsEmpty())
	{
		return;
	}
	const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, 0.25f);
	TArray<FGuid> Completed;
	TArray<FGuid> ActiveIds;
	DetachedAggregateReactionRuntimes.GetKeys(ActiveIds);
	ActiveIds.Sort([](const FGuid& A, const FGuid& B)
	{
		return A.ToString(EGuidFormats::Digits)
			< B.ToString(EGuidFormats::Digits);
	});
	for (const FGuid& SourceId : ActiveIds)
	{
		TUniquePtr<MatterFlux::Reaction::FSourceReactionRuntime>* Runtime =
			DetachedAggregateReactionRuntimes.Find(SourceId);
		if (!Runtime || !Runtime->Get())
		{
			Completed.Add(SourceId);
			continue;
		}
		const MatterFlux::Reaction::FSourceAdvanceResult Result =
			(*Runtime)->AdvanceAuthority(ClampedDelta);
		if (Result.Steps > 0)
		{
			SynchronizeDetachedAggregateReactionState(SourceId);
		}
		if (!(*Runtime)->IsActive())
		{
			Completed.Add(SourceId);
		}
	}
	for (const FGuid& SourceId : Completed)
	{
		DetachedAggregateReactionRuntimes.Remove(SourceId);
	}

	DetachedAggregateReactionPropagationAccumulator += ClampedDelta;
	if (!DetachedAggregateReactionRuntimes.IsEmpty()
		&& DetachedAggregateReactionPropagationAccumulator >= 0.2f)
	{
		DetachedAggregateReactionPropagationAccumulator = FMath::Fmod(
			DetachedAggregateReactionPropagationAccumulator,
			0.2f);
		for (const FGuid& SourceId : ActiveIds)
		{
			const TUniquePtr<
				MatterFlux::Reaction::FSourceReactionRuntime>* Runtime =
				DetachedAggregateReactionRuntimes.Find(SourceId);
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
				EmitReactionParticleToAdjacentLayer(
					*Source,
					Source->LocalTransform * GetActorTransform(),
					Rule->InputB,
					Source->ReactionSeed
						^ static_cast<int32>(Source->ReactionTick));
				break;
			}
		}
	}
}

bool AFragment2DActor::EmitReactionParticleToAdjacentLayer(
	const FFragmentAggregateSourceState& ActiveSource,
	const FTransform& ActiveWorldTransform,
	const FName StimulusMaterial,
	const int32 EventSeed)
{
	if (!HasAuthority()
		|| !ActiveSource.IsValid()
		|| !ActiveSource.bHasReactionState
		|| StimulusMaterial.IsNone()
		|| !ActiveWorldTransform.IsValid())
	{
		return false;
	}
	(void)EventSeed;
	TArray<FVector, TInlineAllocator<64>> ActiveCellCenters;
	for (int32 Index = 0;
		Index < ActiveSource.ActiveMask.SolidMask.Num();
		++Index)
	{
		if (ActiveSource.ActiveMask.SolidMask[Index] == 0)
		{
			continue;
		}
		const int32 X = Index % ActiveSource.SourceMask.Width;
		const int32 Y = Index / ActiveSource.SourceMask.Width;
		ActiveCellCenters.Add(ActiveWorldTransform.TransformPosition(FVector(
			(static_cast<float>(X) + 0.5f
				- ActiveSource.SourceMask.Width * 0.5f)
				* ActiveSource.SourceMask.CellSize,
			0.0f,
			(static_cast<float>(Y) + 0.5f
				- ActiveSource.SourceMask.Height * 0.5f)
				* ActiveSource.SourceMask.CellSize)));
	}
	if (ActiveCellCenters.IsEmpty())
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
		&ActiveCellCenters,
		&ActiveSource](
			const FFragmentAggregateSourceState& Candidate,
			const FTransform& CandidateWorldTransform,
			const bool bRoot)
	{
		if (Candidate.SourceId == ActiveSource.SourceId
			|| !Candidate.IsValid()
			|| Candidate.bHasReactionState
				&& Candidate.ActiveMask.SolidMask.Contains(1))
		{
			return;
		}
		const double MaximumDistanceSquared = FMath::Square(
			FMath::Max(
				ActiveSource.SourceMask.CellSize,
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
			for (const FVector& ActiveCenter : ActiveCellCenters)
			{
				const double DistanceSquared = FVector::DistSquared(
					ActiveCenter,
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
	if (RootReactionState.IsValid())
	{
		ConsiderLayer(
			RootReactionState,
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
	if (AMatterFluxPlayableWorldActor* WorldOwner =
		Cast<AMatterFluxPlayableWorldActor>(GetOwner()))
	{
		return WorldOwner->SetSimulatedMaterialAtWorldLocation(
			Best.WorldLocation,
			StimulusMaterial);
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			if (It->SetSimulatedMaterialAtWorldLocation(
				Best.WorldLocation,
				StimulusMaterial))
			{
				return true;
			}
		}
	}
	return false;
}

void AFragment2DActor::GatherRootReactionVisualTransforms(
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
			|| !State.bHasReactionState
			|| !State.ActiveMask.SolidMask.Contains(1))
		{
			return;
		}
		const FMatterFluxReactionDefinition* Rule = Registry.IsValid()
			? Registry->Reactions.Find(State.ReactionRuleId)
			: nullptr;
		const bool bRenderFlames = Rule
			&& MatterFlux::Reaction::UsesFlamePresentation(*Rule);
		TArray<uint8> Occupied = State.SourceMask.SolidMask;
		if (State.OutputMask.SolidMask.Num() == Occupied.Num())
		{
			for (int32 Index = 0; Index < Occupied.Num(); ++Index)
			{
				Occupied[Index] = Occupied[Index] != 0
					|| State.OutputMask.SolidMask[Index] != 0;
			}
		}
		TArray<int32> VisibleActiveCells;
		for (int32 Index = 0;
			Index < State.ActiveMask.SolidMask.Num();
			++Index)
		{
			if (State.ActiveMask.SolidMask[Index] != 0)
			{
				VisibleActiveCells.Add(Index);
			}
		}
		TArray<int32> SmokeSourceCells;
		MatterFlux::FragmentGeometry::GatherTopExposedActiveMaskCells(
			Occupied,
			State.ActiveMask.SolidMask,
			State.SourceMask.Width,
			State.SourceMask.Height,
			SmokeSourceCells);
		TSet<int32> SmokeSourceCellSet;
		for (const int32 SmokeSourceCell : SmokeSourceCells)
		{
			SmokeSourceCellSet.Add(SmokeSourceCell);
		}
		const FFragmentSourceMask& Mask = State.SourceMask;
		for (const int32 Index : VisibleActiveCells)
		{
			if (OutFlameTransforms.Num() >= MaxVisualInstances
				&& OutSmokeAnchors.Num() >= MaxVisualInstances)
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
			if (bRenderFlames
				&& OutFlameTransforms.Num() < MaxVisualInstances)
			{
				OutFlameTransforms.Emplace(
					VisualTransform.Rotator(),
					Position,
					FlameScale);
			}
			if (SmokeSourceCellSet.Contains(Index)
				&& OutSmokeAnchors.Num() < MaxVisualInstances)
			{
				float SmokeProbability = 0.7f;
				if (Rule)
				{
					SmokeProbability = FMath::Clamp(
						static_cast<float>(Rule->EmissionChancePermille)
							/ 1000.0f,
						0.0f,
						1.0f);
				}
				MatterFlux::Rendering::FMaterialEmissionAnchor& Anchor =
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
	if (IsRootReacting())
	{
		AppendState(RootReactionState, GetActorTransform());
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

void AFragment2DActor::MarkReactionVisualizationDirty() const
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			It->MarkSourceReactionVisualizationDirty();
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
	Candidate.OutputMask = Candidate.SourceMask;
	Candidate.OutputMask.SolidMask.Init(
		0,
		Candidate.SourceMask.SolidMask.Num());
	Candidate.ActiveMask = Candidate.SourceMask;
	Candidate.ActiveMask.SolidMask.Init(
		0,
		Candidate.SourceMask.SolidMask.Num());
	if (StreamingState.bHasReactionState)
	{
		Candidate.OutputMask.SolidMask =
			StreamingState.ReactionState.OutputMask;
		Candidate.ActiveMask.SolidMask =
			StreamingState.ReactionState.ActiveMask;
		for (uint8& Value : Candidate.ActiveMask.SolidMask)
		{
			Value = Value != 0 ? 1 : 0;
		}
		Candidate.bHasReactionState = true;
		Candidate.ReactionRuleId =
			StreamingState.ReactionState.RuleId;
		Candidate.ReactionSeed =
			StreamingState.ReactionState.Seed;
		Candidate.ReactionTick =
			StreamingState.ReactionState.Tick;
		Candidate.ReactionAccumulator =
			StreamingState.ReactionAccumulator;
		Candidate.TotalMaterialEmissionCount =
			StreamingState.TotalMaterialEmissionCount;
		const FMatterFluxContentRegistryPtr Registry =
			IMatterFluxScriptRuntime::IsAvailable()
				? IMatterFluxScriptRuntime::Get().GetActiveRegistry()
				: nullptr;
		if (Registry.IsValid())
		{
			if (const FMatterFluxReactionDefinition* Rule =
				Registry->Reactions.Find(Candidate.ReactionRuleId))
			{
				Candidate.OutputMaterialId = Rule->OutputA;
				if (const FMatterFluxMaterialDefinition* Material =
					Registry->Materials.Find(Rule->OutputA))
				{
					Candidate.OutputColor = Material->Color;
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
	Candidate.OutputMask = Candidate.SourceMask;
	Candidate.OutputMask.SolidMask.Init(
		0,
		Candidate.SourceMask.SolidMask.Num());
	Candidate.ActiveMask = Candidate.SourceMask;
	Candidate.ActiveMask.SolidMask.Init(
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
		SetActorTickEnabled(IsRootReacting());
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
	const bool bUseRootReactionState = bIncludeRootVoxel
		&& RootReactionState.SourceId == SpawnPayload.FragmentId
		&& RootReactionState.SourceMask.HasValidLayout();
	const FFragmentSourceMask& RootRenderMask = bUseRootReactionState
		? RootReactionState.SourceMask
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
		if (bUseRootReactionState
			&& RootReactionState.bHasReactionState
			&& RootReactionState.OutputMask.SolidMask.Contains(1))
		{
			const FName OutputMaterialId =
				RootReactionState.OutputMaterialId.IsNone()
					? FName(TEXT("output"))
					: RootReactionState.OutputMaterialId;
			WholeMaterials.Add({
				FString::Printf(
					TEXT("%s|%08x|%08x"),
					*OutputMaterialId.ToString(),
					RootReactionState.OutputColor.ToFColor(false).DWColor(),
					GetTypeHash(RootRenderMask.CellSize)),
				FragmentMaterial,
				OutputMaterialId,
				RootReactionState.OutputColor,
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
		if (Source.bHasReactionState
			&& Source.OutputMask.SolidMask.Contains(1))
		{
			AddMaterial(
				Source.OutputMaterialId.IsNone()
					? FName(TEXT("output"))
					: Source.OutputMaterialId,
				Source.OutputColor);
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
		if (bUseRootReactionState
			&& RootReactionState.bHasReactionState)
		{
			AddRootLayer(
				RootReactionState.OutputMask.SolidMask,
				RootReactionState.OutputMaterialId.IsNone()
					? FName(TEXT("output"))
					: RootReactionState.OutputMaterialId,
				RootReactionState.OutputColor,
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
		if (Source.bHasReactionState)
		{
			AddLayer(
				Source.OutputMask.SolidMask,
				Source.OutputMaterialId.IsNone()
					? FName(TEXT("output"))
					: Source.OutputMaterialId,
				Source.OutputColor,
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
		if (Source.bHasReactionState
			&& Source.OutputMask.SolidMask.Contains(1))
		{
			RenderLayers.Add({
				&Source.OutputMask.SolidMask,
				Source.OutputMaterialId.IsNone()
					? FName(TEXT("output"))
					: Source.OutputMaterialId,
				Source.OutputColor});
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
			RootReactionState.SourceMask.HasValidLayout()
			&& SpawnPayload.DetachedVoxelMask.GeometryStyle
				== EFragmentSourceGeometryStyle::VoxelBlocks;
		if (bUseCutRootMask)
		{
			TArray<uint8> CollisionMask =
				RootReactionState.SourceMask.SolidMask;
			if (RootReactionState.bHasReactionState
				&& RootReactionState.OutputMask.SolidMask.Num()
					== CollisionMask.Num())
			{
				for (int32 Index = 0; Index < CollisionMask.Num(); ++Index)
				{
					CollisionMask[Index] = CollisionMask[Index] != 0
						|| RootReactionState.OutputMask.SolidMask[Index] != 0
						? 1
						: 0;
				}
			}
			if (CollisionMask.Contains(1))
			{
				MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
				if (!MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
					CollisionMask,
					RootReactionState.SourceMask.Width,
					RootReactionState.SourceMask.Height,
					RootReactionState.SourceMask.CellSize,
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
						RootReactionState.SourceMask.CellSize,
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
		TArray<uint8> CollisionMask = Source.SourceMask.SolidMask;
		if (Source.bHasReactionState
			&& Source.OutputMask.SolidMask.Num() == CollisionMask.Num())
		{
			for (int32 Index = 0; Index < CollisionMask.Num(); ++Index)
			{
				CollisionMask[Index] = CollisionMask[Index] != 0
					|| Source.OutputMask.SolidMask[Index] != 0
					? 1
					: 0;
			}
		}
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
