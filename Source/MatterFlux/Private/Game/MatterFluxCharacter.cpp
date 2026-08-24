#include "Game/MatterFluxCharacter.h"

#include "AbilitySystemComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include "Fragment/Fragment2DActor.h"
#include "Fragment/Fragment2DSourceActor.h"
#include "Game/MatterFluxPlayableWorldActor.h"
#include "Game/MatterFluxTwoStoreyHouseActor.h"
#include "Game/MatterFluxPlayerState.h"
#include "Game/MatterFluxCharacterPhysicsInteraction.h"
#include "Game/MatterFluxCharacterMovementComponent.h"
#include "GAS/GA_CastWand.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "MatterFluxLog.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Material/MatterFluxBuoyancyComponent.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/MatterFluxItemOcclusion.h"
#include "Rendering/MatterFluxGhostFade.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

AMatterFluxCharacter::AMatterFluxCharacter(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<
		UMatterFluxCharacterMovementComponent>(
			ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;
	// Occlusion selection remains cheap at 25 Hz while opacity transitions are
	// frequent enough to read as a continuous cutaway rather than a material pop.
	PrimaryActorTick.TickInterval = 0.04f;
	bReplicates = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	GetCapsuleComponent()->InitCapsuleSize(42.0f, 88.0f);

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->MaxWalkSpeed = 500.0f;
	Movement->JumpZVelocity = 720.0f;
	Movement->AirControl = 0.65f;
	Movement->GravityScale = 1.8f;
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.0f, 720.0f, 0.0f);
	Movement->bConstrainToPlane = false;
	MatterFlux::CharacterPhysics::ConfigurePhysicsInteraction(*Movement);
	BuoyancyComponent = CreateDefaultSubobject<UMatterFluxBuoyancyComponent>(
		TEXT("BuoyancyComponent"));
	BuoyancyComponent->SetBodyDensity(0.65f);
	WandCastRepeatTimers.SetNum(UGA_CastWand::EquipmentSlotCount);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CharacterMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> CharacterMaterial(
		TEXT("/Game/MatterFlux/Materials/M_VoxelPalette.M_VoxelPalette"));
	UStaticMesh* CharacterMeshAsset =
		CharacterMesh.Succeeded() ? CharacterMesh.Object : nullptr;
	UMaterialInterface* CharacterMaterialAsset =
		CharacterMaterial.Succeeded() ? CharacterMaterial.Object : nullptr;
	const auto CreateCharacterPart =
		[this, CharacterMeshAsset, CharacterMaterialAsset](
			const FName Name,
			const FVector& Location,
			const FVector& Scale)
		{
			UStaticMeshComponent* Part =
				CreateDefaultSubobject<UStaticMeshComponent>(Name);
			Part->SetupAttachment(GetCapsuleComponent());
			Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Part->SetCanEverAffectNavigation(false);
			Part->SetCastShadow(true);
			Part->SetRenderCustomDepth(true);
			Part->SetCustomDepthStencilValue(1);
			Part->SetRelativeLocation(Location);
			Part->SetRelativeScale3D(Scale);
			if (CharacterMeshAsset)
			{
				Part->SetStaticMesh(CharacterMeshAsset);
			}
			if (CharacterMaterialAsset)
			{
				Part->SetMaterial(0, CharacterMaterialAsset);
			}
			return Part;
		};
	CharacterVisual = CreateCharacterPart(
		TEXT("CharacterVisual"),
		FVector(0.0f, 0.0f, 4.0f),
		FVector(0.50f, 0.36f, 0.72f));
	HeadVisual = CreateCharacterPart(
		TEXT("HeadVisual"),
		FVector(0.0f, 0.0f, 64.0f),
		FVector(0.46f, 0.44f, 0.46f));
	LeftArmVisual = CreateCharacterPart(
		TEXT("LeftArmVisual"),
		FVector(0.0f, -29.0f, 4.0f),
		FVector(0.17f, 0.18f, 0.57f));
	RightArmVisual = CreateCharacterPart(
		TEXT("RightArmVisual"),
		FVector(0.0f, 29.0f, 4.0f),
		FVector(0.17f, 0.18f, 0.57f));
	LeftFootVisual = CreateCharacterPart(
		TEXT("LeftFootVisual"),
		FVector(9.0f, -14.0f, -55.0f),
		FVector(0.27f, 0.24f, 0.27f));
	RightFootVisual = CreateCharacterPart(
		TEXT("RightFootVisual"),
		FVector(9.0f, 14.0f, -55.0f),
		FVector(0.27f, 0.24f, 0.27f));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> OutlineMaterial(
		TEXT("/Game/MatterFlux/Materials/M_PlayerOutline.M_PlayerOutline"));
	if (OutlineMaterial.Succeeded())
	{
		PlayerOutlineMaterial = OutlineMaterial.Object;
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface>
		GhostOutlineMaterial(
			TEXT("/Game/MatterFlux/Materials/M_PlayerGhostOutline.M_PlayerGhostOutline"));
	if (GhostOutlineMaterial.Succeeded())
	{
		PlayerGhostOutlineMaterial = GhostOutlineMaterial.Object;
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GhostMaterial(
		TEXT("/Game/MatterFlux/Materials/M_VoxelGas.M_VoxelGas"));
	if (GhostMaterial.Succeeded())
	{
		ItemOcclusionGhostMaterial = GhostMaterial.Object;
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> EffectVoxelMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* EffectMesh = EffectVoxelMesh.Succeeded()
		? EffectVoxelMesh.Object
		: nullptr;
	auto CreateEffectComponent =
		[this, EffectMesh](
			const FName Name)
		{
			UInstancedStaticMeshComponent* Component =
				CreateDefaultSubobject<
					UInstancedStaticMeshComponent>(Name);
			Component->SetupAttachment(GetCapsuleComponent());
			Component->SetCollisionEnabled(
				ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
			if (EffectMesh)
			{
				Component->SetStaticMesh(EffectMesh);
			}
			return Component;
		};
	CutEffectInstances =
		CreateEffectComponent(TEXT("CutEffectInstances"));
	FlameEffectInstances =
		CreateEffectComponent(TEXT("FlameEffectInstances"));

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetCapsuleComponent());
	CameraBoom->TargetArmLength = 1480.0f;
	CameraBoom->SetRelativeRotation(FRotator(-45.0f, -45.0f, 0.0f));
	CameraBoom->TargetOffset = FVector(0.0f, 0.0f, 0.0f);
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritPitch = false;
	CameraBoom->bInheritYaw = false;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bDoCollisionTest = false;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 8.0f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	// 2.5D 指固定的斜俯视构图，不等于正交投影。保留受控的透视缩短，
	// 才能让树、房屋和地形具有正确的近大远小与体积层次。
	FollowCamera->ProjectionMode = ECameraProjectionMode::Perspective;
	FollowCamera->FieldOfView = 48.0f;
	FollowCamera->PostProcessSettings.bOverride_AutoExposureMethod = true;
	FollowCamera->PostProcessSettings.AutoExposureMethod = AEM_Manual;
	FollowCamera->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	FollowCamera->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
	FollowCamera->PostProcessSettings.bOverride_AutoExposureBias = true;
	FollowCamera->PostProcessSettings.AutoExposureBias = 0.0f;
	// TSR's temporal history and motion blur both soften or shimmer across the
	// one-cell silhouettes used by the terrain, foliage, and player outline.
	// The project uses FXAA, and the camera explicitly keeps motion blur off so
	// a saved scalability profile cannot reintroduce it while moving.
	FollowCamera->PostProcessSettings.bOverride_MotionBlurAmount = true;
	FollowCamera->PostProcessSettings.MotionBlurAmount = 0.0f;
	FollowCamera->PostProcessSettings.bOverride_AmbientOcclusionIntensity = true;
	FollowCamera->PostProcessSettings.AmbientOcclusionIntensity = 0.60f;
	FollowCamera->PostProcessSettings.bOverride_AmbientOcclusionRadius = true;
	FollowCamera->PostProcessSettings.AmbientOcclusionRadius = 28.0f;
	FollowCamera->PostProcessSettings.bOverride_BloomIntensity = true;
	FollowCamera->PostProcessSettings.BloomIntensity = 0.05f;
	FollowCamera->PostProcessSettings.bOverride_VignetteIntensity = true;
	FollowCamera->PostProcessSettings.VignetteIntensity = 0.06f;
	FollowCamera->PostProcessSettings.bOverride_ColorSaturation = true;
	FollowCamera->PostProcessSettings.ColorSaturation =
		FVector4(1.02f, 1.02f, 1.02f, 1.0f);
	FollowCamera->PostProcessSettings.bOverride_ColorContrast = true;
	FollowCamera->PostProcessSettings.ColorContrast =
		FVector4(1.08f, 1.08f, 1.08f, 1.0f);
}

void AMatterFluxCharacter::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateItemOcclusionGhosting(DeltaSeconds);
}

void AMatterFluxCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (PlayerOutlineMaterial && FollowCamera)
	{
		FollowCamera->PostProcessSettings.AddBlendable(
			PlayerOutlineMaterial,
			1.0f);
	}

	UMaterialInterface* BaseMaterial =
		CharacterVisual ? CharacterVisual->GetMaterial(0) : nullptr;
	const auto ApplyPartColor =
		[this, BaseMaterial](
			UStaticMeshComponent* Part,
			const FLinearColor& Color)
		{
			if (!Part || !BaseMaterial)
			{
				return;
			}
			UMaterialInstanceDynamic* Material =
				UMaterialInstanceDynamic::Create(BaseMaterial, this);
			Material->SetVectorParameterValue(TEXT("Color"), Color);
			Material->SetScalarParameterValue(TEXT("FaceContrast"), 0.88f);
			Material->SetScalarParameterValue(TEXT("ColorVariation"), 0.025f);
			Material->SetScalarParameterValue(TEXT("PixelSize"), 10.0f);
			Material->SetScalarParameterValue(TEXT("Roughness"), 0.84f);
			Material->SetScalarParameterValue(TEXT("ShadowLift"), 0.30f);
			Part->SetMaterial(0, Material);
		};
	ApplyPartColor(CharacterVisual, FLinearColor(0.86f, 0.10f, 0.025f));
	ApplyPartColor(HeadVisual, FLinearColor(1.0f, 0.45f, 0.08f));
	ApplyPartColor(LeftArmVisual, FLinearColor(0.95f, 0.21f, 0.035f));
	ApplyPartColor(RightArmVisual, FLinearColor(0.95f, 0.21f, 0.035f));
	ApplyPartColor(LeftFootVisual, FLinearColor(0.10f, 0.035f, 0.018f));
	ApplyPartColor(RightFootVisual, FLinearColor(0.10f, 0.035f, 0.018f));

	UMaterialInterface* EffectBase = BaseMaterial;
	CutEffectMaterial = EffectBase
		? UMaterialInstanceDynamic::Create(EffectBase, this)
		: nullptr;
	FlameEffectMaterial = EffectBase
		? UMaterialInstanceDynamic::Create(EffectBase, this)
		: nullptr;
	if (CutEffectMaterial && CutEffectInstances)
	{
		CutEffectMaterial->SetVectorParameterValue(
			TEXT("Color"),
			FLinearColor(0.2f, 0.9f, 1.0f));
		CutEffectInstances->SetMaterial(0, CutEffectMaterial);
	}
	if (FlameEffectMaterial && FlameEffectInstances)
	{
		FlameEffectMaterial->SetVectorParameterValue(
			TEXT("Color"),
			FLinearColor(1.0f, 0.16f, 0.01f));
		FlameEffectInstances->SetMaterial(
			0,
			FlameEffectMaterial);
	}
}

void AMatterFluxCharacter::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	RestoreItemOcclusionGhosts();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
		{
			It->UpdateLocalFragmentItemOcclusion(
				FVector::ZeroVector, FBox(ForceInit));
		}
	}
	if (ULocalPlayer* LocalPlayer =
		PlayableInputLocalPlayer.Get())
	{
		if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
			ULocalPlayer::GetSubsystem<
				UEnhancedInputLocalPlayerSubsystem>(
				LocalPlayer))
		{
			if (PlayableMappingContext)
			{
				InputSubsystem->RemoveMappingContext(
					PlayableMappingContext);
			}
		}
	}
	PlayableInputLocalPlayer.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CutEffectTimer);
		World->GetTimerManager().ClearTimer(FlameEffectTimer);
		for (FTimerHandle& Timer : WandCastRepeatTimers)
		{
			World->GetTimerManager().ClearTimer(Timer);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void AMatterFluxCharacter::UpdateItemOcclusionGhosting(
	const float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || !IsLocallyControlled() || !FollowCamera
		|| !GetCapsuleComponent())
	{
		RestoreItemOcclusionGhosts();
		SetGhostRevealOutlineEnabled(false);
		return;
	}
	for (TPair<TWeakObjectPtr<AActor>, FItemOcclusionGhostState>& Pair
		: ItemOcclusionGhostStates)
	{
		Pair.Value.bGhostDesired = false;
	}

	const FVector CameraLocation = FollowCamera->GetComponentLocation();
	const FVector ViewerCenter = GetCapsuleComponent()->GetComponentLocation();
	const FVector ViewerExtent(
		GetCapsuleComponent()->GetScaledCapsuleRadius(),
		GetCapsuleComponent()->GetScaledCapsuleRadius(),
		GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	const FBox ViewerBounds(ViewerCenter - ViewerExtent, ViewerCenter + ViewerExtent);
	bool bMergedItemGhostActive = false;
	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		bMergedItemGhostActive |= It->UpdateLocalFragmentItemOcclusion(
			CameraLocation, ViewerBounds);
	}

	TArray<MatterFlux::ItemOcclusion::FItem, TInlineAllocator<32>> Items;
	TMap<FGuid, TPair<TWeakObjectPtr<AActor>, FLinearColor>> ActorsByItemId;
	for (TActorIterator<AFragment2DSourceActor> It(World); It; ++It)
	{
		AFragment2DSourceActor* Source = *It;
		if (!IsValid(Source) || Source->bBroken
			|| Source->StructuralRole
				!= EMatterFluxMaterialStructuralRole::None
			|| !Source->SourceId.IsValid())
		{
			continue;
		}
		const FBox Bounds = Source->GetActiveWorldBounds();
		if (!Bounds.IsValid)
		{
			continue;
		}
		Items.Add({
			Source->SourceId,
			Source->AggregateId,
			Bounds,
			Source->GetCellSize()});
		ActorsByItemId.Add(
			Source->SourceId,
			TPair<TWeakObjectPtr<AActor>, FLinearColor>(
				Source, Source->FragmentColor));
	}
	for (TActorIterator<AFragment2DActor> It(World); It; ++It)
	{
		AFragment2DActor* Fragment = *It;
		if (!IsValid(Fragment) || !Fragment->SpawnPayload.FragmentId.IsValid())
		{
			continue;
		}
		const FBox Bounds = Fragment->MeshComponent
			? Fragment->MeshComponent->Bounds.GetBox()
			: FBox(ForceInit);
		if (!Bounds.IsValid)
		{
			continue;
		}
		const float CellSize = Fragment->SpawnPayload.DetachedVoxelMask.IsValid()
			? Fragment->SpawnPayload.DetachedVoxelMask.CellSize
			: FMath::Max(Fragment->SpawnPayload.Thickness, 1.0f);
		Items.Add({
			Fragment->SpawnPayload.FragmentId,
			Fragment->SpawnPayload.FragmentId,
			Bounds,
			CellSize});
		ActorsByItemId.Add(
			Fragment->SpawnPayload.FragmentId,
			TPair<TWeakObjectPtr<AActor>, FLinearColor>(
				Fragment, Fragment->FragmentColor));
	}

	MatterFlux::ItemOcclusion::FResult Result;
	MatterFlux::ItemOcclusion::Resolve(
		CameraLocation, ViewerBounds, Items, Result);
	for (const FGuid& ItemId : Result.GhostItemIds)
	{
		const TPair<TWeakObjectPtr<AActor>, FLinearColor>* Item =
			ActorsByItemId.Find(ItemId);
		AActor* Actor = Item ? Item->Key.Get() : nullptr;
		UMeshComponent* ItemMesh = nullptr;
		if (AFragment2DSourceActor* Source = Cast<AFragment2DSourceActor>(Actor))
		{
			ItemMesh = Source->MeshComponent.Get();
		}
		else if (AFragment2DActor* Fragment = Cast<AFragment2DActor>(Actor))
		{
			ItemMesh = Fragment->MeshComponent.Get();
		}
		if (Actor && ItemMesh)
		{
			ApplyItemOcclusionGhost(*Actor, *ItemMesh, Item->Value);
		}
	}

	bool bActorGhostActive = false;
	for (auto It = ItemOcclusionGhostStates.CreateIterator(); It; ++It)
	{
		FItemOcclusionGhostState& State = It.Value();
		UMeshComponent* ItemMesh = State.Mesh.Get();
		if (!ItemMesh)
		{
			It.RemoveCurrent();
			continue;
		}
		const int32 SlotCount = FMath::Min(
			State.SolidMaterials.Num(), State.GhostMaterials.Num());
		for (int32 Slot = 0; Slot < SlotCount; ++Slot)
		{
			UMaterialInterface* Ghost = State.GhostMaterials[Slot].Get();
			if (Ghost && ItemMesh->GetMaterial(Slot) != Ghost)
			{
				State.SolidMaterials[Slot] = ItemMesh->GetMaterial(Slot);
				ItemMesh->SetMaterial(Slot, Ghost);
			}
		}
		State.CurrentOpacity = MatterFlux::GhostFade::AdvanceItemOpacity(
			State.CurrentOpacity, State.bGhostDesired, DeltaSeconds);
		for (const TWeakObjectPtr<UMaterialInterface>& Material
			: State.GhostMaterials)
		{
			if (UMaterialInstanceDynamic* Ghost =
				Cast<UMaterialInstanceDynamic>(Material.Get()))
			{
				Ghost->SetScalarParameterValue(
					TEXT("Opacity"), State.CurrentOpacity);
			}
		}
		if (!State.bGhostDesired && State.CurrentOpacity >= 0.999f)
		{
			for (int32 Slot = 0; Slot < SlotCount; ++Slot)
			{
				if (ItemMesh->GetMaterial(Slot)
					== State.GhostMaterials[Slot].Get())
				{
					ItemMesh->SetMaterial(
						Slot, State.SolidMaterials[Slot].Get());
				}
			}
			It.RemoveCurrent();
			continue;
		}
		bActorGhostActive = true;
	}

	bool bHouseGhostActive = false;
	for (TActorIterator<AMatterFluxTwoStoreyHouseActor> It(World); It; ++It)
	{
		bHouseGhostActive |= It->HasActiveStructureFade();
	}
	SetGhostRevealOutlineEnabled(
		bActorGhostActive || bMergedItemGhostActive || bHouseGhostActive);
}

void AMatterFluxCharacter::ApplyItemOcclusionGhost(
	AActor& Actor,
	UMeshComponent& ItemMesh,
	const FLinearColor& Color)
{
	if (!ItemOcclusionGhostMaterial)
	{
		return;
	}
	const TWeakObjectPtr<AActor> ActorKey(&Actor);
	if (FItemOcclusionGhostState* Existing =
		ItemOcclusionGhostStates.Find(ActorKey))
	{
		Existing->bGhostDesired = true;
		if (Existing->Mesh.Get() == &ItemMesh)
		{
			const int32 ExistingSlotCount = FMath::Min(
				Existing->SolidMaterials.Num(),
				Existing->GhostMaterials.Num());
			for (int32 Slot = 0; Slot < ExistingSlotCount; ++Slot)
			{
				UMaterialInterface* Ghost = Existing->GhostMaterials[Slot].Get();
				if (Ghost && ItemMesh.GetMaterial(Slot) != Ghost)
				{
					// A reaction or mesh rebuild may replace its own material while
					// occluded. Preserve that newest solid state before reapplying.
					Existing->SolidMaterials[Slot] = ItemMesh.GetMaterial(Slot);
					ItemMesh.SetMaterial(Slot, Ghost);
				}
			}
		}
		return;
	}

	FItemOcclusionGhostState& State = ItemOcclusionGhostStates.Add(ActorKey);
	State.Mesh = &ItemMesh;
	State.bGhostDesired = true;
	State.CurrentOpacity = 1.0f;
	const int32 MaterialCount = ItemMesh.GetNumMaterials();
	State.SolidMaterials.Reserve(MaterialCount);
	State.GhostMaterials.Reserve(MaterialCount);
	for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
	{
		State.SolidMaterials.Add(ItemMesh.GetMaterial(Slot));
		UMaterialInstanceDynamic* Ghost = UMaterialInstanceDynamic::Create(
			ItemOcclusionGhostMaterial, this);
		Ghost->SetVectorParameterValue(
			TEXT("Color"), FLinearColor(Color.R, Color.G, Color.B, 1.0f));
		Ghost->SetScalarParameterValue(TEXT("Opacity"), State.CurrentOpacity);
		Ghost->SetScalarParameterValue(TEXT("FaceContrast"), 0.68f);
		Ghost->SetScalarParameterValue(TEXT("PixelSize"), 10.0f);
		Ghost->SetScalarParameterValue(TEXT("Roughness"), 0.82f);
		Ghost->SetScalarParameterValue(TEXT("ShadowLift"), 0.32f);
		State.GhostMaterials.Add(Ghost);
		ItemMesh.SetMaterial(Slot, Ghost);
	}
}

void AMatterFluxCharacter::RestoreItemOcclusionGhosts()
{
	for (TPair<TWeakObjectPtr<AActor>, FItemOcclusionGhostState>& Pair
		: ItemOcclusionGhostStates)
	{
		FItemOcclusionGhostState& State = Pair.Value;
		if (UMeshComponent* ItemMesh = State.Mesh.Get())
		{
			for (int32 Slot = 0; Slot < State.SolidMaterials.Num(); ++Slot)
			{
				if (ItemMesh->GetMaterial(Slot) == State.GhostMaterials[Slot].Get())
				{
					ItemMesh->SetMaterial(Slot, State.SolidMaterials[Slot].Get());
				}
			}
		}
	}
	ItemOcclusionGhostStates.Reset();
}

void AMatterFluxCharacter::SetGhostRevealOutlineEnabled(const bool bEnabled)
{
	if (bGhostRevealOutlineEnabled == bEnabled || !FollowCamera
		|| !PlayerGhostOutlineMaterial)
	{
		return;
	}
	bGhostRevealOutlineEnabled = bEnabled;
	if (bEnabled)
	{
		FollowCamera->PostProcessSettings.AddBlendable(
			PlayerGhostOutlineMaterial, 1.0f);
	}
	else
	{
		FollowCamera->PostProcessSettings.RemoveBlendable(
			PlayerGhostOutlineMaterial);
	}
}

UAbilitySystemComponent* AMatterFluxCharacter::GetAbilitySystemComponent() const
{
	const AMatterFluxPlayerState* MatterFluxPlayerState = GetPlayerState<AMatterFluxPlayerState>();
	return MatterFluxPlayerState ? MatterFluxPlayerState->GetAbilitySystemComponent() : nullptr;
}

void AMatterFluxCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	InitAbilityActorInfo();
}

void AMatterFluxCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	InitAbilityActorInfo();
}

void AMatterFluxCharacter::PawnClientRestart()
{
	Super::PawnClientRestart();
	InstallPlayableInputContext();

	if (IsLocallyControlled() && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			reinterpret_cast<uint64>(this),
			12.0f,
			FColor(120, 220, 255),
			TEXT("MatterFlux 2.5D  |  WASD 移动  Space 跳跃  滚轮缩放  左键/右键/Q/E 施放法杖  I/Tab 魔法工坊"));
	}
}

void AMatterFluxCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	EnsurePlayableInputAssets();

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput)
	{
		UE_LOG(LogMatterFlux, Warning, TEXT("%s requires EnhancedInputComponent for playable controls."), *GetName());
		return;
	}

	EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AMatterFluxCharacter::HandleMove);
	EnhancedInput->BindAction(MoveAction, ETriggerEvent::Completed, this, &AMatterFluxCharacter::HandleMoveCompleted);
	EnhancedInput->BindAction(JumpAction, ETriggerEvent::Started, this, &AMatterFluxCharacter::HandleJumpStarted);
	EnhancedInput->BindAction(JumpAction, ETriggerEvent::Completed, this, &AMatterFluxCharacter::HandleJumpCompleted);
	EnhancedInput->BindAction(CameraZoomAction, ETriggerEvent::Triggered, this, &AMatterFluxCharacter::HandleCameraZoom);
	for (int32 Slot = 0; Slot < CastWandActions.Num(); ++Slot)
	{
		EnhancedInput->BindAction(
			CastWandActions[Slot],
			ETriggerEvent::Started,
			this,
			&AMatterFluxCharacter::HandleCastWandStarted,
			Slot);
		EnhancedInput->BindAction(
			CastWandActions[Slot],
			ETriggerEvent::Completed,
			this,
			&AMatterFluxCharacter::HandleCastWandStopped,
			Slot);
		EnhancedInput->BindAction(
			CastWandActions[Slot],
			ETriggerEvent::Canceled,
			this,
			&AMatterFluxCharacter::HandleCastWandStopped,
			Slot);
	}
	EnhancedInput->BindAction(RegenerateAction, ETriggerEvent::Started, this, &AMatterFluxCharacter::HandleRegenerateRequested);
}

void AMatterFluxCharacter::InitAbilityActorInfo()
{
	AMatterFluxPlayerState* MatterFluxPlayerState = GetPlayerState<AMatterFluxPlayerState>();
	UAbilitySystemComponent* ASC = MatterFluxPlayerState ? MatterFluxPlayerState->GetAbilitySystemComponent() : nullptr;
	if (ASC)
	{
		ASC->InitAbilityActorInfo(MatterFluxPlayerState, this);
		if (HasAuthority())
		{
			MatterFluxPlayerState->GrantDefaultAbilities();
		}
	}
}

void AMatterFluxCharacter::EnsurePlayableInputAssets()
{
	if (PlayableMappingContext)
	{
		return;
	}

	PlayableMappingContext = NewObject<UInputMappingContext>(this, TEXT("Runtime_IMC_Playable"));
	MoveAction = NewObject<UInputAction>(this, TEXT("Runtime_IA_Move"));
	JumpAction = NewObject<UInputAction>(this, TEXT("Runtime_IA_Jump"));
	CameraZoomAction = NewObject<UInputAction>(this, TEXT("Runtime_IA_CameraZoom"));
	CastWandActions.Reset(UGA_CastWand::EquipmentSlotCount);
	for (int32 Slot = 0; Slot < UGA_CastWand::EquipmentSlotCount; ++Slot)
	{
		CastWandActions.Add(NewObject<UInputAction>(
			this,
			*FString::Printf(TEXT("Runtime_IA_CastWand_%d"), Slot)));
	}
	RegenerateAction = NewObject<UInputAction>(this, TEXT("Runtime_IA_Regenerate"));

	MoveAction->ValueType = EInputActionValueType::Axis2D;
	JumpAction->ValueType = EInputActionValueType::Boolean;
	CameraZoomAction->ValueType = EInputActionValueType::Axis1D;
	for (UInputAction* Action : CastWandActions)
	{
		Action->ValueType = EInputActionValueType::Boolean;
	}
	RegenerateAction->ValueType = EInputActionValueType::Boolean;

	auto MapNegativeKey = [this](UInputAction* Action, const FKey Key)
	{
		FEnhancedActionKeyMapping& Mapping = PlayableMappingContext->MapKey(Action, Key);
		Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(PlayableMappingContext));
	};

	MapNegativeKey(MoveAction, EKeys::A);
	MapNegativeKey(MoveAction, EKeys::Left);
	PlayableMappingContext->MapKey(MoveAction, EKeys::D);
	PlayableMappingContext->MapKey(MoveAction, EKeys::Right);
	PlayableMappingContext->MapKey(MoveAction, EKeys::Gamepad_LeftX);

	auto MapYKey = [this](const FKey Key, const bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = PlayableMappingContext->MapKey(MoveAction, Key);
		UInputModifierSwizzleAxis* Swizzle =
			NewObject<UInputModifierSwizzleAxis>(PlayableMappingContext);
		Swizzle->Order = EInputAxisSwizzle::YXZ;
		Mapping.Modifiers.Add(Swizzle);
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(PlayableMappingContext));
		}
	};
	MapYKey(EKeys::W, false);
	MapYKey(EKeys::Up, false);
	MapYKey(EKeys::S, true);
	MapYKey(EKeys::Down, true);
	MapYKey(EKeys::Gamepad_LeftY, false);

	PlayableMappingContext->MapKey(JumpAction, EKeys::SpaceBar);
	PlayableMappingContext->MapKey(JumpAction, EKeys::Gamepad_FaceButton_Bottom);

	PlayableMappingContext->MapKey(CameraZoomAction, EKeys::MouseWheelAxis);

	PlayableMappingContext->MapKey(CastWandActions[0], EKeys::LeftMouseButton);
	PlayableMappingContext->MapKey(CastWandActions[0], EKeys::Gamepad_RightShoulder);
	PlayableMappingContext->MapKey(CastWandActions[1], EKeys::RightMouseButton);
	PlayableMappingContext->MapKey(CastWandActions[1], EKeys::Gamepad_RightTrigger);
	PlayableMappingContext->MapKey(CastWandActions[2], EKeys::Q);
	PlayableMappingContext->MapKey(CastWandActions[2], EKeys::Gamepad_LeftShoulder);
	PlayableMappingContext->MapKey(CastWandActions[3], EKeys::E);
	PlayableMappingContext->MapKey(CastWandActions[3], EKeys::Gamepad_LeftTrigger);
	PlayableMappingContext->MapKey(RegenerateAction, EKeys::R);
}

void AMatterFluxCharacter::InstallPlayableInputContext()
{
	if (!IsLocallyControlled())
	{
		return;
	}

	EnsurePlayableInputAssets();
	const APlayerController* PlayerController =
		Cast<APlayerController>(GetController());
	ULocalPlayer* LocalPlayer =
		PlayerController ? PlayerController->GetLocalPlayer() : nullptr;
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LocalPlayer
		? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer)
		: nullptr;
	if (!InputSubsystem)
	{
		return;
	}

	if (ULocalPlayer* PreviousLocalPlayer =
		PlayableInputLocalPlayer.Get();
		PreviousLocalPlayer && PreviousLocalPlayer != LocalPlayer)
	{
		if (UEnhancedInputLocalPlayerSubsystem* PreviousSubsystem =
			ULocalPlayer::GetSubsystem<
				UEnhancedInputLocalPlayerSubsystem>(
				PreviousLocalPlayer))
		{
			PreviousSubsystem->RemoveMappingContext(
				PlayableMappingContext);
		}
	}
	InputSubsystem->RemoveMappingContext(PlayableMappingContext);
	InputSubsystem->AddMappingContext(PlayableMappingContext, 0);
	PlayableInputLocalPlayer = LocalPlayer;
}

void AMatterFluxCharacter::HandleMove(const FInputActionValue& Value)
{
	const FVector2D MovementInput = Value.Get<FVector2D>();
	PublishPlayerOperation(
		EMatterFluxPlayerOperation::Move,
		MovementInput);
	if (!FollowCamera || MovementInput.IsNearlyZero())
	{
		return;
	}

	FVector CameraRight = FollowCamera->GetRightVector();
	CameraRight.Z = 0.0f;
	CameraRight.Normalize();
	FVector CameraForward = FollowCamera->GetForwardVector();
	CameraForward.Z = 0.0f;
	CameraForward.Normalize();

	AddMovementInput(CameraRight, MovementInput.X);
	AddMovementInput(CameraForward, MovementInput.Y);
}

void AMatterFluxCharacter::HandleMoveCompleted()
{
	PublishPlayerOperation(
		EMatterFluxPlayerOperation::Move,
		FVector2D::ZeroVector);
}

void AMatterFluxCharacter::HandleCameraZoom(const FInputActionValue& Value)
{
	PublishPlayerOperation(
		EMatterFluxPlayerOperation::CameraZoom,
		FVector2D(Value.Get<float>(), 0.0f));
	if (CameraBoom)
	{
		CameraBoom->TargetArmLength = FMath::Clamp(
			CameraBoom->TargetArmLength - Value.Get<float>() * 120.0f,
			800.0f,
			1800.0f);
	}
}

void AMatterFluxCharacter::HandleJumpStarted()
{
	PublishPlayerOperation(
		EMatterFluxPlayerOperation::JumpStarted);
	Jump();
}

void AMatterFluxCharacter::HandleJumpCompleted()
{
	PublishPlayerOperation(
		EMatterFluxPlayerOperation::JumpCompleted);
	StopJumping();
}

void AMatterFluxCharacter::HandleCastWandStarted(
	const int32 EquipmentSlot)
{
	HandleCastWandRequested(EquipmentSlot);
	if (!WandCastRepeatTimers.IsValidIndex(EquipmentSlot)
		|| !GetWorld())
	{
		return;
	}

	FTimerDelegate RepeatCast;
	RepeatCast.BindUObject(
		this,
		&AMatterFluxCharacter::HandleCastWandRequested,
		EquipmentSlot);
	GetWorld()->GetTimerManager().SetTimer(
		WandCastRepeatTimers[EquipmentSlot],
		RepeatCast,
		0.05f,
		true,
		0.05f);
}

void AMatterFluxCharacter::HandleCastWandStopped(
	const int32 EquipmentSlot)
{
	if (WandCastRepeatTimers.IsValidIndex(EquipmentSlot)
		&& GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(
			WandCastRepeatTimers[EquipmentSlot]);
	}
}

void AMatterFluxCharacter::HandleCastWandRequested(
	const int32 EquipmentSlot)
{
	PublishPlayerOperation(
		EMatterFluxPlayerOperation::CastWand,
		FVector2D::ZeroVector,
		EquipmentSlot);
	TryActivateWandSlot(EquipmentSlot);
}

void AMatterFluxCharacter::HandleRegenerateRequested()
{
	if (HasAuthority())
	{
		ExecuteRegenerateRequest();
	}
	else
	{
		ServerRegenerateLevel();
	}
}

void AMatterFluxCharacter::ApplyPlayerOperation(
	const EMatterFluxPlayerOperation Operation,
	const FVector2D& Value,
	const int32 IntegerValue)
{
	switch (Operation)
	{
	case EMatterFluxPlayerOperation::Move:
		HandleMove(FInputActionValue(Value));
		break;
	case EMatterFluxPlayerOperation::JumpStarted:
		HandleJumpStarted();
		break;
	case EMatterFluxPlayerOperation::JumpCompleted:
		HandleJumpCompleted();
		break;
	case EMatterFluxPlayerOperation::CameraZoom:
		HandleCameraZoom(FInputActionValue(static_cast<float>(Value.X)));
		break;
	case EMatterFluxPlayerOperation::Cut:
		HandleCastWandRequested(0);
		break;
	case EMatterFluxPlayerOperation::Flame:
		HandleCastWandRequested(1);
		break;
	case EMatterFluxPlayerOperation::Regenerate:
		ExecuteRegenerateRequest(IntegerValue);
		break;
	case EMatterFluxPlayerOperation::CastWand:
		HandleCastWandRequested(IntegerValue);
		break;
	default:
		break;
	}
}

void AMatterFluxCharacter::PublishPlayerOperation(
	const EMatterFluxPlayerOperation Operation,
	const FVector2D& Value,
	const int32 IntegerValue)
{
	MatterFlux::PlayerOperations::OnApplied().Broadcast(
		*this,
		Operation,
		Value,
		IntegerValue,
		false);
}

void AMatterFluxCharacter::RelayPlayerOperationToServer(
	const EMatterFluxPlayerOperation Operation,
	const FVector2D& Value,
	const int32 IntegerValue)
{
	if (!HasAuthority())
	{
		ServerRelayPlayerOperation(
			static_cast<uint8>(Operation),
			Value,
			IntegerValue);
	}
}

bool AMatterFluxCharacter::ServerRelayPlayerOperation_Validate(
	const uint8 Operation,
	const FVector2D Value,
	const int32 IntegerValue)
{
	const bool bValidOperation =
		Operation <= static_cast<uint8>(
			EMatterFluxPlayerOperation::CastWand)
		&& !Value.ContainsNaN()
		&& FMath::Abs(Value.X) <= 100.0
		&& FMath::Abs(Value.Y) <= 100.0;
	const bool bValidInteger =
		(Operation != static_cast<uint8>(
			EMatterFluxPlayerOperation::Regenerate)
			|| IntegerValue >= 0)
		&& (Operation != static_cast<uint8>(
			EMatterFluxPlayerOperation::CastWand)
			|| (IntegerValue >= 0
				&& IntegerValue < UGA_CastWand::EquipmentSlotCount));
	return bValidOperation && bValidInteger;
}

void AMatterFluxCharacter::ServerRelayPlayerOperation_Implementation(
	const uint8 Operation,
	const FVector2D Value,
	const int32 IntegerValue)
{
	MatterFlux::PlayerOperations::OnApplied().Broadcast(
		*this,
		static_cast<EMatterFluxPlayerOperation>(Operation),
		Value,
		IntegerValue,
		true);
}

void AMatterFluxCharacter::TryActivateWandSlot(
	const int32 EquipmentSlot)
{
	if (EquipmentSlot < 0
		|| EquipmentSlot >= UGA_CastWand::EquipmentSlotCount)
	{
		return;
	}
	if (!HasAuthority())
	{
		ServerActivateWandSlot(EquipmentSlot);
		return;
	}

	UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC)
	{
		return;
	}
	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		if (Spec.InputID == EquipmentSlot
			&& Spec.Ability
			&& Spec.Ability->IsA<UGA_CastWand>())
		{
			ASC->TryActivateAbility(Spec.Handle, true);
			return;
		}
	}
}

bool AMatterFluxCharacter::ServerActivateWandSlot_Validate(
	const int32 EquipmentSlot)
{
	return EquipmentSlot >= 0
		&& EquipmentSlot < UGA_CastWand::EquipmentSlotCount;
}

void AMatterFluxCharacter::ServerActivateWandSlot_Implementation(
	const int32 EquipmentSlot)
{
	TryActivateWandSlot(EquipmentSlot);
}

void AMatterFluxCharacter::ServerRegenerateLevel_Implementation()
{
	// World regeneration is a session-wide administrative action. A remote
	// owning client may invoke this RPC on its pawn, but only the locally
	// controlled listen host is allowed to perform the action.
	if (!IsLocallyControlled())
	{
		UE_LOG(
			LogMatterFlux,
			Warning,
			TEXT("Rejected remote level regeneration request from %s."),
			*GetName());
		return;
	}
	ExecuteRegenerateRequest();
}

void AMatterFluxCharacter::ExecuteRegenerateRequest(
	const int32 RequestedSeed)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || !World)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	if (LastRegenerateRequestTime >= 0.0 && Now - LastRegenerateRequestTime < 0.5)
	{
		return;
	}
	LastRegenerateRequestTime = Now;

	for (TActorIterator<AMatterFluxPlayableWorldActor> It(World); It; ++It)
	{
		It->Regenerate(RequestedSeed);
		PublishPlayerOperation(
			EMatterFluxPlayerOperation::Regenerate,
			FVector2D::ZeroVector,
			It->GetMapSeed());
		return;
	}
}

void AMatterFluxCharacter::BroadcastAbilityEffect(
	const EMatterFluxPlayerAbilityEffect Effect)
{
	if (HasAuthority())
	{
		MulticastPlayAbilityEffect(Effect);
	}
}

void AMatterFluxCharacter::MulticastPlayAbilityEffect_Implementation(
	const EMatterFluxPlayerAbilityEffect Effect)
{
	PlayAbilityEffect(Effect);
}

void AMatterFluxCharacter::PlayAbilityEffect(
	const EMatterFluxPlayerAbilityEffect Effect)
{
	UWorld* World = GetWorld();
	if (!World || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	if (Effect == EMatterFluxPlayerAbilityEffect::Cut)
	{
		ClearCutEffect();
		if (CutEffectInstances)
		{
			TArray<FTransform> Transforms;
			BuildAbilityEffectTransforms(Effect, Transforms);
			for (const FTransform& Transform : Transforms)
			{
				CutEffectInstances->AddInstance(Transform);
			}
		}
		World->GetTimerManager().SetTimer(
			CutEffectTimer,
			this,
			&AMatterFluxCharacter::ClearCutEffect,
			0.14f,
			false);
		return;
	}

	ClearFlameEffect();
	if (FlameEffectInstances)
	{
		TArray<FTransform> Transforms;
		BuildAbilityEffectTransforms(Effect, Transforms);
		for (const FTransform& Transform : Transforms)
		{
			FlameEffectInstances->AddInstance(Transform);
		}
	}
	World->GetTimerManager().SetTimer(
		FlameEffectTimer,
		this,
		&AMatterFluxCharacter::ClearFlameEffect,
		0.18f,
		false);
}

void AMatterFluxCharacter::BuildAbilityEffectTransforms(
	const EMatterFluxPlayerAbilityEffect Effect,
	TArray<FTransform>& OutTransforms)
{
	OutTransforms.Reset();
	if (Effect == EMatterFluxPlayerAbilityEffect::Cut)
	{
		OutTransforms.Reserve(12);
		for (int32 Index = 0; Index < 12; ++Index)
		{
			const float Alpha =
				static_cast<float>(Index) / 11.0f;
			OutTransforms.Emplace(
				FRotator(
					0.0f,
					0.0f,
					-25.0f + Alpha * 50.0f),
				FVector(
					95.0f + Alpha * 780.0f,
					0.0f,
					25.0f),
				FVector(0.22f, 0.035f, 0.035f));
		}
		return;
	}

	OutTransforms.Reserve(24);
	FRandomStream Random(8831);
	for (int32 Index = 0; Index < 24; ++Index)
	{
		const float Alpha =
			static_cast<float>(Index) / 23.0f;
		const float Spread =
			FMath::Lerp(18.0f, 145.0f, Alpha);
		const float Size =
			FMath::Lerp(0.18f, 0.42f, Alpha);
		OutTransforms.Emplace(
			FRotator::ZeroRotator,
			FVector(
				75.0f + Alpha * 720.0f,
				Random.FRandRange(-Spread, Spread),
				25.0f
					+ Random.FRandRange(
						-Spread * 0.35f,
						Spread * 0.5f)),
			FVector(Size * 1.35f, Size, Size));
	}
}

void AMatterFluxCharacter::ClearCutEffect()
{
	if (CutEffectInstances)
	{
		CutEffectInstances->ClearInstances();
	}
}

void AMatterFluxCharacter::ClearFlameEffect()
{
	if (FlameEffectInstances)
	{
		FlameEffectInstances->ClearInstances();
	}
}
