# ADR-002：逻辑 Source 独立，静态渲染与碰撞按区块合并

- 状态：接受
- 日期：2026-08-08

## 背景

森林中的树干、树叶、草、花和岩石都需要独立的 `SourceId`、mask、材质、revision 与聚合关系，但它们不因此都需要 UE Actor。逐对象 Actor 会放大 UObject 生命周期、组件注册、复制、Tick 和 draw-call 成本；特别是 pristine 对象只需要静态显示和碰撞。

## 决策

1. `FragmentSourceChunks` 是静态 Source 的逻辑存储。每个 Source 仍可按确定性 GUID 单独查询和修改。
2. `UMatterFluxFragmentSourceProxyComponent` 是渲染 adapter。它按“可见区块 × 材质 × 正/侧面 × 碰撞策略”合并 ProceduralMesh section。
3. pristine Source 不创建 `AFragment2DSourceActor`。树干的静态复杂碰撞也进入区块 mesh；不可碰撞装饰只贡献视觉三角形。
4. 切割可以短暂实体化目标以复用事务式 damage 管线。仍连接地形的结果立刻把 revision/mask 回写逻辑存储并重新加入区块 batch；只有脱落后需要独立物理、移动和网络生命周期的对象保留 Actor。
5. 正在燃烧的静态 Source 也不保留 Actor。`FSourceCombustionRuntime` 在世界级逻辑存储中推进逐 mask fixed-step；木炭仍合入区块 ProceduralMesh，火焰和烟雾分别进入世界级共享 ISM。临时物化的 Source 在卸载前必须把燃料、燃烧、残渣、随机状态和 fixed-step 时间债原子交还逻辑存储。
6. 服务器只复制发生过修改的 Source。`FMatterFluxReplicatedFragmentSourceStateList` 使用 Fast Array 增量复制 SourceId、revision、1-bit fuel/residue/burning mask 与燃烧元数据；客户端原子更新逻辑状态并只标脏对应区块。初始世界继续由 seed + Lua 内容重建。
7. 隐藏区块同时关闭合并碰撞；预热 mesh 不得在不可见区块留下阻挡。隐藏区块中的逻辑燃烧仍可按活动预算继续，渲染可见性不能决定模拟是否存在。
8. 一个 aggregate（例如树干和三层树冠）完全脱离地形后，只保留一个负责 Chaos 与 replicated movement 的 `AFragment2DActor` Carrier。所有附属 Source 都转为 `FFragmentAggregateSourceState`：SourceId、revision、燃料/残渣/燃烧 mask、局部 transform、材质身份和碰撞策略仍独立，几何按稳定材质/颜色键合入 Carrier 的一个 `UProceduralMeshComponent`。声明碰撞的成员把按局部 transform 生成的凸体加入同一 Chaos 复合刚体，不再保留附属 Actor。
9. 燃烧成员的 fixed-step、随机状态和剩余燃烧时间继续由世界级 `FSourceCombustionRuntime` 推进；Carrier 只接收二值可视 mask 并重建共享材质 section。火焰和烟雾仍进入世界级共享 ISM，但用 Carrier 的当前 transform 定位。非燃烧成员在静态存储中发布零 mask tombstone；燃烧成员保留完整逻辑快照，并用动态所有权标记隐藏静态代理。

## 不变量

- 同一时刻，一个 Source 的可视所有权只能在 chunk proxy 或交互 Actor 之一。
- Source 从 Actor 返回 batch 前必须成功捕获完整运行时状态；捕获或恢复失败时 Actor 和旧逻辑状态都保持不变并等待重试。
- 无效 mask、过期 revision 或超出复制预算的状态不得部分提交。
- 动态碎片和已脱离树体拥有独立世界 transform，因此可以是 Actor；静态身份本身不是创建 Actor 的理由。
- 一个刚性 aggregate 只有一个独立世界 transform，因此只能有一个物理 Carrier Actor；aggregate 成员不能为了材质不同就各占一个 Actor。
- 燃烧计时 mask 可以保存大于 1 的剩余步数；跨入 Carrier 复制边界时必须转换为 0/1 可视 mask，完整计时只留在逻辑 runtime。

## 后果

新生成森林的 pristine Source Actor 数量从 37（测试种子 13579）降为 0，同时保留树干阻挡。切割后的静态树桩和燃烧中的静态对象也不会永久占用 Actor。代价是修改一个 Source 会重建其所在区块的合并 mesh/collision，因此必须继续受世界切割 FIFO、fixed-step 和 dirty-chunk 预算保护。完全脱落并参加独立 Chaos 物理的碎片仍是一物理刚体一 Actor；同一棵倒下的树则是一个刚体 Carrier，而不是“树干 Actor + 三个树冠 Actor”。

迁移已经覆盖普通、燃烧中和声明碰撞的附属成员。专项测试验证了燃烧中砍倒仍继续产生残渣、碰撞成员进入 Carrier 复合刚体、附属 Source Actor 数为零，以及 dedicated server 与两个客户端收到逐字段一致的 Carrier 成员状态。
