# MatterFlux 0.2.0 发布验收审计

日期：2026-07-24

这份文档区分三类证据：

- **源码证据**：实现和配置已经存在，并经过静态检查。
- **测试定义**：自动化测试已经编写，但不代表它已经在当前源码上执行成功。
- **运行证据**：必须来自当前源码构建出的 Editor/Game/Server 或实际 Automation 结果。

在所有运行门禁通过前，0.2.0 只能视为“实现完成、等待验收”，不能标记为正式完成。

## 1. 功能和修复要求

| 要求 | 源码证据 | 测试定义 | 当前结论 |
|---|---|---|---|
| 启用 GeometryProcessing；GeometryCore、GeometryAlgorithms 为运行时私有依赖 | `MatterFlux.uproject`、`MatterFlux.Build.cs` | 项目元数据与实际编译门禁 | 已实现；Editor、Game 正式构建通过 |
| 单一 `BuildFragmentGeometryFromMask` 深模块 | `FragmentGeometry.h/.cpp` | `MatterFlux.Fragment.Geometry.*` | 已实现；几何组 4/4 通过 |
| 8-neighbor component 支持多个外轮廓 | 定向边界追踪和稳定环排序 | `DiagonalTouchProducesStableOuters` | 已实现；测试通过 |
| 外环 CCW、孔洞 CW、最小顶点起始、稳定排序 | 轮廓清理和 canonicalize 路径 | L 形、环形、对角接触测试 | 已实现；测试通过 |
| 孔洞归属和 `FConstrainedDelaunay2d` 三角剖分 | 显式检查 `Triangulate()` 和输出索引/面积 | `RingPreservesHole`、`FailuresClearOutputs` | 已实现；测试通过 |
| payload 使用多外环、孔洞、碰撞轮廓和 Thickness | `FFragmentSpawnPayload` | 确定性与网络 PIE 测试 | 已实现；全套与网络 PIE 通过 |
| component 稳定排序和确定性 GUID | 面积、边界、完整 cell tie-break；几何签名 | `IsFullyDeterministic`、`ComponentTieBreakIsDeterministic` | 已实现；测试通过 |
| 每次破坏硬性最多 16 个 fragment | `MaximumFragmentCount`，资产和深模块共同封顶 | `EnforcesSixteenFragmentBudget` | 已实现；测试通过 |
| 正反面使用剖分结果，侧面独立顶点、正确法线和 UV | `BuildExtrudedMesh` | `SideNormalsAndIndicesAreValid` | 已实现；测试通过 |
| 非法索引、退化/顺时针三角形、非法环和厚度返回失败 | `BuildExtrudedMesh` 的输入验证和清空输出 | `SideNormalsAndIndicesAreValid`、`FailuresClearOutputs` | 已实现；测试通过 |
| Source 从 RuntimeMask 建网格并使用精确三角查询碰撞 | `RebuildSourceMesh`、complex-as-simple | Actor/地图验收 | 已实现；源码复核与 Map Check 通过 |
| 动态 fragment 使用每个外轮廓的凸包，孔洞仅影响可视网格 | `CollisionContours` 和 `RebuildSimpleCollision` | payload/Actor 测试 | 已实现；测试通过 |
| Damage 在临时 mask 上事务式计算 | `ApplyDamageEvent` | `InvalidAndNoChangeRollback` | 已实现；测试通过 |
| SourceId、revision、mask、shape 无效时不提交 | 显式校验和 revision 溢出保护 | `InvalidAndNoChangeRollback` | 已实现；测试通过 |
| 所有 debris 被过滤时仍提交并处理 source | 空 payload 成功路径和 broken 状态 | `AllDebrisFilteredStillCommits` | 已实现；测试通过 |
| `SpawnPayload` 为 `COND_InitialOnly`，OnRep 不覆盖 transform | `Fragment2DActor.cpp` | 网络 PIE 测试 | 已实现；两客户端验证通过 |
| 不销毁 source 时复制 broken 状态并隐藏/禁用碰撞 | `bBroken`、`OnRep_Broken`、always-relevant 策略 | 事务和网络 PIE 测试 | 已实现；两客户端验证通过 |
| 缩放 source 的圆形伤害保持正确空间语义 | Circle 使用 shape inverse transform | `CircleRespectsRelativeTransformScale` | 已实现；测试通过 |
| 编辑器同尺寸 asset mask 修改后刷新预览 | editor construction 重建 RuntimeMask | `EditorConstructionRefreshesAssetMask` | 已实现；测试通过 |
| 非有限 transform、质量和速度不能进入物理 | payload 状态先校验，随后才设置 transform | `InvalidPayloadDisablesCollision` | 已实现；测试通过 |
| 程序化碎片网格不触发无效导航更新 | Source/Fragment 组件默认关闭导航影响 | `ProceduralMeshesDoNotAffectNavigation` | 已实现；警告回归为 0 |
| GAS 不隐式回退扫描 GameplayCue 路径 | `DefaultGame.ini` 显式配置 `/Game`，保持原搜索范围 | 全量 Automation 启动日志 | 已实现；对应警告为 0 |

## 2. 测试基础设施

- `MatterFluxTests` 是 Editor-only 模块。
- Editor Target 显式包含 `MatterFluxTests`。
- 当前源码注册 34 个 `MatterFlux.*` ProductFilter 测试，名称唯一。
- 其中包括 dedicated-server world + 两个 client world 的 in-process PIE 测试。
- PIE 验证按“请求/复制/移动”阶段分别使用 30 秒超时。
- `ULevelEditorPlaySettings` 的 root 生命周期由 UE 5.8 的 `FStartPIEForAutomationCommand` 析构函数配对清理。
- 两个需要外部编排的 Forge Workspace 测试仅在配置
  `-ForgeWorkspace` 或 `FORGE_WORKSPACE` 时进入等待；普通无人值守回归会立即记录跳过信息。
- PIE fixture 在 dedicated server world 中动态 Spawn source，再等待两个客户端收到同一 SourceId，避免把 Editor 临时 startup actor 当成有效网络对象。

最新运行证据位于
`Saved/Logs/ReleaseVerification/20260724-server-preflight-skip-final/Automation.log`：
34 个唯一的 `MatterFlux.*` 测试全部成功，进程退出码为 0。

## 3. 发布门禁

必须在能正常使用项目 AutoSDK 的桌面环境中按顺序完成：

1. `MatterFluxEditor Win64 Development`
2. `MatterFlux Win64 Development`
3. `MatterFluxServer Win64 Development`
4. 在 Editor 或 `UnrealEditor-Cmd` 中运行 `Automation RunTests MatterFlux`
5. 对 `/Game/Default` 执行 Map Check

可在本机可用的正式构建环境中运行
`Scripts/Verify-MatterFluxRelease.ps1` 顺序执行以上五项门禁；日志写入
`Saved/Logs/ReleaseVerification/`。审计文档不会把“脚本存在”当成通过，
仍以脚本实际生成的退出码和日志为准。

每个门禁都必须满足：

- 进程退出码为 0。
- 日志中没有 C++ 编译、UHT、链接、模块加载或 Automation failure。
- Automation 报告 34 个 `MatterFlux.*` 测试全部成功。
- 多人 PIE 测试确实创建 dedicated server 和两个 client，而不是被跳过。
- Map Check 为零 error；新增 warning 也必须人工确认。

| 门禁 | 当前证据 | 状态 |
|---|---|---|
| `MatterFluxEditor Win64 Development` 正式 UBT 构建 | `20260724-server-preflight-skip-final/MatterFluxEditor.log`：当前源码，`Result: Succeeded` | 通过 |
| `MatterFlux Win64 Development` 正式 UBT 构建 | `20260724-server-preflight-skip-final/MatterFlux.log`：当前源码，`Result: Succeeded`；先前完整构建为 80/80 actions | 通过 |
| `MatterFluxServer Win64 Development` 正式 UBT 构建 | `20260724-013516-strict/MatterFluxServer.log`：Launcher UE 5.8 明确报告不支持 Server Target | 引擎分发阻断，未通过 |
| `Automation RunTests MatterFlux` | `20260724-server-preflight-skip-final/Automation.log`：34/34，退出码 0；GameplayCue 路径与空导航 bounds 警告均为 0 | 通过 |
| `/Game/Default` Map Check | `20260724-server-preflight-skip-final/MapCheck.log`：0 error、0 warning，退出码 0 | 通过 |

## 4. 当前可用与不可用的运行证据

可用：

- 2026-07-21 的旧日志证明 MSVC 14.44.35222 能完成一次 `MatterFluxEditor` 95/95 actions。
- 同日 Forge 定向回归曾通过 12/12 个所选测试。
- 2026-07-23 直接使用项目 AutoSDK 的 MSVC 14.44 编译了本轮关键翻译单元，并成功链接临时 runtime/test DLL。
- `Saved/Logs/CodexGeometryTests-v2.log` 记录
  `MatterFlux.Fragment.Geometry` 四项测试全部成功，Automation 以状态 0 退出。
- `Saved/Logs/CodexMatterFlux33.log` 记录 33/33 全部成功；其中多人 PIE 确实创建 dedicated server 和两个 client，并完成破坏、payload、broken state 与移动收敛验证。
- `Saved/Logs/CodexMapCheckFinal.log` 记录 `/Game/Default` 为 0 error、0 warning，`QUIT_EDITOR` 后进程退出码 0。
- 当前 MatterFlux Editor runtime/test 关键 TU 已直接编译并链接；Game 目标的 16 个 MatterFlux runtime TU 已全部用 MSVC 14.44 编译成功。
- UE 日志已确认此前 `dotnet.exe` 来自 `TargetPlatform` 隐式执行
  `Build.bat -Mode=ValidatePlatforms`；无头测试加入 `-Multiprocess` 后不再启动 UBT/dotnet。
- 2026-07-24，`ReleaseVerification/20260724-014144-installed/MatterFluxEditor.log`
  与 `MatterFlux.log` 证明当前源码的 Editor、Game Target 检查成功；
  `ReleaseVerification/20260724-013516-strict/FullRunner.stdout.log` 保留 Game
  首次完整构建 80/80 actions 的编译和链接证据。
- 同日 `ReleaseVerification/20260724-nav-final/` 重新构建当前 Editor、Game
  目标并记录 0 编译警告。最新的
  `ReleaseVerification/20260724-server-preflight-skip-final/` 又确认两目标成功，
  `Automation.log` 中 34/34 测试全部成功，`MapCheck.log` 记录 0 error、
  0 warning。新增导航回归验证 Source 和动态 Fragment 均不会触发导航更新；
  Automation 中 GameplayCue 路径与空 bounds 导航警告均为 0。
- Game 完整构建出现的 60 条 `C4191` 均来自
  `Plugins/UnrealAngelScriptForge/.../ThirdParty/AngelScript` 的 vendored
  第三方源码；MatterFlux 自有 C++ 没有编译警告或错误。本轮没有通过全局禁用
  警告来掩盖它们，也没有擅自修改第三方实现。
- 构建进程缺失 `UE_SDKS_ROOT` 时 UBT 无法发现未注册到 Visual Studio
  Installer 的 MSVC 14.44；缺失 `ProgramData` 时 UE 5.8 UBA 初始化会空引用。
  验收脚本现已显式配置项目 AutoSDK、`UBA_ROOT` 和安全的单并发构建。
- 验收脚本完整模式会先读取引擎 `[InstalledPlatforms]`。当前 Launcher 引擎在
  约 0.2 秒内被明确判定为不支持 Win64 Development Server，且不会创建运行
  目录或先执行 Editor/Game；源码版或声明了 Server 的 Installed Build 会继续
  到真实的 `MatterFluxServer` 构建。`-SkipServer` 分支已再次完成其余四项门禁。

最新 Automation warning 已逐类复核：MatterFlux 日志来自回滚与非 authority
测试刻意提交的拒绝请求；`FNetGUIDCache`/`WorldSettings` 日志来自 UE 对 in-process
PIE 临时关卡的网络诊断；ModelContextProtocol 的许可提示来自启用插件。项目可控的
GameplayCue 路径和程序化网格 empty-bounds 导航警告均已消除，未通过全局降级日志
级别来隐藏问题。

不可作为 0.2.0 发布证据：

- 2026-07-21 的完整 Editor 构建早于本轮源码修改。
- Epic Launcher 的 UE 5.8 安装版没有 Server 预编译配置，正式构建返回
  `Server targets are not currently supported from this engine distribution.`。
  这不是 MatterFlux 编译错误，但也不能算 Server 门禁通过。
- `-SkipServer` 只用于完成安装版可支持的四项验收；最新证据位于
  `ReleaseVerification/20260724-server-preflight-skip-final/`，脚本会明确输出
  “partial verification passed; release is not signed”。
- in-process dedicated-server PIE 覆盖服务器权威和两客户端复制行为，但不能替代
  `MatterFluxServer` 在支持 Server 的源码版/Installed Build 引擎上的正式构建。

## 5. 签署规则

只有第 3 节五项门禁均有当前源码对应的成功日志时，才能把 0.2.0 标记为完成，并在 `CHANGELOG.md` 中把 `Unreleased` 替换为实际发布日期、将三目标构建记录为已验证。
