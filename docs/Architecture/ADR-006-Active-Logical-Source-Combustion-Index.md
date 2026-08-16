# ADR-006：逻辑 Source 燃烧使用可重建活动索引

- 状态：已采纳并实施
- 日期：2026-08-10

## 背景

`StreamedFragmentSourceStates` 是逻辑 Source 的持久真值。树木熄灭后，它仍需要保存燃料、
木炭残渣、revision 和 fixed-step 状态，供流式返回、晚加入客户端与存档恢复使用。因此这张
表会随玩家探索和破坏逐渐增长，这是正确行为。

旧的火焰/烟雾刷新却每 0.1 秒遍历整张持久表，再检查每份 `BurningMask`。世界历史越长，
当前只燃烧一棵树的表现成本也越高。`GetCombustingSourceCount` 还有一份相同的全表扫描，
燃烧推进与传播则分别把 GUID 转成字符串排序，产生不必要的短命分配。

区块代理已经在燃烧期间延迟实体几何重建，熄灭后才批量重建最终网格；因此本轮真正需要
解决的是“活动表现依赖历史状态”，不是再次改造 mask→轮廓管线。

## 决策

新增普通 C++ deep module `FLogicalSourceCombustionIndex`。它只接受最新快照，并维护当前
`BurningMask` 至少含一个非零 cell 的 SourceId：

```text
ApplySnapshot(SourceId, hasCombustionState, burningMask)
Remove(SourceId)
Reset()
GatherStableIds() -> 当前燃烧 SourceId
```

熄灭不会删除 `StreamedFragmentSourceStates` 中的持久快照，只会从活动索引移除 SourceId。
索引是派生数据，可以从持久表完整重建。服务端点火/步进、Actor 与逻辑运行时交接、存档
恢复、客户端 Fast Array 应用、状态移除和世界销毁都更新同一索引。

火焰/烟雾 ISM、燃烧传播和公开计数只遍历活动索引。输出按 `FGuid` 的 A/B/C/D 四个
`uint32` 分量排序，不依赖 `TSet` 顺序，也不创建 GUID 字符串。

## 不变量

- 持久状态表回答“这个 Source 最后是什么状态”；活动索引只回答“现在是否仍有火”。
- 只有至少一个非零 burning cell 才能进入活动索引；只剩残渣不能产生持续表现工作。
- 相同活动集合必须产生相同 GUID 顺序，与插入顺序和平台无关。
- 晚加入客户端应用完整 Fast Array 后，必须重建出与服务器相同的活动集合。
- 读档、材质化、归档、删除和世界重生成不能留下悬空活动 SourceId。
- 索引失败或丢失时可以从持久快照重建，索引本身不复制、不存档。

## TDD 与验证

专项测试先保留旧的“只要有燃烧元数据就常驻”语义，稳定得到三个断言失败：熄灭 Source
仍在索引中、历史数量仍为 2、全部熄灭后仍不为空。实现按当前 burning cell 更新 membership
后转绿，并接入完整生命周期。

最终验证：

- `MatterFlux.Combustion`：17/17；
- `MatterFlux.Playable.StreamingArchivesCombustingAndResidueSources`：1/1；
- `MatterFlux.Performance.LargeWorldStreamingMovementAndCombustion`：通过；
- 完整 `Automation RunTests MatterFlux`：176/176，0 失败、0 未运行；
- Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。

日志位于 `Saved/Logs/MatterFluxLogicalSourceIndexRed.log`、
`MatterFluxLogicalSourceIndexGreen.log`、`MatterFluxCombustionIndexIntegration.log`、
`MatterFluxCombustionStreamingGreen.log`、`MatterFluxLogicalSourceIndexPerformance.log` 和
`MatterFluxLogicalSourceIndexFull.log`。

## 后果

火焰/烟雾刷新成本现在与当前燃烧 Source 数量相关，不再与玩家整段探索历史相关；公开
计数也不再扫描所有归档状态。代价是每个状态生命周期入口都必须同步派生索引，因此新增
入口时必须复用 `ApplySnapshot/Remove/Reset`，并保留晚加入与存档测试。

现有大世界性能测试只点燃一棵树，不能量化“许多历史已熄灭 Source”场景的收益。本轮不把
单次受机器负载影响的总 tick 数据宣称为提速；后续应增加专门的长时间探索历史基准。

该缺口已由后续门禁补齐：在 65,536 个已熄灭历史 Source 之后，32 个当前火源执行
10,000 次稳定活动集合刷新，独立进程为 5.65ms、完整共享套件为 6.87ms。测试直接锁定
“工作量随当前火源而非历史状态增长”的性能特征。
