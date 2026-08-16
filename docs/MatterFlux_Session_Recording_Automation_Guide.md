# MatterFlux 会话录制、自动回放与截图测试指南

本文面向刚接触 Unreal Engine 的开发者，介绍 MatterFlux 的可复现会话测试框架。

## 1. 它解决什么问题

随机地图、物理、多人复制和连续输入组合在一起后，仅凭一句“这里偶尔不对”通常很难复现。MatterFlux 会话录制器会把一次游戏过程写成一个可读的 JSON 文件，其中包含：

- 地图名称和世界随机种子；
- 每个玩家的稳定会话 ID 与名字；
- 玩家的移动、跳跃、按装备槽施放法杖和重新生成地图等语义操作；旧版 `Cut`/`Flame`
  记录仍可回放为槽 1/2，新的 `CastWand` 会保存 0～3 的准确槽位；
- 定时采样的玩家位置、旋转、速度和 Movement Mode；
- 应当在什么时间截图，以及截图标签；
- UE 版本、录制格式版本和总时长。

回放时，框架先恢复世界种子，再按时间重新注入操作、重新截图，并将实际玩家位置与录制状态比较。超过容差会把回放标记为失败。

它记录的是“玩家想做什么”，而不是 Windows 键盘扫描码。这使同一份录制可以用于本地游戏、Listen Server 和自动化测试，也不会依赖窗口是否获得键盘焦点。

## 2. 最简单的录制方式

从 PowerShell 启动：

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
  "C:\Users\hepta\Documents\Codes\MatterFlux\MatterFlux.uproject" `
  -game `
  -MFRecord `
  -MFRecordDir="C:\MatterFluxRuns" `
  -MFRecordName="ForestCut" `
  -MFSeed=1337 `
  -MFRecordStateHz=10 `
  -MFRecordScreenshots="1.0:BeforeCut,2.0:AfterCut"
```

正常关闭游戏时，录制器会先原子写入：

```text
C:\MatterFluxRuns\ForestCut.mfrecord.json
```

截图位于 `C:\MatterFluxRuns\Screenshots\`。

“原子写入”表示先写临时文件，成功后再替换目标文件。应用不会留下一个看似存在、实际只写了一半的录制文件。

## 3. 适合自动化测试的有界录制

以下参数会录制 5 秒，保存后自动退出：

```text
-MFRecord -MFRecordDuration=5 -MFRecordQuit
```

完整示例：

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
  "C:\Users\hepta\Documents\Codes\MatterFlux\MatterFlux.uproject" `
  -game -windowed -ResX=1280 -ResY=720 `
  -MFRecord `
  -MFRecordDir="C:\MatterFluxRuns\E2E" `
  -MFRecordName="SmokeTest" `
  -MFSeed=1337 `
  -MFRecordScreenshots="0.5:Start,3.0:Settled" `
  -MFRecordDuration=5 `
  -MFRecordQuit
```

保存成功时进程返回 0；保存失败时返回 3。

## 4. 回放录制文件

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
  "C:\Users\hepta\Documents\Codes\MatterFlux\MatterFlux.uproject" `
  -game -windowed -ResX=1280 -ResY=720 `
  -MFReplay="C:\MatterFluxRuns\ForestCut.mfrecord.json" `
  -MFReplayOutputDir="C:\MatterFluxRuns\ForestCut_Replay" `
  -MFReplayQuit
```

回放会依次读取并校验格式、恢复世界 seed、注入玩家操作、重新截图、比较玩家状态，然后输出 `PASS` 或 `FAIL`。通过时进程返回 0，失败时返回 2。

默认位置误差容差为 30 cm，可通过 `-MFReplayLocationTolerance=50` 调整。仅观看而不比较状态时使用 `-MFReplayNoVerify`。

## 5. 运行中手动插入截图

打开 UE 控制台后输入：

```text
mf.Record.Screenshot TreeBeforeCut
```

录制器会使用当前会话时间，在 JSON 的 `screenshots` 数组中加入标记并进入截图队列。需要立即停止采集并保存时使用：

```text
mf.Record.Flush
```

该命令同样使用原子写入，并且可以安全地重复调用。

### 无窗口焦点的语义操作调度

自动化进程不应依赖 Windows 窗口焦点。可以用下列通用命令调度玩家操作：

```text
mf.Record.Inject <延迟秒数> <操作> [X] [Y] [整数值] [玩家序号]
```

例如：

```text
mf.Record.Inject 1.0 Move 1 0
mf.Record.Inject 2.0 Move 0 0
mf.Record.Inject 2.4 JumpStarted
mf.Record.Inject 2.6 JumpCompleted
mf.Record.Inject 3.5 Regenerate 0 0 24681357
```

非零 Move 会持续生效，直到同一玩家收到零 Move；这与 Enhanced Input 的 `Triggered`/`Completed` 语义一致。其他操作在指定时间执行一次。玩家序号默认是 0，按 PlayerState ID 排序。

它可以通过 `-ExecCmds` 组成无人工参与的录制场景：

```text
-ExecCmds="mf.Record.Inject 1.0 Move 1 0,mf.Record.Inject 2.0 Move 0 0,mf.Record.Inject 2.4 JumpStarted"
```

## 6. 启动参数表

| 参数 | 作用 |
|---|---|
| `-MFRecord` | 开启会话录制 |
| `-MFRecordDir=<目录>` | JSON 和录制截图根目录 |
| `-MFRecordName=<名称>` | 输出文件名；自动添加 `.mfrecord.json` |
| `-MFSeed=<正整数>` | 强制本次世界种子；不填写则记录实际随机种子 |
| `-MFRecordStateHz=<频率>` | 每秒玩家状态采样次数，默认 10，范围 `(0,120]` |
| `-MFRecordScreenshots=<列表>` | `时间:标签` 的逗号列表 |
| `-MFRecordDuration=<秒>` | 自动化录制时长；0 表示直到人工关闭 |
| `-MFRecordQuit` | 到达录制时长并保存后退出 |
| `-MFReplay=<文件>` | 加载并回放一个 `.mfrecord.json` |
| `-MFReplayOutputDir=<目录>` | 回放截图输出目录 |
| `-MFReplayLocationTolerance=<cm>` | 状态比较的位置容差 |
| `-MFReplayNoVerify` | 跳过状态比较 |
| `-MFReplayQuit` | 回放完成后按 PASS/FAIL 返回退出码 |

`-MFRecord` 与 `-MFReplay` 互斥，错误组合会被明确拒绝。

## 7. 多人模式

服务器是录制文件的权威汇总者：

- 服务器采样所有玩家的权威位置、速度与 Movement Mode；
- 客户端记录本地 Enhanced Input 语义操作；
- 仅在录制模式下，客户端把发生变化的操作可靠转发给服务器；
- 服务器把所有玩家写入同一个 JSON；
- 普通客户端不会再写一份重复 JSON。

进行多进程录制时，服务器和客户端都应带 `-MFRecord`。Dedicated Server 没有 viewport，因此不会截图；需要截图的客户端应使用各自独立的 `-MFRecordDir`，避免多个进程覆盖同名 PNG。

Listen Server 同时是服务器和本地玩家，不需要额外转发自己的输入。

## 8. 录制文件结构

简化示例：

```json
{
  "schema": "MatterFluxSessionRecording",
  "version": 1,
  "map": "Default",
  "world_seed": 1337,
  "players": [
    { "id": 256, "name": "Host" }
  ],
  "operations": [
    {
      "time": 0.5,
      "player": 256,
      "operation": "Move",
      "value": [1.0, 0.0],
      "integer_value": 0
    }
  ],
  "states": [
    {
      "time": 0.5,
      "player": 256,
      "location": [-700, -500, 216.15],
      "rotation": [0, 0, 0],
      "velocity": [0, 0, 0],
      "movement_mode": 1
    }
  ],
  "screenshots": [
    { "time": 1.0, "label": "BeforeCut" }
  ]
}
```

`integer_value` 用于完整保存 32 位随机种子，避免把大整数塞入 float 后丢失精度。

## 9. 代码结构

- `FMatterFluxSessionRecording`：录制文件的版本化领域模型；
- `MatterFluxSessionRecordingCodec.cpp`：实现 `ParseLaunchOptions`、`SaveToJson` 和
  `LoadFromJson`，独占 schema、预算、payload 校验、稳定排序和 JSON 格式；
- `MatterFluxSessionRecordingPolicy.h`：Developer module 内部共享的数量/尺寸限制、实时
  operation/state 校验和截图标签规范，不属于公共 interface；
- `FReplayRuntime`：不依赖 `UWorld` 的纯 C++ 时间线运行时；负责事务式推进、一次性操作、
  持续移动、hitch 状态折叠/插值、稳定排序、截图标记和一次性完成信号；
- `UMatterFluxSessionRecorderSubsystem`：负责 GameInstance 生命周期、World 采样、文件
  原子替换、Viewport 截图与回放 adapter；它不再依赖 JSON DOM，也不直接维护回放游标；
- `MatterFlux::PlayerOperations::OnApplied()`：Runtime 中与任何 recorder 无关的语义操作
  多播 seam；零监听者时不会查询 UObject 或发送 RPC；
- `AMatterFluxCharacter::ApplyPlayerOperation`：录制注入、回放和测试共用的正式语义操作入口；
- `UMatterFluxSessionRecorderSubsystem::HandlePlayerOperation`：Developer adapter；按
  GameInstance 过滤事件，并仅在 Client 本地记录成功后请求可靠服务器中继。

录制器使用 `UGameInstanceSubsystem`，因为它需要跨越 World 生命周期，并在应用关闭时仍有统一清理入口。它同时实现 `FTickableGameObject`，用游戏世界时间调度操作、状态和截图。

上述 recording types 和实现都位于 `Source/MatterFluxDeveloper/`。Development Game 与
Editor 通过 `bBuildDeveloperTools` 加载该模块；Shipping 不链接模块，因此不会携带 JSON
codec、录制命令、截图或回放实现。正式 gameplay 仍完整保留在 Runtime。

解码采用 prepare→commit：所有字段先进入局部候选，schema、版本、引用关系、数值范围和
数组预算全部通过后才替换调用者对象。加载损坏文件时，错误文本会更新，但调用者原来
持有的 recording 不会被清空或半覆盖。回放同样使用 prepare→commit：非法或倒退时间
不会推进任何 timeline index 或持续移动状态；Subsystem 只消费完整 `FReplayFrame`，再将
它适配到 Character、Viewport 和最终退出码。

## 10. 确定性的边界

框架保证同一录制使用同一世界 seed、操作顺序和截图时间来自同一文件、玩家权威状态可自动比较，并在 schema/version 不兼容或 JSON 损坏时拒绝回放。

它不承诺不同硬件、不同 UE 版本和不同物理帧率下逐 bit 相同。Chaos 物理与网络到包时间可能产生小误差，因此使用显式状态容差。碎片 GUID、mask、轮廓和三角形等离散数据仍由现有逐字段确定性测试负责。

## 11. 当前自动化验收

运行：

```text
Automation RunTests MatterFlux.Recording
```

当前 8 项用例覆盖启动参数、错误组合、未加引号的多截图列表、多玩家 JSON 往返、32 位
重建 seed、未知 schema 拒绝、失败解码 prepare→commit、控制台命令注册，以及
ReplayRuntime 的稳定批帧、hitch 插值、重复时间、倒退时间原子性和一次性完成信号。

2026-08-09 的最终运行结果为 8/8；同一改动的 Editor/Game Development/Shipping 构建和
`MatterFlux.Fragment.Network.ListenHostAndClient` 1/1 也通过。该网络测试临时启用两个
PIE GameInstance 的 recorder，验证远端 CameraZoom 在 Client 与 Host 各恰好记录一次，
Host JSON 最终也只有一个 operation；2～4 人 near/far Network Scale 另有 6/6 通过。
对应日志为：

- `Saved/Logs/RecordingDeveloperMigration.log`
- `Saved/Logs/ListenHostClientRecordingRelay.log`
- `Saved/Logs/NetworkScalePlayerOperationFinal.log`

项目还执行了真实 `-game` 端到端验证：3 秒录制写出 2 个语义操作和 27 个状态采样，
再由同一 JSON 启动回放并得到 `PASS`。日志分别是 `Saved/Logs/RecordE2E.log` 与
`Saved/Logs/ReplayE2E.log`。
