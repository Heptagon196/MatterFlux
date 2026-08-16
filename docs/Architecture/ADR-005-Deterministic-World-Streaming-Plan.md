# ADR-005：用确定性计划统一世界区块流式窗口

- 状态：已采纳并实施
- 日期：2026-08-09

## 背景

`AMatterFluxPlayableWorldActor` 曾分别为地形、关卡层和可破坏 Source 手写区块窗口。
这些实现都在计算“多个玩家附近区块的并集”，但细节并不一致：

- 地形还要覆盖等距镜头朝 `+X/-Y` 看到的额外窗口；
- 缺失地形组件曾直接按 `TSet` 的迭代顺序创建，不同进程的创建顺序不稳定；
- 同一刷新帧内的活动区块曾因 `TMap` 遍历顺序获得不同 LRU 时间；
- 淘汰代数相同时没有确定性的仲裁规则；
- Source 驻留判断曾扫描可见区块中的全部静态定义，只为判断少量已物化 Actor；
- 窗口配置非法时，调用者可能先提交新焦点，再发现窗口无法生成。

这些问题不一定立刻改变画面，但会使客户端对象创建顺序、缓存淘汰和性能难以复现，也让
将来扩大地图或改变半径时更容易出现边界卡顿。

## 决策

新增纯 C++ deep module `MatterFlux::WorldStreaming`，公开两个窄操作：

```text
BuildChunkWindow(request) -> 稳定排序的唯一区块数组
SelectEvictionCandidate(residents, active, generations) -> 一个稳定候选
```

`FChunkWindowRequest` 包含焦点、额外窗口偏移、半径和最大输出数量。规划器会先去重并按
`X/Y` 字典序排序焦点与偏移，再使用 64 位中间坐标建立并集，最后返回同样按 `X/Y`
排序的结果。地形传入 `{(0,0), (1,-1)}` 两个偏移；层和 Source 只使用默认零偏移。

输出预算当前硬限制为 65,536 个区块，纯接口还拒绝超过 1,048,576 的请求、负半径、
无效预算和 `int32` 坐标溢出。任何失败都保留调用者旧输出。世界 Actor 也只在地形计划
成功后提交 `VisibleLayerFocusChunks`，因此配置错误不会形成半提交焦点。

LRU 不再给同帧活动区块逐个增加时间。一次刷新只生成一个 generation，所有活动区块
共享它；淘汰选择最小 generation，平局按 `X/Y` 仲裁。缺少历史的 resident 按 generation
0 处理，所以它会优先淘汰，但结果仍不依赖 `TMap` 的迭代顺序。

Source 驻留只检查 `SourceId -> Chunk` locator 与 desired chunk set，不再为每次焦点变化
遍历可见区块中的全部 pristine Source。逻辑数据仍逐 Source 独立，渲染仍由区块代理合并。

## 不变量

- 相同焦点集合、偏移、半径和预算必须逐元素产生相同区块数组，与输入顺序和重复项无关。
- 不允许把 `TSet`/`TMap` 的迭代顺序作为组件创建、复制或淘汰顺序。
- 预算、坐标或配置失败不得修改旧输出，也不得提交新的可见焦点。
- 活动区块永不成为淘汰候选；相同使用代数必须按坐标稳定仲裁。
- 地形、层和 Source 消费同一窗口语义；特有视野通过 request offset 表达，不复制算法。
- 规划器只计算数据，不创建 UObject、不读 World，也不拥有缓存生命周期。

## TDD 与验证

本切片按 RED→GREEN 完成：

1. 缺少规划器头文件时，四项新测试先不能编译；
2. 窗口去重、排序、镜头偏移和失败原子性转绿；
3. 先声明 LRU 接口制造链接失败，再实现稳定淘汰使测试转绿；
4. 接入三个消费方，并修复 Unity Build 中两个测试文件匿名命名空间 helper 同名的问题；
5. 静态复核发现焦点先提交的边界，再改为地形计划成功后提交并重复回归。

最终结果：

- `MatterFlux.Streaming.Plan`：4/4；
- `MatterFlux.Playable`：9/9；
- `MatterFlux.Fragment.Network.ListenHostAndClient`：1/1；
- `MatterFlux.Network.Scale`：2～4 人 near/far 共 6/6；
- 大世界移动：1200 cm/s 与 2500 cm/s 都跨越 9 个边界，最终边界 World Tick 最大分别为
  0.31 ms 与 0.30 ms；
- 完整 `Automation RunTests MatterFlux`：175/175、退出码 0；
- Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。

日志位于 `Saved/Logs/WorldStreamingPlanTransactionalFinal.log`、
`PlayableStreamingTransactionalFinal.log`、`ListenStreamingTransactionalFinal.log`、
`NetworkScaleStreamingPlanner.log`、`LargeWorldSparseMembershipFinal.log` 和
`MatterFluxWorldStreamingFullGreen.log`。

## 后果

正面：窗口算法具有单一事实来源；对象创建和缓存淘汰可复现；非法设置不会半提交；
Source 焦点变化的分类成本与已物化 Actor 数量相关，而不再与可见森林的全部逻辑 Source
数量相关。

代价：每次窗口变化仍需构造一个短命数组和集合。当前最大可见集合约几十个区块，成本
远低于网格与碰撞创建；只有真实 profile 证明它成为热点时才考虑复用 scratch storage。
规划器也没有解决燃烧期间的 mask→轮廓→三角剖分成本，该热点属于后续独立切片。
