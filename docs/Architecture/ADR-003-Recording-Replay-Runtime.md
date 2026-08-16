# ADR-003：录制回放时间线进入纯 C++ ReplayRuntime

- 状态：已接受并实施
- 日期：2026-08-09

## 背景

录制 JSON 已由 `MatterFluxSessionRecordingCodec` 独占，但
`UMatterFluxSessionRecorderSubsystem::TickReplay` 仍同时负责：

- 消费到期的一次性操作；
- 保存并逐帧重放持续移动输入；
- 把一次 hitch 中到期的多个状态折叠为每玩家最后一项；
- 在当前状态和下一状态之间插值期望位置；
- 调度截图；
- 判断时间线是否完整结束。

这些规则不需要 World、Actor、Viewport 或文件系统。当前实现还通过 `TMap` 枚举持续
移动和待验证状态，枚举顺序不是回放格式的一部分，因而不能作为跨平台确定性顺序。

## 决策

建立普通 C++ deep module `FReplayRuntime`。它只有两个主要操作：

```text
Initialize(recording, settings, error)
Advance(elapsed_seconds, out_frame, error)
```

`Initialize` 验证时间线已规范排序，并持有只读 recording 引用。调用者必须保证该
recording 在 runtime 生命周期内地址稳定且内容不再变化。Replay Subsystem 满足此条件：
文件只在启动时加载一次，回放期间不修改。

`Advance` 返回一个完整 `FReplayFrame`：

- 本帧到期的一次性操作，保持文件中的稳定顺序；
- 当前所有持续移动输入，按 `PlayerId` 升序；
- 每玩家最多一个待验证状态，按 `PlayerId` 升序，并已计算当前时刻的插值位置；
- 本帧到期的截图标记，保持时间与文件稳定顺序；
- 时间线是否已满足完成条件。

runtime 隐藏 operation/state/screenshot index、持续移动表、hitch 折叠和未来状态搜索。
Subsystem 只把 frame 适配到 Character、Viewport 和退出码。找不到玩家、位置超差等
World 相关失败仍由 adapter 记录，不进入纯时间线 module。

`Advance` 必须是事务式的：非有限时间、倒退时间或内部不变量失败时不推进任何 index、
不改变持续移动状态，并清空 `OutFrame`。正常重复传入相同时间不会重复发出一次性操作或
截图，但会继续返回当前持续移动集合。

## TDD 顺序

1. 一个包含乱序 PlayerId、同刻操作和重叠状态的时间线，在一次 `Advance` 中输出稳定的
   operation、movement、expected-state 和 screenshot frame。
2. 大步 hitch 只为每个玩家返回最后一个到期状态，同时使用下一未来状态插值。
3. 同一时间重复 `Advance` 不重复一次性事件；持续移动仍存在。
4. 时间倒退被原子拒绝，随后使用合法时间继续时结果与未发生错误完全一致。
5. 所有事件消费完并超过 completion grace 后只报告一次完成。

每条按单独 RED→GREEN 纵向切片实现。测试只包含 recording 数据和 runtime interface，
不创建 UWorld；Subsystem 的 Character 应用和最终退出码继续由现有端到端录制/回放测试
覆盖。

截至 2026-08-09，`FReplayRuntime` 已落地并接入
`UMatterFluxSessionRecorderSubsystem`。稳定批帧、hitch 状态折叠/插值、重复时间、倒退时间
原子拒绝和完成信号只发一次均有自动化覆盖。`MatterFlux.Recording` 8/8、Editor/Game
Development 构建以及 Listen Host + Client PIE 1/1 均通过。

## 后果

正面：回放规则获得 locality；确定性排序成为 interface 保证；hitch、重复 Tick 和时间
异常可在纯测试中覆盖；Subsystem 将删除多个 index、movement map 和插值循环。

代价：runtime 持有只读 recording 引用，因此 interface 明确要求生命周期稳定。若未来
需要编辑中回放，再把所有权升级为不可变共享快照，不在本轮提前复制百万级数组。
