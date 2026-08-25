#include "Fragment/Fragment2DAsset.h"
#include "Fragment/FragmentGeometry.h"
#include "Misc/AutomationTest.h"
#include "Serialization/BitReader.h"
#include "Serialization/BitWriter.h"

#include <limits>

namespace
{
	using MatterFlux::FragmentGeometry::FFragmentGeometry2D;

	TArray<uint8> MakeMask(const int32 Width, const int32 Height, const TArray<FIntPoint>& Cells)
	{
		TArray<uint8> Mask;
		Mask.Init(0, Width * Height);
		for (const FIntPoint& Cell : Cells)
		{
			Mask[Cell.Y * Width + Cell.X] = 1;
		}
		return Mask;
	}

	double TriangleArea(const FFragmentGeometry2D& Geometry)
	{
		double Area = 0.0;
		for (int32 Index = 0; Index < Geometry.TriangleIndices.Num(); Index += 3)
		{
			const FVector2D& A = Geometry.Vertices2D[Geometry.TriangleIndices[Index]];
			const FVector2D& B = Geometry.Vertices2D[Geometry.TriangleIndices[Index + 1]];
			const FVector2D& C = Geometry.Vertices2D[Geometry.TriangleIndices[Index + 2]];
			Area += FMath::Abs((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X)) * 0.5;
		}
		return Area;
	}

	bool TriangleContainsPoint(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		const double C0 = FVector2D::CrossProduct(B - A, P - A);
		const double C1 = FVector2D::CrossProduct(C - B, P - B);
		const double C2 = FVector2D::CrossProduct(A - C, P - C);
		return (C0 >= 0.0 && C1 >= 0.0 && C2 >= 0.0) || (C0 <= 0.0 && C1 <= 0.0 && C2 <= 0.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentAssetCellSizeClampTest,
	"MatterFlux.Fragment.Asset.NonFiniteCellSizeUsesSafeMinimum",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentAssetCellSizeClampTest::RunTest(
	const FString& Parameters)
{
	UFragment2DAsset* Asset = NewObject<UFragment2DAsset>();
	if (!TestNotNull(TEXT("Fragment asset exists"), Asset))
	{
		return false;
	}

	Asset->CellSize = std::numeric_limits<float>::quiet_NaN();
	TestEqual(
		TEXT("NaN cell size clamps to the safe minimum"),
		Asset->GetClampedCellSize(),
		1.0f);
	Asset->CellSize = std::numeric_limits<float>::infinity();
	TestEqual(
		TEXT("Infinite cell size clamps to the safe minimum"),
		Asset->GetClampedCellSize(),
		1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxCircleDamageTest, "MatterFlux.Fragment.Damage.CircleChangesMask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCircleDamageTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Mask;
	Mask.Init(1, 16 * 16);
	FFragmentDamageShape Shape;
	Shape.Type = EFragmentDamageShapeType::Circle;
	Shape.WorldTransform = FTransform::Identity;
	Shape.Radius = 25.0f;
	TestTrue(TEXT("Circle damage removes at least one cell"), MatterFlux::FragmentGeometry::ApplyDamageShape(Mask, 16, 16, 10.0f, Shape));
	TestTrue(TEXT("Damaged mask contains an empty cell"), Mask.Contains(0));

	TArray<uint8> InvalidMask = {1, 2, 1};
	const TArray<uint8> OriginalInvalidMask = InvalidMask;
	TestFalse(
		TEXT("Non-binary damage masks are rejected"),
		MatterFlux::FragmentGeometry::ApplyDamageShape(
			InvalidMask,
			3,
			1,
			10.0f,
			Shape));
	TestTrue(
		TEXT("Rejected non-binary mask remains unchanged"),
		InvalidMask == OriginalInvalidMask);

	FFragmentDamageShape InvalidShape = Shape;
	InvalidShape.Radius =
		std::numeric_limits<float>::quiet_NaN();
	TArray<uint8> ValidMask = {1, 1, 1};
	TestFalse(
		TEXT("Non-finite damage shapes are rejected"),
		MatterFlux::FragmentGeometry::ApplyDamageShape(
			ValidMask,
			3,
			1,
			10.0f,
			InvalidShape));
	TestTrue(
		TEXT("Rejected shape leaves the mask unchanged"),
		ValidMask == TArray<uint8>({1, 1, 1}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxScaledCircleDamageTest, "MatterFlux.Fragment.Damage.CircleRespectsRelativeTransformScale", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxScaledCircleDamageTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Mask = { 1, 1, 1 };
	FFragmentDamageShape Shape;
	Shape.Type = EFragmentDamageShapeType::Circle;
	Shape.WorldTransform = FTransform(FQuat::Identity, FVector::ZeroVector, FVector(0.5f, 0.5f, 0.5f));
	Shape.Radius = 15.0f;

	TestTrue(TEXT("Scaled circle removes at least one cell"),
		MatterFlux::FragmentGeometry::ApplyDamageShape(Mask, 3, 1, 10.0f, Shape));
	TestTrue(TEXT("Relative shape scale keeps world-space radius semantics"),
		Mask == TArray<uint8>({ 1, 0, 1 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxLineSplitTest, "MatterFlux.Fragment.Damage.LineSplitsMask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLineSplitTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Mask;
	Mask.Init(1, 16 * 16);
	FFragmentDamageShape Shape;
	Shape.Type = EFragmentDamageShapeType::Line;
	Shape.WorldTransform = FTransform::Identity;
	Shape.Extents.X = 200.0;
	Shape.Thickness = 20.0f;
	MatterFlux::FragmentGeometry::ApplyDamageShape(Mask, 16, 16, 10.0f, Shape);
	TArray<MatterFlux::FragmentGeometry::FFragmentComponent> Components;
	MatterFlux::FragmentGeometry::ExtractConnectedComponents(Mask, 16, 16, Components);
	TestEqual(TEXT("Center line creates two components"), Components.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSingleCellLineRasterTest,
	"MatterFlux.Fragment.Damage.SingleCellLineUsesTargetSourceResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxSingleCellLineRasterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	constexpr int32 Side = 31;
	for (const float CellSize : {8.0f, 10.0f, 17.0f, 18.0f})
	{
		for (const bool bVertical : {false, true})
		{
			TArray<uint8> Mask;
			Mask.Init(1, Side * Side);
			FFragmentDamageShape Shape;
			Shape.Type = EFragmentDamageShapeType::Line;
			Shape.WorldTransform = FTransform(
				bVertical
					? FQuat(FVector::YAxisVector, UE_HALF_PI)
					: FQuat::Identity,
				FVector(2.1f, 0.0f, 3.2f));
			Shape.Extents.X = 120.0f;
			Shape.Thickness = 10.0f;
			Shape.bSingleCellLine = true;
			if (!TestTrue(
				*FString::Printf(
					TEXT("%.0f cm %s line changes the mask"),
					CellSize,
					bVertical ? TEXT("vertical") : TEXT("horizontal")),
				MatterFlux::FragmentGeometry::ApplyDamageShape(
					Mask,
					Side,
					Side,
					CellSize,
					Shape)))
			{
				continue;
			}

			TSet<int32> RemovedColumns;
			TSet<int32> RemovedRows;
			int32 RemovedCells = 0;
			for (int32 Y = 0; Y < Side; ++Y)
			{
				for (int32 X = 0; X < Side; ++X)
				{
					if (Mask[Y * Side + X] != 0)
					{
						continue;
					}
					RemovedColumns.Add(X);
					RemovedRows.Add(Y);
					++RemovedCells;
				}
			}
			TestTrue(TEXT("Single-cell line has visible length"),
				RemovedCells > 1);
			TestEqual(
				*FString::Printf(
					TEXT("%.0f cm %s cut occupies exactly one cell across"),
					CellSize,
					bVertical ? TEXT("vertical") : TEXT("horizontal")),
				bVertical ? RemovedColumns.Num() : RemovedRows.Num(),
				1);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxHorizontalWorldLineOrientationTest,
	"MatterFlux.Fragment.Damage.HorizontalWorldLineNeverTransposesTreeCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxHorizontalWorldLineOrientationTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 Width = 8;
	constexpr int32 Height = 12;
	constexpr float CellSize = 18.0f;
	for (const float SourceYaw : {0.0f, 45.0f, 90.0f, 135.0f})
	{
		const FTransform SourceTransform(
			FRotator(0.0f, SourceYaw, 0.0f),
			FVector(240.0f, -170.0f, 300.0f));
		for (const float CutYaw : {0.0f, 45.0f, 90.0f, 135.0f})
		{
			TArray<uint8> Mask;
			Mask.Init(1, Width * Height);
			FFragmentDamageShape WorldShape;
			WorldShape.Type = EFragmentDamageShapeType::Line;
			WorldShape.WorldTransform = FTransform(
				FRotator(0.0f, CutYaw, 0.0f),
				SourceTransform.GetLocation());
			WorldShape.Extents.X = 1200.0f;
			WorldShape.Thickness = CellSize * 1.1f;
			FFragmentDamageShape LocalShape = WorldShape;
			LocalShape.WorldTransform = WorldShape.WorldTransform
				.GetRelativeTransform(SourceTransform);
			if (!TestTrue(
				*FString::Printf(
					TEXT("Horizontal world cut changes yaw %.0f source at yaw %.0f"),
					CutYaw,
					SourceYaw),
				MatterFlux::FragmentGeometry::ApplyDamageShape(
					Mask,
					Width,
					Height,
					CellSize,
					LocalShape)))
			{
				continue;
			}
			FIntPoint Minimum(MAX_int32, MAX_int32);
			FIntPoint Maximum(MIN_int32, MIN_int32);
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					if (Mask[Y * Width + X] != 0)
					{
						continue;
					}
					Minimum.X = FMath::Min(Minimum.X, X);
					Minimum.Y = FMath::Min(Minimum.Y, Y);
					Maximum.X = FMath::Max(Maximum.X, X);
					Maximum.Y = FMath::Max(Maximum.Y, Y);
				}
			}
			const int32 RemovedWidth = Maximum.X - Minimum.X + 1;
			const int32 RemovedHeight = Maximum.Y - Minimum.Y + 1;
			TestTrue(
				*FString::Printf(
					TEXT("Horizontal cut stays a row band (source yaw %.0f, cut yaw %.0f, span %dx%d)"),
					SourceYaw,
					CutYaw,
					RemovedWidth,
					RemovedHeight),
				RemovedWidth >= Width - 1 && RemovedHeight <= 2);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxEightNeighborTest, "MatterFlux.Fragment.Components.EightNeighborConnectivity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxEightNeighborTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Mask = MakeMask(3, 3, { FIntPoint(0, 0), FIntPoint(1, 1) });
	TArray<MatterFlux::FragmentGeometry::FFragmentComponent> Components;
	MatterFlux::FragmentGeometry::ExtractConnectedComponents(Mask, 3, 3, Components);
	TestEqual(TEXT("Diagonal cells remain one 8-neighbor component"), Components.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxLShapeGeometryTest, "MatterFlux.Fragment.Geometry.LShapePreservesArea", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxLShapeGeometryTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Mask = MakeMask(2, 2, { FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(0, 1) });
	FFragmentGeometry2D Geometry;
	TestTrue(TEXT("L-shaped mask should triangulate"), MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(Mask, 2, 2, 1.0f, Geometry));
	TestEqual(TEXT("L-shaped triangle area equals three solid cells"), TriangleArea(Geometry), 3.0, 0.0001);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxRingGeometryTest, "MatterFlux.Fragment.Geometry.RingPreservesHole", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxRingGeometryTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Mask;
	Mask.Init(1, 9);
	Mask[4] = 0;
	FFragmentGeometry2D Geometry;
	TestTrue(TEXT("Ring mask should triangulate"), MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(Mask, 3, 3, 1.0f, Geometry));
	TestEqual(TEXT("Ring has one outer contour"), Geometry.OuterContours.Num(), 1);
	TestEqual(TEXT("Ring has one hole contour"), Geometry.HoleContours.Num(), 1);
	TestEqual(TEXT("Ring triangle area excludes the center cell"), TriangleArea(Geometry), 8.0, 0.0001);
	for (int32 Index = 0; Index < Geometry.TriangleIndices.Num(); Index += 3)
	{
		const FVector2D& A = Geometry.Vertices2D[Geometry.TriangleIndices[Index]];
		const FVector2D& B = Geometry.Vertices2D[Geometry.TriangleIndices[Index + 1]];
		const FVector2D& C = Geometry.Vertices2D[Geometry.TriangleIndices[Index + 2]];
		TestFalse(TEXT("No triangle fills the hole center"), TriangleContainsPoint(FVector2D::ZeroVector, A, B, C));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxDiagonalGeometryTest, "MatterFlux.Fragment.Geometry.DiagonalTouchProducesStableOuters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDiagonalGeometryTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Mask = MakeMask(2, 2, { FIntPoint(0, 0), FIntPoint(1, 1) });
	FFragmentGeometry2D A;
	FFragmentGeometry2D B;
	TestTrue(TEXT("Diagonal mask triangulates"), MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(Mask, 2, 2, 1.0f, A));
	TestTrue(TEXT("Repeated diagonal mask triangulates"), MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(Mask, 2, 2, 1.0f, B));
	TestEqual(TEXT("8-neighbor component may produce two outer contours"), A.OuterContours.Num(), 2);
	TestTrue(TEXT("Outer contour ordering is stable"), A.OuterContours.Num() == B.OuterContours.Num() && A.OuterContours[0].Vertices == B.OuterContours[0].Vertices && A.OuterContours[1].Vertices == B.OuterContours[1].Vertices);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxDeterministicPayloadTest, "MatterFlux.Fragment.Payload.IsFullyDeterministic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDeterministicPayloadTest::RunTest(const FString& Parameters)
{
	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	Component.Min = FIntPoint(0, 0);
	Component.Max = FIntPoint(1, 1);
	Component.Cells = { FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(0, 1) };
	const TArray<MatterFlux::FragmentGeometry::FFragmentComponent> Components = { Component };
	const FGuid SourceId(1, 2, 3, 4);
	TArray<FFragmentSpawnPayload> A;
	TArray<FFragmentSpawnPayload> B;
	TestTrue(TEXT("First payload build succeeds"), MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
		Components, SourceId, FTransform::Identity, 2, 2, 7, 10.0f, 1, 16, FVector::ZeroVector, 1000.0f, 42, A));
	TestTrue(TEXT("Second payload build succeeds"), MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
		Components, SourceId, FTransform::Identity, 2, 2, 7, 10.0f, 1, 16, FVector::ZeroVector, 1000.0f, 42, B));
	if (!TestEqual(TEXT("Payload counts match"), A.Num(), B.Num()) || A.Num() != 1) return false;
	TestEqual(TEXT("Fragment id is deterministic"), A[0].FragmentId, B[0].FragmentId);
	TestTrue(TEXT("Face vertices are deterministic"), A[0].Vertices2D == B[0].Vertices2D);
	TestTrue(TEXT("Triangles are deterministic"), A[0].TriangleIndices == B[0].TriangleIndices);
	TestEqual(TEXT("Mass is deterministic"), A[0].Mass, B[0].Mass);
	TestEqual(TEXT("Thickness is deterministic"), A[0].Thickness, B[0].Thickness);
	TestTrue(TEXT("Linear velocity is deterministic"), A[0].InitialLinearVelocity.Equals(B[0].InitialLinearVelocity, 0.0));
	TestTrue(TEXT("Angular velocity is deterministic"), A[0].InitialAngularVelocity.Equals(B[0].InitialAngularVelocity, 0.0));
	TestTrue(TEXT("Outer contours are deterministic"), A[0].OuterContours.Num() == B[0].OuterContours.Num() && A[0].OuterContours[0].Vertices == B[0].OuterContours[0].Vertices);

	TArray<FFragmentSpawnPayload> VoxelPayloads;
	TestTrue(
		TEXT("Voxel payload preserves its detached component mask"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			Components, SourceId, FTransform::Identity, 2, 2, 7, 10.0f,
			1, 16, FVector::ZeroVector, 1000.0f, 42, VoxelPayloads,
			EFragmentSourceGeometryStyle::VoxelBlocks));
	if (TestEqual(TEXT("One voxel payload is built"), VoxelPayloads.Num(), 1))
	{
		TestTrue(
			TEXT("Detached voxel mask is valid and tightly cropped"),
			VoxelPayloads[0].DetachedVoxelMask.IsValid()
				&& VoxelPayloads[0].DetachedVoxelMask.Width == 2
				&& VoxelPayloads[0].DetachedVoxelMask.Height == 2
				&& VoxelPayloads[0].DetachedVoxelMask.SolidMask
					== TArray<uint8>({1, 1, 1, 0}));
	}

	MatterFlux::FragmentGeometry::FFragmentComponent Left;
	Left.Min = Left.Max = FIntPoint(0, 0);
	Left.Cells = { FIntPoint(0, 0) };
	MatterFlux::FragmentGeometry::FFragmentComponent Right;
	Right.Min = Right.Max = FIntPoint(2, 0);
	Right.Cells = { FIntPoint(2, 0) };
	const TArray<MatterFlux::FragmentGeometry::FFragmentComponent> EqualShapeComponents = { Left, Right };
	TArray<FFragmentSpawnPayload> EqualShapePayloads;
	TestTrue(TEXT("Equal-shaped components build"), MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
		EqualShapeComponents, SourceId, FTransform::Identity, 3, 1, 7, 10.0f, 1, 16, FVector::ZeroVector, 1000.0f, 42, EqualShapePayloads));
	TestTrue(TEXT("Absolute component geometry keeps deterministic ids unique"), EqualShapePayloads.Num() == 2 && EqualShapePayloads[0].FragmentId != EqualShapePayloads[1].FragmentId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxZeroPowerPayloadTest, "MatterFlux.Fragment.Payload.ZeroPowerProducesNoMotion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxZeroPowerPayloadTest::RunTest(const FString& Parameters)
{
	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	Component.Cells = { FIntPoint(0, 0) };
	const TArray<MatterFlux::FragmentGeometry::FFragmentComponent> Components = { Component };
	TArray<FFragmentSpawnPayload> Payloads;
	TestTrue(TEXT("A zero-power payload can still be built"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			Components,
			FGuid(1, 2, 3, 4),
			FTransform::Identity,
			1,
			1,
			1,
			10.0f,
			1,
			16,
			FVector::ZeroVector,
			0.0f,
			42,
			Payloads));
	if (!TestEqual(TEXT("One payload was built"), Payloads.Num(), 1))
	{
		return false;
	}

	TestTrue(TEXT("Zero power produces no linear velocity"), Payloads[0].InitialLinearVelocity.IsZero());
	TestTrue(TEXT("Zero power produces no angular velocity"), Payloads[0].InitialAngularVelocity.IsZero());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxExtremeDamagePayloadTest,
	"MatterFlux.Fragment.Payload.RejectsNonFiniteDerivedPhysics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxExtremeDamagePayloadTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	Component.Cells = {FIntPoint(1, 0)};
	TArray<FFragmentSpawnPayload> Payloads;
	Payloads.AddDefaulted();
	const FTransform ExtremeFiniteTransform(
		FQuat::Identity,
		FVector::ZeroVector,
		FVector(std::numeric_limits<double>::max()));
	TestFalse(
		TEXT("Finite inputs cannot produce an infinite physics payload"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{Component},
			FGuid(1, 2, 3, 4),
			ExtremeFiniteTransform,
			2,
			1,
			1,
			10.0f,
			1,
			1,
			FVector(1.0, 0.0, 0.0),
			1.0f,
			42,
			Payloads));
	TestEqual(
		TEXT("Rejected physics derivation leaves no partial payload"),
		Payloads.Num(),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxComponentTieBreakTest, "MatterFlux.Fragment.Payload.ComponentTieBreakIsDeterministic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxComponentTieBreakTest::RunTest(const FString& Parameters)
{
	MatterFlux::FragmentGeometry::FFragmentComponent LeftL;
	LeftL.Cells = {
		FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(2, 0),
		FIntPoint(0, 1), FIntPoint(0, 2)
	};
	MatterFlux::FragmentGeometry::FFragmentComponent RightL;
	RightL.Cells = {
		FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(2, 0),
		FIntPoint(2, 1), FIntPoint(2, 2)
	};

	TArray<FFragmentSpawnPayload> Forward;
	TArray<FFragmentSpawnPayload> Reversed;
	const FGuid SourceId(1, 2, 3, 4);
	TestTrue(TEXT("Forward component order builds"), MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
		{ LeftL, RightL }, SourceId, FTransform::Identity,
		3, 3, 7, 10.0f, 1, 16, FVector::ZeroVector, 1000.0f, 42, Forward));
	TestTrue(TEXT("Reversed component order builds"), MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
		{ RightL, LeftL }, SourceId, FTransform::Identity,
		3, 3, 7, 10.0f, 1, 16, FVector::ZeroVector, 1000.0f, 42, Reversed));
	if (!TestEqual(TEXT("Both builds produce two payloads"), Forward.Num(), 2)
		|| !TestEqual(TEXT("Reversed build produces the same count"), Reversed.Num(), Forward.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Forward.Num(); ++Index)
	{
		TestEqual(TEXT("Geometry tie-break keeps payload ids in the same order"), Forward[Index].FragmentId, Reversed[Index].FragmentId);
		TestTrue(TEXT("Geometry tie-break keeps payload vertices in the same order"), Forward[Index].Vertices2D == Reversed[Index].Vertices2D);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxFragmentBudgetTest, "MatterFlux.Fragment.Payload.EnforcesSixteenFragmentBudget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentBudgetTest::RunTest(const FString& Parameters)
{
	TArray<MatterFlux::FragmentGeometry::FFragmentComponent> Components;
	for (int32 X = 0; X < MatterFlux::FragmentGeometry::MaximumFragmentCount + 1; ++X)
	{
		MatterFlux::FragmentGeometry::FFragmentComponent& Component = Components.AddDefaulted_GetRef();
		Component.Cells = { FIntPoint(X, 0) };
	}

	TArray<FFragmentSpawnPayload> Payloads;
	TestTrue(TEXT("Oversized fragment request still builds within the runtime budget"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			Components, FGuid(1, 2, 3, 4), FTransform::Identity,
			Components.Num(), 1, 1, 10.0f, 1, 100,
			FVector::ZeroVector, 100.0f, 7, Payloads));
	TestEqual(TEXT("Payload count is capped at the shared sixteen-fragment budget"),
		Payloads.Num(), MatterFlux::FragmentGeometry::MaximumFragmentCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRadialColumnMeshTest,
	"MatterFlux.Fragment.Geometry.RadialColumnsAreFacetedAndValid",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxRadialColumnMeshTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<uint8> Mask = {
		0, 1, 1, 1, 0,
		0, 1, 1, 1, 0,
		0, 0, 1, 0, 0,
		0, 0, 1, 0, 0
	};
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	int32 PrimaryIndexCount = 0;
	TestTrue(TEXT("A trunk mask builds radial geometry"),
		MatterFlux::FragmentGeometry::BuildRadialColumnMeshFromMask(
			Mask, 5, 4, 10.0f,
			Vertices, Triangles, Normals, UVs, PrimaryIndexCount));
	TestTrue(TEXT("Radial geometry separates body and cap sections"),
		PrimaryIndexCount > 0 && PrimaryIndexCount < Triangles.Num());
	TestEqual(TEXT("Every radial vertex has a normal"),
		Normals.Num(), Vertices.Num());
	TestEqual(TEXT("Every radial vertex has a UV"),
		UVs.Num(), Vertices.Num());
	for (const int32 Index : Triangles)
	{
		TestTrue(TEXT("Every radial triangle index is valid"),
			Vertices.IsValidIndex(Index));
	}
	TestTrue(TEXT("The body contains normals around more than one depth plane"),
		Normals.ContainsByPredicate([](const FVector& Normal)
		{
			return FMath::Abs(Normal.Y) > 0.5f;
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxSourceMaskNetSerializationTest,
	"MatterFlux.Fragment.Network.SourceMaskUsesBoundedBitPacking",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxSourceMaskNetSerializationTest::RunTest(
	const FString& Parameters)
{
	FFragmentSourceMask Source;
	Source.Width = 256;
	Source.Height = 256;
	Source.CellSize = 4.0f;
	Source.MinFragmentAreaPixels = 3;
	Source.MaxFragmentsPerBreak = 16;
	Source.SupportMode = EFragmentSupportMode::Bottom;
	Source.GeometryStyle = EFragmentSourceGeometryStyle::VoxelBlocks;
	Source.SolidMask.SetNumUninitialized(
		Source.Width * Source.Height);
	for (int32 Index = 0; Index < Source.SolidMask.Num(); ++Index)
	{
		Source.SolidMask[Index] =
			static_cast<uint8>((Index * 17 + Index / 31) & 1);
	}

	FBitWriter Writer(0, true);
	bool bSaveSucceeded = false;
	TestTrue(TEXT("Maximum source mask serializes"),
		Source.NetSerialize(Writer, nullptr, bSaveSucceeded));
	TestTrue(TEXT("Source mask save reports success"),
		bSaveSucceeded && !Writer.IsError());
	TestTrue(TEXT("Maximum source mask stays below ten KiB"),
		Writer.GetNumBits() <= 10 * 1024 * 8);

	FBitReader Reader(
		Writer.GetData(),
		Writer.GetNumBits());
	FFragmentSourceMask RoundTripped;
	bool bLoadSucceeded = false;
	TestTrue(TEXT("Packed source mask deserializes"),
		RoundTripped.NetSerialize(
			Reader,
			nullptr,
			bLoadSucceeded));
	TestTrue(TEXT("Source mask load reports success"),
		bLoadSucceeded && !Reader.IsError());
	TestEqual(TEXT("Width round trips"),
		RoundTripped.Width,
		Source.Width);
	TestEqual(TEXT("Height round trips"),
		RoundTripped.Height,
		Source.Height);
	TestEqual(TEXT("Cell size round trips"),
		RoundTripped.CellSize,
		Source.CellSize);
	TestEqual(TEXT("Geometry style round trips"),
		RoundTripped.GeometryStyle,
		Source.GeometryStyle);
	TestTrue(TEXT("Solid cells round trip exactly"),
		RoundTripped.SolidMask == Source.SolidMask);

	FFragmentSourceMask Invalid = Source;
	Invalid.SupportMode =
		static_cast<EFragmentSupportMode>(255);
	TestFalse(TEXT("Unknown support mode is rejected"),
		Invalid.HasValidLayout());

	FBitWriter InvalidWriter(0, true);
	int32 InvalidWidth = 257;
	int32 Height = 1;
	float CellSize = 1.0f;
	int32 MinFragmentArea = 1;
	int32 MaxFragments = 1;
	uint8 SupportMode =
		static_cast<uint8>(EFragmentSupportMode::Bottom);
	uint8 GeometryStyle =
		static_cast<uint8>(EFragmentSourceGeometryStyle::ExtrudedMask);
	InvalidWriter << InvalidWidth;
	InvalidWriter << Height;
	InvalidWriter << CellSize;
	InvalidWriter << MinFragmentArea;
	InvalidWriter << MaxFragments;
	InvalidWriter << SupportMode;
	InvalidWriter << GeometryStyle;
	FBitReader InvalidReader(
		InvalidWriter.GetData(),
		InvalidWriter.GetNumBits());
	FFragmentSourceMask ReusedTarget = Source;
	bool bInvalidLoadSucceeded = true;
	TestFalse(
		TEXT("Invalid network metadata is rejected"),
		ReusedTarget.NetSerialize(
			InvalidReader,
			nullptr,
			bInvalidLoadSucceeded));
	TestFalse(
		TEXT("Invalid network metadata reports failure"),
		bInvalidLoadSucceeded);
	TestEqual(
		TEXT("Failed network load clears stale width"),
		ReusedTarget.Width,
		0);
	TestTrue(
		TEXT("Failed network load clears stale mask data"),
		ReusedTarget.SolidMask.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxReplicationBudgetTest, "MatterFlux.Fragment.Payload.RejectsGeometryOutsideReplicationBudget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxReplicationBudgetTest::RunTest(const FString& Parameters)
{
	constexpr int32 Width = 24;
	constexpr int32 Height = 24;
	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			if ((X + Y) % 2 == 0)
			{
				Component.Cells.Add(FIntPoint(X, Y));
			}
		}
	}

	TArray<FFragmentSpawnPayload> Payloads;
	Payloads.AddDefaulted();
	TestFalse(TEXT("A single high-boundary component cannot exceed the initial replication budget"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{ Component }, FGuid(1, 2, 3, 4), FTransform::Identity,
			Width, Height, 1, 10.0f, 1, 16,
			FVector::ZeroVector, 100.0f, 7, Payloads));
	TestEqual(TEXT("Replication-budget rejection leaves no partial payloads"), Payloads.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxExtrusionTest, "MatterFlux.Fragment.Mesh.SideNormalsAndIndicesAreValid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxExtrusionTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> FaceVertices = {
		FVector2D(-1.0, -1.0), FVector2D(-1.0, 1.0), FVector2D(1.0, -1.0), FVector2D(1.0, 1.0)
	};
	const TArray<int32> FaceTriangles = { 0, 2, 3, 0, 3, 1 };
	FFragmentContour Outer;
	Outer.Vertices = { FVector2D(-1.0, -1.0), FVector2D(1.0, -1.0), FVector2D(1.0, 1.0), FVector2D(-1.0, 1.0) };
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TestTrue(TEXT("Valid face extrudes"), MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		FaceVertices, FaceTriangles, { Outer }, {}, 2.0f, Vertices, Triangles, Normals, UVs));
	for (const int32 Index : Triangles) TestTrue(TEXT("Every triangle index is valid"), Vertices.IsValidIndex(Index));
	const int32 SideStart = FaceVertices.Num() * 2;
	for (int32 EdgeIndex = 0; EdgeIndex < Outer.Vertices.Num(); ++EdgeIndex)
	{
		const FVector2D Edge = Outer.Vertices[(EdgeIndex + 1) % Outer.Vertices.Num()] - Outer.Vertices[EdgeIndex];
		const FVector Expected = FVector(Edge.Y, 0.0, -Edge.X).GetSafeNormal();
		TestTrue(TEXT("Side normal points outward"), FVector::DotProduct(Expected, Normals[SideStart + EdgeIndex * 4]) > 0.999);
	}
	TArray<FVector> InvalidVertices;
	TArray<int32> InvalidTriangles;
	TArray<FVector> InvalidNormals;
	TArray<FVector2D> InvalidUVs;
	TestFalse(TEXT("Invalid face indices are rejected"), MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		FaceVertices, { 0, 1, 99 }, { Outer }, {}, 2.0f, InvalidVertices, InvalidTriangles, InvalidNormals, InvalidUVs));
	TestEqual(TEXT("Rejected mesh has no vertices"), InvalidVertices.Num(), 0);

	TArray<FVector> DegenerateVertices = { FVector::ZeroVector };
	TArray<int32> DegenerateTriangles = { 0 };
	TArray<FVector> DegenerateNormals = { FVector::UpVector };
	TArray<FVector2D> DegenerateUVs = { FVector2D::ZeroVector };
	TestFalse(TEXT("Degenerate face triangles are rejected"), MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		FaceVertices, { 0, 0, 1 }, { Outer }, {}, 2.0f,
		DegenerateVertices, DegenerateTriangles, DegenerateNormals, DegenerateUVs));
	TestEqual(TEXT("Rejected degenerate mesh clears every output"),
		DegenerateVertices.Num() + DegenerateTriangles.Num() + DegenerateNormals.Num() + DegenerateUVs.Num(), 0);

	TArray<FVector2D> NonFiniteFace = FaceVertices;
	NonFiniteFace[0].X = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("Non-finite face vertices are rejected"), MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		NonFiniteFace, FaceTriangles, { Outer }, {}, 2.0f,
		DegenerateVertices, DegenerateTriangles, DegenerateNormals, DegenerateUVs));
	TestFalse(TEXT("A face without boundary rings is rejected"), MatterFlux::FragmentGeometry::BuildExtrudedMesh(
		FaceVertices, FaceTriangles, {}, {}, 2.0f,
		DegenerateVertices, DegenerateTriangles, DegenerateNormals, DegenerateUVs));
	TestFalse(TEXT("Clockwise face triangles are rejected because their fixed face normals would be wrong"),
		MatterFlux::FragmentGeometry::BuildExtrudedMesh(
			FaceVertices, { 0, 3, 2 }, { Outer }, {}, 2.0f,
			DegenerateVertices, DegenerateTriangles, DegenerateNormals, DegenerateUVs));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxFailureAtomicityTest, "MatterFlux.Fragment.Geometry.FailuresClearOutputs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxFailureAtomicityTest::RunTest(const FString& Parameters)
{
	FFragmentGeometry2D Geometry;
	Geometry.Vertices2D.Add(FVector2D(1.0, 1.0));
	Geometry.TriangleIndices.Add(0);
	TestFalse(TEXT("Invalid mask dimensions fail"), MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
		{ 1, 1, 1 }, 2, 2, 1.0f, Geometry));
	TestEqual(TEXT("Failed geometry build leaves no partial geometry"),
		Geometry.Vertices2D.Num() + Geometry.TriangleIndices.Num() + Geometry.OuterContours.Num()
		+ Geometry.HoleContours.Num() + Geometry.CollisionContours.Num(), 0);

	MatterFlux::FragmentGeometry::FFragmentComponent Valid;
	Valid.Cells = { FIntPoint(0, 0) };
	MatterFlux::FragmentGeometry::FFragmentComponent Invalid;
	Invalid.Cells = { FIntPoint(2, 0) };
	TArray<FFragmentSpawnPayload> Payloads;
	Payloads.AddDefaulted();
	TestFalse(TEXT("A malformed later component rejects the whole payload batch"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{ Valid, Invalid }, FGuid(1, 2, 3, 4), FTransform::Identity,
			2, 1, 1, 10.0f, 1, 16, FVector::ZeroVector, 100.0f, 7, Payloads));
	TestEqual(TEXT("Failed payload batch is atomic"), Payloads.Num(), 0);

	Payloads.AddDefaulted();
	TestFalse(TEXT("A malformed component is rejected even when it is below the debris area threshold"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{ Invalid }, FGuid(1, 2, 3, 4), FTransform::Identity,
			2, 1, 1, 10.0f, 2, 16, FVector::ZeroVector, 100.0f, 7, Payloads));
	TestEqual(TEXT("Filtered malformed input also leaves the payload batch empty"), Payloads.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatterFluxDerivedBoundsTest, "MatterFlux.Fragment.Payload.DerivesBoundsFromCells", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxDerivedBoundsTest::RunTest(const FString& Parameters)
{
	MatterFlux::FragmentGeometry::FFragmentComponent Component;
	Component.Cells = { FIntPoint(2, 0) };
	Component.Min = FIntPoint(-100, -100);
	Component.Max = FIntPoint(100, 100);
	TArray<FFragmentSpawnPayload> Payloads;
	TestTrue(TEXT("Payload ignores stale cached component bounds"),
		MatterFlux::FragmentGeometry::BuildSpawnPayloadsFromComponents(
			{ Component }, FGuid(1, 2, 3, 4), FTransform::Identity,
			3, 1, 1, 10.0f, 1, 16, FVector::ZeroVector, 100.0f, 7, Payloads));
	if (!TestEqual(TEXT("One payload is produced"), Payloads.Num(), 1)) return false;
	TestTrue(TEXT("Payload center comes from its cell coordinates"),
		Payloads[0].InitialTransform.GetLocation().Equals(FVector(10.0, 0.0, 0.0), 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxVoxelBlockMeshTest,
	"MatterFlux.Fragment.Geometry.VoxelBlocksBuildIndependentCubes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxVoxelBlockMeshTest::RunTest(const FString& Parameters)
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	int32 PrimaryIndexCount = 0;
	TestTrue(TEXT("Two solid cells build as one batched voxel mesh"),
		MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromMask(
			{ 1, 1 }, 2, 1, 10.0f,
			Vertices, Triangles, Normals, UVs, PrimaryIndexCount));
	TestEqual(TEXT("Adjacent cells omit their two shared internal quad faces"),
		Vertices.Num(), 40);
	TestEqual(TEXT("Each cell contributes twelve triangle indices per side group"),
		PrimaryIndexCount, 24);
	TestEqual(TEXT("Only the ten exterior quads contribute triangles"),
		Triangles.Num(), 60);
	TestEqual(TEXT("Every voxel vertex has a normal"),
		Normals.Num(), Vertices.Num());
	TestEqual(TEXT("Every voxel vertex has a UV"),
		UVs.Num(), Vertices.Num());
	for (const int32 Index : Triangles)
	{
		TestTrue(TEXT("Every voxel triangle index is valid"),
			Vertices.IsValidIndex(Index));
	}
	for (int32 Index = 0; Index < Triangles.Num(); Index += 3)
	{
		const int32 A = Triangles[Index];
		const int32 B = Triangles[Index + 1];
		const int32 C = Triangles[Index + 2];
		const FVector FacingNormal = FVector::CrossProduct(
			Vertices[C] - Vertices[A],
			Vertices[B] - Vertices[A]).GetSafeNormal();
		TestTrue(
			TEXT("Every voxel face uses UE outward-facing triangle winding"),
			FVector::DotProduct(FacingNormal, Normals[A]) > 0.99f);
	}

	bool bHasSharedInternalSide = false;
	for (int32 Index = 0; Index < Vertices.Num(); ++Index)
	{
		bHasSharedInternalSide |= FMath::IsNearlyZero(Vertices[Index].X)
			&& FMath::Abs(Normals[Index].X) > 0.99f;
	}
	TestFalse(TEXT("No X-normal face remains on the shared X=0 plane"),
		bHasSharedInternalSide);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxVoxelVolumeMeshTest,
	"MatterFlux.Fragment.Geometry.VoxelVolumeCullsDepthInternalFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxVoxelVolumeMeshTest::RunTest(const FString& Parameters)
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	int32 PrimaryIndexCount = 0;
	TestTrue(TEXT("Two depth-adjacent cells build as one voxel volume"),
		MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromCells(
			{FIntVector(0, 0, 0), FIntVector(0, 1, 0)},
			10.0f,
			Vertices,
			Triangles,
			Normals,
			UVs,
			PrimaryIndexCount));
	TestEqual(TEXT("Only the two outer depth faces remain"),
		PrimaryIndexCount, 12);
	TestEqual(TEXT("The shared depth plane contributes no triangles"),
		Triangles.Num(), 60);
	TestEqual(TEXT("Ten exposed quads contribute forty vertices"),
		Vertices.Num(), 40);

	bool bHasInternalDepthFace = false;
	for (int32 Index = 0; Index < Vertices.Num(); ++Index)
	{
		bHasInternalDepthFace |= FMath::IsNearlyEqual(Vertices[Index].Y, 5.0f)
			&& FMath::Abs(Normals[Index].Y) > 0.99f;
	}
	TestFalse(TEXT("No Y-normal face remains between adjacent canopy slices"),
		bHasInternalDepthFace);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxVoxelMaterialBoundaryTest,
	"MatterFlux.Fragment.Geometry.VoxelMaterialBoundaryCullsHiddenFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxVoxelMaterialBoundaryTest::RunTest(const FString& Parameters)
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	int32 PrimaryIndexCount = 0;
	TestTrue(TEXT("A leaf block can occlude a neighbouring wood block"),
		MatterFlux::FragmentGeometry::BuildVoxelBlockMeshFromCellsWithOccluders(
			{FIntVector(0, 0, 0)},
			{FIntVector(1, 0, 0)},
			10.0f,
			Vertices,
			Triangles,
			Normals,
			UVs,
			PrimaryIndexCount));
	TestEqual(TEXT("The material boundary removes one quad"),
		Triangles.Num(), 30);
	bool bHasHiddenBoundaryFace = false;
	for (int32 Index = 0; Index < Vertices.Num(); ++Index)
	{
		bHasHiddenBoundaryFace |= FMath::IsNearlyEqual(Vertices[Index].X, 5.0f)
			&& Normals[Index].X > 0.99f;
	}
	TestFalse(TEXT("No wood face is emitted against the leaf neighbour"),
		bHasHiddenBoundaryFace);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReactionTopSurfaceTest,
	"MatterFlux.Fragment.Geometry.ReactionUsesOnlyTopExposedVoxelCells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxReactionTopSurfaceTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Occupied = {
		1, 1,
		1, 1,
		1, 0
	};
	const TArray<uint8> Active = {
		1, 1,
		1, 1,
		1, 0
	};
	TArray<int32> VisibleCells;
	if (!TestTrue(TEXT("Valid reaction masks are accepted"),
		MatterFlux::FragmentGeometry::GatherTopExposedActiveMaskCells(
			Occupied, Active, 2, 3, VisibleCells)))
	{
		return false;
	}
	TestTrue(TEXT("Only the top exposed cell of each occupied column emits flame"),
		VisibleCells == TArray<int32>({3, 4}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReactionVisibleSurfaceTest,
	"MatterFlux.Fragment.Geometry.InternalReactionDoesNotTeleportToSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxReactionVisibleSurfaceTest::RunTest(
	const FString& Parameters)
{
	const TArray<uint8> Occupied = {
		1, 1,
		1, 1,
		1, 0
	};
	const TArray<uint8> Active = {
		1, 0,
		0, 1,
		0, 0
	};
	TArray<int32> VisibleCells;
	if (!TestTrue(
		TEXT("Visible reaction masks are accepted"),
		MatterFlux::FragmentGeometry::GatherTopExposedActiveMaskCells(
			Occupied,
			Active,
			2,
			3,
			VisibleCells)))
	{
		return false;
	}
	TestTrue(
		TEXT("Internal fire does not create disconnected flame pixels on distant surface voxels"),
		VisibleCells == TArray<int32>({3}));
	return true;
}
