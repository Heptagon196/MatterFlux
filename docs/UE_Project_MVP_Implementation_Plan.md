# UE Project MVP 实施方案

> **历史设计稿（不作为 0.2.0 接口依据）**
>
> 本文记录最初的 MVP 规划，其中单数 `OuterContour`、`CollisionContour`
> 等示例已经被多轮廓、孔洞和确定性三角剖分实现取代。学习和使用当前项目时，
> 请以 `MatterFlux_UE_Beginner_Guide.md` 为入口，以
> `MatterFlux_0.2.0_Release_Audit.md` 判断实际完成和验收状态。

## 1. 文档定位

本文只描述项目 MVP 的临时实施范围、模块拆分、任务顺序和验收标准。

最终设计目标见：

- `docs/UE_GAS_DS_SegmentColumn_Design..md`
- `docs/UE_Dynamic_Fragment_Triangulation_System.md`

MVP 不代表最终能力边界。MVP 的目标是用最小范围验证核心链路，而不是一次性完成全部最终系统。

------------------------------------------------------------------------

## 2. MVP 验证目标

MVP 必须验证：

- Dedicated Server 权威事件流
- GAS 到世界模拟事件的边界
- Chunk / Segment Column 的最小数据结构
- 预设 2D 物体的运行时几何切割
- Fragment spawn、mesh 生成、简单碰撞和 replicated movement
- ChunkDiff / FragmentEvent 的同步路径
- UE MVVM 驱动的调试 HUD / 状态面板

MVP 不验证：

- 任意地形 detach 成 fragment
- fragment 递归切碎
- fragment 回写 Segment Column
- 大规模流体、火焰、结构破坏的完整预算系统
- 复杂多材料 fragment
- 完整存档和回放

------------------------------------------------------------------------

## 3. MVP 验收场景

最终可演示场景：

1. 启动 Dedicated Server 和两个客户端。
2. 地图中存在一个预设 2D 可破坏物体。
3. 客户端使用一个 GAS Ability 触发破坏。
4. Server 接收 Gameplay Event，并生成 FragmentDamageEvent。
5. Server 对预设 2D 物体执行运行时几何切割。
6. Server 销毁或隐藏原物体，生成多个 Fragment2DActor。
7. 两个客户端看到一致的 fragment 形状、位置和初始冲量。
8. fragment 由服务器物理驱动，并通过 replicated movement 同步。
9. 小于阈值的碎片被丢弃或降级为视觉 debris。

验收重点不是最终美术质量，而是权威链路、几何链路和同步链路闭合。

------------------------------------------------------------------------

## 4. MVP 系统范围

### 包含

- 一个 UFragment2DAsset 数据资产
- 一个 AFragment2DSourceActor，代表未破坏的预设 2D 物体
- 一个 AFragment2DActor，代表切割后生成的动态碎片
- 一个服务器侧 FragmentSimulationSubsystem
- 一个最小 FragmentDamageEvent
- 一个最小 FragmentSpawnEvent
- 一个调试用 GAS Ability
- 一个调试地图
- 一个基于 UE MVVM 的调试 UI

### 不包含

- 任意 Chunk 地形 detach
- fragment 再次几何切割
- fragment 回写 Segment Column
- 多材料边界切割
- 任意多边形布尔
- 复杂 spline 切割
- 客户端预测切割

------------------------------------------------------------------------

## 5. 模块边界

### GAS

GAS 只负责输入、冷却、消耗和事件发送。

GAS 输出：

```text
GameplayEvent.FragmentDamageRequested
  TargetActor
  DamageShape
  DamagePower
  Instigator
```

GAS 禁止：

- 直接修改 SolidMask
- 直接生成 fragment actor
- 直接执行 triangulation
- 直接决定 split 结果

### FragmentSimulationSubsystem

服务器侧子系统，负责：

- 接收 FragmentDamageRequested
- 校验目标和权限
- 执行几何切割
- 生成 fragment payload
- spawn AFragment2DActor
- 广播 FragmentSpawnEvent 或依赖 actor replication
- 管理小碎片降级

### Fragment2DSourceActor

未破坏的预设物体，负责：

- 持有 UFragment2DAsset
- 提供世界 transform
- 提供当前 Revision
- 接收服务器 damage 请求
- 在破坏后隐藏、销毁或切换状态

### Fragment2DActor

动态碎片 actor，负责：

- 根据 payload 构建 runtime mesh
- 创建 simple collision
- 开启服务器物理
- 同步 transform
- 进入 sleeping / destroyed 生命周期

### UI / MVVM

MVP UI 使用 Unreal Engine 的 MVVM 框架。

UI 负责：

- 显示当前 debug 状态
- 显示 fragment count
- 显示 selected source / fragment id
- 显示 revision
- 显示当前 CellSize、MinFragmentAreaPixels 等调试参数
- 发起调试命令请求

ViewModel 负责：

- 暴露 FieldNotify 属性
- 接收来自 PlayerController、Subsystem 或 replicated state 的只读状态
- 将 UI command 转发给 PlayerController 或调试 Subsystem

UI 禁止：

- 直接修改 SolidMask
- 直接执行几何切割
- 直接 spawn fragment actor
- 直接改写服务器权威状态

------------------------------------------------------------------------

## 6. 核心数据契约

### EFragmentDamageShapeType

```cpp
enum class EFragmentDamageShapeType : uint8
{
    Circle,
    Box,
    Line
};
```

### FFragmentDamageShape

```cpp
struct FFragmentDamageShape
{
    EFragmentDamageShapeType Type;
    FTransform WorldTransform;
    FVector2D Extents;
    float Radius;
    float Thickness;
};
```

规则：

- Circle 使用 `WorldTransform.Location` 和 `Radius`。
- Box 使用 `WorldTransform` 和 `Extents`。
- Line 使用 `WorldTransform`、`Extents.X` 作为长度、`Thickness` 作为宽度。

### FFragmentDamageEvent

```cpp
struct FFragmentDamageEvent
{
    FGuid SourceId;
    int32 BaseRevision;
    FFragmentDamageShape DamageShape;
    float DamagePower;
    int32 EventSeed;
};
```

### FFragmentSpawnPayload

```cpp
struct FFragmentSpawnPayload
{
    FGuid FragmentId;
    int32 Revision;
    TArray<FVector2D> OuterContour;
    TArray<int32> TriangleIndices;
    TArray<FVector2D> Vertices2D;
    TArray<FVector2D> CollisionContour;
    FTransform InitialTransform;
    FVector InitialLinearVelocity;
    FVector InitialAngularVelocity;
    float Mass;
};
```

MVP 可以先传 contour 和 triangle payload。后续再优化为 seed + deterministic rebuild。

### UFragmentDebugViewModel

```cpp
UCLASS()
class UFragmentDebugViewModel : public UMVVMViewModelBase
{
    GENERATED_BODY()

public:
    void SetFragmentCount(int32 NewValue);
    void SetSelectedFragmentId(FGuid NewValue);
    void SetSelectedRevision(int32 NewValue);
    void SetCellSize(float NewValue);
    void SetMinFragmentAreaPixels(int32 NewValue);

private:
    UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta=(AllowPrivateAccess))
    int32 FragmentCount = 0;

    UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta=(AllowPrivateAccess))
    FGuid SelectedFragmentId;

    UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta=(AllowPrivateAccess))
    int32 SelectedRevision = 0;

    UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta=(AllowPrivateAccess))
    float CellSize = 10.0f;

    UPROPERTY(BlueprintReadOnly, FieldNotify, Getter, meta=(AllowPrivateAccess))
    int32 MinFragmentAreaPixels = 16;
};
```

MVP 只要求基础状态展示和调试命令入口，不要求完整游戏 HUD。

------------------------------------------------------------------------

## 7. 几何处理管线

MVP 使用 solid mask 作为运行时切割的中间表达。

```text
UFragment2DAsset
→ Initial SolidMask
→ Apply DamageShape
→ Connected Components
→ Drop Small Islands
→ Contour Extraction
→ Contour Simplification
→ Triangulation
→ Collision Contour Simplification
→ FragmentSpawnPayload
```

### SolidMask

MVP 推荐固定分辨率：

- 小型物体：64 x 64
- 中型物体：128 x 128
- 单个物体上限：256 x 256

像素含义：

- 0：空
- 1：固体

MVP 不做材料分层，只保留一个 MaterialSummary。

### 2D 到 UE 3D 映射

MVP 使用正方体单元表达 2D fragment：

- 2D 轮廓位于 UE 的 XZ 平面
- 厚度沿 UE 的 Y 轴 extrusion
- 每个 mask cell 映射为一个边长相同的 3D 正方体单元

必须暴露一个配置项，用于调试手感和性能：

```text
CellSize = 10cm
```

该参数同时决定：

- fragment 世界尺寸
- extrusion 厚度
- mesh 顶点位置
- collision hull 尺寸
- mass 估算
- inertia 估算
- physics stability
- 客户端渲染和同步成本

规则：

- SolidMask 只表达拓扑。
- `CellSize` 决定拓扑映射到 UE 世界后的实际尺度。
- 单个 mask cell 在 UE 中对应 `CellSize x CellSize x CellSize` 的正方体单元。
- 性能测试时可以调整 `CellSize`，观察 fragment 尺寸、碰撞成本、物理稳定性和网络同步表现。

### DamageShape Apply

Circle / Box / Line 都先 rasterize 到 SolidMask，再从 mask 中移除对应区域。

规则：

- damage 后没有分裂时，可以只更新 source actor mesh，也可以直接视为未产生 fragment。
- damage 后产生多个 connected components 时，生成多个 fragment。
- 最大 component 保留或生成 fragment，由 SourceActor 的 `bDestroySourceOnFirstBreak` 配置决定。

MVP 推荐：破坏后原 SourceActor 销毁，所有有效 connected components 都生成 fragment。

### Connected Components

使用 4-neighbor 或 8-neighbor 必须固定。

推荐：8-neighbor。它更符合视觉上的连续块，不容易因为斜向接触产生过碎 split。

### Contour Extraction

推荐实现：

- mask boundary tracing
- 或 marching squares

输出必须满足：

- contour 闭合
- 顶点顺序稳定
- 面积小于阈值的 contour 丢弃
- 自交或退化 contour 直接降级为 debris

### Triangulation

MVP 优先接 UE GeometryProcessing / DynamicMesh 能力，或接入稳定 polygon triangulation 库。

禁止把 triangulation 逻辑和 actor replication 混在一起。几何处理应放在纯数据 helper 或 subsystem 内，便于后续测试。

------------------------------------------------------------------------

## 8. 碰撞和物理

MVP 不使用复杂三角碰撞作为动态 fragment 的默认碰撞。

策略：

- 小 fragment：单 convex hull
- 中 fragment：简化 contour 后单 hull
- 复杂 fragment：MVP 可降级为 bounding box 或丢弃

Actor 设置：

```text
bReplicates = true
bReplicateMovement = true
CollisionEnabled = QueryAndPhysics
ObjectType = PhysicsBody
SimulatePhysics = true only on server
```

初始冲量：

- 从 damage center 指向 fragment center
- 强度由 DamagePower、fragment mass 和距离衰减决定
- 加少量 angular impulse，用 EventSeed 保持可复现

------------------------------------------------------------------------

## 9. 网络同步策略

MVP 采用服务器权威 actor replication。

服务器负责：

- 执行 cut
- spawn fragment actor
- 设置 initial transform / velocity
- 复制 actor 和 movement

客户端负责：

- 根据 replicated payload 构建 mesh
- 播放视觉效果
- 接收 movement replication

MVP 暂不做客户端预测切割。

Payload 选择：

- 初期可以复制 `FFragmentSpawnPayload`。
- 如果 payload 过大，再改成复制 contour + seed。
- 不在 MVP 中追求最优带宽。

------------------------------------------------------------------------

## 10. Chunk / Segment Column MVP

MVP 只需要最小世界结构，用来验证主架构链路。

最小数据：

```text
ChunkCoord
ChunkState
TileCoord
SegmentList
MaterialId
```

MVP 可先实现：

- 一个测试 Chunk
- 固定 32 x 32 tile
- 简单 material id
- ChunkDiff 只支持 add/remove/material change 的数据结构定义

MVP 不要求：

- 完整流体
- 完整火焰
- 完整结构稳定性
- 任意地形 detach

------------------------------------------------------------------------

## 11. 工作包拆分

### WP1：项目骨架和调试地图

交付：

- Dedicated Server 可启动
- 两个客户端可连接
- 调试地图含一个 Fragment2DSourceActor
- 基础日志分类和 debug draw 开关

验收：

- Server 日志能看到客户端触发的 Gameplay Event

### WP2：Fragment2DAsset 和 SourceActor

交付：

- UFragment2DAsset
- SourceActor 从 asset 构建初始 mesh
- SourceActor 有稳定 SourceId 和 Revision

验收：

- 地图中能显示一个由 asset 生成的 2D 物体

### WP3：DamageShape 和 SolidMask 修改

交付：

- Circle / Box / Line damage shape
- WorldTransform 转 local mask space
- damage shape rasterize
- mask 修改后可 debug draw

验收：

- 命令或 ability 能在 debug overlay 中切掉 mask 区域

### WP4：Connected Components 和 Fragment Payload

交付：

- 8-neighbor connected components
- min area 过滤
- component 转 payload

验收：

- 一次切割能稳定得到 N 个 component
- 小碎片会被过滤

### WP5：Contour / Triangulation / Mesh

交付：

- component contour extraction
- contour simplification
- triangulation
- runtime mesh section

验收：

- 每个有效 component 能生成可见 mesh
- 无退化三角形导致的明显渲染错误

### WP6：Collision / Physics

交付：

- simple collision contour
- mass 估算
- initial impulse
- server physics
- movement replication

验收：

- fragment 在服务器物理中飞出、落地、碰撞
- 两个客户端看到的位置大体一致

### WP7：GAS 集成

交付：

- 调试 Ability
- Gameplay Event 到 FragmentSimulationSubsystem
- 权限校验

验收：

- 客户端按键触发 ability 后，server 执行 cut
- 客户端不能本地伪造权威 cut

### WP8：MVVM 调试 UI

交付：

- 启用 ModelViewViewModel 依赖
- UFragmentDebugViewModel
- 调试 HUD Widget
- fragment count / selected id / revision / CellSize 展示
- debug command 通过 ViewModel 转发，不直接修改模拟状态

验收：

- UI 状态通过 ViewModel FieldNotify 更新
- UI command 最终由 PlayerController / Subsystem 发起服务器请求
- Widget 内没有几何切割、spawn 或权威状态修改逻辑

### WP9：MVP 收口

交付：

- 错误处理
- debug commands
- 参数配置
- 性能采样
- 文档更新

验收：

- 完成 MVP 验收场景

------------------------------------------------------------------------

## 12. 调试工具

必须提供：

- 显示 source actor bounds
- 显示 solid mask
- 显示 damage shape
- 显示 connected component id
- 显示 contour vertices
- 显示 collision contour
- 显示 fragment id / revision

推荐控制台命令：

```text
mf.Fragment.Debug 0|1
mf.Fragment.DrawMask 0|1
mf.Fragment.DrawContours 0|1
mf.Fragment.MinArea <value>
mf.Fragment.CellSize <value>
mf.Fragment.ForceDamageCircle <radius>
mf.Fragment.ForceDamageLine <length> <thickness>
```

------------------------------------------------------------------------

## 13. 参数默认值

建议初始值：

```text
MaskResolution = 128
CellSize = 10cm
MinFragmentAreaPixels = 16
MaxFragmentsPerBreak = 16
MaxVerticesPerFragment = 128
ContourSimplifyTolerance = 0.75 mask pixels
CollisionSimplifyTolerance = 2.0 mask pixels
MinMassKg = 0.5
MaxMassKg = 50.0
SleepTimeoutSeconds = 5.0
```

超过预算时：

- 超小碎片丢弃
- 超复杂 contour 降级为 box collision
- 超过 MaxFragmentsPerBreak 的碎片按面积保留最大的 N 个

------------------------------------------------------------------------

## 14. 自动化测试建议

几何逻辑应尽量写成纯数据函数，至少覆盖：

- Circle cut 把一个 solid mask 分成两个 component
- Line cut 把矩形切成两个 component
- Box cut 后小岛被 min area 过滤
- contour 输出闭合
- triangulation 面积接近 contour 面积
- 相同输入和 EventSeed 输出稳定

网络和集成测试覆盖：

- server 执行 cut 后生成 fragment actor
- client 不执行权威 cut
- fragment actor replicated movement 生效
- BaseRevision 不匹配时拒绝 damage event

------------------------------------------------------------------------

## 15. 失败处理

几何处理失败时不得崩溃。

处理策略：

- contour 无效：该 component 降级为 debris 或丢弃
- triangulation 失败：该 component 降级为 box mesh 或丢弃
- collision 失败：生成 QueryOnly visual fragment，或丢弃
- payload 过大：按面积保留最大 fragment，丢弃其余
- revision 不匹配：拒绝事件并记录日志

------------------------------------------------------------------------

## 16. MVP 停止条件

MVP 交付标准：

- 一个预设 2D 物体能被服务器事件切开
- 切开后生成多个可见 fragment
- fragment 有简单碰撞和服务器权威物理
- 两个客户端能看到一致的 fragment 结果
- GAS 不直接修改几何，只触发事件
- UI 使用 UE MVVM，且不直接修改模拟状态
- 几何失败有降级路径，不会崩溃
- 关键调试视图可用

明确不要求：

- fragment 再次切碎
- 任意地形 detach
- 回写 Segment Column
- 大规模压力测试达标
- 最优网络带宽

------------------------------------------------------------------------

## 17. MVP 之后

MVP 通过后再进入：

- fragment 递归切碎
- 任意地形 detach
- fragment 回写世界
- 多材料 fragment
- 更严格的 interest management
- 更完整的服务器预算和降级策略
- seed + deterministic rebuild 网络优化

------------------------------------------------------------------------

## 18. Grill 决策记录

已确定：

- 最终目标支持任意地形 detach。
- MVP 只做预设 2D 可破坏物体。
- 最终目标支持 fragment 继续切碎。
- MVP 只做单次破坏。
- MVP 使用运行时几何切割。
- MVP 不使用纯设计师预设 fracture pieces。
- MVP 的 2D 轮廓映射到 UE XZ 平面，沿 Y 轴 extrusion。
- 每个 mask cell 对应一个正方体单元，只暴露 `CellSize` 一个尺寸配置项。
- 项目 UI 使用 UE MVVM 框架；MVP 调试 UI 也按 MVVM 实现。
