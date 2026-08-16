#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentGeometry.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "GAS/GA_CastWand.h"
#include "Game/MatterFluxCharacter.h"
#include "Game/MatterFluxGameMode.h"
#include "Game/MatterFluxPlayableLevel.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxTerrainMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"
#include "IMatterFluxScriptRuntime.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "MaterialShared.h"
#include "Misc/AutomationTest.h"

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
			|| Left.Mask.SolidMask != Right.Mask.SolidMask)
		{
			return false;
		}
	}
	return true;
}
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
		TEXT("Seeded level layout builds"),
		MatterFlux::PlayableLevel::BuildLevelLayout(1337, Layout)))
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

	MatterFlux::TerrainMesh::FChunk Chunk;
	if (!TestTrue(
		TEXT("A terrain chunk builds from the heightfield"),
		MatterFlux::TerrainMesh::BuildChunk(
			Terrain,
			FIntPoint(-8, -6),
			32,
			Chunk)))
	{
		return false;
	}
	TestTrue(TEXT("Merged chunk geometry is valid"), Chunk.IsValid());
	TestEqual(TEXT("Chunk keeps three material bands"), Chunk.Sections.Num(), 3);
	TestEqual(TEXT("Interior chunk covers 32 by 32 pixels"),
		Chunk.CellBounds.Area(), 32 * 32);
	double TopSurfaceArea = 0.0;
	int32 TopTriangleCount = 0;
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
	TestFalse(
		TEXT("Character movement does not apply its extra high-force physics push to detached terrain"),
		Character->GetCharacterMovement()->bEnablePhysicsInteraction);
	TestFalse(TEXT("The character does not inherit controller yaw"),
		Character->bUseControllerRotationYaw);
	TestTrue(TEXT("The camera uses a useful three-quarter distance"),
		Character->CameraBoom && Character->CameraBoom->TargetArmLength >= 800.0f);
	const FRotator CameraRotation = Character->CameraBoom->GetRelativeRotation();
	TestTrue(TEXT("The 2.5D camera uses an isometric-style yaw"),
		FMath::Abs(CameraRotation.Yaw) >= 35.0f && FMath::Abs(CameraRotation.Yaw) <= 55.0f);
	TestTrue(TEXT("The 2.5D camera looks down onto the terrain"),
		CameraRotation.Pitch <= -40.0f);
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
	const MatterFlux::PlayableLevel::FLevelLayer* Backdrop =
		A.FindLayer(TEXT("Backdrop"));
	if (!TestNotNull(TEXT("Layout has a soil layer"), Soil)
		|| !TestTrue(TEXT("Layout has a fine terrain heightfield"),
			A.Terrain.IsValid())
		|| !TestNotNull(TEXT("Layout has a stream layer"), Stream)
		|| !TestNotNull(TEXT("Layout has a visual backdrop"), Backdrop))
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
		&& !Backdrop->bEnableCollision);
	TestTrue(TEXT("The visual backdrop extends beyond both playable dimensions"),
		Backdrop->Instances.Num() == 1
		&& Backdrop->Instances[0].GetScale3D().X
			> Soil->Instances[0].GetScale3D().X
		&& Backdrop->Instances[0].GetScale3D().Y
			> Soil->Instances[0].GetScale3D().Y
		&& Backdrop->Instances[0].GetLocation().Z
			< Soil->Instances[0].GetLocation().Z);
	TestTrue(TEXT("Stream retains hard-face voxel lighting"),
		Stream->RenderMode
			== MatterFlux::PlayableLevel::ELevelLayerRenderMode::VoxelLit);
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
	int32 CollisionEnabledFragmentSourceCount = 0;
	for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
		: A.FragmentSources)
	{
		FragmentSourceCounts.FindOrAdd(Source.Name)++;
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
			|| Source.Name == TEXT("TreeCanopyBack")
			|| Source.Name == TEXT("TreeCanopy")
			|| Source.Name == TEXT("TreeCanopyFront");
		TestEqual(
			TEXT("Only tree parts declare aggregate membership"),
			Source.AggregateId.IsValid(),
			bTreeSource);
		if (bTreeSource)
		{
			TreeAggregateMemberCounts.FindOrAdd(Source.AggregateId)++;
			TreeAggregateRootCounts.FindOrAdd(Source.AggregateId) +=
				Source.bAggregateRoot ? 1 : 0;
			TestEqual(
				TEXT("Only the trunk is an aggregate root"),
				Source.bAggregateRoot,
				Source.Name == TEXT("TreeTrunk"));
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
	const int32 TreeCount = FragmentSourceCounts.FindRef(TEXT("TreeTrunk"));
	TestEqual(TEXT("Every tree contributes exactly one collidable source"),
		CollisionEnabledFragmentSourceCount,
		TreeCount);
	TestTrue(TEXT("Tree trunks and layered tree canopies are MatterFlux mask sources"),
		TreeCount >= 10
		&& FragmentSourceCounts.FindRef(TEXT("TreeCanopyBack")) == TreeCount
		&& FragmentSourceCounts.FindRef(TEXT("TreeCanopy")) == TreeCount
		&& FragmentSourceCounts.FindRef(TEXT("TreeCanopyFront")) == TreeCount);
	TestEqual(
		TEXT("Every generated tree has one deterministic aggregate"),
		TreeAggregateMemberCounts.Num(),
		TreeCount);
	for (const TPair<FGuid, int32>& Pair : TreeAggregateMemberCounts)
	{
		TestEqual(
			TEXT("Tree aggregate contains trunk and three canopy layers"),
			Pair.Value,
			4);
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
	TestTrue(TEXT("Rocks use independently seeded MatterFlux mask sources"),
		FragmentSourceCounts.FindRef(TEXT("RockCluster")) >= 12);

	const MatterFlux::PlayableLevel::FLevelFragmentSource* Trunk =
		A.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeTrunk");
			});
	const MatterFlux::PlayableLevel::FLevelFragmentSource* Canopy =
		A.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeCanopy");
			});
	const MatterFlux::PlayableLevel::FLevelFragmentSource* CanopyBack =
		A.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeCanopyBack");
			});
	const MatterFlux::PlayableLevel::FLevelFragmentSource* CanopyFront =
		A.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeCanopyFront");
			});
	TestTrue(TEXT("Tree trunk material remains visibly brown"),
		Trunk && Trunk->Color.R >= 0.30f
		&& Trunk->Color.R >= Trunk->Color.G * 1.8f
		&& Trunk->Color.G >= Trunk->Color.B * 2.5f);
	TestTrue(TEXT("Tree canopy material remains visibly green"),
		Canopy && Canopy->Color.G >= 0.35f
		&& Canopy->Color.G >= Canopy->Color.R * 3.0f
		&& Canopy->Color.G >= Canopy->Color.B * 3.0f);
	if (Trunk && CanopyBack && Canopy && CanopyFront)
	{
		const float BackDepth = Trunk->Transform.InverseTransformPosition(
			CanopyBack->Transform.GetLocation()).Y;
		const float MainDepth = Trunk->Transform.InverseTransformPosition(
			Canopy->Transform.GetLocation()).Y;
		const float FrontDepth = Trunk->Transform.InverseTransformPosition(
			CanopyFront->Transform.GetLocation()).Y;
		TestTrue(TEXT("Tree layers have distinct depths and cannot Z-fight"),
			BackDepth > 2.0f
			&& MainDepth > BackDepth + 2.0f
			&& FrontDepth > MainDepth + 2.0f);
		TestTrue(TEXT("Tree crown uses nested voxel silhouettes"),
			CanopyBack->Mask.Width > Canopy->Mask.Width
			&& Canopy->Mask.Width > CanopyFront->Mask.Width);
		TestTrue(TEXT("Tree crown depth layers use separate green shades"),
			CanopyBack->Color != Canopy->Color
			&& Canopy->Color != CanopyFront->Color);
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
	bool bFoundBatchedSourceCollision = false;
	for (const UProceduralMeshComponent* Component
		: TerrainComponents)
	{
		if (Component
			&& Component->IsVisible()
			&& Component->GetNumSections() > 0)
		{
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
				TEXT("Merged terrain has exact static-world collision"),
				Component->GetCollisionEnabled()
					== ECollisionEnabled::QueryAndPhysics
				&& Component->bUseComplexAsSimpleCollision);
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
	TestTrue(TEXT("Playable world seeds visible powder cells"),
		WorldActor->GetSimulatedMaterialCount(TEXT("sand")) > 0);
	TestTrue(TEXT("Playable world seeds visible gas cells"),
		WorldActor->GetSimulatedMaterialCount(TEXT("steam")) > 0);
	TestTrue(TEXT("Playable world seeds a reactive lava pool"),
		WorldActor->GetSimulatedMaterialCount(TEXT("lava")) > 0);
	TestTrue(TEXT("Each active material has a voxel visualization layer"),
		WorldActor->GetGeneratedMaterialLayerCount() >= 5);
	WorldActor->Tick(0.25f);
	TestTrue(TEXT("Authority advances material simulation at a fixed step"),
		WorldActor->GetMaterialSimulationStep() > 0);

	const MatterFlux::PlayableLevel::FLevelFragmentSource* TrunkLayout =
		Layout.FragmentSources.FindByPredicate(
			[](const MatterFlux::PlayableLevel::FLevelFragmentSource& Source)
			{
				return Source.Name == TEXT("TreeTrunk");
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
			&& SourceLayout->Name == TEXT("TreeTrunk"))
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
	bool bDamagedSourceActorRemains = false;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		bDamagedSourceActorRemains |=
			It->SourceId == DamagedSourceId
			&& !It->IsActorBeingDestroyed();
	}
	TestFalse(
		TEXT("A supported static stump does not remain a per-object Actor"),
		bDamagedSourceActorRemains);

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
		TestEqual(
			TEXT("All three generated canopy layers become logical carrier members"),
			TreeCarrier->GetAggregateMemberCount(),
			3);
		for (const MatterFlux::PlayableLevel::FLevelFragmentSource& Source
			: Layout.FragmentSources)
		{
			if (Source.AggregateId == TrunkLayout->AggregateId
				&& !Source.bAggregateRoot)
			{
				TestTrue(
					TEXT("Every canopy SourceId remains independently addressable"),
					TreeCarrier->ContainsAggregateSource(Source.SourceId));
			}
		}
		TestTrue(
			TEXT("Carrier mesh keeps trunk and canopy material sections"),
			TreeCarrier->MeshComponent->GetNumSections() > 2);
		int32 CarriedSolidCells = 0;
		for (const FFragmentAggregateSourceState& Member
			: TreeCarrier->AggregateSources)
		{
			for (const uint8 Cell : Member.SourceMask.SolidMask)
			{
				CarriedSolidCells += Cell != 0 ? 1 : 0;
			}
		}
		TestTrue(
			TEXT("Carrier physics mass includes its visual canopy members"),
			TreeCarrier->SpawnPayload.Mass
				> static_cast<float>(CarriedSolidCells) * 0.05f);
	}
	int32 DetachedCanopyActorCount = 0;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		DetachedCanopyActorCount +=
			!It->IsActorBeingDestroyed()
			&& It->AggregateId == TrunkLayout->AggregateId
			&& !It->bAggregateRoot
				? 1
				: 0;
	}
	TestEqual(
		TEXT("Generated canopy layers no longer allocate detached Source Actors"),
		DetachedCanopyActorCount,
		0);
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
			TestTrue(
				TEXT("Every carried SourceId has a persistent static-world tombstone"),
				AggregateSaveState.RemovedFragmentSourceIds.Contains(
					Member.SourceId));
			const FMatterFluxSavedFragmentSourceState* SavedTombstone =
				AggregateSaveState.FragmentSources.FindByPredicate(
					[&Member](
						const FMatterFluxSavedFragmentSourceState& Saved)
					{
						return Saved.SourceId == Member.SourceId;
					});
			TestNotNull(
				TEXT("Every carried SourceId saves a replicated mask tombstone"),
				SavedTombstone);
			if (SavedTombstone)
			{
				TestFalse(
					TEXT("Saved tombstone contains no pristine solid cells"),
					SavedTombstone->RuntimeMask.Contains(1));
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
	FMatterFluxCombustionVisualHierarchyTest,
	"MatterFlux.Playable.CombustionSmokeReadsAsSmokeNotGroundChunks",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxCombustionVisualHierarchyTest::RunTest(
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
		WorldActor->IgniteFirstGeneratedTree(5150)))
	{
		return false;
	}
	for (int32 Step = 0; Step < 4; ++Step)
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
		GroundFlames = Name == TEXT("GroundCombustionFlames")
			? Component : GroundFlames;
		GroundSmoke = Name == TEXT("GroundCombustionSmoke")
			? Component : GroundSmoke;
	}
	if (!TestNotNull(TEXT("Source flame visual exists"), SourceFlames)
		|| !TestNotNull(TEXT("Source smoke visual exists"), SourceSmoke)
		|| !TestNotNull(TEXT("Ground flame visual exists"), GroundFlames)
		|| !TestNotNull(TEXT("Ground smoke visual exists"), GroundSmoke))
	{
		return false;
	}

	TestTrue(TEXT("Burning source emits visible flame voxels"),
		SourceFlames->GetInstanceCount() > 0);
	TestTrue(TEXT("Burning source emits visible smoke voxels"),
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
	FMatterFluxCombustingSourceStreamingTest,
	"MatterFlux.Playable.StreamingArchivesCombustingAndResidueSources",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ProductFilter)

bool FMatterFluxCombustingSourceStreamingTest::RunTest(
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
		WorldActor->IgniteFirstGeneratedTree(5150)))
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
	const int32 FuelBeforeUnload =
		WorldActor->GetLogicalCombustionFuelCellCount(TEXT("wood"));
	const int32 ResidueBeforeUnload =
		WorldActor->GetLogicalCombustionResidueCellCount(TEXT("wood"));
	const int32 SmokeBeforeUnload =
		WorldActor->GetLogicalCombustionSmokeEmissionCount();

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
		TEXT("Logical combustion continues independently of render residency"),
		WorldActor->GetLogicalCombustionFuelCellCount(TEXT("wood"))
			< FuelBeforeUnload
			|| WorldActor->GetLogicalCombustionResidueCellCount(TEXT("wood"))
				> ResidueBeforeUnload
			|| WorldActor->GetLogicalCombustionSmokeEmissionCount()
				> SmokeBeforeUnload);

	WorldActor->SetWorldStreamingFocus(FVector::ZeroVector);
	for (int32 Step = 0; Step < 64; ++Step)
	{
		WorldActor->Tick(0.1f);
	}
	const int32 FinalResidueCount =
		WorldActor->GetCombustionResidueCellCount();
	TestTrue(TEXT("Logical sources retain solid combustion residue"),
		FinalResidueCount > 0);

	WorldActor->SetWorldStreamingFocus(
		FVector(-10000000.0, -10000000.0, 0.0));
	WorldActor->Tick(0.0f);
	WorldActor->SetWorldStreamingFocus(FVector::ZeroVector);
	WorldActor->Tick(0.0f);
	TestEqual(TEXT("Render streaming preserves logical residue exactly"),
		WorldActor->GetCombustionResidueCellCount(),
		FinalResidueCount);
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
	TestNotNull(TEXT("World actor has a sky atmosphere"), WorldActor->SkyAtmosphere.Get());
	TestNotNull(TEXT("External visual capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(TEXT("mf.Visual.Capture")));
	TestNotNull(TEXT("Occluded-player visual capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.CaptureOccludedPlayer")));
	TestNotNull(TEXT("Tree cut sequence capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.TreeCutSequence")));
	TestNotNull(TEXT("Frame stability capture command is registered"),
		IConsoleManager::Get().FindConsoleObject(
			TEXT("mf.Visual.StabilitySequence")));
	const AMatterFluxGameMode* GameMode = GetDefault<AMatterFluxGameMode>();
	TestTrue(TEXT("Default GameMode automatically spawns the playable world class"),
		GameMode && GameMode->PlayableWorldClass == AMatterFluxPlayableWorldActor::StaticClass());
	return true;
}
