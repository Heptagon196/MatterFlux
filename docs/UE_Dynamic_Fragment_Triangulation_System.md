# UE Dynamic Fragment / Triangle Body 系统设计文档

> **架构背景文档**
>
> 本文解释系统的长期设计方向，不保证示例接口与 0.2.0 源码逐字段一致。
> 当前实现、网络行为、测试和构建方式请阅读
> `MatterFlux_UE_Beginner_Guide.md`；发布状态以
> `MatterFlux_0.2.0_Release_Audit.md` 为准。

## 1. 系统目标

构建一套独立于世界主存储、但由服务器权威驱动的动态碎片系统，用于处理：

- 预设 2D 可破坏物体
- 地形或结构被打断后脱离世界的实体
- 爆炸、挖掘、火焰烧蚀后产生的大块动态固体
- 可以被 Chaos 物理模拟、碰撞、休眠和回收的 fragment actor

该系统的目标不是替代 Segment Column 世界模型，而是在需要“可运动大块实体”时，从 Segment Column 世界中剥离出临时的三角网格刚体表达。

------------------------------------------------------------------------

## 2. 与 Segment Column 的边界

### Segment Column 负责

- 世界主数据
- 固体、液体、火焰、材料反应
- Chunk Diff 生成
- 持久化
- 服务器权威模拟

### Dynamic Fragment 负责

- 脱离世界的固体块
- 预设 2D 物体的运行时破坏
- 三角网格渲染
- 简化碰撞体
- Chaos 刚体模拟
- 休眠、销毁或回写世界

### 禁止

- 不把整个世界主结构改成三角网格
- 不让客户端决定 fragment 的断裂结果
- 不用高精度三角碰撞承载大量动态碎片
- 不让 GAS 直接修改 fragment 几何

------------------------------------------------------------------------

## 3. 核心数据模型

### Fragment2DActor

```cpp
struct FFragment2DState
{
    FGuid FragmentId;
    int32 Revision;

    TArray<FIntPoint> SourceChunkCoords;
    FBox2D LocalBounds;

    FFragmentSolidMask SolidMask;
    TArray<FFragmentContour> OuterContours;
    TArray<FFragmentContour> HoleContours;
    TArray<FFragmentTriangle> RenderTriangles;
    TArray<FFragmentConvexHull> CollisionHulls;

    TArray<FFragmentMaterialRegion> MaterialRegions;

    float Mass;
    FVector2D CenterOfMass2D;
    float Inertia2D;

    FTransform Transform;
    FVector LinearVelocity;
    FVector AngularVelocity;

    EFragmentLifecycleState LifecycleState;
};
```

### 生命周期状态

- Building：后台生成轮廓、三角形和碰撞体
- Dynamic：参与服务器物理模拟
- Sleeping：物理休眠，但仍作为 actor 存在
- Frozen：远距离或低价值 fragment，只保留静态状态
- Merging：正在回写 Segment Column
- Destroyed：被销毁或被吸收到世界

------------------------------------------------------------------------

## 4. 预设 2D 物体导入

预设 2D 物体不应只保存为静态网格。它需要同时保存可破坏数据：

- 初始 solid mask
- 材料区域
- 轮廓或源多边形
- 断裂约束
- 锚点
- 最大 fragment 数量
- 最小有效碎片面积

推荐资源形式：

```text
UFragment2DAsset
  SourceTexture / SourcePolygon
  MaterialMap
  AnchorMap
  DamageRules
  TriangulationSettings
  CollisionSettings
```

运行时由服务器实例化为 `Fragment2DActor`。客户端只接收 spawn event 和重建所需的确定性数据。

------------------------------------------------------------------------

## 5. 地形断裂流程

当爆炸、挖掘或其他破坏事件修改 Chunk 后，服务器执行：

1. 标记 dirty columns。
2. 在受影响 Chunk 和邻接 Chunk 上做 solid 连通域检测。
3. 判断每个 solid region 是否仍连接到世界锚点。
4. 保留锚定 region 在 Segment Column 世界内。
5. 将脱离锚点的 region detach 成 fragment。
6. 从 Segment Column 中移除 detached region，生成 ChunkDiff。
7. 为 detached region 创建 Fragment2DActor。

### 锚点判断

锚点可以来自：

- Chunk 边界连接
- 世界静态根结构
- 设计师标记的不可脱离区域
- 超大质量区域
- 当前预算不允许剥离的区域

锚点判断必须由服务器执行，并受到面积、质量、Chunk 边界连接和模拟预算共同约束。

------------------------------------------------------------------------

## 6. 轮廓提取

输入可以是：

- tile solid mask
- Segment Column 投影后的 2D solid mask
- 预设物体的多边形源数据

处理流程：

```text
Solid Mask
→ Marching Squares / Boundary Tracing
→ Outer Contours
→ Hole Contours
→ Contour Cleanup
→ Simplification
```

### 轮廓清理

必须处理：

- 重复点
- 极短边
- 共线点
- 自交
- 过窄通道
- 面积过小的洞
- 材料边界造成的碎裂噪声

### 简化策略

使用 Ramer-Douglas-Peucker 或类似算法降低顶点数。简化阈值需要按用途区分：

- 渲染轮廓：较精细
- 碰撞轮廓：更粗糙
- 网络轮廓：优先 compact

------------------------------------------------------------------------

## 7. 三角剖分策略

三角剖分用于渲染，不直接等同于物理碰撞。

推荐流程：

```text
Outer Contour + Hole Contours
→ Constrained Delaunay Triangulation
→ Material Region Assignment
→ UV Generation
→ Normal / Tangent Generation
→ Procedural Mesh Section
```

### 实现建议

优先使用成熟几何库或 UE GeometryProcessing / DynamicMesh 相关能力，不建议手写通用 polygon triangulator。

必须支持：

- 带洞多边形
- 多材料区域
- 稳定顶点排序
- 确定性输出
- 退化三角形剔除

------------------------------------------------------------------------

## 8. 碰撞体生成

动态 fragment 不应默认使用复杂三角碰撞。

推荐策略：

- 小 fragment：单个 convex hull
- 中等 fragment：多个 convex hull
- 大 fragment：粗轮廓分解后生成少量 hull
- 低价值 fragment：只用 box / capsule / simplified hull

### UE 侧表达

渲染可以使用 ProceduralMeshComponent 或 DynamicMeshComponent。

碰撞需要优先生成 simple collision。`UseComplexAsSimple` 只允许用于静态、低频或调试场景。

------------------------------------------------------------------------

## 9. 服务器权威物理

所有 fragment 的权威生命周期由 Dedicated Server 决定。

服务器负责：

- fragment spawn
- damage application
- split
- merge
- destroy
- initial impulse
- sleep / wake
- transform correction

客户端可以做视觉预测，但不能提交权威断裂结果。

------------------------------------------------------------------------

## 10. 网络同步

不复制完整动态世界。只同步事件、revision 和必要的 compact geometry 数据。

### FragmentSpawn

```cpp
struct FFragmentSpawnEvent
{
    FGuid FragmentId;
    int32 Revision;
    TArray<FIntPoint> SourceChunkCoords;
    FFragmentShapePayload ShapePayload;
    FTransform InitialTransform;
    FVector InitialLinearVelocity;
    FVector InitialAngularVelocity;
};
```

### FragmentDamage

```cpp
struct FFragmentDamageEvent
{
    FGuid FragmentId;
    int32 BaseRevision;
    FFragmentDamageShape DamageShape;
    int32 EventSeed;
};
```

### FragmentSplit

```cpp
struct FFragmentSplitEvent
{
    FGuid OldFragmentId;
    int32 OldRevision;
    TArray<FFragmentSpawnEvent> NewFragments;
};
```

### FragmentMergeToWorld

```cpp
struct FFragmentMergeToWorldEvent
{
    FGuid FragmentId;
    int32 FinalRevision;
    TArray<FChunkDiff> ChunkDiffs;
};
```

------------------------------------------------------------------------

## 11. 回写世界

fragment 静止后有三种处理方式：

### 保留为 actor

适合仍需要碰撞、被再次破坏、被玩家推动或保留独立身份的 fragment。

### 冻结为静态 fragment

适合远距离碎片或低交互价值碎片。保留视觉和简单碰撞，不再高频模拟。

### 回写 Segment Column

适合需要继续参与流体、火焰、挖掘和持久化的碎片。流程是：

```text
Fragment Mesh / Mask
→ Transform 到世界空间
→ Rasterize / Segmentize
→ 写入 Chunk
→ 生成 ChunkDiff
→ 销毁 Fragment2DActor
```

回写策略由交互需求、持久化需求和服务器预算共同决定。

------------------------------------------------------------------------

## 12. 性能预算

必须设硬预算：

- 每 tick 最大 detach region 数
- 每 tick 最大 triangulation job 数
- 最大动态 fragment 数量
- 最大单个 fragment 顶点数
- 最大单个 fragment collision hull 数
- 最小有效 fragment 面积
- 最远同步距离
- 休眠超时时间

### 降级策略

- 过小碎片直接变粒子或 decal
- 过多碎片合并成低精度 debris actor
- 远距离 fragment 冻结
- 低价值 fragment 不生成复杂碰撞
- 三角剖分任务排队，超过预算延迟生成

------------------------------------------------------------------------

## 13. 与 GAS 的关系

GAS 只负责：

- 技能触发
- 冷却和消耗
- Gameplay Event
- 伤害参数传入

GAS 不负责：

- 连通域检测
- 轮廓提取
- 三角剖分
- 物理模拟
- fragment split / merge 决策

推荐事件链：

```text
Client Input
→ GAS Ability
→ Server Gameplay Event
→ Voxel / Fragment Damage Event
→ Server Simulation
→ ChunkDiff + FragmentEvent
→ Client Apply
```

------------------------------------------------------------------------

## 14. 完整能力范围

最终系统需要覆盖两类来源：

- 预设 2D 可破坏物体
- 从 Segment Column 世界中脱离的地形或结构 fragment

最终系统需要支持：

- 运行时几何切割
- fragment 继续被切碎
- 小于最小面积、质量、顶点数或价值阈值时停止分割
- 从世界 detach 成 fragment
- fragment 回写 Segment Column 世界
- fragment 休眠、冻结、销毁和重新唤醒
- 多材料 fragment 的渲染和物理表达
- 服务器权威的事件同步和 revision 校验

------------------------------------------------------------------------

## 15. 主要风险

- 几何边界条件复杂，容易产生非法多边形
- 动态碰撞体过多会压垮服务器物理
- 三角剖分和 convex decomposition 不适合在 game thread 同步执行
- 客户端重建结果必须和服务器 revision 对齐
- 回写世界可能导致材料、火焰、液体状态丢失
- 多材料 fragment 的视觉和物理边界可能不一致

------------------------------------------------------------------------

## 16. 系统级决策

- Fragment 作为 Segment Column 之上的动态刚体表达，不改变世界主数据结构。
- 动态碰撞默认 simple collision，不使用复杂三角碰撞。
- 静止后可保留为 sleeping actor、冻结为静态 fragment，或回写 Segment Column。
- 只有服务器能创建、分裂、销毁和回写 fragment。
- 网络同步 fragment event，不同步完整世界网格。
- 允许 fragment 递归切碎，但必须有最小面积、质量、顶点数和服务器预算作为停止条件。
- 预设 2D 物体和地形 detach fragment 共享同一套轮廓、三角剖分、碰撞和网络同步管线。
