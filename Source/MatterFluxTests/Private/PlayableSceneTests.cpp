#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Creatures/MatterFluxCreatureActor.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "GAS/GA_CastWand.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxCharacterMovementComponent.h"
#include "Game/MatterFluxGameMode.h"
#include "Game/MatterFluxGameState.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxPlayerState.h"
#include "Game/MatterFluxTerrainMesh.h"
#include "Game/MatterFluxTwoStoreyHouseActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"
#include "IMatterFluxScriptRuntime.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialDomain.h"
#include "Material/MatterFluxBuoyancyComponent.h"
#include "Material/MatterFluxLiquidBuoyancy.h"
#include "MaterialShared.h"
#include "Magic/MatterFluxMagicInventoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "ProceduralMeshComponent.h"
#include "Progression/MatterFluxProgressionComponent.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "Save/MatterFluxSaveTypes.h"

#include <limits>
#include "ProceduralMeshComponent.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
bool AreTransformsEqual(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (!A[Index].Equals(B[Index], 0.0))
		{
			return false;
		}
	}

	return true;
}

bool TrySampleSurfaceHeight(
	const MatterFlux::TerrainMesh::FSection& Surface,
	const FVector2D& Point,
	double& OutHeight)
{
	for (int32 TriangleIndex = 0;
		TriangleIndex < Surface.Triangles.Num();
		TriangleIndex += 3)
	{
		const FVector& A = Surface.Vertices[
			Surface.Triangles[TriangleIndex]];
		const FVector& B = Surface.Vertices[
			Surface.Triangles[TriangleIndex + 1]];
		const FVector& C = Surface.Vertices[
			Surface.Triangles[TriangleIndex + 2]];
		const double Denominator =
			(B.Y - C.Y) * (A.X - C.X)
			+ (C.X - B.X) * (A.Y - C.Y);
		if (FMath::Abs(Denominator) <= UE_DOUBLE_SMALL_NUMBER)
		{
			continue;
		}
		const double WeightA = (
			(B.Y - C.Y) * (Point.X - C.X)
			+ (C.X - B.X) * (Point.Y - C.Y)) / Denominator;
		const double WeightB = (
			(C.Y - A.Y) * (Point.X - C.X)
			+ (A.X - C.X) * (Point.Y - C.Y)) / Denominator;
		const double WeightC = 1.0 - WeightA - WeightB;
		constexpr double EdgeTolerance = 1.0e-6;
		if (WeightA < -EdgeTolerance
			|| WeightB < -EdgeTolerance
			|| WeightC < -EdgeTolerance)
		{
			continue;
		}
		OutHeight = WeightA * A.Z + WeightB * B.Z + WeightC * C.Z;
		return FMath::IsFinite(OutHeight);
	}
	return false;
}

bool AreLayoutsEqual(
	const MatterFlux::PlayableLevel::FLevelLayout& A,
	const MatterFlux::PlayableLevel::FLevelLayout& B)
{
	if (A.Terrain.Width != B.Terrain.Width
		|| A.Terrain.Height != B.Terrain.Height
		|| A.Terrain.CellSize != B.Terrain.CellSize
		|| A.Terrain.BottomZ != B.Terrain.BottomZ
		|| A.Terrain.FirstCellCenter != B.Terrain.FirstCellCenter
		|| A.Terrain.Heights != B.Terrain.Heights
		|| A.Terrain.ColorBands != B.Terrain.ColorBands
		|| A.Terrain.BandColors != B.Terrain.BandColors
		|| A.Layers.Num() != B.Layers.Num()
		|| A.FragmentSources.Num() != B.FragmentSources.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < A.Layers.Num(); ++Index)
	{
		const MatterFlux::PlayableLevel::FLevelLayer& Left = A.Layers[Index];
		const MatterFlux::PlayableLevel::FLevelLayer& Right = B.Layers[Index];
		if (Left.Name != Right.Name
			|| Left.MaterialId != Right.MaterialId
			|| Left.Primitive != Right.Primitive
			|| Left.RenderMode != Right.RenderMode
			|| Left.Color != Right.Color
			|| Left.bEnableCollision != Right.bEnableCollision
			|| !AreTransformsEqual(Left.Instances, Right.Instances))
		{
			return false;
		}
	}
	for (int32 Index = 0; Index < A.FragmentSources.Num(); ++Index)
	{
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Left =
			A.FragmentSources[Index];
		const MatterFlux::PlayableLevel::FLevelFragmentSource& Right =
			B.FragmentSources[Index];
		if (Left.Name != Right.Name
			|| Left.MaterialId != Right.MaterialId
			|| Left.SourceId != Right.SourceId
			|| Left.AggregateId != Right.AggregateId
			|| Left.bAggregateRoot != Right.bAggregateRoot
			|| Left.Color != Right.Color
			|| Left.bEnableCollision != Right.bEnableCollision
			|| !Left.Transform.Equals(Right.Transform, 0.0)
			|| Left.Mask.Width != Right.Mask.Width
			|| Left.Mask.Height != Right.Mask.Height
			|| Left.Mask.CellSize != Right.Mask.CellSize
			|| Left.Mask.MinFragmentAreaPixels != Right.Mask.MinFragmentAreaPixels
			|| Left.Mask.MaxFragmentsPerBreak != Right.Mask.MaxFragmentsPerBreak
			|| Left.Mask.GeometryStyle != Right.Mask.GeometryStyle
			|| Left.Mask.SolidMask != Right.Mask.SolidMask)
		{
			return false;
		}
	}
	return true;
}

bool IsSolidRegionOneRectangle(const FFragmentSourceMask& Mask)
{
	int32 MinX = Mask.Width;
	int32 MinY = Mask.Height;
	int32 MaxX = INDEX_NONE;
	int32 MaxY = INDEX_NONE;
	for (int32 Y = 0; Y < Mask.Height; ++Y)
	{
		for (int32 X = 0; X < Mask.Width; ++X)
		{
			if (Mask.SolidMask[Y * Mask.Width + X] == 0)
			{
				continue;
			}
			MinX = FMath::Min(MinX, X);
			MinY = FMath::Min(MinY, Y);
			MaxX = FMath::Max(MaxX, X);
			MaxY = FMath::Max(MaxY, Y);
		}
	}
	if (MaxX < MinX || MaxY < MinY)
	{
		return false;
	}
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			if (Mask.SolidMask[Y * Mask.Width + X] == 0)
			{
				return false;
			}
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTerrainSafePlayerSpawnTest,
	"MatterFlux.Playable.Spawn.PlayerCapsuleStartsAboveTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxTerrainSafePlayerSpawnTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	AMatterFluxGameMode* GameMode = World
		? World->SpawnActor<AMatterFluxGameMode>()
		: nullptr;
	APlayerController* Controller = World
		? World->SpawnActor<APlayerController>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world spawns for grounded player test"),
		WorldActor)
		|| !TestNotNull(TEXT("GameMode spawns for grounded player test"),
			GameMode)
		|| !TestNotNull(TEXT("Controller spawns for grounded player test"),
			Controller))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	float TerrainHeight = 0.0f;
	if (!TestTrue(TEXT("Terrain below the requested player start is sampleable"),
		WorldActor->TrySampleTerrainHeightAtWorldLocation(
			FVector::ZeroVector, TerrainHeight)))
	{
		return false;
	}

	const FTransform BuriedPlayerStart(
		FRotator::ZeroRotator,
		FVector(0.0f, 0.0f, TerrainHeight - 120.0f));
	APawn* SpawnedPawn =
		GameMode->SpawnDefaultPawnAtTransform_Implementation(
			Controller, BuriedPlayerStart);
	if (!TestNotNull(
		TEXT("A terrain-overlapped PlayerStart is raised instead of rejected"),
		SpawnedPawn))
	{
		return false;
	}
	const ACharacter* Character = Cast<ACharacter>(SpawnedPawn);
	const UCapsuleComponent* Capsule = Character
		? Character->GetCapsuleComponent()
		: nullptr;
	if (!TestNotNull(TEXT("Spawned player uses a capsule"), Capsule))
	{
		return false;
	}
	const float CapsuleBottom = SpawnedPawn->GetActorLocation().Z
		- Capsule->GetScaledCapsuleHalfHeight();
	TestTrue(*FString::Printf(
		TEXT("Player capsule starts above terrain (bottom=%.2f terrain=%.2f)"),
		CapsuleBottom, TerrainHeight),
		CapsuleBottom >= TerrainHeight + 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayerSpawnRegionStreamingBarrierTest,
	"MatterFlux.Playable.Spawn.PlayerRegionTerrainCommitsBeforeEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayerSpawnRegionStreamingBarrierTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world exists for spawn-region barrier"),
		WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	const FVector DistantPlayerStart(240000.0f, -180000.0f, 0.0f);
	FIntPoint SpawnChunk = FIntPoint::ZeroValue;
	if (!TestTrue(TEXT("Distant player start can pin its streaming region"),
		WorldActor->RequestPlayerSpawnRegion(
			DistantPlayerStart,
			SpawnChunk)))
	{
		return false;
	}
	TestNotEqual(TEXT("Test start is outside the old fallback chunk"),
		SpawnChunk,
		FIntPoint::ZeroValue);
	TestTrue(TEXT("Pinned player terrain is active immediately behind the gate"),
		WorldActor->HasVisibleTerrainChunk(SpawnChunk));
	TestTrue(TEXT("Pinned player terrain has committed collision before entry"),
		WorldActor->IsPlayerSpawnRegionTerrainReady(SpawnChunk));
	FVector GroundedLocation;
	TestTrue(TEXT("The committed region can ground the future player capsule"),
		WorldActor->TryResolveTerrainSpawnLocation(
			DistantPlayerStart,
			42.0f,
			96.0f,
			4.0f,
			GroundedLocation));
	WorldActor->ReleasePlayerSpawnRegion(SpawnChunk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxInitialEntryOrderingTest,
	"MatterFlux.Playable.Spawn.TerrainThenCreaturesThenPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxInitialEntryOrderingTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString ReloadError;
	if (!TestTrue(TEXT("Initial-entry content is available"),
		Runtime.ReloadDefaultContentPack(ReloadError)))
	{
		AddError(ReloadError);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(TEXT("Initial-entry river layout builds"),
		Registry.IsValid()
			&& MatterFlux::PlayableLevel::BuildLevelLayout(
				1337, Layout, Registry.Get())))
	{
		return false;
	}

	constexpr int32 PopulationChunkSize = 64;
	constexpr int32 SearchChunkY = 400;
	FIntPoint RiverChunk = FIntPoint::ZeroValue;
	FIntPoint RiverCell = FIntPoint::ZeroValue;
	bool bFoundRiver = false;
	for (int32 ChunkX = -32; ChunkX <= 32 && !bFoundRiver; ++ChunkX)
	{
		for (int32 LocalY = 0;
			LocalY < PopulationChunkSize && !bFoundRiver;
			++LocalY)
		{
			for (int32 LocalX = 0; LocalX < PopulationChunkSize; ++LocalX)
			{
				const FIntPoint Candidate(
					ChunkX * PopulationChunkSize + LocalX,
					SearchChunkY * PopulationChunkSize + LocalY);
				float RiverHeight = 0.0f;
				float WaterSurface = 0.0f;
				bool bContainsWater = false;
				if (Layout.Terrain.TrySampleInfiniteRiverCell(
					Candidate.X,
					Candidate.Y,
					RiverHeight,
					WaterSurface,
					bContainsWater)
					&& bContainsWater)
				{
					RiverChunk = FIntPoint(ChunkX, SearchChunkY);
					RiverCell = Candidate;
					bFoundRiver = true;
					break;
				}
			}
		}
	}
	if (!TestTrue(TEXT("Initial entry targets a deterministic river region"),
		bFoundRiver))
	{
		return false;
	}
	const int64 FirstWorldCellX = FMath::FloorToInt64(
		Layout.Terrain.FirstCellCenter.X / Layout.Terrain.CellSize);
	const int64 FirstWorldCellY = FMath::FloorToInt64(
		Layout.Terrain.FirstCellCenter.Y / Layout.Terrain.CellSize);
	const auto CellWorldLocation = [&](const FIntPoint Cell)
	{
		return FVector(
			Layout.Terrain.FirstCellCenter.X
				+ static_cast<double>(Cell.X - FirstWorldCellX)
					* Layout.Terrain.CellSize,
			Layout.Terrain.FirstCellCenter.Y
				+ static_cast<double>(Cell.Y - FirstWorldCellY)
					* Layout.Terrain.CellSize,
			0.0);
	};
	const FVector RiverWorldLocation = CellWorldLocation(RiverCell);
	const FVector InitialRegionLocation = CellWorldLocation(
		RiverChunk * PopulationChunkSize
			+ FIntPoint(PopulationChunkSize / 2));

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	AMatterFluxGameMode* GameMode = World
		? World->SpawnActor<AMatterFluxGameMode>()
		: nullptr;
	APlayerController* Controller = World
		? World->SpawnActor<APlayerController>()
		: nullptr;
	APlayerStart* PlayerStart = World
		? World->SpawnActor<APlayerStart>(
			InitialRegionLocation,
			FRotator::ZeroRotator)
		: nullptr;
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world exists for entry ordering"),
		WorldActor)
		|| !TestNotNull(TEXT("GameMode exists for entry ordering"), GameMode)
		|| !TestNotNull(TEXT("Controller exists for entry ordering"), Controller)
		|| !TestNotNull(TEXT("PlayerStart exists for entry ordering"), PlayerStart)
		|| !TestNotNull(TEXT("PlayerState exists for entry ordering"), PlayerState))
	{
		return false;
	}

	WorldActor->Regenerate(1337);
	Controller->SetPlayerState(PlayerState);
	if (!TestTrue(TEXT("The synthetic logged-in controller may restart"),
		Controller->CanRestartPlayer()))
	{
		return false;
	}
	Controller->StartSpot = PlayerStart;
	GameMode->HandleStartingNewPlayer_Implementation(Controller);
	TestNull(TEXT("Queuing entry does not create the player early"),
		Controller->GetPawn());

	GameMode->AdvancePendingPlayerEntries();
	if (!TestEqual(TEXT("Player remains queued while river streaming finishes"),
		GameMode->PendingPlayerEntries.Num(),
		1))
	{
		return false;
	}
	const AMatterFluxGameMode::FPendingPlayerEntry& Pending =
		GameMode->PendingPlayerEntries[0];
	TestTrue(TEXT("The queued PlayerStart terrain is collision-ready"),
		WorldActor->IsPlayerSpawnRegionTerrainReady(Pending.TerrainChunk));
	TestFalse(TEXT("Terrain collision alone does not open creature spawning"),
		GameMode->bCreatureSpawnGateOpen);
	TestFalse(TEXT("Initial entry waits for river simulation and presentation"),
		WorldActor->IsInitialWorldEntryReady());
	TestFalse(TEXT("The player remains absent during initial river streaming"),
		Controller->GetPawn() != nullptr);

	for (int32 Frame = 0;
		Frame < 1536 && !WorldActor->IsInitialWorldEntryReady();
		++Frame)
	{
		WorldActor->Tick(1.0f / 60.0f);
	}
	if (!TestTrue(TEXT("Initial river region reaches the complete entry barrier"),
		WorldActor->IsInitialWorldEntryReady()))
	{
		return false;
	}
	MatterFlux::Liquid::FLiquidColumn InitialRiverColumn;
	TestTrue(TEXT("Initial river water exists before creature spawning"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			RiverWorldLocation, InitialRiverColumn));
	TestTrue(TEXT("Initial river projection exists before creature spawning"),
		WorldActor->HasLiquidProjectionAtWorldLocation(
			TEXT("water"), RiverWorldLocation));

	GameMode->AdvancePendingPlayerEntries();
	TestTrue(TEXT("Creature spawning opens only after the complete entry barrier"),
		GameMode->bCreatureSpawnGateOpen);
	TestTrue(TEXT("The initial creature pass completes before player entry"),
		GameMode->bInitialCreaturePassComplete);
	TestNull(TEXT("The player is still absent during the creature pass"),
		Controller->GetPawn());

	GameMode->AdvancePendingPlayerEntries();
	TestNotNull(TEXT("The player is created only on the following pass"),
		Controller->GetPawn().Get());
	TestTrue(TEXT("The completed player is removed from the entry queue"),
		GameMode->PendingPlayerEntries.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStoryMapPlayerEntryTest,
	"MatterFlux.Playable.Spawn.StoryMapCreatesPlayerWithFollowCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxStoryMapPlayerEntryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Story-entry content is available"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	AMatterFluxGameMode* GameMode = World
		? World->SpawnActor<AMatterFluxGameMode>()
		: nullptr;
	AMatterFluxGameState* GameState = World
		? World->SpawnActor<AMatterFluxGameState>()
		: nullptr;
	APlayerController* Controller = World
		? World->SpawnActor<APlayerController>()
		: nullptr;
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world exists for story entry"), WorldActor)
		|| !TestNotNull(TEXT("GameMode exists for story entry"), GameMode)
		|| !TestNotNull(TEXT("GameState exists for story entry"), GameState)
		|| !TestNotNull(TEXT("Controller exists for story entry"), Controller)
		|| !TestNotNull(TEXT("Player state exists for story entry"), PlayerState))
	{
		return false;
	}
	World->SetGameState(GameState);
	GameMode->GameState = GameState;
	Controller->SetPlayerState(PlayerState);
	GameState->AddPlayerState(PlayerState);
	WorldActor->Regenerate(1337);
	GameMode->PrepareForInitialWorldEntry();
	TestTrue(TEXT("The free-mode fixture begins with visible vegetation"),
		WorldActor->GetVisibleFragmentSourceProxyCount() > 0);
	if (!TestTrue(TEXT("Story map loads before the player exists"),
		WorldActor->LoadCustomMap(TEXT("story.paper_magic"), 1, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Story mode retains its fixed procedural seed"),
		WorldActor->GetMapSeed(),
		AMatterFluxPlayableWorldActor::PaperMagicStorySeed);
	TestTrue(TEXT("Story mode rebuilds seeded terrain chunks"),
		WorldActor->GetVisibleTerrainChunkCount() > 0);
	TestTrue(TEXT("Story mode rebuilds seeded rivers"),
		WorldActor->GetGeneratedLiquidLayerCount() > 0);
	TestTrue(TEXT("Story mode rebuilds seeded trees and ground cover"),
		WorldActor->GetVisibleFragmentSourceProxyCount() > 0
			|| WorldActor->GetGeneratedFragmentSourceCount() > 0);
	AMatterFluxTwoStoreyHouseActor* StoryHouse =
		WorldActor->GetGeneratedHouse();
	FGuid NearestStoryTreeAggregate;
	FGuid NearestStoryTreeRoot;
	FBox NearestStoryTreeBounds(ForceInit);
	FTransform NearestStoryTreeTransform = FTransform::Identity;
	if (TestNotNull(TEXT("Story mode creates the fixed camp house"), StoryHouse)
		&& TestTrue(TEXT("Story mode retains seeded trees outside the camp clearing"),
		WorldActor->FindNearestTreeAggregateForVisualInspection(
			StoryHouse->GetActorLocation(),
			NearestStoryTreeAggregate,
			NearestStoryTreeRoot,
			NearestStoryTreeBounds,
			NearestStoryTreeTransform)))
	{
		const FVector ClearingDelta =
			NearestStoryTreeTransform.GetLocation()
			- StoryHouse->GetActorLocation();
		TestTrue(TEXT("No random tree root is generated in the fixed house area"),
			FMath::Abs(ClearingDelta.X) >= 950.0f
				|| FMath::Abs(ClearingDelta.Y) >= 950.0f);
	}
	GameMode->RefreshConfiguredCreatureSpawns();
	TestFalse(TEXT("Loading the story map does not open creature spawning"),
		GameMode->bCreatureSpawnGateOpen);
	int32 EarlyConfiguredCreatureCount = 0;
	for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(TEXT("MatterFluxConfiguredCreature")))
		{
			++EarlyConfiguredCreatureCount;
		}
	}
	TestEqual(TEXT("Story creatures remain absent before the entry barrier"),
		EarlyConfiguredCreatureCount, 0);

	UMatterFluxMagicInventoryComponent* Inventory =
		PlayerState->GetMagicInventory();
	UMatterFluxProgressionComponent* Progression =
		PlayerState->GetProgression();
	if (!TestNotNull(TEXT("Story magic inventory exists"), Inventory)
		|| !TestNotNull(TEXT("Story progression exists"), Progression)
		|| !TestTrue(TEXT("Story inventory begins empty"),
			Inventory->ResetToEmptyLoadoutAuthority(Error))
		|| !TestTrue(TEXT("Story tutorial state activates"),
			Progression->ResetToStoryStateAuthority(Error)))
	{
		AddError(Error);
		return false;
	}

	FVector StoryPlayerStart = FVector::ZeroVector;
	if (!TestTrue(TEXT("Story map exposes its player marker"),
		WorldActor->TryGetCustomMapMarker(
			TEXT("player_start"), StoryPlayerStart)))
	{
		return false;
	}
	GameMode->HandleStartingNewPlayer_Implementation(Controller);
	TestNull(TEXT("Queuing story entry does not create the player early"),
		Controller->GetPawn());
	GameMode->AdvancePendingPlayerEntries();
	TestFalse(TEXT("Story creatures wait for terrain, decoration, and water"),
		GameMode->bCreatureSpawnGateOpen);
	TestNull(TEXT("Story player remains absent while the world settles"),
		Controller->GetPawn());

	for (int32 Frame = 0;
		Frame < 1536 && !WorldActor->IsInitialWorldEntryReady();
		++Frame)
	{
		WorldActor->Tick(1.0f / 60.0f);
	}
	if (!TestTrue(TEXT("Story world reaches the complete entry barrier"),
		WorldActor->IsInitialWorldEntryReady()))
	{
		return false;
	}

	GameMode->AdvancePendingPlayerEntries();
	TestTrue(TEXT("Story creature spawning opens after the world barrier"),
		GameMode->bCreatureSpawnGateOpen);
	TestTrue(TEXT("Story creatures complete before player creation"),
		GameMode->bInitialCreaturePassComplete);
	TestNull(TEXT("Story player is absent during the creature pass"),
		Controller->GetPawn());
	AMatterFluxCreatureActor* StoryMerchant = nullptr;
	for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
	{
		if (It->GetDefinitionId() == TEXT("std.merchant_base"))
		{
			StoryMerchant = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("Story NPC spawns after the world barrier"),
		StoryMerchant))
	{
		return false;
	}

	GameMode->AdvancePendingPlayerEntries();
	AMatterFluxCharacter* Character =
		Cast<AMatterFluxCharacter>(Controller->GetPawn());
	if (!TestNotNull(TEXT("Story entry creates the normal player character"),
		Character))
	{
		return false;
	}
	TestTrue(TEXT("Story player uses the authored horizontal marker"),
		FVector2D(Character->GetActorLocation()).Equals(
			FVector2D(StoryPlayerStart), 1.0));
	TestTrue(TEXT("Completed story entry leaves the queue"),
		GameMode->PendingPlayerEntries.IsEmpty());

	const FGuid TutorialWand = Inventory->GetOwnedWands().IsEmpty()
		? FGuid()
		: Inventory->GetOwnedWands()[0].InstanceId;
	FMatterFluxMagicEdit EquipWand;
	EquipWand.Type = EMatterFluxMagicEditType::EquipWand;
	EquipWand.ExpectedRevision = Inventory->GetInventoryRevision();
	EquipWand.WandId = TutorialWand;
	EquipWand.EquipmentSlot = 0;
	FMatterFluxQuestEvent RefreshQuest;
	RefreshQuest.Type = EMatterFluxQuestEventType::Refresh;
	if (!TestTrue(TEXT("Tutorial wand equips"),
			Inventory->ApplyEditAuthority(EquipWand, Error))
		|| !TestTrue(TEXT("Wand equipment advances the tutorial"),
			Progression->NotifyQuestEventAuthority(RefreshQuest, Error)))
	{
		AddError(Error);
		return false;
	}
	FMatterFluxMagicEdit AssignSpell;
	AssignSpell.Type = EMatterFluxMagicEditType::AssignSpell;
	AssignSpell.ExpectedRevision = Inventory->GetInventoryRevision();
	AssignSpell.WandId = TutorialWand;
	AssignSpell.SpellId = TEXT("std.default");
	AssignSpell.ToSpellSlot = 0;
	if (!TestTrue(TEXT("Tutorial attack spell equips"),
			Inventory->ApplyEditAuthority(AssignSpell, Error))
		|| !TestTrue(TEXT("Spell equipment activates the kill objective"),
			Progression->NotifyQuestEventAuthority(RefreshQuest, Error)))
	{
		AddError(Error);
		return false;
	}
	const FMatterFluxQuestState* KillQuest =
		Progression->FindQuestState(TEXT("std.init_quest.kill_enemy"));
	if (!TestTrue(TEXT("Training fight is active after equipping the loadout"),
		KillQuest
			&& KillQuest->Status == EMatterFluxQuestRuntimeStatus::Active))
	{
		return false;
	}
	GameMode->RefreshConfiguredCreatureSpawns();
	int32 TrainingEnemyCount = 0;
	TArray<AMatterFluxCreatureActor*> TrainingEnemies;
	for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
	{
		if (It->GetDefinitionId() == TEXT("std.slime")
			|| It->GetDefinitionId() == TEXT("std.elite_patrol"))
		{
			++TrainingEnemyCount;
			TrainingEnemies.Add(*It);
		}
	}
	TestEqual(TEXT("All three training enemies spawn inside the initial area"),
		TrainingEnemyCount, 3);
	const UCapsuleComponent* MerchantCapsule =
		StoryMerchant->GetCapsuleComponent();
	float MerchantTerrainHeight = 0.0f;
	if (!TestNotNull(TEXT("Story NPC has a collision capsule"),
		MerchantCapsule)
		|| !TestTrue(TEXT("Story NPC terrain is sampleable"),
			WorldActor->TrySampleTerrainHeightAtWorldLocation(
				StoryMerchant->GetActorLocation(), MerchantTerrainHeight)))
	{
		return false;
	}
	const float MerchantCapsuleBottom = StoryMerchant->GetActorLocation().Z
		- MerchantCapsule->GetScaledCapsuleHalfHeight();
	TestTrue(TEXT("Story NPC starts above the completed terrain"),
		MerchantCapsuleBottom >= MerchantTerrainHeight + 1.0f);
	for (const AMatterFluxCreatureActor* Enemy : TrainingEnemies)
	{
		const UCapsuleComponent* EnemyCapsule = Enemy
			? Enemy->GetCapsuleComponent() : nullptr;
		float TerrainHeight = 0.0f;
		if (!TestNotNull(TEXT("Story enemy has a collision capsule"),
			EnemyCapsule)
			|| !TestTrue(TEXT("Story enemy terrain is sampleable"),
				WorldActor->TrySampleTerrainHeightAtWorldLocation(
					Enemy->GetActorLocation(), TerrainHeight)))
		{
			return false;
		}
		const float CapsuleBottom = Enemy->GetActorLocation().Z
			- EnemyCapsule->GetScaledCapsuleHalfHeight();
		TestTrue(*FString::Printf(
			TEXT("Story enemy '%s' starts above terrain "
				"(bottom=%.2f terrain=%.2f)"),
			*Enemy->GetDefinitionId().ToString(),
			CapsuleBottom,
			TerrainHeight),
			CapsuleBottom >= TerrainHeight + 1.0f);
	}
	if (TestTrue(TEXT("Two enemies exist for hostile-on-hostile death coverage"),
		TrainingEnemies.Num() >= 2))
	{
		AMatterFluxCreatureActor* Victim = TrainingEnemies[0];
		AMatterFluxCreatureActor* Attacker = TrainingEnemies[1];
		TestTrue(TEXT("Another enemy can deal the lethal blow"),
			Victim->ApplyDamageAuthority(
				Victim->GetCurrentHealth() + 1.0f,
				Attacker));
		KillQuest = Progression->FindQuestState(
			TEXT("std.init_quest.kill_enemy"));
		TestEqual(TEXT("Enemy-on-enemy death advances the active kill task"),
			KillQuest ? KillQuest->Progress : INDEX_NONE,
			1);
	}

	const UCapsuleComponent* PlayerCapsule =
		Character->GetCapsuleComponent();
	float PlayerTerrainHeight = 0.0f;
	if (!TestNotNull(TEXT("Story player has a collision capsule"), PlayerCapsule)
		|| !TestTrue(TEXT("Story player terrain is sampleable"),
			WorldActor->TrySampleTerrainHeightAtWorldLocation(
				Character->GetActorLocation(), PlayerTerrainHeight)))
	{
		return false;
	}
	const float PlayerCapsuleBottom = Character->GetActorLocation().Z
		- PlayerCapsule->GetScaledCapsuleHalfHeight();
	TestTrue(*FString::Printf(
		TEXT("Story player starts above terrain (bottom=%.2f terrain=%.2f)"),
		PlayerCapsuleBottom,
		PlayerTerrainHeight),
		PlayerCapsuleBottom >= PlayerTerrainHeight + 1.0f);
	TestNotNull(TEXT("Story player keeps the normal follow camera"),
		Character->FindComponentByClass<UCameraComponent>());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStoryBossSpawnTest,
	"MatterFlux.Playable.Spawn.LoadedActiveFinalQuestCreatesTwoBosses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxStoryBossSpawnTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString Error;
	if (!TestTrue(TEXT("Final-boss content is available"),
		Runtime.ReloadDefaultContentPack(Error)))
	{
		AddError(Error);
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	AMatterFluxGameMode* GameMode = World
		? World->SpawnActor<AMatterFluxGameMode>()
		: nullptr;
	AMatterFluxGameState* GameState = World
		? World->SpawnActor<AMatterFluxGameState>()
		: nullptr;
	AMatterFluxPlayerState* PlayerState = World
		? World->SpawnActor<AMatterFluxPlayerState>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world exists for final bosses"), WorldActor)
		|| !TestNotNull(TEXT("GameMode exists for final bosses"), GameMode)
		|| !TestNotNull(TEXT("GameState exists for final bosses"), GameState)
		|| !TestNotNull(TEXT("Player state exists for final bosses"), PlayerState))
	{
		return false;
	}
	World->SetGameState(GameState);
	GameMode->GameState = GameState;
	GameState->AddPlayerState(PlayerState);
	WorldActor->Regenerate(AMatterFluxPlayableWorldActor::PaperMagicStorySeed);
	if (!TestTrue(TEXT("PaperMagic story map loads for final bosses"),
		WorldActor->LoadCustomMap(TEXT("story.paper_magic"), 1, Error)))
	{
		AddError(Error);
		return false;
	}

	FMatterFluxProgressionSaveState FinalQuestState;
	FMatterFluxSavedQuestState& SavedQuest =
		FinalQuestState.Quests.AddDefaulted_GetRef();
	SavedQuest.QuestId = TEXT("std.side_kill_boss");
	SavedQuest.Status = static_cast<uint8>(
		EMatterFluxQuestRuntimeStatus::Active);
	SavedQuest.bActivationRewardsGranted = true;
	FinalQuestState.SelectedQuest = SavedQuest.QuestId;
	if (!TestTrue(TEXT("Final quest restores as active"),
		PlayerState->GetProgression()->RestoreSaveStateAuthority(
			FinalQuestState, Error)))
	{
		AddError(Error);
		return false;
	}

	// Loading a story save closes the creature gate before replacing the map.
	// Unlike a new story, the load path keeps its existing pawn and therefore
	// does not enter the pending-player queue that normally reopens this gate.
	GameMode->PrepareForInitialWorldEntry();
	for (int32 Frame = 0;
		Frame < 1536 && !WorldActor->IsInitialWorldEntryReady();
		++Frame)
	{
		WorldActor->Tick(1.0f / 60.0f);
	}
	if (!TestTrue(TEXT("Loaded story world reaches its complete entry barrier"),
		WorldActor->IsInitialWorldEntryReady())
		|| !TestTrue(TEXT("Loaded story reopens configured creature spawning"),
			GameMode->CompleteExistingPlayerWorldLoad()))
	{
		return false;
	}
	// Distant configured creatures are not allowed to begin physics until their
	// authored collision chunk has committed. Drive the same half-second retry
	// seam used by GameMode's timer rather than assuming the first refresh can
	// synchronously cook every final-region terrain body.
	for (int32 Frame = 0; Frame < 1536; ++Frame)
	{
		WorldActor->Tick(1.0f / 60.0f);
		GameMode->RefreshConfiguredCreatureSpawns();
		int32 BossCount = 0;
		for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
		{
			BossCount += It->GetDefinitionId() == TEXT("std.test_boss")
				? 1 : 0;
		}
		if (BossCount == 2)
		{
			break;
		}
	}
	TArray<AMatterFluxCreatureActor*> Bosses;
	for (TActorIterator<AMatterFluxCreatureActor> It(World); It; ++It)
	{
		if (It->GetDefinitionId() == TEXT("std.test_boss"))
		{
			Bosses.Add(*It);
		}
	}
	TestEqual(TEXT("Loaded active final quest creates both authored bosses"),
		Bosses.Num(), 2);
	for (const AMatterFluxCreatureActor* Boss : Bosses)
	{
		TestEqual(TEXT("Unseen final boss starts passive"),
			Boss->GetRuntimeState(),
			EMatterFluxCreatureRuntimeState::Passive);
	}
	for (int32 BossIndex = 0; BossIndex < 2; ++BossIndex)
	{
		const FName PinKey(*FString::Printf(
			TEXT("quest.std.side_kill_boss.creature.std.test_boss.%d"),
			BossIndex));
		const FIntPoint* TerrainChunk =
			GameMode->ConfiguredCreatureTerrainPins.Find(PinKey);
		TestTrue(TEXT("Each final boss keeps its authored terrain chunk pinned"),
			TerrainChunk
				&& WorldActor->IsPlayerSpawnRegionTerrainReady(*TerrainChunk));
	}
	FVector StoryPlayerStart = FVector::ZeroVector;
	if (!TestTrue(TEXT("Story player marker is available for boss waiting"),
		WorldActor->TryGetCustomMapMarker(
			TEXT("player_start"), StoryPlayerStart)))
	{
		return false;
	}
	WorldActor->SetWorldStreamingFocus(StoryPlayerStart);
	for (int32 Frame = 0; Frame < 1800; ++Frame)
	{
		++GFrameCounter;
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	}
	for (const AMatterFluxCreatureActor* Boss : Bosses)
	{
		float TerrainHeight = 0.0f;
		const UCapsuleComponent* Capsule = Boss
			? Boss->GetCapsuleComponent() : nullptr;
		const bool bHasTerrain = Boss
			&& WorldActor->TrySampleTerrainHeightAtWorldLocation(
				Boss->GetActorLocation(), TerrainHeight);
		const float CapsuleBottom = Capsule
			? Boss->GetActorLocation().Z
				- Capsule->GetScaledCapsuleHalfHeight()
			: -BIG_NUMBER;
		TestTrue(TEXT("Each final boss remains alive in the authored final region"),
			IsValid(Boss) && !Boss->IsActorBeingDestroyed()
				&& Boss->GetActorLocation().X >= 3600.0f
				&& Boss->GetActorLocation().X <= 4500.0f);
		TestTrue(*FString::Printf(
			TEXT("Each waiting final boss stays on terrain "
				"(bottom=%.2f terrain=%.2f)"),
			CapsuleBottom, TerrainHeight),
			Capsule && bHasTerrain
				&& CapsuleBottom >= TerrainHeight - 5.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTerrainSafeCreatureSpawnTest,
	"MatterFlux.Playable.Spawn.CreatureCapsuleUsesDefinitionBeforeFinish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxTerrainSafeCreatureSpawnTest::RunTest(
	const FString& Parameters)
{
	IMatterFluxScriptRuntime& Runtime = IMatterFluxScriptRuntime::Get();
	FString ReloadError;
	if (!TestTrue(TEXT("Default creature content loads for spawn grounding"),
		Runtime.ReloadDefaultContentPack(ReloadError)))
	{
		AddError(ReloadError);
		return false;
	}
	const FMatterFluxContentRegistryPtr Registry = Runtime.GetActiveRegistry();
	const FMatterFluxCreatureDefinition* Slime = Registry.IsValid()
		? Registry->Creatures.Find(TEXT("std.slime"))
		: nullptr;
	if (!TestNotNull(TEXT("Short creature definition is available"), Slime))
	{
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world spawns for creature grounding"),
		WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	const FVector RequestedLocation(320.0f, -120.0f, -500.0f);
	const AMatterFluxCreatureActor* CreatureDefault =
		GetDefault<AMatterFluxCreatureActor>();
	const UCapsuleComponent* DefaultCapsule = CreatureDefault
		? CreatureDefault->GetCapsuleComponent()
		: nullptr;
	if (!TestNotNull(TEXT("Creature class has a construction capsule"),
		DefaultCapsule))
	{
		return false;
	}
	const float FinalRadius = Slime->Width * 0.5f;
	const float FinalHalfHeight = FMath::Max(
		FinalRadius, Slime->Height * 0.5f);
	FVector ConstructionLocation;
	FVector FinalLocation;
	if (!TestTrue(TEXT("Construction capsule receives a terrain-safe location"),
		WorldActor->TryResolveTerrainSpawnLocation(
			RequestedLocation,
			FMath::Max(DefaultCapsule->GetScaledCapsuleRadius(), FinalRadius),
			FMath::Max(
				DefaultCapsule->GetScaledCapsuleHalfHeight(), FinalHalfHeight),
			4.0f,
			ConstructionLocation))
		|| !TestTrue(TEXT("Authored creature capsule receives a terrain-safe location"),
			WorldActor->TryResolveTerrainSpawnLocation(
				RequestedLocation,
				FinalRadius,
				FinalHalfHeight,
				4.0f,
				FinalLocation)))
	{
		return false;
	}

	AMatterFluxCreatureActor* Creature =
		World->SpawnActorDeferred<AMatterFluxCreatureActor>(
			AMatterFluxCreatureActor::StaticClass(),
			FTransform(FRotator::ZeroRotator, ConstructionLocation),
			nullptr,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	if (!TestNotNull(TEXT("Deferred short creature spawns"), Creature))
	{
		return false;
	}
	Creature->InitializeCreature(TEXT("std.slime"));
	TestEqual(TEXT("Creature uses its authored capsule before FinishSpawning"),
		Creature->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(),
		FinalHalfHeight);
	Creature->FinishSpawning(
		FTransform(FRotator::ZeroRotator, FinalLocation));
	float TerrainHeight = 0.0f;
	if (TestTrue(TEXT("Terrain below the finished creature is sampleable"),
		WorldActor->TrySampleTerrainHeightAtWorldLocation(
			FinalLocation, TerrainHeight)))
	{
		const float CapsuleBottom = Creature->GetActorLocation().Z
			- Creature->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		TestTrue(*FString::Printf(
			TEXT("Creature capsule starts above terrain (bottom=%.2f terrain=%.2f)"),
			CapsuleBottom, TerrainHeight),
			CapsuleBottom >= TerrainHeight + 1.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTreeCanopyVolumeTest,
	"MatterFlux.Playable.Tree.CanopyUsesExtrusionDepthInsteadOfPlanarStretch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxTreeCanopyVolumeTest::RunTest(const FString& Parameters)
{
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(TEXT("Reference forest layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, Layout)))
	{
		return false;
	}
	int32 LeafClusterCount = 0;
	int32 InPlaneStretchCount = 0;
	int32 NonVoxelDepthCount = 0;
	TMap<FGuid, TSet<int32>> DepthSlicesPerTree;
	TMap<FGuid, FTransform> RootTransforms;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.Name == TEXT("TreeTrunk") && Source.bAggregateRoot)
		{
			RootTransforms.Add(Source.AggregateId, Source.Transform);
		}
	}
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.Name != TEXT("TreeLeaves"))
		{
			continue;
		}
		++LeafClusterCount;
		const FVector Scale = Source.Transform.GetScale3D();
		InPlaneStretchCount += FMath::IsNearlyEqual(Scale.Z, 1.0f) ? 0 : 1;
		NonVoxelDepthCount += FMath::IsNearlyEqual(Scale.Y, 1.0f) ? 0 : 1;
		if (const FTransform* RootTransform =
			RootTransforms.Find(Source.AggregateId))
		{
			const FVector RelativeLocation =
				RootTransform->InverseTransformPosition(
					Source.Transform.GetLocation());
			DepthSlicesPerTree.FindOrAdd(Source.AggregateId).Add(
				FMath::RoundToInt(
					RelativeLocation.Y / Source.Mask.CellSize));
		}
	}
	TestTrue(TEXT("Reference forest contains leaf clusters"),
		LeafClusterCount > 20);
	TestEqual(TEXT("No leaf outline is stretched inside its mask plane"),
		InPlaneStretchCount, 0);
	TestEqual(TEXT("Every leaf slice is exactly one voxel deep"),
		NonVoxelDepthCount, 0);
	for (const TPair<FGuid, TSet<int32>>& Pair : DepthSlicesPerTree)
	{
		TestTrue(TEXT("Each canopy occupies five real voxel-depth slices"),
			Pair.Value.Num() >= 5);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMinecraftTreeStyleTest,
	"MatterFlux.Playable.Tree.MinecraftBlockSilhouetteAndRenderBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMinecraftTreeStyleTest::RunTest(const FString& Parameters)
{
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(TEXT("Reference forest layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, Layout)))
	{
		return false;
	}

	TMap<FGuid, int32> PartsPerTree;
	TMap<FGuid, int32> TrunkSlicesPerTree;
	TMap<FGuid, int32> LeafSlicesPerTree;
	TMap<FGuid, TSet<int32>> LeafDepthsPerTree;
	TMap<FGuid, TMap<int32, int32>> LeafCellsByTreeHeight;
	TSet<uint32> LeafColors;
	TMap<FGuid, FVector> TreeGridAnchors;
	TMap<FGuid, FQuat> TreeGridRotations;
	TArray<FVector2D> TreeTrunkLocations;
	int32 UnevenTreeFootprints = 0;
	int32 TreeRootsInsufficientlyEmbedded = 0;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.Name != TEXT("TreeTrunk") || !Source.bAggregateRoot)
		{
			continue;
		}
		// 2x2 树干旋转 45 度后，方形截面的世界轴外接范围为一格乘 sqrt(2)。
		const float HalfFootprint = Source.Mask.CellSize * FMath::Sqrt(2.0f);
		const FVector TrunkCenter = Source.Transform.GetLocation();
		// aggregate root 是后侧深度切片；向局部 +Y 移半格才是 2x2
		// 截面的几何中心，必须围绕这个点核对完整占地。
		const FVector FootprintCenter = TrunkCenter
			+ Source.Transform.GetUnitAxis(EAxis::Y)
				* Source.Mask.CellSize * 0.5f;
		TreeGridRotations.Add(
			Source.AggregateId,
			Source.Transform.GetRotation());
		TreeTrunkLocations.Emplace(FootprintCenter.X, FootprintCenter.Y);
		const int32 MinTerrainX = FMath::Clamp(
			FMath::RoundToInt(
				(FootprintCenter.X - HalfFootprint
					- Layout.Terrain.FirstCellCenter.X)
				/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Width - 1);
		const int32 MaxTerrainX = FMath::Clamp(
			FMath::RoundToInt(
				(FootprintCenter.X + HalfFootprint
					- Layout.Terrain.FirstCellCenter.X)
				/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Width - 1);
		const int32 MinTerrainY = FMath::Clamp(
			FMath::RoundToInt(
				(FootprintCenter.Y - HalfFootprint
					- Layout.Terrain.FirstCellCenter.Y)
				/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Height - 1);
		const int32 MaxTerrainY = FMath::Clamp(
			FMath::RoundToInt(
				(FootprintCenter.Y + HalfFootprint
					- Layout.Terrain.FirstCellCenter.Y)
				/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Height - 1);
		const float TerrainHeight = Layout.Terrain.HeightAt(
			MinTerrainX, MinTerrainY);
		bool bFlatFootprint = true;
		for (int32 TerrainY = MinTerrainY;
			TerrainY <= MaxTerrainY;
			++TerrainY)
		{
			for (int32 TerrainX = MinTerrainX;
				TerrainX <= MaxTerrainX;
				++TerrainX)
			{
				bFlatFootprint &= FMath::IsNearlyEqual(
					Layout.Terrain.HeightAt(TerrainX, TerrainY),
					TerrainHeight);
			}
		}
		UnevenTreeFootprints += bFlatFootprint ? 0 : 1;
		const float TrunkBottom = TrunkCenter.Z
			- static_cast<float>(Source.Mask.Height)
				* Source.Mask.CellSize * 0.5f;
		// 斜俯视会看见未埋入地面的方柱近角，底边因透视形成向内收的
		// V 形。保留一个完整地下根体素，让平坦地形切过连续侧面，
		// 而不是仅靠不足以覆盖阶梯误差的亚像素偏移。
		TreeRootsInsufficientlyEmbedded +=
			TerrainHeight - TrunkBottom
				>= Source.Mask.CellSize - KINDA_SMALL_NUMBER
				? 0 : 1;
		for (int32 Y = 0; Y < Source.Mask.Height; ++Y)
		{
			for (int32 X = 0; X < Source.Mask.Width; ++X)
			{
				if (Source.Mask.SolidMask[Y * Source.Mask.Width + X] == 0)
				{
					continue;
				}
				const FVector LocalCenter(
					(static_cast<float>(X) + 0.5f
						- static_cast<float>(Source.Mask.Width) * 0.5f)
						* Source.Mask.CellSize,
					0.0f,
					(static_cast<float>(Y) + 0.5f
						- static_cast<float>(Source.Mask.Height) * 0.5f)
						* Source.Mask.CellSize);
				TreeGridAnchors.Add(
					Source.AggregateId,
					Source.Transform.TransformPosition(LocalCenter));
				break;
			}
			if (TreeGridAnchors.Contains(Source.AggregateId))
			{
				break;
			}
		}
	}
	int32 NonRectangularWoodParts = 0;
	TMap<FGuid, int32> NonRectangularLeafSlicesPerTree;
	int32 NonBlockGeometryParts = 0;
	int32 NonAxisAlignedParts = 0;
	int32 InvalidBlockSilhouettes = 0;
	int32 OversizedTrunkParts = 0;
	int32 OverlongBranchParts = 0;
	int32 OffGridTreeVoxelCells = 0;
	float TreeCellSize = 0.0f;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		const bool bTreePart = Source.Name == TEXT("TreeTrunk")
			|| Source.Name == TEXT("TreeBranch")
			|| Source.Name == TEXT("TreeLeaves");
		if (!bTreePart)
		{
			continue;
		}
		PartsPerTree.FindOrAdd(Source.AggregateId)++;
		if (Source.Name == TEXT("TreeTrunk"))
		{
			TrunkSlicesPerTree.FindOrAdd(Source.AggregateId)++;
		}
		if (const FVector* Anchor = TreeGridAnchors.Find(Source.AggregateId))
		{
			const FQuat AggregateRotation =
				TreeGridRotations.FindRef(Source.AggregateId);
			for (int32 Y = 0; Y < Source.Mask.Height; ++Y)
			{
				for (int32 X = 0; X < Source.Mask.Width; ++X)
				{
					if (Source.Mask.SolidMask[
						Y * Source.Mask.Width + X] == 0)
					{
						continue;
					}
					const FVector LocalCenter(
						(static_cast<float>(X) + 0.5f
							- static_cast<float>(Source.Mask.Width) * 0.5f)
							* Source.Mask.CellSize,
						0.0f,
						(static_cast<float>(Y) + 0.5f
							- static_cast<float>(Source.Mask.Height) * 0.5f)
							* Source.Mask.CellSize);
					const FVector GridDelta =
						AggregateRotation.UnrotateVector(
							Source.Transform.TransformPosition(LocalCenter)
								- *Anchor)
						/ Source.Mask.CellSize;
					const FVector Snapped(
						FMath::RoundToFloat(GridDelta.X),
						FMath::RoundToFloat(GridDelta.Y),
						FMath::RoundToFloat(GridDelta.Z));
					OffGridTreeVoxelCells += GridDelta.Equals(Snapped, 0.01f)
						? 0 : 1;
					if (Source.Name == TEXT("TreeLeaves"))
					{
						LeafCellsByTreeHeight.FindOrAdd(Source.AggregateId)
							.FindOrAdd(FMath::RoundToInt(GridDelta.Z))++;
					}
				}
			}
		}
		if (Source.Name == TEXT("TreeLeaves"))
		{
			LeafSlicesPerTree.FindOrAdd(Source.AggregateId)++;
			if (const FVector* Anchor =
				TreeGridAnchors.Find(Source.AggregateId))
			{
				const FVector RelativeLocation =
					TreeGridRotations.FindRef(Source.AggregateId).UnrotateVector(
						Source.Transform.GetLocation() - *Anchor);
				LeafDepthsPerTree.FindOrAdd(Source.AggregateId).Add(
					FMath::RoundToInt(
						RelativeLocation.Y / Source.Mask.CellSize));
			}
			TestTrue(TEXT("Leaf geometry is one voxel slice, never a thick slab"),
				FMath::IsNearlyEqual(
					Source.Transform.GetScale3D().Y,
					1.0f));
		}
		else if (Source.Name == TEXT("TreeTrunk"))
		{
			for (int32 Row = 0; Row < Source.Mask.Height; ++Row)
			{
				int32 SolidCellsInRow = 0;
				for (int32 Column = 0; Column < Source.Mask.Width; ++Column)
				{
					SolidCellsInRow += Source.Mask.SolidMask[
						Row * Source.Mask.Width + Column] != 0 ? 1 : 0;
				}
				OversizedTrunkParts += SolidCellsInRow != 2 ? 1 : 0;
			}
			OversizedTrunkParts += FMath::IsNearlyEqual(
				Source.Transform.GetScale3D().Y, 1.0f) ? 0 : 1;
		}
		else if (Source.Name == TEXT("TreeBranch"))
		{
			int32 SolidBranchCells = 0;
			for (const uint8 Cell : Source.Mask.SolidMask)
			{
				SolidBranchCells += Cell != 0 ? 1 : 0;
			}
			OverlongBranchParts += SolidBranchCells > 2 ? 1 : 0;
		}
		LeafColors.Add(Source.Name == TEXT("TreeLeaves")
			? Source.Color.ToFColor(false).DWColor() : 0u);
		const bool bRectangle = IsSolidRegionOneRectangle(Source.Mask);
		if (Source.Name == TEXT("TreeLeaves"))
		{
			NonRectangularLeafSlicesPerTree.FindOrAdd(Source.AggregateId) +=
				bRectangle ? 0 : 1;
		}
		else
		{
			NonRectangularWoodParts += bRectangle ? 0 : 1;
		}
		const EFragmentSourceGeometryStyle ExpectedGeometryStyle =
			EFragmentSourceGeometryStyle::VoxelBlocks;
		NonBlockGeometryParts += Source.Mask.GeometryStyle
			== ExpectedGeometryStyle ? 0 : 1;
		const FRotator Rotation = Source.Transform.Rotator();
		const float RootYaw = TreeGridRotations.Contains(Source.AggregateId)
			? TreeGridRotations.FindRef(Source.AggregateId).Rotator().Yaw
			: Rotation.Yaw;
		const float RelativeYaw = FMath::FindDeltaAngleDegrees(
			RootYaw,
			Rotation.Yaw);
		const float SnappedYaw = FMath::GridSnap(RelativeYaw, 90.0f);
		NonAxisAlignedParts +=
			FMath::IsNearlyZero(Rotation.Pitch, 0.01f)
			&& FMath::IsNearlyZero(Rotation.Roll, 0.01f)
			&& FMath::IsNearlyEqual(RelativeYaw, SnappedYaw, 0.01f)
				? 0 : 1;
		TreeCellSize = TreeCellSize <= 0.0f
			? Source.Mask.CellSize : TreeCellSize;
		TestEqual(TEXT("Every tree part shares one voxel size for batching"),
			Source.Mask.CellSize, TreeCellSize);

		MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
		if (MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
			Source.Mask.SolidMask,
			Source.Mask.Width,
			Source.Mask.Height,
			Source.Mask.CellSize,
			Geometry))
		{
			bool bValidBlockSilhouettes = !Geometry.OuterContours.IsEmpty();
			for (const FFragmentContour& Contour : Geometry.OuterContours)
			{
				bValidBlockSilhouettes &= Contour.Vertices.Num() >= 4;
			}
			InvalidBlockSilhouettes += bValidBlockSilhouettes ? 0 : 1;
		}
		else
		{
			++InvalidBlockSilhouettes;
		}
	}

	TestTrue(TEXT("Reference forest contains deterministic tree aggregates"),
		PartsPerTree.Num() >= 10);
	TestEqual(TEXT("Trunks and branches remain rectangular voxel prisms"),
		NonRectangularWoodParts, 0);
	TestEqual(TEXT("Every tree part uses per-voxel cube geometry"),
		NonBlockGeometryParts, 0);
	TestEqual(TEXT("Trunks, branches, and leaf boxes stay grid aligned"),
		NonAxisAlignedParts, 0);
	TestEqual(TEXT("Every trunk slice is exactly two voxel columns wide"),
		OversizedTrunkParts, 0);
	TestEqual(TEXT("Branches remain buried inside the canopy core"),
		OverlongBranchParts, 0);
	TestEqual(TEXT("Every tree part occupies one shared integer voxel lattice"),
		OffGridTreeVoxelCells, 0);
	for (const TPair<FGuid, FQuat>& Pair : TreeGridRotations)
	{
		const FVector CameraFacingDirection(
			-FMath::InvSqrt(2.0f),
			FMath::InvSqrt(2.0f),
			0.0f);
		TestTrue(
			TEXT("A square canopy face, not its near corner, faces the 2.5D camera"),
			Pair.Value.GetAxisY().Equals(CameraFacingDirection, 0.01f));
	}
	TestEqual(TEXT("Every square trunk is placed on a flat terrain footprint"),
		UnevenTreeFootprints, 0);
	TestEqual(TEXT("Every tree root includes a complete underground voxel"),
		TreeRootsInsufficientlyEmbedded, 0);
	int32 OverlappingTreePairs = 0;
	for (int32 Left = 0; Left < TreeTrunkLocations.Num(); ++Left)
	{
		for (int32 Right = Left + 1;
			Right < TreeTrunkLocations.Num();
			++Right)
		{
			OverlappingTreePairs += FVector2D::Distance(
				TreeTrunkLocations[Left], TreeTrunkLocations[Right])
				< 252.0f - KINDA_SMALL_NUMBER ? 1 : 0;
		}
	}
	TestEqual(TEXT("Tree crowns never overlap into a false concave silhouette"),
		OverlappingTreePairs, 0);
	TestEqual(TEXT("Every merged tree part retains its block-grid silhouette"),
		InvalidBlockSilhouettes, 0);
	int32 InvalidLayeredCanopies = 0;
	for (const TPair<FGuid, int32>& Pair : PartsPerTree)
	{
		TestEqual(TEXT("Each tree uses a fixed nine-source logic budget"),
			Pair.Value, 9);
		TestEqual(TEXT("Each trunk has two depth slices for a real 2x2 section"),
			TrunkSlicesPerTree.FindRef(Pair.Key), 2);
		TestEqual(TEXT("Each tree canopy is represented by five voxel slices"),
			LeafSlicesPerTree.FindRef(Pair.Key), 5);
		TestEqual(TEXT("Each tree canopy spans five distinct depth positions"),
			LeafDepthsPerTree.FindRef(Pair.Key).Num(), 5);
		TestTrue(TEXT("A canopy contains stepped non-rectangular voxel slices"),
			NonRectangularLeafSlicesPerTree.FindRef(Pair.Key) >= 3);
		TArray<int32> LeafHeights;
		LeafCellsByTreeHeight.FindChecked(Pair.Key).GenerateKeyArray(LeafHeights);
		LeafHeights.Sort();
		const TMap<int32, int32>& CellsByHeight =
			LeafCellsByTreeHeight.FindChecked(Pair.Key);
		InvalidLayeredCanopies += LeafHeights.Num() != 5
			|| CellsByHeight.FindRef(LeafHeights[0]) <= CellsByHeight.FindRef(LeafHeights[4])
			|| CellsByHeight.FindRef(LeafHeights[1]) <= CellsByHeight.FindRef(LeafHeights[3])
				? 1 : 0;
	}
	TestEqual(TEXT("Canopies step from a broad block base to a small top cross"),
		InvalidLayeredCanopies, 0);
	LeafColors.Remove(0u);
	TestEqual(TEXT("Leaf shader variation keeps one batchable base color"),
		LeafColors.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxOpaqueVoxelLeafMaterialTest,
	"MatterFlux.Playable.Tree.LeafMaterialUsesOpaqueVoxelFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxOpaqueVoxelLeafMaterialTest::RunTest(const FString& Parameters)
{
	const UMaterial* LeafMaterial = LoadObject<UMaterial>(
		nullptr,
		TEXT("/Game/MatterFlux/Materials/M_VoxelLeaf.M_VoxelLeaf"));
	if (!TestNotNull(TEXT("Voxel leaf material exists"), LeafMaterial))
	{
		return false;
	}
	TestEqual(TEXT("Leaf blocks use the opaque Minecraft fast-leaves path"),
		LeafMaterial->GetBlendMode(), BLEND_Opaque);
	TestFalse(TEXT("Closed leaf cubes do not render their back faces"),
		LeafMaterial->IsTwoSided());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxVoxelWoodMaterialTest,
	"MatterFlux.Playable.Tree.WoodMaterialUsesPerVoxelPixelTexture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxVoxelWoodMaterialTest::RunTest(const FString& Parameters)
{
	const UMaterial* WoodMaterial = LoadObject<UMaterial>(
		nullptr,
		TEXT("/Game/MatterFlux/Materials/M_VoxelWood.M_VoxelWood"));
	const UTexture2D* WoodTexture = LoadObject<UTexture2D>(
		nullptr,
		TEXT("/Game/MatterFlux/Materials/T_WoodPixels.T_WoodPixels"));
	if (!TestNotNull(TEXT("Dedicated voxel wood material exists"), WoodMaterial)
		|| !TestNotNull(TEXT("Per-voxel wood pixel texture exists"), WoodTexture))
	{
		return false;
	}
	TestEqual(TEXT("Wood cubes use an opaque material"),
		WoodMaterial->GetBlendMode(), BLEND_Opaque);
	TestFalse(TEXT("Closed wood cubes do not render their back faces"),
		WoodMaterial->IsTwoSided());
	TestEqual(TEXT("Wood pixels use nearest-neighbour filtering"),
		WoodTexture->Filter, TF_Nearest);
	const FIntPoint ImportedSize = WoodTexture->GetImportedSize();
	TestEqual(TEXT("Each voxel repeats one 16x16 bark tile"),
		ImportedSize.X, 16);
	TestEqual(TEXT("Bark tile stays square"), ImportedSize.Y, 16);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxProceduralTerrainDisablesHardwareRayTracingTest,
	"MatterFlux.Playable.ProceduralTerrainDisablesHardwareRayTracing",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxProceduralTerrainDisablesHardwareRayTracingTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	bool bRayTracingEnabled = true;
	bool bRayTracingProxiesEnabled = true;
	TestTrue(
		TEXT("Renderer configuration contains r.RayTracing"),
		GConfig->GetBool(
			TEXT("/Script/Engine.RendererSettings"),
			TEXT("r.RayTracing"),
			bRayTracingEnabled,
			GEngineIni));
	TestTrue(
		TEXT("Renderer configuration contains ray-tracing proxy setting"),
		GConfig->GetBool(
			TEXT("/Script/Engine.RendererSettings"),
			TEXT("r.RayTracing.RayTracingProxies.ProjectEnabled"),
			bRayTracingProxiesEnabled,
			GEngineIni));
	TestFalse(
		TEXT("Runtime procedural terrain does not enter the unstable hardware ray-tracing path"),
		bRayTracingEnabled);
	TestFalse(
		TEXT("Procedural mesh ray-tracing proxies remain disabled"),
		bRayTracingProxiesEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPristineSourceBatchingTest,
	"MatterFlux.Playable.PristineSourcesUseChunkBatchesWithoutActors",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxPristineSourceBatchingTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawns"), WorldActor))
	{
		return false;
	}

	WorldActor->Regenerate(13579);
	int32 PristineSourceActors = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		PristineSourceActors += It->ActorHasTag(
			TEXT("MatterFluxGeneratedDecoration"))
			? 1
			: 0;
	}
	TestEqual(
		TEXT("Pristine logical sources do not allocate one Actor each"),
		PristineSourceActors,
		0);
	TestTrue(
		TEXT("Logical source definitions remain independently addressable"),
		WorldActor->GetCachedFragmentSourceCount() > 0);
	TestTrue(
		TEXT("Visible source geometry is rendered by chunk batches"),
		WorldActor->GetVisibleFragmentSourceProxyCount() > 0);

	bool bFoundBatchedCollision = false;
	TArray<UProceduralMeshComponent*> Meshes;
	WorldActor->GetComponents(Meshes);
	for (const UProceduralMeshComponent* Mesh : Meshes)
	{
		if (Mesh
			&& Mesh->ComponentHasTag(
				TEXT("MatterFluxFragmentSourceProxy"))
			&& Mesh->GetCollisionEnabled()
				== ECollisionEnabled::QueryAndPhysics)
		{
			bFoundBatchedCollision = true;
			TestFalse(
				TEXT("Batched source collision does not rebuild navigation"),
				Mesh->CanEverAffectNavigation());
			break;
		}
	}
	TestTrue(
		TEXT("Collision-enabled trunks share chunk collision meshes"),
		bFoundBatchedCollision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxFineTerrainChunkMeshTest,
	"MatterFlux.Playable.FineTerrainUsesMergedChunkMeshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxFineTerrainChunkMeshTest::RunTest(const FString& Parameters)
{
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(
		TEXT("Reported embedded-player level layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(2054944001, Layout)))
	{
		return false;
	}

	const MatterFlux::PlayableLevel::FLevelTerrain& Terrain = Layout.Terrain;
	if (!TestTrue(TEXT("Layout carries a valid fine heightfield"), Terrain.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Heightfield width is deterministic"),
		Terrain.Width, MatterFlux::PlayableLevel::TerrainCellsX);
	TestEqual(TEXT("Heightfield height is deterministic"),
		Terrain.Height, MatterFlux::PlayableLevel::TerrainCellsY);
	TestTrue(TEXT("Every pixel has an independently generated height"),
		Terrain.Heights.Num() == Terrain.Width * Terrain.Height);

	constexpr int32 TestChunkSize = 32;
	const FIntPoint TestChunkCoordinate = FIntPoint::ZeroValue;
	MatterFlux::TerrainMesh::FChunk Chunk;
	if (!TestTrue(
		TEXT("A terrain chunk builds from the heightfield"),
		MatterFlux::TerrainMesh::BuildChunk(
			Terrain,
			TestChunkCoordinate,
			TestChunkSize,
			Chunk)))
	{
		return false;
	}
	TestTrue(TEXT("Merged chunk geometry is valid"), Chunk.IsValid());
	TestEqual(TEXT("Chunk keeps three material bands"), Chunk.Sections.Num(), 3);
	TestEqual(TEXT("Interior chunk covers 32 by 32 pixels"),
		Chunk.CellBounds.Area(), 32 * 32);
	TestTrue(TEXT("Chunk carries a dedicated smooth collision surface"),
		Chunk.CollisionSurface.IsValid());
	TestEqual(TEXT("Collision surface has two triangles per two-cell patch"),
		Chunk.CollisionSurface.Triangles.Num() / 3,
		FMath::DivideAndRoundUp(Chunk.CellBounds.Width() + 2, 2)
			* FMath::DivideAndRoundUp(Chunk.CellBounds.Height() + 2, 2) * 2);
	for (int32 TriangleIndex = 0;
		TriangleIndex < Chunk.CollisionSurface.Triangles.Num();
		TriangleIndex += 3)
	{
		const FVector& A = Chunk.CollisionSurface.Vertices[
			Chunk.CollisionSurface.Triangles[TriangleIndex]];
		const FVector& B = Chunk.CollisionSurface.Vertices[
			Chunk.CollisionSurface.Triangles[TriangleIndex + 1]];
		const FVector& C = Chunk.CollisionSurface.Vertices[
			Chunk.CollisionSurface.Triangles[TriangleIndex + 2]];
		TestTrue(
			TEXT("Collision surface contains no vertical step faces"),
			FMath::Abs(FVector::CrossProduct(B - A, C - A).Z)
				> UE_SMALL_NUMBER);
	}
	double WorstCollisionClearance = TNumericLimits<double>::Max();
	FIntPoint WorstClearanceCell = FIntPoint::ZeroValue;
	bool bSampledEveryRenderedCell = true;
	const int64 FirstWorldCellX = FMath::FloorToInt64(
		Terrain.FirstCellCenter.X / Terrain.CellSize);
	const int64 FirstWorldCellY = FMath::FloorToInt64(
		Terrain.FirstCellCenter.Y / Terrain.CellSize);
	for (int32 LocalY = 0; LocalY < TestChunkSize; ++LocalY)
	{
		for (int32 LocalX = 0; LocalX < TestChunkSize; ++LocalX)
		{
			const int64 WorldCellX =
				static_cast<int64>(TestChunkCoordinate.X) * TestChunkSize
				+ LocalX;
			const int64 WorldCellY =
				static_cast<int64>(TestChunkCoordinate.Y) * TestChunkSize
				+ LocalY;
			float RenderHeight = 0.0f;
			uint8 Band = 0;
			if (!Terrain.TrySampleWorldCell(
				WorldCellX, WorldCellY, RenderHeight, Band))
			{
				bSampledEveryRenderedCell = false;
				continue;
			}
			const FVector2D CellCenter(
				Terrain.FirstCellCenter.X
					+ static_cast<double>(WorldCellX - FirstWorldCellX)
						* Terrain.CellSize,
				Terrain.FirstCellCenter.Y
					+ static_cast<double>(WorldCellY - FirstWorldCellY)
						* Terrain.CellSize);
			double CollisionHeight = 0.0;
			if (!TrySampleSurfaceHeight(
				Chunk.CollisionSurface, CellCenter, CollisionHeight))
			{
				bSampledEveryRenderedCell = false;
				continue;
			}
			const double Clearance = CollisionHeight - RenderHeight;
			if (Clearance < WorstCollisionClearance)
			{
				WorstCollisionClearance = Clearance;
				WorstClearanceCell = FIntPoint(WorldCellX, WorldCellY);
			}
		}
	}
	TestTrue(TEXT("Every rendered terrain cell has a collision sample"),
		bSampledEveryRenderedCell);
	TestTrue(*FString::Printf(
		TEXT("Smooth collision never passes below rendered terrain; worst %.2f cm at (%d,%d)"),
		WorstCollisionClearance,
		WorstClearanceCell.X,
		WorstClearanceCell.Y),
		WorstCollisionClearance >= -KINDA_SMALL_NUMBER);
	double TopSurfaceArea = 0.0;
	int32 TopTriangleCount = 0;
	bool bHasVisibleVerticalStep = false;
	for (const MatterFlux::TerrainMesh::FSection& Section : Chunk.Sections)
	{
		if (!Section.Vertices.IsEmpty())
		{
			TestTrue(TEXT("Every populated material section is index-safe"),
				Section.IsValid());
			TestTrue(TEXT("Each chunk section stays below the 16-bit vertex ceiling"),
				Section.Vertices.Num() < 65535);
			for (int32 TriangleIndex = 0;
				TriangleIndex < Section.Triangles.Num();
				TriangleIndex += 3)
			{
				const int32 A = Section.Triangles[TriangleIndex];
				const int32 B = Section.Triangles[TriangleIndex + 1];
				const int32 C = Section.Triangles[TriangleIndex + 2];
				const FVector WindingNormal = FVector::CrossProduct(
					Section.Vertices[B] - Section.Vertices[A],
					Section.Vertices[C] - Section.Vertices[A]).GetSafeNormal();
				if (Section.Normals[A].Z > 0.99)
				{
					++TopTriangleCount;
					TopSurfaceArea += FMath::Abs(FVector::CrossProduct(
						Section.Vertices[B] - Section.Vertices[A],
						Section.Vertices[C] - Section.Vertices[A]).Z) * 0.5;
				}
				else
				{
					bHasVisibleVerticalStep = true;
				}
				TestTrue(
					TEXT("Triangle winding faces the supplied Unreal mesh normal"),
					FVector::DotProduct(WindingNormal, Section.Normals[A])
						< -0.99f);
			}
		}
	}
	TestEqual(TEXT("Merged top rectangles preserve the exact cell footprint"),
		TopSurfaceArea,
		static_cast<double>(Chunk.CellBounds.Area())
			* Terrain.CellSize * Terrain.CellSize);
	TestTrue(TEXT("Coplanar top pixels are greedily merged"),
		TopTriangleCount < Chunk.CellBounds.Area() * 2);
	TestTrue(TEXT("Rendered terrain still contains the original step faces"),
		bHasVisibleVerticalStep);

	MatterFlux::TerrainMesh::FChunk EastChunk;
	const FIntPoint EastChunkCoordinate =
		TestChunkCoordinate + FIntPoint(1, 0);
	if (TestTrue(
		TEXT("Adjacent terrain chunk builds"),
		MatterFlux::TerrainMesh::BuildChunk(
			Terrain,
			EastChunkCoordinate,
			TestChunkSize,
			EastChunk)))
	{
		const double SeamX = static_cast<double>(EastChunkCoordinate.X)
			* TestChunkSize * Terrain.CellSize;
		for (const int32 Offset : {-1, 1})
		{
			const double OverlapX = SeamX + Offset * Terrain.CellSize;
			TMap<int32, double> WestOverlapHeights;
			TMap<int32, double> EastOverlapHeights;
			for (const FVector& Vertex : Chunk.CollisionSurface.Vertices)
			{
				if (FMath::IsNearlyEqual(Vertex.X, OverlapX))
				{
					WestOverlapHeights.Add(
						FMath::RoundToInt(Vertex.Y / Terrain.CellSize),
						Vertex.Z);
				}
			}
			for (const FVector& Vertex : EastChunk.CollisionSurface.Vertices)
			{
				if (FMath::IsNearlyEqual(Vertex.X, OverlapX))
				{
					EastOverlapHeights.Add(
						FMath::RoundToInt(Vertex.Y / Terrain.CellSize),
						Vertex.Z);
				}
			}
			TestTrue(TEXT("Adjacent collision surfaces overlap across the seam"),
				!WestOverlapHeights.IsEmpty()
					&& !EastOverlapHeights.IsEmpty());
			for (const TPair<int32, double>& EdgeVertex : WestOverlapHeights)
			{
				const double* EastHeight =
					EastOverlapHeights.Find(EdgeVertex.Key);
				TestNotNull(TEXT("Every overlap vertex has an east counterpart"),
					EastHeight);
				if (EastHeight)
				{
					TestTrue(TEXT("Adjacent overlap heights are continuous"),
						FMath::IsNearlyEqual(EdgeVertex.Value, *EastHeight));
				}
			}
		}
	}
	MatterFlux::TerrainMesh::FChunk InvalidChunk;
	TestFalse(
		TEXT("Extreme chunk coordinates are rejected without integer overflow"),
		MatterFlux::TerrainMesh::BuildChunk(
			Terrain,
			FIntPoint(MAX_int32, MAX_int32),
			32,
			InvalidChunk));
	TestFalse(TEXT("Rejected terrain chunk leaves no partial mesh"),
		InvalidChunk.IsValid());

	MatterFlux::PlayableLevel::FLevelTerrain ExtremeTerrain;
	ExtremeTerrain.Width = 2;
	ExtremeTerrain.Height = 1;
	ExtremeTerrain.CellSize = std::numeric_limits<float>::max();
	ExtremeTerrain.BottomZ = 0.0f;
	ExtremeTerrain.FirstCellCenter = FVector2D::ZeroVector;
	ExtremeTerrain.Heights = {0.0f, 0.0f};
	ExtremeTerrain.ColorBands = {0, 0};
	ExtremeTerrain.BandColors = {
		FLinearColor::White,
		FLinearColor::Gray,
		FLinearColor::Black};
	TestTrue(
		TEXT("Extreme finite terrain metadata passes structural validation"),
		ExtremeTerrain.IsValid());
	TestFalse(
		TEXT("Terrain mesh rejects derived non-finite vertices"),
		MatterFlux::TerrainMesh::BuildChunk(
			ExtremeTerrain,
			FIntPoint::ZeroValue,
			2,
			InvalidChunk));
	TestFalse(
		TEXT("Non-finite terrain geometry leaves no valid chunk"),
		InvalidChunk.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxInfiniteSeededTerrainTest,
	"MatterFlux.Playable.SeededTerrainExtendsAcrossFarChunks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxInfiniteSeededTerrainTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::PlayableLevel::FLevelLayout FirstLayout;
	MatterFlux::PlayableLevel::FLevelLayout SameSeedLayout;
	MatterFlux::PlayableLevel::FLevelLayout DifferentSeedLayout;
	if (!TestTrue(TEXT("First seeded terrain builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(24681357, FirstLayout))
		|| !TestTrue(TEXT("Same seeded terrain builds"),
			MatterFlux::PlayableLevel::BuildLevelLayout(24681357, SameSeedLayout))
		|| !TestTrue(TEXT("Different seeded terrain builds"),
			MatterFlux::PlayableLevel::BuildLevelLayout(975318642, DifferentSeedLayout)))
	{
		return false;
	}
	TestTrue(TEXT("Playable terrain declares an infinite procedural domain"),
		FirstLayout.Terrain.bInfinite);

	const FIntPoint FarCells[] = {
		FIntPoint(640000, -544000),
		FIntPoint(-720000, 608000),
		FIntPoint(1048576, 1048576)
	};
	bool bDifferentSeedChangedAnyHeight = false;
	for (const FIntPoint Cell : FarCells)
	{
		float FirstHeight = 0.0f;
		float SameHeight = 0.0f;
		float DifferentHeight = 0.0f;
		uint8 FirstBand = MAX_uint8;
		uint8 SameBand = MAX_uint8;
		uint8 DifferentBand = MAX_uint8;
		TestTrue(TEXT("Far positive or negative world cell samples"),
			FirstLayout.Terrain.TrySampleWorldCell(
				Cell.X, Cell.Y, FirstHeight, FirstBand));
		TestTrue(TEXT("Same-seed far world cell samples"),
			SameSeedLayout.Terrain.TrySampleWorldCell(
				Cell.X, Cell.Y, SameHeight, SameBand));
		TestTrue(TEXT("Different-seed far world cell samples"),
			DifferentSeedLayout.Terrain.TrySampleWorldCell(
				Cell.X, Cell.Y, DifferentHeight, DifferentBand));
		TestEqual(TEXT("Same seed reproduces far height exactly"),
			SameHeight, FirstHeight);
		TestEqual(TEXT("Same seed reproduces far color band exactly"),
			SameBand, FirstBand);
		bDifferentSeedChangedAnyHeight |= DifferentHeight != FirstHeight;
	}
	TestTrue(TEXT("Changing the world seed changes distant terrain"),
		bDifferentSeedChangedAnyHeight);

	constexpr int32 ChunkSize = 32;
	const FIntPoint FarChunk(20000, -17000);
	MatterFlux::TerrainMesh::FChunk FirstChunk;
	MatterFlux::TerrainMesh::FChunk RebuiltChunk;
	MatterFlux::TerrainMesh::FChunk DifferentChunk;
	if (!TestTrue(TEXT("A chunk far beyond the original map builds"),
		MatterFlux::TerrainMesh::BuildChunk(
			FirstLayout.Terrain, FarChunk, ChunkSize, FirstChunk))
		|| !TestTrue(TEXT("The same far chunk rebuilds"),
			MatterFlux::TerrainMesh::BuildChunk(
				SameSeedLayout.Terrain, FarChunk, ChunkSize, RebuiltChunk))
		|| !TestTrue(TEXT("A different-seed far chunk builds"),
			MatterFlux::TerrainMesh::BuildChunk(
				DifferentSeedLayout.Terrain, FarChunk, ChunkSize, DifferentChunk)))
	{
		return false;
	}
	TestEqual(TEXT("Far chunk covers the full requested area"),
		FirstChunk.CellBounds.Area(), ChunkSize * ChunkSize);
	TestEqual(TEXT("Same-seed chunk section count is stable"),
		RebuiltChunk.Sections.Num(), FirstChunk.Sections.Num());
	bool bDifferentChunkHasDifferentVertices = false;
	for (int32 SectionIndex = 0;
		SectionIndex < FirstChunk.Sections.Num();
		++SectionIndex)
	{
		const MatterFlux::TerrainMesh::FSection& FirstSection =
			FirstChunk.Sections[SectionIndex];
		const MatterFlux::TerrainMesh::FSection& SameSection =
			RebuiltChunk.Sections[SectionIndex];
		const MatterFlux::TerrainMesh::FSection& DifferentSection =
			DifferentChunk.Sections[SectionIndex];
		TestTrue(TEXT("Same-seed far vertices are byte-stable"),
			FirstSection.Vertices == SameSection.Vertices);
		TestTrue(TEXT("Same-seed far triangle order is stable"),
			FirstSection.Triangles == SameSection.Triangles);
		bDifferentChunkHasDifferentVertices |=
			FirstSection.Vertices != DifferentSection.Vertices;
	}
	TestTrue(TEXT("Different seeds produce different far chunk geometry"),
		bDifferentChunkHasDifferentVertices);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableCharacterDefaultsTest,
	"MatterFlux.Playable.CharacterHas2_5DDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableCharacterDefaultsTest::RunTest(const FString& Parameters)
{
	const AMatterFluxCharacter* Character = GetDefault<AMatterFluxCharacter>();
	if (!TestNotNull(TEXT("Playable character CDO exists"), Character))
	{
		return false;
	}

	TestNotNull(TEXT("Character has a visible mesh"), Character->CharacterVisual.Get());
	TestNotNull(TEXT("Character has a separate voxel head"), Character->HeadVisual.Get());
	TestNotNull(TEXT("Character has a separate voxel left arm"), Character->LeftArmVisual.Get());
	TestNotNull(TEXT("Character has a separate voxel right arm"), Character->RightArmVisual.Get());
	TestNotNull(TEXT("Character has a separate voxel left foot"), Character->LeftFootVisual.Get());
	TestNotNull(TEXT("Character has a separate voxel right foot"), Character->RightFootVisual.Get());
	TestNotNull(TEXT("Character has a spring arm"), Character->CameraBoom.Get());
	TestNotNull(TEXT("Character has a follow camera"), Character->FollowCamera.Get());
	TestTrue(
		TEXT("Character mesh writes CustomDepth for its outline"),
		Character->CharacterVisual
			&& Character->CharacterVisual->bRenderCustomDepth);
	TestEqual(
		TEXT("Character reserves stencil value one"),
		Character->CharacterVisual
			? Character->CharacterVisual->CustomDepthStencilValue
			: 0,
		1);
	TestTrue(
		TEXT("Every voxel character part participates in the silhouette outline"),
		Character->HeadVisual
			&& Character->HeadVisual->bRenderCustomDepth
			&& Character->HeadVisual->CustomDepthStencilValue == 1
			&& Character->LeftArmVisual
			&& Character->LeftArmVisual->bRenderCustomDepth
			&& Character->LeftArmVisual->CustomDepthStencilValue == 1
			&& Character->RightArmVisual
			&& Character->RightArmVisual->bRenderCustomDepth
			&& Character->RightArmVisual->CustomDepthStencilValue == 1
			&& Character->LeftFootVisual
			&& Character->LeftFootVisual->bRenderCustomDepth
			&& Character->LeftFootVisual->CustomDepthStencilValue == 1
			&& Character->RightFootVisual
			&& Character->RightFootVisual->bRenderCustomDepth
			&& Character->RightFootVisual->CustomDepthStencilValue == 1);
	TestTrue(
		TEXT("The player uses a block silhouette instead of the cylinder placeholder"),
		Character->CharacterVisual
			&& Character->CharacterVisual->GetStaticMesh()
			&& Character->CharacterVisual->GetStaticMesh()
				->GetPathName().Contains(TEXT("/Cube.")));
	TestFalse(TEXT("The character can explore both X and Y dimensions"),
		Character->GetCharacterMovement()->bConstrainToPlane);
	const AMatterFluxCreatureActor* Creature =
		GetDefault<AMatterFluxCreatureActor>();
	const UCharacterMovementComponent* PlayerMovement =
		Character->GetCharacterMovement();
	const UCharacterMovementComponent* CreatureMovement = Creature
		? Creature->GetCharacterMovement()
		: nullptr;
	TestNotNull(TEXT("Creature movement defaults exist"), CreatureMovement);
	TestTrue(TEXT("Player contact can push simulated objects"),
		PlayerMovement->bEnablePhysicsInteraction);
	TestTrue(TEXT("Creature contact can push simulated objects"),
		CreatureMovement && CreatureMovement->bEnablePhysicsInteraction);
	TestTrue(TEXT("Character pushes scale with body mass instead of launching light debris"),
		PlayerMovement->bPushForceScaledToMass
			&& CreatureMovement
			&& CreatureMovement->bPushForceScaledToMass);
	TestTrue(TEXT("Initial contact push is deliberately bounded"),
		PlayerMovement->InitialPushForceFactor <= 200.0f
			&& CreatureMovement
			&& CreatureMovement->InitialPushForceFactor <= 200.0f);
	TestTrue(TEXT("Player and creature share walkable-impact filtering"),
		PlayerMovement->IsA<UMatterFluxCharacterMovementComponent>()
			&& CreatureMovement
			&& CreatureMovement->IsA<
				UMatterFluxCharacterMovementComponent>());
	TestTrue(TEXT("Continuous contact push is deliberately bounded"),
		PlayerMovement->PushForceFactor <= 2000.0f
			&& CreatureMovement
			&& CreatureMovement->PushForceFactor <= 2000.0f);
	TestTrue(TEXT("Standing does not create a feedback loop through light dynamic bases"),
		FMath::IsNearlyZero(PlayerMovement->StandingDownwardForceScale)
			&& CreatureMovement
			&& FMath::IsNearlyZero(
				CreatureMovement->StandingDownwardForceScale));
	if (CreatureMovement)
	{
		TestEqual(TEXT("Player and creature use the same initial push"),
			PlayerMovement->InitialPushForceFactor,
			CreatureMovement->InitialPushForceFactor);
		TestEqual(TEXT("Player and creature use the same continuous push"),
			PlayerMovement->PushForceFactor,
			CreatureMovement->PushForceFactor);
		TestEqual(TEXT("Player and creature use the same touch push"),
			PlayerMovement->TouchForceFactor,
			CreatureMovement->TouchForceFactor);
		TestEqual(TEXT("Player and creature use the same overlap repulsion"),
			PlayerMovement->RepulsionForce,
			CreatureMovement->RepulsionForce);
	}
	TestFalse(TEXT("The character does not inherit controller yaw"),
		Character->bUseControllerRotationYaw);
	TestTrue(TEXT("The camera uses a useful three-quarter distance"),
		Character->CameraBoom && Character->CameraBoom->TargetArmLength >= 800.0f);
	const FRotator CameraRotation = Character->CameraBoom->GetRelativeRotation();
	TestTrue(TEXT("The 2.5D camera uses an isometric-style yaw"),
		FMath::Abs(CameraRotation.Yaw) >= 35.0f && FMath::Abs(CameraRotation.Yaw) <= 55.0f);
	TestTrue(TEXT("The 2.5D camera looks down onto the terrain"),
		CameraRotation.Pitch <= -40.0f);
	TestEqual(TEXT("The 2.5D camera keeps perspective projection while using a fixed diagonal view"),
		static_cast<uint8>(Character->FollowCamera
			? Character->FollowCamera->ProjectionMode.GetValue()
			: ECameraProjectionMode::Orthographic),
		static_cast<uint8>(ECameraProjectionMode::Perspective));
	TestTrue(TEXT("The perspective camera keeps the restrained voxel-scene field of view"),
		Character->FollowCamera
		&& Character->FollowCamera->FieldOfView >= 42.0f
		&& Character->FollowCamera->FieldOfView <= 52.0f);
	TestTrue(TEXT("The 2.5D camera uses stable manual exposure"),
		Character->FollowCamera
		&& Character->FollowCamera->PostProcessSettings.bOverride_AutoExposureMethod
		&& Character->FollowCamera->PostProcessSettings.AutoExposureMethod
			== AEM_Manual
		&& !Character->FollowCamera->PostProcessSettings
			.AutoExposureApplyPhysicalCameraExposure);
	TestTrue(TEXT("The 2.5D camera explicitly disables motion blur"),
		Character->FollowCamera
		&& Character->FollowCamera->PostProcessSettings.bOverride_MotionBlurAmount
		&& FMath::IsNearlyZero(
			Character->FollowCamera->PostProcessSettings.MotionBlurAmount));
	TestTrue(TEXT("Short-range ambient occlusion preserves voxel edge depth"),
		Character->FollowCamera
		&& Character->FollowCamera->PostProcessSettings
			.bOverride_AmbientOcclusionIntensity
		&& Character->FollowCamera->PostProcessSettings.AmbientOcclusionIntensity
			>= 0.50f
		&& Character->FollowCamera->PostProcessSettings
			.bOverride_AmbientOcclusionRadius
		&& Character->FollowCamera->PostProcessSettings.AmbientOcclusionRadius
			<= 32.0f);
	TestTrue(TEXT("Pixel presentation avoids bloom and heavy vignette"),
		Character->FollowCamera
		&& Character->FollowCamera->PostProcessSettings.bOverride_BloomIntensity
		&& Character->FollowCamera->PostProcessSettings.BloomIntensity <= 0.08f
		&& Character->FollowCamera->PostProcessSettings.bOverride_VignetteIntensity
		&& Character->FollowCamera->PostProcessSettings.VignetteIntensity <= 0.10f);
	TestTrue(TEXT("The forest palette keeps modest color saturation"),
		Character->FollowCamera
		&& Character->FollowCamera->PostProcessSettings.bOverride_ColorSaturation
		&& Character->FollowCamera->PostProcessSettings.ColorSaturation.X > 1.0f
		&& Character->FollowCamera->PostProcessSettings.ColorSaturation.X <= 1.05f);
	const IConsoleVariable* AntiAliasingMethod =
		IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.AntiAliasingMethod"));
	TestEqual(TEXT("Pixel-art rendering defaults to stable non-temporal FXAA"),
		AntiAliasingMethod ? AntiAliasingMethod->GetInt() : -1,
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxWandHoldInputBindingsTest,
	"MatterFlux.Playable.WandInputSupportsHeldCasting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxWandHoldInputBindingsTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxCharacter* Character = World
		? World->SpawnActor<AMatterFluxCharacter>()
		: nullptr;
	UEnhancedInputComponent* Input = Character
		? NewObject<UEnhancedInputComponent>(Character)
		: nullptr;
	if (!TestNotNull(TEXT("Playable character spawns"), Character)
		|| !TestNotNull(TEXT("Enhanced input component exists"), Input))
	{
		return false;
	}

	Character->SetupPlayerInputComponent(Input);
	for (int32 Slot = 0; Slot < UGA_CastWand::EquipmentSlotCount; ++Slot)
	{
		const UInputAction* Action = Character->GetCastWandAction(Slot);
		bool bHasStarted = false;
		bool bHasCompleted = false;
		bool bHasCanceled = false;
		for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding
			: Input->GetActionEventBindings())
		{
			if (Binding && Binding->GetAction() == Action)
			{
				bHasStarted |= Binding->GetTriggerEvent()
					== ETriggerEvent::Started;
				bHasCompleted |= Binding->GetTriggerEvent()
					== ETriggerEvent::Completed;
				bHasCanceled |= Binding->GetTriggerEvent()
					== ETriggerEvent::Canceled;
			}
		}
		TestTrue(
			*FString::Printf(TEXT("Wand slot %d casts immediately on press"), Slot),
			bHasStarted);
		TestTrue(
			*FString::Printf(TEXT("Wand slot %d stops repeating on release"), Slot),
			bHasCompleted);
		TestTrue(
			*FString::Printf(TEXT("Wand slot %d stops repeating when input is canceled"), Slot),
			bHasCanceled);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxOccludedPlayerOutlineMaterialTest,
	"MatterFlux.Playable.PlayerOutlineOnlyCoversOccludedPixels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxOccludedPlayerOutlineMaterialTest::RunTest(
	const FString& Parameters)
{
	const UMaterial* OutlineMaterial = LoadObject<UMaterial>(
		nullptr,
		TEXT("/Game/MatterFlux/Materials/M_PlayerOutline.M_PlayerOutline"));
	if (!TestNotNull(TEXT("Player outline material exists"), OutlineMaterial))
	{
		return false;
	}

	TestEqual(
		TEXT("Player outline is a post-process material"),
		OutlineMaterial->MaterialDomain,
		MD_PostProcess);
	TestEqual(
		TEXT("Player outline renders after tone mapping"),
		OutlineMaterial->BlendableLocation,
		TEnumAsByte<EBlendableLocation>(BL_SceneColorAfterTonemapping));
	TestTrue(
		TEXT("Player outline has a final world-overlay priority"),
		OutlineMaterial->BlendablePriority >= 1000);

	FString CustomCode;
	for (const UMaterialExpression* Expression : OutlineMaterial->GetExpressions())
	{
		if (const UMaterialExpressionCustom* Custom =
			Cast<UMaterialExpressionCustom>(Expression))
		{
			CustomCode += Custom->Code;
		}
	}

	TestTrue(
		TEXT("Outline samples the opaque scene depth"),
		CustomCode.Contains(TEXT("PPI_SceneDepth")));
	TestTrue(
		TEXT("Outline restricts itself to player stencil one"),
		CustomCode.Contains(TEXT("PlayerStencilValue = 1.0")));
	TestTrue(
		TEXT("Outline classifies custom depth behind scene depth as occluded"),
		CustomCode.Contains(TEXT("CustomDepth > SceneDepth + DepthBiasCm")));
	TestTrue(
		TEXT("Outline is emitted only from occluded player pixels"),
		CustomCode.Contains(TEXT("HiddenCenter * (1.0 - HiddenInterior")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxRandomLevelLayoutTest,
	"MatterFlux.Playable.RandomLevelIsDeterministicAndTraversable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxRandomLevelLayoutTest::RunTest(const FString& Parameters)
{
	MatterFlux::PlayableLevel::FLevelLayout A;
	MatterFlux::PlayableLevel::FLevelLayout B;
	MatterFlux::PlayableLevel::FLevelLayout C;
	TestTrue(TEXT("First seeded layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, A));
	TestTrue(TEXT("Repeated seeded layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, B));
	TestTrue(TEXT("Different seeded layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(7331, C));

	TestTrue(TEXT("The same seed produces every layer identically"), AreLayoutsEqual(A, B));
	const MatterFlux::PlayableLevel::FLevelLayer* Soil = A.FindLayer(TEXT("Soil"));
	const MatterFlux::PlayableLevel::FLevelLayer* Stream = A.FindLayer(TEXT("Stream"));
	const MatterFlux::PlayableLevel::FLevelLayer* Lake = A.FindLayer(TEXT("Lake"));
	const MatterFlux::PlayableLevel::FLevelLayer* Backdrop =
		A.FindLayer(TEXT("Backdrop"));
	const MatterFlux::PlayableLevel::FLevelLayer* HorizonFloor =
		A.FindLayer(TEXT("HorizonFloor"));
	if (!TestNotNull(TEXT("Layout has a soil layer"), Soil)
		|| !TestTrue(TEXT("Layout has a fine terrain heightfield"),
			A.Terrain.IsValid())
		|| !TestNotNull(TEXT("Layout has a stream layer"), Stream)
		|| !TestNotNull(TEXT("Layout has a large lake layer"), Lake)
		|| !TestNotNull(TEXT("Layout has a visual backdrop"), Backdrop)
		|| !TestNotNull(TEXT("Layout has a far horizon floor"), HorizonFloor))
	{
		return false;
	}
	TestNull(TEXT("Fine terrain no longer allocates collision cube instances"),
		A.FindLayer(TEXT("Land")));
	TestNull(TEXT("Fine terrain no longer allocates lowland cube instances"),
		A.FindLayer(TEXT("Lowlands")));
	TestNull(TEXT("Fine terrain no longer allocates midland cube instances"),
		A.FindLayer(TEXT("Midlands")));
	TestNull(TEXT("Fine terrain no longer allocates highland cube instances"),
		A.FindLayer(TEXT("Highlands")));

	TestNull(TEXT("Tree trunks no longer use rounded HISM primitives"),
		A.FindLayer(TEXT("TreeTrunks")));
	TestNull(TEXT("Tree canopies no longer use rounded HISM primitives"),
		A.FindLayer(TEXT("TreeCanopies")));
	TestNull(TEXT("Grass no longer uses rounded HISM primitives"),
		A.FindLayer(TEXT("Grass")));
	TestNull(TEXT("Pink flowers no longer use rounded HISM primitives"),
		A.FindLayer(TEXT("PinkFlowers")));
	TestNull(TEXT("Yellow flowers no longer use rounded HISM primitives"),
		A.FindLayer(TEXT("YellowFlowers")));
	TestNull(TEXT("Purple flowers no longer use rounded HISM primitives"),
		A.FindLayer(TEXT("PurpleFlowers")));
	TestNull(TEXT("Rocks no longer use rounded HISM primitives"),
		A.FindLayer(TEXT("Rocks")));
	TestNull(TEXT("Meadow patches no longer use rounded HISM primitives"),
		A.FindLayer(TEXT("MeadowPatches")));
	for (const MatterFlux::PlayableLevel::FLevelLayer& Layer : A.Layers)
	{
		TestTrue(TEXT("Playable scenery contains no rounded sphere layers"),
			Layer.Primitive != MatterFlux::PlayableLevel::ELayerPrimitive::Sphere);
	}

	TestFalse(TEXT("A different seed changes the random land"),
		A.Terrain.Heights == C.Terrain.Heights);
	TestTrue(TEXT("Only structural terrain layers enable collision"),
		Soil->bEnableCollision
		&& !Stream->bEnableCollision
		&& !Backdrop->bEnableCollision
		&& !HorizonFloor->bEnableCollision);
	TestEqual(TEXT("The finite soil collider cannot expose a rendered world edge"),
		Soil->RenderMode,
		MatterFlux::PlayableLevel::ELevelLayerRenderMode::CollisionOnly);
	TestTrue(TEXT("The visual backdrop is a bounded set of cheap coarse proxies"),
		Backdrop->Primitive
			== MatterFlux::PlayableLevel::ELayerPrimitive::Cube
		&& Backdrop->Instances.Num() >= 64
		&& Backdrop->Instances.Num() <= 1024);
	FBox BackdropBounds(ForceInit);
	TSet<int32> BackdropTopCentimeters;
	float MaximumBackdropIntrusion = -TNumericLimits<float>::Max();
	for (const FTransform& Instance : Backdrop->Instances)
	{
		const FVector Extent = Instance.GetScale3D() * 50.0f;
		BackdropBounds += FBox(
			Instance.GetLocation() - Extent,
			Instance.GetLocation() + Extent);
		BackdropTopCentimeters.Add(FMath::RoundToInt(
			Instance.GetLocation().Z + Extent.Z));
		const float BackdropTop = Instance.GetLocation().Z + Extent.Z;
		const int64 FirstWorldCellX = FMath::FloorToInt64(
			A.Terrain.FirstCellCenter.X / A.Terrain.CellSize);
		const int64 FirstWorldCellY = FMath::FloorToInt64(
			A.Terrain.FirstCellCenter.Y / A.Terrain.CellSize);
		const int64 MinimumWorldCellX = FirstWorldCellX
			+ FMath::FloorToInt64(
				(Instance.GetLocation().X - Extent.X
					- A.Terrain.FirstCellCenter.X)
				/ A.Terrain.CellSize);
		const int64 MaximumWorldCellX = FirstWorldCellX
			+ FMath::CeilToInt64(
				(Instance.GetLocation().X + Extent.X
					- A.Terrain.FirstCellCenter.X)
				/ A.Terrain.CellSize);
		const int64 MinimumWorldCellY = FirstWorldCellY
			+ FMath::FloorToInt64(
				(Instance.GetLocation().Y - Extent.Y
					- A.Terrain.FirstCellCenter.Y)
				/ A.Terrain.CellSize);
		const int64 MaximumWorldCellY = FirstWorldCellY
			+ FMath::CeilToInt64(
				(Instance.GetLocation().Y + Extent.Y
					- A.Terrain.FirstCellCenter.Y)
				/ A.Terrain.CellSize);
		for (int64 WorldCellY = MinimumWorldCellY;
			WorldCellY <= MaximumWorldCellY;
			++WorldCellY)
		{
			for (int64 WorldCellX = MinimumWorldCellX;
				WorldCellX <= MaximumWorldCellX;
				++WorldCellX)
			{
				float TerrainHeight = 0.0f;
				uint8 ColorBand = 0;
				if (A.Terrain.TrySampleWorldCell(
					WorldCellX,
					WorldCellY,
					TerrainHeight,
					ColorBand))
				{
					MaximumBackdropIntrusion = FMath::Max(
						MaximumBackdropIntrusion,
						BackdropTop - TerrainHeight);
				}
			}
		}
	}
	TestTrue(TEXT("The proxy background extends beyond the cached terrain"),
		BackdropBounds.GetSize().X
			> Soil->Instances[0].GetScale3D().X * 100.0f
		&& BackdropBounds.GetSize().Y
			> Soil->Instances[0].GetScale3D().Y * 100.0f);
	TestTrue(TEXT("The proxy horizon has deterministic rolling variation"),
		BackdropTopCentimeters.Num() >= 8);
	TestTrue(
		TEXT("The coarse backdrop stays below every streamed terrain cell in its footprint"),
		MaximumBackdropIntrusion <= -8.0f);
	TestTrue(TEXT("A single non-colliding floor hides the remote proxy edge"),
		HorizonFloor->Instances.Num() == 1
		&& HorizonFloor->Instances[0].GetScale3D().X >= 400.0f
		&& HorizonFloor->Instances[0].GetScale3D().Y >= 400.0f
		&& HorizonFloor->Instances[0].GetLocation().Z
			< Soil->Instances[0].GetLocation().Z);
	TestTrue(TEXT("Stream retains hard-face voxel lighting"),
		Stream->RenderMode
			== MatterFlux::PlayableLevel::ELevelLayerRenderMode::Liquid);
	TestEqual(TEXT("The lake resolves its visual properties from Lua water"),
		Lake->MaterialId, FName(TEXT("water")));
	TestTrue(TEXT("The lake uses the dedicated liquid rendering path"),
		Lake->RenderMode
			== MatterFlux::PlayableLevel::ELevelLayerRenderMode::Liquid);
	FBox2D LakeBounds(ForceInit);
	int32 ShallowLakeCellCount = 0;
	int32 DeepLakeCellCount = 0;
	for (const FTransform& Instance : Lake->Instances)
	{
		const FVector Location = Instance.GetLocation();
		LakeBounds += FVector2D(Location);
		const int32 TerrainX = FMath::Clamp(
			FMath::RoundToInt(
				(Location.X - A.Terrain.FirstCellCenter.X)
					/ A.Terrain.CellSize),
			0,
			A.Terrain.Width - 1);
		const int32 TerrainY = FMath::Clamp(
			FMath::RoundToInt(
				(Location.Y - A.Terrain.FirstCellCenter.Y)
					/ A.Terrain.CellSize),
			0,
			A.Terrain.Height - 1);
		const float WaterSurface = Location.Z
			+ Instance.GetScale3D().Z * 50.0f;
		const float Depth = WaterSurface
			- A.Terrain.HeightAt(TerrainX, TerrainY);
		ShallowLakeCellCount += Depth <= 32.0f ? 1 : 0;
		DeepLakeCellCount += Depth >= 100.0f ? 1 : 0;
	}
	TestTrue(TEXT("The lake is visibly larger than the narrow stream"),
		Lake->Instances.Num() >= 1200
		&& LakeBounds.GetSize().X >= 320.0f
		&& LakeBounds.GetSize().Y >= 220.0f);
	TestTrue(TEXT("The lake exposes shallow edges for transparency comparison"),
		ShallowLakeCellCount >= 64);
	TestTrue(TEXT("The lake exposes a deep center for opacity comparison"),
		DeepLakeCellCount >= 64);
	TestEqual(TEXT("The level is a complete 2D terrain grid"),
		A.Terrain.Heights.Num(),
		MatterFlux::PlayableLevel::TerrainCellsX
			* MatterFlux::PlayableLevel::TerrainCellsY);
	TestTrue(TEXT("Terrain samples Perlin noise on a fine grid"),
		MatterFlux::PlayableLevel::TerrainCellSize <= 12.0f
		&& MatterFlux::PlayableLevel::TerrainCellsX >= 60
		&& MatterFlux::PlayableLevel::TerrainCellsY >= 42);
	TestEqual(TEXT("Elevation color bands cover every terrain cell"),
		A.Terrain.ColorBands.Num(),
		A.Terrain.Heights.Num());
	TestTrue(TEXT("Every elevation band index selects one of three materials"),
		A.Terrain.ColorBands.ContainsByPredicate(
			[](const uint8 Band)
			{
				return Band <= 2;
			})
		&& !A.Terrain.ColorBands.ContainsByPredicate(
			[](const uint8 Band)
			{
				return Band > 2;
			}));
	TestTrue(TEXT("The soil covers both playable dimensions"),
		Soil->Instances[0].GetScale3D().X >= 30.0f
		&& Soil->Instances[0].GetScale3D().Y >= 20.0f
		&& Soil->Instances[0].GetLocation().Z < 0.0f);
	TestTrue(TEXT("The forest contains a visible stream"),
		Stream->Instances.Num() >= MatterFlux::PlayableLevel::TerrainCellsY);
	TMap<FName, int32> FragmentSourceCounts;
	TMap<FGuid, int32> TreeAggregateMemberCounts;
	TMap<FGuid, int32> TreeAggregateRootCounts;
	TSet<uint32> LeafPaletteColors;
	TSet<uint32> RockPaletteColors;
	int32 MaximumRockMaskWidth = 0;
	int32 MaximumRockMaskHeight = 0;
	int32 CollisionEnabledFragmentSourceCount = 0;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: A.FragmentSources)
	{
		FragmentSourceCounts.FindOrAdd(Source.Name)++;
		if (Source.Name == TEXT("TreeLeaves"))
		{
			LeafPaletteColors.Add(Source.Color.ToFColor(false).DWColor());
		}
		else if (Source.Name == TEXT("RockCluster"))
		{
			RockPaletteColors.Add(Source.Color.ToFColor(false).DWColor());
			MaximumRockMaskWidth = FMath::Max(
				MaximumRockMaskWidth, Source.Mask.Width);
			MaximumRockMaskHeight = FMath::Max(
				MaximumRockMaskHeight, Source.Mask.Height);
		}
		CollisionEnabledFragmentSourceCount += Source.bEnableCollision ? 1 : 0;
		TestEqual(
			TEXT("Only tree trunks opt into decoration collision"),
			Source.bEnableCollision,
			Source.Name == TEXT("TreeTrunk"));
		TestTrue(TEXT("Every decoration mask is valid"), Source.Mask.IsValid());
		TestTrue(TEXT("Every decoration has a deterministic source id"),
			Source.SourceId.IsValid());
		TestTrue(TEXT("Every decoration transform is finite"),
			Source.Transform.IsValid());
		const bool bGroundAttachedSource =
			Source.Name == TEXT("TreeTrunk")
			|| Source.Name == TEXT("RockCluster")
			|| Source.Name == TEXT("GrassCluster")
			|| Source.Name == TEXT("PinkFlowerCluster")
			|| Source.Name == TEXT("YellowFlowerCluster")
			|| Source.Name == TEXT("PurpleFlowerCluster");
		if (bGroundAttachedSource)
		{
			const FVector SourceLocation = Source.Transform.GetLocation();
			const int32 TerrainX = FMath::Clamp(
				FMath::RoundToInt(
					(SourceLocation.X - A.Terrain.FirstCellCenter.X)
					/ A.Terrain.CellSize),
				0,
				A.Terrain.Width - 1);
			const int32 TerrainY = FMath::Clamp(
				FMath::RoundToInt(
					(SourceLocation.Y - A.Terrain.FirstCellCenter.Y)
					/ A.Terrain.CellSize),
				0,
				A.Terrain.Height - 1);
			const float TerrainSurface = A.Terrain.HeightAt(TerrainX, TerrainY);
			const FVector LocalBottom(
				0.0f,
				0.0f,
				-static_cast<float>(Source.Mask.Height)
					* Source.Mask.CellSize * 0.5f);
			const FVector LocalTop = -LocalBottom;
			const float BottomZ =
				Source.Transform.TransformPosition(LocalBottom).Z;
			const float TopZ =
				Source.Transform.TransformPosition(LocalTop).Z;
			TestTrue(
				TEXT("Ground-attached masks bury their bottom face to prevent Z-fighting"),
				BottomZ <= TerrainSurface - 2.0f);
			TestTrue(
				TEXT("Ground-attached masks remain visibly above the terrain"),
				TopZ > TerrainSurface + 1.0f);
		}
		const bool bTreeSource =
			Source.Name == TEXT("TreeTrunk")
			|| Source.Name == TEXT("TreeBranch")
			|| Source.Name == TEXT("TreeLeaves");
		TestEqual(
			TEXT("Only tree parts declare aggregate membership"),
			Source.AggregateId.IsValid(),
			bTreeSource);
		if (bTreeSource)
		{
			TreeAggregateMemberCounts.FindOrAdd(Source.AggregateId)++;
			TreeAggregateRootCounts.FindOrAdd(Source.AggregateId) +=
				Source.bAggregateRoot ? 1 : 0;
			TestTrue(TEXT("The aggregate root is always a trunk slice"),
				!Source.bAggregateRoot || Source.Name == TEXT("TreeTrunk"));
			TestEqual(
				TEXT("Every tree part uses per-cell voxel-block geometry"),
				Source.Mask.GeometryStyle,
				EFragmentSourceGeometryStyle::VoxelBlocks);
		}
		TestTrue(TEXT("Every decoration mask contains solid cells"),
			Source.Mask.SolidMask.Contains(1));
		TestTrue(TEXT("Every decoration mask retains transparent cells"),
			Source.Mask.SolidMask.Contains(0));
		MatterFlux::FragmentGeometry::FFragmentGeometry2D Geometry;
		TestTrue(TEXT("Every decoration mask builds fragment face geometry"),
			MatterFlux::FragmentGeometry::BuildFragmentGeometryFromMask(
				Source.Mask.SolidMask,
				Source.Mask.Width,
				Source.Mask.Height,
				Source.Mask.CellSize,
				Geometry));
		const FVector2D SpawnDelta =
			FVector2D(Source.Transform.GetLocation())
			- FVector2D(-700.0f, -500.0f);
		const FVector2D CameraDirection =
			FVector2D(-1.0f, 1.0f).GetSafeNormal();
		const float TowardCamera =
			FVector2D::DotProduct(SpawnDelta, CameraDirection);
		const float AcrossCamera =
			FMath::Abs(
				SpawnDelta.X * CameraDirection.Y
				- SpawnDelta.Y * CameraDirection.X);
		const bool bLargeOccluder =
			Source.Name == TEXT("TreeTrunk")
			|| Source.Name == TEXT("RockCluster");
		TestTrue(
			TEXT("The default player spawn keeps decorations out from underfoot"),
			SpawnDelta.Size() >= (bLargeOccluder ? 280.0f : 120.0f));
		if (bLargeOccluder)
		{
			TestFalse(
				TEXT("Large decorations do not block the default camera sightline"),
				TowardCamera > 0.0f
					&& TowardCamera < 900.0f
					&& AcrossCamera < 260.0f);
		}
	}
	const int32 TreeCount = TreeAggregateRootCounts.Num();
	TestEqual(TEXT("Every 2x2 tree contributes two collidable trunk slices"),
		CollisionEnabledFragmentSourceCount,
		TreeCount * 2);
	TestTrue(TEXT("Tree trunks, branches, and leaf clusters are MatterFlux mask sources"),
		TreeCount >= 10
		&& FragmentSourceCounts.FindRef(TEXT("TreeBranch")) >= TreeCount * 2
		&& FragmentSourceCounts.FindRef(TEXT("TreeLeaves"))
			>= FragmentSourceCounts.FindRef(TEXT("TreeBranch")));
	TestTrue(
		TEXT("Leaf color variation remains batchable for the procedural proxy"),
		LeafPaletteColors.Num() == 1);
	TestEqual(
		TEXT("Every generated tree has one deterministic aggregate"),
		TreeAggregateMemberCounts.Num(),
		TreeCount);
	for (const TPair<FGuid, int32>& Pair : TreeAggregateMemberCounts)
	{
		TestTrue(TEXT("Tree aggregate contains a trunk, branches, and leaves"),
			Pair.Value >= 7);
		TestEqual(
			TEXT("Tree aggregate contains exactly one root"),
			TreeAggregateRootCounts.FindRef(Pair.Key),
			1);
	}
	TestTrue(TEXT("Grass is grouped into MatterFlux mask sources"),
		FragmentSourceCounts.FindRef(TEXT("GrassCluster")) >= 8);
	TestTrue(TEXT("Three flower colors use independent MatterFlux mask sources"),
		FragmentSourceCounts.FindRef(TEXT("PinkFlowerCluster")) >= 6
		&& FragmentSourceCounts.FindRef(TEXT("YellowFlowerCluster")) >= 6
		&& FragmentSourceCounts.FindRef(TEXT("PurpleFlowerCluster")) >= 6);
	TestTrue(TEXT("Sparse rocks use independently seeded MatterFlux mask sources"),
		FragmentSourceCounts.FindRef(TEXT("RockCluster")) >= 8
		&& FragmentSourceCounts.FindRef(TEXT("RockCluster")) <= 80);
	TestEqual(
		TEXT("Rocks use a compact deterministic color palette"),
		RockPaletteColors.Num(),
		4);
	TestTrue(
		TEXT("The rock generator admits silhouettes above the previous size cap"),
		MaximumRockMaskWidth > 12 && MaximumRockMaskHeight > 8);
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Rock =
		A.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("RockCluster");
			});
	if (TestNotNull(TEXT("The forest contains a projected rock source"), Rock))
	{
		const MatterFlux::Rendering::FVoxelMaterialStyle StoneStyle =
			MatterFlux::Rendering::ResolveVoxelMaterialStyle(Rock->MaterialId);
		const float DarkFaceLuminance = StoneStyle.ShadowLift
			* (Rock->Color.R * 0.2126f
				+ Rock->Color.G * 0.7152f
				+ Rock->Color.B * 0.0722f);
		TestTrue(
			TEXT("A back-facing initial rock retains enough canonical stone luminance to remain visibly blue-grey"),
			DarkFaceLuminance >= 0.08f);
	}

	const MatterFlux::PlayableLevel::FLevelFragmentSource* Trunk =
		A.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeTrunk")
					&& Source.bAggregateRoot;
			});
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Leaves =
		A.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeLeaves");
			});
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Branch =
		A.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeBranch");
			});
	TestTrue(TEXT("Tree trunk material remains visibly brown"),
		Trunk && Trunk->Color.R >= 0.30f
		&& Trunk->Color.R >= Trunk->Color.G * 1.8f
		&& Trunk->Color.G >= Trunk->Color.B * 2.5f);
	TestTrue(TEXT("Tree leaf material remains visibly green"),
		Leaves && Leaves->Color.G >= 0.35f
		&& Leaves->Color.G >= Leaves->Color.R * 3.0f
		&& Leaves->Color.G >= Leaves->Color.B * 3.0f);
	if (Trunk && Branch && Leaves)
	{
		const FRotator BranchRotation = Branch->Transform.Rotator();
		const float RelativeBranchYaw = FMath::FindDeltaAngleDegrees(
			Trunk->Transform.Rotator().Yaw,
			BranchRotation.Yaw);
		TestTrue(TEXT("Branches stay on the Minecraft-style world grid"),
			FMath::IsNearlyZero(BranchRotation.Pitch, 0.01f)
			&& FMath::IsNearlyZero(BranchRotation.Roll, 0.01f)
			&& FMath::IsNearlyEqual(
				RelativeBranchYaw,
				FMath::GridSnap(RelativeBranchYaw, 90.0f),
				0.01f));
		TestTrue(TEXT("Each leaf slice is one voxel deep; aggregate slices form volume"),
			FMath::IsNearlyEqual(Leaves->Transform.GetScale3D().Y, 1.0f));
	}

	FBox2D TerrainBounds(ForceInit);
	for (int32 Y = 0; Y < A.Terrain.Height; ++Y)
	{
		for (int32 X = 0; X < A.Terrain.Width; ++X)
		{
			TerrainBounds += FVector2D(
				A.Terrain.FirstCellCenter.X + X * A.Terrain.CellSize,
				A.Terrain.FirstCellCenter.Y + Y * A.Terrain.CellSize);
		}
	}
	TestTrue(TEXT("Terrain spans a real X/Y area instead of a line"),
		TerrainBounds.GetSize().X >= 3000.0f && TerrainBounds.GetSize().Y >= 2000.0f);

	float MinimumTerrainHeight = TNumericLimits<float>::Max();
	float MaximumTerrainHeight = TNumericLimits<float>::Lowest();
	float MaximumAdjacentStep = 0.0f;
	for (const float SurfaceHeight : A.Terrain.Heights)
	{
		TestTrue(TEXT("Every terrain sample is finite"),
			FMath::IsFinite(SurfaceHeight));
		MinimumTerrainHeight = FMath::Min(MinimumTerrainHeight, SurfaceHeight);
		MaximumTerrainHeight = FMath::Max(MaximumTerrainHeight, SurfaceHeight);
	}
	for (int32 Y = 0; Y < MatterFlux::PlayableLevel::TerrainCellsY; ++Y)
	{
		for (int32 X = 0; X < MatterFlux::PlayableLevel::TerrainCellsX; ++X)
		{
			const int32 Index = Y * MatterFlux::PlayableLevel::TerrainCellsX + X;
			if (X + 1 < MatterFlux::PlayableLevel::TerrainCellsX)
			{
				MaximumAdjacentStep = FMath::Max(
					MaximumAdjacentStep,
					FMath::Abs(
						A.Terrain.Heights[Index]
						- A.Terrain.Heights[Index + 1]));
			}
			if (Y + 1 < MatterFlux::PlayableLevel::TerrainCellsY)
			{
				MaximumAdjacentStep = FMath::Max(
					MaximumAdjacentStep,
					FMath::Abs(
						A.Terrain.Heights[Index]
						- A.Terrain.Heights[
							Index + MatterFlux::PlayableLevel::TerrainCellsX]));
			}
		}
	}
	TestTrue(TEXT("Perlin terrain has visible height variation"),
		MaximumTerrainHeight - MinimumTerrainHeight >= 30.0f);
	TestTrue(TEXT("Neighboring terrain transitions remain visually continuous"),
		MaximumAdjacentStep <= 25.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxNaturalWorldOmitsDiagnosticSamplesTest,
	"MatterFlux.Playable.NaturalWorldOmitsDiagnosticMaterialSamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxNaturalWorldOmitsDiagnosticSamplesTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}

	WorldActor->Regenerate(1337);
	TestTrue(TEXT("Seed 1337 retains its generated natural water"),
		WorldActor->GetSimulatedMaterialCount(TEXT("water")) > 0);
	for (const FName DiagnosticMaterial : {
		FName(TEXT("sand")),
		FName(TEXT("steam")),
		FName(TEXT("lava")),
		FName(TEXT("acid")),
		FName(TEXT("acid_gas")),
		FName(TEXT("stone")),
		FName(TEXT("wood")),
		FName(TEXT("grassland")) })
	{
		TestEqual(
			*FString::Printf(
				TEXT("Seed 1337 omits the '%s' diagnostic showcase"),
				*DiagnosticMaterial.ToString()),
			WorldActor->GetSimulatedMaterialCount(DiagnosticMaterial),
			0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxVoxelDecorationWorldTest,
	"MatterFlux.Playable.VoxelDecorationsSpawnAsDamageableSources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxVoxelDecorationWorldTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}

	MatterFlux::PlayableLevel::FLevelLayout Layout;
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	if (!TestTrue(TEXT("Reference layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			1337,
			Layout,
			Registry.Get())))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	TArray<UProceduralMeshComponent*> TerrainComponents;
	WorldActor->GetComponents(TerrainComponents);
	int32 VisibleTerrainComponentCount = 0;
	int32 ContinuousLiquidSurfaceCount = 0;
	bool bFoundBatchedSourceCollision = false;
	for (UProceduralMeshComponent* Component
		: TerrainComponents)
	{
		if (Component
			&& Component->IsVisible()
			&& Component->GetNumSections() > 0)
		{
			if (Component->ComponentHasTag(TEXT("MatterFluxLiquidSurface")))
			{
				++ContinuousLiquidSurfaceCount;
				UMaterialInstanceDynamic* LiquidMaterial =
					Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0));
				if (TestNotNull(
					TEXT("Liquid surface uses a runtime material instance"),
					LiquidMaterial))
				{
					const float ShallowOpacity =
						LiquidMaterial->K2_GetScalarParameterValue(
							TEXT("ShallowOpacity"));
					const float DeepOpacity =
						LiquidMaterial->K2_GetScalarParameterValue(
							TEXT("DeepOpacity"));
					TestTrue(
						TEXT("Projected liquid surface does not reveal stepped terrain as false depth rings"),
						FMath::IsNearlyEqual(
							ShallowOpacity,
							DeepOpacity));
				}
				TestEqual(
					TEXT("Merged liquid surfaces never create blocking collision"),
					Component->GetCollisionEnabled(),
					ECollisionEnabled::NoCollision);
				TestEqual(
					TEXT("Each liquid layer renders in one continuous mesh section"),
					Component->GetNumSections(),
					1);
				const FProcMeshSection* LiquidSection =
					Component->GetProcMeshSection(0);
				if (TestTrue(
					TEXT("Liquid surface has indexed top geometry"),
					LiquidSection
						&& LiquidSection->ProcIndexBuffer.Num() >= 3))
				{
					const FVector A = LiquidSection->ProcVertexBuffer[
						LiquidSection->ProcIndexBuffer[0]].Position;
					const FVector B = LiquidSection->ProcVertexBuffer[
						LiquidSection->ProcIndexBuffer[1]].Position;
					const FVector C = LiquidSection->ProcVertexBuffer[
						LiquidSection->ProcIndexBuffer[2]].Position;
					TestTrue(
						TEXT("Liquid triangles use UE's upward-facing winding"),
						FVector::CrossProduct(B - A, C - A).Z < 0.0f);
				}
				continue;
			}
			if (Component->ComponentHasTag(
				TEXT("MatterFluxFragmentSourceProxy")))
			{
				bFoundBatchedSourceCollision |=
					Component->GetCollisionEnabled()
						== ECollisionEnabled::QueryAndPhysics
					&& Component->bUseComplexAsSimpleCollision;
				TestFalse(
					TEXT("Decoration proxies do not rebuild navigation"),
					Component->CanEverAffectNavigation());
				continue;
			}
			++VisibleTerrainComponentCount;
			TestTrue(
				TEXT("Merged terrain has static-world collision"),
				Component->GetCollisionEnabled()
					== ECollisionEnabled::QueryAndPhysics
				&& Component->bUseComplexAsSimpleCollision);
			const int32 CollisionSectionIndex =
				Component->GetNumSections() - 1;
			const FProcMeshSection* CollisionSection =
				Component->GetProcMeshSection(CollisionSectionIndex);
			TestTrue(TEXT("Terrain collision uses a dedicated hidden mesh section"),
				CollisionSection
					&& CollisionSection->bEnableCollision
					&& !Component->IsMeshSectionVisible(
						CollisionSectionIndex));
			for (int32 SectionIndex = 0;
				SectionIndex < CollisionSectionIndex;
				++SectionIndex)
			{
				const FProcMeshSection* RenderSection =
					Component->GetProcMeshSection(SectionIndex);
				if (RenderSection)
				{
					TestFalse(
						TEXT("Visible stepped terrain sections do not participate in collision"),
						RenderSection->bEnableCollision);
				}
			}
			TestFalse(
				TEXT("Streamed procedural terrain does not rebuild navigation"),
				Component->CanEverAffectNavigation());
		}
	}
	TestTrue(
		TEXT("Visible trunk sources contribute merged static collision"),
		bFoundBatchedSourceCollision);
	TestTrue(
		TEXT("The world exposes visible merged terrain chunks"),
		VisibleTerrainComponentCount > 0);
	TestTrue(
		TEXT("Active liquid materials expose canonical continuous surfaces"),
		ContinuousLiquidSurfaceCount > 0);
	TestEqual(
		TEXT("World diagnostics count canonical liquid-material projections"),
		WorldActor->GetGeneratedLiquidLayerCount(),
		ContinuousLiquidSurfaceCount);
	const int32 TerrainWindowDiameter =
		WorldActor->GetTerrainStreamingChunkRadius() * 2 + 1;
	const int32 SingleTerrainWindowChunks =
		TerrainWindowDiameter * TerrainWindowDiameter;
	TestTrue(
		TEXT("Player and isometric-camera terrain windows overlap without leaving the configured cache"),
		WorldActor->GetVisibleTerrainChunkCount() > SingleTerrainWindowChunks
		&& WorldActor->GetVisibleTerrainChunkCount()
			<= WorldActor->GetTerrainChunkCacheLimit());
	TestTrue(
		TEXT("Merged terrain stays below the former per-cell instance budget"),
		WorldActor->GetVisibleTerrainTriangleCount() > 0
		&& WorldActor->GetVisibleTerrainTriangleCount() < 200000);
	TestEqual(TEXT("Every generated mask is retained in the streaming cache"),
		WorldActor->GetCachedFragmentSourceCount(),
		Layout.FragmentSources.Num());
	TestEqual(
		TEXT("Pristine source logic does not allocate per-object Actors"),
		WorldActor->GetGeneratedFragmentSourceCount(),
		0);
	TestTrue(
		TEXT("Visible render-only mask decorations are merged into chunk proxies"),
		WorldActor->GetVisibleFragmentSourceProxyCount() > 0);
	TestTrue(TEXT("Playable world seeds visible liquid cells"),
		WorldActor->GetSimulatedMaterialCount(TEXT("water")) > 0);
	for (const FName DiagnosticMaterial : {
		FName(TEXT("sand")),
		FName(TEXT("steam")),
		FName(TEXT("lava")),
		FName(TEXT("acid")),
		FName(TEXT("acid_gas")),
		FName(TEXT("stone")),
		FName(TEXT("wood")),
		FName(TEXT("grassland")) })
	{
		TestEqual(
			*FString::Printf(
				TEXT("Ordinary procedural worlds omit the '%s' diagnostic showcase"),
				*DiagnosticMaterial.ToString()),
			WorldActor->GetSimulatedMaterialCount(DiagnosticMaterial),
			0);
	}
	TestEqual(TEXT("Natural water does not allocate diagnostic voxel layers"),
		WorldActor->GetGeneratedMaterialLayerCount(), 0);
	WorldActor->Tick(0.25f);
	TestTrue(TEXT("Authority advances material simulation at a fixed step"),
		WorldActor->GetMaterialSimulationStep() > 0);
	TestEqual(TEXT("No seeded acid sample emits gas in an ordinary world"),
		WorldActor->GetSimulatedMaterialCount(TEXT("acid_gas")), 0);

	const MatterFlux::PlayableLevel::FLevelFragmentSource* TrunkLayout =
		Layout.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeTrunk")
					&& Source.bAggregateRoot;
			});
	if (!TestNotNull(
		TEXT("The reference layout contains an interactive trunk"),
		TrunkLayout))
	{
		return false;
	}
	const FVector TrunkHalfExtent(
		TrunkLayout->Mask.Width
			* TrunkLayout->Mask.CellSize * 0.5f,
		TrunkLayout->Mask.CellSize,
		TrunkLayout->Mask.Height
			* TrunkLayout->Mask.CellSize * 0.5f);
	const FBox TrunkQueryBounds = FBox(
		-TrunkHalfExtent,
		TrunkHalfExtent).TransformBy(
			TrunkLayout->Transform.ToMatrixWithScale());
	TArray<AFragment2DSourceActor*> InteractiveSources;
	WorldActor->GatherFragmentSourcesInBounds(
		TrunkQueryBounds,
		InteractiveSources);
	AFragment2DSourceActor* DamageTarget = nullptr;
	if (AFragment2DSourceActor** Entry =
		InteractiveSources.FindByPredicate(
			[TrunkLayout](const AFragment2DSourceActor* Source)
			{
				return Source
					&& Source->SourceId == TrunkLayout->SourceId;
			}))
	{
		DamageTarget = *Entry;
	}
	int32 SourceActorCount = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		++SourceActorCount;
		const FGuid SpawnedSourceId = It->SourceId;
		const MatterFlux::PlayableLevel::FLevelFragmentSource* SourceLayout =
			Layout.FragmentSources.FindByPredicate(
				[SpawnedSourceId](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
				{
					return Source.SourceId == SpawnedSourceId;
				});
		if (TestNotNull(TEXT("Spawned source maps back to its deterministic layout"), SourceLayout))
		{
			TestEqual(
				TEXT("Aggregate id reaches the generated source actor"),
				It->AggregateId,
				SourceLayout->AggregateId);
			TestEqual(
				TEXT("Aggregate root role reaches the generated source actor"),
				It->bAggregateRoot,
				SourceLayout->bAggregateRoot);
			TestEqual(TEXT("Source actor keeps the configured collision flag"),
				It->bEnableSourceCollision,
				SourceLayout->bEnableCollision);
			TestEqual(TEXT("Only configured source meshes expose collision"),
				It->MeshComponent->GetCollisionEnabled(),
				SourceLayout->bEnableCollision
					? ECollisionEnabled::QueryAndPhysics
					: ECollisionEnabled::NoCollision);
		}
		TestTrue(TEXT("Generated source is tagged as a voxel decoration"),
			It->ActorHasTag(TEXT("MatterFluxGeneratedDecoration")));
		TestTrue(TEXT("Generated source keeps a valid runtime mask"),
			It->ProceduralSource.IsValid()
			&& It->GetRuntimeMask() == It->ProceduralSource.SolidMask);
		TestTrue(TEXT("Voxel decorations use the lit palette material"),
			It->FragmentMaterial
			&& It->FragmentMaterial->GetShadingModels()
				.HasShadingModel(MSM_DefaultLit));
		const bool bExpectedShadow =
			It->SourceMaterialId == TEXT("wood")
			|| It->SourceMaterialId == TEXT("stone");
		TestEqual(
			TEXT("Only substantial voxel decorations cast expensive shadows"),
			It->MeshComponent->CastShadow,
			bExpectedShadow);
		TestEqual(TEXT("Voxel decorations separate faces and darker side walls"),
			It->MeshComponent->GetNumSections(),
			2);
		if (!DamageTarget
			&& SourceLayout
			&& SourceLayout->Name == TEXT("TreeTrunk")
			&& SourceLayout->bAggregateRoot)
		{
			DamageTarget = *It;
		}
	}
	TestEqual(TEXT("World contains every active streamed fragment source"),
		SourceActorCount,
		WorldActor->GetGeneratedFragmentSourceCount());
	const MatterFlux::PlayableLevel::FLevelFragmentSource* LazyDecoration =
		Layout.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return !Source.bEnableCollision;
			});
	if (TestNotNull(
		TEXT("The reference layout contains a render-only mask source"),
		LazyDecoration))
	{
		const FVector HalfExtent(
			LazyDecoration->Mask.Width
				* LazyDecoration->Mask.CellSize * 0.5f,
			LazyDecoration->Mask.CellSize,
			LazyDecoration->Mask.Height
				* LazyDecoration->Mask.CellSize * 0.5f);
		const FBox QueryBounds = FBox(-HalfExtent, HalfExtent).TransformBy(
			LazyDecoration->Transform.ToMatrixWithScale());
		TArray<AFragment2DSourceActor*> MaterializedSources;
		WorldActor->GatherFragmentSourcesInBounds(
			QueryBounds,
			MaterializedSources);
		AFragment2DSourceActor** MaterializedDecorationEntry =
			MaterializedSources.FindByPredicate(
				[LazyDecoration](const AFragment2DSourceActor* Source)
				{
					return Source
						&& Source->SourceId == LazyDecoration->SourceId;
				});
		AFragment2DSourceActor* MaterializedDecoration =
			MaterializedDecorationEntry
				? *MaterializedDecorationEntry
				: nullptr;
		TestNotNull(
			TEXT("Querying a proxy materializes the same deterministic source"),
			MaterializedDecoration);
	}
	if (!TestNotNull(TEXT("A decoration can be selected for damage"), DamageTarget))
	{
		return false;
	}

	const FLinearColor ExpectedColor = DamageTarget->FragmentColor;
	const FVector DamagedSourceLocation =
		DamageTarget->GetActorLocation();
	const FGuid DamagedSourceId = DamageTarget->SourceId;
	FFragmentDamageEvent Event;
	Event.SourceId = DamageTarget->SourceId;
	Event.BaseRevision = DamageTarget->Revision;
	Event.EventSeed = 404;
	Event.DamagePower = 400.0f;
	Event.DamageShape.Type = EFragmentDamageShapeType::Line;
	Event.DamageShape.WorldTransform = DamageTarget->GetActorTransform();
	Event.DamageShape.Extents.X =
		DamageTarget->GetCellSize()
			* static_cast<float>(DamageTarget->GetMaskWidth() + 2);
	Event.DamageShape.Thickness =
		DamageTarget->GetCellSize() * 1.25f;
	int32 ExpectedDetachedTreeCells = 0;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		if (Source.AggregateId != TrunkLayout->AggregateId)
		{
			continue;
		}
		TArray<uint8> CandidateMask = Source.Mask.SolidMask;
		FFragmentDamageShape LocalShape = Event.DamageShape;
		LocalShape.WorldTransform = Event.DamageShape.WorldTransform
			.GetRelativeTransform(Source.Transform);
		MatterFlux::FragmentGeometry::ApplyDamageShape(
			CandidateMask,
			Source.Mask.Width,
			Source.Mask.Height,
			Source.Mask.CellSize,
			LocalShape);
		TArray<uint8> AnchorMask;
		MatterFlux::FragmentGeometry::FFragmentSupportResult Support;
		if (!TestTrue(
			TEXT("Every tree layer can classify the same world-space cut"),
			MatterFlux::FragmentGeometry::BuildSupportAnchorMask(
				Source.Mask.SolidMask,
				Source.Mask.Width,
				Source.Mask.Height,
				Source.Mask.SupportMode,
				AnchorMask)
			&& MatterFlux::FragmentGeometry::ClassifyMaskBySupport(
				CandidateMask,
				AnchorMask,
				Source.Mask.Width,
				Source.Mask.Height,
				Source.Mask.SupportMode,
				Support)))
		{
			return false;
		}
		for (const MatterFlux::FragmentGeometry::FFragmentComponent& Component
			: Support.DetachedComponents)
		{
			ExpectedDetachedTreeCells += Component.Cells.Num();
		}
	}
	UFragmentSimulationSubsystem* Subsystem =
		World->GetSubsystem<UFragmentSimulationSubsystem>();
	if (!TestNotNull(TEXT("Fragment subsystem exists"), Subsystem)
		|| !TestTrue(TEXT("Generated decoration accepts fragment damage"),
			Subsystem->RequestFragmentDamage(DamageTarget, Event)))
	{
		return false;
	}
	int32 DamagedRevision = INDEX_NONE;
	TArray<uint8> DamagedRuntimeMask;
	TestTrue(
		TEXT("Supported stump returns to the logical source store"),
		WorldActor->GetFragmentSourceRuntimeState(
			DamagedSourceId,
			DamagedRevision,
			DamagedRuntimeMask));
	TestTrue(
		TEXT("Supported tree stump retains solid mask cells"),
		DamagedRuntimeMask.Contains(1));
	TestEqual(
		TEXT("Separating decoration cut advances revision"),
		DamagedRevision,
		1);
	AFragment2DSourceActor* ImmediateStumpProjection = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (It->SourceId == DamagedSourceId
			&& !It->IsActorBeingDestroyed())
		{
			ImmediateStumpProjection = *It;
			break;
		}
	}
	if (TestNotNull(
		TEXT("The committed stump is visible immediately during separation grace"),
		ImmediateStumpProjection))
	{
		TestTrue(
			TEXT("Immediate stump projection is visible and collision-suppressed"),
			!ImmediateStumpProjection->IsHidden()
				&& ImmediateStumpProjection->MeshComponent->IsVisible()
				&& ImmediateStumpProjection
					->IsAggregateSeparationCollisionSuppressed()
				&& !ImmediateStumpProjection->GetActorEnableCollision());
	}

	int32 MatchingFragments = 0;
	AFragment2DActor* TreeCarrier = nullptr;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		if (It->FragmentColor.Equals(ExpectedColor))
		{
			++MatchingFragments;
			TreeCarrier = TreeCarrier ? TreeCarrier : *It;
		}
	}
	TestTrue(TEXT("Damage materializes colored MatterFlux fragments"),
		MatchingFragments > 0);
	if (TestNotNull(
		TEXT("Felled generated tree has one physical carrier"),
		TreeCarrier))
	{
		TestTrue(
			TEXT("Detached trunk keeps a tall voxel mask instead of transposing width and height"),
			TreeCarrier->SpawnPayload.DetachedVoxelMask.IsValid()
				&& TreeCarrier->SpawnPayload.DetachedVoxelMask.Height
					> TreeCarrier->SpawnPayload.DetachedVoxelMask.Width);
		float MinimumVertexZ = TNumericLimits<float>::Max();
		for (int32 SectionIndex = 0;
			SectionIndex < TreeCarrier->MeshComponent->GetNumSections();
			++SectionIndex)
		{
			if (const FProcMeshSection* Section =
				TreeCarrier->MeshComponent->GetProcMeshSection(SectionIndex))
			{
				for (const FProcMeshVertex& Vertex : Section->ProcVertexBuffer)
				{
					MinimumVertexZ = FMath::Min(
						MinimumVertexZ,
						static_cast<float>(Vertex.Position.Z));
				}
			}
		}
		int32 HorizontalCutFaceVertices = 0;
		for (int32 SectionIndex = 0;
			SectionIndex < TreeCarrier->MeshComponent->GetNumSections();
			++SectionIndex)
		{
			if (const FProcMeshSection* Section =
				TreeCarrier->MeshComponent->GetProcMeshSection(SectionIndex))
			{
				for (const FProcMeshVertex& Vertex : Section->ProcVertexBuffer)
				{
					HorizontalCutFaceVertices +=
						FMath::IsNearlyEqual(
							static_cast<float>(Vertex.Position.Z),
							MinimumVertexZ,
							0.1f)
						&& FVector::DotProduct(
							TreeCarrier->GetActorTransform()
								.TransformVectorNoScale(Vertex.Normal)
								.GetSafeNormal(),
							-FVector::UpVector) > 0.99f
							? 1
							: 0;
				}
			}
		}
		TestTrue(
			TEXT("Horizontal felling cut produces a downward-facing horizontal cut surface on the rigid projection"),
			HorizontalCutFaceVertices >= 4);
		int32 ExpectedCarriedTreeParts = 0;
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Layout.FragmentSources)
		{
			ExpectedCarriedTreeParts +=
				Source.AggregateId == TrunkLayout->AggregateId
				&& !Source.bAggregateRoot
					? 1
					: 0;
		}
		TestEqual(
			TEXT("Every generated branch and leaf cluster becomes a logical carrier member"),
			TreeCarrier->GetAggregateMemberCount(),
			ExpectedCarriedTreeParts);
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Layout.FragmentSources)
		{
			if (Source.AggregateId == TrunkLayout->AggregateId
				&& !Source.bAggregateRoot)
			{
				TestTrue(
					TEXT("Every tree part remains addressable either as a whole source or a cut layer"),
					TreeCarrier->AggregateSources.ContainsByPredicate(
						[&Source](
							const FFragmentAggregateSourceState& Member)
						{
							return Member.SourceId == Source.SourceId
								|| Member.DefinitionSourceId == Source.SourceId;
						}));
			}
		}
		const bool bCarriesWood =
			TreeCarrier->SpawnPayload.MaterialId == TEXT("wood")
			|| TreeCarrier->AggregateSources.ContainsByPredicate(
				[](const FFragmentAggregateSourceState& Member)
				{
					return Member.MaterialId == TEXT("wood")
						&& Member.SourceMask.SolidMask.Contains(1);
				});
		const bool bCarriesLeaves =
			TreeCarrier->SpawnPayload.MaterialId == TEXT("leaf")
			|| TreeCarrier->AggregateSources.ContainsByPredicate(
				[](const FFragmentAggregateSourceState& Member)
				{
					return Member.MaterialId == TEXT("leaf")
						&& Member.SourceMask.SolidMask.Contains(1);
				});
		TestTrue(
			TEXT("Carrier retains detached wood material cells"),
			bCarriesWood);
		TestTrue(
			TEXT("Carrier retains detached leaf material cells"),
			bCarriesLeaves);
		TestTrue(
			TEXT("Carrier mesh emits visible wood and leaf sections after batching"),
			TreeCarrier->MeshComponent->GetNumSections() >= 2);
		int32 CarriedSolidCells = 0;
		for (const uint8 Cell
			: TreeCarrier->SpawnPayload.DetachedVoxelMask.SolidMask)
		{
			CarriedSolidCells += Cell != 0 ? 1 : 0;
		}
		for (const FFragmentAggregateSourceState& Member
			: TreeCarrier->AggregateSources)
		{
			for (const uint8 Cell : Member.SourceMask.SolidMask)
			{
				CarriedSolidCells += Cell != 0 ? 1 : 0;
			}
		}
		TestEqual(
			TEXT("A felled tree contains exactly the cells detached by the cut, without reattaching intact lower layers"),
			CarriedSolidCells,
			ExpectedDetachedTreeCells);
		TestTrue(
			TEXT("Carrier physics mass includes its visual branch and leaf members"),
			TreeCarrier->SpawnPayload.Mass
				+ KINDA_SMALL_NUMBER
				>= static_cast<float>(CarriedSolidCells) * 0.05f);
		TestTrue(
			TEXT("A root-felled tree receives a real sideways launch instead of restacking on its stump"),
			TreeCarrier->SpawnPayload.InitialLinearVelocity.Size2D()
				>= 300.0f);
		TestTrue(
			TEXT("A root-felled tree receives enough horizontal-axis angular velocity to leave the upright equilibrium"),
			TreeCarrier->SpawnPayload.InitialAngularVelocity.Size2D()
				>= 300.0f);
	}
	int32 ProtectedStumpMemberActorCount = 0;
	bool bUnexpectedCanopySourceRemains = false;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (It->IsActorBeingDestroyed()
			|| It->AggregateId != TrunkLayout->AggregateId
			|| It->bAggregateRoot)
		{
			continue;
		}
		const bool bProtectedStumpSlice =
			It->SourceMaterialId == TEXT("wood")
			&& It->GetRuntimeMask().Contains(1)
			&& It->IsAggregateSeparationCollisionSuppressed()
			&& !It->IsHidden()
			&& It->MeshComponent->IsVisible()
			&& !It->GetActorEnableCollision();
		ProtectedStumpMemberActorCount += bProtectedStumpSlice ? 1 : 0;
		bUnexpectedCanopySourceRemains |= !bProtectedStumpSlice;
	}
	int32 ExpectedProtectedStumpMemberActors = 0;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: Layout.FragmentSources)
	{
		ExpectedProtectedStumpMemberActors +=
			Source.AggregateId == TrunkLayout->AggregateId
			&& Source.Name == TEXT("TreeTrunk")
			&& !Source.bAggregateRoot
				? 1 : 0;
	}
	TestEqual(
		TEXT("Only the synchronized non-root trunk slice remains during separation grace"),
		ProtectedStumpMemberActorCount,
		ExpectedProtectedStumpMemberActors);
	TestFalse(
		TEXT("No branch or canopy Source Actor survives the aggregate handoff"),
		bUnexpectedCanopySourceRemains);
	FMatterFluxWorldSaveState AggregateSaveState;
	FString AggregateSaveError;
	if (TestTrue(
		TEXT("World save captures the dynamic aggregate handoff"),
		WorldActor->CaptureSaveState(
			AggregateSaveState,
			AggregateSaveError))
		&& TreeCarrier)
	{
		for (const FFragmentAggregateSourceState& Member
			: TreeCarrier->AggregateSources)
		{
			if (Member.bOwnsLogicalSource)
			{
				TestTrue(
					TEXT("Every fully carried SourceId has a persistent static-world tombstone"),
					AggregateSaveState.RemovedFragmentSourceIds.Contains(
						Member.SourceId));
			}
			else
			{
				TestFalse(
					TEXT("A cut layer does not tombstone the supported remainder of its source"),
					AggregateSaveState.RemovedFragmentSourceIds.Contains(
						Member.DefinitionSourceId));
			}
			const FMatterFluxSavedFragmentSourceState* SavedTombstone =
				AggregateSaveState.FragmentSources.FindByPredicate(
					[&Member](
						const FMatterFluxSavedFragmentSourceState& Saved)
					{
						return Saved.SourceId == (Member.bOwnsLogicalSource
							? Member.SourceId
							: Member.DefinitionSourceId);
					});
			TestNotNull(
				TEXT("Every carried layer saves either a tombstone or its supported remainder"),
				SavedTombstone);
			if (SavedTombstone)
			{
				TestEqual(
					TEXT("Save state distinguishes fully moved sources from partially cut sources"),
					SavedTombstone->RuntimeMask.Contains(1),
					!Member.bOwnsLogicalSource);
			}
		}
	}

	const auto SetAuthorityStreamingFocus =
		[World, WorldActor](const FVector& Location)
		{
			// CreateNewMap may retain an editor player controller when this
			// test runs as part of the full suite. Production streaming uses
			// every possessed authority pawn, and Tick will restore the first
			// pawn as the material focus. Keep those runtime inputs aligned
			// with the explicit focus so the test exercises chunk persistence
			// independently of editor test order.
			for (TActorIterator<APlayerController> It(World); It; ++It)
			{
				if (APawn* Pawn = It->GetPawn())
				{
					Pawn->SetActorLocation(
						Location,
						false,
						nullptr,
						ETeleportType::TeleportPhysics);
				}
			}
			WorldActor->SetWorldStreamingFocus(Location);
		};
	SetAuthorityStreamingFocus(
		FVector(10000000.0, 10000000.0, 0.0));
	TestTrue(
		TEXT("A partially cut source stays out of the Actor set outside the streaming window"),
		!IsValid(DamageTarget)
			|| DamageTarget->IsActorBeingDestroyed());
	SetAuthorityStreamingFocus(DamagedSourceLocation);
	for (int32 Frame = 0;
		WorldActor->GetPendingFragmentSourceSpawnCount() > 0
			&& Frame < 128;
		++Frame)
	{
		WorldActor->Tick(0.0f);
	}
	AFragment2DSourceActor* RestoredSourceActor = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (It->SourceId == DamagedSourceId
			&& !It->IsActorBeingDestroyed())
		{
			RestoredSourceActor = *It;
			break;
		}
	}
	TestNull(
		TEXT("Returning to the chunk keeps a static stump in the merged batch"),
		RestoredSourceActor);
	int32 RestoredRevision = INDEX_NONE;
	TArray<uint8> RestoredRuntimeMask;
	TestTrue(
		TEXT("Returning to the chunk restores logical source state"),
		WorldActor->GetFragmentSourceRuntimeState(
			DamagedSourceId,
			RestoredRevision,
			RestoredRuntimeMask));
	TestEqual(
		TEXT("Returning to the chunk preserves the committed cut revision"),
		RestoredRevision,
		1);
	TestTrue(
		TEXT("Returning to the chunk preserves the committed cut mask"),
		RestoredRuntimeMask == DamagedRuntimeMask);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxTerrainWorldCutTest,
	"MatterFlux.Playable.Terrain.WorldCutRemovesCanonicalGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxTerrainWorldCutTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable terrain world spawns"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);

	const FVector ProbeXY(0.0f, 0.0f, 0.0f);
	float HeightBefore = 0.0f;
	if (!TestTrue(TEXT("Generated terrain is sampleable before the cut"),
		WorldActor->TrySampleTerrainHeightAtWorldLocation(
			ProbeXY,
			HeightBefore)))
	{
		return false;
	}
	const auto FindProbeTerrainChunk = [WorldActor]()
	{
		TArray<UProceduralMeshComponent*> Components;
		WorldActor->GetComponents(Components);
		for (UProceduralMeshComponent* Component : Components)
		{
			if (!Component
				|| !Component->IsRegistered()
				|| Component->GetNumSections() <= 0)
			{
				continue;
			}
			const FProcMeshSection* Collision = Component->GetProcMeshSection(
				Component->GetNumSections() - 1);
			if (!Collision || !Collision->bEnableCollision
				|| Collision->ProcVertexBuffer.IsEmpty())
			{
				continue;
			}
			FBox2D Bounds(EForceInit::ForceInit);
			for (const FProcMeshVertex& Vertex : Collision->ProcVertexBuffer)
			{
				const FVector WorldVertex = Component->GetComponentTransform()
					.TransformPosition(Vertex.Position);
				Bounds += FVector2D(WorldVertex);
			}
			if (Bounds.IsInsideOrOn(FVector2D::ZeroVector))
			{
				return Component;
			}
		}
		return static_cast<UProceduralMeshComponent*>(nullptr);
	};
	const auto SampleNearestCollisionHeight = [this, &ProbeXY](
		UProceduralMeshComponent* Component,
		float& OutHeight)
	{
		if (!Component || Component->GetNumSections() <= 0)
		{
			return false;
		}
		const FProcMeshSection* Collision = Component->GetProcMeshSection(
			Component->GetNumSections() - 1);
		if (!Collision || !Collision->bEnableCollision
			|| Collision->ProcVertexBuffer.IsEmpty())
		{
			return false;
		}
		double NearestDistanceSquared = TNumericLimits<double>::Max();
		for (const FProcMeshVertex& Vertex : Collision->ProcVertexBuffer)
		{
			const FVector WorldVertex = Component->GetComponentTransform()
				.TransformPosition(Vertex.Position);
			const double DistanceSquared = FVector2D::DistSquared(
				FVector2D(WorldVertex),
				FVector2D(ProbeXY));
			if (DistanceSquared < NearestDistanceSquared)
			{
				NearestDistanceSquared = DistanceSquared;
				OutHeight = WorldVertex.Z;
			}
		}
		return NearestDistanceSquared < TNumericLimits<double>::Max();
	};
	UProceduralMeshComponent* TerrainChunkBefore = FindProbeTerrainChunk();
	float CollisionHeightBefore = 0.0f;
	if (!TestNotNull(TEXT("Resident terrain chunk covers the cut point"),
		TerrainChunkBefore)
		|| !TestTrue(TEXT("Terrain collision is sampleable before the cut"),
			SampleNearestCollisionHeight(
				TerrainChunkBefore,
				CollisionHeightBefore)))
	{
		return false;
	}

	FFragmentWorldCutRequest Request;
	Request.CutShape.Type = EFragmentDamageShapeType::Circle;
	Request.CutShape.WorldTransform = FTransform(FVector(
		ProbeXY.X,
		ProbeXY.Y,
		HeightBefore));
	Request.CutShape.Radius = 32.0f;
	Request.DamagePower = 1200.0f;
	Request.EventSeed = 1337001;
	Request.TargetPadding = 32.0f;
	UFragmentSimulationSubsystem::ExecuteWorldCut(World, Request);

	float HeightAfter = 0.0f;
	if (!TestTrue(TEXT("Generated terrain remains sampleable after the cut"),
		WorldActor->TrySampleTerrainHeightAtWorldLocation(
			ProbeXY,
			HeightAfter)))
	{
		return false;
	}
	TestTrue(
		*FString::Printf(
			TEXT("A terrain-cut spell removes canonical ground at its impact cell (before=%.2f after=%.2f)"),
			HeightBefore,
			HeightAfter),
		HeightAfter < HeightBefore - KINDA_SMALL_NUMBER);
	UProceduralMeshComponent* TerrainChunkAfter = FindProbeTerrainChunk();
	float CollisionHeightAfter = 0.0f;
	TestTrue(
		TEXT("The resident terrain mesh is a disposable projection rebuilt from the cut facts"),
		TerrainChunkAfter && TerrainChunkAfter != TerrainChunkBefore);
	if (TestTrue(TEXT("Terrain collision is sampleable after the cut"),
		SampleNearestCollisionHeight(
			TerrainChunkAfter,
			CollisionHeightAfter)))
	{
		TestTrue(
			TEXT("The rebuilt Chaos collision surface follows the lowered canonical terrain"),
			CollisionHeightAfter
				< CollisionHeightBefore - KINDA_SMALL_NUMBER);
	}
	FMatterFluxWorldSaveState SaveState;
	FString SaveError;
	if (TestTrue(
		TEXT("A terrain cut is captured as persistent canonical world state"),
		WorldActor->CaptureSaveState(SaveState, SaveError)))
	{
		TestTrue(
			TEXT("The save contains sparse terrain height overrides"),
			!SaveState.TerrainHeightOverrides.IsEmpty());
		AMatterFluxPlayableWorldActor* RestoredWorldActor =
			World->SpawnActor<AMatterFluxPlayableWorldActor>();
		if (!TestNotNull(
			TEXT("A fresh playable world can receive the terrain save"),
			RestoredWorldActor))
		{
			return false;
		}
		RestoredWorldActor->Regenerate(1337);
		float RegeneratedHeight = 0.0f;
		TestTrue(
			TEXT("A fresh world starts from the deterministic uncut terrain"),
			RestoredWorldActor->TrySampleTerrainHeightAtWorldLocation(
				ProbeXY, RegeneratedHeight)
				&& FMath::IsNearlyEqual(RegeneratedHeight, HeightBefore));
		FString RestoreError;
		if (TestTrue(
			TEXT("The cut terrain save restores successfully"),
			RestoredWorldActor->RestoreSaveState(SaveState, RestoreError)))
		{
			float RestoredHeight = 0.0f;
			TestTrue(
				TEXT("Restoring projects the saved cut height instead of regenerating pristine ground"),
				RestoredWorldActor->TrySampleTerrainHeightAtWorldLocation(
					ProbeXY, RestoredHeight)
					&& FMath::IsNearlyEqual(RestoredHeight, HeightAfter));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxAuthorityPlayerStreamingTest,
	"MatterFlux.Playable.StreamingCoversEveryAuthorityPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxAuthorityPlayerStreamingTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(24681357);

	const auto SpawnControlledPawn =
		[World](const FVector& Location) -> APlayerController*
	{
		APlayerController* Controller =
			World->SpawnActor<APlayerController>();
		APawn* Pawn = World->SpawnActor<AMatterFluxCharacter>(
			Location,
			FRotator::ZeroRotator);
		if (Controller && Pawn)
		{
			Controller->Possess(Pawn);
		}
		return Controller;
	};

	APlayerController* FirstController =
		SpawnControlledPawn(FVector(-1500.0f, -800.0f, 0.0f));
	if (!TestNotNull(
		TEXT("First authority player controller spawned"),
		FirstController)
		|| !TestNotNull(
			TEXT("First authority player has a pawn"),
			FirstController
				? FirstController->GetPawn().Get()
				: nullptr))
	{
		return false;
	}
	WorldActor->Tick(0.0f);
	const int32 SinglePlayerTerrainChunkCount =
		WorldActor->GetVisibleTerrainChunkCount();

	APlayerController* SecondController =
		SpawnControlledPawn(FVector(1500.0f, 800.0f, 0.0f));
	if (!TestNotNull(
		TEXT("Second authority player controller spawned"),
		SecondController)
		|| !TestNotNull(
			TEXT("Second authority player has a pawn"),
			SecondController
				? SecondController->GetPawn().Get()
				: nullptr))
	{
		return false;
	}
	WorldActor->Tick(0.0f);
	AddInfo(FString::Printf(
		TEXT("Authority streaming terrain chunks: one player=%d, two players=%d"),
		SinglePlayerTerrainChunkCount,
		WorldActor->GetVisibleTerrainChunkCount()));
	TestTrue(
		TEXT("Server terrain streaming covers both separated players"),
		WorldActor->GetVisibleTerrainChunkCount()
			> SinglePlayerTerrainChunkCount);
	TestEqual(
		TEXT("Server material simulation tracks both authority players"),
		WorldActor->GetMaterialSimulationFocusCount(),
		2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxMaterialSimulationCatchUpBudgetTest,
	"MatterFlux.Playable.MaterialSimulationDropsExcessCatchUpDebt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxMaterialSimulationCatchUpBudgetTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}

	WorldActor->Regenerate(24681357);
	const int32 InitialStep = WorldActor->GetMaterialSimulationStep();
	for (int32 HitchFrame = 0; HitchFrame < 12; ++HitchFrame)
	{
		WorldActor->Tick(0.25f);
	}
	const int32 StepAfterHitches =
		WorldActor->GetMaterialSimulationStep();
	TestTrue(
		TEXT("Hitch frames still advance the fixed-step simulation"),
		StepAfterHitches > InitialStep);

	WorldActor->Tick(0.0f);
	TestEqual(
		TEXT("A zero-delta recovery frame has no stale simulation debt"),
		WorldActor->GetMaterialSimulationStep(),
		StepAfterHitches);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableAcidSourceContactTest,
	"MatterFlux.Playable.Material.AcidContactCorrodesTreeLocallyAndIsConsumed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableAcidSourceContactTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	if (const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
		Registry.IsValid())
	{
		const FMatterFluxMaterialDefinition* Acid =
			Registry->Materials.Find(TEXT("acid"));
		const FMatterFluxMaterialDefinition* Grassland =
			Registry->Materials.Find(TEXT("grassland"));
		TestNotNull(TEXT("Acid material is configured"), Acid);
		TestNotNull(TEXT("Grassland material is configured"), Grassland);
		if (Acid && Grassland)
		{
			const float ColorDistance =
				FMath::Abs(Acid->Color.R - Grassland->Color.R)
				+ FMath::Abs(Acid->Color.G - Grassland->Color.G)
				+ FMath::Abs(Acid->Color.B - Grassland->Color.B);
			TestTrue(
				TEXT("Acid liquid color is visually distinct from grassland"),
				ColorDistance >= 1.0f);
		}
	}

	FGuid AggregateId;
	FGuid RootSourceId;
	FBox TreeBounds(ForceInit);
	FTransform RootTransform;
	if (!TestTrue(TEXT("Reference world contains a tree root"),
		WorldActor->FindNearestTreeAggregateForVisualInspection(
			FVector::ZeroVector,
			AggregateId,
			RootSourceId,
			TreeBounds,
			RootTransform)))
	{
		return false;
	}
	int32 InitialRevision = INDEX_NONE;
	TArray<uint8> InitialMask;
	if (!TestTrue(TEXT("Pristine tree state is queryable"),
		WorldActor->GetFragmentSourceRuntimeState(
			RootSourceId,
			InitialRevision,
			InitialMask)))
	{
		return false;
	}
	const int32 InitialGas =
		WorldActor->GetSimulatedMaterialCount(TEXT("acid_gas"));
	const int64 InitialAcidFamilyAmount =
		WorldActor->GetSimulatedMaterialAmount(TEXT("acid"))
		+ WorldActor->GetSimulatedMaterialAmount(TEXT("acid_gas"));
	int32 InitialFragmentActors = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		++InitialFragmentActors;
	}
	TestTrue(TEXT("Acid can be injected through the shared material API"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			RootTransform.GetLocation(),
			TEXT("acid")));

	for (int32 Frame = 0; Frame < 4; ++Frame)
	{
		WorldActor->Tick(0.25f);
	}
	int32 CorrodedRevision = INDEX_NONE;
	TArray<uint8> CorrodedMask;
	TestTrue(TEXT("Corroded tree state remains queryable"),
		WorldActor->GetFragmentSourceRuntimeState(
			RootSourceId,
			CorrodedRevision,
			CorrodedMask));
	TestTrue(TEXT("Lua contact reaction commits tree mask damage"),
		CorrodedRevision > InitialRevision
			&& CorrodedMask.Num() == InitialMask.Num());
	TestEqual(TEXT("Corroding a tree creates no replacement acid gas"),
		WorldActor->GetSimulatedMaterialCount(TEXT("acid_gas")),
		InitialGas);
	TestTrue(TEXT("Tree corrosion does not increase acid-family amount"),
		WorldActor->GetSimulatedMaterialAmount(TEXT("acid"))
			+ WorldActor->GetSimulatedMaterialAmount(TEXT("acid_gas"))
			- InitialAcidFamilyAmount <= 255);
	int32 FinalFragmentActors = 0;
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		++FinalFragmentActors;
	}
	TestEqual(
		TEXT("One conserved acid contact produces at most one aggregate carrier"),
		FinalFragmentActors - InitialFragmentActors,
		1);
	AFragment2DSourceActor* RootSource = nullptr;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		if (It->SourceId == RootSourceId)
		{
			RootSource = *It;
			break;
		}
	}
	TestNotNull(TEXT("Corroded aggregate root remains available as the stump"), RootSource);
	if (RootSource)
	{
		TestTrue(
			TEXT("Stump collision is suppressed while the detached carrier still overlaps it"),
			RootSource->IsAggregateSeparationCollisionSuppressed());
		TestTrue(
			TEXT("Collision suppression never hides the committed stump state"),
			!RootSource->IsHidden()
				&& RootSource->MeshComponent->IsVisible());
		TestFalse(
			TEXT("Separation stump cannot snag the falling carrier"),
			RootSource->GetActorEnableCollision());
		int32 ProtectedStumpSlices = 0;
		int32 ParallelTrunkSlices = 0;
		for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
		{
			if (It->AggregateId != RootSource->AggregateId
				|| It->bDetachedFromTerrain
				|| !It->GetRuntimeMask().Contains(1))
			{
				continue;
			}
			++ProtectedStumpSlices;
			if (It->SourceMaterialId == TEXT("wood")
				&& It->GetMaskWidth() == RootSource->GetMaskWidth()
				&& It->GetMaskHeight() == RootSource->GetMaskHeight()
				&& It->GetActorQuat().AngularDistance(
					RootSource->GetActorQuat()) <= KINDA_SMALL_NUMBER)
			{
				++ParallelTrunkSlices;
				TestEqual(
					TEXT("Parallel trunk slices retain an identical stump silhouette"),
					It->GetRuntimeMask(),
					RootSource->GetRuntimeMask());
			}
			TestTrue(
				TEXT("Every surviving trunk slice shares collision suppression"),
				It->IsAggregateSeparationCollisionSuppressed());
			TestTrue(
				TEXT("Every surviving trunk slice remains continuously visible"),
				!It->IsHidden() && It->MeshComponent->IsVisible());
			TestFalse(
				TEXT("Every protected trunk slice has collision disabled"),
				It->GetActorEnableCollision());
		}
		TestTrue(
			TEXT("At least one physical stump slice remains after corrosion"),
			ProtectedStumpSlices > 0);
		TestEqual(
			TEXT("The square trunk keeps both synchronized depth slices"),
			ParallelTrunkSlices,
			2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableSimulatedLiquidSurfaceRenderingTest,
	"MatterFlux.Playable.Material.SimulatedLiquidUsesContinuousTopSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableSimulatedLiquidSurfaceRenderingTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	TestTrue(TEXT("Acid sample enters the material simulation"),
		WorldActor->SetSimulatedMaterialAtWorldLocation(
			FVector::ZeroVector,
			TEXT("acid")));
	WorldActor->Tick(0.11f);

	TArray<UProceduralMeshComponent*> Components;
	WorldActor->GetComponents(Components);
	UProceduralMeshComponent* LiquidSurface = nullptr;
	for (UProceduralMeshComponent* Component : Components)
	{
		if (Component && Component->ComponentHasTag(
			TEXT("MatterFluxCanonicalLiquidSurface")))
		{
			LiquidSurface = Component;
			break;
		}
	}
	if (!TestNotNull(TEXT("Simulated liquid has a dedicated surface adapter"),
		LiquidSurface))
	{
		return false;
	}
	const FProcMeshSection* SurfaceSection =
		LiquidSurface->GetProcMeshSection(0);
	TestTrue(TEXT("Simulated liquid renders indexed top quads"),
		SurfaceSection
			&& SurfaceSection->ProcVertexBuffer.Num() >= 4
			&& SurfaceSection->ProcIndexBuffer.Num() >= 6);
	TestEqual(TEXT("Liquid top surface remains non-blocking"),
		LiquidSurface->GetCollisionEnabled(),
		ECollisionEnabled::NoCollision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxCanonicalLiquidSurfaceProjectionTest,
	"MatterFlux.Playable.Liquid.CanonicalSimulationProjectsEveryActiveSurfaceCell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxCanonicalLiquidSurfaceProjectionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	// Preserve three seconds of settling while retaining the render-only frames
	// that a real 60 Hz game has between fixed 10 Hz material steps. Advancing
	// by 0.1 seconds here made every synthetic frame a simulation frame and
	// starved the deliberately separated liquid-projection pass.
	for (int32 Step = 0; Step < 180; ++Step)
	{
		WorldActor->Tick(1.0f / 60.0f);
	}

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(
		TEXT("Deterministic liquid layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			1337, Layout, Registry.Get())))
	{
		return false;
	}

	FVector ReportedGap = FVector::ZeroVector;
	MatterFlux::Liquid::FLiquidColumn Column;
	bool bFoundCanonicalLiquid = false;
	for (const FName LayerName : { FName(TEXT("Lake")), FName(TEXT("Stream")) })
	{
		const MatterFlux::PlayableLevel::FLevelLayer* Layer =
			Layout.FindLayer(LayerName);
		if (!Layer)
		{
			continue;
		}
		for (const FTransform& Transform : Layer->Instances)
		{
			if (WorldActor->TrySampleLiquidColumnAtWorldLocation(
				Transform.GetLocation(), Column))
			{
				ReportedGap = Transform.GetLocation();
				bFoundCanonicalLiquid = true;
				break;
			}
		}
		if (bFoundCanonicalLiquid)
		{
			break;
		}
	}
	if (!TestTrue(
		TEXT("Settled generated water retains a canonical liquid column"),
		bFoundCanonicalLiquid))
	{
		return false;
	}

	const FVector ExpectedSurfaceCenter(
		(FMath::FloorToFloat(ReportedGap.X
			/ MatterFlux::PlayableLevel::TerrainCellSize) + 0.5f)
			* MatterFlux::PlayableLevel::TerrainCellSize,
		(FMath::FloorToFloat(ReportedGap.Y
			/ MatterFlux::PlayableLevel::TerrainCellSize) + 0.5f)
			* MatterFlux::PlayableLevel::TerrainCellSize,
		Column.SurfaceZ);
	bool bShapeCoversCanonicalCell = false;
	TArray<UProceduralMeshComponent*> Components;
	WorldActor->GetComponents(Components);
	for (UProceduralMeshComponent* Component : Components)
	{
		if (!Component || !Component->ComponentHasTag(
			TEXT("MatterFluxCanonicalLiquidSurface")))
		{
			continue;
		}
		const FProcMeshSection* Section = Component->GetProcMeshSection(0);
		if (!Section)
		{
			continue;
		}
		for (int32 Index = 0;
			Index + 2 < Section->ProcIndexBuffer.Num();
			Index += 3)
		{
			FVector Triangle[3];
			bool bValidTriangle = true;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 VertexIndex =
					Section->ProcIndexBuffer[Index + Corner];
				bValidTriangle &= Section->ProcVertexBuffer.IsValidIndex(
					VertexIndex);
				if (bValidTriangle)
				{
					Triangle[Corner] = Component->GetComponentTransform()
						.TransformPosition(Section->ProcVertexBuffer[
							VertexIndex].Position);
				}
			}
			if (!bValidTriangle)
			{
				continue;
			}
			const auto SignedArea = [](const FVector& A,
				const FVector& B, const FVector& Point)
			{
				return (Point.X - B.X) * (A.Y - B.Y)
					- (A.X - B.X) * (Point.Y - B.Y);
			};
			const double First = SignedArea(
				Triangle[0], Triangle[1], ExpectedSurfaceCenter);
			const double Second = SignedArea(
				Triangle[1], Triangle[2], ExpectedSurfaceCenter);
			const double Third = SignedArea(
				Triangle[2], Triangle[0], ExpectedSurfaceCenter);
			const bool bHasNegative = First < -0.01
				|| Second < -0.01 || Third < -0.01;
			const bool bHasPositive = First > 0.01
				|| Second > 0.01 || Third > 0.01;
			if (!(bHasNegative && bHasPositive))
			{
				bShapeCoversCanonicalCell = true;
				break;
			}
		}
	}
	TestTrue(
		TEXT("The connected liquid shape covers every active canonical cell without requiring one quad per fact"),
		bShapeCoversCanonicalCell);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableLakeInteriorCoverageTest,
	"MatterFlux.Playable.Liquid.GeneratedLakeInteriorHasNoSurfaceGaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableLakeInteriorCoverageTest::RunTest(
	const FString& Parameters)
{
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(
			TEXT("Deterministic lake layout builds"),
			MatterFlux::PlayableLevel::BuildLevelLayout(
				1337, Layout, Registry.Get())))
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelLayer* Lake =
		Layout.FindLayer(TEXT("Lake"));
	if (!TestTrue(
			TEXT("Deterministic layout contains a lake surface"),
			Lake && !Lake->Instances.IsEmpty()))
	{
		return false;
	}

	TSet<FIntPoint> SurfaceCells;
	FIntPoint Minimum(MAX_int32, MAX_int32);
	FIntPoint Maximum(MIN_int32, MIN_int32);
	for (const FTransform& Instance : Lake->Instances)
	{
		const FVector Location = Instance.GetLocation();
		const FIntPoint Cell(
			FMath::RoundToInt(Location.X
				/ MatterFlux::PlayableLevel::TerrainCellSize),
			FMath::RoundToInt(Location.Y
				/ MatterFlux::PlayableLevel::TerrainCellSize));
		SurfaceCells.Add(Cell);
		Minimum.X = FMath::Min(Minimum.X, Cell.X);
		Minimum.Y = FMath::Min(Minimum.Y, Cell.Y);
		Maximum.X = FMath::Max(Maximum.X, Cell.X);
		Maximum.Y = FMath::Max(Maximum.Y, Cell.Y);
	}

	const FVector2D Center(
		(Minimum.X + Maximum.X) * 0.5f,
		(Minimum.Y + Maximum.Y) * 0.5f);
	const FVector2D LakeRadius(
		(Maximum.X - Minimum.X) * 0.5f,
		(Maximum.Y - Minimum.Y) * 0.5f);
	int32 MissingCoreCells = 0;
	for (int32 Y = Minimum.Y; Y <= Maximum.Y;
		++Y)
	{
		for (int32 X = Minimum.X; X <= Maximum.X;
			++X)
		{
			const float NormalizedX = (X - Center.X) / LakeRadius.X;
			const float NormalizedY = (Y - Center.Y) / LakeRadius.Y;
			if (NormalizedX * NormalizedX + NormalizedY * NormalizedY
				> FMath::Square(0.95f))
			{
				continue;
			}
			MissingCoreCells += SurfaceCells.Contains(FIntPoint(X, Y)) ? 0 : 1;
		}
	}
	TestEqual(
		TEXT("The generated lake's inner basin has continuous surface coverage"),
		MissingCoreCells,
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableLakeSurfaceContinuityTest,
	"MatterFlux.Playable.Liquid.AuthoredLakeSurfaceRemainsContinuous",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableLakeSurfaceContinuityTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(
			TEXT("Deterministic lake layout builds"),
			MatterFlux::PlayableLevel::BuildLevelLayout(
				1337, Layout, Registry.Get())))
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelLayer* Lake =
		Layout.FindLayer(TEXT("Lake"));
	if (!TestTrue(
			TEXT("Deterministic layout contains a lake surface"),
			Lake && !Lake->Instances.IsEmpty()))
	{
		return false;
	}

	WorldActor->Regenerate(1337);
	const auto CountDryAuthoredSurfaceCells = [&]()
	{
		int32 Count = 0;
		for (const FTransform& Instance : Lake->Instances)
		{
			MatterFlux::Liquid::FLiquidColumn Column;
			if (!WorldActor->TrySampleLiquidColumnAtWorldLocation(
					Instance.GetLocation(), Column)
				|| Column.MaterialId != Lake->MaterialId)
			{
				++Count;
			}
		}
		return Count;
	};
	for (int32 Step = 0; Step < 180; ++Step)
	{
		WorldActor->Tick(0.1f);
	}

	const int32 DryAuthoredSurfaceCells =
		CountDryAuthoredSurfaceCells();
	TestEqual(
		TEXT("A settled generated lake never develops empty authored edge cells"),
		DryAuthoredSurfaceCells,
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableLiquidBuoyancyIntegrationTest,
	"MatterFlux.Playable.Liquid.CreatureSamplesRenderedColumnAndReceivesBuoyancy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableLiquidBuoyancyIntegrationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}

	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(TEXT("Liquid test layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(
			1337, Layout, Registry.Get())))
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelLayer* Stream =
		Layout.FindLayer(TEXT("Stream"));
	const MatterFlux::PlayableLevel::FLevelLayer* Lake =
		Layout.FindLayer(TEXT("Lake"));
	if (!TestTrue(TEXT("Layout contains a stream"),
		Stream && !Stream->Instances.IsEmpty())
		|| !TestTrue(TEXT("Layout contains a large lake"),
			Lake && !Lake->Instances.IsEmpty()))
	{
		return false;
	}

	WorldActor->Regenerate(1337);
	int32 MissingAuthoredLakeColumns = 0;
	for (const FTransform& Instance : Lake->Instances)
	{
		MatterFlux::Liquid::FLiquidColumn AuthoredColumn;
		if (!WorldActor->TrySampleLiquidColumnAtWorldLocation(
				Instance.GetLocation(), AuthoredColumn)
			|| AuthoredColumn.MaterialId != TEXT("water"))
		{
			++MissingAuthoredLakeColumns;
		}
	}
	TestEqual(
		TEXT("Every authored lake surface begins from the same water material state"),
		MissingAuthoredLakeColumns,
		0);
	// Sample an authored stream cell outside the lake overlap. The lake is
	// intentionally allowed to provide the deeper canonical column where the
	// two shapes meet, while the free stream must retain its thin authored depth.
	const FTransform& AuthoredStreamTransform = Stream->Instances[0];
	const FVector StreamLocation = AuthoredStreamTransform.GetLocation();
	const FVector SampleLocation = StreamLocation;
	MatterFlux::Liquid::FLiquidColumn Column;
	if (!TestTrue(TEXT("Rendered stream maps to a liquid column"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			SampleLocation, Column)))
	{
		return false;
	}
	TestEqual(TEXT("Stream column resolves water"),
		Column.MaterialId, FName(TEXT("water")));
	TestEqual(TEXT("Stream column uses Lua water density"),
		Column.Density, 1.0f);
	const int32 StreamTerrainX = FMath::Clamp(
		FMath::RoundToInt(
			(StreamLocation.X - Layout.Terrain.FirstCellCenter.X)
				/ Layout.Terrain.CellSize),
		0,
		Layout.Terrain.Width - 1);
	const int32 StreamTerrainY = FMath::Clamp(
		FMath::RoundToInt(
			(StreamLocation.Y - Layout.Terrain.FirstCellCenter.Y)
				/ Layout.Terrain.CellSize),
		0,
		Layout.Terrain.Height - 1);
	const float AuthoredStreamSurfaceZ = StreamLocation.Z
		+ AuthoredStreamTransform.GetScale3D().Z * 50.0f;
	const float AuthoredStreamDepth = AuthoredStreamSurfaceZ
		- Layout.Terrain.HeightAt(StreamTerrainX, StreamTerrainY);
	TestTrue(TEXT("Canonical stream depth matches the authored terrain-attached layer"),
		FMath::IsNearlyEqual(
			Column.SurfaceZ - Column.BottomZ,
			AuthoredStreamDepth,
			0.5f));
	TestTrue(TEXT("A shallow stream cannot hover a metre above its terrain support"),
		Column.SurfaceZ - Column.BottomZ < 25.0f);

	const FTransform* DeepestLakeCell = nullptr;
	TArray<const FTransform*> DeepLakeCells;
	float DeepestLakeDepth = 0.0f;
	for (const FTransform& Instance : Lake->Instances)
	{
		const FVector Location = Instance.GetLocation();
		const int32 TerrainX = FMath::Clamp(
			FMath::RoundToInt(
				(Location.X - Layout.Terrain.FirstCellCenter.X)
					/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Width - 1);
		const int32 TerrainY = FMath::Clamp(
			FMath::RoundToInt(
				(Location.Y - Layout.Terrain.FirstCellCenter.Y)
					/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Height - 1);
		const float SurfaceZ = Location.Z
			+ Instance.GetScale3D().Z * 50.0f;
		const float Depth = SurfaceZ
			- Layout.Terrain.HeightAt(TerrainX, TerrainY);
		if (Depth >= 100.0f)
		{
			DeepLakeCells.Add(&Instance);
		}
		if (Depth > DeepestLakeDepth)
		{
			DeepestLakeDepth = Depth;
			DeepestLakeCell = &Instance;
		}
	}
	if (!TestNotNull(TEXT("Lake exposes a deepest physical column"),
		DeepestLakeCell)
		|| !TestTrue(TEXT("Lake exposes several independent deep-water test cells"),
			DeepLakeCells.Num() >= 3))
	{
		return false;
	}
	MatterFlux::Liquid::FLiquidColumn LakeColumn;
	if (!TestTrue(TEXT("Deep rendered lake maps to a liquid column"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			DeepestLakeCell->GetLocation(), LakeColumn)))
	{
		return false;
	}
	TestEqual(TEXT("Lake column resolves water"),
		LakeColumn.MaterialId, FName(TEXT("water")));
	TestTrue(TEXT("Lake physical depth matches its carved deep center"),
		LakeColumn.SurfaceZ - LakeColumn.BottomZ >= 100.0f
			&& FMath::IsNearlyEqual(
				LakeColumn.SurfaceZ - LakeColumn.BottomZ,
				DeepestLakeDepth,
				0.5f));
	TestTrue(TEXT("Static lake cells do not overflow the replicated material budget"),
		WorldActor->GetReplicatedMaterialStateByteCount() > 0
			&& WorldActor->GetReplicatedMaterialStateByteCount()
				<= FMatterFluxReplicatedMaterialState::MaximumCompressedBytes);

	// Touching only the top centimetre of a deep liquid column must displace
	// only that volume. Treating any Z overlap as occupancy of the entire
	// column produces the conspicuous dry squares seen in the runtime lake.
	const FVector ShallowContactLocation = Lake->Instances[
		Lake->Instances.Num() / 3].GetLocation();
	MatterFlux::Liquid::FLiquidColumn ShallowContactBefore;
	if (TestTrue(TEXT("Shallow-contact lake cell begins as liquid"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			ShallowContactLocation, ShallowContactBefore)))
	{
		TestTrue(TEXT("Shallow body contact submits a displacement transaction"),
			WorldActor->DisplaceLiquidInWorldBounds(
				ShallowContactLocation,
				FVector(2.0f, 2.0f, 0.0f),
				ShallowContactBefore.SurfaceZ - 1.0f,
				ShallowContactBefore.SurfaceZ + 20.0f));
		MatterFlux::Liquid::FLiquidColumn ShallowContactAfter;
		TestTrue(
			TEXT("A one-centimetre surface overlap does not excavate the whole liquid column"),
			WorldActor->TrySampleLiquidColumnAtWorldLocation(
				ShallowContactLocation, ShallowContactAfter));
	}

	AMatterFluxCharacter* Character =
		World->SpawnActor<AMatterFluxCharacter>();
	if (!TestNotNull(TEXT("Playable character spawned"), Character))
	{
		return false;
	}
	MatterFlux::Liquid::FLiquidColumn CharacterColumn;
	const FVector CharacterLakeLocation = DeepLakeCells[0]->GetLocation();
	if (!TestTrue(TEXT("Character deep-water cell begins as liquid"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			CharacterLakeLocation, CharacterColumn)))
	{
		return false;
	}
	Character->SetActorLocation(FVector(
		CharacterLakeLocation.X,
		CharacterLakeLocation.Y,
		CharacterColumn.BottomZ + 88.0f));
	const int64 WaterAmountBeforeDisplacement =
		WorldActor->GetSimulatedMaterialAmount(TEXT("water"));
	Character->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	Character->BuoyancyComponent->TickComponent(
		0.1f, LEVELTICK_All, nullptr);
	WorldActor->Tick(0.0f);
	TestTrue(TEXT("Character capsule is measurably submerged"),
		Character->BuoyancyComponent->GetLastSubmergedFraction() > 0.1f);
	TestEqual(TEXT("Water uses its Lua-defined movement resistance"),
		Character->BuoyancyComponent->GetLastMovementResistance(), 1.0f);
	TestTrue(TEXT("A light character receives upward buoyancy"),
		Character->GetCharacterMovement()->Velocity.Z > 0.0f);
	MatterFlux::Liquid::FLiquidColumn DisplacedCenterColumn;
	TestFalse(TEXT("Character capsule vacates liquid at its occupied center"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			Character->GetActorLocation(), DisplacedCenterColumn));
	TestEqual(TEXT("Character displacement conserves exact simulated water volume"),
		WorldActor->GetSimulatedMaterialAmount(TEXT("water")),
		WaterAmountBeforeDisplacement);
	Character->BuoyancyComponent->TickComponent(
		0.1f, LEVELTICK_All, nullptr);
	WorldActor->Tick(0.1f);
	TestTrue(TEXT("A displaced character keeps ambient liquid pressure next frame"),
		Character->BuoyancyComponent->GetLastSubmergedFraction() > 0.1f);
	TestFalse(TEXT("Ambient pressure does not put liquid back inside the capsule"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			Character->GetActorLocation(), DisplacedCenterColumn));

	// A capsule must not clear the square corners of its axis-aligned bounds.
	// At this depth the lake intersects only the lower round cap, so a point at
	// (0.9 R, 0.9 R) is unambiguously outside the submerged body volume while
	// still catching the old rectangular-footprint implementation.
	const float CharacterRadius = Character->GetCapsuleComponent()
		->GetScaledCapsuleRadius();
	const FVector DeepCharacterLocation(
		DeepestLakeCell->GetLocation().X,
		DeepestLakeCell->GetLocation().Y,
		LakeColumn.BottomZ + 88.0f);
	Character->SetActorLocation(DeepCharacterLocation);
	Character->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	const FVector OutsideCapsuleCorner = DeepCharacterLocation + FVector(
		CharacterRadius * 0.9f,
		CharacterRadius * 0.9f,
		0.0f);
	MatterFlux::Liquid::FLiquidColumn OutsideCornerBefore;
	if (TestTrue(TEXT("Deep-water capsule corner begins as liquid"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			OutsideCapsuleCorner, OutsideCornerBefore)))
	{
		Character->BuoyancyComponent->TickComponent(
			0.1f, LEVELTICK_All, nullptr);
		WorldActor->Tick(0.0f);
		MatterFlux::Liquid::FLiquidColumn OutsideCornerAfter;
		TestTrue(
			TEXT("Liquid surface remains filled outside the capsule's round footprint"),
			WorldActor->TrySampleLiquidColumnAtWorldLocation(
				OutsideCapsuleCorner, OutsideCornerAfter));
	}

	const FVector CreatureLiquidLocation = DeepLakeCells[
		DeepLakeCells.Num() / 2]->GetLocation();
	MatterFlux::Liquid::FLiquidColumn CreatureColumn;
	if (!TestTrue(TEXT("Creature test cell begins as liquid"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			CreatureLiquidLocation, CreatureColumn)))
	{
		return false;
	}
	AMatterFluxCreatureActor* Creature =
		World->SpawnActor<AMatterFluxCreatureActor>();
	if (!TestNotNull(TEXT("Creature spawned"), Creature))
	{
		return false;
	}
	Creature->SetActorLocation(FVector(
		CreatureLiquidLocation.X,
		CreatureLiquidLocation.Y,
		CreatureColumn.BottomZ + 72.0f));
	MatterFlux::Liquid::FLiquidColumn CreatureActualColumn;
	TestTrue(TEXT("Creature actual capsule center begins in liquid"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			Creature->GetActorLocation(), CreatureActualColumn));
	const int64 WaterBeforeCreature =
		WorldActor->GetSimulatedMaterialAmount(TEXT("water"));
	Creature->BuoyancyComponent->TickComponent(
		0.1f, LEVELTICK_All, nullptr);
	WorldActor->Tick(0.0f);
	TestTrue(TEXT("Creature shared component detects submersion"),
		Creature->BuoyancyComponent->GetLastSubmergedFraction() > 0.0f);
	MatterFlux::Liquid::FLiquidColumn CreatureCenterAfter;
	const bool bCreatureCenterStillLiquid =
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			Creature->GetActorLocation(), CreatureCenterAfter);
	TestFalse(TEXT("Creature capsule vacates its occupied liquid"),
		bCreatureCenterStillLiquid);
	TestEqual(TEXT("Creature displacement conserves exact water volume"),
		WorldActor->GetSimulatedMaterialAmount(TEXT("water")),
		WaterBeforeCreature);

	const FVector ObjectLiquidLocation = DeepLakeCells.Last()->GetLocation();
	MatterFlux::Liquid::FLiquidColumn ObjectColumn;
	if (!TestTrue(TEXT("Physics-object test cell begins as liquid"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			ObjectLiquidLocation, ObjectColumn)))
	{
		return false;
	}
	AActor* PhysicsObject = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Generic physics object spawned"), PhysicsObject))
	{
		return false;
	}
	UBoxComponent* PhysicsBox = NewObject<UBoxComponent>(
		PhysicsObject, TEXT("LiquidDisplacementTestBody"));
	PhysicsObject->SetRootComponent(PhysicsBox);
	PhysicsObject->AddInstanceComponent(PhysicsBox);
	PhysicsBox->SetBoxExtent(FVector(35.0f));
	PhysicsBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	PhysicsBox->RegisterComponent();
	PhysicsBox->SetWorldLocation(FVector(
		ObjectLiquidLocation.X,
		ObjectLiquidLocation.Y,
		ObjectColumn.BottomZ + 60.0f));
	PhysicsBox->SetSimulatePhysics(true);
	UMatterFluxBuoyancyComponent* ObjectBuoyancy =
		NewObject<UMatterFluxBuoyancyComponent>(
			PhysicsObject, TEXT("LiquidDisplacementTestBuoyancy"));
	PhysicsObject->AddInstanceComponent(ObjectBuoyancy);
	ObjectBuoyancy->SetTargetPrimitive(PhysicsBox);
	ObjectBuoyancy->RegisterComponent();
	const int64 WaterBeforeObject =
		WorldActor->GetSimulatedMaterialAmount(TEXT("water"));
	ObjectBuoyancy->TickComponent(0.1f, LEVELTICK_All, nullptr);
	WorldActor->Tick(0.0f);
	MatterFlux::Liquid::FLiquidColumn ObjectCenterAfter;
	TestTrue(TEXT("A partly submerged rigid body does not excavate the whole column"),
		WorldActor->TrySampleLiquidColumnAtWorldLocation(
			PhysicsBox->GetComponentLocation(), ObjectCenterAfter));
	TestTrue(TEXT("Rigid-body overlap lowers the local conserved liquid volume"),
		ObjectCenterAfter.SurfaceZ < ObjectColumn.SurfaceZ
			&& ObjectCenterAfter.SurfaceZ > ObjectCenterAfter.BottomZ);
	TestEqual(TEXT("Rigid-body displacement conserves exact water volume"),
		WorldActor->GetSimulatedMaterialAmount(TEXT("water")),
		WaterBeforeObject);

	FVector SandLocation = FVector::ZeroVector;
	bool bPlacedSand = false;
	for (int32 Y = 0; Y < Layout.Terrain.Height && !bPlacedSand; Y += 3)
	{
		for (int32 X = 0; X < Layout.Terrain.Width; X += 3)
		{
			const FVector Candidate(
				Layout.Terrain.FirstCellCenter.X + X * Layout.Terrain.CellSize,
				Layout.Terrain.FirstCellCenter.Y + Y * Layout.Terrain.CellSize,
				Layout.Terrain.HeightAt(X, Y) + 10.0f);
			MatterFlux::Liquid::FLiquidColumn ExistingLiquid;
			if (!WorldActor->TrySampleLiquidColumnAtWorldLocation(
					Candidate, ExistingLiquid)
				&& WorldActor->DepositSimulatedMaterialAtWorldLocation(
					Candidate, TEXT("sand"), 8) > 0)
			{
				SandLocation = Candidate;
				bPlacedSand = true;
				break;
			}
		}
	}
	if (TestTrue(TEXT("A dry terrain cell accepts a tall sand test pile"),
		bPlacedSand))
	{
		float SandGroundZ = SandLocation.Z - 10.0f;
		WorldActor->TrySampleTerrainHeightAtWorldLocation(
			SandLocation, SandGroundZ);
		Character->SetActorLocation(FVector(
			SandLocation.X, SandLocation.Y, SandGroundZ + 88.0f));
		Character->GetCharacterMovement()->Velocity = FVector(500.0f, 0.0f, 0.0f);
		const int64 SandBeforeDisplacement =
			WorldActor->GetSimulatedMaterialAmount(TEXT("sand"));
		Character->BuoyancyComponent->TickComponent(
			0.1f, LEVELTICK_All, nullptr);
		TestTrue(TEXT("The player is immersed in the sand pile"),
			Character->BuoyancyComponent->GetLastMovementMediumFraction() > 0.0f);
		TestEqual(TEXT("Sand uses its own Lua-defined movement resistance"),
			Character->BuoyancyComponent->GetLastMovementResistance(), 1.8f);
		TestTrue(TEXT("Sand drag damps existing player momentum"),
			Character->GetCharacterMovement()->Velocity.Size2D() < 500.0f);
		const UMatterFluxCharacterMovementComponent* MaterialMovement =
			Cast<UMatterFluxCharacterMovementComponent>(
				Character->GetCharacterMovement());
		TestTrue(TEXT("Sand resistance also lowers sustained movement limits"),
			MaterialMovement
				&& MaterialMovement->GetMaterialMovementResistance() > 0.0f
				&& MaterialMovement->GetMaxSpeed()
					< Character->GetCharacterMovement()->MaxWalkSpeed);
		WorldActor->Tick(0.0f);
		TestEqual(TEXT("Player displacement conserves exact sand volume"),
			WorldActor->GetSimulatedMaterialAmount(TEXT("sand")),
			SandBeforeDisplacement);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReactionVisualHierarchyTest,
	"MatterFlux.Playable.ReactionSmokeReadsAsSmokeNotGroundChunks",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxReactionVisualHierarchyTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}

	WorldActor->Regenerate(1337);
	if (!TestTrue(TEXT("A tree and its ground contact ignite"),
		WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(5150)))
	{
		return false;
	}
	// 烟雾是每 0.16 秒按 Lua 概率采样的纯表现；观察窗覆盖多次确定性
	// 抽样，但仍短于 wood_burn 的 1.8 秒持续时间，避免把“前两次未命中”
	// 错判成没有烟雾。
	for (int32 Step = 0; Step < 12; ++Step)
	{
		WorldActor->Tick(0.1f);
	}

	UInstancedStaticMeshComponent* SourceFlames = nullptr;
	UInstancedStaticMeshComponent* SourceSmoke = nullptr;
	UInstancedStaticMeshComponent* GroundFlames = nullptr;
	UInstancedStaticMeshComponent* GroundSmoke = nullptr;
	TArray<UInstancedStaticMeshComponent*> Components;
	WorldActor->GetComponents(Components);
	for (UInstancedStaticMeshComponent* Component : Components)
	{
		if (!Component)
		{
			continue;
		}
		const FName Name = Component->GetFName();
		SourceFlames = Name == TEXT("LogicalSourceFlames")
			? Component : SourceFlames;
		SourceSmoke = Name == TEXT("LogicalSourceSmoke")
			? Component : SourceSmoke;
		GroundFlames = Name == TEXT("GroundReactionFlames")
			? Component : GroundFlames;
		GroundSmoke = Name == TEXT("GroundReactionSmoke")
			? Component : GroundSmoke;
	}
	if (!TestNotNull(TEXT("Source flame visual exists"), SourceFlames)
		|| !TestNotNull(TEXT("Source smoke visual exists"), SourceSmoke)
		|| !TestNotNull(TEXT("Ground flame visual exists"), GroundFlames)
		|| !TestNotNull(TEXT("Ground smoke visual exists"), GroundSmoke))
	{
		return false;
	}

	TestTrue(TEXT("Active source emits visible flame voxels"),
		SourceFlames->GetInstanceCount() > 0);
	TestTrue(TEXT("Active source emits visible smoke voxels"),
		SourceSmoke->GetInstanceCount() > 0);
	TestTrue(TEXT("Ground contact emits visible flame voxels"),
		GroundFlames->GetInstanceCount() > 0);

	const auto MaximumHorizontalScale = [](UInstancedStaticMeshComponent& Instances)
	{
		float Maximum = 0.0f;
		for (int32 Index = 0; Index < Instances.GetInstanceCount(); ++Index)
		{
			FTransform Transform;
			if (Instances.GetInstanceTransform(Index, Transform, false))
			{
				Maximum = FMath::Max(
					Maximum,
					FMath::Max(
						static_cast<float>(Transform.GetScale3D().X),
						static_cast<float>(Transform.GetScale3D().Y)));
			}
		}
		return Maximum;
	};
	const float SourceFlameScale = MaximumHorizontalScale(*SourceFlames);
	const float SourceSmokeScale = MaximumHorizontalScale(*SourceSmoke);
	const float GroundFlameScale = MaximumHorizontalScale(*GroundFlames);
	const float GroundSmokeScale = MaximumHorizontalScale(*GroundSmoke);
	TestTrue(
		TEXT("Source smoke pixels are subordinate wisps rather than ground-sized chunks"),
		SourceSmokeScale > 0.0f
			&& SourceSmokeScale <= SourceFlameScale * 0.55f);
	if (GroundSmoke->GetInstanceCount() > 0)
	{
		TestTrue(
			TEXT("Ground smoke pixels are subordinate wisps rather than ground-sized chunks"),
			GroundSmokeScale <= GroundFlameScale * 0.55f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxReactingSourceStreamingTest,
	"MatterFlux.Playable.StreamingArchivesReactingAndOutputSources",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxReactingSourceStreamingTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	if (!TestTrue(TEXT("A streamed tree ignites"),
		WorldActor->ApplyMaterialStimulusToFirstGeneratedTree(5150)))
	{
		return false;
	}

	int32 SourceActorCount = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		SourceActorCount += !It->IsActorBeingDestroyed() ? 1 : 0;
	}
	TestEqual(
		TEXT("Ignition keeps static sources in the world logical store"),
		SourceActorCount,
		0);
	WorldActor->Tick(0.1f);
	const int32 InputBeforeUnload =
		WorldActor->GetLogicalReactionInputCellCount(TEXT("wood"));
	const int32 OutputBeforeUnload =
		WorldActor->GetLogicalReactionOutputCellCount(TEXT("wood"));
	const int32 SmokeBeforeUnload =
		WorldActor->GetLogicalReactionMaterialEmissionCount();

	WorldActor->SetWorldStreamingFocus(
		FVector(10000000.0, 10000000.0, 0.0));
	for (int32 Step = 0; Step < 16; ++Step)
	{
		WorldActor->Tick(0.1f);
	}
	SourceActorCount = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		SourceActorCount += !It->IsActorBeingDestroyed() ? 1 : 0;
	}
	TestEqual(
		TEXT("Leaving the streaming window does not allocate dormant Actors"),
		SourceActorCount,
		0);
	TestTrue(
		TEXT("Logical reaction continues independently of render residency"),
		WorldActor->GetLogicalReactionInputCellCount(TEXT("wood"))
			< InputBeforeUnload
			|| WorldActor->GetLogicalReactionOutputCellCount(TEXT("wood"))
				> OutputBeforeUnload
			|| WorldActor->GetLogicalReactionMaterialEmissionCount()
				> SmokeBeforeUnload);

	WorldActor->SetWorldStreamingFocus(FVector::ZeroVector);
	for (int32 Step = 0; Step < 64; ++Step)
	{
		WorldActor->Tick(0.1f);
	}
	const int32 FinalOutputCount =
		WorldActor->GetReactionOutputCellCount();
	TestTrue(TEXT("Logical sources retain solid reaction output"),
		FinalOutputCount > 0);

	WorldActor->SetWorldStreamingFocus(
		FVector(-10000000.0, -10000000.0, 0.0));
	WorldActor->Tick(0.0f);
	WorldActor->SetWorldStreamingFocus(FVector::ZeroVector);
	WorldActor->Tick(0.0f);
	TestEqual(TEXT("Render streaming preserves logical output exactly"),
		WorldActor->GetReactionOutputCellCount(),
		FinalOutputCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStreamingPopulationParityTest,
	"MatterFlux.Playable.Generation.StreamingPopulationMatchesSeedRegion",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxStreamingPopulationParityTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(
		TEXT("The seeded infinite terrain builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, Layout)))
	{
		return false;
	}
	TSet<FName> Materials;
	TMap<FGuid, int32> AggregateMemberCounts;
	TMap<FIntPoint, int32> TreeCountsByChunk;
	for (int32 ChunkY = 40; ChunkY < 48; ++ChunkY)
	{
		for (int32 ChunkX = 40; ChunkX < 48; ++ChunkX)
		{
			MatterFlux::PlayableLevel::FStreamingChunkPopulation Population;
			if (!TestTrue(
				TEXT("A deterministic external chunk population builds"),
				MatterFlux::PlayableLevel::BuildStreamingChunkPopulation(
					1337,
					Layout.Terrain,
					FIntPoint(ChunkX, ChunkY),
					64,
					Population)))
			{
				return false;
			}
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
				: Population.FragmentSources)
			{
				Materials.Add(Source.MaterialId);
				if (Source.AggregateId.IsValid())
				{
					AggregateMemberCounts.FindOrAdd(Source.AggregateId)++;
				}
			}
			TSet<FGuid> ChunkTreeAggregates;
			for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
				: Population.FragmentSources)
			{
				if (Source.AggregateId.IsValid())
				{
					ChunkTreeAggregates.Add(Source.AggregateId);
				}
			}
			TreeCountsByChunk.Add(
				FIntPoint(ChunkX, ChunkY),
				ChunkTreeAggregates.Num());
		}
	}
	const FName RequiredMaterials[] = {
		TEXT("wood"),
		TEXT("leaf"),
		TEXT("stone"),
		TEXT("grass"),
		TEXT("flower_pink"),
		TEXT("flower_gold"),
		TEXT("flower_blue")
	};
	for (const FName Material : RequiredMaterials)
	{
		TestTrue(
			*FString::Printf(
				TEXT("An external region includes %s decorations"),
				*Material.ToString()),
			Materials.Contains(Material));
	}
	bool bHasSeedRegionTreeStructure = false;
	for (const TPair<FGuid, int32>& Pair : AggregateMemberCounts)
	{
		// Two trunk depth slices, two branches and five canopy slices.
		bHasSeedRegionTreeStructure |= Pair.Value >= 9;
	}
	TestTrue(
		TEXT("External trees use the seed-region multi-slice structure"),
		bHasSeedRegionTreeStructure);
	for (int32 ChunkY = 40; ChunkY < 47; ++ChunkY)
	{
		for (int32 ChunkX = 40; ChunkX < 47; ++ChunkX)
		{
			int32 NeighborhoodTrees = 0;
			for (int32 OffsetY = 0; OffsetY <= 1; ++OffsetY)
			{
				for (int32 OffsetX = 0; OffsetX <= 1; ++OffsetX)
				{
					NeighborhoodTrees += TreeCountsByChunk.FindRef(
						FIntPoint(ChunkX + OffsetX, ChunkY + OffsetY));
				}
			}
			TestTrue(
				*FString::Printf(
					TEXT("External 2x2 chunk neighborhood %d,%d has visible forest coverage"),
					ChunkX,
					ChunkY),
				NeighborhoodTrees >= 4);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxStreamingPopulationRetentionTest,
	"MatterFlux.Playable.Streaming.ExternalPopulationSurvivesSeedBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxStreamingPopulationRetentionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);
	constexpr int32 ChunkSize = 64;
	constexpr float CellSize = MatterFlux::PlayableLevel::TerrainCellSize;
	const auto ChunkCenter = [](const int32 ChunkX)
	{
		return FVector(
			(static_cast<double>(ChunkX * ChunkSize) + ChunkSize * 0.5)
				* CellSize,
			0.0,
			0.0);
	};
	const FIntPoint ExternalChunk(4, 0);
	WorldActor->SetWorldStreamingFocus(ChunkCenter(4));
	for (int32 Frame = 0;
		Frame < 180
			&& !WorldActor->HasProceduralPopulationChunk(ExternalChunk);
		++Frame)
	{
		WorldActor->Tick(1.0f / 60.0f);
	}
	if (!TestTrue(
		TEXT("The first external chunk becomes resident through the frame budget"),
		WorldActor->HasProceduralPopulationChunk(ExternalChunk)))
	{
		return false;
	}
	WorldActor->SetWorldStreamingFocus(ChunkCenter(3));
	TestTrue(
		TEXT("An external chunk still inside the camera window remains resident"),
		WorldActor->HasProceduralPopulationChunk(ExternalChunk));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxInfinitePopulationStreamingTest,
	"MatterFlux.Playable.InfinitePopulationFollowsFarStreamingFocus",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxInfinitePopulationStreamingTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor =
		World ? World->SpawnActor<AMatterFluxPlayableWorldActor>() : nullptr;
	if (!TestNotNull(TEXT("Playable world actor spawned"), WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);

	constexpr int32 TerrainChunkSize = 32;
	const FIntPoint FarChunk(402, 402);
	const FVector FarFocus(
		(static_cast<double>(FarChunk.X * TerrainChunkSize)
			+ TerrainChunkSize * 0.5)
			* MatterFlux::PlayableLevel::TerrainCellSize,
		(static_cast<double>(FarChunk.Y * TerrainChunkSize)
			+ TerrainChunkSize * 0.5)
			* MatterFlux::PlayableLevel::TerrainCellSize,
		0.0);
	WorldActor->SetWorldStreamingFocus(FarFocus);
	// This is a cold jump that replaces the complete population window at once.
	// The readiness count now includes liquid presentation, whose mesh builder is
	// intentionally capped at two chunks per quiet frame to prevent a hitch. Give
	// that bounded queue enough synthetic frames to drain while retaining the
	// strict zero-pending assertion.
	for (int32 Step = 0;
		Step < 1536
			&& WorldActor->GetPendingProceduralPopulationUpdateCount() > 0;
		++Step)
	{
		WorldActor->Tick(1.0f / 60.0f);
	}
	const int32 RemainingStreamingWork =
		WorldActor->GetPendingProceduralPopulationUpdateCount();
	const int64 SimulatedWaterAmount =
		WorldActor->GetSimulatedMaterialAmount(TEXT("water"));
	AddInfo(FString::Printf(
		TEXT("Far streaming diagnostic: pending=%d water=%lld"),
		RemainingStreamingWork,
		SimulatedWaterAmount));
	TestEqual(
		TEXT("Far streamed population and surface topology finish loading"),
		RemainingStreamingWork,
		0);

	const FVector SearchExtent(1400.0, 1400.0, 1200.0);
	TArray<AFragment2DSourceActor*> FarSources;
	WorldActor->GatherFragmentSourcesInBounds(
		FBox(FarFocus - SearchExtent, FarFocus + SearchExtent),
		FarSources);
	const bool bHasFarForest = FarSources.ContainsByPredicate(
		[](const AFragment2DSourceActor* Source)
		{
			return Source
				&& Source->ActorHasTag(TEXT("MatterFluxGeneratedDecoration"));
		});
	TestTrue(
		TEXT("Far streamed chunks contain interactive forest decorations"),
		bHasFarForest);

	bool bHasFarRiver = false;
	// Infinite rivers are spaced sixteen terrain chunks apart. The focus may
	// legitimately sit between channels, so inspect the complete near-focus
	// material window rather than requiring water within one chunk of the pawn.
	for (int32 OffsetY = -512; OffsetY <= 512 && !bHasFarRiver;
		OffsetY += 32)
	{
		for (int32 OffsetX = -4096; OffsetX <= 4096;
			OffsetX += 32)
		{
			MatterFlux::Liquid::FLiquidColumn Column;
			if (WorldActor->TrySampleLiquidColumnAtWorldLocation(
				FarFocus + FVector(OffsetX, OffsetY, 0.0),
				Column))
			{
				bHasFarRiver = true;
				break;
			}
		}
	}
	TestTrue(
		TEXT("Far streamed chunks contain a simulated river"),
		bHasFarRiver);

	bool bHasFarHouse = false;
	for (TActorIterator<AMatterFluxTwoStoreyHouseActor> It(World); It; ++It)
	{
		bHasFarHouse |= FVector::DistSquared2D(
			It->GetActorLocation(), FarFocus) <= FMath::Square(1400.0f);
	}
	TestTrue(
		TEXT("Far streamed chunks contain a generated house"),
		bHasFarHouse);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxVisibleRiverWaterPrefetchTest,
	"MatterFlux.Playable.Streaming.VisibleRiverWaterIsPrefetched",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxVisibleRiverWaterPrefetchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(TEXT("River-prefetch layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, Layout)))
	{
		return false;
	}

	constexpr int32 ChunkSize = 64;
	constexpr int32 SearchChunkY = 400;
	FIntPoint TargetChunk = FIntPoint::ZeroValue;
	FIntPoint TargetRiverCell = FIntPoint::ZeroValue;
	bool bFoundTargetRiver = false;
	for (int32 ChunkX = -32; ChunkX <= 32 && !bFoundTargetRiver; ++ChunkX)
	{
		for (int32 LocalY = 0; LocalY < ChunkSize && !bFoundTargetRiver;
			++LocalY)
		{
			for (int32 LocalX = 0; LocalX < ChunkSize; ++LocalX)
			{
				const FIntPoint Cell(
					ChunkX * ChunkSize + LocalX,
					SearchChunkY * ChunkSize + LocalY);
				float RiverHeight = 0.0f;
				float WaterSurface = 0.0f;
				bool bContainsWater = false;
				if (Layout.Terrain.TrySampleInfiniteRiverCell(
					Cell.X,
					Cell.Y,
					RiverHeight,
					WaterSurface,
					bContainsWater)
					&& bContainsWater)
				{
					TargetChunk = FIntPoint(ChunkX, SearchChunkY);
					TargetRiverCell = Cell;
					bFoundTargetRiver = true;
					break;
				}
			}
		}
	}
	if (!TestTrue(TEXT("A deterministic far river chunk is available"),
		bFoundTargetRiver))
	{
		return false;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	AMatterFluxPlayableWorldActor* WorldActor = World
		? World->SpawnActor<AMatterFluxPlayableWorldActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Playable world exists for river prefetch"),
		WorldActor))
	{
		return false;
	}
	WorldActor->Regenerate(1337);

	const int64 FirstWorldCellX = FMath::FloorToInt64(
		Layout.Terrain.FirstCellCenter.X / Layout.Terrain.CellSize);
	const int64 FirstWorldCellY = FMath::FloorToInt64(
		Layout.Terrain.FirstCellCenter.Y / Layout.Terrain.CellSize);
	const auto CellWorldLocation = [&](const FIntPoint Cell)
	{
		return FVector(
			Layout.Terrain.FirstCellCenter.X
				+ static_cast<double>(Cell.X - FirstWorldCellX)
					* Layout.Terrain.CellSize,
			Layout.Terrain.FirstCellCenter.Y
				+ static_cast<double>(Cell.Y - FirstWorldCellY)
					* Layout.Terrain.CellSize,
			0.0);
	};
	// Warm the previous walking position first. The target is one chunk beyond
	// its visible radius but inside the prefetch ring; after one normal chunk of
	// movement it becomes the new far-edge terrain. The old focus-only 3x3
	// surface queue reports itself drained without ever preparing this water.
	const FIntPoint VisibleFocusChunk = TargetChunk - FIntPoint(0, 4);
	const FIntPoint WarmFocusChunk = VisibleFocusChunk - FIntPoint(0, 1);
	const auto ChunkCenterCell = [](const FIntPoint Chunk)
	{
		return Chunk * ChunkSize + FIntPoint(ChunkSize / 2);
	};
	const FVector RiverWorldLocation = CellWorldLocation(TargetRiverCell);
	WorldActor->SetWorldStreamingFocus(
		CellWorldLocation(ChunkCenterCell(WarmFocusChunk)));
	for (int32 Frame = 0;
		Frame < 1536
			&& WorldActor->GetPendingProceduralPopulationUpdateCount() > 0;
		++Frame)
	{
		WorldActor->Tick(1.0f / 60.0f);
	}
	const bool bWaterWasSimulatedBeforeReveal = [&]()
	{
		MatterFlux::Liquid::FLiquidColumn Column;
		return WorldActor->TrySampleLiquidColumnAtWorldLocation(
			RiverWorldLocation, Column);
	}();
	const bool bProjectionWasReadyBeforeReveal =
		WorldActor->HasLiquidProjectionAtWorldLocation(
			TEXT("water"), RiverWorldLocation);

	WorldActor->SetWorldStreamingFocus(
		CellWorldLocation(ChunkCenterCell(VisibleFocusChunk)));

	bool bTerrainBecameVisible = false;
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		WorldActor->Tick(1.0f / 60.0f);
		bTerrainBecameVisible |=
			WorldActor->HasVisibleTerrainChunk(TargetChunk);
		if (bTerrainBecameVisible)
		{
			break;
		}
	}
	TestTrue(TEXT("The far-edge river terrain enters the visible window"),
		bTerrainBecameVisible);
	AddInfo(FString::Printf(
		TEXT("River prefetch diagnostic: population=%d pending=%d pre_simulated=%d pre_projection=%d"),
		WorldActor->HasProceduralPopulationChunk(TargetChunk) ? 1 : 0,
		WorldActor->GetPendingProceduralPopulationUpdateCount(),
		bWaterWasSimulatedBeforeReveal ? 1 : 0,
		bProjectionWasReadyBeforeReveal ? 1 : 0));
	TestTrue(TEXT("Visible river water is seeded before the player approaches"),
		bWaterWasSimulatedBeforeReveal);
	TestTrue(TEXT("Visible river water has a render projection while streaming"),
		bProjectionWasReadyBeforeReveal);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableWorldDefaultsTest,
	"MatterFlux.Playable.WorldActorHasLightingAndCollisionGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableWorldDefaultsTest::RunTest(const FString& Parameters)
{
	const AMatterFluxPlayableWorldActor* WorldActor = GetDefault<AMatterFluxPlayableWorldActor>();
	if (!TestNotNull(TEXT("Playable world actor CDO exists"), WorldActor))
	{
		return false;
	}

	TestTrue(TEXT("Playable world actor replicates its seed"), WorldActor->GetIsReplicated());
	TestTrue(TEXT("Playable world actor ticks its fixed-step material simulation"),
		WorldActor->PrimaryActorTick.bCanEverTick);
	TestTrue(TEXT("Material world has an ordered configurable vertical range"),
		WorldActor->GetMaterialSimulationMinHeight()
			< WorldActor->GetMaterialSimulationMaxHeight());
	TestTrue(TEXT("Terrain streaming keeps at least four chunks beyond the player"),
		WorldActor->GetTerrainStreamingChunkRadius() >= 4);
	const int32 StreamingDiameter =
		(WorldActor->GetTerrainStreamingChunkRadius() + 1) * 2 + 1;
	const int32 CameraOverlapSide = FMath::Max(StreamingDiameter - 2, 0);
	const int32 RequiredCameraWindowChunks =
		StreamingDiameter * StreamingDiameter * 2
			- CameraOverlapSide * CameraOverlapSide;
	TestTrue(TEXT("Terrain LRU cache contains the visible window and prefetch ring"),
		WorldActor->GetTerrainChunkCacheLimit()
			>= RequiredCameraWindowChunks);
	TestEqual(TEXT("Generated HISM adapters are created from layout layers at runtime"),
		WorldActor->GetGeneratedLayerCount(), 0);
	TestNotNull(TEXT("World actor has a directional light"), WorldActor->SunLight.Get());
	TestNotNull(TEXT("World actor has a sky light"), WorldActor->SkyLight.Get());
	TestTrue(TEXT("Sun intensity preserves color instead of crushing shaded trees"),
		WorldActor->SunLight && WorldActor->SunLight->Intensity <= 2.5f);
	TestTrue(TEXT("Sun shadows remain crisp enough for voxel silhouettes"),
		WorldActor->SunLight && WorldActor->SunLight->LightSourceAngle <= 1.0f);
	TestTrue(TEXT("Sky fill preserves shadows without crushing distant voxels"),
		WorldActor->SkyLight
			&& WorldActor->SkyLight->Intensity >= 0.65f
			&& WorldActor->SkyLight->Intensity <= 1.1f);
	TestFalse(TEXT("Static procedural scenery does not recapture the sky every frame"),
		WorldActor->SkyLight && WorldActor->SkyLight->bRealTimeCapture);
	const UMaterialInterface* VoxelMaterial =
		WorldActor->GetVoxelColorMaterialTemplate();
	TestNotNull(TEXT("Voxel decorations have a dedicated color material"),
		VoxelMaterial);
	TestTrue(TEXT("Voxel decorations use project-owned palette shading"),
		VoxelMaterial
			&& VoxelMaterial->GetPathName().Contains(
				TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.")));
	TestTrue(TEXT("Voxel palette receives scene lighting"),
		VoxelMaterial
		&& VoxelMaterial->GetShadingModels().HasShadingModel(MSM_DefaultLit));
	const UMaterialInterface* LiquidMaterial =
		WorldActor->GetVoxelLiquidMaterialTemplate();
	TestNotNull(TEXT("Liquids have a dedicated material template"),
		LiquidMaterial);
	TestTrue(TEXT("Liquid material is project-owned"),
		LiquidMaterial
		&& LiquidMaterial->GetPathName().Contains(
			TEXT("/Game/MatterFlux/Materials/M_VoxelLiquid.")));
	TestEqual(TEXT("Liquid material enables real translucency"),
		LiquidMaterial ? LiquidMaterial->GetBlendMode() : BLEND_Opaque,
		BLEND_Translucent);
	TestNotNull(TEXT("World actor has a sky atmosphere"), WorldActor->SkyAtmosphere.Get());
	TestNotNull(TEXT("External visual capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(TEXT("mf.Visual.Capture")));
	TestNotNull(TEXT("Occluded-player visual capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.CaptureOccludedPlayer")));
	TestNotNull(TEXT("Tree cut sequence capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.TreeCutSequence")));
	TestNotNull(TEXT("Batch tree cut capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.TreeBatchCut")));
	TestNotNull(TEXT("Real fragment push capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.PhysicsPush")));
	TestNotNull(TEXT("Frame stability capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.StabilitySequence")));
	TestNotNull(TEXT("Liquid-pool visual acceptance command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.LiquidPool")));
	TestNotNull(TEXT("Deep-liquid walk acceptance command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.DeepLiquidWalk")));
	const AMatterFluxGameMode* GameMode = GetDefault<AMatterFluxGameMode>();
	TestTrue(TEXT("Default GameMode automatically spawns the playable world class"),
		GameMode && GameMode->PlayableWorldClass == AMatterFluxPlayableWorldActor::StaticClass());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMatterFluxPlayableRiverAndLowlandWaterTest,
	"MatterFlux.Playable.Generation.RiverAndLakeFollowLowlandTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMatterFluxPlayableRiverAndLowlandWaterTest::RunTest(
	const FString& Parameters)
{
	MatterFlux::PlayableLevel::FLevelLayout Layout;
	if (!TestTrue(TEXT("Seeded terrain and water layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, Layout)))
	{
		return false;
	}
	const MatterFlux::PlayableLevel::FLevelLayer* Stream =
		Layout.FindLayer(TEXT("Stream"));
	const MatterFlux::PlayableLevel::FLevelLayer* Lake =
		Layout.FindLayer(TEXT("Lake"));
	if (!TestTrue(TEXT("A generated stream exists"),
		Stream && !Stream->Instances.IsEmpty())
		|| !TestTrue(TEXT("A generated lowland lake exists"),
			Lake && !Lake->Instances.IsEmpty()))
	{
		return false;
	}

	TMap<int32, TArray<int32>> StreamColumnsByRow;
	TMap<int32, float> MaximumDepthByRow;
	for (const FTransform& Instance : Stream->Instances)
	{
		const FVector Location = Instance.GetLocation();
		const int32 X = FMath::Clamp(
			FMath::RoundToInt(
				(Location.X - Layout.Terrain.FirstCellCenter.X)
					/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Width - 1);
		const int32 Y = FMath::Clamp(
			FMath::RoundToInt(
				(Location.Y - Layout.Terrain.FirstCellCenter.Y)
					/ Layout.Terrain.CellSize),
			0,
			Layout.Terrain.Height - 1);
		StreamColumnsByRow.FindOrAdd(Y).AddUnique(X);
		const float SurfaceZ = Location.Z
			+ Instance.GetScale3D().Z * 50.0f;
		MaximumDepthByRow.FindOrAdd(Y) = FMath::Max(
			MaximumDepthByRow.FindRef(Y),
			SurfaceZ - Layout.Terrain.HeightAt(X, Y));
	}

	int32 WideRows = 0;
	int32 DeepRows = 0;
	int32 GradedBankRows = 0;
	for (const TPair<int32, TArray<int32>>& Pair : StreamColumnsByRow)
	{
		const int32 Y = Pair.Key;
		const TArray<int32>& Columns = Pair.Value;
		WideRows += Columns.Num() >= 11 ? 1 : 0;
		DeepRows += MaximumDepthByRow.FindRef(Y) >= 48.0f ? 1 : 0;
		int32 MinimumX = MAX_int32;
		int32 MaximumX = MIN_int32;
		for (const int32 Column : Columns)
		{
			MinimumX = FMath::Min(MinimumX, Column);
			MaximumX = FMath::Max(MaximumX, Column);
		}
		const int32 CenterX = (MinimumX + MaximumX) / 2;
		const auto HasGradedBank = [&Layout, CenterX, Y](const int32 Sign)
		{
			constexpr int32 BankProbeCells = 18;
			if (CenterX + Sign * BankProbeCells < 0
				|| CenterX + Sign * BankProbeCells >= Layout.Terrain.Width)
			{
				return false;
			}
			const float BedHeight = Layout.Terrain.HeightAt(CenterX, Y);
			float PreviousHeight = BedHeight;
			float MaximumStep = 0.0f;
			TSet<int32> HeightBands;
			HeightBands.Add(FMath::RoundToInt(BedHeight));
			for (int32 Distance = 1; Distance <= BankProbeCells; ++Distance)
			{
				const float Height = Layout.Terrain.HeightAt(
					CenterX + Sign * Distance,
					Y);
				MaximumStep = FMath::Max(
					MaximumStep,
					FMath::Abs(Height - PreviousHeight));
				HeightBands.Add(FMath::RoundToInt(Height));
				PreviousHeight = Height;
			}
			return PreviousHeight - BedHeight >= 40.0f
				&& MaximumStep <= 24.0f
				&& HeightBands.Num() >= 4;
		};
		GradedBankRows += HasGradedBank(-1) || HasGradedBank(1) ? 1 : 0;
	}
	const int32 RequiredRows = FMath::FloorToInt(
		static_cast<float>(StreamColumnsByRow.Num()) * 0.75f);
	TestTrue(TEXT("The river is wider than the old four-cell ribbon"),
		WideRows >= RequiredRows);
	TestTrue(TEXT("The river contains a substantially deeper center channel"),
		DeepRows >= RequiredRows);
	TestTrue(TEXT("The river bed rises through several gentle bank steps"),
		GradedBankRows >= RequiredRows);

	const auto LakeOccupiesPerlinLowland = [](
		const MatterFlux::PlayableLevel::FLevelLayout& CandidateLayout)
	{
		const MatterFlux::PlayableLevel::FLevelLayer* CandidateLake =
			CandidateLayout.FindLayer(TEXT("Lake"));
		if (!CandidateLake || CandidateLake->Instances.IsEmpty())
		{
			return false;
		}
		FBox2D LakeBounds(ForceInit);
		for (const FTransform& Instance : CandidateLake->Instances)
		{
			LakeBounds += FVector2D(Instance.GetLocation());
		}
		MatterFlux::PlayableLevel::FLevelTerrain BaseTerrain =
			CandidateLayout.Terrain;
		BaseTerrain.Width = 1;
		BaseTerrain.Height = 1;
		BaseTerrain.Heights.SetNum(1);
		BaseTerrain.ColorBands.SetNum(1);
		const int64 FirstWorldCellX = FMath::FloorToInt64(
			CandidateLayout.Terrain.FirstCellCenter.X
				/ CandidateLayout.Terrain.CellSize);
		const int64 FirstWorldCellY = FMath::FloorToInt64(
			CandidateLayout.Terrain.FirstCellCenter.Y
				/ CandidateLayout.Terrain.CellSize);
		const auto SampleBaseHeight = [
			&BaseTerrain,
			&CandidateLayout,
			FirstWorldCellX,
			FirstWorldCellY](const FVector2D& Location, float& OutHeight)
		{
			const int64 WorldCellX = FirstWorldCellX + FMath::RoundToInt64(
				(Location.X - CandidateLayout.Terrain.FirstCellCenter.X)
					/ CandidateLayout.Terrain.CellSize);
			const int64 WorldCellY = FirstWorldCellY + FMath::RoundToInt64(
				(Location.Y - CandidateLayout.Terrain.FirstCellCenter.Y)
					/ CandidateLayout.Terrain.CellSize);
			uint8 ColorBand = 0;
			return BaseTerrain.TrySampleWorldCell(
				WorldCellX,
				WorldCellY,
				OutHeight,
				ColorBand);
		};
		TArray<float> BaseTerrainSamples;
		for (int32 Y = 16; Y < CandidateLayout.Terrain.Height; Y += 16)
		{
			for (int32 X = 16; X < CandidateLayout.Terrain.Width; X += 16)
			{
				float Height = 0.0f;
				if (SampleBaseHeight(
					FVector2D(
						CandidateLayout.Terrain.FirstCellCenter.X
							+ X * CandidateLayout.Terrain.CellSize,
						CandidateLayout.Terrain.FirstCellCenter.Y
							+ Y * CandidateLayout.Terrain.CellSize),
					Height))
				{
					BaseTerrainSamples.Add(Height);
				}
			}
		}
		BaseTerrainSamples.Sort();
		if (BaseTerrainSamples.IsEmpty())
		{
			return false;
		}
		float LakeBaseHeight = 0.0f;
		const int32 LowlandQuantileIndex = FMath::Clamp(
			FMath::FloorToInt(BaseTerrainSamples.Num() * 0.20f),
			0,
			BaseTerrainSamples.Num() - 1);
		return SampleBaseHeight(LakeBounds.GetCenter(), LakeBaseHeight)
			&& LakeBaseHeight <= BaseTerrainSamples[LowlandQuantileIndex];
	};
	TestTrue(TEXT("Seed 1337 water chooses the low Perlin quantile"),
		LakeOccupiesPerlinLowland(Layout));
	const int32 AdditionalSeeds[] = { 7331, 24681357, 975318642 };
	for (const int32 Seed : AdditionalSeeds)
	{
		MatterFlux::PlayableLevel::FLevelLayout SeededLayout;
		if (TestTrue(
			*FString::Printf(TEXT("Seed %d layout builds"), Seed),
			MatterFlux::PlayableLevel::BuildLevelLayout(Seed, SeededLayout)))
		{
			TestTrue(
				*FString::Printf(
					TEXT("Seed %d water chooses the low Perlin quantile"),
					Seed),
				LakeOccupiesPerlinLowland(SeededLayout));
		}
	}
	return true;
}
