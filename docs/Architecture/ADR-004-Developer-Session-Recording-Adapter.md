# ADR-004：会话录制通过玩家语义操作 seam 进入 Developer module

- 状态：已采纳并实施
- 日期：2026-08-09

## 背景

视觉捕获和开发控制台命令已经进入 `MatterFluxDeveloper`，但会话录制仍位于主 Runtime：

- `AMatterFluxCharacter.cpp` 直接 include `UMatterFluxSessionRecorderSubsystem`；
- 每个移动、跳跃、镜头和法杖输入都直接查询 GameInstance recorder；
- 客户端录制转发 RPC 以 `RecordedOperation` 命名并由 Character 直接调用 recorder；
- Codec 是 Runtime 中唯一使用 JSON DOM 的实现，因此 `MatterFlux.Build.cs` 仍依赖 `Json`；
- `-MFRecord`、`-MFReplay` 和 `mf.Record.*` implementation 自然进入 Shipping。

输入、法杖激活和重新生成是正式 gameplay；把它们重写到 Developer module 会反转依赖。
录制文件、截图、回放时钟和自动退出才是 Developer implementation。需要在两者之间建立
窄 seam，而不是让 Runtime 了解具体 recorder。

## 决策

### 1. Runtime 只拥有通用玩家语义操作

新增 `EMatterFluxPlayerOperation`，值和当前录制格式保持稳定：

```text
Move, JumpStarted, JumpCompleted, CameraZoom,
Cut, Flame, Regenerate, CastWand
```

它描述“玩家想做什么”，不是“录制器怎样保存”。Character 的公开入口改名为
`ApplyPlayerOperation`。Network Scale、回放、机器人或未来输入重映射都可以使用同一个
正式 gameplay interface。

Character 暴露一个静态 multicast 语义操作事件。事件参数包含 Character、operation、
二维值、整数值，以及事件是否来自客户端录制转发。零监听者时只执行一次空 delegate
检查，不查找 UObject、不分配数组，也不进行 RPC。

### 2. Developer recorder 是事件 adapter

`UMatterFluxSessionRecorderSubsystem` 移入 `MatterFluxDeveloper`，初始化时订阅语义操作，
反初始化时解除。每个 GameInstance 都有自己的 Subsystem；回调先比较 Character 的
GameInstance，因此 in-process 多 PIE 不会把事件写入错误会话。

本地客户端成功记录后，由 Developer adapter 请求 Character 执行可靠 Server RPC。RPC
仍属于 Runtime Actor，因为 UE 只会为 Actor 生成网络通道；服务器实现只重新广播同一
语义操作并标记为 relayed，不 include 或查询 recorder。服务器 GameInstance 的 Developer
adapter 接收后写入权威录制。标志防止客户端再次转发形成循环。

### 3. 整个录制 implementation 移入 Developer

以下文件及其公开 recording 类型进入 `MatterFluxDeveloper`：

- `MatterFluxSessionRecorderSubsystem`
- `MatterFluxSessionRecordingCodec`
- `MatterFluxSessionRecordingPolicy`
- `MatterFluxReplayRuntime`
- `MatterFluxSessionRecordingTypes`

跨 DLL 类型改用 `MATTERFLUXDEVELOPER_API`。`MatterFluxTests` 已依赖 Developer module，
测试 interface 不需要改变用途。`Json` 从 `MatterFlux.Build.cs` 删除并成为
`MatterFluxDeveloper` 私有依赖。

Game Target 只在 `bBuildDeveloperTools` 时包含该 module；Shipping 因而同时排除 JSON
codec、文件读写、Viewport 截图、录制控制台命令和 replay adapter。Runtime 不用
`#if UE_BUILD_SHIPPING` 伪装隔离。

## 事务与网络不变量

- operation 请求进入 Character 时同步发布；delegate 没有返回值，observer 不能 veto
  后续 gameplay。它记录的是输入意图，法杖冷却等正式规则仍可拒绝实际效果。
- 只有 active recording adapter 返回成功时，客户端才发送录制转发 RPC。
- RPC 验证仍限制 operation 范围、有限二维值、Regenerate seed 和法杖槽位。
- relayed 事件只供服务器 Developer adapter 记录，不再次执行 gameplay，也不再次转发。
- replay 调用 `ApplyPlayerOperation`；因为 replay 与 record 互斥，不会把回放重新录成输入。
- Subsystem 解除订阅后不能留下绑定到已销毁 GameInstance 的 UObject delegate。

## TDD 顺序

1. 为语义操作事件增加测试：零/多个监听者、稳定参数、解除订阅后不再调用。
2. Character 输入仍执行 gameplay，并只广播一次相应 operation。
3. in-process Host + Client 录制验证：客户端事件只通过 RPC 在服务器 recording 中出现
   一次；不同 PIE GameInstance 不串写。
4. 迁移 recording 文件和 API 宏，运行 `MatterFlux.Recording` 8/8 与 Network Scale。
5. 构建 Editor、Development Game、Shipping；Development 中 `MFRecord`/`mf.Record.*`
   存在，Shipping 二进制和 target metadata 中均不存在 `MatterFluxDeveloper`、
   `MFRecord`、`mf.Record.Screenshot`，且 Runtime link 不再包含 Json。

每步先建立 RED，再完成最小 GREEN。真实端到端录制→JSON→回放仍保留为最终门禁，不能
只用 codec 纯测试替代。

## 实施与验证记录

2026-08-09 已完成本 ADR：

- `AMatterFluxCharacter` 不再 include 或查询录制 Subsystem；Runtime 只发布
  `EMatterFluxPlayerOperation` 多播事件，并保留一个经过参数校验的可靠中继 RPC；
- Recorder 在 active session 初始化时订阅，在 `Deinitialize` 时按 handle 解绑，并以
  GameInstance 身份和 `bRelayedFromClient` 过滤事件；
- recording types、Codec、Policy、ReplayRuntime 与 Subsystem 已全部迁入
  `MatterFluxDeveloper`，跨 DLL 类型使用 `MATTERFLUXDEVELOPER_API`；
- `Json` 已从 `MatterFlux.Build.cs` 删除，仅由 `MatterFluxDeveloper.Build.cs` 私有依赖；
- 玩家操作多播测试 1/1、`MatterFlux.Recording` 8/8 通过；增强后的 Listen Host + Client
  PIE 在启用录制时证明远端 Client 与 Host 各恰好增加一个同 PlayerId 的 CameraZoom，Host
  最终文件也只有一个 operation；
- `MatterFlux.Network.Scale` 的 2～4 人 near/far 六种 PIE 场景全部通过；
- 真实 `-game` 录制生成 2 operations、27 states 的 JSON，随后用同一文件回放得到
  `MatterFlux replay complete: PASS`；
- Editor、Development Game 与 Shipping 均使用 MSVC 14.44.35222 构建成功。UTF-16
  二进制扫描确认 `MatterFluxDeveloper`、`MFRecord`、`MFReplay`、三个 `mf.Record.*`
  命令和 `MatterFluxSessionRecording` 在 Development 中存在、在 Shipping 中全部不存在。

## 后果

正面：Shipping dependency graph 真正排除自动化 implementation；Character 不再查询
Developer UObject；玩家语义操作成为可被录制、回放、测试和未来 bot 共用的正式
interface；JSON 知识具有 locality。

代价：Runtime 增加一个小型语义操作事件和录制转发 RPC。RPC 本身在 Shipping 仍存在，
但无 Developer listener 时不会被正常 gameplay 调用；若之后必须连这几个字节的生成代码
也排除，再评估 Target-specific UHT，而不在本轮引入条件 UFUNCTION。
