# ADR-011：代理 Source 状态原子提交并使用稳定定位器

- 状态：已采纳并实施
- 日期：2026-08-10

## 背景

ADR-010 让客户端只应用 Fast Array 中变化的 Source，但真正进入
`UMatterFluxFragmentSourceProxyComponent` 后，runtime mask 与 residue mask 仍分别调用两个
setter。每个 setter 都先通过 `SourceId -> Chunk` 找到区块，再线性扫描区块内全部 Source；
一次成对更新因此扫描两遍。区块中存在 4096 个 Source 时，尾部 Source 的 8192 次更新需要
100.05ms。

两个 setter 也允许半提交：runtime mask 已经写入后，residue mask 才发现尺寸非法。客户端
复制路径同样曾先修改逻辑 map，再请求代理应用；代理拒绝时，逻辑状态与可视状态可能分离。

燃烧还有一项跨层语义：高频 mask 变化需要立即进入逻辑真值，但不应每一步都执行
mask→轮廓→三角剖分→ProceduralMesh/碰撞重建。客户端过去没有把 burning 状态交给代理，
因此会在每个复制更新上重建整块网格。

## 决策

代理建立 `SourceId -> { Chunk, SourceIndex }` 的稳定 locator。`SetSourceChunks` 完成复制并固定
各区块数组后一次建立 locator；下一次整体 `ResetSources/SetSourceChunks` 之前，调用方不会改变
数组结构。

删除分离的 runtime/residue setter，公开唯一深接口：

```cpp
EMatterFluxFragmentSourceProxyApplyResult ApplySourceState(
    const FGuid& SourceId,
    const TArray<uint8>& RuntimeMask,
    const TArray<uint8>& ResidueMask,
    FName ResidueMaterialId,
    const FLinearColor& ResidueColor,
    bool bCombustionActive);
```

接口先用 locator O(1) 找到 Source，再完整校验两张 mask 的尺寸和值；只有全部合法才同时更新
runtime、residue、材质、颜色和燃烧活动状态。返回值区分 `Invalid`、`Unchanged`、`Changed`，
调用方不需要从多个 bool 猜测是否发生了半提交。

燃烧 active 时只替换缓存真值，不把 chunk 放进即时 dirty 集合；true→false 时进入按 chunk
去重的 deferred 集合。批次结束再把 deferred chunk 合入 dirty 并重建一次。切割和物化等非燃烧
修改仍即时标脏。

客户端复制应用先构造完整 `FFragment2DSourceStreamingState` 候选，让代理接受后才把候选移动进
逻辑 map 并更新燃烧索引。代理拒绝时旧逻辑状态、索引和代理状态都保持不变。

## 不变量

- locator 只指向代理自己持有的 Source 数组；数组结构变化必须整体重建 locator。
- runtime 与 residue 必须作为同一 Source 状态一起验证、一起提交。
- 非二值 mask、尺寸不匹配或未知 SourceId 不得修改任一缓存。
- active 燃烧允许逻辑 mask 高频变化，但可见 chunk 网格只在结束批次重建。
- 完成燃烧后即使最终 mask 与最后一次 active 更新相同，也必须安排一次 deferred rebuild。
- 客户端逻辑 map 的提交不能早于代理接受候选状态。

## TDD 与验证

第一条性能 RED 在同一区块放置 4096 个 Source，反复更新尾部 Source 8192 次；旧实现为
100.05ms，并超过 50ms 初始门槛。稳定 locator 后为 2.54ms；合并原子接口后的最终重编结果
为 3.18ms。长期门槛收紧到 25ms，保护的是“不随区块 Source 数线性扫描”，而不是某台机器
的绝对帧时间。

原子性测试先提交变化 runtime + 非法 residue，随后用原始完整状态验证返回 `Unchanged`；合法
成对状态返回 `Changed`，重复提交返回 `Unchanged`。燃烧延迟测试直接检查
ProceduralMesh 顶点数：active 更新保持旧 mesh，结束前仍保持，deferred flush 后才变成最终几何。

最终结果：

- 代理行为测试：2/2；4096 Source 性能测试：1/1，3.18ms；
- `MatterFlux.Combustion`：19/19；`MatterFlux.Save`：5/5；
- Listen Host + Client PIE：1/1；
- 完整 `Automation RunTests MatterFlux`：189/189，0 失败；
- Editor Development、Game Development、Game Shipping 均使用 MSVC 14.44.35222 构建成功。

主要日志：

- `Saved/Logs/MatterFluxProxyLookupRed.log`
- `Saved/Logs/MatterFluxProxyAtomicGreen.log`
- `Saved/Logs/MatterFluxProxyCombustionDeferralGreen.log`
- `Saved/Logs/MatterFluxProxyFinalPerformance.log`
- `Saved/Logs/MatterFluxProxyTransactionalListenHostClient.log`
- `Saved/Logs/MatterFluxProxyTransactionalFullAutomation.log`

## 后果

一次客户端 Source delta 现在从 Fast Array 计划、mask 解包、代理查找到逻辑提交都只随变化量
增长，并且没有 runtime/residue 或逻辑/可视半提交窗口。代价是 locator 的正确性依赖代理数组
结构只通过整体设置入口变化；这个约束比让任意调用方直接修改内部数组更窄，也更容易测试。

