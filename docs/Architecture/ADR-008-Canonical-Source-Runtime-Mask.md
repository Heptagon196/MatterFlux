# ADR-008：流式 Source 只保留一份 runtime mask 真值

- 状态：已采纳并实施
- 日期：2026-08-10
- 取代说明：ADR-015 部分取代本决策。RuntimeMask 只保留为旧资产输入和一层挤出 Source 的确定性占用编码/投影；Volume 事务是占用、材质、结构与能量的统一事实。

## 背景

`FFragment2DSourceStreamingState` 原来同时保存通用 `RuntimeMask` 和燃烧 snapshot 中的
`FuelMask`。只要 `bHasCombustionState=true`，两者必须逐字节相等；世界 Actor 每个 fixed
step 仍会执行一次完整复制来维持这个不变量。

这造成两个问题：

- 每个燃烧或带残渣 Source 同时保存 runtime、fuel、residue、burning 四张等尺寸数组；
- 存档、复制、SourceActor、动态 Carrier 和世界 Actor 都能直接读取公开 `RuntimeMask`，
  调用方需要自行记住燃烧态的重复一致性规则。

重复数据没有表达不同领域含义。燃烧态的 fuel mask 本身就是当前剩余实体 mask。

## 决策

流式状态现在只提供一个读取 interface：

```text
GetRuntimeMask()
SetRuntimeMask(mask)
CaptureCombustionState(runtime)
```

- 未燃烧状态把 mask 保存在私有 `StandaloneRuntimeMask`；
- 存在燃烧 snapshot 时，`GetRuntimeMask()` 直接返回 `FuelMask`；
- 成功捕获燃烧 runtime 后释放 standalone 数组；
- `SetRuntimeMask` 根据当前状态把值提交到 standalone 或 fuel，不允许形成双份真值；
- `GetStoredMaskValueCount()` 提供稳定的内存门禁，而不暴露私有数组。

存档仍写出原有 runtime mask 字段，网络仍发送原有 packed runtime mask，因此外部格式没有
改变。客户端解包时先建立 combustion metadata，再把 runtime mask 移入 fuel；代理、Carrier
和 SourceActor 都从 `GetRuntimeMask()` 读取。

## 不变量

- 同一个流式 Source 任意时刻只能有一份有效 runtime mask 真值。
- 未燃烧状态保存一张 standalone mask；燃烧状态保存 fuel/residue/burning 三张 mask。
- 燃烧捕获失败不得改变旧真值或存储模式。
- 存档、Fast Array、Actor 交接和 Carrier 应用必须读取同一 interface。
- 网络与存档 wire format 保持兼容，晚加入客户端仍恢复逐字段相同的 fuel 和 revision。

## TDD 与验证

专项 RED 使用 64-cell Source，确认旧状态保存 256 个 mask 值，而规范化目标为 192。GREEN
后同一测试确认有效 runtime mask 与 runtime fuel 完全相同、存储量为三张 mask，失败捕获
保持旧状态。

最终结果：

- 每个燃烧/残渣 Source 的 mask 值存储从四张降为三张，减少 25%；
- `MatterFlux.Combustion`：19/19；
- 完整 Save 测试组与燃烧 Source 流式归档专项通过；
- 完整 `Automation RunTests MatterFlux`：179/179，0 失败、0 未运行；
- 大世界共享进程控制段 234.64ms、燃烧段 391.92ms，性能门禁通过；
- Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。

日志位于 `Saved/Logs/MatterFluxCanonicalRuntimeMaskRed.log`、
`MatterFluxCanonicalRuntimeMaskGreen.log`、`MatterFluxCanonicalRuntimeMaskCombustion.log`、
`MatterFluxCanonicalRuntimeMaskSave.log`、`MatterFluxCanonicalRuntimeMaskStreaming.log` 和
`MatterFluxCanonicalRuntimeMaskFull.log`。

## 后果

每个燃烧 fixed step 少一次 fuel→runtime 整 mask 复制，持久状态少一张等尺寸数组，调用方
也不再承担“两个字段必须相等”的知识。代价是所有 runtime mask 读写必须通过流式状态
interface；新增 adapter 若绕开它会在编译期失败，因为 standalone 存储是私有的。

三张 mask 的网络 bit-pack 和逐条 `ForceNetUpdate` 后续已由事务式 batch 取代；见
`ADR-009-Transactional-Source-Replication-Batches.md`。当前剩余热点是客户端收到任一 Fast
Array delta 后仍会扫描完整 Source 状态列表。
