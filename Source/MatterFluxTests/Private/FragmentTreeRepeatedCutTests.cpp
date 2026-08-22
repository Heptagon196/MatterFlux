#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"

#include "Algo/Count.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "ProceduralMeshComponent.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
	constexpr int32 TreeWidth = 41;
	constexpr int32 TreeHeight = 56;
	constexpr float TreeCellSize = 8.0f;
	constexpr int32 DirectionCount = 8;
	constexpr int32 CutsPerDirection = 2;

	void SetSolid(
		FFragmentSourceMask& Mask,
		const int32 X,
		const int32 Y)
	{
		if (X >= 0 && X < Mask.Width
			&& Y >= 0 && Y < Mask.Height)
		{
			Mask.SolidMask[Mask.Width * Y + X] = 1;
		}
	}

	FFragmentSourceMask MakeStressTreeMask()
	{
		FFragmentSourceMask Mask;
		Mask.Width = TreeWidth;
		Mask.Height = TreeHeight;
		Mask.CellSize = TreeCellSize;
		Mask.MinFragmentAreaPixels = 4;
		Mask.MaxFragmentsPerBreak = 16;
		Mask.SupportMode = EFragmentSupportMode::Bottom;
		Mask.SolidMask.Init(0, Mask.Width * Mask.Height);

		// Broad roots and a thick trunk make room for repeated, non-severing
		// notches before the final felling cut.
		for (int32 Y = 0; Y <= 5; ++Y)
		{
			const int32 HalfWidth = 12 - Y;
			for (int32 X = 20 - HalfWidth; X <= 20 + HalfWidth; ++X)
			{
				SetSolid(Mask, X, Y);
			}
		}
		for (int32 Y = 6; Y <= 42; ++Y)
		{
			const int32 HalfWidth = Y < 18 ? 6 : 5;
			for (int32 X = 20 - HalfWidth; X <= 20 + HalfWidth; ++X)
			{
				SetSolid(Mask, X, Y);
			}
		}

		// A dense oval of wood stands in for the branch junctions beneath the
		// independently-materialed leaf aggregate.
		for (int32 Y = 29; Y < TreeHeight; ++Y)
		{
			for (int32 X = 2; X < TreeWidth - 2; ++X)
			{
				const double NormalizedX =
					static_cast<double>(X - 20) / 17.0;
				const double NormalizedY =
					static_cast<double>(Y - 42) / 12.0;
				if (NormalizedX * NormalizedX
					+ NormalizedY * NormalizedY <= 1.0)
				{
					SetSolid(Mask, X, Y);
				}
			}
		}
		return Mask;
	}

	FFragmentSourceMask MakeLeafMask()
	{
		FFragmentSourceMask Mask;
		Mask.Width = 15;
		Mask.Height = 9;
		Mask.CellSize = TreeCellSize;
		Mask.MinFragmentAreaPixels = 4;
		Mask.MaxFragmentsPerBreak = 4;
		Mask.SupportMode = EFragmentSupportMode::Bottom;
		Mask.SolidMask.Init(0, Mask.Width * Mask.Height);
		for (int32 Y = 0; Y < Mask.Height; ++Y)
		{
			const int32 HalfWidth =
				FMath::Clamp(6 - FMath::Abs(Y - 4), 2, 6);
			for (int32 X = 7 - HalfWidth; X <= 7 + HalfWidth; ++X)
			{
				SetSolid(Mask, X, Y);
			}
		}
		return Mask;
	}

	FVector CellCenterLocal(
		const int32 Width,
		const int32 Height,
		const int32 X,
		const int32 Y)
	{
		return FVector(
			(static_cast<float>(X) + 0.5f
				- static_cast<float>(Width) * 0.5f) * TreeCellSize,
			0.0f,
			(static_cast<float>(Y) + 0.5f
				- static_cast<float>(Height) * 0.5f) * TreeCellSize);
	}

	FIntPoint FindExposedCrownCell(
		const TArray<uint8>& Mask,
		const FVector2D& Direction)
	{
		const FVector2D CrownCenter(20.0, 42.0);
		FIntPoint BestCell(INDEX_NONE, INDEX_NONE);
		double BestProjection = -TNumericLimits<double>::Max();
		double BestPerpendicularDistance =
			TNumericLimits<double>::Max();
		for (int32 Y = 29; Y < TreeHeight; ++Y)
		{
			for (int32 X = 0; X < TreeWidth; ++X)
			{
				if (Mask[Y * TreeWidth + X] == 0)
				{
					continue;
				}
				const FVector2D Offset =
					FVector2D(
						static_cast<double>(X),
						static_cast<double>(Y))
					- CrownCenter;
				const double Projection =
					FVector2D::DotProduct(Offset, Direction);
				const double PerpendicularDistance =
					FMath::Abs(
						FVector2D::CrossProduct(
							Direction,
							Offset));
				if (Projection > BestProjection
					|| (FMath::IsNearlyEqual(
							Projection,
							BestProjection)
						&& PerpendicularDistance
							< BestPerpendicularDistance))
				{
					BestProjection = Projection;
					BestPerpendicularDistance =
						PerpendicularDistance;
					BestCell = FIntPoint(X, Y);
				}
			}
		}
		return BestCell;
	}

	bool IsPayloadGeometryValid(
		const FFragmentSpawnPayload& Payload)
	{
		if (!Payload.FragmentId.IsValid()
			|| !Payload.InitialTransform.IsValid()
			|| !FMath::IsFinite(Payload.Mass)
			|| Payload.Mass <= 0.0f
			|| Payload.InitialLinearVelocity.ContainsNaN()
			|| Payload.InitialAngularVelocity.ContainsNaN()
			|| Payload.Vertices2D.Num() < 3
			|| Payload.TriangleIndices.IsEmpty()
			|| Payload.TriangleIndices.Num() % 3 != 0
			|| Payload.OuterContours.IsEmpty()
			|| Payload.CollisionContours.IsEmpty())
		{
			return false;
		}
		for (const FVector2D& Vertex : Payload.Vertices2D)
		{
			if (Vertex.ContainsNaN())
			{
				return false;
			}
		}
		for (const int32 Index : Payload.TriangleIndices)
		{
			if (!Payload.Vertices2D.IsValidIndex(Index))
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTreeRepeatedMultiDirectionCutTest,
	"MatterFlux.Fragment.Stress.TreeRepeatedMultiDirectionCuts",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxTreeRepeatedMultiDirectionCutTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Stress-test world exists"), World))
	{
		return false;
	}

	AFragment2DSourceActor* Trunk =
		World->SpawnActor<AFragment2DSourceActor>();
	AFragment2DSourceActor* Leaves =
		World->SpawnActor<AFragment2DSourceActor>(
			FVector(0.0f, -16.0f, 170.0f),
			FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Stress tree trunk spawns"), Trunk)
		|| !TestNotNull(TEXT("Stress tree leaves spawn"), Leaves))
	{
		return false;
	}

	const FGuid AggregateId =
		FGuid::NewDeterministicGuid(
			TEXT("TreeRepeatedMultiDirectionAggregate"),
			1);
	Trunk->bDestroySourceOnFirstBreak = false;
	Leaves->bDestroySourceOnFirstBreak = false;
	if (!TestTrue(
			TEXT("Stress tree trunk mask initializes"),
			Trunk->InitializeFromProceduralMask(
				MakeStressTreeMask(),
				FGuid::NewDeterministicGuid(
					TEXT("TreeRepeatedMultiDirectionTrunk"),
					1),
				FLinearColor(0.32f, 0.12f, 0.035f),
				TEXT("wood")))
		|| !TestTrue(
			TEXT("Stress tree leaf mask initializes"),
			Leaves->InitializeFromProceduralMask(
				MakeLeafMask(),
				FGuid::NewDeterministicGuid(
					TEXT("TreeRepeatedMultiDirectionLeaves"),
					1),
				FLinearColor(0.08f, 0.55f, 0.12f),
				TEXT("leaf"))))
	{
		return false;
	}
	Trunk->ConfigureAggregate(AggregateId, true);
	Leaves->ConfigureAggregate(AggregateId, false);
	Leaves->SetSourceCollisionEnabled(false);
	const FGuid LeafSourceId = Leaves->SourceId;

	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem))
	{
		return false;
	}

	const int32 InitialSolidCellCount =
		static_cast<int32>(
			Algo::Count(
				Trunk->GetRuntimeMask(),
				static_cast<uint8>(1)));
	double TotalNotchMilliseconds = 0.0;
	double MaximumNotchMilliseconds = 0.0;
	int32 SuccessfulNotches = 0;
	bool bTouchedOverlappingLeafLayer = false;
	for (int32 Pass = 0; Pass < CutsPerDirection; ++Pass)
	{
		for (int32 DirectionIndex = 0;
			DirectionIndex < DirectionCount;
			++DirectionIndex)
		{
			const double Angle =
				2.0 * UE_DOUBLE_PI
				* static_cast<double>(DirectionIndex)
				/ static_cast<double>(DirectionCount);
			const FVector2D Direction(
				FMath::Cos(Angle),
				FMath::Sin(Angle));
			const FIntPoint TargetCell =
				FindExposedCrownCell(
					Trunk->GetRuntimeMask(),
					Direction);
			if (!TestTrue(
				TEXT("Every approach direction finds exposed wood"),
				TargetCell.X != INDEX_NONE))
			{
				return false;
			}

			const int32 SolidBefore =
				static_cast<int32>(
					Algo::Count(
						Trunk->GetRuntimeMask(),
						static_cast<uint8>(1)));
			const int32 TrunkRevisionBefore =
				Trunk->Revision;
			const int32 LeafRevisionBefore =
				Leaves->Revision;
			FFragmentWorldCutRequest Request;
			Request.CutShape.Type =
				EFragmentDamageShapeType::Line;
			Request.CutShape.WorldTransform =
				FTransform(
					FQuat(
						FVector::YAxisVector,
						static_cast<float>(-Angle)),
					CellCenterLocal(
						TreeWidth,
						TreeHeight,
						TargetCell.X,
						TargetCell.Y));
			Request.CutShape.Extents.X =
				TreeCellSize * 0.9f;
			Request.CutShape.Thickness =
				TreeCellSize * 0.9f;
			Request.DamagePower = 250.0f;
			Request.EventSeed =
				5000 + Pass * DirectionCount
				+ DirectionIndex;
			Request.TargetPadding = TreeCellSize;

			const double CutStart =
				FPlatformTime::Seconds();
			const int32 AcceptedSources =
				Subsystem->RequestWorldCut(Request);
			const double CutMilliseconds =
				(FPlatformTime::Seconds() - CutStart)
				* 1000.0;
			TotalNotchMilliseconds += CutMilliseconds;
			MaximumNotchMilliseconds =
				FMath::Max(
					MaximumNotchMilliseconds,
					CutMilliseconds);

			const int32 ChangedSourceCount =
				(Trunk->Revision - TrunkRevisionBefore)
					+ (Leaves->Revision - LeafRevisionBefore);
			bTouchedOverlappingLeafLayer |=
				Leaves->Revision > LeafRevisionBefore;
			if (TestTrue(
				TEXT("Each directional notch affects the trunk"),
				AcceptedSources >= 1)
				&& TestEqual(
					TEXT("One aggregate reports one affected logical target"),
					AcceptedSources,
					1)
				&& TestTrue(
					TEXT("One logical target may update multiple material layers"),
					ChangedSourceCount >= 1)
				&& TestEqual(
					TEXT("Each directional notch advances the trunk once"),
					Trunk->Revision,
					TrunkRevisionBefore + 1))
			{
				++SuccessfulNotches;
			}
			const int32 SolidAfter =
				static_cast<int32>(
					Algo::Count(
						Trunk->GetRuntimeMask(),
						static_cast<uint8>(1)));
			TestTrue(
				TEXT("Every accepted notch removes wood"),
				SolidAfter < SolidBefore);
			TestEqual(
				TEXT("Every accepted notch advances exactly one revision"),
				Trunk->Revision,
				SuccessfulNotches);
			TestFalse(
				TEXT("Edge notches do not break the supported trunk"),
				Trunk->bBroken);
			TestTrue(
				TEXT("Leaves remain terrain-bound before the trunk is severed"),
				Leaves->GetAttachParentActor() == nullptr);
			TestFalse(
				TEXT("Leaves are not marked detached by a surface notch"),
				Leaves->bDetachedFromTerrain);
		}
	}

	int32 PrematureFragmentCount = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		++PrematureFragmentCount;
	}
	TestEqual(
		TEXT("Surface notches do not create disconnected debris"),
		PrematureFragmentCount,
		0);
	TestEqual(
		TEXT("All repeated directional notches commit"),
		SuccessfulNotches,
		DirectionCount * CutsPerDirection);
	TestTrue(
		TEXT("At least one cut exercises overlapping wood and leaf layers"),
		bTouchedOverlappingLeafLayer);
	TestEqual(
		TEXT("Repeated notches remove exactly one cell each"),
		InitialSolidCellCount
			- static_cast<int32>(
				Algo::Count(
					Trunk->GetRuntimeMask(),
					static_cast<uint8>(1))),
		DirectionCount * CutsPerDirection);

	FFragmentWorldCutRequest FellingCut;
	FellingCut.CutShape.Type =
		EFragmentDamageShapeType::Line;
	FellingCut.CutShape.WorldTransform =
		FTransform(
			CellCenterLocal(
				TreeWidth,
				TreeHeight,
				TreeWidth / 2,
				10));
	FellingCut.CutShape.Extents.X =
		TreeWidth * TreeCellSize;
	FellingCut.CutShape.Thickness =
		TreeCellSize * 0.9f;
	FellingCut.DamagePower = 600.0f;
	FellingCut.EventSeed = 9001;
	FellingCut.TargetPadding = TreeCellSize;
	const double FellingStart = FPlatformTime::Seconds();
	const int32 AcceptedFellingSources =
		Subsystem->RequestWorldCut(FellingCut);
	const double FellingMilliseconds =
		(FPlatformTime::Seconds() - FellingStart)
			* 1000.0;

	AddInfo(
		FString::Printf(
			TEXT("Tree repeated-cut performance: %d notches total %.2f ms, average %.2f ms, max %.2f ms; felling %.2f ms"),
			SuccessfulNotches,
			TotalNotchMilliseconds,
			SuccessfulNotches > 0
				? TotalNotchMilliseconds
					/ static_cast<double>(SuccessfulNotches)
				: 0.0,
			MaximumNotchMilliseconds,
			FellingMilliseconds));
	TestEqual(
		TEXT("Final felling cut targets only the trunk"),
		AcceptedFellingSources,
		1);
	TestEqual(
		TEXT("Final felling cut advances one final revision"),
		Trunk->Revision,
		DirectionCount * CutsPerDirection + 1);
	TestFalse(
		TEXT("Supported stump remains part of the terrain"),
		Trunk->bBroken);
	TestFalse(
		TEXT("Supported stump is visible in the same frame as aggregate separation"),
		Trunk->IsHidden());
	TestTrue(
		TEXT("Supported stump mesh never relies on a delayed reveal"),
		Trunk->MeshComponent->IsVisible());

	TSet<FGuid> FragmentIds;
	AFragment2DActor* Carrier = nullptr;
	int32 FragmentCount = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		AFragment2DActor* Fragment = *It;
		++FragmentCount;
		Carrier = Carrier ? Carrier : Fragment;
		TestTrue(
			TEXT("Detached fragment payload remains structurally valid"),
			IsPayloadGeometryValid(Fragment->SpawnPayload));
		TestFalse(
			TEXT("Repeated cuts never duplicate a fragment id"),
			FragmentIds.Contains(
				Fragment->SpawnPayload.FragmentId));
		FragmentIds.Add(Fragment->SpawnPayload.FragmentId);
		TestEqual(
			TEXT("Detached tree fragment has query and physics collision"),
			Fragment->MeshComponent->GetCollisionEnabled(),
			ECollisionEnabled::QueryAndPhysics);
		TestTrue(
			TEXT("Detached tree fragment has a rendered mesh section"),
			Fragment->MeshComponent->GetNumSections() > 0);
	}
	TestEqual(
		TEXT("Felling the connected upper tree creates one carrier"),
		FragmentCount,
		1);
	if (TestNotNull(
		TEXT("Felled tree has a physical carrier"),
		Carrier))
	{
		TestTrue(
			TEXT("Felled tree uses continuous collision detection against a moving character"),
			Carrier->MeshComponent->BodyInstance.bUseCCD);
		TestTrue(
			TEXT("Felled tree has enough linear damping to reject push-force velocity spikes"),
			Carrier->MeshComponent->GetLinearDamping() >= 1.0f);
		TestTrue(
			TEXT("Felled tree has enough angular damping to prevent contact jitter"),
			Carrier->MeshComponent->GetAngularDamping() >= 4.0f);
		TestEqual(
			TEXT("Felled tree can be pushed and toppled on every world axis"),
			Carrier->MeshComponent->BodyInstance.DOFMode.GetValue(),
			EDOFMode::None);
		TestEqual(
			TEXT("Characters push detached material instead of stepping over it"),
			Carrier->MeshComponent->CanCharacterStepUpOn.GetValue(),
			ECB_No);
		TestNotNull(
			TEXT("Felled tree uses a deliberate voxel contact material"),
			Carrier->FragmentPhysicalMaterial.Get());
		if (Carrier->FragmentPhysicalMaterial)
		{
			TestTrue(
				TEXT("Voxel contact friction stays low enough to clear terrain edges"),
				Carrier->FragmentPhysicalMaterial->StaticFriction <= 0.5f);
		}
		TestEqual(
			TEXT("Independent debris bodies do not depenetrate each other"),
			Carrier->MeshComponent->GetCollisionResponseToChannel(ECC_PhysicsBody),
			ECR_Ignore);
		TestEqual(
			TEXT("Felled tree uses a custom early-sleep policy"),
			Carrier->MeshComponent->BodyInstance.SleepFamily,
			ESleepFamily::Custom);
		TestTrue(
			TEXT("Low-speed felled trees settle instead of jittering indefinitely"),
			Carrier->MeshComponent->BodyInstance.CustomSleepThresholdMultiplier >= 2.0f);
		TestTrue(
			TEXT("Felled tree gets a bounded render-only depth lane"),
			!FMath::IsNearlyZero(Carrier->GetVisualDepthOffset())
			&& FMath::Abs(Carrier->GetVisualDepthOffset()) < 2.0f);
		TestEqual(
			TEXT("Felled carrier owns one logical leaf member"),
			Carrier->GetAggregateMemberCount(),
			1);
		TestTrue(
			TEXT("Leaf SourceId transfers into the carrier"),
			Carrier->ContainsAggregateSource(LeafSourceId));
		TestEqual(
			TEXT("Leaf material remains separate inside the shared renderer"),
			Carrier->GetAggregateSourceMaterialId(LeafSourceId),
			FName(TEXT("leaf")));
	}
	bool bLeafActorStillExists = false;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		bLeafActorStillExists |= !It->IsActorBeingDestroyed()
			&& It->SourceId == LeafSourceId;
	}
	TestFalse(
		TEXT("Felled leaves no longer retain a Source Actor"),
		bLeafActorStillExists);

	// These budgets guard synchronous gameplay work. The ordinary nicks must
	// stay frame-sized; the one-time physics materialization gets a wider cap.
	TestTrue(
		TEXT("Sixteen repeated notches stay below the aggregate CPU budget"),
		TotalNotchMilliseconds < 150.0);
	TestTrue(
		TEXT("No individual surface notch exceeds one 60 FPS frame"),
		MaximumNotchMilliseconds < 16.67);
	TestTrue(
		TEXT("The final physics-producing cut avoids a visible long hitch"),
		FellingMilliseconds < 50.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTreeFellingAngleCollisionTest,
	"MatterFlux.Fragment.Physics.TreeFellingAnglesBuildStableTrunkCollision",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxTreeFellingAngleCollisionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	UFragmentSimulationSubsystem* Subsystem = World
		? World->GetSubsystem<UFragmentSimulationSubsystem>()
		: nullptr;
	if (!TestNotNull(TEXT("Felling-angle test world exists"), World)
		|| !TestNotNull(TEXT("Fragment subsystem exists"), Subsystem))
	{
		return false;
	}

	const TArray<float> CutAngles = { -30.0f, -15.0f, 0.0f, 15.0f, 30.0f };
	for (int32 AngleIndex = 0; AngleIndex < CutAngles.Num(); ++AngleIndex)
	{
		const float CutAngle = CutAngles[AngleIndex];
		const FVector TreeOrigin(
			static_cast<float>(AngleIndex) * 900.0f,
			0.0f,
			0.0f);
		AFragment2DSourceActor* Trunk =
			World->SpawnActor<AFragment2DSourceActor>(
				TreeOrigin,
				FRotator::ZeroRotator);
		AFragment2DSourceActor* Leaves =
			World->SpawnActor<AFragment2DSourceActor>(
				TreeOrigin + FVector(0.0f, -16.0f, 170.0f),
				FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Angled tree trunk spawns"), Trunk)
			|| !TestNotNull(TEXT("Angled tree leaves spawn"), Leaves))
		{
			return false;
		}

		const FGuid AggregateId = FGuid::NewDeterministicGuid(
			TEXT("TreeFellingAngleAggregate"),
			AngleIndex);
		const FGuid TrunkId = FGuid::NewDeterministicGuid(
			TEXT("TreeFellingAngleTrunk"),
			AngleIndex);
		const FGuid LeafId = FGuid::NewDeterministicGuid(
			TEXT("TreeFellingAngleLeaves"),
			AngleIndex);
		Trunk->bDestroySourceOnFirstBreak = false;
		Leaves->bDestroySourceOnFirstBreak = false;
		if (!TestTrue(
				TEXT("Angled tree trunk mask initializes"),
				Trunk->InitializeFromProceduralMask(
					MakeStressTreeMask(),
					TrunkId,
					FLinearColor(0.32f, 0.12f, 0.035f),
					TEXT("wood")))
			|| !TestTrue(
				TEXT("Angled tree leaf mask initializes"),
				Leaves->InitializeFromProceduralMask(
					MakeLeafMask(),
					LeafId,
					FLinearColor(0.08f, 0.55f, 0.12f),
					TEXT("leaf"))))
		{
			return false;
		}
		Trunk->ConfigureAggregate(AggregateId, true);
		Leaves->ConfigureAggregate(AggregateId, false);
		Leaves->SetSourceCollisionEnabled(false);

		FFragmentWorldCutRequest FellingCut;
		FellingCut.CutShape.Type = EFragmentDamageShapeType::Line;
		FellingCut.CutShape.WorldTransform = FTransform(
			FQuat(
				FVector::YAxisVector,
				FMath::DegreesToRadians(CutAngle)),
			TreeOrigin + CellCenterLocal(
				TreeWidth,
				TreeHeight,
				TreeWidth / 2,
				10));
		FellingCut.CutShape.Extents.X = TreeWidth * TreeCellSize;
		// Match the shipped TerrainCut spell instead of using a sub-cell test
		// incision. With 8-neighbour connectivity, a one-cell diagonal can
		// intentionally remain connected through its corners.
		FellingCut.CutShape.Thickness = 30.0f;
		FellingCut.DamagePower = 600.0f;
		FellingCut.EventSeed = 9400 + AngleIndex;
		FellingCut.TargetPadding = TreeCellSize;
		TestEqual(
			*FString::Printf(TEXT("%.0f degree cut affects only its trunk"), CutAngle),
			Subsystem->RequestWorldCut(FellingCut),
			1);

		AFragment2DActor* Carrier = nullptr;
		for (TActorIterator<AFragment2DActor> It(World); It; ++It)
		{
			if (It->ContainsAggregateSource(LeafId))
			{
				Carrier = *It;
				break;
			}
		}
		if (!TestNotNull(
				*FString::Printf(TEXT("%.0f degree cut creates one tree carrier"), CutAngle),
				Carrier))
		{
			continue;
		}
		TestEqual(
			TEXT("Every angled carrier enables query and physics collision"),
			Carrier->MeshComponent->GetCollisionEnabled(),
			ECollisionEnabled::QueryAndPhysics);
		const UBodySetup* BodySetup = Carrier->MeshComponent->GetBodySetup();
		TestNotNull(TEXT("Every angled carrier has a body setup"), BodySetup);
		if (BodySetup)
		{
			TestTrue(
				TEXT("Every angled trunk produces at least one convex collision hull"),
				!BodySetup->AggGeom.ConvexElems.IsEmpty());
			TestEqual(
				TEXT("Leaf visuals do not add a second collision hull"),
				BodySetup->AggGeom.ConvexElems.Num(),
				Carrier->SpawnPayload.CollisionContours.Num());
		}
		TestTrue(
			TEXT("Every angled carrier retains its collision-free leaf member"),
			Carrier->ContainsAggregateSource(LeafId));
		TestEqual(
			TEXT("Every angled leaf member remains independently materialed"),
			Carrier->GetAggregateSourceMaterialId(LeafId),
			FName(TEXT("leaf")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxBatchTreeCutPerformanceTest,
	"MatterFlux.Fragment.Stress.BatchTreeCutPerformance",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxBatchTreeCutPerformanceTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 TreeCount = 24;
	constexpr float TreeSpacing = TreeWidth * TreeCellSize + 16.0f;
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Batch tree-cut world exists"), World))
	{
		return false;
	}

	const FFragmentSourceMask TreeMask = MakeStressTreeMask();
	for (int32 Index = 0; Index < TreeCount; ++Index)
	{
		AFragment2DSourceActor* Tree =
			World->SpawnActor<AFragment2DSourceActor>(
				FVector(Index * TreeSpacing, 0.0f, 0.0f),
				FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Batch tree spawns"), Tree))
		{
			return false;
		}
		Tree->bDestroySourceOnFirstBreak = false;
		if (!TestTrue(TEXT("Batch tree mask initializes"),
			Tree->InitializeFromProceduralMask(
				TreeMask,
				FGuid::NewDeterministicGuid(
					FString::Printf(TEXT("BatchTree%d"), Index),
					1),
				FLinearColor(0.32f, 0.12f, 0.035f),
				TEXT("wood"))))
		{
			return false;
		}
	}

	FFragmentWorldCutRequest Cut;
	Cut.CutShape.Type = EFragmentDamageShapeType::Line;
	Cut.CutShape.WorldTransform = FTransform(FVector(
		(TreeCount - 1) * TreeSpacing * 0.5f,
		0.0f,
		CellCenterLocal(TreeWidth, TreeHeight, TreeWidth / 2, 10).Z));
	Cut.CutShape.Extents.X = TreeCount * TreeSpacing;
	Cut.CutShape.Thickness = TreeCellSize * 0.9f;
	Cut.DamagePower = 600.0f;
	Cut.EventSeed = 9100;
	Cut.TargetPadding = TreeCellSize;
	Cut.MaxAffectedSources = TreeCount;
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 AcceptedTrees =
		UFragmentSimulationSubsystem::ExecuteWorldCut(World, Cut);
	const double ElapsedMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	const double PerTreeMilliseconds =
		ElapsedMilliseconds / static_cast<double>(TreeCount);

	int32 PhysicalCarriers = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		if (It->SpawnPayload.bEnableCollision)
		{
			++PhysicalCarriers;
		}
	}
	AddInfo(FString::Printf(
		TEXT("Batch tree cut: %d trees in %.2f ms (%.2f ms/tree)"),
		AcceptedTrees,
		ElapsedMilliseconds,
		PerTreeMilliseconds));
	TestEqual(TEXT("One explicit batch request cuts every tree"),
		AcceptedTrees,
		TreeCount);
	TestEqual(TEXT("Every felled tree produces one physical carrier"),
		PhysicalCarriers,
		TreeCount);
	TestTrue(TEXT("Twenty-four tree batch avoids a one-second hitch"),
		ElapsedMilliseconds < 1000.0);
	TestTrue(TEXT("Per-tree cut cost remains bounded"),
		PerTreeMilliseconds < 35.0);
	return true;
}
