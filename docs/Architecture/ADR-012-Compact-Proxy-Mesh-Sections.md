# ADR-012：代理网格 section 只提交被索引引用的顶点

- 状态：已采纳并实施
- 日期：2026-08-10

## 背景

区块代理把每个 Source 的挤出网格拆成正反面 section 与侧面 section，以便使用不同材质参数。
旧的 `AppendMeshPart` 虽然只把对应范围的 triangle index 加进目标 section，却在两个调用中都
复制整套 Source 顶点、法线和 UV。因此正面 section 携带不会被正面三角形引用的侧面顶点，
侧面 section 也携带不会被侧面三角形引用的正反面顶点。

`UProceduralMeshComponent::CreateMeshSection` 会为传入的完整顶点数组创建 CPU/渲染资源；未被
索引引用并不等于免费。最小 2×2 Source 的两个 section 共提交 48 个顶点，其中 24 个从未被
任何三角形引用。森林中的每个 pristine Source 都走这条合并路径，浪费会随可见 Source 数
线性增长。

## 决策

保留 `BuildExtrudedMesh` 的规范化完整输出和 `FaceIndexCount` seam，不改变公共几何接口。
只深化代理内部的 `AppendMeshPart`：

1. 先事务式验证 index range、法线/UV 数量和全部源顶点索引；非法输入不向目标 group 追加
   半份数据。
2. 为本次 Source 建立 `SourceVertexIndex -> LocalVertexIndex` 临时重映射。
3. 按目标 triangle index 的原始顺序遍历；某个源顶点第一次出现时才转换位置/法线并复制 UV。
4. triangle 保持原顺序，只把顶点索引替换为当前 group 的局部紧凑索引。

首次出现顺序由确定性的 triangle index 数组唯一决定，因此不依赖 `TMap` 迭代顺序。每个
Source/section 使用独立 remap，再通过已有 group vertex offset 追加，不会错误合并空间位置相同
但法线、UV 或 Source transform 不同的顶点。

## 不变量

- 每个 ProceduralMesh section 中的每个顶点必须至少被一个 triangle index 引用。
- triangle 顺序、绕序、法线、UV、材质分组和碰撞开关保持不变。
- 同一输入重复构建时，section 数量、顶点、法线、UV 和 index 必须逐字段一致。
- 不跨 Source 焊接顶点；逻辑独立和硬边法线不能因压缩而丢失。
- 任一源索引非法时，目标 group 不得得到半次追加。

## TDD 与验证

RED 通过公开的 `UProceduralMeshComponent` section 检查引用关系。2×2 Source 得到 48 个顶点、
24 个未引用，断言稳定失败。GREEN 后得到 24 个顶点、0 个未引用，几何顶点提交量减少 50%。

第二项回归把 64 个 1×1 Source 合并进同一区块并构建两次：每个 Source 只贡献 24 个实际使用
顶点，总计 1536；两个代理的 section vertex count、index count、位置、法线、UV 和索引逐字段
一致，且未引用顶点仍为 0。

大世界性能门禁第一次独立运行的燃烧增量为 281.49ms，超过既有 275ms 门槛 6.49ms；没有
放宽门槛。立即在独立进程复跑为 226.23ms 并通过，最终全量共享进程为 155.90ms。移动
500/1200/2500cm/s 的边界 tick 最大值均低于 1ms，未出现区块重建回归。

最终结果：

- FragmentSourceProxy 行为：4/4；
- 大世界移动/燃烧性能门禁：通过；
- Listen Host + Client PIE：1/1；
- 完整 `Automation RunTests MatterFlux`：191/191，0 失败；
- Editor Development、Game Development、Game Shipping 均使用 MSVC 14.44.35222 构建成功。

主要日志：

- `Saved/Logs/MatterFluxProxyCompactVerticesRed.log`
- `Saved/Logs/MatterFluxProxyCompactVerticesGreen.log`
- `Saved/Logs/MatterFluxProxyCompactBatchGreen.log`
- `Saved/Logs/MatterFluxProxyCompactLargeWorld.log`
- `Saved/Logs/MatterFluxProxyCompactLargeWorldRerun.log`
- `Saved/Logs/MatterFluxProxyCompactListenHostClient.log`
- `Saved/Logs/MatterFluxProxyCompactFullAutomation.log`

## 后果

代理仍按材质、颜色、CellSize、正/侧面和碰撞策略生成相同 draw-call 数，但每个 section 不再为
另一类三角形携带无效顶点。CPU 聚合、ProceduralMesh CPU buffer、渲染 buffer 和碰撞输入都
少处理这些废数据。代价是追加时需要一个临时 remap；小 Source 使用 inline storage，大 Source
才回退堆分配。最终大世界门禁证明这项局部成本没有形成移动或燃烧回归。
