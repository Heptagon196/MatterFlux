# ADR-013：物品材质只有一份视觉投影，分离状态不控制可见性

- 状态：已采纳并实施
- 日期：2026-08-22

## 背景

MatterFlux 的可切割物品会在 Source Actor、流式 chunk proxy、WholeObject 合批网格和动态
Fragment/aggregate carrier 之间切换。过去每个 renderer 各自硬编码 `FaceContrast`、
`ColorVariation`、`Roughness`、`ShadowLift` 和侧面亮度。同一堵 `stone` 墙在切割前后的
逻辑材质与颜色虽然没有改变，渲染参数却会从一组值跳到另一组值。

树木还有一项交叉状态：切断后的短暂物理保护同时隐藏根部并关闭碰撞，计时结束后再显示
根部。这使已经提交的树桩像是稍后在原地生成。

Noita 的公开说明把世界像素视为带材质的模拟单元；GDC 技术分享进一步说明静态模拟与动态
刚体只是同一材料状态的不同物理表示。MatterFlux 不复制其 2D 引擎实现，但采用相同边界：
材质状态是事实，mesh/actor/proxy 只是可替换的投影。

- <https://noitagame.com/>
- <https://www.gdcvault.com/play/1025695/Exploring-the-Tech-and-DesignAt>

## 决策

新增 `MatterFluxVoxelMaterialStyle` 深接口；调用者提交材质事实和面角色，不能再分别拼装颜色与
shader 参数：

```text
MaterialId + BaseColor + CellSize + FaceRole
    -> FVoxelMaterialProjection
    -> ApplyVoxelMaterialProjection(material, projection)
```

Source Actor、chunk proxy、普通 Fragment、aggregate carrier 和房屋 WholeObject 都通过该接口
得到实体材质参数。即使基础颜色为白色，也建立 MID 并应用同一风格，不能隐式依赖父材质的
默认值。透明 Ghost、角色和魔法投射物不属于可切割实体材质投影，保留独立视觉语义。

`GeometryStyle` 和 `FaceRole` 同样属于材质状态的投影契约。房屋静态 WholeObject 与切下的
动态 Fragment 都使用 `VoxelBlocks`；WholeObject 的侧面 section 必须提交 `Side`，不能在切割
前把所有面当成 `Primary`。逻辑 Source 可通过 `SetSourceMeshProjectionEnabled(false)` 明确把
显示权交给 WholeObject；伤害提交和复制回调不得擅自恢复第二份可视网格。

树的分离保护改为 `bAggregateSeparationCollisionSuppressed`：它可以短时关闭根部碰撞，但不能
隐藏 Actor 或 mesh。已提交的根部从切断同一帧开始持续可见；保护结束只改变碰撞/物化方式，
不产生视觉揭示事件。

## 不变量

- 同一个 `MaterialId + Color + CellSize` 在所有实体渲染表示中使用相同风格参数。
- 同一个实体的 `GeometryStyle` 与 `FaceRole` 在静态和动态投影中保持一致。
- 表示切换不得修改逻辑材质、颜色或视觉风格。
- 同一时刻只有一个 renderer 拥有某份逻辑 Source 的可视投影权。
- 碰撞抑制、网络物化和可见性是三个正交状态。
- 已提交的残留 mask 不能由计时器延迟显示。
- 动态刚体只能取得已从静态 mask 中原子移除的部分；残留 mask 始终由静态表示显示。

## 回归门禁

- 树木重复多方向切割检查切断同帧树桩可见且 mesh 不依赖延迟揭示。
- 房屋结构测试比较 Source 与 WholeObject 的规范投影、侧面角色、几何样式，并验证切割会改变
  WholeObject 三角形且不会重新显示逻辑 Source。
- `mf.Visual.HouseSequence` 必须对实际生成房屋的相机可见墙面执行真实切割，保存同机位前后图；
  排除刀口后的未修改墙面颜色差异必须维持在像素阈值内。
- `mf.Visual.TreeCutSequence` 必须走世界切割服务并保存静态、刚分离、下落、落地四阶段截图；
  横切面的动态网格法线必须朝下，mask 宽高不得转置。
- 世界切割目标预算按刀口到逻辑物体表面的距离排序，不能用聚合根 Actor 中心代表整棵树。
- 酸腐蚀树测试检查重叠期所有树桩切片可见但碰撞受抑制。
- 物理角度、批量切割、SourceProxy 与 Dedicated Server 双客户端测试保护相邻路径。
