#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Reverse.h"
#include "Rendering/MatterFluxWholeObjectGeometry.h"

namespace
{
	MatterFlux::WholeObject::FLayer MakeLayer(
		const int32 MaterialIndex,
		const int32 Priority,
		const FIntVector& GridCell,
		const float CellSize = 10.0f)
	{
		MatterFlux::WholeObject::FLayer Layer;
		Layer.MaterialIndex = MaterialIndex;
		Layer.Priority = Priority;
		Layer.Width = 1;
		Layer.Height = 1;
		Layer.CellSize = CellSize;
		Layer.SolidMask = {1};
		Layer.LocalTransform = FTransform(
			FVector(GridCell) * CellSize);
		return Layer;
	}

	int32 CountTriangles(
		const MatterFlux::WholeObject::FBuildResult& Result)
	{
		int32 TriangleCount = 0;
		for (const MatterFlux::WholeObject::FMeshSection& Section
			: Result.Sections)
		{
			TriangleCount += Section.Triangles.Num() / 3;
		}
		return TriangleCount;
	}

	bool HasOutwardUnrealWinding(
		const MatterFlux::WholeObject::FMeshSection& Section)
	{
		if (Section.Triangles.Num() % 3 != 0
			|| Section.Normals.Num() != Section.Vertices.Num())
		{
			return false;
		}

		for (int32 Index = 0; Index < Section.Triangles.Num(); Index += 3)
		{
			const int32 A = Section.Triangles[Index];
			const int32 B = Section.Triangles[Index + 1];
			const int32 C = Section.Triangles[Index + 2];
			if (!Section.Vertices.IsValidIndex(A)
				|| !Section.Vertices.IsValidIndex(B)
				|| !Section.Vertices.IsValidIndex(C))
			{
				return false;
			}

			// UE 的 ProceduralMesh 正面使用顺时针绕序。这里的 C-A x B-A
			// 与 KismetProceduralMeshLibrary::GenerateBoxMesh 的外法线一致。
			const FVector FacingNormal = FVector::CrossProduct(
				Section.Vertices[C] - Section.Vertices[A],
				Section.Vertices[B] - Section.Vertices[A]).GetSafeNormal();
			if (FVector::DotProduct(FacingNormal, Section.Normals[A]) < 0.99f)
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWholeObjectOutwardWindingTest,
	"MatterFlux.Rendering.WholeObject.FacesUseOutwardUnrealWinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxWholeObjectOutwardWindingTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::WholeObject::FBuildResult Result;
	if (!TestTrue(TEXT("A single solid voxel compiles"),
		MatterFlux::WholeObject::BuildMesh(
			{MakeLayer(0, 0, FIntVector::ZeroValue)}, Result)))
	{
		return false;
	}

	for (const MatterFlux::WholeObject::FMeshSection& Section : Result.Sections)
	{
		TestTrue(
			TEXT("Every rasterized face points along its declared outward normal"),
			HasOutwardUnrealWinding(Section));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWholeObjectOcclusionTest,
	"MatterFlux.Rendering.WholeObject.CullsInternalFacesAcrossMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxWholeObjectOcclusionTest::RunTest(const FString& Parameters)
{
	TArray<MatterFlux::WholeObject::FLayer> Layers;
	Layers.Add(MakeLayer(0, 0, FIntVector(0, 0, 0)));
	Layers.Add(MakeLayer(1, 10, FIntVector(1, 0, 0)));
	MatterFlux::WholeObject::FBuildResult Result;
	FString Error;
	if (!TestTrue(TEXT("Adjacent multi-material object compiles"),
		MatterFlux::WholeObject::BuildMesh(Layers, Result, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Both occupied cells are preserved"),
		Result.OccupiedCellCount, 2);
	TestEqual(TEXT("The shared material boundary is removed"),
		CountTriangles(Result), 20);
	TestEqual(TEXT("Only ten exterior faces remain"),
		Result.VisibleQuadCount, 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWholeObjectGreedyMeshingTest,
	"MatterFlux.Rendering.WholeObject.MergesCompatibleCoplanarFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxWholeObjectGreedyMeshingTest::RunTest(
	const FString& Parameters)
{
	TArray<MatterFlux::WholeObject::FLayer> Layers;
	Layers.Add(MakeLayer(0, 0, FIntVector(0, 0, 0)));
	Layers.Add(MakeLayer(0, 0, FIntVector(1, 0, 0)));
	MatterFlux::WholeObject::FBuildResult Result;
	if (!TestTrue(TEXT("Compatible adjacent cells compile"),
		MatterFlux::WholeObject::BuildMesh(Layers, Result)))
	{
		return false;
	}
	TestEqual(TEXT("A two-cell cuboid compiles to six quads"),
		Result.VisibleQuadCount, 6);
	TestEqual(TEXT("A two-cell cuboid compiles to twelve triangles"),
		CountTriangles(Result), 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWholeObjectPriorityTest,
	"MatterFlux.Rendering.WholeObject.MaterialPriorityResolvesOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxWholeObjectPriorityTest::RunTest(const FString& Parameters)
{
	TArray<MatterFlux::WholeObject::FLayer> Layers;
	Layers.Add(MakeLayer(5, 1, FIntVector::ZeroValue));
	Layers.Add(MakeLayer(2, 100, FIntVector::ZeroValue));
	MatterFlux::WholeObject::FBuildResult Result;
	if (!TestTrue(TEXT("Overlapping object compiles"),
		MatterFlux::WholeObject::BuildMesh(Layers, Result)))
	{
		return false;
	}
	TestEqual(TEXT("Overlapping layers produce one voxel"),
		Result.OccupiedCellCount, 1);
	for (const MatterFlux::WholeObject::FMeshSection& Section : Result.Sections)
	{
		TestEqual(TEXT("Higher-priority material owns every visible face"),
			Section.MaterialIndex, 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWholeObjectDeterminismTest,
	"MatterFlux.Rendering.WholeObject.OutputIsDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxWholeObjectDeterminismTest::RunTest(const FString& Parameters)
{
	TArray<MatterFlux::WholeObject::FLayer> Forward;
	Forward.Add(MakeLayer(1, 10, FIntVector(0, 0, 0)));
	Forward.Add(MakeLayer(0, 0, FIntVector(0, 1, 0)));
	Forward.Add(MakeLayer(1, 10, FIntVector(0, 1, 1)));
	TArray<MatterFlux::WholeObject::FLayer> Reverse = Forward;
	Algo::Reverse(Reverse);

	MatterFlux::WholeObject::FBuildResult First;
	MatterFlux::WholeObject::FBuildResult Second;
	if (!TestTrue(TEXT("Forward input compiles"),
		MatterFlux::WholeObject::BuildMesh(Forward, First))
		|| !TestTrue(TEXT("Reverse input compiles"),
			MatterFlux::WholeObject::BuildMesh(Reverse, Second)))
	{
		return false;
	}
	TestEqual(TEXT("Section order is stable"),
		First.Sections.Num(), Second.Sections.Num());
	for (int32 Index = 0;
		Index < FMath::Min(First.Sections.Num(), Second.Sections.Num());
		++Index)
	{
		const MatterFlux::WholeObject::FMeshSection& A = First.Sections[Index];
		const MatterFlux::WholeObject::FMeshSection& B = Second.Sections[Index];
		TestEqual(TEXT("Material index is deterministic"),
			A.MaterialIndex, B.MaterialIndex);
		TestEqual(TEXT("Face role is deterministic"),
			static_cast<uint8>(A.FaceRole), static_cast<uint8>(B.FaceRole));
		TestTrue(TEXT("Vertices are deterministic"), A.Vertices == B.Vertices);
		TestTrue(TEXT("Triangles are deterministic"), A.Triangles == B.Triangles);
		TestTrue(TEXT("Normals are deterministic"), A.Normals == B.Normals);
		TestTrue(TEXT("UVs are deterministic"), A.UVs == B.UVs);
		TestTrue(TEXT("Vertex AO is deterministic"),
			A.VertexColors == B.VertexColors);
	}
	return true;
}

#endif
