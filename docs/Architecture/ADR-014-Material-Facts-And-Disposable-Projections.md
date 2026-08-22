# ADR-014：材质事实与可丢弃投影

- 状态：已采纳并实施
- 日期：2026-08-22

## 问题

MatterFlux 同时存在落沙材料、可切割物品、静态合批网格和动态 Chaos 刚体。如果把 mesh、
Actor 或渲染缓存也当成状态，就会出现同一物体的两份事实：树木切断后旧代理重新显示、房屋
切割后换了一套颜色、横切 mask 被动态网格转置，以及液体排开后逐格顶面暴露成裂缝。

## 决策

逻辑材质状态是唯一事实；具体事实容器按材料类别区分，但渲染表示永远不是事实：

- 流动材料（液体、粉末、气体）以 `FChunkedMaterialWorld` 的格子快照为事实。
- 可切割固体物品以 `SourceId + Revision + MaterialId + RuntimeMask` 为事实。静态 Source、
  SourceProxy、WholeObject 和动态 Fragment/aggregate carrier 只取得这份 mask 的投影权。
- 静态到动态的切换必须在同一事务内先从静态 mask 移除分离部分，再把同一部分交给动态
  carrier；残留 mask 从提交同帧起持续可见。
- 碰撞抑制、网络物化、合批和可见性互相正交。计时器只能改变碰撞或表示所有权，不能揭示
  一份此前隐藏的逻辑残留。
- `MatterFluxLiquidSurfaceProjection` 只读当前材质快照，生成可丢弃的连通轮廓和自由表面网格。
  它只能为当前存在的液体列生成顶面，并在当前相邻列的有限高度范围内插值；禁止补空坐标、
  保留上一帧高度、为连通体选择统一水位，或施加只升不降的滤波。透明度和 2 cm 深度偏移仅为
  当帧表现，不能回写材质世界、参与浮力采样或改变体积守恒。
- 身体进入液体时只提交当前物理体积约束。被排开的 `Amount` 逐单位转移到当前最低的可用粒子
  表面，身体离开后再从最近的被排开余量守恒回流；任何路径都不得生成、删除或复制液体量。
- `BodyDisplacementVacancy` 是短期的身体—材质事务事实，不是渲染历史。事务活动期间，生成地图
  的每个湖格保留其种子粒子量，只允许其上的排开余量继续流动；否则尾迹虽会补上，空洞却会被
  搬到供体位置。回填搜索覆盖排开半径的完整直径，但仍沿当前同材质连通粒子搜索，不能跨越干地
  把不同水体按高度合并。

## 表示所有权

```text
可切割固体 RuntimeMask
    -> Source Actor / streaming proxy / WholeObject（恰好一个静态投影者）
    -> 原子分离事务
    -> residual RuntimeMask + detached carrier mask

FChunkedMaterialWorld snapshot
    -> body-volume constraint -> conserved particle transfer / restitution
    -> gameplay sampling / reactions / replication
    -> disposable liquid surface projection
```

`MatterFluxVoxelMaterialStyle` 是所有固体投影的统一材质适配器；调用者不能为静态和动态表示
分别硬编码颜色、面明暗、粗糙度或变化参数。

## 必须保持的不变量

- 一个逻辑单元在同一时刻只有一个事实容器和一个可见投影所有者。
- 表示切换不修改 `MaterialId`、基础颜色、mask 坐标约定或切面方向。
- 横向世界切割在动态刚体中仍产生水平切面，宽高不转置。
- 房屋未切墙面在 WholeObject 重建前后保持相同规范材质投影。
- 玩家、生物和物体通过同一物理边界推动并排开材质；表现缓存不参与力或排量计算。
- 投影不得修复液面缺口；空格就是当前材质事实。身体尾迹必须由模拟层搬回真实液体量，并且不得
  抽低未受影响的供体格。
- 液面网格的中位高度相对当前 canonical 材质列只允许固定渲染偏移；反复排液不得累积投影高度。
- 身体移走并完成回填后，所有记录尾迹均达到各自事务基准，液体总 `Amount` 精确不变；静态网格、
  动态刚体和液面 mesh 均无权保存或抬高另一份“水位”。

## 验证门禁

- `MatterFlux.Fragment`：事务、横切方向、重复切割、动态碰撞、专服复制。
- `MatterFlux.Playable.House`：房屋 Source/WholeObject 所有权和材质参数一致性。
- `MatterFlux.Rendering.WholeObject` 与 `MatterFlux.Game.FragmentSourceProxy`：静态投影确定性和
  原子更新。
- `MatterFlux.Playable.Liquid`：排液、浮力、守恒和可丢弃液面形状。
- `mf.Visual.TreeCutSequence`、`mf.Visual.HouseSequence`、`mf.Visual.PhysicsPush`、
  `mf.Visual.LiquidPool`、`mf.Visual.DeepLiquidWalk`：真实运行时截图与量化验收。
