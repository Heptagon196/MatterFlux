#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"

#include "Algo/Count.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
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
	int32 TotalAcceptedSources = 0;
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
			if (TestTrue(
				TEXT("Each directional notch affects the trunk"),
				AcceptedSources >= 1)
				&& TestEqual(
					TEXT("World-cut result matches all changed material layers"),
					AcceptedSources,
					ChangedSourceCount)
				&& TestEqual(
					TEXT("Each directional notch advances the trunk once"),
					Trunk->Revision,
					TrunkRevisionBefore + 1))
			{
				++SuccessfulNotches;
			}
			TotalAcceptedSources += AcceptedSources;
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
		TotalAcceptedSources
			> DirectionCount * CutsPerDirection);
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
