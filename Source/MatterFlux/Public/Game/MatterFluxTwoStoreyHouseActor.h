#pragma once

#include "CoreMinimal.h"
#include "Fragment/FragmentTypes.h"
#include "GameFramework/Actor.h"
#include "Rendering/MatterFluxMaterialCutaway.h"
#include "MatterFluxTwoStoreyHouseActor.generated.h"

class ACharacter;
class AFragment2DSourceActor;
class UBoxComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UMeshComponent;
class UPointLightComponent;
class UProceduralMeshComponent;
class USceneComponent;
class UStaticMesh;

/**
 * 可复用的双层体素房屋。
 *
 * Lua 选择有界生成能力并配置切面策略；Actor 负责生成和投影适配。
 * Floor/Wall/Furniture 身份以及切割后的 RuntimeMask 保存在 canonical
 * Source 中，房屋类和 Actor 包围盒不参与虚化事实判定。
 */
UCLASS()
class MATTERFLUX_API AMatterFluxTwoStoreyHouseActor : public AActor
{
	GENERATED_BODY()

public:
	AMatterFluxTwoStoreyHouseActor();

	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** 房屋在本地坐标中的完整占地半尺寸。 */
	static constexpr float HalfSizeX = 560.0f;
	static constexpr float HalfSizeY = 420.0f;
	static constexpr float StoreyHeight = 360.0f;

	/** 判断世界位置是否位于房屋室内空间。 */
	bool IsInsideHouse(const FVector& WorldLocation) const;

	/**
	 * 返回位置所在楼层：0 为一楼，1 为二楼，INDEX_NONE 为室外或不在地板上。
	 * 对 Character 使用脚底高度，因此胶囊中心不会被误判成二楼。
	 */
	int32 GetFloorIndexAtWorldLocation(const FVector& WorldLocation) const;

	/** 返回指定楼层地板表面的世界 Z。 */
	float GetFloorSurfaceWorldZ(int32 FloorIndex) const;

	/** 室内 AI 使用的闭环路线，包含上下楼梯两端。 */
	int32 GetIndoorPatrolWaypointCount() const;
	FVector GetIndoorPatrolWaypoint(int32 WaypointIndex) const;

	/** 当前本地切面楼层；INDEX_NONE 表示房屋完全实显。 */
	int32 GetCurrentCutawayFloor() const { return CurrentCutawayFloor; }
	float GetCurrentStructureOpacity() const { return CurrentStructureOpacity; }
	bool HasActiveStructureFade() const
	{
		return !CurrentGhostSourceIds.IsEmpty()
			|| CurrentStructureOpacity < 0.999f;
	}
	/** 返回地板表面当前本地显示透明度，便于 UI/测试核对切面语义。 */
	float GetFloorSurfaceOpacity(int32 FloorIndex) const;
	/** 返回指定楼层家具投影的最低透明度；没有家具时返回 1。 */
	float GetFurnitureOpacity(int32 FloorIndex) const;
	int32 GetTrackedInteriorActorCount() const;

	/** 自动测试与截图可显式指定观察者；传 nullptr 恢复本地玩家自动选择。 */
	void SetCutawayViewerOverride(ACharacter* Viewer);
	void RefreshCutawayImmediately();
	/** Selects the immutable Lua structure profile before FinishSpawning. */
	void InitializeStructureDefinition(FName DefinitionId);
	/** Hide and reset a streamed house while retaining its expensive component graph. */
	void DeactivateForStreamingPool();
	/** Reuse a pooled component graph at a new deterministic streamed location. */
	void ReactivateFromStreamingPool(
		const FTransform& WorldTransform,
		FName DefinitionId);

	/** 查找包含该位置的房屋，供通用生物控制器选择室内路线。 */
	static AMatterFluxTwoStoreyHouseActor* FindContainingHouse(
		const UWorld& World,
		const FVector& WorldLocation,
		float ExtraMargin = 0.0f);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "House")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "House|Collision")
	TObjectPtr<UBoxComponent> StairRampCollision;

private:
	struct FStructureFadeGroup
	{
		TWeakObjectPtr<UMeshComponent> Component;
		FLinearColor Color = FLinearColor::White;
		FName MaterialId = NAME_None;
		FGuid SourceId;
		EMatterFluxMaterialStructuralRole StructuralRole =
			EMatterFluxMaterialStructuralRole::None;
		int32 FloorTier = 0;
		bool bNeverFade = false;
		bool bFloorSurface = false;
		bool bCuttable = false;
		bool bInteriorFixture = false;
		/**
		 * 透明结构会在屏幕空间反复叠加；屋顶、墙、地板和家具必须
		 * 使用不同目标透明度，否则六层屋瓦叠在一起仍会完全遮住室内。
		 */
		float GhostOpacity = 0.12f;
		float CurrentOpacity = 1.0f;
		/** 为空表示控制组件全部材质槽；合并网格按楼层只控制指定槽。 */
		TArray<int32> MaterialSlots;
		TArray<TWeakObjectPtr<UMaterialInterface>> SolidMaterials;
		TWeakObjectPtr<UMaterialInstanceDynamic> GhostMaterial;
	};

	UInstancedStaticMeshComponent* CreateVoxelGroup(
		const FName Name,
		bool bEnableCollision,
		int32 FloorTier,
		const FLinearColor& Color,
		bool bNeverFade = false,
		bool bFloorSurface = false,
		float GhostOpacity = 0.12f,
		bool bCuttable = false,
		bool bInteriorFixture = false,
		FName MaterialId = NAME_None);
	void AddBox(
		UInstancedStaticMeshComponent& Group,
		const FVector& Center,
		const FVector& Size,
		const FRotator& Rotation = FRotator::ZeroRotator);
	void BuildHouseGeometry();
	void BuildFoundationAndFloors();
	void BuildWallsAndRoof();
	void BuildStairs();
	void BuildFurniture();
	void ConfigureGroupMaterials();
	void ConfigureRampCollision();
	void SpawnCuttableStructureSources();
	void RebuildCuttableWholeObjectMesh(bool bForce = false);
	uint32 ComputeCuttableWholeObjectSignature() const;
	void RefreshReplicatedCuttableStructureSources(float DeltaSeconds);
	void DestroyCuttableStructureSources();
	void MoveHouseAndCuttableSources(const FTransform& WorldTransform);
	ACharacter* ResolveLocalViewer() const;
	bool ResolveViewerCameraLocation(
		ACharacter& Viewer,
		FVector& OutCameraLocation) const;
	void UpdateCutawayFloor(ACharacter* Viewer, float DeltaSeconds);
	void UpdateStructureFade(float DeltaSeconds);

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SolidMaterialTemplate;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> GhostMaterialTemplate;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> VoxelGroups;

	/** 动态材质必须有强引用；尚未赋给组件的 ghost MID 也不能被 GC。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> HouseMaterials;

	/** 墙体、屋顶和家具使用真正的碎片 Source；HISM 只保留不可切结构。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AFragment2DSourceActor>> CuttableStructureSources;

	/** 房屋可切 Source 的统一表现；Source 自身只保留碰撞和伤害状态。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "House|Rendering")
	TObjectPtr<UProceduralMeshComponent> CuttableWholeObjectMesh;

	/** 合并网格重建时替换，避免每次切割永久积累 MID。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> CuttableWholeObjectMaterials;

	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> LowerFloorLight;

	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> UpperFloorLight;

	TArray<FStructureFadeGroup> StructureFadeGroups;
	TWeakObjectPtr<ACharacter> CutawayViewerOverride;
	FName StructureDefinitionId = TEXT("structure.house.two_storey");
	FDelegateHandle StructureReloadHandle;
	FGuid CurrentFloorSourceId;
	TSet<FGuid> CurrentGhostSourceIds;
	/** Reused adapter from GC-aware ownership to the resolver's read-only view. */
	TArray<AFragment2DSourceActor*> CutawaySourceView;
	MatterFlux::MaterialCutaway::FPolicy CutawayPolicy;
	float CutawayExitGraceSeconds = 0.18f;
	float StructureFadeSpeed = 4.5f;
	float WallGhostOpacity = 0.055f;
	float RoofGhostOpacity = 0.025f;
	int32 CurrentCutawayFloor = INDEX_NONE;
	float CutawayExitAccumulator = 0.0f;
	float CurrentStructureOpacity = 1.0f;
	float CuttableSourceDiscoveryAccumulator = 0.0f;
	bool bMaterialsConfigured = false;
	bool bExteriorOcclusionActive = false;
	uint32 CuttableWholeObjectSignature = MAX_uint32;
};
