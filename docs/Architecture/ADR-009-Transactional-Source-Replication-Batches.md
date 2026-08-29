# ADR-009：逻辑 Source 状态按 fixed step 事务式批量复制

- 状态：已采纳并实施
- 日期：2026-08-10
- 取代说明：ADR-015 部分取代本决策。旧 Source Reaction Fast Array 由带 Revision/Hash 的 Volume Delta 与快照协议取代。

## 背景

ADR-008 删除了 runtime/fuel 双份真值，但世界 Actor 仍在每个逻辑 Source 的燃烧推进后
分别执行：

1. 为 runtime、residue、burning 创建三份临时 packed 数组；
2. 单独更新 Fast Array；
3. 单独调用一次 `ForceNetUpdate()`。

因此同一个 fixed step 有 16 或 64 个 Source 变化时，会形成同样数量的临时数组集合和网络
唤醒。单项 Fast Array 查找已经是 O(1)，真正的 seam 应位于“一个模拟步骤产生的一批逻辑
状态”与“UE Fast Array 网络 adapter”之间。

## 决策

新增 `MatterFluxFragmentSourceReplication.h`，集中声明 replicated item、Fast Array store 和
唯一运行时写入 interface：

```text
UpsertAuthorityBatch(updates, maxItems, maxPayloadBytes)
```

每个 update 只借用一个 `SourceId` 和 `FFragment2DSourceStreamingState`；调用期间同步消费，不
复制三张原始 mask。模块内部完成：

- 按 GUID 四个整数分量稳定排序，不转字符串、不依赖 `TMap` 遍历顺序；
- 先校验整批 SourceId、revision、mask 值/尺寸、燃烧余量和烟雾计数；
- 再预计算整批条目预算和 payload 字节预算；
- 任一成员无效或超预算时不修改 Fast Array、revision、payload 或缓存计数；
- 仅在全批通过后 bit-pack，并直接写入目标 item 已有数组以复用容量；
- 保留已有 Fast Array item 的 replication ID/key，只标记实际提交的 item dirty。

旧的 `UpsertAuthorityItem` 已删除，生产代码与测试都不能绕过批事务。`AdvanceLogicalSourceCombustion`
先完成本地状态同步，再一次提交该 fixed step 的全部 Source，并只调用一次 `ForceNetUpdate()`。
存档恢复中的多个 Source 也在全部规则和状态恢复成功后一次提交。单个点燃、切割 tombstone 等
事件仍通过同一批接口提交一个元素。

活动 ID、完成 ID、发布 ID 和 batch update 数组由 WorldActor 跨帧复用容量；常见不超过 64
项的排序索引使用 `TInlineAllocator<64>`。新增 `Source Combustion Replication` CPU Stat，可在
Unreal Insights 或 `stat MatterFlux` 中单独观察该阶段。

## 不变量

- 一次 batch 要么全部提交，要么完全不修改已复制状态。
- 无效状态的判定先于预算判定；条目预算判定先于 payload 字节预算。
- 同一 batch 不允许出现重复 SourceId。
- runtime 和 residue 必须是二值 mask；burning 只复制 presence，因此非零剩余时长均编码为 1。
- 三张燃烧 mask 必须具有相同 cell 数。
- 本轮不降低 fixed-step 模拟频率，也不丢弃中间已提交步骤；仅合并同一步内的编码与网络唤醒。
- 晚加入客户端仍由 Fast Array 当前完整状态恢复，不依赖历史 RPC。

## TDD 与验证

第一条 RED 使用乱序的三个 Source，stub 返回 `InvalidState`，测试明确得到 0 个条目；GREEN
后按 SourceId 稳定提交并得到 `0x55/0xaa/0x33` 三个 packed mask。第二条测试让 batch 中第一
个更新有效、第二个 mask 非法，确认两项旧 revision、payload 和总字节计数全部不变。

最终结果：

- Source 网络事务测试：3/3；
- 燃烧测试：19/19；Save 测试：5/5；
- Listen Host + Client PIE：1/1；逻辑 Source 双客户端复制：1/1；
- 256 次重复编码的平均单批耗时：1 Source 0.005ms、16 Source 0.082ms、64 Source 0.332ms；
- 4096 项列表上的 8192 次 logical-state 更新：16.77ms；
- 完整 `Automation RunTests MatterFlux`：182/182，0 失败、0 未运行；
- Editor Development、Game Development、Game Shipping 均使用 MSVC 14.44.35222 构建成功。

主要日志：

- `Saved/Logs/MatterFluxSourceBatchRed.log`
- `Saved/Logs/MatterFluxSourceBatchNetworkFinal.log`
- `Saved/Logs/MatterFluxSourceBatchAllPerformance.log`
- `Saved/Logs/MatterFluxSourceBatchListenHostClient.log`
- `Saved/Logs/MatterFluxSourceBatchLogicalNetwork.log`
- `Saved/Logs/MatterFluxSourceBatchFull.log`

## 后果

同时燃烧 N 个 Source 时，网络唤醒从每步 N 次降为一次；重复更新不再先创建 candidate 的三份
临时 packed 数组，而是复用 Fast Array item 的数组容量。调用方只需表达“本步骤哪些 Source
变化”，位打包、预算、稳定顺序和失败语义都留在一个深模块 interface 后面。

原本遗留的客户端全历史扫描已由
`ADR-010-Incremental-Client-Source-FastArray-Apply.md` 解决。服务器批事务与客户端 delta
应用共同组成复制管线；晚加入、代理重建和 delta 解析失配仍保留明确的完整快照慢路径。
