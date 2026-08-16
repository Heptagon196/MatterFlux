# ADR-001：UE 对象作为 Adapter，领域复杂性进入 Deep Module

- 状态：接受
- 日期：2026-08-08

## 背景

MatterFlux 同时使用 Actor 生命周期、网络复制、GAS、Slate/UMG、Lua 内容和纯 C++ 模拟。随着功能增加，若每个新行为都直接写入 Actor、UActorComponent 或 UUserWidget，UObject 会同时承担引擎接入、领域规则、数据格式和视图构建，最终形成很宽的 interface。

设置页曾在 Shell 与魔法工作台中重复实现，是这个问题最直观的信号。相同问题也出现在世界 Actor 的复制格式和巨型 Slate 编译单元中。

## 决策

1. Actor、Subsystem、UActorComponent 和 UUserWidget 是 UE adapter，负责生命周期、反射、复制、输入绑定和 UObject 所有权。
2. 算法、状态转换、编解码、布局和视图树进入普通 C++ 或私有 Slate deep module。
3. module 的 interface 必须明显小于 implementation；只有 create、refresh、build、encode/decode 一类高 leverage 操作可以跨 seam。
4. 共享规则必须只有一个所有者。视觉 token 由 `MatterFluxPaperStyle` 拥有，设置行为由 `SMatterFluxSettingsPanel` 拥有，物质复制 wire format 由 `FMatterFluxReplicatedMaterialState` 拥有。
5. 测试优先穿过 deep module interface；只有复制、生命周期和引擎集成行为使用 PIE/World 测试。
6. UE module 的 public dependency 只为编译 public header 所需；implementation 依赖放入 private dependency。

## 当前落地

- `MatterFluxShellSlate` 与 `MatterFluxMagicWorkbenchSlate` 隐藏 Slate implementation。
- `MatterFluxPaperStyle` 提供共享黑白主题。
- `MatterFluxReplicatedMaterialState` 隐藏压缩和 net serialization。
- `FChunkedMaterialWorld` 通过一个多焦点 interface 隐藏排序、去重、公平 round-robin、硬预算、归档和 v1/v2 快照规则。
- `FGroundCombustionRuntime` 统一持有地表燃烧 fixed-step、可见 mask、dirty chunks、revision、客户端去重和存档时间债；`AMatterFluxPlayableWorldActor` 只负责坐标换算、网络 Actor 与 ISM 显示。
- `FGroundCombustionRuntime` 还输出稳定的 changed-cell 集合和按区块稀疏的 burning/residue 查询；Actor 不读取或重建其私有索引。
- `FSourceSpatialIndex::QueryMany` 隐藏多 bounds 的桶遍历、去重、精确相交和确定性排序，传播调用者不再自己拼 `TSet` 与二次排序。
- `FMatterFluxGroundStateChunk` 位于 Material 领域而非 Game Actor 头文件；编码、CRC、压缩和 NetSerialize 只有一个所有者，复制 Actor 只运输已完成的原子 payload。
- `UMatterFluxMagicInventoryComponent::ApplyProgressionEffectsAuthority` 隐藏奖励与法力整批 prepare→commit；调用者不再按奖励类型逐项修改实时状态。
- `FragmentGeometry` 和 `FMatterFluxSpellProgramLayoutBuilder` 已符合相同方向。
- `FragmentSourceChunks` 保存逐 Source 的逻辑定义与运行时 mask；`UMatterFluxFragmentSourceProxyComponent` 只负责按区块、材质、正侧面和碰撞策略合并显示。交互切割即时刷新；燃烧中的高频 mask 更新与昂贵实体网格重建解耦，最终态按 chunk 批量合回。
- `FMatterFluxReplicatedFragmentSourceState` 只运输发生过修改的 Source revision 与 1-bit mask，晚加入客户端无需依赖短命 Source Actor 的生成/销毁历史。
- `MatterFluxSessionRecordingCodec` 独占录制 schema、预算、payload 校验、规范化排序和 JSON 编解码；`FReplayRuntime` 独占时间线游标、持续输入、hitch 状态折叠/插值、稳定输出顺序与一次性完成信号。`UMatterFluxSessionRecorderSubsystem` 不再包含 JSON implementation 或回放游标，只负责 GameInstance 生命周期、World 采样、文件、截图和 Character/Viewport adapter。失败解码只修改错误文本，调用者已有 recording 保持不变。
- `MatterFluxDeveloper` 是从 Target 依赖图隔离的 DeveloperTool module：视觉/UI 捕获、稳定帧序列、树木点燃和自动法杖触发命令单向依赖 Runtime 的公共 interface；`MatterFlux` 不知道该 module，也不保存开发命令名。Development/Editor 保留外部验收能力，Shipping 排除整组 implementation。

## 后果

正面：状态和规则获得 locality；UI 或复制格式只需修改一个 module；适配器文件可以快速审阅；纯 computation 可以不启动 World 进行测试。

代价：私有 module 之间需要明确的窄 interface；重构期间会增加少量文件；跨状态所有者的事务必须显式设计，不能靠调用顺序假装原子性。

## 禁止模式

- 为了“复用”创建只转发一次调用的 shallow module。
- 把整个 Actor 传给纯规则模块，让 implementation 继续任意读取字段。
- 为测试暴露私有状态 getter。
- 在两个 UI 页面复制颜色、字体、描边或设置行为。
- 因为文件很长就按固定行数切分；先找稳定 seam。
