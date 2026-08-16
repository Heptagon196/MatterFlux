# MatterFlux 通用切割系统：UE 初学者指南

本文描述当前项目已经运行的实现。目标是让树木、花草、岩石以及后续加入的
mask 地形或物品共享同一条切割管线，而不是为“砍树”写一个特殊分支。

## 1. 现在砍树会发生什么

树由四个 `AFragment2DSourceActor` 组成：

- 一个棕色树干，是聚合体的 root，并按 Lua decorator 配置启用碰撞。
- 三层深浅不同的绿色树冠，是聚合体的 attached members，不启用碰撞。

它们拥有同一个确定性的 `AggregateId`。树干被横向切开后：

1. 切口以下、仍连接原始底部支撑格的树桩保留在地形中。
2. 切口以上的树干 mask 变成 `AFragment2DActor`，构建凸碰撞并由服务器模拟物理。
3. 三层树冠保持各自的叶片材质和 mask，附着到树干碎片这个物理载体上。
4. 树冠因此会和树干一起倒下，但不会凭空获得叶片碰撞。
5. 树冠仍是 mask 源，所以倒下后仍可继续被切割或燃烧。
6. 若在根部切断、没有任何格子仍受支撑，原树干 source 才进入 broken 状态。

这套关系不是通过 `"TreeTrunk"` 字符串判断实现的。任何 source 组合都可以声明
一个 aggregate root 和若干 attached members，因此以后可复用于路牌、吊灯、建筑
附属件等组合物体。

## 2. 最重要的三个 Actor

### `AFragment2DSourceActor`

表示一个仍属于地形或世界结构的可切割物体。它保存：

- `RuntimeMask`：当前哪些格子仍存在。
- `SupportAnchorMask`：初始状态中哪些格子连接支撑面。
- `SourceId` 和 `Revision`：防止请求发给错误或过期的状态。
- `AggregateId`、`bAggregateRoot`：可选的组合物体关系。

### `AFragment2DActor`

表示已经脱离地形的动态碎片。服务器创建网格、凸碰撞、质量和初速度，开启物理并
复制 movement；客户端通过初始复制的 `FFragmentSpawnPayload` 重建相同网格。

### `UFragmentSimulationSubsystem`

这是通用切割入口。它负责：

- 接收单一 source 的精确 damage event。
- 接收一次世界切割，并找到所有可能相交的 mask source。
- 延迟生成碎片，任何一步失败时回滚。
- 成功后提交 source mask，并处理 aggregate attachments。

默认“伐木法杖”里的地形切割法术、编辑器调试命令、敌人攻击或爆炸都应调用这个
subsystem，而不应自己复制一份遍历和切割逻辑。左键只是默认绑定到这根法杖的装备键，
并不直接绑定切割能力。

## 3. 一次切割的完整数据流

```text
FFragmentWorldCutRequest
        |
        v
世界空间粗筛所有 AFragment2DSourceActor
        |
        v
FFragmentDamageEvent（带 SourceId + BaseRevision）
        |
        v
复制 RuntimeMask，切 CandidateMask
        |
        v
8-neighbor component + 原始支撑 anchor 分类
        |
        +--> supported component：写回静态 source
        |
        +--> unsupported component
                  |
                  +--> 小于 Lua 阈值：丢弃
                  |
                  +--> 足够大：轮廓/孔洞/三角剖分/挤出/凸碰撞
        |
        v
延迟创建并初始化所有动态 Actor
        |
        +--> 失败：销毁候选 Actor，mask/revision 完全不变
        |
        +--> 成功：提交 mask/revision，重建并复制静态残余
```

`RequestFragmentDamage` 返回 `true` 的含义是“切割状态已经提交”。即使所有脱离块
都太小、最终没有生成动态 Actor，它仍然可以返回 `true`。

## 4. 支撑模式

`EFragmentSupportMode` 目前有两种：

- `Bottom`：初始 mask 最低一行的实心格是 anchor。切割后，仍通过 8-neighbor
  连到 anchor 的 component 留在 source 中；其余 component 脱离。
- `None`：没有世界支撑。切割后所有剩余 component 都作为脱离候选。适合已悬空
  的独立物品和专门的测试。

程序生成的树木、花草和岩石默认使用 `Bottom`。`UFragment2DAsset` 和没有 asset
的 source 也都可分别配置支撑模式。

一个容易犯的错误是“每次切完都把整个 source 标记 broken”。这样树桩会消失，
并且只是挖一个孔也会让整块地形变成物理碎片。当前实现只在 supported mask
完全为空时标记 broken。

## 5. Lua 中的全局小碎片阈值

引擎级设置与内容定义已经分开：

```text
Content/Lua/MatterFluxEngine.lua   全局力学/引擎参数
Content/Lua/MatterFluxContent.lua  材质、反应、燃烧、decorator、entity
```

当前引擎配置是：

```lua
content.configure_fragmentation(4)
```

含义是：少于 4 个 mask cell 的脱离 component 不创建复制物理 Actor。合法范围是
1～65536。单个 source 的 `MinFragmentAreaPixels` 可以把阈值提高，但不能低于
这个全局下限：

```text
实际阈值 = max(Lua 全局阈值, source 局部阈值)
```

Editor 和 Development 构建每 0.5 秒检查两个 Lua 文件。修改任意一个都会在临时
registry 中重新执行、校验；失败时旧 registry 继续工作。版本哈希只基于规范化的
脚本文本，不包含机器绝对路径，也不受 CRLF/LF 换行差异影响。

## 6. 在别的系统中调用通用切割

下面的代码可用于 Gameplay Ability、武器、陷阱或编辑器工具：

```cpp
FFragmentWorldCutRequest Request;
Request.CutShape.Type = EFragmentDamageShapeType::Line;
Request.CutShape.WorldTransform = CutTransform;
Request.CutShape.Extents.X = 900.0;
Request.CutShape.Thickness = 45.0f;
Request.DamagePower = 1200.0f;
Request.EventSeed = StableEventSeed;
Request.TargetPadding = 160.0f;

const int32 AcceptedSourceCount =
    World->GetSubsystem<UFragmentSimulationSubsystem>()
        ->RequestWorldCut(Request);
```

`TargetPadding` 只放宽粗筛，不会扩大实际删除 cell 的形状。每个 source 的最终
`EventSeed` 会和 `SourceId` 组合，因此一次世界切割命中多个物体时仍有稳定但不同
的碎片 ID 和速度。

如果调用方已经明确知道目标 source，则构造 `FFragmentDamageEvent` 并调用
`RequestFragmentDamage`，可以避免世界查询。

## 7. 怎样让新物体进入切割体系

新增一个可切割物体时，按下面顺序做：

1. 用 `FFragmentSourceMask` 表示占用格，不要另外创建一套“树专用像素”。
2. 选择 `Bottom` 或 `None` 支撑模式。
3. 设置稳定的 `SourceId`；程序生成内容应使用 `NewDeterministicGuid`。
4. 调用 `InitializeFromProceduralMask`。
5. 只在确实需要时启用 source 碰撞。
6. 若物体由多个材质部分组成，为它们分配同一个 `AggregateId`，只设一个 root。
7. 让攻击系统调用 `RequestWorldCut`，不要直接写 `RuntimeMask`。

当前随机森林中的树干、树冠、草、花和岩石都满足这份契约。大型背景高度场仍由
HISM 分块渲染和碰撞，它是关卡承载面，不是一个可搬动的“物品”；若以后要挖地面
形成洞穴，应先把目标地块迁移为独立的 mask/voxel chunk source，再复用相同的
事务、支撑和小块过滤规则，不能直接删除 HISM instance 后绕开该管线。

## 8. 多人复制为什么这样设计

- 只有服务器可以提交 damage transaction。
- source 的 `ProceduralSource` 会持续复制，因此树桩等静态残余能在客户端重建。
- `Revision` 用于拒绝旧请求。
- 动态碎片的 `SpawnPayload` 使用 `COND_InitialOnly`。
- 物理位置由 replicated movement 同步，`OnRep_SpawnPayload` 不重设 Transform。
- aggregate attachment 由服务器建立；附属 source 保持自己的复制 mask 和材质。
- source 完全耗尽时，复制的 `bBroken` 让客户端同步隐藏并关闭碰撞。

## 9. 如何验证

构建 Editor：

```powershell
Build.bat MatterFluxEditor Win64 Development `
  -Project="C:\Users\hepta\Documents\Codes\MatterFlux\MatterFlux.uproject"
```

运行全部项目测试：

```text
Automation RunTests MatterFlux
```

与本功能直接相关的测试包括：

- `MatterFlux.Fragment.Support.SupportedRemainderStaysStatic`
- `MatterFlux.Fragment.Support.LuaMinimumDiscardsTinyDetachedComponent`
- `MatterFlux.Fragment.Aggregate.AttachedPartsFollowDetachedRoot`
- `MatterFlux.Fragment.Cut.WorldRequestTargetsIntersectingSources`
- `MatterFlux.Lua.FragmentationSettingsAreValidatedAndTransactional`
- `MatterFlux.Fragment.Network.DedicatedServerTwoClients`
- `MatterFlux.Playable.VoxelDecorationsSpawnAsDamageableSources`

项目级验收入口是 `Scripts/Verify-MatterFluxRelease.ps1`。它会检查测试数量和关键测试
名称，避免“测试根本没有被发现”却误报成功。
