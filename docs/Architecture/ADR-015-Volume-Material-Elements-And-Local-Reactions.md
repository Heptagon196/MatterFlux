# ADR-015：Volume Material Elements 与局部反应

- 状态：已采纳，已实施
- 日期：2026-08-29
- 部分取代：ADR-006、ADR-007、ADR-008、ADR-009、ADR-010、ADR-011、ADR-014

## 背景

二维 Fragment Mask、单高度地形、Surface MaterialWorld 和对象级 ReactionState 分别维护占用、
材料与反应进度。这使任意方向切割、洞穴内部表面、能量传播、存档和网络同步需要跨多个容器
维持重复事实。传播反应的 ActiveMask 还把“正在燃烧”固化成对象生命周期状态，无法自然统一
固体格、地表格和空中粒子。

## 决策

### Volume 是固体占用与结构的唯一事实

可切割物体和编辑地形使用按 U/V 列组织的三维 Material Volume。拓扑包含不可变 16×16 Chunk
快照、`int32` 半开 Span、结构锚点和稀疏 Seam。默认六面连接；跨材质接触保持结构连通，只有
Seam 显式断开。

Topology Revision 只在占用、材质或结构连接改变时递增。实例的世界姿态、线速度和角速度属于
`FMaterialVolumeInstance`，不进入 topology hash 或 delta。逻辑 hash 按排序后的材料格与结构连接
计算，Span/Dense adapter 的物理布局不得影响结果。

### Energy 是独立的稀疏字段

每个材料格拥有确定性的 `uint16` 比能。默认能量来自材料定义；只有不同于环境/默认值的格才写入
`FMaterialVolumeFields`。Field Revision 独立递增，能量变化不得触发 topology mesh/collision rebuild。

### 所有反应都是 Material Element 的局部接触

World Cell、Volume Cell 与 Airborne Particle 都通过稳定 `FMaterialElementAddress` 寻址。Particle
必须使用稳定 ID，数组位置不构成身份。接触 adapter 只产出已去重、确定性排序的
`FMaterialContact`。

`FLocalMaterialReactionKernel` 读取不可变视图、局部接触规则和 fixed-step context，返回
`FMaterialDeltaBatch`。Kernel 不访问 Actor、World、网络或存档，也不直接修改输入。Batch 在写入
前验证全部地址、数量、能量、base/target revision、预算和结果 hash；任何失败都不产生部分修改。

反应定义不再拥有 Propagating、DurationSteps、传播概率或 ActiveMask。燃烧由能量传导、点燃阈值、
材料转换、显式配置的燃烧热及普通 fire/smoke 排放构成。每个有序接触完成后立即检查点燃；冷却
在 fixed step 末对每个被触及 Element 只应用一次。“正在燃烧”是派生查询，不可保存或复制。

固体局部反应的 fixed-step 不依赖流体活跃窗口。权威步号进入 V6 存档，并用于整数概率和稳定粒子
ID；运行时墙钟 accumulator 不保存。预算按 World→Source、Volume 内部和跨实例接触分区，Source
顺序随已保存步号确定性轮转，因此总预算受控且不会由固定 GUID 前缀长期垄断。

### 权威与投影

Dedicated Server 是 Volume、Material Element 和 DeltaBatch 的唯一权威提交者。动态运动继续使用
Movement Replication。客户端第一版不预测反应或切割；Volume Delta base revision 不匹配时丢弃
整批并请求快照。

Actor、ProceduralMesh、碰撞、WholeObject、Source proxy、火焰/烟雾表现和活动索引均为可重建投影，
不得保存第二份材料、能量或反应事实。

程序地形本身保持隐式；terrain Volume 使用由地图种子派生的稳定 `InstanceId`，只保存偏离基线的
Span 列与非环境能量。一个列载荷可仅包含 Field 编辑而不物化其基线拓扑。网络或存档解码必须先在
候选 overlay/fields 上验证全部材质名、半开区间、唯一能量地址与占用关系，再一次替换现场状态。
Ground 不得再拥有 Active/Output Mask、传播 accumulator 或专用复制 Actor；地表火焰、烟雾与灰烬
实例均从 terrain Span 材料和能量派生。

动态 Source 移交给刚体 carrier 时，稳定 Volume 地址、Topology/Field Revision、环境能量和稀疏格
状态必须随同移交；运动姿态仍只走 Movement Replication。非环境能量尚存时不得把 Volume 从权威
调度中 dematerialize。authoring impact 的成功写入本身构成下一 fixed step 的稳定输入，即使对应
MaterialWorld Chunk 不在 active window；active 投影或静态 Source 子预算不得使移动 Volume 饥饿。
carrier 内部接触也必须以不可变快照生成一个 batch：提交前验证全部 ExpectedBefore，任一陈旧地址使
整批失败；成功时每个受影响层只提升一次 Topology/Field Revision。稀疏材质替换必须进入可重建网格
投影，能量本身不得触发占用或碰撞重建。

跨两个刚体 carrier 的接触仍是一个父 batch，而不是两个可独立成功的写入。协调器按 OwnerId 将已验证
的父 batch 投影为两个本地候选，只有双方均可提交时才发布；任何一侧 stale 或重建失败都恢复双方材料、
Revision、网格和复制投影。

接触几何不得隐式改变目标身份。Gas/热粒子命中 aggregate member 时保留该 member 的稳定地址；
只有需要整体结构事务的 Liquid 破坏 adapter 可以显式重定向到 aggregate root。

### 兼容性

保留现有 UCLASS/资产名。旧二维 Mask 与高度覆盖只允许作为加载输入转换成 Volume。SaveVersion 6
保存 Volume/Material Element 能量；V5 只有在完全不含 Source/Ground ReactionState 时可以转换。
发现旧 ReactionState 必须在应用世界前失败，错误包含对象种类和稳定 ID，且加载零副作用。

## 不变量

- 相同逻辑格、Seam 和锚点产生相同 topology hash，与 adapter、分配和 TMap 迭代顺序无关。
- 世界变换或运动不改变 topology/field revision。
- 相邻不同材质默认连通；只有显式 Seam 能断开对应六面连接。
- 数量与比能运算使用 `int64 Amount × Energy` 中间值；除显式 source/sink 外守恒。
- Contact 输入顺序和容器插入顺序不改变 DeltaBatch。
- Batch 校验或提交失败时，Topology、Fields、World Cells 和 Particles 全部保持原样。
- 同一实例任一时刻只有一个权威写入者；Shadow Compare 不能提交。
- 网格、碰撞、Reaction query 和表现不能反向写入权威材料事实。

## 后果

正面结果是任意方向重复切割、洞穴多表面、粒子/固体统一反应、确定性存档复制和纯数据异步计算
拥有同一 seam。Field-only 变化不再迫使几何重建。

代价是 V5 活跃反应存档明确不兼容，燃烧节奏已按量化能量模型重调。迁移使用的单写者功能开关
在切换完成后已经删除；只读 Mask/高度转换器继续承担旧资产和 reaction-free V5 输入。
Volume mesh/collision 与接触收集仍是不同 adapter，分别接受预算、测试和性能回归。

## 验证

权威测试清单位于 `MatterFlux-Volume-Span-Migration-Plan.md`。至少覆盖逻辑 hash 的 adapter 等价、
跨材质连通与 Seam、原子回滚、能量守恒、乱序 delta 请求快照、V5 反应状态零副作用拒绝，以及
Dedicated Server/两客户端/Late Join 一致性。
