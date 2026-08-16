#include "Game/MatterFluxFragmentSourceProxyComponent.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "ProceduralMeshComponent.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
	constexpr int32 ProxyBenchmarkSourceCount = 4096;
	constexpr int32 ProxyBenchmarkIterations = 8192;
	constexpr int32 ProxyBenchmarkMaskCellCount = 64;

	FGuid MakeProxyBenchmarkSourceId(const int32 SourceIndex)
	{
		return FGuid(
			static_cast<uint32>(SourceIndex + 1),
			0x9e3779b9u,
			0x85ebca6bu,
			0xc2b2ae35u);
	}

	int32 CountProxyMeshVertices(const AActor& Owner)
	{
		TArray<UProceduralMeshComponent*> Meshes;
		Owner.GetComponents(Meshes);
		int32 VertexCount = 0;
		for (UProceduralMeshComponent* Mesh : Meshes)
		{
			if (!Mesh
				|| !Mesh->ComponentHasTag(TEXT("MatterFluxFragmentSourceProxy")))
			{
				continue;
			}
			for (int32 SectionIndex = 0;
				SectionIndex < Mesh->GetNumSections();
				++SectionIndex)
			{
				if (const FProcMeshSection* Section =
					Mesh->GetProcMeshSection(SectionIndex))
				{
					VertexCount += Section->ProcVertexBuffer.Num();
				}
			}
		}
		return VertexCount;
	}

	int32 CountProxyMeshUnreferencedVertices(const AActor& Owner)
	{
		TArray<UProceduralMeshComponent*> Meshes;
		Owner.GetComponents(Meshes);
		int32 UnreferencedVertexCount = 0;
		for (UProceduralMeshComponent* Mesh : Meshes)
		{
			if (!Mesh
				|| !Mesh->ComponentHasTag(TEXT("MatterFluxFragmentSourceProxy")))
			{
				continue;
			}
			for (int32 SectionIndex = 0;
				SectionIndex < Mesh->GetNumSections();
				++SectionIndex)
			{
				const FProcMeshSection* Section =
					Mesh->GetProcMeshSection(SectionIndex);
				if (!Section)
				{
					continue;
				}
				TBitArray<> ReferencedVertices(
					false,
					Section->ProcVertexBuffer.Num());
				for (const uint32 VertexIndex : Section->ProcIndexBuffer)
				{
					if (ReferencedVertices.IsValidIndex(
						static_cast<int32>(VertexIndex)))
					{
						ReferencedVertices[VertexIndex] = true;
					}
				}
				for (const bool bReferenced : ReferencedVertices)
				{
					UnreferencedVertexCount += bReferenced ? 0 : 1;
				}
			}
		}
		return UnreferencedVertexCount;
	}

	struct FProxyMeshSnapshot
	{
		TArray<int32> SectionVertexCounts;
		TArray<int32> SectionIndexCounts;
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<uint32> Indices;

		bool operator==(const FProxyMeshSnapshot& Other) const
		{
			return SectionVertexCounts == Other.SectionVertexCounts
				&& SectionIndexCounts == Other.SectionIndexCounts
				&& Positions == Other.Positions
				&& Normals == Other.Normals
				&& UVs == Other.UVs
				&& Indices == Other.Indices;
		}
	};

	FProxyMeshSnapshot CaptureProxyMeshSnapshot(const AActor& Owner)
	{
		FProxyMeshSnapshot Snapshot;
		TArray<UProceduralMeshComponent*> Meshes;
		Owner.GetComponents(Meshes);
		for (UProceduralMeshComponent* Mesh : Meshes)
		{
			if (!Mesh
				|| !Mesh->ComponentHasTag(TEXT("MatterFluxFragmentSourceProxy")))
			{
				continue;
			}
			for (int32 SectionIndex = 0;
				SectionIndex < Mesh->GetNumSections();
				++SectionIndex)
			{
				const FProcMeshSection* Section =
					Mesh->GetProcMeshSection(SectionIndex);
				if (!Section)
				{
					continue;
				}
				Snapshot.SectionVertexCounts.Add(
					Section->ProcVertexBuffer.Num());
				Snapshot.SectionIndexCounts.Add(
					Section->ProcIndexBuffer.Num());
				for (const FProcMeshVertex& Vertex
					: Section->ProcVertexBuffer)
				{
					Snapshot.Positions.Add(Vertex.Position);
					Snapshot.Normals.Add(Vertex.Normal);
					Snapshot.UVs.Add(Vertex.UV0);
				}
				Snapshot.Indices.Append(Section->ProcIndexBuffer);
			}
		}
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceProxyAtomicApplyTest,
	"MatterFlux.Game.FragmentSourceProxyRejectsStateAtomically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceProxyAtomicApplyTest::RunTest(
	const FString& Parameters)
{
	UMatterFluxFragmentSourceProxyComponent* Proxy =
		NewObject<UMatterFluxFragmentSourceProxyComponent>();
	if (!TestNotNull(TEXT("Proxy component is created"), Proxy))
	{
		return false;
	}
	MatterFlux::PlayableLevel::FLevelFragmentSource Source;
	Source.SourceId = MakeProxyBenchmarkSourceId(0);
	Source.Mask.Width = 2;
	Source.Mask.Height = 2;
	Source.Mask.CellSize = 4.0f;
	Source.Mask.SolidMask.Init(1, 4);
	TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>> Chunks;
	Chunks.FindOrAdd(FIntPoint::ZeroValue).Add(Source);
	Proxy->SetSourceChunks(Chunks);

	TArray<uint8> ChangedRuntime = {0, 1, 1, 1};
	TArray<uint8> InvalidResidue = {1, 0, 0};
	TestEqual(
		TEXT("An invalid residue rejects the whole Source state"),
		Proxy->ApplySourceState(
			Source.SourceId,
			ChangedRuntime,
			InvalidResidue,
			TEXT("charcoal"),
			FLinearColor::Black,
			false),
		EMatterFluxFragmentSourceProxyApplyResult::Invalid);

	TArray<uint8> OriginalRuntime;
	OriginalRuntime.Init(1, 4);
	TArray<uint8> EmptyResidue;
	EmptyResidue.Init(0, 4);
	TestEqual(
		TEXT("Rejected input leaves both previously committed masks unchanged"),
		Proxy->ApplySourceState(
			Source.SourceId,
			OriginalRuntime,
			EmptyResidue,
			NAME_None,
			FLinearColor::Transparent,
			false),
		EMatterFluxFragmentSourceProxyApplyResult::Unchanged);
	TArray<uint8> ValidResidue = {1, 0, 0, 0};
	TestEqual(
		TEXT("A valid pair commits as one changed Source state"),
		Proxy->ApplySourceState(
			Source.SourceId,
			ChangedRuntime,
			ValidResidue,
			TEXT("charcoal"),
			FLinearColor::Black,
			false),
		EMatterFluxFragmentSourceProxyApplyResult::Changed);
	TestEqual(
		TEXT("Repeating the same complete Source state is idempotent"),
		Proxy->ApplySourceState(
			Source.SourceId,
			ChangedRuntime,
			ValidResidue,
			TEXT("charcoal"),
			FLinearColor::Black,
			false),
		EMatterFluxFragmentSourceProxyApplyResult::Unchanged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceProxyCompactSectionsTest,
	"MatterFlux.Game.FragmentSourceProxySectionsContainOnlyReferencedVertices",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceProxyCompactSectionsTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AActor* Owner = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("Proxy owner is spawned"), Owner))
	{
		return false;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
	Owner->SetRootComponent(Root);
	Owner->AddInstanceComponent(Root);
	Root->RegisterComponent();
	UMatterFluxFragmentSourceProxyComponent* Proxy =
		NewObject<UMatterFluxFragmentSourceProxyComponent>(Owner, TEXT("Proxy"));
	Owner->AddInstanceComponent(Proxy);
	Proxy->RegisterComponent();
	Proxy->Configure(Root, nullptr);

	MatterFlux::PlayableLevel::FLevelFragmentSource Source;
	Source.SourceId = MakeProxyBenchmarkSourceId(0);
	Source.MaterialId = TEXT("wood");
	Source.Mask.Width = 2;
	Source.Mask.Height = 2;
	Source.Mask.CellSize = 4.0f;
	Source.Mask.SolidMask.Init(1, 4);
	TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>> Chunks;
	Chunks.FindOrAdd(FIntPoint::ZeroValue).Add(Source);
	Proxy->SetSourceChunks(Chunks);
	Proxy->SetVisibleChunks({FIntPoint::ZeroValue});

	const int32 VertexCount = CountProxyMeshVertices(*Owner);
	const int32 UnreferencedVertexCount =
		CountProxyMeshUnreferencedVertices(*Owner);
	AddInfo(FString::Printf(
		TEXT("Single 2x2 proxy Source uses %d section vertices, %d unreferenced"),
		VertexCount,
		UnreferencedVertexCount));
	TestTrue(TEXT("The proxy creates visible section vertices"), VertexCount > 0);
	TestEqual(
		TEXT("Every proxy section vertex participates in at least one triangle"),
		UnreferencedVertexCount,
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceProxyCompactBatchTest,
	"MatterFlux.Game.FragmentSourceProxyCompactSectionsScaleDeterministically",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceProxyCompactBatchTest::RunTest(
	const FString& Parameters)
{
	constexpr int32 SourceCount = 64;
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Test world is created"), World))
	{
		return false;
	}
	TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>> Chunks;
	TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>& Sources =
		Chunks.FindOrAdd(FIntPoint::ZeroValue);
	Sources.Reserve(SourceCount);
	for (int32 SourceIndex = 0; SourceIndex < SourceCount; ++SourceIndex)
	{
		MatterFlux::PlayableLevel::FLevelFragmentSource& Source =
			Sources.AddDefaulted_GetRef();
		Source.SourceId = MakeProxyBenchmarkSourceId(SourceIndex);
		Source.MaterialId = TEXT("wood");
		Source.Transform.SetLocation(
			FVector(SourceIndex * 8.0f, 0.0f, 0.0f));
		Source.Mask.Width = 1;
		Source.Mask.Height = 1;
		Source.Mask.CellSize = 4.0f;
		Source.Mask.SolidMask = {1};
	}

	auto BuildProxy = [World, &Chunks](const FName OwnerName)
	{
		AActor* Owner = World->SpawnActor<AActor>();
		Owner->Rename(*OwnerName.ToString());
		USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
		Owner->SetRootComponent(Root);
		Owner->AddInstanceComponent(Root);
		Root->RegisterComponent();
		UMatterFluxFragmentSourceProxyComponent* Proxy =
			NewObject<UMatterFluxFragmentSourceProxyComponent>(
				Owner,
				TEXT("Proxy"));
		Owner->AddInstanceComponent(Proxy);
		Proxy->RegisterComponent();
		Proxy->Configure(Root, nullptr);
		Proxy->SetSourceChunks(Chunks);
		Proxy->SetVisibleChunks({FIntPoint::ZeroValue});
		return Owner;
	};
	AActor* FirstOwner = BuildProxy(TEXT("FirstProxyOwner"));
	AActor* SecondOwner = BuildProxy(TEXT("SecondProxyOwner"));
	if (!TestNotNull(TEXT("First proxy owner is created"), FirstOwner)
		|| !TestNotNull(TEXT("Second proxy owner is created"), SecondOwner))
	{
		return false;
	}

	const FProxyMeshSnapshot FirstSnapshot =
		CaptureProxyMeshSnapshot(*FirstOwner);
	const FProxyMeshSnapshot SecondSnapshot =
		CaptureProxyMeshSnapshot(*SecondOwner);
	TestEqual(
		TEXT("Each compact 1x1 Source contributes only its 24 used vertices"),
		FirstSnapshot.Positions.Num(),
		SourceCount * 24);
	TestEqual(
		TEXT("The batched proxy contains no unreferenced vertices"),
		CountProxyMeshUnreferencedVertices(*FirstOwner),
		0);
	TestTrue(
		TEXT("Repeated compact proxy builds are identical by section and field"),
		FirstSnapshot == SecondSnapshot);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceProxyCombustionDeferralTest,
	"MatterFlux.Game.FragmentSourceProxyDefersCombustionGeometryUntilFinished",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxFragmentSourceProxyCombustionDeferralTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AActor* Owner = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("Proxy owner is spawned"), Owner))
	{
		return false;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
	Owner->SetRootComponent(Root);
	Owner->AddInstanceComponent(Root);
	Root->RegisterComponent();
	UMatterFluxFragmentSourceProxyComponent* Proxy =
		NewObject<UMatterFluxFragmentSourceProxyComponent>(Owner, TEXT("Proxy"));
	Owner->AddInstanceComponent(Proxy);
	Proxy->RegisterComponent();
	Proxy->Configure(Root, nullptr);

	MatterFlux::PlayableLevel::FLevelFragmentSource Source;
	Source.SourceId = MakeProxyBenchmarkSourceId(0);
	Source.MaterialId = TEXT("wood");
	Source.Mask.Width = 2;
	Source.Mask.Height = 2;
	Source.Mask.CellSize = 4.0f;
	Source.Mask.SolidMask.Init(1, 4);
	TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>> Chunks;
	Chunks.FindOrAdd(FIntPoint::ZeroValue).Add(Source);
	Proxy->SetSourceChunks(Chunks);
	TSet<FIntPoint> VisibleChunks = {FIntPoint::ZeroValue};
	Proxy->SetVisibleChunks(VisibleChunks);
	const int32 InitialVertexCount = CountProxyMeshVertices(*Owner);
	if (!TestTrue(
		TEXT("The pristine Source creates a visible proxy mesh"),
		InitialVertexCount > 0))
	{
		return false;
	}

	TArray<uint8> CutRuntime = {0, 1, 1, 1};
	TArray<uint8> EmptyResidue;
	EmptyResidue.Init(0, 4);
	Proxy->ApplySourceState(
		Source.SourceId,
		CutRuntime,
		EmptyResidue,
		NAME_None,
		FLinearColor::Transparent,
		true);
	Proxy->FlushPendingChanges();
	TestEqual(
		TEXT("An active combustion update keeps the cached chunk mesh"),
		CountProxyMeshVertices(*Owner),
		InitialVertexCount);

	Proxy->ApplySourceState(
		Source.SourceId,
		CutRuntime,
		EmptyResidue,
		NAME_None,
		FLinearColor::Transparent,
		false);
	Proxy->FlushPendingChanges();
	TestEqual(
		TEXT("Finishing combustion remains deferred until the batch flush"),
		CountProxyMeshVertices(*Owner),
		InitialVertexCount);
	Proxy->FlushDeferredCombustionChanges();
	Proxy->FlushPendingChanges();
	TestNotEqual(
		TEXT("The finished combustion rebuilds the completed proxy geometry"),
		CountProxyMeshVertices(*Owner),
		InitialVertexCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFragmentSourceProxyLookupPerformanceTest,
	"MatterFlux.Performance.FragmentSourceProxyUpdatesIgnoreChunkPopulation",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::PerfFilter)

bool FMatterFluxFragmentSourceProxyLookupPerformanceTest::RunTest(
	const FString& Parameters)
{
	UMatterFluxFragmentSourceProxyComponent* Proxy =
		NewObject<UMatterFluxFragmentSourceProxyComponent>();
	if (!TestNotNull(TEXT("Proxy component is created"), Proxy))
	{
		return false;
	}

	TMap<
		FIntPoint,
		TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>> Chunks;
	TArray<MatterFlux::PlayableLevel::FLevelFragmentSource>& Sources =
		Chunks.FindOrAdd(FIntPoint::ZeroValue);
	Sources.Reserve(ProxyBenchmarkSourceCount);
	for (int32 SourceIndex = 0;
		SourceIndex < ProxyBenchmarkSourceCount;
		++SourceIndex)
	{
		MatterFlux::PlayableLevel::FLevelFragmentSource& Source =
			Sources.AddDefaulted_GetRef();
		Source.SourceId = MakeProxyBenchmarkSourceId(SourceIndex);
		Source.Name = TEXT("ProxyBenchmark");
		Source.MaterialId = TEXT("wood");
		Source.Mask.Width = 8;
		Source.Mask.Height = 8;
		Source.Mask.CellSize = 4.0f;
		Source.Mask.SolidMask.Init(1, ProxyBenchmarkMaskCellCount);
	}
	Proxy->SetSourceChunks(Chunks);

	const FGuid TailSourceId =
		MakeProxyBenchmarkSourceId(ProxyBenchmarkSourceCount - 1);
	TArray<uint8> RuntimeMask;
	RuntimeMask.Init(1, ProxyBenchmarkMaskCellCount);
	TArray<uint8> ResidueMask;
	ResidueMask.Init(0, ProxyBenchmarkMaskCellCount);
	bool bAllUpdatesAccepted = true;
	const double StartSeconds = FPlatformTime::Seconds();
	for (int32 Iteration = 0;
		Iteration < ProxyBenchmarkIterations;
		++Iteration)
	{
		RuntimeMask[0] = static_cast<uint8>(Iteration & 1);
		ResidueMask[0] = static_cast<uint8>((Iteration + 1) & 1);
		bAllUpdatesAccepted &= Proxy->ApplySourceState(
			TailSourceId,
			RuntimeMask,
			ResidueMask,
			TEXT("charcoal"),
			FLinearColor(0.08f, 0.07f, 0.06f),
			false)
			!= EMatterFluxFragmentSourceProxyApplyResult::Invalid;
	}
	const double ElapsedMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("Proxy tail Source: %d paired mask updates across %d chunk Sources in %.2f ms"),
		ProxyBenchmarkIterations,
		ProxyBenchmarkSourceCount,
		ElapsedMilliseconds));
	TestTrue(TEXT("Every valid proxy update is accepted"), bAllUpdatesAccepted);
	TestTrue(
		TEXT("Proxy state lookup cost does not scale with chunk population"),
		ElapsedMilliseconds < 25.0);
	return true;
}
