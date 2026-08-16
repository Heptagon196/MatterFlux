# MatterFlux 代码质量与系统架构评审（2026-08-08）

## 结论

项目的底层算法和自动化测试基础较好，但运行时编排层已经出现明显的职责膨胀。最突出的风险不是“文件太长”本身，而是多个领域状态由同一个 Actor 或 Subsystem 直接拥有，导致 interface 过宽、状态修改缺少 locality，并让跨系统事务越来越难证明。

本轮没有用“按行数拆文件”代替架构优化。已完成的改动都围绕明确的 seam：UE 生命周期 adapter、Slate view implementation、物质复制 wire format 和共享视觉主题。

评审时的代码规模：88 个生产 C++ 文件、约 30,258 行生产代码、25 个测试源文件、约 12,146 行测试代码。

## 优先级问题

### P1：`AMatterFluxPlayableWorldActor` 拥有过多运行时状态

证据：评审前公开头约 397 行、实现约 3,842 行，共有 146 处类型或方法引用。它同时拥有：

- 异步地图生成状态机；
- 地形和装饰流送缓存；
- 碎片 Source 流送；
- 液体、气体和颗粒模拟；
- 地表与 Source 燃烧；
- 多种复制快照；
- 世界存档捕获与恢复；
- 光照和运行时材质实例。

风险：任何新增世界功能都倾向于继续向 Actor 加字段和 Tick 分支。调用者也会直接依赖具体 Actor，而不是依赖窄 interface。

本轮处理：先把 `FMatterFluxReplicatedMaterialState` 的压缩、CRC 校验和 `NetSerialize` 提取到独立 material module；删除 Actor 中未被调用的旧 bit-mask 编解码实现。Actor 也不再自行计算单焦点活动区，而是把所有权威 Pawn 的焦点集合交给 `FChunkedMaterialWorld`。

后续切片又新增 `FSimulationRuntime`。它在 `FChunkedMaterialWorld` 之上隐藏固定步长债务、
焦点切换帧暂停、追帧上限、logical step、active-state 导出/压缩、applied/rejected revision
和晚加入导入。`AMatterFluxPlayableWorldActor` 已删除对应 accumulator、focus 数组和 revision
字段，只保留 UE 复制属性、可视化脏标记与生命周期适配。两条直接 interface 测试、完整
Material/Playable 测试和 2～4 人 PIE 都已跨这个 seam 验证。后续仍应按“地面燃烧运行时 →
流送运行时 → 生成协调器”继续纵向切片，Actor 最终只保留 UObject 生命周期、组件持有和
复制回调。

后续切片已新增 `UMatterFluxFragmentSourceProxyComponent`：它独立拥有所有 pristine mask 的
区块批处理、材质分组、静态碰撞和代理/Actor 互斥状态。树干、树叶、草、花与岩石仍有
独立 SourceId 和 mask，但新生成世界不再为任一 pristine Source 创建 Actor。切割后仍连接
地形的树桩会把 revision/mask 回写逻辑缓存并立即重新合入区块；只有脱落后需要物理与独立
网络 transform 的对象保留 Actor。代理按可见区块复用 ProceduralMesh，并缓存每个 Source
的确定性挤出网格；局部刷新只重建 dirty chunk，隐藏区块同时关闭合并碰撞。

为避免短命 Source Actor 导致晚加入分歧，世界 Actor 现在复制规范化的
`FMatterFluxReplicatedFragmentSourceState`：仅包含发生过修改的 SourceId、revision 与 1-bit
mask。客户端将完整候选状态应用到逻辑缓存后才标脏对应区块；pristine 世界继续由 seed 与
Lua registry 重建，不产生逐对象复制负担。

本轮继续把上述状态改成 `FFastArraySerializer` 增量协议，不再因一个 Source 变化重发完整
状态数组。Source 燃烧也已经移出 `AFragment2DSourceActor`：`FSourceCombustionRuntime` 以普通
C++ 对象保存燃料、燃烧、残渣、随机状态和 fixed-step 时间债；区块代理合并残渣网格，世界级
共享 ISM 表现火焰和烟雾。临时物化的燃烧 Source 卸载时使用 prepare→commit 交接，失败会保留
Actor 等待重试，不会先删除所有者再尝试恢复逻辑状态。

本轮进一步修复了代理首次进入区块时同步三角剖分的冷启动尖峰。当前森林的代理区块数
小于独立的 `FragmentSourceProxyCacheLimit`，因此在加载阶段按稳定顺序预热网格，移动时
只切换已完成组件的可见性；更大的世界超过上限后仍走按需路径，不会无界占用内存。
同一轮还把材质可视化按“材质 × 模拟区块”分组，以稳定 hash 跳过未变化组，并用组件池
复用离开窗口的 ISM。地面火焰、烟雾、残渣和少数流式 HISM 也复用原地批量更新 helper，
不再通过 `ClearInstances` 制造短暂空帧。

压力回归继续暴露出两项同步成本，现也已修复：第一，无碰撞 Source/Fragment 曾先生成
tri-mesh/convex 物理数据再关闭碰撞；现在碰撞策略进入 payload，无碰撞装饰不会烹饪
物理数据或启动 Chaos。第二，多名玩家同帧切割会重复扫描全部缓存 Source 并堆叠完整
事务；现在伤害先按区块和世界包围盒查询，GameWorld 的世界切割进入服务器 FIFO，每帧
按固定预算处理。编辑器纯事务测试仍保持同步，未弱化 prepare→commit 回滚约束。

### 已修复 P1：进度系统跨模块副作用可能半提交

原实现的 `UMatterFluxProgressionComponent::ApplyEffectsAuthority` 可以先修改生命值或法杖法力，再向魔法背包发放奖励。如果后一个动作失败，前一个动作没有回滚。

现已由 `ApplyProgressionEffectsAuthority` 在魔法背包内部先复制法术、法杖、装备和法力状态，完整校验整批奖励后一次提交；Progression 先验证生命值、Gameplay Tag 和全部魔法奖励，再执行不会失败的属性提交，最后才发送可重入的 Gameplay Event。非法奖励与法力恢复的组合测试会逐字段确认法力、数组和 revision 均未变化。

### 已修复 P1：多人世界物质模拟只跟随第一个 PlayerController

原实现中地形和装饰使用所有玩家焦点的并集，但权威物质模拟使用 `GetFirstPlayerController()`。多人分散探索时，非首位玩家附近的流体、气体和颗粒不会进入活动区。

现由 Actor 收集、去重并排序所有权威 Pawn 所在的材质 chunk；`FChunkedMaterialWorld` 按距离环和焦点 round-robin 生成活动块，始终受 `MaxActiveChunks` 硬预算约束。快照 v2 写入规范化的完整焦点列表，导入仍兼容 v1 单焦点格式。双焦点纯测试、分离玩家 World 测试以及 2～4 人 near/far PIE 均已覆盖该行为。

### 已修复 P2（第二阶段）：录制 Codec、ReplayRuntime 与 UE Adapter 混合

原 `MatterFluxSessionRecorderSubsystem.cpp` 为 2,055 行。JSON schema 校验是纯 computation，截图和回放则依赖 Engine/Viewport/World；它们共处一个 implementation，格式知识和 UE 生命周期无法独立定位。

现已提取 `MatterFluxSessionRecordingCodec.cpp`，公共 interface 保持为 `ParseLaunchOptions`、`SaveToJson`、`LoadFromJson`；schema、数量/尺寸预算、payload 校验和规范化排序由私有 policy 集中拥有。`LoadFromJson` 同时修复为真正的 prepare→commit：坏 JSON 不会先清空调用者已有 recording。

第二阶段又提取了 184 行的 `FReplayRuntime`，以 `Initialize/Advance` 两个主要操作隐藏
operation/state/screenshot 游标、持续移动表、hitch 折叠、未来状态插值和完成判断。
Subsystem 当前约 1,009 行，不再直接维护回放游标；同一帧的 movement 与 expected state
按 PlayerId 稳定排序，非法倒退时间不提交候选状态，完成信号只发一次。文件、Viewport、
Character 应用、截图执行和退出码仍留在 UE adapter。决策与剩余测试记录在
`Architecture/ADR-003-Recording-Replay-Runtime.md`。

第三阶段按 ADR-004 把 types、Codec、Policy、ReplayRuntime 与整个
`UMatterFluxSessionRecorderSubsystem` 迁入 `MatterFluxDeveloper`。Runtime Character
只拥有通用 `EMatterFluxPlayerOperation`、多播事件和经过验证的中继 RPC，不再 include、
查询或命名 recorder。`Json` 也从 Runtime Build.cs 删除。这样录制能力仍可服务 Editor 和
Development Game，而 Shipping 从 Target 依赖图中排除整套 JSON、文件、截图和回放实现。

### 已修复 P2：开发者捕获工具位于主 Runtime module

原 `MatterFluxVisualCapture.cpp` 超过 1,000 行，并直接依赖可游玩世界和碎片实现。它对 Development 自动验收很有价值，但不应自然进入 Shipping 依赖面。

现已建立单向依赖的 `MatterFluxDeveloper → MatterFlux` DeveloperTool module，并把视觉/UI
捕获、树木点燃和真实 GAS 法杖触发等六个开发命令整体移入。Game Target 只在
`bBuildDeveloperTools` 时显式加入该 module；Shipping 不链接它。Runtime 不反向 include、
链接或保存这些命令名。`MatterFluxTests` 通过 Developer module interface 验证 Editor 中
模块已加载和命令已注册。跨 DLL 使用的 `LogMatterFlux` 同时补上模块导出属性。

### P2：文档曾把过宽模块描述成最终架构

旧文档把世界 Actor 和录制 Subsystem 都称为“最终理想的单一深模块”。深 module 的判断依据应是窄 interface 隐藏大量 implementation，而不是“所有代码放在一个地方”。本轮已修正文档表述并增加 ADR。

## 本轮已实施优化

1. `UMatterFluxMagicWorkbenchWidget` 与 `UMatterFluxShellWidget` 只保留 UMG 生命周期、状态绑定和领域命令。
2. Slate 树分别进入私有 `MatterFluxMagicWorkbenchSlate` 和 `MatterFluxShellSlate` deep module，通过 create/refresh interface 访问。
3. 设置内容继续由唯一 `SMatterFluxSettingsPanel` 提供；本轮又抽出 `SMatterFluxPaperWindow / SMatterFluxPaperTab`，Shell 和工作台不再分别实现大页面窗框、页签、帮助与关闭按钮。
4. 新增 `MatterFluxPaperStyle`，统一黑白颜色 token、字体、按钮状态和双层描边面板；Shell、工作台、设置和任务追踪共用。
5. 新增 `MatterFluxReplicatedMaterialState` module，集中压缩预算、CRC 和原子 net serialization。
6. 删除世界 Actor 中未调用的 bit-mask 打包代码。
7. 修复法术投射物重复执行 presentation 时以 MID 创建 MID 的非法材质父链；现在动态材质实例只创建一次并复用。
8. 将 Enhanced Input、InputCore、PhysicsCore 和 ProceduralMesh 从 Runtime module 的 public dependency 移到 private dependency。
9. 魔法奖励、法杖奖励和法力恢复合并为一个 prepare→commit 批处理，消除 Progression 的跨组件半提交窗口。
10. 材质活动区改为确定性多焦点 round-robin；活动状态协议升级到 v2 并保留 v1 导入兼容。
11. 显示型碎片 Source 进入独立区块代理；交互时按 SourceId 精确提升为 Actor。
12. 无碰撞 Source/Fragment 不再创建物理三角网格、凸包或 Chaos 刚体；payload 复制碰撞策略。
13. 世界切割先走局部区块/包围盒查询，多人同时请求通过有界 FIFO 分摊到服务器帧。
14. 显示型碎片 Actor 按帧预算实例化，避免单帧集中创建 ProceduralMesh 和 MID。
15. 新增 `FSimulationRuntime`，把材质 fixed-step、焦点状态和复制 revision 接受/拒绝从世界 Actor 移入 material deep module。
16. 材质、地表燃烧和流式层的运行时 ISM 更新改为保留旧实例、批量原地更新并只增删尾部，消除整层清空闪动。
17. 显示型 Source 代理增加独立的 128 区块默认预热上限；当前森林在加载阶段建好代理，移动边界不再同步剖分。
18. 新增 `FGroundCombustionRuntime`，把地表燃烧 fixed-step、mask、dirty chunks、revision、客户端去重和存档时间债移出世界 Actor；分块复制改为先完整编码整批 payload，再提交 revision 和清除 dirty。
19. 地表燃烧重生成改为候选状态成功后一次提交，不再先清空火焰、烟雾和焦痕再构建，消除跨帧消失/重现。
20. 抽出内部 `MatterFluxInstanceVisuals` 同步器；可破坏 Source 自身的火焰和烟雾也改为原地更新、只增删尾部，不再每 0.1 秒整批 `ClearInstances`。现存显式清空只用于技能特效结束，以及 dedicated server 上无需渲染的无碰撞层。
21. 所有 pristine Source（包括有碰撞树干）进入区块合并网格；测试种子 13579 的初始 Source Actor 从 37 降为 0。
22. 静态切割结果通过 `DematerializeFragmentSource` 回写逻辑 mask/revision 并重新合批，Source Actor 不再因一次交互永久驻留。
23. 新增修改 Source 的位压缩复制快照和稳定空间索引；晚加入客户端可恢复 mask，局部 Actor 查询不再依赖全世界遍历。
24. 修改 Source 复制改为 Fast Array；单项变化只发送对应 delta，并保留总条目数与字节预算。
25. 静态 Source 燃烧进入无 Actor 的 `FSourceCombustionRuntime` 活动表；残渣合回区块 mesh，火焰/烟雾合入共享 ISM。
26. 增加 Actor→逻辑运行时事务交接；燃烧中的静态树在离开流式窗口后继续模拟，不积累 Source Actor。
27. `IgniteNearest` 缓存 mask 尺寸，移除每次火焰命中为读取宽高而复制整份燃烧快照的 O(mask) 热路径。
28. 倒下树木的所有树冠不再作为附属 Source Actor；一个物理 Carrier 保存独立成员状态，并按稳定材质/颜色键合入同一 ProceduralMeshComponent。
29. 普通成员接管时发布零 mask tombstone；燃烧成员保留完整逻辑快照。两者都登记动态所有权，静态代理、晚加入客户端和存档恢复不会重新显示 pristine 树冠。
30. 声明碰撞的成员按局部 Transform 把凸体加入 Carrier 的同一 Chaos 复合刚体；逻辑独立不再制造额外物理 Actor。
31. 燃烧成员在砍倒时事务式交给世界级 `FSourceCombustionRuntime`；Carrier 复制二值 fuel/residue/burning mask 并合并木炭网格，世界共享 ISM 按 Carrier 当前 Transform 绘制火焰和烟雾。
32. 逻辑 Source 定义统一进入 `FSourceSpatialIndex`；局部点燃、火焰锥体、伤害物化和 bounds 查询不再扫描整个 `FragmentSourceChunks`。`SourceId` 查找同时增加 GUID→chunk locator，复制应用和运行时状态读取不再逐区块查找。
33. 已物化 Source Actor 的世界切割、法杖火焰和火球落点统一复用 `UFragmentSimulationSubsystem` 的注册空间索引；索引只做 broad phase，原有 bounds、锥体和逐 cell 精确判断继续作为 narrow phase。
34. 增加 65,536 次远距离局部点燃性能门禁。同一固定输入在修复前为 5,146.99ms，首次绿色运行 17.21ms，最终全量套件为 14.91ms，约提升 299 倍且结果仍为零命中。
35. 修复空间索引改变候选顺序后暴露的 aggregate 交接漏洞：已经切空、无燃烧残渣的 Source 保持为逻辑 tombstone，不再重新物化成不可见 Actor，也不会被错误吸收到动态 Carrier；带残渣状态仍按原规则交接。
36. `FGroundCombustionRuntime` 按 64×64 区块维护稀疏 burning/residue cell 集合；点燃、fixed-step、复制块导入和存档恢复都更新同一可重建派生索引。
37. 新增 `FSourceSpatialIndex::QueryMany`，对多个燃烧区块的桶候选统一去重、精确相交并只稳定排序一次；公开接口回归验证重复 bounds 也只返回一个 SourceId。
38. 地面可视刷新只读取当前 terrain window 对应的稀疏 burning/residue cell，不再扫描每个可见区块的全部 4096 cell。
39. `Ignite` 和 `AdvanceAuthority` 只同步改变过的 visible mask cell。512×384 mask 上 8192 次稀疏点燃由 25.15ms 降至 0.72ms。
40. `FGroundAdvanceResult` 返回稳定排序的 `ChangedCellIndices`；Authority 可视层按 cell 增删 ISM，并在流式窗口变化或客户端整块同步时回退到完整重建。
41. 阶段计时确认主要燃烧热点是逻辑 Source 每步触发 mask→轮廓→三角剖分→碰撞网格，而不是地面数组复制；一次性诊断日志随后已移除，正式代码保留细分 CPU Stat。
42. 区块代理在 Source 燃烧期间延迟实体网格重建，熄灭后的最终 mask 进入 0.5 秒按 chunk 去重批次；切割/物化仍即时 Flush。相同机器状态下燃烧增量由 335.28ms 降至 254.81ms。
43. 大世界门禁比较同一世界 120 个无火/燃烧 tick，要求燃烧增量小于 275ms，避免把机器固定基线误判成燃烧回归。
44. `FMatterFluxReplicatedFragmentSourceStateList` 现在自己维护 `SourceId → item index` 和总 payload 字节数；发布单个燃烧 Source 不再为预算统计和查找各扫描一次完整历史列表。新增/覆盖在同一接口中先验证条目与字节预算再提交，并在覆盖时保留 Fast Array 的 replication ID/key。4096 条状态上的 8192 次覆盖更新为 2.36ms，预算拒绝专项测试确认 revision、payload 和缓存计数均不会半提交。
45. `UFragmentSimulationSubsystem` 增加多 bounds 的稳定去重查询；已物化 Source 之间的燃烧传播不再调用会物化 pristine 邻居的 `GatherFragmentSourcesInBounds`，而是分别点燃逻辑 Source、查询已注册 Actor。地面火对已物化 Source 也复用同一批量索引。专项 RED 复现传播后 Actor 从 1 增至 7，修复后保持 1；大世界 120 tick 燃烧增量由 264.37ms 降至 171.32ms，约减少 35%。
46. 录制 schema、启动参数、预算、payload 校验和 JSON 编解码进入独立 `MatterFluxSessionRecordingCodec` deep module；Subsystem 只消费窄 interface。新增 RED 证明失败解码会错误修改调用者旧状态，修复后只在完整候选通过时提交。
47. 回放时间线进入纯 C++ `FReplayRuntime`；Subsystem 删除三个 replay index 和持续移动表。专项测试覆盖稳定批帧、hitch 折叠/插值与完成脉冲只发一次。一次性完成用例先稳定复现第二次 `Advance` 仍为 true，修复后转绿。
48. ReplayRuntime 补齐重复时间与倒退时间的事务覆盖：相同时间不会重复注入一次性操作或截图，但持续 Move 保留；倒退推进会清空输出且不消费未来 Cut/截图，随后合法推进可继续完整读取。
49. 新增 `MatterFluxDeveloper` DeveloperTool module，把 1,223 行视觉捕获以及点燃树木、真实法杖触发的开发命令移出 Runtime。新增模块存在/加载/六命令注册测试；Runtime 源码已无这些命令名，依赖保持单向。首次 Shipping 审计发现两个 Runtime 命令仍间接保留 `mf.Visual.Capture` 字符串，随后也一并迁移，避免只做表面文件搬运。
50. 新增通用 `EMatterFluxPlayerOperation` 事件 seam；Character 不再直接依赖 recorder。Recorder 以 GameInstance 过滤事件，远端 Client 只在本地记录成功后调用可靠 RPC，Host 对 relayed 事件只记录不重放。多播测试覆盖参数稳定与退订，Listen Host + Client 测试覆盖同一远端操作在两侧各恰好一次。
51. 会话录制 types、Codec、Policy、ReplayRuntime 和 Subsystem 全部迁入 `MatterFluxDeveloper`，跨 DLL API 宏同步调整；Runtime 删除唯一的 `Json` 依赖。真实 3 秒录制生成 2 operations/27 states，自动回放 PASS；Shipping 二进制不含录制开关、命令或 schema。
52. 新增 `MatterFlux::WorldStreaming` deep module；地形、关卡层和可破坏 Source 不再分别手写多焦点窗口。规划器统一去重、`X/Y` 稳定排序、等距镜头偏移、64 位坐标检查和 65,536 区块预算，失败保留旧输出。
53. 地形组件按规划数组而不是 `TSet` 遍历顺序创建；LRU 同帧活动区块共享一个 generation，淘汰平局按 `X/Y` 仲裁，因此缓存结果不再依赖 `TMap` 顺序。
54. Source 驻留判断改为 `SourceId -> Chunk` locator 的 O(1) membership，不再扫描可见区块中的全部 pristine Source 来分类少量已物化 Actor。
55. 流式焦点改为地形计划成功后提交；非法半径、预算或坐标溢出不会留下“焦点已更新、组件仍是旧窗口”的半提交状态。Unity Build 同时修复 Listen/Network 两个测试文件匿名 helper 同名的既有冲突。
56. `MatterFluxTests` 明确禁用 Unity Build。Forge 验收依赖真实跨翻译单元入口、private access 和 internal linkage；Unity 把 fixture 与调用者拼进同一 TU 后，MSVC 可绕开待补丁入口。相同 `MatterFlux.Forge` 命令在修复前稳定为 7/12，修复后为 12/12。
57. `FGroundCombustionRuntime::AdvanceAuthority` 不再用 `TSet` 汇总后再排序 changed cell，而是数组排序后线性去重；burning/residue 稀疏索引只在布尔成员资格实际翻转时更新。120 步 advance 从 108.57ms 降至 64.10ms，约减少 41%，published chunk、复制耗时和最终 41,324 个 residue cell 不变。
58. 复核确认区块代理早已在燃烧期间延迟实体几何重建，因此“剩余热点仍是每步 mask→轮廓→三角剖分”的结论已经过时。真正无界的是共享火焰/烟雾每 0.1 秒扫描全部历史 `StreamedFragmentSourceStates`。新增 `FLogicalSourceCombustionIndex` 只维护当前含 burning cell 的逻辑 Source；熄灭残渣继续保存在持久表，却不再进入表现、传播和计数热路径。索引从快照可重建，服务端步进、存档、Fast Array 晚加入、归档、材质化和销毁入口均已接入；GUID 使用四个整数分量稳定排序，不再转字符串分配。
59. `FMaskCombustion::CaptureState`、Source runtime 和 Ground runtime 不再在捕获开始时清空调用方输出。相同尺寸的连续捕获复用三张 mask 的已有容量，失败捕获保持旧快照不变。`FFragment2DSourceStreamingState` 直接承载 `FSourceRuntimeSnapshot`；世界 Actor 每步直接捕获到持久 map，存档恢复、归档和再次点燃也直接传同一 snapshot，不再构造局部三-mask 对象再复制。专项 RED 同时复现容量丢失与失败擦除，GREEN 后 4096 次三张 4096-cell mask 捕获在独立进程为 0.83ms、全量共享进程为 0.24ms。
60. 流式 Source 不再同时保存相同的公开 `RuntimeMask` 与燃烧 `FuelMask`。`StandaloneRuntimeMask` 改为私有，`GetRuntimeMask/SetRuntimeMask/CaptureCombustionState` 根据状态选择唯一真值；成功燃烧捕获会释放 standalone 数组。SourceActor、动态 Carrier、存档、Fast Array 客户端应用、代理和世界查询全部迁移到同一 interface。64-cell RED 明确得到 256 个存储值，GREEN 为 fuel/residue/burning 共 192 个；每个燃烧或残渣 Source 的 mask 存储减少 25%，fixed step 同时少一次 fuel→runtime 整数组复制。
61. 逻辑 Source 复制新增唯一运行时写入口 `UpsertAuthorityBatch`：按 GUID 稳定排序，先验证整批 mask/metadata，再预计算 item/byte 预算，最后直接复用 Fast Array item 的 packed 数组容量；任何失败均保持旧 revision、payload 与缓存计数。旧 `UpsertAuthorityItem` 已删除。燃烧 fixed step 和存档恢复都只提交一次 batch、只调用一次 `ForceNetUpdate`；活动/完成/发布 ID 与 update 数组跨帧复用容量，64 项排序走 inline allocator。1/16/64 Source 平均为 0.005/0.082/0.332ms，4096 项列表上的 8192 次更新为 16.77ms。
62. 客户端 Source Fast Array 改为 reconstructible delta apply plan：remove 保存稳定 SourceId，add/change 保存 ReplicationID，并在接收完成后通过当前 ItemMap 解析；不跨帧保留易失数组下标。计划按 GUID 稳定排序去重，同批删除/重加由最终 upsert 获胜，任何映射失配原子回退完整快照。WorldActor 只应用变化 Source 并在批末 Flush 一次；初次加入、代理重建和晚加入仍从当前完整真值恢复。4096 条历史下 1/16/64 delta 计划平均为 0.000/0.000/0.001ms。
63. Source 代理删除分离的 runtime/residue setter，改为唯一 `ApplySourceState` 深接口：先完整校验两张 mask，再原子提交 runtime、残渣材质/颜色和燃烧活动状态。代理建立 `SourceId -> {Chunk, SourceIndex}` 稳定 locator，4096 Source 同区块尾部的 8192 次成对更新从 100.05ms 降到最终 3.18ms；性能门槛收紧到 25ms。客户端复制应用也改为先准备候选、代理接受后再移动提交逻辑 map。燃烧 active 只更新真值，true→false 才按 chunk 延迟一次最终网格重建。
64. 区块代理按正反面/侧面拆 section 时不再各自复制整套挤出顶点。`AppendMeshPart` 先验证完整 index range，再按 triangle 首次引用顺序建立 Source 局部 remap；不跨 Source 焊点，不改变 triangle 顺序、法线、UV、材质或碰撞。2×2 Source 从 48 个 section 顶点（24 个未引用）降为 24 个（0 个未引用）；64 Source 两次构建的 section/位置/法线/UV/索引逐字段一致。

## 验证结果

- `MatterFluxEditor Win64 Development`：通过；依赖收缩后触发 36 个非 Unity 编译动作仍全部通过。
- `MatterFlux Win64 Development`：通过，生成 `Binaries/Win64/MatterFlux.exe`。
- `MatterFluxServer Win64 Development`：未进入编译；UE 5.8 Launcher 发行版明确返回 `Server targets are not currently supported from this engine distribution`。这不是 MSVC 版本或 C++ 编译错误，需要支持 Server Target 的引擎发行版验收。
- `MatterFlux.Material`：22/22 通过；`MatterFlux.Progression`：6/6 通过。
- `MatterFlux.Magic.Network`：1/1 通过；修复后日志不再出现 MID 父材质警告。
- `MatterFlux.Network.Scale`：优化后 2～4 人 near/far 共 6/6 通过。最终完整套件中 4 人 far 的移动 p95 为 25.86ms、动作 p95 为 216.90ms、最坏帧 303.41ms；服务器 Source 峰值 673、单客户端峰值 200、碎片峰值 114。旧基线的服务器 Source 峰值为 1325，且同类四人远区块切割曾出现 867.28ms 最坏帧。
- `MatterFlux.Performance.LargeWorldStreamingMovementAndCombustion`：通过。最终完整套件中的步行/冲刺/高速边界 Tick 最大分别为 0.93/1.95/2.04ms；修复前步行边界曾达到 18.96ms。地图加载阶段预热代理后生成耗时仍低于 5 秒门禁。
- 完整 `Automation RunTests MatterFlux`：加入真实 Listen Host + Client 门禁后 158/158 完成，0 项失败，命令行最终退出码 0；日志位于 `Saved/Logs/MatterFlux-FullAutomation-ListenHost-Final.log`。
- 最新 4 人 far 压力结果：移动 p95 20.62ms、动作 p95 189.54ms、最坏帧 240.85ms；服务器 Source 峰值由上一轮 673 降至 614，客户端峰值 144，碎片峰值 108。
- 标准 UBT（显式把 `UBA_ROOT` 放到项目 `Saved/UBA`）的 Editor/Game Development 均成功；Game 输出 `Binaries/Win64/MatterFlux.exe`。Server 目标仍被当前 Launcher 引擎发行版明确拒绝，与 MSVC 无关。
- GroundCombustion 深模块切片：UHT、运行时、Actor、wire format 和测试编译/链接通过；`MatterFlux.Combustion` 12/12 成功，其中 3 项新测试覆盖单 dirty chunk 批处理、坏包原子拒绝/幂等应用和存档 fixed-step 时间债。报告位于 `Saved/TestReports/GroundRuntimeCombustion/`。
- 逻辑 Source 燃烧集中回归：`MatterFlux.Combustion` 13/13 通过；Actor→逻辑交接新增专项测试通过；流式残渣归档与 dedicated server + 两客户端复制专项测试均通过。
- 最新 Editor 与 Game 串行构建使用 MSVC 14.44.35222 成功；Server 仍只被 Launcher 引擎发行版能力拦截，没有进入 C++ 编译。
- 动态 aggregate 完整切片：`MembersShareOneCarrierActor`、`BurningTreeMemberMovesIntoOneDynamicCarrier`、真实生成树场景、16 次多方向反复切树和 dedicated server + 两客户端 Carrier 成员一致性检查均通过。聚焦压力运行中反复切割平均 1.38 ms、最大 1.60 ms、最终砍倒 3.26 ms；最终完整套件共享进程运行中平均 2.15 ms、最大 2.67 ms、最终砍倒 5.15 ms。
- Listen Host + Client：新增 `MatterFlux.Fragment.Network.ListenHostAndClient`，验证一个 `NM_ListenServer` Host 与一个 `NM_Client` 都获得 Pawn、共享两人 PlayerState、切割 payload/材质逐字段一致且碎片移动收敛。测试首先复现了单一 PlayerStart 被 Host 占用后 Client Pawn 生成失败；现由 `AMatterFluxGameMode` 在附近确定性搜索无碰撞出生位置。默认 PIE 已改为 Host + Client 两玩家。
- 本轮局部查询最终验收：`MatterFlux.Network.Scale` 的 2～4 人 near/far 为 6/6；4 人 far 移动 p95 24.78ms、动作 p95 62.31ms，服务器/单客户端 Source 峰值均为 8。完整 `Automation RunTests MatterFlux` 为 158/158，日志位于 `Saved/Logs/MatterFlux-FullAutomation-SpatialQueries-Final.log`；Editor 与 Game Development 均使用 MSVC 14.44.35222 构建成功。
- 本轮稀疏燃烧与代理最终态合并验收：`MatterFlux.Combustion` 15/15、`MatterFlux.Fragment.SpatialIndex` 3/3、`MatterFlux.Fragment.Network.ListenHostAndClient` 与大世界性能门禁均通过。最终性能日志为无火 348.52ms、燃烧 622.75ms，增量 274.23ms/120 tick；Editor/Game Development 使用 MSVC 14.44.35222 构建成功。日志为 `Saved/Logs/MatterFlux-GroundSparseUpdate-Green.log`、`MatterFlux-CombustionFinalMeshBatch-Final.log`、`MatterFlux-Combustion-Final.log`、`MatterFlux-SpatialIndex-Final.log` 和 `MatterFlux-ListenHostClient-Final.log`。
- Source 复制状态增量索引最终验收：Editor/Game Development 使用 MSVC 14.44.35222 冷构建成功；`FragmentSourceStateUpsertScalesWithUpdates` 为 2.36ms/8192 updates，`FragmentSourceStateBudgetsAreAtomic`、燃烧 15/15、空间索引 3/3 和 Listen Host+Client 1/1 均通过。大世界无火 338.37ms、燃烧 602.74ms，增量 264.37ms/120 tick。对应日志为 `MatterFlux-FragmentSourceUpsert-Green.log`、`MatterFlux-FragmentSourceBudgets-Green.log`、`MatterFlux-Combustion-SourceUpsert-Final.log`、`MatterFlux-SpatialIndex-SourceUpsert-Final.log`、`MatterFlux-LargeWorld-SourceUpsert-Final.log` 和 `MatterFlux-ListenHostClient-SourceUpsert-Final.log`。
- 燃烧 Actor 空间查询最终验收：Editor/Game Development 使用 MSVC 14.44.35222 构建成功；`MatterFlux.Fragment.SpatialIndex` 4/4、`MatterFlux.Combustion` 16/16、Listen Host+Client 1/1 和大世界性能门禁均通过。大世界无火 238.77ms、燃烧 410.09ms，增量 171.32ms/120 tick。对应日志为 `MatterFlux-SpatialIndex-Batch-Green.log`、`MatterFlux-Combustion-Spatial-Final.log`、`MatterFlux-LargeWorld-CombustionSpatial-Final.log` 和 `MatterFlux-ListenHostClient-CombustionSpatial-Final.log`。
- 录制回放 deep module 最终验收：Editor/Game Development 使用 MSVC 14.44.35222 构建成功；`MatterFlux.Recording` 8/8、Listen Host + Client PIE 1/1 通过。日志为 `MatterFlux-Recording-ReplayRuntime-Transactional-Final.log` 和 `MatterFlux-ListenHostClient-ReplayRuntime-Final.log`。
- Developer 隔离最终验收：Editor 中六个捕获/调试命令测试 1/1；Development Game 明确编译 Developer 源；Shipping 只编译 Runtime。二进制逐字符串审计确认 `MatterFluxDeveloper` 与六个命令在 Development 为 true、Shipping 全为 false。
- 录制 Developer 迁移最终验收：玩家操作多播 1/1、`MatterFlux.Recording` 8/8、带 active recorder 的 Listen Host + Client 1/1、`MatterFlux.Network.Scale` 2～4 人 near/far 6/6。Host 输出文件包含唯一远端 CameraZoom，Client 明确记录“no duplicate recording file”。真实录制日志 `Saved/Logs/RecordE2E.log` 生成 2 operations/27 states，`Saved/Logs/ReplayE2E.log` 回放 PASS。迁移后的 Editor、Development Game、Shipping 均构建成功；`MFRecord`、`MFReplay`、三个 `mf.Record.*` 与 schema 在 Shipping 扫描中全为 false。Network Scale 日志为 `Saved/Logs/NetworkScalePlayerOperationFinal.log`。
- 确定性世界流式最终验收：规划器 4/4、Playable 9/9、Listen Host+Client 1/1、Network Scale 6/6。大世界在 1200/2500 cm/s 下各跨 9 个边界，最终边界 World Tick 最大 0.31/0.30ms。Forge 非 Unity 修复后 12/12，Combustion 16/16；完整 `Automation RunTests MatterFlux` 为 175/175、退出码 0。Editor、Development Game 和 Shipping 均使用 MSVC 14.44.35222 构建成功。对应决策见 `Architecture/ADR-005-Deterministic-World-Streaming-Plan.md`，全量日志为 `Saved/Logs/MatterFluxWorldStreamingFullGreen.log`。
- 逻辑 Source 活动燃烧索引最终验收：专项 RED 稳定复现熄灭历史仍常驻活动集合的 3 个失败断言，GREEN 后 `MatterFlux.Combustion` 17/17、燃烧 Source 流式恢复 1/1、大世界性能门禁通过；完整 `Automation RunTests MatterFlux` 为 176/176，0 失败、0 未运行。Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。决策见 `Architecture/ADR-006-Active-Logical-Source-Combustion-Index.md`，全量日志为 `Saved/Logs/MatterFluxLogicalSourceIndexFull.log`。
- 可复用 Source 快照最终验收：容量复用/失败原子性专项、`MatterFlux.Combustion` 18/18 与 65,536 个已熄灭 Source 长期历史门禁均通过；32 个活动火源的 10,000 次刷新为 5.65～6.87ms。完整 `Automation RunTests MatterFlux` 为 178/178，0 失败、0 未运行。Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。决策见 `Architecture/ADR-007-Reusable-Combustion-Snapshots.md`，全量日志为 `Saved/Logs/MatterFluxReusableSourceSnapshotFull.log`。
- Source runtime mask 单一真值最终验收：专项 RED→GREEN、燃烧 19/19、完整 Save 组与燃烧 Source 流式归档专项均通过；完整 `Automation RunTests MatterFlux` 为 179/179，0 失败、0 未运行。大世界共享进程控制段 234.64ms、燃烧段 391.92ms，门禁通过；Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。决策见 `Architecture/ADR-008-Canonical-Source-Runtime-Mask.md`，全量日志为 `Saved/Logs/MatterFluxCanonicalRuntimeMaskFull.log`。
- Source 事务式批复制最终验收：网络事务 3/3、燃烧 19/19、Save 5/5、Listen Host+Client 1/1、逻辑 Source 双客户端复制 1/1；完整 `Automation RunTests MatterFlux` 为 182/182，0 失败、0 未运行。1/16/64 Source 平均批成本为 0.005/0.082/0.332ms，4096 项上的 8192 次更新为 16.77ms。Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。决策见 `Architecture/ADR-009-Transactional-Source-Replication-Batches.md`，全量日志为 `Saved/Logs/MatterFluxSourceBatchFull.log`。
- Source 客户端增量应用最终验收：计划行为 3/3、Listen Host+Client 1/1、双客户端 Source 收敛 1/1；完整 `Automation RunTests MatterFlux` 为 186/186，0 失败、0 未运行。4096 条历史下 1/16/64 delta 计划平均为 0.000/0.000/0.001ms；Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。决策见 `Architecture/ADR-010-Incremental-Client-Source-FastArray-Apply.md`，全量日志为 `Saved/Logs/MatterFluxClientDeltaFull.log`。
- Source 代理原子状态最终验收：代理行为 2/2、4096 Source 性能 1/1（8192 次成对更新 3.18ms）、燃烧 19/19、Save 5/5、Listen Host+Client 1/1；最终完整 `Automation RunTests MatterFlux` 为 189/189，0 失败。Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。决策见 `Architecture/ADR-011-Atomic-Proxy-Source-State-And-Stable-Locator.md`，全量日志为 `Saved/Logs/MatterFluxProxyTransactionalFullAutomation.log`。
- 代理 section 紧凑化最终验收：FragmentSourceProxy 4/4、64 Source 批量逐字段确定性、大世界移动/燃烧和 Listen Host+Client 均通过；完整 `Automation RunTests MatterFlux` 为 191/191，0 失败。2×2 Source 的 section 顶点提交量减少 50%。Editor、Development Game、Shipping 均使用 MSVC 14.44.35222 构建成功。决策见 `Architecture/ADR-012-Compact-Proxy-Mesh-Sections.md`，全量日志为 `Saved/Logs/MatterFluxProxyCompactFullAutomation.log`。
- `/Game/Default` 编辑器 `MAP CHECK`：0 个错误、0 个警告。
- 2026-08-10 阻塞项发布门禁：修复 `Verify-MatterFluxRelease.ps1` 中写死的 113 项测试数量和已更名 aggregate 用例；脚本现在以 Automation 实际发现数量为准，并要求所有发现项逐项成功。`20260810-blocking-final` 运行中 Editor/Game Development 构建、191/191 Automation（包含 `ListenHostAndClient`）、`/Game/Default` Map Check、完整 Cook/Stage/IoStore 以及打包程序启动退出冒烟全部通过。按当前 Host + Client 范围显式跳过 Server Target；本轮未发现需要继续修改游戏代码的发布阻塞项。
- 2026-08-10 四人网络与纯画面收口：新增真实 `Host + 3 Clients` PIE 门禁，逐客户端验证 Pawn/PlayerState、Source、切割 payload、材质、FragmentId 和碎片位置收敛；客户端 ProceduralMesh collision cook 改为异步，服务器权威碰撞/质量事务保持同步。完整套件中的 4 人 far/near 移动 p95 为 20.23/20.28ms，动作 p95 为 39.11/43.22ms，最大帧为 259.70/124.76ms；最终 `Automation RunTests MatterFlux` 为 192/192，0 失败，日志位于 `Saved/Logs/MatterFlux-GoalFinal-FullAutomation-Retry.log`。画面侧只调整体素场景曝光后的 AO、低强度 Bloom/Vignette、太阳软阴影与体素材质明暗，并增加无碰撞的深绿色地图底景以消除有限地图外黑洞；最终截图为 `Saved/Screenshots/WindowsEditor/HighresScreenshot00037.png`。Editor/Game Development 均使用 MSVC 14.44.35222 构建成功。
- 2026-08-10 遮挡描边与贴地接缝：`M_PlayerOutline` 改为 SceneDepth/CustomDepth/stencil 1 的隐藏掩码内侧边缘，只覆盖被遮挡角色像素；放在 Tonemapping 后、priority 1000，并为非隐藏像素提前退出。树干/岩石/草/花统一埋入半个自身 voxel，树干按实际深度偏移后的 XY 采样地表，消除地形连接处共面 Z-fighting。新增材质语义和贴地深度门禁，`MatterFlux.Playable` 10/10，最终完整 Automation 193/193；Editor/Game Development 均使用 MSVC 14.44.35222 构建成功。8 帧固定 seed 分析中，左侧静态树区 `range_gt8` 从 0.50% 降至 0.20%，上方静态地面从 0.70% 降至 0.26%。日志为 `Saved/Logs/MatterFlux-OcclusionAndGroundSeam-FullAutomation.log`，序列位于 `Saved/Screenshots/WindowsEditor/MatterFluxStability/20260810-213035`。

本轮代理/物理/切割预算切片完成后已重复 Editor、Game、完整自动化和网络矩阵。Server 构建需换用支持该 Target 的 UE 发行版。

## 推荐的后续顺序

1. 长期历史门禁已经覆盖 65,536 个已熄灭 Source，并证明表现工作只随 32 个当前火源增长。下一步把同一场景接到真实 World Actor/ISM adapter，分别记录 active plan、transform 生成和实例同步，避免纯索引门禁掩盖渲染 adapter 回归。
2. Source 复制两端已分别成为服务端批事务和客户端 delta 计划；代理查找、原子状态应用与燃烧最终网格 Flush 也已有行为/性能门禁。下一步应使用 Unreal Insights 在真实多人燃烧压力场景分别观察 mask 解包、轮廓/三角剖分和 ProceduralMesh/Chaos 提交，不再继续微调已经低于毫秒级的 plan/locator。
3. 录制已完整进入 Developer module；把本轮“单机录制→回放 PASS”和“Listen Host 远端操作恰好一次”保留为定期门禁。未来只有在真实多进程丢包/重连数据证明需要时，再增加录制确认或序列号协议。

可视化评审报告位于本机临时目录：`C:\Users\hepta\AppData\Local\Temp\matterflux-architecture-review-20260808.html`。
