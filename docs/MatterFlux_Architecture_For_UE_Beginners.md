# MatterFlux 当前架构：UE 初学者导读

## 1. 先记住一条主线

在 Unreal Engine 项目里，`AActor`、`UActorComponent`、`UGameInstanceSubsystem` 和 `UUserWidget` 很重要，但它们不应该自动成为所有业务逻辑的家。

MatterFlux 现在采用的方向是：

```text
UE 输入 / 生命周期 / 复制
          ↓
       Adapter
          ↓
窄 interface 的 Deep Module
          ↓
纯数据、算法、状态转换或 Slate implementation
```

Adapter 是“接 UE 的插头”，deep module 是“真正隐藏复杂性的机器”。例如角色输入必须在 `ACharacter` 中绑定，但 mask 轮廓和三角剖分不需要知道 Actor 是什么。

## 2. 项目中的主要系统

### 可破坏 mask 与碎片

入口在 `FragmentSimulationSubsystem`，几何规则在 `FragmentGeometry`。`Fragment2DSourceActor` 只用于正在交互或已经脱离地形、确实拥有独立世界生命周期的 Source。

一次切割先在临时 mask 上计算，再提取 8-neighbor component、外轮廓和孔洞，进行约束 Delaunay 三角剖分，最后生成正面、背面和独立侧面顶点。只有整个计划有效时才提交 revision 和 actor 状态。这叫事务式 mutation。

森林里“每棵树有自己的 mask”不等于“每棵树必须是 Actor”。当前实现把两件事分开：

```text
逐 Source 逻辑记录（GUID / mask / revision / 材质 / 聚合关系）
                         ↓
        区块代理按材质合并视觉三角形与静态碰撞
                         ↓ 仅在真正脱落后
              独立 Fragment / 物理 Actor
```

玩家砍树时，目标可短暂进入现有事务管线；仍与土地相连的树桩马上回写逻辑 mask 并重新合入区块。只有倒下的树体或飞出的碎片需要独立 transform、Chaos 与 replicated movement，因此才继续作为 Actor。正在燃烧的静态 Source 也由世界级逻辑 runtime 推进，不需要常驻 Actor；代理只在燃烧结束后按区块合并重建最终网格。

联机时初始森林由相同 seed 与 Lua 内容重建，不复制成百上千个 Actor。服务器只复制被修改 Source 的 `SourceId + revision + 1-bit mask`，所以晚加入客户端也能看到同一个树桩，而不依赖它是否碰巧收到过短命 Actor。

这些修改使用 UE 的 Fast Array。服务器把同一个 fixed step 改变的 Source 先整批校验，再只
唤醒一次网络更新；客户端普通收包也不会扫描全部历史 Source。删除回调立即保存稳定的
`SourceId`，增加/修改回调保存 `ReplicationID`，等 UE 完成本次数组整理后再通过 `ItemMap`
解析成当前下标。这一点很关键：Fast Array 回调给出的数组下标可能在删除和压缩后变化，
把旧下标留到下一帧会改错树。若 ID 无法重建，客户端丢弃整份 delta 计划并从当前完整复制
状态恢复；初次加入和关卡代理重建也走这条慢路径。因此增量路径负责性能，完整快照负责纠错。

delta 解包后通过代理的单一 `ApplySourceState` 接口提交 runtime、残渣和燃烧状态。代理内部用
`SourceId -> {区块, 数组下标}` 直接定位，不在区块中逐棵扫描；它会先检查两张 mask，再一起
提交。客户端也先准备完整候选状态，代理接受后才替换逻辑 map，因此不会出现“树的逻辑已经
烧掉、画面仍是旧树”的半提交。4096 个同区块 Source 中对尾部 Source 更新 8192 次，旧实现
为 100.05ms，最终实现为 3.18ms。

代理把完整挤出网格分成正反面和侧面 section 时，也只复制该 section 的 triangle 真正引用的
顶点。过去两个 section 都携带整套顶点，最小 2×2 Source 因而提交 48 个顶点，其中一半没有
任何索引；现在按 triangle 首次引用顺序做局部 remap，只提交 24 个且 0 个未引用。它不跨
Source 焊点，所以硬边法线、UV、材质和逻辑身份都不变。

### 随机世界与流送

`MatterFluxPlayableLevel` 根据 seed 和 Lua registry 生成地形高度、河流、树木、花草、岩石和 fragment source。`AMatterFluxPlayableWorldActor` 目前负责把布局变成 UE 组件，并管理地形、装饰、碎片和物质分块的可见性。

区块窗口的纯计算已经进入 `MatterFluxWorldStreamingPlan`。服务器收集所有玩家焦点，客户端
只收集本地玩家焦点；规划器把焦点附近的正方形窗口做并集，并返回按 `X/Y` 排序的数组。
地形还额外传入 `(1,-1)` 偏移，因为斜置的 2.5D 镜头会多看到那个方向的一块区域。

这里“排序”不是为了好看。`TSet` 和 `TMap` 很适合快速查找，但不能承诺跨进程迭代顺序。
如果直接按 `TSet` 顺序创建 ProceduralMesh，Host 和 Client 可能得到不同的 UObject 创建
顺序；如果按 `TMap` 顺序增加 LRU 时间，同一帧的区块也会被人为分出先后。当前实现让
同帧活动区块共享 generation，平局按坐标淘汰，行为因此可复现。

规划也是一个小事务：先在临时数组里完成去重、预算和坐标检查，成功后才替换旧窗口；
地形计划成功后，世界 Actor 才提交新的共享焦点。初学者可以把它理解成“先把购物清单
检查完整，再开始搬家”，避免只搬了一半。

这个 Actor 仍然过宽，是当前最重要的架构债务。Source 的合并渲染已进入独立组件，材质、
地表燃烧和流式窗口已有 deep runtime/plan；初学者不要继续向世界 Actor 随手添加 Tick
分支。下一步应继续把燃烧网格调度和生成协调器按窄接口切出，而不是按文件行数机械拆分。

### 液体、气体、颗粒与化学反应

`FChunkedMaterialWorld` 是纯 C++ deep module。它管理活动 chunk、归档 chunk、固定步长模拟、跨 chunk 边界移动、液体下落和横向流动、气体上升、颗粒堆叠及 Lua 配置的反应。

服务器运行权威模拟，并通过 `FMatterFluxReplicatedMaterialState` 把有大小上限、带 CRC 的压缩快照复制给客户端。Actor 收集所有权威 Pawn 的 chunk 焦点，材质世界再以确定性 round-robin 在固定预算内覆盖每个玩家。快照 v2 携带完整焦点集合，晚加入客户端不依赖过去的焦点历史；导入仍兼容 v1 单焦点存档。Actor 只决定何时发布或应用快照；压缩格式和活动区规则不写在 Actor 里。

### Lua 内容

Lua 不直接获得任意 UObject。项目提供受限能力接口，把 Materials、World、Entities、Spells、Wands、Items 和 Quests 编译成只读 registry。热重载先构造候选 registry、完成交叉引用和 hash 校验，成功后再原子替换。

这种设计比“Lua 随便调用所有 UE API”更适合联机：服务器和客户端可以比较内容 hash，脚本也不能绕过权威状态。

### GAS 法杖与法术编程

角色按装备键位激活 `GA_CastWand`。法杖底盘提供法力上限、恢复速度、容量、抽取数、释放间隔、充能时间、散布和乱序规则；Lua 法术通过受限能力组合形成 cast plan。

GAS 负责能力激活和服务器权威入口，`MatterFluxWandProgram` 负责纯编译，投射物 Actor 负责复制移动与命中。切割和火焰不是角色硬编码特技，而是法术产生的世界效果。

### 道具与任务

`UMatterFluxProgressionComponent` 用 Fast Array 复制道具栈和任务状态，纯规则在 `FMatterFluxProgressionRules`。道具、任务图、奖励和使用行为来自 Lua。

数组状态先在临时副本中计算；魔法背包再用 `ApplyProgressionEffectsAuthority` 同时准备法术、法杖、装备和法力变化。全部奖励合法后才一次提交，随后才修改不会失败的生命值并发送 Gameplay Event。这样非法奖励不会留下“法力已经恢复、奖励却没发”的半提交状态。

### 菜单、背包、设置和任务 UI

`UMatterFluxShellWidget`、`UMatterFluxMagicWorkbenchWidget` 是 UMG adapter，只处理 UObject 生命周期、键盘入口、状态绑定和领域命令。

真正的 Slate 树位于私有 `MatterFluxShellSlate` 与 `MatterFluxMagicWorkbenchSlate` deep module。设置内容由唯一 `SMatterFluxSettingsPanel` 构建；大页面白色窗框和顶部页签由唯一 `SMatterFluxPaperWindow / SMatterFluxPaperTab` 构建；颜色、字体、按钮状态和双层描边来自唯一 `MatterFluxPaperStyle`。因此在开始菜单和工作台中打开设置，内容与页面外壳都是同一个实现，不是两份长得相似的代码。

### 存档、多人和自动化

`UMatterFluxSaveSubsystem` 串联异步保存、异步读取和异步世界生成。主机使用 Listen Server；Dedicated Server PIE 测试用于验证服务器权威和两个客户端收敛。`MatterFluxTests` 是 Editor-only module，不进入 Game 或 Server 构建。

Session Recorder 能记录操作、状态和截图并回放，但它当前仍混合 JSON codec、命令行、采样和截图职责，属于下一阶段要深化的 module。

## 3. 为什么分 Public 和 Private

`Source/MatterFlux/Public` 是其他 UE module 可见的 interface；`Private` 是实现。一个类型只在 `.cpp` 使用时，对应 Build.cs 依赖也应优先放在 `PrivateDependencyModuleNames`。

本轮把 Enhanced Input、InputCore、PhysicsCore 和 ProceduralMesh 移到 private dependency，因为公开头只做前置声明，并不包含它们的头。这能减少下游重新编译和传递依赖。

## 4. 三种测试各测什么

1. 纯 C++ Automation：几何、布局、法术编译、任务规则、材质模拟和 codec。
2. World 测试：需要 Actor、Component、碰撞或 Tick 的行为。
3. 多人 PIE：RPC 权威、Fast Array、初始 payload、移动复制和客户端收敛。

不要为了“更真实”把所有测试都做成 PIE。能穿过 deep module interface 证明的行为，用纯测试更快也更稳定。

还有一个容易忽略的构建细节：UE 的 Unity Build 会把多个 `.cpp` 合成一个大翻译单元，
通常能加快编译，但会改变 internal linkage 和编译器可见范围。`MatterFluxTests` 包含 Forge
跨翻译单元入口补丁验收，所以这个 Editor-only module 明确设置 `bUseUnity = false`。这不是
“测试环境越慢越真实”，而是被测契约本身就是跨 TU；若测试的构建方式消除了这条边界，
测试就不再测原来的行为。Runtime module 仍可正常使用 Unity Build。

## 5. 新增功能时如何选择位置

- 新法术、法杖、材质、反应、任务或道具：先看能否只加 Lua module。
- 新纯规则：放进现有 deep module，或建立一个窄 interface 的新 module。
- 新输入：Character/Controller 只负责把输入转换成领域命令。
- 新复制字段：先判断客户端需要结果、事件还是完整状态，不要直接复制 implementation cache。
- 新 UI 页面：UUserWidget 只接生命周期；Slate implementation 使用共享 Paper style。
- 新跨系统奖励：先设计 prepare/commit/rollback，再写任何实际 mutation。

## 6. 推荐阅读顺序

1. `MatterFlux_UE_Beginner_Guide.md`：完整项目和 UE 基础。
2. `MatterFlux_Cutting_For_UE_Beginners.md`：mask、轮廓、三角剖分和事务破坏。
3. `MatterFlux_Material_Simulation_Beginner_Guide.md`：chunk 与固定步长模拟。
4. `MatterFlux_Lua_Content_System_Beginner_Guide.md`：Lua 安全接口和热重载。
5. `MatterFlux_Magic_System_Beginner_Guide.md`：GAS、法杖和法术编译。
6. `MatterFlux_Quest_Item_System_Beginner_Guide.md`：Fast Array、任务图和道具。
7. `MatterFlux_Menu_Settings_Save_Beginner_Guide.md`：菜单、设置、存档和 Listen Server。
8. `MatterFlux_Code_Quality_Review_2026-08-08.md`：当前风险与后续重构顺序。
9. `Architecture/ADR-009-Transactional-Source-Replication-Batches.md` 与
   `Architecture/ADR-010-Incremental-Client-Source-FastArray-Apply.md`：Source 复制两端的事务与增量设计。
10. `Architecture/ADR-011-Atomic-Proxy-Source-State-And-Stable-Locator.md`：复制状态进入区块代理时的原子提交、O(1) 定位与燃烧延迟重建。
11. `Architecture/ADR-012-Compact-Proxy-Mesh-Sections.md`：区块代理如何删除正面/侧面 section 的未引用顶点，同时保持确定性与碰撞语义。
