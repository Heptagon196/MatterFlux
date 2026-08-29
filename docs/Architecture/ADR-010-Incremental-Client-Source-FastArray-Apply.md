# ADR-010：客户端按 Fast Array delta 应用逻辑 Source

- 状态：已采纳并实施
- 日期：2026-08-10
- 取代说明：ADR-015 部分取代本决策。客户端新路径只原子应用匹配 base revision 的 Volume Delta，否则请求快照。

## 背景

ADR-009 已让服务器按 fixed step 批量编码和提交逻辑 Source，但客户端收到任一 add、change
或 remove 后仍只设置一个全局 dirty 标记。下一次应用会遍历最多 4096 个 replicated item，
并再次遍历所有历史 Applied ID 来找删除项。一次只改变一棵燃烧树时，客户端工作量仍随整个
探索历史增长。

Fast Array 的回调有一个重要生命周期约束：`PreReplicatedRemove` 的数组下标只在删除前有效，
`PostReplicatedAdd/Change` 的下标只对当次最终数组有效。不能把这些下标保存到下一帧使用。

## 决策

`FMatterFluxReplicatedFragmentSourceStateList` 现在同时拥有客户端 apply-plan 队列：

- remove 回调在元素仍存在时复制稳定的 `SourceId`；
- add/change 回调只记录 Fast Array 的 `ReplicationID`；
- 整个 delta 接收完成后只通知 WorldActor 一次；
- 消费计划时通过 Fast Array 当前 `ItemMap` 把 `ReplicationID` 解析为当前下标；
- upsert 与 remove 都按 GUID 的 A/B/C/D 分量稳定排序并去重；
- 同一 Source 在一个接收批次中先删除再加入时，以最终 upsert 为准；
- 任一 ID 无法解析、下标失配或 SourceId 非法时，丢弃整个增量计划并请求一次完整重建。

`AMatterFluxPlayableWorldActor` 只负责 UE adapter 工作：消费计划、把有效 item 解包进
`FFragment2DSourceStreamingState`、更新代理和燃烧活动索引，并在批次末尾只 Flush 一次。
初次复制、晚加入、关卡定义/代理重建以及缺失代理期间仍显式走完整快照路径。

## 不变量

- 队列不保存跨帧可能失效的 Fast Array 数组下标。
- 普通 delta 的 CPU 工作只随 add/change/remove 数量增长，不随历史 item 总数增长。
- 解析失败不能暴露半份 upsert/remove 计划。
- 删除与重加冲突时，最终列表中的 item 获胜。
- 完整重建永远可以只从 replicated 当前真值恢复，不依赖过去是否收到某个回调。
- 客户端不改变服务器权威 revision；比本地状态旧的复制 revision 不回退本地状态。

## TDD 与验证

第一条 RED 在 4096 个历史 Source 中只改变下标 4095 和 7；stub 仍返回 full rebuild 且 0 个
upsert。GREEN 后只返回两个按 SourceId 排序的当前下标。后续测试覆盖重复删除稳定去重、
同批删除/重加以 upsert 为准，以及 `ReplicationID` 无法解析时原子回退完整重建。

最终结果：

- 客户端 apply-plan 行为测试：3/3；
- 4096 条历史下，1/16/64 个 delta 的平均计划成本为 0.000/0.000/0.001ms；
- Listen Host + Client PIE：1/1；Dedicated Server world + 两客户端：1/1；
- 完整 `Automation RunTests MatterFlux`：186/186，0 失败、0 未运行；
- Editor Development、Game Development、Game Shipping 均使用 MSVC 14.44.35222 构建成功。

主要日志：

- `Saved/Logs/MatterFluxClientDeltaPlanTests.log`
- `Saved/Logs/MatterFluxClientDeltaPerformance.log`
- `Saved/Logs/MatterFluxClientDeltaListenHost.log`
- `Saved/Logs/MatterFluxClientDeltaTwoClients.log`
- `Saved/Logs/MatterFluxClientDeltaFull.log`

## 后果

逻辑 Source 复制现在两端都以变化量为常规成本：服务器按 fixed step 事务批提交，客户端按
Fast Array delta 批应用。全量扫描不再是每次收包的默认行为，而是可恢复性所需的明确慢路径。
下一阶段若继续优化，应测量 `ApplyReplicatedFragmentSourceState` 内真实 mask 解包与代理网格
刷新，而不是继续微调已经低于 0.001ms 的 apply-plan 排序。
