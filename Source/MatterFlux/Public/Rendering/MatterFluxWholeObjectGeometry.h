#pragma once

#include "CoreMinimal.h"

namespace MatterFlux::WholeObject
{
	/** 体素面在整体物体中的语义角色，用于稳定地选择纹理和明暗策略。 */
	enum class EFaceRole : uint8
	{
		FrontBack,
		Side,
		Top,
		Bottom
	};

	/**
	 * 一个仍由 MatterFlux 二维 mask 管理的逻辑层。
	 * 编译器会把所有层投影到同一个三维格点中；调用方不需要了解
	 * 跨层遮挡、材料优先级或网格合并规则。
	 */
	struct MATTERFLUX_API FLayer
	{
		int32 MaterialIndex = INDEX_NONE;
		int32 Priority = 0;
		bool bEnableCollision = false;
		int32 Width = 0;
		int32 Height = 0;
		float CellSize = 0.0f;
		FTransform LocalTransform = FTransform::Identity;
		TArray<uint8> SolidMask;

		bool IsValid() const;
	};

	/** 一个材质和面角色对应一个稳定的 ProceduralMesh section。 */
	struct MATTERFLUX_API FMeshSection
	{
		int32 MaterialIndex = INDEX_NONE;
		EFaceRole FaceRole = EFaceRole::Side;
		bool bEnableCollision = false;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		/** 灰度值保存廉价体素角落 AO；材质可直接读取 Vertex Color。 */
		TArray<FColor> VertexColors;
	};

	struct MATTERFLUX_API FBuildResult
	{
		TArray<FMeshSection> Sections;
		FBox LocalBounds = FBox(ForceInit);
		int32 OccupiedCellCount = 0;
		int32 VisibleQuadCount = 0;

		void Reset();
	};

	/**
	 * 将若干 2.5D 逻辑层编译为一个整体三维体素网格。
	 *
	 * 保证：
	 * - 相同输入逐字段输出一致；
	 * - 同一格只保留最高优先级材料；
	 * - 所有材料之间统一剔除内部面；
	 * - 共面且材质/碰撞一致的面会确定性合并；
	 * - 失败时 OutResult 为空，不产生世界副作用。
	 */
	MATTERFLUX_API bool BuildMesh(
		const TArray<FLayer>& Layers,
		FBuildResult& OutResult,
		FString* OutError = nullptr);
}
