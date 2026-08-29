# MatterFlux Volume / Span 与局部材料反应迁移计划

- 状态：已实施，完成门禁通过
- 日期：2026-08-29
- 权威性：本文件是仓库内迁移计划；Downloads 中的提案仅保留为输入材料
- 决策记录：ADR-015

## 摘要

将可切割固体从二维 Mask、地形从单高度，统一迁移为三维 Volume；同时彻底删除对象级
`ReactionState`，让固体格、地表材料格和空中粒子都作为 `Material Element` 参与局部接触、
能量传递和材料转化。

反应模块采用深模块 seam：读取不可变材料视图和已排序接触集合，只返回可原子提交的
`FMaterialDeltaBatch`。Actor、渲染、碰撞、存档和复制均不得保存第二份材料或反应事实。

## 目标事实与坐标

### Grid Frame

Volume 使用抽象 U/V/N 基底。地形映射为 X/Y/Z；旧 Fragment Mask 的平面是 X/Z、挤出轴是 Y，
因此兼容转换使用 X/Z/Y。切割形状先由世界空间变换到实例的局部 Grid Frame，再量化到格坐标。

`WorldTransform`、线速度和角速度属于 `FMaterialVolumeInstance`，不属于 Volume 拓扑，不参与
`TopologyRevision` 或逻辑哈希。实例运动不能导致网格拓扑、碰撞拓扑或 Volume Delta 变化。

### Span 与 Chunk

- `FMaterialSpan` 使用 `int32` 半开区间 `[BeginN, EndNExclusive)`；只有编码阶段可以按已验证范围压缩。
- 同列 Span 按起点严格升序、互不重叠；相邻且材质和 flags 相同的 Span 必须合并。
- 不同材质的相邻格仍可结构连通；默认六面邻接，稀疏 `FStructuralSeam` 可显式断开一条面连接。
- Chunk 默认 16×16。Chunk 是不可变快照，Builder 每次整块重建 headers 与连续 SpanPool，禁止持有跨 Revision 的 pool offset。
- Span 与 Dense 只是同一逻辑接口后的存储 adapter。哈希、连通和序列化语义基于逻辑材料格，不基于 adapter 字节布局。
- 未编辑地形继续由程序基线隐式表达；仅保存相对基线的稀疏 Chunk 覆盖，恢复成基线时立即删除覆盖。

### 公共类型

- `FMaterialVolumeTopology`：`DefinitionId`、局部 Grid Frame、不可变 Chunk 快照、结构锚点和 Seam。只有占用、材质或结构连接变化才增加 `TopologyRevision`。
- `FMaterialVolumeFields`：按格稀疏保存非环境值的 `uint16 Energy`。只增加 `FieldRevision`，不触发网格或碰撞重建。
- `FMaterialVolumeInstance`：`InstanceId`、父实例、世界姿态和运动状态；引用 Volume 状态但不进入拓扑哈希。
- `FMaterialElementAddress`：稳定标识 World Cell、Volume Cell 或 Airborne Particle；粒子使用稳定 `ParticleId`，不使用数组下标。
- `FMaterialElementState`：`MaterialIndex`、`Amount`、`Energy`、`RemainingLifetime`。
- `FMaterialContact`：两个 Element 地址、接触单位数和确定性排序键。
- `FMaterialDeltaBatch`：材料替换、数量/能量转移、粒子排放、Volume 编辑和目标 Revision；提交前完整校验，失败不产生部分修改。
- `FLocalMaterialReactionKernel`：唯一入口为“不可变材料视图 + 有序接触 + Fixed Step Context → DeltaBatch”。

## 局部反应与内容数据

`FMatterFluxReactionDefinition` 只表达局部接触规则。删除 `Propagating`、`DurationSteps`、传播概率、
ActiveMask 与 accumulator 语义。规则支持双方材料输出、整数概率、双方能量变化，以及最多两个普通
材料排放结果。

材料定义增加量化热学配置：默认能量、导热率、冷却率、点燃阈值、燃烧产物、`combustion_energy`
和燃烧排放。`combustion_energy` 是点燃时写入燃烧产物的显式热源，必须计入 DeltaBatch 的
`ExplicitEnergyDelta`。能量是
确定性的 `uint16` 比能；合并和转移使用 `Amount × Energy` 的 `int64` 中间值。除显式配置的热源、
排放和冷却外，提交必须守恒总能量与材料量。

局部反应拥有独立于流体/粒子活跃窗口的 fixed-step 时钟；其确定性步号进入 V6 存档。接触先稳定
排序，每个接触完成传导和局部规则后立即检查点燃，冷却每个被触及 Element 每步只应用一次。
每帧总提交预算在 World→Source、Volume 内部和跨 Volume 接触之间保留确定性配额，并按已保存步号
轮转 Source 顺序，避免持续活跃的前几个实例使其他接触永久饥饿。

火、烟、蒸汽均为普通材料。“正在燃烧”只是高能可燃材料及邻近火焰粒子的查询结果，不是持久状态。
Lua Schema 升级后，`reaction.define` 不再接受 `propagating`；旧内容得到包含字段名和 schema version 的
明确错误。燃烧体验允许按新能量模型全面重调。

## 分阶段实施

### 当前实施快照（2026-08-29）

- Volume Core、Span/Dense 逻辑哈希、切割器、连通/Seam、质量属性、地形 Span 覆盖、快照与 Delta
  编码已落地，并已有自动化测试覆盖。
- Source Volume、World Cell 和稳定 Airborne Particle 已接入同一个局部 Kernel；燃烧、导热、冷却、
  腐蚀与排放以 `FMaterialDeltaBatch` 表达，Lua Schema 已切换到局部规则。
- 静态 Source 移交为动态刚体 carrier 时会保留稳定 SourceId、Topology/Field Revision、环境能量和
  每个稀疏材料/能量格。carrier 可直接解析稳定 Volume 地址，内部同层四邻接及接触中的跨层材料格
  使用同一个局部 Kernel 推进；多格结果先完整校验，再一次发布并使每个受影响层的 Revision 只增加一次。
- carrier 稀疏材质替换已进入 WholeObject 网格投影；能量仍只改变字段和火焰/烟雾派生表现，不改变占用
  或碰撞。孤立且没有材料邻居的热格不会维持无效 fixed-step 调度。
- 两个独立动态 carrier 之间会用稳定 Element 地址发现最近接触并生成一个父 `DeltaBatch`；提交时双方
  完整验证，任一 stale base 或网格失败都会恢复两个 carrier。内部、跨 carrier 与静态 Source 各有
  固定预算保留，不能长期互相饿死。
- authored material impact 即使位于 MaterialWorld 当前 active window 之外，也会以成功写入的 pending
  稳定格加入下一局部 fixed step；静态 Source 子预算耗尽不会跳过同格或后续格的动态 carrier。
- 热 Volume 在能量恢复到环境值前禁止 dematerialize，避免局部 fixed step 丢失仍需推进的材料事实。
- Gas 接触保持命中实际 aggregate member；只有 Liquid 的整体破坏事务会重定向到 aggregate root。
- 动态 carrier 已不再持有或推进 `FSourceReactionRuntime`；火焰与烟雾完全由稀疏格能量派生。
- Ground 已切换为稳定 terrain Volume 地址：程序地形是隐式基线，Span 覆盖是唯一材料拓扑，
  `FMaterialVolumeFields` 是唯一能量事实。列拓扑与能量共用同一 V6 保存/复制载荷，先完整校验再原子替换；
  旧 Ground runtime、Active/Output Mask、Chunk codec 和专用复制 Actor 已删除。
- 旧静态/逻辑 Source 与 Ground reaction runtime、活动索引、专用 Ground 复制 Actor 和 Chunk codec
  已删除。旧 Mask/高度字段只作为资产、V5 读取和兼容传输 adapter；提交后始终重新投影自同一
  Volume 事务，不再构成独立材料或反应事实。
- 地形 Cave Chunk 已按暴露的顶部、底部和侧壁生成 Volume 网格与碰撞；空中粒子以三维轨迹查询
  最近暴露面，并用 `XY + SurfaceN + Face` 稳定键保存、复制和恢复沉降面。
- 运行时 Source Actor 使用包含 InstanceId、Topology/Field Revision、环境能量和稀疏材料格的
  Volume 快照；世界拥有的流送 Source 使用 revisioned FastArray。姿态仍由 Movement Replication
  运输，Field-only 更新不会重建网格或碰撞。逻辑热源查询按稳定 SourceId 合并 World 权威状态与
  Actor 投影，不受投影复制先后影响；MaterialWorld 原子压缩快照使用 8 KiB 硬上限。
- 灰度开关已经随单写者切换完成而删除；Volume/Span 与局部反应是唯一运行路径。
- 最终回归：`MatterFlux.Volume` 15/15、`MatterFlux.Material` 59/59（其中 LocalReaction 9/9）、
  `MatterFlux.Reaction` 16/16、`MatterFlux.Save` 7/7、`MatterFlux.Lua` 25/25、
  `MatterFlux.Performance` 12/12；Dedicated Server + 两初始客户端 + 真实 Late Join 测试，以及两玩家
  近区块压力场景通过。
  连续遍历 1639 帧的 playable-world Tick p99 为 6.28 ms、最大 12.13 ms、0 慢 Tick、0 可见卡顿；
  大世界 120 个反应步为 1162.45 ms。V5 旧 ReactionState 字段仅保留为 deprecated 检测结构，
  V6 不写入、不恢复也不复制这些字段。

### 1. 冻结基线与可观测性

- 落地本计划和 ADR-015；为 ADR-006～011、ADR-014 增加部分取代说明。
- 记录切割、燃烧、腐蚀、存档、专服同步和性能基线。
- 统计布尔运算、连通、反应、网格、碰撞、提交和同步字节。

### 2. Volume Core 与材料基础

- 实现 Span 标准化、差集、布尔切割、不可变 Chunk Builder、确定性逻辑哈希、质量/质心/惯性、结构连通和 Span/Dense adapter。
- 实现旧 Mask X/Z/Y 与地形单高度 X/Y/Z 的只读转换器。
- 实现 Material Element、稀疏能量字段、接触排序、局部 Reaction Kernel 和原子 DeltaBatch。
- 同时定义快照与增量编码；每个权威切换必须先接入存档和复制。

### 3. 有界 Volume 原型

- 在纯测试载体中验证球、定向盒、胶囊、Plane Slab 和 Swept Blade。
- 接入 Greedy Mesher、低分辨率碰撞、异步数组生成和主线程预算提交。
- 验证旋转实例切割、结构 Seam、跨材质连通、事务回滚和重复切割。
- 原型门禁通过前不修改玩家可见地形。

### 4. 可切割物体与局部反应

- 树、墙、房屋、家具和动态碎片以对象局部 Volume 为权威；旧类名和资产名保持不变，旧 Mask 仅作加载输入。
- Volume Cell、空中粒子和移动材料格通过 contact adapter 进入同一个 Reaction Kernel。
- 燃烧由能量传递、点燃阈值、材料转化及火/烟排放驱动；腐蚀、水火、熔岩水使用同一 DeltaBatch。
- Source 切换完成后删除 `FMaskReaction`、`FSourceReactionRuntime`、Source ActiveMask、Accumulator 和活动反应索引。
- 子碎片速度为 `Vchild = Vparent + ω × (COMchild - COMparent)`，并重算质量、质心和惯性。

### 5. 地形、多表面材料与洞穴

- 地形采用隐式程序基线加稀疏多 Span 覆盖；恢复基线的列删除覆盖。
- 保留最高表面兼容查询，新增整列、指定高度实体、最近表面和暴露面查询。
- 单表面 Chunk 保留高度场路径；镂空 Chunk 使用 Volume 网格和碰撞。
- settled 材料使用包含 XY、SurfaceN 和 Face 的稳定表面键；空中粒子按真实三维轨迹落入洞穴内部表面。
- 支撑分析只遍历脏区域，从脏区边界及世界底部播种，不扫描整个世界。
- Ground 切换完成后删除专用 Reaction runtime、Active/Output Mask 和复制路径。

### 6. 存档、复制与事务完整性

- SaveVersion 提升至 6，MaterialWorld 二进制状态提升版本并为所有 Material Element 编码 Energy。
- V5 若含任意 Source/Ground ReactionState，加载前明确拒绝且不修改世界；无 ReactionState 的 V5 可转换高度覆盖、Mask 和现有材料状态。
- Legacy Reaction 字段以 deprecated 读取结构保留一个版本，只用于检测不兼容，绝不恢复旧 runtime。
- V6 保存局部反应 fixed-step 步号；恢复时清空墙钟 accumulator，并从保存步号继续确定性概率与粒子 ID。
- Volume Delta 携带 InstanceId、Topology/Field 基础与目标 Revision、Chunk 变化和结果哈希；Revision 不匹配时请求快照。
- 动态刚体姿态继续由 Movement Replication 运输，不进入 Volume Delta。
- Dedicated Server 权威提交；第一版不做客户端反应或切割预测。

### 7. 灰度与清理（完成）

- 已按有界原型 → 静态物体 → 动态碎片 → 地形 → 洞穴材料顺序完成单写者切换。
- 已删除 `mf.Volume.EnableFragmentVolumes`、`mf.Volume.EnableTerrainSpans`、
  `mf.Material.EnableLocalReactions`，不再保留可回到双权威运行时的开关。
- 已删除对象级 ReactionState 运行时/编解码、旧 Lua 传播字段和专用 Ground 复制路径。
- V5 deprecated 读取结构继续保留一个版本，仅用于在应用世界前检测并拒绝旧活动反应存档。

## 测试与验收

- Volume Core：轴转换、半开区间、跨材质连通、Seam、斜切量化、Span/Dense 等价哈希、Chunk 回滚和确定性 Component 排序。
- Reaction Kernel：能量传导、冷却、点燃、熄灭、腐蚀、熔岩水、排放守恒、输入顺序无关、固定预算和保存恢复。
- 物体：三个局部轴及 30°/45°/60° 切割、部分深度、前后分离、旋转碎片再切割、多材质房屋拆分和动量一致性。
- 地形：封闭空腔、贯通隧道、内部顶部/底部/侧壁、洞穴材料沉降、Chunk 边界无裂缝。
- 存档：含旧 ReactionState 的 V5 明确失败且零副作用；无反应 V5 自动转换；V6 往返哈希一致。
- 联机：Dedicated Server + 两客户端 + Late Join；乱序 Delta 请求快照；拓扑、能量、材料量和派生 FragmentId 一致。

## 性能门槛

| 指标 | 初始门槛 |
| --- | ---: |
| 切割调度主线程 P95 | `< 0.5 ms` |
| Worker 布尔切割 + 连通 P95 | `< 8 ms` |
| 网格/碰撞主线程提交 P95 | `< 2 ms` |
| 新系统增加帧卡顿 P99 | `< 4 ms` |
| 单事务物理碎片 | `<= 16` |
| 常规切割平均/P95 同步量 | `< 32 KB / < 128 KB` |
| 压力场景 Volume 常驻内存 | `<= 阶段 1 基线的 2 倍` |

强制约束：未编辑地形不得整体物化；重网格与碰撞数组生成必须能离开主线程；游戏线程提交有预算。

## 完成定义

- 代码、存档和复制中不存在对象级 ReactionState。
- 地形可形成、保存、同步并碰撞封闭洞穴和贯通隧道，且未编辑地形保持隐式。
- 树、房屋与旋转掉落物可从任意方向、任意深度重复切割；多材质对象按三维结构连接拆分。
- 切割、反应、质量、支撑、存档和复制读取同一 Volume/Material Element 事实。
- 网格、碰撞、Actor 和表现缓存均为可重建投影。
- V5 兼容/拒绝行为、V6 往返、专服与 Late Join 门禁全部通过。
- 普通切割和帧预算达到上述性能目标。
