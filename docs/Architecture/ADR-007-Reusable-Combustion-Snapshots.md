# ADR-007：燃烧快照由调用方持有并重复复用

- 状态：已采纳并实施
- 日期：2026-08-10

## 背景

逻辑 Source 每个 fixed step 都要把 fuel、residue、burning 三张 mask 同步到流式持久状态。
旧实现有两层浪费：

1. `CaptureState` 先把输出对象重置为空，释放调用方已经拥有的数组容量，再为同样尺寸重新
   分配；未初始化 runtime 的失败捕获也会清空调用方最后一份有效快照。
2. 世界 Actor 先捕获到局部 `FSourceRuntimeSnapshot`，再把三张 mask 复制进
   `FFragment2DSourceStreamingState`。因此一次同步同时承担临时分配与第二轮整 mask 复制。

mask 尺寸在同一个 Source 生命周期内不变，这些成本没有产生新的领域信息。

## 决策

`CaptureState(OutState)` 的 interface 现在保证：

- 成功时覆盖最新值，但复用 `OutState` 三个 `TArray` 已有容量；
- 失败时不修改 `OutState`，调用方保留最后一次已提交快照；
- 捕获格式、确定性字段和恢复校验保持不变。

`FFragment2DSourceStreamingState` 直接扩展 `FSourceRuntimeSnapshot`，再增加 revision、通用
runtime mask 与是否存在燃烧状态等流式元数据。世界 Actor 因而可以把 runtime 直接捕获到
持久 map 中；存档恢复、Actor→逻辑归档以及已有燃烧 Source 再点燃，也直接把流式状态传给
`RestoreState`，不再创建中间三-mask 对象。

底层 `FMaskCombustion`、Source runtime 与 Ground runtime 的捕获都采用相同的失败原子性。
Ground 的数据类型没有变化，只获得了容量复用行为。

## 不变量

- 捕获失败不得擦除调用方最后一次有效快照。
- 相同尺寸的连续成功捕获不得主动缩减调用方预留的 mask 容量。
- `RestoreState(CaptureState())` 仍必须逐字段恢复 fixed-step 时间债、随机 Tick 与烟雾计数。
- 流式状态是一份 runtime snapshot 加流式元数据，不再平行维护另一份燃烧 snapshot。
- 网络和存档格式保持兼容；本轮不改变发布频率或客户端模拟策略。

## TDD 与验证

专项 RED 首先稳定复现两个失败：成功捕获丢失预留容量、失败捕获清空已提交状态。移除输出
预清空并让流式状态直接承载 snapshot 后转绿。

最终结果：

- 4096 次三张 4096-cell mask 捕获：独立进程 0.83 ms，完整共享套件中 0.24 ms；
- 65,536 个已熄灭历史 Source 后，32 个当前火源执行 10,000 次稳定刷新：独立进程
  5.65 ms，完整共享套件中 6.87 ms；
- `MatterFlux.Combustion`：18/18；
- 完整 `Automation RunTests MatterFlux`：178/178，0 失败、0 未运行；
- Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。

日志位于 `Saved/Logs/MatterFluxSourceSnapshotReuseRed.log`、
`MatterFluxSourceSnapshotReuseGreen.log`、`MatterFluxSourceSnapshotThroughput.log`、
`MatterFluxLogicalSourceHistoryPerformance.log` 和
`MatterFluxReusableSourceSnapshotFull.log`。

## 后果

Source fixed-step 同步不再为临时 snapshot 反复分配，也不再把三张 mask 从临时对象复制到
持久对象；重复捕获的数组存储由持久 Source 状态自然复用。代价是流式状态与 runtime
snapshot 明确形成继承关系，未来向 snapshot 增加确定性字段时会自动进入流式内存模型，
仍需显式决定该字段是否进入存档和网络编码。

后续 ADR-008 已消除 `RuntimeMask` 与燃烧 `FuelMask` 的语义重复：燃烧态直接以 fuel 为
runtime 真值，每个 Source 的 mask 存储从四张降为三张。剩余热点是每个 fixed step 的
三-mask 打包与逐条 `ForceNetUpdate`。
