#include "Game/MatterFluxTwoStoreyHouseActor.h"

#include "Camera/CameraComponent.h"
#include "IMatterFluxScriptRuntime.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Fragment/FragmentSimulationSubsystem.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Crc.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/MatterFluxGhostFade.h"
#include "Rendering/MatterFluxVoxelMaterialStyle.h"
#include "Rendering/MatterFluxWholeObjectGeometry.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float FloorThickness = 34.0f;
	constexpr float GroundFloorTop = 34.0f;
	constexpr float UpperFloorTop = GroundFloorTop
		+ AMatterFluxTwoStoreyHouseActor::StoreyHeight;
	constexpr float WallThickness = 34.0f;
	constexpr float WallHeight = 326.0f;
	constexpr float StructureCellSize = 17.0f;
	constexpr float InteriorMargin = 46.0f;
	constexpr float FloorGhostOpacity = 0.16f;
	// Entering a floor is precise, but leaving uses a Schmitt-trigger envelope.
	// CharacterMovement may briefly depenetrate a capsule beyond the strict
	// interior bounds at walls, stair lips and low-FPS floor corrections.
	constexpr int32 RoofStepCount = 12;
	constexpr float RoofRun = 36.0f;
	constexpr float RoofRise = 18.0f;
	constexpr float RoofShellThickness = 36.0f;
	constexpr float RoofRidgeLength =
		AMatterFluxTwoStoreyHouseActor::HalfSizeX * 2.0f + 76.0f;
	constexpr float RoofOuterWidth =
		AMatterFluxTwoStoreyHouseActor::HalfSizeY * 2.0f + 76.0f;
	constexpr float RoofTotalHeight =
		RoofShellThickness + static_cast<float>(RoofStepCount) * RoofRise;
	constexpr float RoofCellSize = 18.0f;

	int32 FindRoofSurfaceStep(const float LocalCrossSlope)
	{
		for (int32 Step = RoofStepCount; Step >= 0; --Step)
		{
			const float StepWidth = RoofOuterWidth
				- static_cast<float>(Step) * RoofRun * 2.0f;
			if (FMath::Abs(LocalCrossSlope) <= StepWidth * 0.5f)
			{
				return Step;
			}
		}
		return INDEX_NONE;
	}

	FVector ScaleForBox(const FVector& Size)
	{
		return FVector(
			Size.X / 100.0f,
			Size.Y / 100.0f,
			Size.Z / 100.0f);
	}

	FTransform SnapMaskLayerToGrid(
		const FTransform& LocalTransform,
		const FFragmentSourceMask& Mask)
	{
		FTransform Snapped = LocalTransform;
		if (!Mask.HasValidLayout())
		{
			return Snapped;
		}
		const int32 DepthCellCount = FMath::Clamp(
			FMath::RoundToInt(
				FMath::Abs(LocalTransform.GetScale3D().Y)),
			1,
			256);
		const FVector FirstCellCenter(
			(0.5f - static_cast<float>(Mask.Width) * 0.5f) * Mask.CellSize,
			(0.5f - static_cast<float>(DepthCellCount) * 0.5f)
				* Mask.CellSize,
			(0.5f - static_cast<float>(Mask.Height) * 0.5f) * Mask.CellSize);
		// Source Actor 的 Y 缩放表示总厚度；格点校准必须使用展开后的
		// 未缩放体素中心，否则偶数厚度会因墙朝向不同落到不同半格。
		const FTransform Unscaled(
			Snapped.GetRotation(),
			Snapped.GetLocation(),
			FVector::OneVector);
		const FVector FirstInHouse =
			Unscaled.TransformPosition(FirstCellCenter);
		const FVector SnappedFirst(
			FMath::RoundToFloat(FirstInHouse.X / Mask.CellSize) * Mask.CellSize,
			FMath::RoundToFloat(FirstInHouse.Y / Mask.CellSize) * Mask.CellSize,
			FMath::RoundToFloat(FirstInHouse.Z / Mask.CellSize) * Mask.CellSize);
		Snapped.AddToTranslation(SnappedFirst - FirstInHouse);
		return Snapped;
	}

	FGuid MakeHouseSourceId(
		const FVector& HouseLocation,
		const FName GroupName,
		const int32 PieceIndex)
	{
		return FGuid::NewDeterministicGuid(
			FString::Printf(
				TEXT("MatterFluxHouse|%.0f|%.0f|%.0f|Group=%s|Piece=%d"),
				HouseLocation.X,
				HouseLocation.Y,
				HouseLocation.Z,
				*GroupName.ToString(),
				PieceIndex),
			static_cast<uint64>(HashCombineFast(
				GetTypeHash(HouseLocation.X),
				HashCombineFast(
					GetTypeHash(HouseLocation.Y),
					GetTypeHash(HouseLocation.Z)))));
	}
}

AMatterFluxTwoStoreyHouseActor::AMatterFluxTwoStoreyHouseActor()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(true);
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.05f;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	// Streamed houses are pooled and teleported between chunk locations. A
	// static root rejects SetActorTransform in game worlds, leaving the visible
	// whole-house projection behind while its logical cuttable Sources move.
	SceneRoot->SetMobility(EComponentMobility::Movable);
	SetRootComponent(SceneRoot);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SolidFinder(
		TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GhostFinder(
		TEXT("/Game/MatterFlux/Materials/M_VoxelGas.M_VoxelGas"));
	CubeMesh = CubeFinder.Object;
	SolidMaterialTemplate = SolidFinder.Object;
	GhostMaterialTemplate = GhostFinder.Object;

	StairRampCollision = CreateDefaultSubobject<UBoxComponent>(
		TEXT("StairRampCollision"));
	StairRampCollision->SetupAttachment(SceneRoot);
	StairRampCollision->SetMobility(EComponentMobility::Movable);
	StairRampCollision->SetCollisionObjectType(ECC_WorldStatic);
	StairRampCollision->SetCollisionEnabled(
		ECollisionEnabled::QueryAndPhysics);
	StairRampCollision->SetCollisionResponseToAllChannels(ECR_Block);
	StairRampCollision->SetHiddenInGame(true);
	StairRampCollision->SetCanEverAffectNavigation(true);

	ExteriorGroundCollision = CreateDefaultSubobject<UBoxComponent>(
		TEXT("ExteriorGroundCollision"));
	ExteriorGroundCollision->SetupAttachment(SceneRoot);
	ExteriorGroundCollision->SetMobility(EComponentMobility::Movable);
	ExteriorGroundCollision->SetBoxExtent(FVector(760.0f, 760.0f, 8.0f));
	// Generated houses are placed at FoundationTop + 4. The collision pad's
	// top therefore sits exactly on the flattened terrain at local Z=-4.
	ExteriorGroundCollision->SetRelativeLocation(FVector(0.0f, 0.0f, -12.0f));
	ExteriorGroundCollision->SetCollisionObjectType(ECC_WorldStatic);
	ExteriorGroundCollision->SetCollisionEnabled(
		ECollisionEnabled::QueryAndPhysics);
	ExteriorGroundCollision->SetCollisionResponseToAllChannels(ECR_Block);
	ExteriorGroundCollision->SetHiddenInGame(true);
	ExteriorGroundCollision->SetCanEverAffectNavigation(true);

	CuttableWholeObjectMesh = CreateDefaultSubobject<UProceduralMeshComponent>(
		TEXT("CuttableWholeObjectMesh"));
	CuttableWholeObjectMesh->SetupAttachment(SceneRoot);
	CuttableWholeObjectMesh->SetMobility(EComponentMobility::Movable);
	CuttableWholeObjectMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CuttableWholeObjectMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	CuttableWholeObjectMesh->SetCanEverAffectNavigation(false);
	CuttableWholeObjectMesh->SetCastShadow(true);

	LowerFloorLight = CreateDefaultSubobject<UPointLightComponent>(
		TEXT("LowerFloorLight"));
	LowerFloorLight->SetupAttachment(SceneRoot);
	LowerFloorLight->SetMobility(EComponentMobility::Movable);
	LowerFloorLight->SetRelativeLocation(FVector(-120.0f, -60.0f, 250.0f));
	LowerFloorLight->Intensity = 950.0f;
	LowerFloorLight->AttenuationRadius = 720.0f;
	LowerFloorLight->LightColor = FColor(255, 190, 118);
	LowerFloorLight->SetCastShadows(false);

	UpperFloorLight = CreateDefaultSubobject<UPointLightComponent>(
		TEXT("UpperFloorLight"));
	UpperFloorLight->SetupAttachment(SceneRoot);
	UpperFloorLight->SetMobility(EComponentMobility::Movable);
	UpperFloorLight->SetRelativeLocation(FVector(120.0f, 20.0f, 610.0f));
	UpperFloorLight->Intensity = 820.0f;
	UpperFloorLight->AttenuationRadius = 680.0f;
	UpperFloorLight->LightColor = FColor(255, 215, 155);
	UpperFloorLight->SetCastShadows(false);

	BuildHouseGeometry();
	ConfigureRampCollision();
}

void AMatterFluxTwoStoreyHouseActor::BeginPlay()
{
	Super::BeginPlay();
	InitializeStructureDefinition(StructureDefinitionId);
	if (IMatterFluxScriptRuntime::IsAvailable())
	{
		StructureReloadHandle = IMatterFluxScriptRuntime::Get()
			.OnContentReloaded().AddWeakLambda(
				this,
				[this](const FMatterFluxContentRegistryPtr)
				{
					InitializeStructureDefinition(StructureDefinitionId);
				});
	}
	SpawnCuttableStructureSources();
	RebuildCuttableWholeObjectMesh(true);
	ConfigureGroupMaterials();
	RefreshCutawayImmediately();
	if (GetNetMode() == NM_DedicatedServer)
	{
		SetActorTickEnabled(false);
	}
}

void AMatterFluxTwoStoreyHouseActor::InitializeStructureDefinition(
	const FName DefinitionId)
{
	StructureDefinitionId = DefinitionId;
	if (!IMatterFluxScriptRuntime::IsAvailable())
	{
		return;
	}
	const FMatterFluxContentRegistryPtr Registry =
		IMatterFluxScriptRuntime::Get().GetActiveRegistry();
	const FMatterFluxStructureDefinition* Definition = Registry.IsValid()
		? Registry->Structures.Find(DefinitionId) : nullptr;
	if (!Definition || Definition->GeneratorId != TEXT("two_storey_house"))
	{
		return;
	}
	CutawayPolicy.ContactToleranceCentimeters =
		Definition->ContactToleranceCentimeters;
	CutawayPolicy.FloorSnapHeightCentimeters =
		Definition->FloorSnapHeightCentimeters;
	CutawayPolicy.PreferredFloorPaddingCentimeters =
		Definition->PreferredFloorPaddingCentimeters;
	CutawayPolicy.PreferredFloorVerticalRangeCentimeters =
		Definition->PreferredFloorVerticalRangeCentimeters;
	CutawayExitGraceSeconds = Definition->ExitGraceSeconds;
	StructureFadeSpeed = Definition->FadeSpeed;
	WallGhostOpacity = Definition->WallGhostOpacity;
	RoofGhostOpacity = Definition->RoofGhostOpacity;
	for (FStructureFadeGroup& Group : StructureFadeGroups)
	{
		if (Group.StructuralRole
			== EMatterFluxMaterialStructuralRole::Wall)
		{
			Group.GhostOpacity = Group.MaterialId == TEXT("roof")
				? RoofGhostOpacity : WallGhostOpacity;
		}
	}
}

void AMatterFluxTwoStoreyHouseActor::DeactivateForStreamingPool()
{
	for (AFragment2DSourceActor* Source : CuttableStructureSources)
	{
		if (IsValid(Source))
		{
			Source->SetActorHiddenInGame(true);
			Source->SetActorEnableCollision(false);
			Source->SetActorTickEnabled(false);
		}
	}
	CurrentFloorSourceId.Invalidate();
	CurrentGhostSourceIds.Reset();
	CurrentCutawayFloor = INDEX_NONE;
	CurrentStructureOpacity = 1.0f;
	SetActorTickEnabled(false);
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	MoveHouseAndCuttableSources(FTransform(
		GetActorQuat(),
		FVector(0.0, 0.0, -10000000.0),
		GetActorScale3D()));
}

void AMatterFluxTwoStoreyHouseActor::ReactivateFromStreamingPool(
	const FTransform& WorldTransform,
	const FName DefinitionId)
{
	MoveHouseAndCuttableSources(WorldTransform);
	InitializeStructureDefinition(DefinitionId);
	int32 PieceIndex = 0;
	for (AFragment2DSourceActor* Source : CuttableStructureSources)
	{
		if (!IsValid(Source))
		{
			continue;
		}
		FName GroupName = TEXT("Unknown");
		for (const FName Tag : Source->Tags)
		{
			FString TagText = Tag.ToString();
			if (TagText.RemoveFromStart(TEXT("MatterFluxHouseGroup.")))
			{
				GroupName = FName(*TagText);
				break;
			}
		}
		Source->ResetForStreamingReuse(MakeHouseSourceId(
			GetActorLocation(),
			GroupName,
			PieceIndex++));
		Source->SetActorHiddenInGame(false);
		Source->SetActorEnableCollision(true);
		Source->SetActorTickEnabled(true);
	}
	CuttableWholeObjectSignature = MAX_uint32;
	RebuildCuttableWholeObjectMesh(true);
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	SetActorTickEnabled(GetNetMode() != NM_DedicatedServer);
	RefreshCutawayImmediately();
}

void AMatterFluxTwoStoreyHouseActor::MoveHouseAndCuttableSources(
	const FTransform& WorldTransform)
{
	const FTransform PreviousHouseTransform = GetActorTransform();
	TArray<FTransform> RelativeSourceTransforms;
	RelativeSourceTransforms.Reserve(CuttableStructureSources.Num());
	for (const AFragment2DSourceActor* Source : CuttableStructureSources)
	{
		RelativeSourceTransforms.Add(IsValid(Source)
			? Source->GetActorTransform().GetRelativeTransform(
				PreviousHouseTransform)
			: FTransform::Identity);
	}
	SetActorTransform(
		WorldTransform,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	for (int32 SourceIndex = 0;
		SourceIndex < CuttableStructureSources.Num();
		++SourceIndex)
	{
		AFragment2DSourceActor* Source = CuttableStructureSources[SourceIndex];
		if (IsValid(Source))
		{
			Source->SetActorTransform(
				RelativeSourceTransforms[SourceIndex] * WorldTransform,
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
			if (UFragmentSimulationSubsystem* FragmentSubsystem =
				GetWorld()
					? GetWorld()->GetSubsystem<UFragmentSimulationSubsystem>()
					: nullptr)
			{
				FragmentSubsystem->RegisterSourceActor(*Source);
			}
		}
	}
}

void AMatterFluxTwoStoreyHouseActor::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (StructureReloadHandle.IsValid()
		&& IMatterFluxScriptRuntime::IsAvailable())
	{
		IMatterFluxScriptRuntime::Get().OnContentReloaded().Remove(
			StructureReloadHandle);
		StructureReloadHandle.Reset();
	}
	DestroyCuttableStructureSources();
	Super::EndPlay(EndPlayReason);
}

void AMatterFluxTwoStoreyHouseActor::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	ConfigureGroupMaterials();
}

void AMatterFluxTwoStoreyHouseActor::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	RefreshReplicatedCuttableStructureSources(DeltaSeconds);
	RebuildCuttableWholeObjectMesh();
	ACharacter* Viewer = ResolveLocalViewer();
	UpdateCutawayFloor(Viewer, DeltaSeconds);
	UpdateStructureFade(DeltaSeconds);
}

UInstancedStaticMeshComponent*
AMatterFluxTwoStoreyHouseActor::CreateVoxelGroup(
	const FName Name,
	const bool bEnableCollision,
	const int32 FloorTier,
	const FLinearColor& Color,
	const bool bNeverFade,
	const bool bFloorSurface,
	const float GhostOpacity,
	const bool bCuttable,
	const bool bInteriorFixture,
	const FName MaterialId)
{
	UInstancedStaticMeshComponent* Group =
		CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
	Group->SetupAttachment(SceneRoot);
	// The entire generated house is pooled between streamed chunks. Every
	// attached projection must accept the same teleport as the movable root;
	// otherwise static HISM groups (most visibly the stairs) stay behind at the
	// previous chunk while the cuttable whole-object projection moves.
	Group->SetMobility(EComponentMobility::Movable);
	Group->SetStaticMesh(CubeMesh);
	Group->SetCollisionObjectType(ECC_WorldStatic);
	Group->SetCollisionEnabled(
		bEnableCollision
			? ECollisionEnabled::QueryAndPhysics
			: ECollisionEnabled::NoCollision);
	Group->SetCollisionResponseToAllChannels(
		bEnableCollision ? ECR_Block : ECR_Ignore);
	Group->SetCanEverAffectNavigation(bEnableCollision);
	Group->SetCastShadow(true);
	VoxelGroups.Add(Group);
	FStructureFadeGroup& Fade = StructureFadeGroups.AddDefaulted_GetRef();
	Fade.Component = Group;
	Fade.Color = Color;
	Fade.MaterialId = MaterialId;
	Fade.FloorTier = FloorTier;
	Fade.bNeverFade = bNeverFade;
	Fade.bFloorSurface = bFloorSurface;
	Fade.bCuttable = bCuttable;
	Fade.bInteriorFixture = bInteriorFixture;
	Fade.GhostOpacity = FMath::Clamp(GhostOpacity, 0.0f, 1.0f);
	return Group;
}

void AMatterFluxTwoStoreyHouseActor::AddBox(
	UInstancedStaticMeshComponent& Group,
	const FVector& Center,
	const FVector& Size,
	const FRotator& Rotation)
{
	Group.AddInstance(FTransform(Rotation, Center, ScaleForBox(Size)));
}

void AMatterFluxTwoStoreyHouseActor::BuildHouseGeometry()
{
	BuildFoundationAndFloors();
	BuildWallsAndRoof();
	BuildStairs();
	BuildFurniture();
}

void AMatterFluxTwoStoreyHouseActor::BuildFoundationAndFloors()
{
	UInstancedStaticMeshComponent* Foundation = CreateVoxelGroup(
		TEXT("Foundation"), true, 0,
		FLinearColor(0.22f, 0.25f, 0.28f), true, false, 0.12f,
		false, false, TEXT("stone"));
	AddBox(*Foundation, FVector(0.0f, 0.0f, -54.0f),
		FVector(1160.0f, 880.0f, 142.0f));

	UInstancedStaticMeshComponent* GroundFloor = CreateVoxelGroup(
		TEXT("GroundFloor"), true, 0,
		FLinearColor(0.48f, 0.25f, 0.095f), true, true, 0.12f,
		true, false, TEXT("wood"));
	AddBox(*GroundFloor,
		FVector(0.0f, 0.0f, GroundFloorTop - FloorThickness * 0.5f),
		FVector(1080.0f, 800.0f, FloorThickness));

	UInstancedStaticMeshComponent* UpperFloor = CreateVoxelGroup(
		TEXT("UpperFloor"), true, 1,
		FLinearColor(0.54f, 0.30f, 0.12f), false, true, FloorGhostOpacity,
		true, false, TEXT("wood"));
	// 二楼地板围绕楼梯井分成四块，避免视觉地板和坡面碰撞互相穿插。
	AddBox(*UpperFloor, FVector(-410.0f, 0.0f,
		UpperFloorTop - FloorThickness * 0.5f),
		FVector(260.0f, 800.0f, FloorThickness));
	AddBox(*UpperFloor, FVector(430.0f, 0.0f,
		UpperFloorTop - FloorThickness * 0.5f),
		FVector(220.0f, 800.0f, FloorThickness));
	AddBox(*UpperFloor, FVector(10.0f, 175.0f,
		UpperFloorTop - FloorThickness * 0.5f),
		FVector(580.0f, 450.0f, FloorThickness));
	AddBox(*UpperFloor, FVector(10.0f, -365.0f,
		UpperFloorTop - FloorThickness * 0.5f),
		FVector(580.0f, 70.0f, FloorThickness));
}

void AMatterFluxTwoStoreyHouseActor::BuildWallsAndRoof()
{
	const FLinearColor Plaster(0.82f, 0.70f, 0.48f);
	UInstancedStaticMeshComponent* LowerWalls = CreateVoxelGroup(
		TEXT("LowerWalls"), true, 0, Plaster, false, false, 0.055f,
		true, false, TEXT("stone"));
	UInstancedStaticMeshComponent* UpperWalls = CreateVoxelGroup(
		TEXT("UpperWalls"), true, 1, Plaster,
		false, false, 0.055f, true, false, TEXT("stone"));

	const auto AddStoreyWalls = [this](
		UInstancedStaticMeshComponent& Group,
		const float BaseZ,
		const float Height)
	{
		const float CenterZ = BaseZ + Height * 0.5f;
		constexpr float WestLowerCenterY = -275.0f;
		constexpr float WestLowerLength = 255.0f;
		constexpr float WestUpperCenterY = 315.0f;
		constexpr float WestUpperLength = 210.0f;
		constexpr float NorthLeftCenterX = -320.0f;
		constexpr float NorthLeftLength = 430.0f;
		constexpr float NorthRightCenterX = 405.0f;
		constexpr float NorthRightLength = 310.0f;
		// Independent source masks snap to the shared structure grid. Give each
		// lintel one cell of bearing on both side walls so grid rounding cannot
		// reopen a visible joint in the merged WholeObject projection.
		const float WestLintelMinY =
			WestLowerCenterY + WestLowerLength * 0.5f - StructureCellSize;
		const float WestLintelMaxY =
			WestUpperCenterY - WestUpperLength * 0.5f + StructureCellSize;
		const float NorthLintelMinX =
			NorthLeftCenterX + NorthLeftLength * 0.5f - StructureCellSize;
		const float NorthLintelMaxX =
			NorthRightCenterX - NorthRightLength * 0.5f + StructureCellSize;
		// 背向镜头的两面完整承重墙。
		AddBox(Group, FVector(HalfSizeX - WallThickness * 0.5f, 0.0f, CenterZ),
			FVector(WallThickness, HalfSizeY * 2.0f, Height));
		AddBox(Group, FVector(0.0f, -HalfSizeY + WallThickness * 0.5f, CenterZ),
			FVector(HalfSizeX * 2.0f, WallThickness, Height));
		// 朝镜头的两面保留门洞和大窗，形成可读的娃娃屋切面。
		AddBox(Group, FVector(-HalfSizeX + WallThickness * 0.5f,
			WestLowerCenterY, CenterZ),
			FVector(WallThickness, WestLowerLength, Height));
		AddBox(Group, FVector(-HalfSizeX + WallThickness * 0.5f,
			WestUpperCenterY, CenterZ),
			FVector(WallThickness, WestUpperLength, Height));
		AddBox(Group, FVector(-HalfSizeX + WallThickness * 0.5f,
			(WestLintelMinY + WestLintelMaxY) * 0.5f,
			BaseZ + Height - 38.0f),
			FVector(WallThickness, WestLintelMaxY - WestLintelMinY, 76.0f));
		AddBox(Group, FVector(NorthLeftCenterX,
			HalfSizeY - WallThickness * 0.5f, CenterZ),
			FVector(NorthLeftLength, WallThickness, Height));
		AddBox(Group, FVector(NorthRightCenterX,
			HalfSizeY - WallThickness * 0.5f, CenterZ),
			FVector(NorthRightLength, WallThickness, Height));
		AddBox(Group, FVector(
			(NorthLintelMinX + NorthLintelMaxX) * 0.5f,
			HalfSizeY - WallThickness * 0.5f,
			BaseZ + Height - 38.0f),
			FVector(NorthLintelMaxX - NorthLintelMinX,
				WallThickness, 76.0f));
		// 角柱让开放切面仍然清楚表现为完整建筑。
		for (const FVector2D Corner : {
			FVector2D(-HalfSizeX + 26.0f, -HalfSizeY + 26.0f),
			FVector2D(-HalfSizeX + 26.0f, HalfSizeY - 26.0f),
			FVector2D(HalfSizeX - 26.0f, -HalfSizeY + 26.0f),
			FVector2D(HalfSizeX - 26.0f, HalfSizeY - 26.0f) })
		{
			AddBox(Group, FVector(Corner, CenterZ),
				FVector(52.0f, 52.0f, Height));
		}
	};
	AddStoreyWalls(*LowerWalls, GroundFloorTop, WallHeight);
	// 二楼墙从楼板底开始包住楼板边缘；它与一楼墙共享同一体素
	// 边界，楼板不再作为一圈棕色接缝暴露在外立面上。
	AddStoreyWalls(*UpperWalls,
		UpperFloorTop - FloorThickness, StoreyHeight);

	// 屋顶是两片一格厚的阶梯瓦壳，不是逐层填满的实心山墙。
	// 每一级左右各一条贯穿长轴的瓦梁，最高一级合并成屋脊。
	UInstancedStaticMeshComponent* Roof = CreateVoxelGroup(
		TEXT("Roof"), true, 2,
		// 旧值接近纯黑，背光坡面会完全丢失体素层级。颜色仍保持
		// 深红陶瓦，但给真实光照和顶点 AO 留出足够动态范围。
		FLinearColor(0.34f, 0.085f, 0.035f), false, false, 0.025f,
		true, false, TEXT("roof"));
	const float RoofBottom = UpperFloorTop + WallHeight;
	for (int32 Step = 0; Step <= RoofStepCount; ++Step)
	{
		const float StepWidth = RoofOuterWidth
			- static_cast<float>(Step) * RoofRun * 2.0f;
		const float CenterZ = RoofBottom
			+ static_cast<float>(Step) * RoofRise
			+ RoofShellThickness * 0.5f;
		if (Step == RoofStepCount)
		{
			AddBox(*Roof, FVector(0.0f, 0.0f, CenterZ),
				FVector(RoofRidgeLength, StepWidth, RoofShellThickness));
			continue;
		}

		const float OuterHalfWidth = StepWidth * 0.5f;
		const float StripCenter = OuterHalfWidth - RoofRun * 0.5f;
		AddBox(*Roof, FVector(0.0f, -StripCenter, CenterZ),
			FVector(RoofRidgeLength, RoofRun, RoofShellThickness));
		AddBox(*Roof, FVector(0.0f, StripCenter, CenterZ),
			FVector(RoofRidgeLength, RoofRun, RoofShellThickness));
	}
}

void AMatterFluxTwoStoreyHouseActor::BuildStairs()
{
	UInstancedStaticMeshComponent* Stairs = CreateVoxelGroup(
		TEXT("Stairs"), false, 0,
		FLinearColor(0.34f, 0.16f, 0.055f), true, false, 0.12f,
		false, false, TEXT("wood"));
	constexpr int32 StepCount = 14;
	constexpr float StartX = -330.0f;
	constexpr float Run = 660.0f;
	constexpr float StairY = -245.0f;
	constexpr float StairWidth = 190.0f;
	for (int32 Step = 0; Step < StepCount; ++Step)
	{
		const float Alpha = static_cast<float>(Step)
			/ static_cast<float>(StepCount - 1);
		// The visible stair is a projection of the same walkable span as the
		// collision ramp.  Its first and last treads must meet the two canonical
		// floor surfaces; stopping below UpperFloorTop leaves a capsule stranded
		// in the stair opening and prevents upper-floor material selection.
		const float StepHeight = GroundFloorTop + Alpha * StoreyHeight;
		AddBox(*Stairs,
			FVector(StartX + Alpha * Run, StairY, StepHeight * 0.5f),
			FVector(Run / static_cast<float>(StepCount) + 5.0f,
				StairWidth, StepHeight));
	}
	// 楼梯扶手使用稀疏体素柱，不参与碰撞。
	for (int32 Rail = 0; Rail < 6; ++Rail)
	{
		const float Alpha = static_cast<float>(Rail) / 5.0f;
		const float Z = 95.0f + Alpha * (StoreyHeight - 34.0f);
		AddBox(*Stairs,
			FVector(StartX + Alpha * Run, StairY + StairWidth * 0.52f, Z),
			FVector(22.0f, 22.0f, 150.0f));
	}
}

void AMatterFluxTwoStoreyHouseActor::BuildFurniture()
{
	UInstancedStaticMeshComponent* LowerWood = CreateVoxelGroup(
		TEXT("LowerFurnitureWood"), true, 0,
		FLinearColor(0.36f, 0.15f, 0.045f), false, false, 0.22f,
		true, true, TEXT("wood"));
	UInstancedStaticMeshComponent* LowerAccent = CreateVoxelGroup(
		TEXT("LowerFurnitureAccent"), true, 0,
		FLinearColor(0.12f, 0.38f, 0.36f), false, false, 0.22f,
		true, true, TEXT("fabric"));
	UInstancedStaticMeshComponent* UpperWood = CreateVoxelGroup(
		TEXT("UpperFurnitureWood"), true, 1,
		FLinearColor(0.40f, 0.19f, 0.07f), false, false, 0.22f,
		true, true, TEXT("wood"));
	UInstancedStaticMeshComponent* UpperAccent = CreateVoxelGroup(
		TEXT("UpperFurnitureAccent"), true, 1,
		FLinearColor(0.64f, 0.16f, 0.24f), false, false, 0.22f,
		true, true, TEXT("fabric"));

	// 楼梯移到后侧后，家具整体让到前侧，保持后侧坡面与楼梯井净空。
	// 一楼：长桌、凳子、书架和地毯。
	AddBox(*LowerWood, FVector(-115.0f, 120.0f, 118.0f),
		FVector(300.0f, 145.0f, 28.0f));
	for (const FVector2D Leg : {
		FVector2D(-225.0f, 165.0f), FVector2D(-5.0f, 165.0f),
		FVector2D(-225.0f, 75.0f), FVector2D(-5.0f, 75.0f) })
	{
		AddBox(*LowerWood, FVector(Leg, 66.0f), FVector(24.0f, 24.0f, 104.0f));
	}
	AddBox(*LowerWood, FVector(465.0f, 300.0f, 150.0f),
		FVector(90.0f, 180.0f, 265.0f));
	AddBox(*LowerAccent, FVector(-120.0f, 115.0f, 40.0f),
		FVector(390.0f, 235.0f, 12.0f));
	AddBox(*LowerAccent, FVector(285.0f, -50.0f, 78.0f),
		FVector(145.0f, 92.0f, 88.0f));

	// 二楼：床、床头柜、矮书桌与彩色床毯。
	const float UpperBase = UpperFloorTop;
	AddBox(*UpperWood, FVector(-170.0f, 235.0f, UpperBase + 62.0f),
		FVector(350.0f, 205.0f, 58.0f));
	AddBox(*UpperWood, FVector(-326.0f, 235.0f, UpperBase + 120.0f),
		FVector(38.0f, 205.0f, 175.0f));
	AddBox(*UpperAccent, FVector(-150.0f, 235.0f, UpperBase + 96.0f),
		FVector(300.0f, 185.0f, 24.0f));
	AddBox(*UpperWood, FVector(350.0f, 170.0f, UpperBase + 102.0f),
		FVector(225.0f, 115.0f, 26.0f));
	AddBox(*UpperAccent, FVector(360.0f, 168.0f, UpperBase + 145.0f),
		FVector(58.0f, 58.0f, 58.0f));
}

void AMatterFluxTwoStoreyHouseActor::SpawnCuttableStructureSources()
{
	UWorld* World = GetWorld();
	int32 PieceIndex = 0;
	const auto SpawnMaskSource = [this, World, &PieceIndex](
		const FFragmentSourceMask& Mask,
		const FTransform& WorldSourceTransform,
		const bool bEnableCollision,
		const FStructureFadeGroup& Group,
		const FName GroupName,
		const FName MaterialId)
	{
		AFragment2DSourceActor* Source =
			World->SpawnActorDeferred<AFragment2DSourceActor>(
				AFragment2DSourceActor::StaticClass(),
				WorldSourceTransform,
				this,
				nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Source)
		{
			return;
		}
		const FGuid SourceId = MakeHouseSourceId(
			GetActorLocation(),
			GroupName,
			PieceIndex++);
		Source->FragmentMaterial = SolidMaterialTemplate;
		Source->bDestroySourceOnFirstBreak = false;
		Source->SetSourceCollisionEnabled(bEnableCollision);
		const EMatterFluxMaterialStructuralRole StructuralRole =
			Group.bFloorSurface
				? EMatterFluxMaterialStructuralRole::Floor
				: Group.bInteriorFixture
					? EMatterFluxMaterialStructuralRole::Furniture
					: EMatterFluxMaterialStructuralRole::Wall;
		if (!Source->InitializeFromProceduralMask(
			Mask, SourceId, Group.Color, MaterialId, StructuralRole))
		{
			Source->Destroy();
			return;
		}
		Source->Tags.AddUnique(TEXT("MatterFluxHouseStructure"));
		if (Group.bInteriorFixture)
		{
			Source->Tags.AddUnique(TEXT("MatterFluxHouseFurniture"));
		}
		Source->Tags.AddUnique(FName(*FString::Printf(
			TEXT("MatterFluxHouseGroup.%s"),
			*GroupName.ToString())));
		Source->FinishSpawning(WorldSourceTransform);
		// 逻辑 Source 保留查询碰撞和伤害状态；可视表现由房屋的
		// WholeObject mesh 统一批处理，防止重合面与静态/切割形态跳变。
		Source->SetSourceMeshProjectionEnabled(false);
		CuttableStructureSources.Add(Source);
	};
	const int32 OriginalGroupCount = StructureFadeGroups.Num();
	for (int32 GroupIndex = 0;
		GroupIndex < OriginalGroupCount; ++GroupIndex)
	{
		const FStructureFadeGroup Group = StructureFadeGroups[GroupIndex];
		if (!Group.bCuttable)
		{
			continue;
		}
		UInstancedStaticMeshComponent* Instances =
			Cast<UInstancedStaticMeshComponent>(Group.Component.Get());
		if (!Instances)
		{
			continue;
		}
		// 可切结构由 Source Actor 负责显示和碰撞；原 HISM 必须同时
		// 关闭，否则切口后仍会看见一层未损坏的“幽灵墙”。
		const bool bWasCollidable = Instances->GetCollisionEnabled()
			!= ECollisionEnabled::NoCollision;
		Instances->SetVisibility(false, true);
		Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (!World || (World->IsGameWorld() && !HasAuthority()))
		{
			continue;
		}
		if (Instances->GetFName() == TEXT("Roof"))
		{
			// 编辑预览的二十五条瓦梁在游戏态合成一个薄壳截面 mask，
			// 再沿房屋长轴挤出。每列只保留最高台阶附近两格厚度；
			// 屋檐下保持中空，斜视时不会形成实心扇形条带。
			FFragmentSourceMask RoofMask;
			RoofMask.Width = FMath::RoundToInt(
				RoofOuterWidth / RoofCellSize);
			RoofMask.Height = FMath::RoundToInt(
				RoofTotalHeight / RoofCellSize);
			RoofMask.CellSize = RoofCellSize;
			RoofMask.MinFragmentAreaPixels = 3;
			RoofMask.MaxFragmentsPerBreak = 16;
			RoofMask.SupportMode = EFragmentSupportMode::None;
			RoofMask.GeometryStyle =
				EFragmentSourceGeometryStyle::VoxelBlocks;
			RoofMask.SolidMask.Init(
				0, RoofMask.Width * RoofMask.Height);
			for (int32 X = 0; X < RoofMask.Width; ++X)
			{
				const float LocalCrossSlope =
					(static_cast<float>(X) + 0.5f
						- static_cast<float>(RoofMask.Width) * 0.5f)
					* RoofCellSize;
				const int32 SurfaceStep = FindRoofSurfaceStep(LocalCrossSlope);
				if (SurfaceStep == INDEX_NONE)
				{
					continue;
				}
				const float ShellBottom =
					static_cast<float>(SurfaceStep) * RoofRise;
				const float ShellTop = ShellBottom + RoofShellThickness;
				for (int32 Y = 0; Y < RoofMask.Height; ++Y)
				{
					const float LocalZ =
						(static_cast<float>(Y) + 0.5f) * RoofCellSize;
					if (LocalZ >= ShellBottom && LocalZ < ShellTop)
					{
						RoofMask.SolidMask[Y * RoofMask.Width + X] = 1;
					}
				}
			}

			const FTransform LocalRoofTransform = SnapMaskLayerToGrid(
				FTransform(
				FQuat(FVector::ZAxisVector, HALF_PI),
				FVector(
					0.0f,
					0.0f,
					UpperFloorTop + WallHeight
						+ RoofTotalHeight * 0.5f),
				FVector(1.0f, RoofRidgeLength / RoofCellSize, 1.0f)),
				RoofMask);
			SpawnMaskSource(
				RoofMask,
				LocalRoofTransform * GetActorTransform(),
				bWasCollidable,
				Group,
				Instances->GetFName(),
				TEXT("roof"));

			// 屋顶薄壳只描述两片坡面；沿屋脊方向的两端还需要山墙
			// 封板。封板复用相同阶梯函数，填充“墙顶到坡面下缘”
			// 的区域，因此不会穿进瓦壳，也不会把整个阁楼填实。
			FFragmentSourceMask GableMask = RoofMask;
			GableMask.SolidMask.Init(
				0, GableMask.Width * GableMask.Height);
			for (int32 X = 0; X < GableMask.Width; ++X)
			{
				const float LocalCrossSlope =
					(static_cast<float>(X) + 0.5f
						- static_cast<float>(GableMask.Width) * 0.5f)
					* RoofCellSize;
				const int32 SurfaceStep = FindRoofSurfaceStep(LocalCrossSlope);
				if (SurfaceStep <= 0)
				{
					continue;
				}
				const float GableTop =
					static_cast<float>(SurfaceStep) * RoofRise;
				for (int32 Y = 0; Y < GableMask.Height; ++Y)
				{
					const float LocalZ =
						(static_cast<float>(Y) + 0.5f) * RoofCellSize;
					if (LocalZ < GableTop)
					{
						GableMask.SolidMask[
							Y * GableMask.Width + X] = 1;
					}
				}
			}

			for (const float EndSign : { -1.0f, 1.0f })
			{
				const FTransform LocalGableTransform = SnapMaskLayerToGrid(
					FTransform(
					FQuat(FVector::ZAxisVector, HALF_PI),
					FVector(
						EndSign * (HalfSizeX - WallThickness * 0.5f),
						0.0f,
						UpperFloorTop + WallHeight
							+ RoofTotalHeight * 0.5f),
					FVector(
						1.0f,
						WallThickness / RoofCellSize,
						1.0f)),
					GableMask);
				SpawnMaskSource(
					GableMask,
					LocalGableTransform * GetActorTransform(),
					bWasCollidable,
					Group,
					TEXT("RoofGable"),
					TEXT("roof"));
			}
			continue;
		}

		for (int32 InstanceIndex = 0;
			InstanceIndex < Instances->GetNumInstances(); ++InstanceIndex)
		{
			FTransform BoxTransform;
			if (!Instances->GetInstanceTransform(
				InstanceIndex, BoxTransform, false))
			{
				continue;
			}
			const FVector Size = BoxTransform.GetScale3D().GetAbs() * 100.0f;
			const int32 ThinAxis = Size.X <= Size.Y && Size.X <= Size.Z
				? 0
				: Size.Y <= Size.Z ? 1 : 2;
			float Width = ThinAxis == 0 ? Size.Y : Size.X;
			float Height = ThinAxis == 2 ? Size.Y : Size.Z;
			const float Thickness = ThinAxis == 0
				? Size.X : ThinAxis == 1 ? Size.Y : Size.Z;
			// Fragment mask 的平面固定为局部 XZ，挤出轴固定为局部 Y。
			// 因此 Y 薄的墙不需要旋转；X 薄的墙绕 Z 旋转；Z 薄的
			// 屋顶绕 X 旋转。这里显式记录轴约定，避免把墙误铺成
			// 贯穿室内的水平碰撞薄板。
			const FQuat PlaneRotation = ThinAxis == 1
				? FQuat::Identity
				: ThinAxis == 0
					? FQuat(FVector::ZAxisVector, HALF_PI)
					: FQuat(FVector::XAxisVector, HALF_PI);
			const FQuat LocalRotation =
				BoxTransform.GetRotation() * PlaneRotation;

			FFragmentSourceMask Mask;
			// 每个独立盒体都向共同体素格的外侧取整。向最近格收缩会在
			// 相邻墙段、墙与楼板包边之间留下至多一格的可见裂缝。
			Mask.Width = FMath::Clamp(
				FMath::CeilToInt(Width / StructureCellSize), 1, 256);
			Mask.Height = FMath::Clamp(
				FMath::CeilToInt(Height / StructureCellSize), 1, 256);
			Mask.CellSize = StructureCellSize;
			Mask.MinFragmentAreaPixels = 3;
			Mask.MaxFragmentsPerBreak = 16;
			Mask.SupportMode = ThinAxis == 2
				? EFragmentSupportMode::None
				: EFragmentSupportMode::Bottom;
			Mask.GeometryStyle = EFragmentSourceGeometryStyle::VoxelBlocks;
			Mask.SolidMask.Init(1, Mask.Width * Mask.Height);

			const FTransform LocalSourceTransform = SnapMaskLayerToGrid(
				FTransform(
				LocalRotation,
				BoxTransform.GetLocation(),
				// Fragment mesh 的挤出轴固定为局部 Y；厚度只能缩放 Y。
				// 旧代码误缩放 Z，水平屋顶会把整条屋脊长度再乘一次
				// 厚度比例，墙体也会被异常拉高。
				FVector(1.0f,
					Thickness / StructureCellSize,
					1.0f)),
				Mask);
			const FTransform WorldSourceTransform =
				LocalSourceTransform * GetActorTransform();
			SpawnMaskSource(
				Mask,
				WorldSourceTransform,
				bWasCollidable,
				Group,
				Instances->GetFName(),
				Group.MaterialId.IsNone()
					? TEXT("stone") : Group.MaterialId);
		}
	}
}

uint32 AMatterFluxTwoStoreyHouseActor::
	ComputeCuttableWholeObjectSignature() const
{
	TArray<const AFragment2DSourceActor*> OrderedSources;
	for (const AFragment2DSourceActor* Source : CuttableStructureSources)
	{
		if (IsValid(Source) && !Source->IsActorBeingDestroyed())
		{
			OrderedSources.Add(Source);
		}
	}
	OrderedSources.Sort([](
		const AFragment2DSourceActor& A,
		const AFragment2DSourceActor& B)
	{
		if (A.SourceId.A != B.SourceId.A) return A.SourceId.A < B.SourceId.A;
		if (A.SourceId.B != B.SourceId.B) return A.SourceId.B < B.SourceId.B;
		if (A.SourceId.C != B.SourceId.C) return A.SourceId.C < B.SourceId.C;
		return A.SourceId.D < B.SourceId.D;
	});
	uint32 Signature = GetTypeHash(OrderedSources.Num());
	for (const AFragment2DSourceActor* Source : OrderedSources)
	{
		Signature = HashCombineFast(Signature, GetTypeHash(Source->SourceId));
		Signature = HashCombineFast(Signature, GetTypeHash(Source->Revision));
		Signature = HashCombineFast(Signature, GetTypeHash(Source->bBroken));
		Signature = HashCombineFast(
			Signature,
			GetTypeHash(static_cast<uint8>(Source->StructuralRole)));
		const TArray<uint8>& Mask = Source->GetRuntimeMask();
		Signature = HashCombineFast(
			Signature,
			Mask.IsEmpty()
				? 0u
				: FCrc::MemCrc32(Mask.GetData(), Mask.Num()));
	}
	return Signature;
}

void AMatterFluxTwoStoreyHouseActor::RebuildCuttableWholeObjectMesh(
	const bool bForce)
{
	if (!CuttableWholeObjectMesh || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	const uint32 NextSignature = ComputeCuttableWholeObjectSignature();
	if (!bForce && NextSignature == CuttableWholeObjectSignature)
	{
		return;
	}
	CuttableWholeObjectSignature = NextSignature;

	StructureFadeGroups.RemoveAll([this](const FStructureFadeGroup& Group)
	{
		return Group.Component.Get() == CuttableWholeObjectMesh;
	});
	CuttableWholeObjectMesh->ClearAllMeshSections();
	CuttableWholeObjectMaterials.Reset();

	struct FSourceView
	{
		AFragment2DSourceActor* Source = nullptr;
		FTransform LocalTransform = FTransform::Identity;
		float CellSize = 0.0f;
		EMatterFluxMaterialStructuralRole StructuralRole =
			EMatterFluxMaterialStructuralRole::None;
		int32 FloorTier = 0;
	};
	TArray<FSourceView> SourceViews;
	for (AFragment2DSourceActor* Source : CuttableStructureSources)
	{
		if (!IsValid(Source)
			|| Source->IsActorBeingDestroyed()
			|| Source->bBroken
			|| !Source->GetRuntimeMask().Contains(1)
			|| !Source->ProceduralSource.HasValidLayout())
		{
			continue;
		}
		const FTransform LocalTransform =
			Source->GetActorTransform().GetRelativeTransform(GetActorTransform());
		SourceViews.Add({
			Source,
			LocalTransform,
			Source->GetCellSize(),
			Source->StructuralRole,
			Source->ActorHasTag(TEXT("MatterFluxHouseGroup.UpperFloor"))
				? 1 : 0});
	}
	SourceViews.Sort([](const FSourceView& A, const FSourceView& B)
	{
		if (A.CellSize != B.CellSize) return A.CellSize < B.CellSize;
		if (A.StructuralRole != B.StructuralRole)
		{
			return static_cast<uint8>(A.StructuralRole)
				< static_cast<uint8>(B.StructuralRole);
		}
		const FGuid& Left = A.Source->SourceId;
		const FGuid& Right = B.Source->SourceId;
		if (Left.A != Right.A) return Left.A < Right.A;
		if (Left.B != Right.B) return Left.B < Right.B;
		if (Left.C != Right.C) return Left.C < Right.C;
		return Left.D < Right.D;
	});
	if (SourceViews.IsEmpty())
	{
		CuttableWholeObjectMesh->SetVisibility(false, true);
		return;
	}

	TArray<float> CellSizes;
	for (const FSourceView& View : SourceViews)
	{
		CellSizes.AddUnique(View.CellSize);
	}
	CellSizes.Sort();
	int32 SectionIndex = 0;
	bool bAllBatchesBuilt = true;
	for (const float CellSize : CellSizes)
	{
		struct FMaterialInfo
		{
			FString StableKey;
			TObjectPtr<UMaterialInterface> Parent;
			FName MaterialId = NAME_None;
			FLinearColor Color = FLinearColor::White;
			FGuid SourceId;
			EMatterFluxMaterialStructuralRole StructuralRole =
				EMatterFluxMaterialStructuralRole::None;
			int32 FloorTier = 0;
		};
		TArray<FMaterialInfo> Materials;
		for (const FSourceView& View : SourceViews)
		{
			if (!FMath::IsNearlyEqual(View.CellSize, CellSize))
			{
				continue;
			}
			const FString StableKey = FString::Printf(
				TEXT("%s|%d|%s|%08x"),
				*View.Source->SourceId.ToString(EGuidFormats::Digits),
				static_cast<int32>(View.StructuralRole),
				*View.Source->SourceMaterialId.ToString(),
				View.Source->FragmentColor.ToFColor(false).DWColor());
			if (!Materials.ContainsByPredicate(
				[&StableKey](const FMaterialInfo& Existing)
				{
					return Existing.StableKey == StableKey;
				}))
			{
				Materials.Add({
					StableKey,
					View.Source->FragmentMaterial,
					View.Source->SourceMaterialId,
					View.Source->FragmentColor,
					View.Source->SourceId,
					View.StructuralRole,
					View.FloorTier});
			}
		}
		Materials.Sort([](const FMaterialInfo& A, const FMaterialInfo& B)
		{
			return A.StableKey < B.StableKey;
		});

		TArray<MatterFlux::WholeObject::FLayer> WholeLayers;
		for (const FSourceView& View : SourceViews)
		{
			if (!FMath::IsNearlyEqual(View.CellSize, CellSize))
			{
				continue;
			}
			const FString StableKey = FString::Printf(
				TEXT("%s|%d|%s|%08x"),
				*View.Source->SourceId.ToString(EGuidFormats::Digits),
				static_cast<int32>(View.StructuralRole),
				*View.Source->SourceMaterialId.ToString(),
				View.Source->FragmentColor.ToFColor(false).DWColor());
			const int32 MaterialIndex = Materials.IndexOfByPredicate(
				[&StableKey](const FMaterialInfo& Existing)
				{
					return Existing.StableKey == StableKey;
				});
			const FVector SourceScale =
				View.LocalTransform.GetScale3D().GetAbs();
			const int32 DepthCellCount = FMath::Clamp(
				FMath::RoundToInt(SourceScale.Y), 1, 256);
			const FTransform BaseTransform(
				View.LocalTransform.GetRotation(),
				View.LocalTransform.GetLocation(),
				FVector::OneVector);
			for (int32 DepthIndex = 0;
				DepthIndex < DepthCellCount;
				++DepthIndex)
			{
				const float LocalDepth =
					(static_cast<float>(DepthIndex) + 0.5f
						- static_cast<float>(DepthCellCount) * 0.5f)
					* CellSize;
				MatterFlux::WholeObject::FLayer& Layer =
					WholeLayers.AddDefaulted_GetRef();
				Layer.MaterialIndex = MaterialIndex;
				Layer.Priority =
					static_cast<int32>(View.StructuralRole) * 10 + 10;
				Layer.bEnableCollision = View.Source->bEnableSourceCollision;
				Layer.Width = View.Source->GetMaskWidth();
				Layer.Height = View.Source->GetMaskHeight();
				Layer.CellSize = CellSize;
				Layer.LocalTransform = FTransform(
					BaseTransform.GetRotation(),
					BaseTransform.TransformPosition(
						FVector(0.0f, LocalDepth, 0.0f)));
				Layer.SolidMask = View.Source->GetRuntimeMask();
			}
		}

		MatterFlux::WholeObject::FBuildResult BatchMesh;
		FString BuildError;
		if (!MatterFlux::WholeObject::BuildMesh(
			WholeLayers, BatchMesh, &BuildError))
		{
			UE_LOG(LogTemp, Error,
				TEXT("House WholeObject build failed: %s"), *BuildError);
			bAllBatchesBuilt = false;
			break;
		}
		for (const MatterFlux::WholeObject::FMeshSection& Section
			: BatchMesh.Sections)
		{
			if (!Materials.IsValidIndex(Section.MaterialIndex))
			{
				bAllBatchesBuilt = false;
				break;
			}
			const FMaterialInfo& Info = Materials[Section.MaterialIndex];
			CuttableWholeObjectMesh->CreateMeshSection(
				SectionIndex,
				Section.Vertices,
				Section.Triangles,
				Section.Normals,
				Section.UVs,
				Section.VertexColors,
				TArray<FProcMeshTangent>(),
				false);

			UMaterialInterface* Parent = Info.Parent
				? Info.Parent.Get() : SolidMaterialTemplate.Get();
			UMaterialInstanceDynamic* Solid = Parent
				? UMaterialInstanceDynamic::Create(Parent, this)
				: nullptr;
			if (Solid)
			{
				const bool bSide = Section.FaceRole
					== MatterFlux::WholeObject::EFaceRole::Side;
				MatterFlux::Rendering::ApplyVoxelMaterialProjection(
					*Solid,
					MatterFlux::Rendering::ResolveVoxelMaterialProjection(
						Info.Color,
						Info.MaterialId,
						CellSize,
						bSide
							? MatterFlux::Rendering::EVoxelMaterialFaceRole::Side
							: MatterFlux::Rendering::EVoxelMaterialFaceRole::Primary));
				CuttableWholeObjectMesh->SetMaterial(SectionIndex, Solid);
				CuttableWholeObjectMaterials.Add(Solid);
			}

			FStructureFadeGroup& Fade =
				StructureFadeGroups.AddDefaulted_GetRef();
			Fade.Component = CuttableWholeObjectMesh;
			Fade.Color = Info.Color;
			Fade.SourceId = Info.SourceId;
			Fade.StructuralRole = Info.StructuralRole;
			Fade.FloorTier = Info.FloorTier;
			Fade.bCuttable = true;
			Fade.bFloorSurface = Info.StructuralRole
				== EMatterFluxMaterialStructuralRole::Floor;
			Fade.bInteriorFixture = Info.StructuralRole
				== EMatterFluxMaterialStructuralRole::Furniture;
			Fade.MaterialId = Info.MaterialId;
			Fade.GhostOpacity = Info.MaterialId == TEXT("roof")
				? RoofGhostOpacity
				: Info.StructuralRole
					== EMatterFluxMaterialStructuralRole::Floor
						? FloorGhostOpacity
						: WallGhostOpacity;
			Fade.MaterialSlots.Add(SectionIndex);
			Fade.SolidMaterials.Add(Solid);
			if (GhostMaterialTemplate)
			{
				const bool bRoof = Info.MaterialId == TEXT("roof");
				UMaterialInstanceDynamic* Ghost =
					UMaterialInstanceDynamic::Create(
						GhostMaterialTemplate, this);
				Ghost->SetVectorParameterValue(TEXT("Color"), Info.Color);
				Ghost->SetScalarParameterValue(TEXT("Opacity"), 1.0f);
				Ghost->SetScalarParameterValue(
					TEXT("FaceContrast"), bRoof ? 0.24f : 0.12f);
				Ghost->SetScalarParameterValue(
					TEXT("PixelSize"), FMath::Max(CellSize, 4.0f));
				Ghost->SetScalarParameterValue(
					TEXT("ShadowLift"), bRoof ? 0.28f : 0.08f);
				Fade.GhostMaterial = Ghost;
				CuttableWholeObjectMaterials.Add(Ghost);
			}
			++SectionIndex;
		}
		if (!bAllBatchesBuilt)
		{
			break;
		}
	}

	if (!bAllBatchesBuilt)
	{
		CuttableWholeObjectMesh->ClearAllMeshSections();
		CuttableWholeObjectMesh->SetVisibility(false, true);
		for (const FSourceView& View : SourceViews)
		{
			View.Source->MeshComponent->SetVisibility(true, true);
		}
		return;
	}
	for (const FSourceView& View : SourceViews)
	{
		View.Source->MeshComponent->SetVisibility(false, true);
	}
	CuttableWholeObjectMesh->SetVisibility(SectionIndex > 0, true);
	CuttableWholeObjectMesh->SetCastShadow(true);
}

void AMatterFluxTwoStoreyHouseActor::
	RefreshReplicatedCuttableStructureSources(const float DeltaSeconds)
{
	if (GetNetMode() != NM_Client || !GetWorld())
	{
		return;
	}
	CuttableSourceDiscoveryAccumulator += FMath::Max(DeltaSeconds, 0.0f);
	if (CuttableSourceDiscoveryAccumulator < 0.25f)
	{
		return;
	}
	CuttableSourceDiscoveryAccumulator = FMath::Fmod(
		CuttableSourceDiscoveryAccumulator, 0.25f);
	CuttableStructureSources.RemoveAll(
		[](const AFragment2DSourceActor* Source)
		{
			return !IsValid(Source) || Source->IsActorBeingDestroyed();
		});
	for (TActorIterator<AFragment2DSourceActor> It(GetWorld()); It; ++It)
	{
		AFragment2DSourceActor* Source = *It;
		if (!IsValid(Source)
			|| Source->GetOwner() != this
			|| !Source->ActorHasTag(TEXT("MatterFluxHouseStructure"))
			|| CuttableStructureSources.Contains(Source))
		{
			continue;
		}
		CuttableStructureSources.Add(Source);
		Source->SetSourceMeshProjectionEnabled(false);
		CuttableWholeObjectSignature = MAX_uint32;
	}
}

void AMatterFluxTwoStoreyHouseActor::DestroyCuttableStructureSources()
{
	const bool bCanDestroyAuthoritativeSources = !GetWorld()
		|| !GetWorld()->IsGameWorld()
		|| HasAuthority();
	for (AFragment2DSourceActor* Source : CuttableStructureSources)
	{
		if (bCanDestroyAuthoritativeSources
			&& IsValid(Source) && !Source->IsActorBeingDestroyed())
		{
			Source->Destroy();
		}
	}
	CuttableStructureSources.Reset();
}

void AMatterFluxTwoStoreyHouseActor::ConfigureGroupMaterials()
{
	if (bMaterialsConfigured)
	{
		return;
	}
	bMaterialsConfigured = true;
	for (FStructureFadeGroup& Group : StructureFadeGroups)
	{
		UMeshComponent* Component = Group.Component.Get();
		if (!Component || !SolidMaterialTemplate)
		{
			continue;
		}
		UMaterialInstanceDynamic* Solid = UMaterialInstanceDynamic::Create(
			SolidMaterialTemplate, this);
		MatterFlux::Rendering::ApplyVoxelMaterialProjection(
			*Solid,
			MatterFlux::Rendering::ResolveVoxelMaterialProjection(
				Group.Color,
				Group.MaterialId,
				17.0f,
				MatterFlux::Rendering::EVoxelMaterialFaceRole::Primary));
		Group.SolidMaterials.Reset();
		TArray<int32> TargetSlots = Group.MaterialSlots;
		if (TargetSlots.IsEmpty())
		{
			const int32 MaterialSlotCount = FMath::Max(
				Component->GetNumMaterials(), 1);
			for (int32 SlotIndex = 0;
				SlotIndex < MaterialSlotCount; ++SlotIndex)
			{
				TargetSlots.Add(SlotIndex);
			}
		}
		for (const int32 SlotIndex : TargetSlots)
		{
			Group.SolidMaterials.Add(Solid);
			Component->SetMaterial(SlotIndex, Solid);
		}
		HouseMaterials.Add(Solid);

		if (GhostMaterialTemplate && !Group.bNeverFade)
		{
			UMaterialInstanceDynamic* Ghost = UMaterialInstanceDynamic::Create(
				GhostMaterialTemplate, this);
			Ghost->SetVectorParameterValue(TEXT("Color"),
				FLinearColor(Group.Color.R, Group.Color.G, Group.Color.B, 1.0f));
			Ghost->SetScalarParameterValue(TEXT("Opacity"), 1.0f);
			Ghost->SetScalarParameterValue(TEXT("FaceContrast"), 0.68f);
			Ghost->SetScalarParameterValue(TEXT("PixelSize"), 10.0f);
			Ghost->SetScalarParameterValue(TEXT("Roughness"), 0.82f);
			Ghost->SetScalarParameterValue(TEXT("ShadowLift"), 0.32f);
			Group.GhostMaterial = Ghost;
			HouseMaterials.Add(Ghost);
		}
	}
}

void AMatterFluxTwoStoreyHouseActor::ConfigureRampCollision()
{
	constexpr float Run = 660.0f;
	// Span the complete surface-to-surface rise.  The previous subtraction of
	// FloorThickness ended the ramp roughly one floor slab below its landing.
	constexpr float Rise = StoreyHeight;
	const float RampLength = FMath::Sqrt(Run * Run + Rise * Rise);
	const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(Rise, Run));
	StairRampCollision->SetBoxExtent(FVector(
		RampLength * 0.5f, 88.0f, 12.0f));
	StairRampCollision->SetRelativeLocation(FVector(
		0.0f, -245.0f, 34.0f + Rise * 0.5f - 6.0f));
	StairRampCollision->SetRelativeRotation(FRotator(Pitch, 0.0f, 0.0f));
}

bool AMatterFluxTwoStoreyHouseActor::IsInsideHouse(
	const FVector& WorldLocation) const
{
	const FVector Local = GetActorTransform().InverseTransformPosition(WorldLocation);
	return FMath::Abs(Local.X) <= HalfSizeX - InteriorMargin
		&& FMath::Abs(Local.Y) <= HalfSizeY - InteriorMargin
		&& Local.Z >= -20.0f
		&& Local.Z <= UpperFloorTop + WallHeight + 80.0f;
}

int32 AMatterFluxTwoStoreyHouseActor::GetFloorIndexAtWorldLocation(
	const FVector& WorldLocation) const
{
	const FVector Local = GetActorTransform().InverseTransformPosition(WorldLocation);
	if (FMath::Abs(Local.X) > HalfSizeX - InteriorMargin
		|| FMath::Abs(Local.Y) > HalfSizeY - InteriorMargin)
	{
		return INDEX_NONE;
	}
	const float GroundDistance = FMath::Abs(Local.Z - GroundFloorTop);
	const float UpperDistance = FMath::Abs(Local.Z - UpperFloorTop);
	if (GroundDistance <= 105.0f)
	{
		return 0;
	}
	if (UpperDistance <= 105.0f)
	{
		return 1;
	}
	return INDEX_NONE;
}

float AMatterFluxTwoStoreyHouseActor::GetFloorSurfaceWorldZ(
	const int32 FloorIndex) const
{
	const float LocalZ = FloorIndex <= 0 ? GroundFloorTop : UpperFloorTop;
	return GetActorTransform().TransformPosition(FVector(0.0f, 0.0f, LocalZ)).Z;
}

int32 AMatterFluxTwoStoreyHouseActor::GetIndoorPatrolWaypointCount() const
{
	return 8;
}

FVector AMatterFluxTwoStoreyHouseActor::GetIndoorPatrolWaypoint(
	const int32 WaypointIndex) const
{
	const FVector LocalWaypoints[] = {
		FVector(-300.0f, 180.0f, GroundFloorTop),
		FVector(-330.0f, -245.0f, GroundFloorTop),
		FVector(330.0f, -245.0f, UpperFloorTop),
		FVector(360.0f, 170.0f, UpperFloorTop),
		FVector(-270.0f, 170.0f, UpperFloorTop),
		FVector(330.0f, -245.0f, UpperFloorTop),
		FVector(-330.0f, -245.0f, GroundFloorTop),
		FVector(260.0f, 150.0f, GroundFloorTop)
	};
	const int32 Count = UE_ARRAY_COUNT(LocalWaypoints);
	const int32 Wrapped = ((WaypointIndex % Count) + Count) % Count;
	return GetActorTransform().TransformPosition(LocalWaypoints[Wrapped]);
}

ACharacter* AMatterFluxTwoStoreyHouseActor::ResolveLocalViewer() const
{
	if (CutawayViewerOverride.IsValid())
	{
		return CutawayViewerOverride.Get();
	}
	const UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator();
		It; ++It)
	{
		APlayerController* Controller = It->Get();
		if (Controller && Controller->IsLocalController())
		{
			return Cast<ACharacter>(Controller->GetPawn());
		}
	}
	return nullptr;
}

bool AMatterFluxTwoStoreyHouseActor::ResolveViewerCameraLocation(
	ACharacter& Viewer,
	FVector& OutCameraLocation) const
{
	if (APlayerController* Controller = Cast<APlayerController>(
		Viewer.GetController()))
	{
		FRotator ViewRotation;
		Controller->GetPlayerViewPoint(OutCameraLocation, ViewRotation);
		if (!OutCameraLocation.ContainsNaN())
		{
			return true;
		}
	}
	if (const UCameraComponent* Camera =
		Viewer.FindComponentByClass<UCameraComponent>())
	{
		OutCameraLocation = Camera->GetComponentLocation();
		return !OutCameraLocation.ContainsNaN();
	}
	return false;
}

void AMatterFluxTwoStoreyHouseActor::UpdateCutawayFloor(
	ACharacter* Viewer,
	const float DeltaSeconds)
{
	MatterFlux::MaterialCutaway::FResult Result;
	bool bResolved = false;
	if (Viewer)
	{
		FVector ViewerFeet = Viewer->GetActorLocation();
		if (const UCapsuleComponent* Capsule = Viewer->GetCapsuleComponent())
		{
			ViewerFeet.Z -= Capsule->GetScaledCapsuleHalfHeight();
		}
		CutawaySourceView.Reset(CuttableStructureSources.Num());
		for (AFragment2DSourceActor* Source : CuttableStructureSources)
		{
			CutawaySourceView.Add(Source);
		}
		if (IsInsideHouse(Viewer->GetActorLocation()))
		{
			bResolved = MatterFlux::MaterialCutaway::Resolve(
				ViewerFeet,
				CutawaySourceView,
				CurrentFloorSourceId,
				CutawayPolicy,
				Result);
		}
	}
	if (bResolved)
	{
		CurrentFloorSourceId = Result.FloorSourceId;
		CurrentCutawayFloor = Result.FloorOrdinal;
		CurrentGhostSourceIds = MoveTemp(Result.GhostSourceIds);
		bExteriorOcclusionActive = false;
		CutawayExitAccumulator = 0.0f;
		return;
	}

	if (CurrentCutawayFloor != INDEX_NONE)
	{
		CutawayExitAccumulator += FMath::Max(DeltaSeconds, 0.0f);
		if (CutawayExitAccumulator < CutawayExitGraceSeconds)
		{
			return;
		}
		CurrentCutawayFloor = INDEX_NONE;
		CurrentFloorSourceId.Invalidate();
		CurrentGhostSourceIds.Reset();
		CutawayExitAccumulator = 0.0f;
	}

	FVector CameraLocation;
	MatterFlux::MaterialCutaway::FResult ExteriorResult;
	bool bExteriorResolved = false;
	if (Viewer && ResolveViewerCameraLocation(*Viewer, CameraLocation))
	{
		const FVector ViewerCenter = Viewer->GetActorLocation();
		FVector ViewerExtent(44.0f, 44.0f, 88.0f);
		if (const UCapsuleComponent* Capsule = Viewer->GetCapsuleComponent())
		{
			ViewerExtent = FVector(
				Capsule->GetScaledCapsuleRadius(),
				Capsule->GetScaledCapsuleRadius(),
				Capsule->GetScaledCapsuleHalfHeight());
		}
		const FBox ViewerBounds(
			ViewerCenter - ViewerExtent,
			ViewerCenter + ViewerExtent);
		bExteriorResolved = MatterFlux::MaterialCutaway::ResolveOccludingWalls(
			CameraLocation,
			ViewerBounds,
			CutawaySourceView,
			CutawayPolicy,
			ExteriorResult);
	}
	if (bExteriorResolved)
	{
		CurrentFloorSourceId.Invalidate();
		CurrentCutawayFloor = INDEX_NONE;
		CurrentGhostSourceIds = MoveTemp(ExteriorResult.GhostSourceIds);
		bExteriorOcclusionActive = true;
		CutawayExitAccumulator = 0.0f;
		return;
	}

	if (!bExteriorOcclusionActive)
	{
		CutawayExitAccumulator = 0.0f;
		return;
	}

	CutawayExitAccumulator += FMath::Max(DeltaSeconds, 0.0f);
	if (CutawayExitAccumulator >= CutawayExitGraceSeconds)
	{
		CurrentGhostSourceIds.Reset();
		bExteriorOcclusionActive = false;
		CutawayExitAccumulator = 0.0f;
	}
}

void AMatterFluxTwoStoreyHouseActor::SetCutawayViewerOverride(
	ACharacter* Viewer)
{
	CutawayViewerOverride = Viewer;
}

void AMatterFluxTwoStoreyHouseActor::RefreshCutawayImmediately()
{
	ACharacter* Viewer = ResolveLocalViewer();
	CurrentCutawayFloor = INDEX_NONE;
	CurrentFloorSourceId.Invalidate();
	CurrentGhostSourceIds.Reset();
	bExteriorOcclusionActive = false;
	UpdateCutawayFloor(Viewer, CutawayExitGraceSeconds);
	// Resolve targets immediately but let opacity converge on subsequent ticks.
	UpdateStructureFade(0.0f);
}

void AMatterFluxTwoStoreyHouseActor::UpdateStructureFade(
	const float DeltaSeconds)
{
	float LowestOpacity = 1.0f;
	for (FStructureFadeGroup& Group : StructureFadeGroups)
	{
		UMeshComponent* Component = Group.Component.Get();
		if (!Component)
		{
			continue;
		}
		const bool bInteriorUpperFloor = !bExteriorOcclusionActive
			&& CurrentCutawayFloor != INDEX_NONE
			&& Group.StructuralRole
				== EMatterFluxMaterialStructuralRole::Floor
			&& Group.FloorTier > CurrentCutawayFloor;
		const bool bCanGhost = Group.StructuralRole
				== EMatterFluxMaterialStructuralRole::Wall
			|| (bExteriorOcclusionActive
				&& Group.StructuralRole
					== EMatterFluxMaterialStructuralRole::Floor);
		const bool bGhost = bInteriorUpperFloor
			|| (bCanGhost
				&& Group.SourceId.IsValid()
				&& CurrentGhostSourceIds.Contains(Group.SourceId));
		if (Group.bNeverFade && !bGhost)
		{
			continue;
		}
		UMaterialInstanceDynamic* Ghost = Group.GhostMaterial.Get();
		if (!Ghost)
		{
			// Never replace a missing fade material with a visibility pop.
			Component->SetVisibility(true, true);
			Group.CurrentOpacity = 1.0f;
			continue;
		}
		const float TargetOpacity = bGhost
			? MatterFlux::GhostFade::ResolveStructureTargetOpacity(
				Group.GhostOpacity)
			: 1.0f;
		const float MaximumSpeed = bGhost
			? MatterFlux::GhostFade::FadeToGhostSpeed
			: MatterFlux::GhostFade::FadeToSolidSpeed;
		const float NextOpacity = MatterFlux::GhostFade::AdvanceOpacity(
			Group.CurrentOpacity,
			TargetOpacity,
			DeltaSeconds,
			FMath::Min(StructureFadeSpeed, MaximumSpeed));
		if (bGhost || NextOpacity < 0.999f)
		{
			TArray<int32> TargetSlots = Group.MaterialSlots;
			if (TargetSlots.IsEmpty())
			{
				for (int32 SlotIndex = 0;
					SlotIndex < Component->GetNumMaterials(); ++SlotIndex)
				{
					TargetSlots.Add(SlotIndex);
				}
			}
			for (const int32 SlotIndex : TargetSlots)
			{
				Component->SetMaterial(SlotIndex, Ghost);
			}
			Ghost->SetScalarParameterValue(TEXT("Opacity"), NextOpacity);
			Ghost->SetVectorParameterValue(TEXT("Color"), FLinearColor(
				Group.Color.R, Group.Color.G, Group.Color.B, NextOpacity));
			Component->SetCastShadow(
				Component == CuttableWholeObjectMesh
					|| NextOpacity > 0.72f);
		}
		else if (!Group.SolidMaterials.IsEmpty())
		{
			for (int32 SlotIndex = 0;
				SlotIndex < Group.SolidMaterials.Num(); ++SlotIndex)
			{
				const int32 ComponentSlot = Group.MaterialSlots.IsEmpty()
					? SlotIndex
					: Group.MaterialSlots[SlotIndex];
				Component->SetMaterial(
					ComponentSlot,
					Group.SolidMaterials[SlotIndex].Get());
			}
			Component->SetCastShadow(true);
		}
		LowestOpacity = FMath::Min(LowestOpacity, NextOpacity);
		Group.CurrentOpacity = NextOpacity;
	}
	CurrentStructureOpacity = LowestOpacity;
	LowerFloorLight->SetVisibility(CurrentCutawayFloor != INDEX_NONE, true);
	UpperFloorLight->SetVisibility(CurrentCutawayFloor != INDEX_NONE, true);
}

float AMatterFluxTwoStoreyHouseActor::GetFloorSurfaceOpacity(
	const int32 FloorIndex) const
{
	float Opacity = 1.0f;
	for (const FStructureFadeGroup& Group : StructureFadeGroups)
	{
		if (Group.bFloorSurface && Group.FloorTier == FloorIndex)
		{
			Opacity = FMath::Min(Opacity, Group.CurrentOpacity);
		}
	}
	return Opacity;
}

float AMatterFluxTwoStoreyHouseActor::GetFurnitureOpacity(
	const int32 FloorIndex) const
{
	float Opacity = 1.0f;
	for (const FStructureFadeGroup& Group : StructureFadeGroups)
	{
		if (Group.bInteriorFixture && Group.FloorTier == FloorIndex)
		{
			Opacity = FMath::Min(Opacity, Group.CurrentOpacity);
		}
	}
	return Opacity;
}

int32 AMatterFluxTwoStoreyHouseActor::GetTrackedInteriorActorCount() const
{
	return 0;
}

AMatterFluxTwoStoreyHouseActor*
AMatterFluxTwoStoreyHouseActor::FindContainingHouse(
	const UWorld& World,
	const FVector& WorldLocation,
	const float ExtraMargin)
{
	for (TActorIterator<AMatterFluxTwoStoreyHouseActor> It(&World); It; ++It)
	{
		AMatterFluxTwoStoreyHouseActor* House = *It;
		if (!IsValid(House))
		{
			continue;
		}
		const FVector Local = House->GetActorTransform()
			.InverseTransformPosition(WorldLocation);
		if (FMath::Abs(Local.X) <= HalfSizeX + ExtraMargin
			&& FMath::Abs(Local.Y) <= HalfSizeY + ExtraMargin
			&& Local.Z >= -40.0f
			&& Local.Z <= UpperFloorTop + WallHeight + 100.0f)
		{
			return House;
		}
	}
	return nullptr;
}
