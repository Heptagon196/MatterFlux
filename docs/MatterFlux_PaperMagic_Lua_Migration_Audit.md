# PaperMagic Lua 迁移审计

审计日期：2026-08-19

本审计逐文件对照 `C:/Users/hepta/Documents/Codes/PaperMagic/GameData/Scripts` 与
MatterFlux 的 Lua 内容、受限编译格式和服务器运行时。判断标准不是“存在同名 ID”，
而是玩家可观察的玩法结果是否存在、是否由服务器权威执行，以及是否有自动化测试。

## 结论

PaperMagic Demo 的内容逻辑已经按 MatterFlux 架构完成等价迁移：9 个法术、2 个法术
效果、3 件施法装备、1 个普通道具、5 类生物、10 个任务脚本、商人对话与 10 个商店
报价均有对应实现。PaperMagic 的 `Lib` 脚本没有逐行复制；其对象继承、行为树、背包、
任务通知和任意 Unity API 调用被替换为受限 Lua DSL、不可变 registry、纯 C++ 规则、
服务器 Actor/GAS、Fast Array 复制和事务存档。

本轮补齐了此前审计发现的两个真实缺口：

- `std.default` 与 `std.default_shoe` 现在保留原容量、法力、回复和施法间隔，并通过
  `starter_count` 作为“初始拥有但不强制绑定”的施法器进入背包。
- `std.test_boss` 不再把技能简化为一帧齐射。普通攻击为两发、每发确定性散射
  `±10°`；技能先获得 `500 cm/s` 水平与 `1500 cm/s` 垂直速度，再以 `0.2 s`
  间隔发射 12 个红色环形投射物，最后保持 1 秒收招状态。

## 逐类对照

| PaperMagic 目录 | 数量 | MatterFlux 对应 | 状态 |
|---|---:|---|---|
| `Spells` | 9 | `Content/Lua/Spells/PaperMagic` | 完成；伤害、法力、子节点、散射、颜色、圆轨与触发语义有编译测试 |
| `SpellEffects` | 2 | Projectile/Jump 世界执行器 | 完成；15 Unity unit/s→1500 cm/s、3 秒寿命、600 cm/s 跳跃 |
| `Equipments` | 3 | `Content/Lua/Wands/PaperMagic*.lua` | 完成；默认法杖、鞋型施法器、高级法杖均保留关键数值 |
| `Items` | 1 | `Content/Lua/Items/HealingPotion.lua` | 完成；权威恢复 30 生命，满血不消耗 |
| `Creatures` | 5 | `Content/Lua/Creatures` + Creature Actor/AI | 完成；巡逻、追击、后退、仇恨记忆、攻击、技能、掉落和商人交互 |
| `Quests` | 10 | `Content/Lua/Quests` | 完成；前置、子任务、可选目标、击杀、消费、奖励、追踪和存档 |
| `Text` | 1 | `Content/Lua/Dialogues/CampMerchant.lua` | 完成；商店入口、退出和扩展的传闻分支 |
| `Lib` | 8 | `MatterFluxEngine.lua` + C++ deep modules | 等价替换；不复制可绕过权限的 Unity API 桥接层 |

## 法术语义核对

| ID | 关键语义 |
|---|---|
| `std.default` | 10 伤害、10 法力、1500 cm/s、3 秒寿命 |
| `std.circle_trail` | 圆轨半径 300 cm，寿命乘 2 |
| `std.double_cast` | 两个子节点，目标散射范围 `±10°` |
| `std.triple_cast` | 三个子节点，目标散射范围 `±10°` |
| `std.set_color_red` | 子投射物 InitialOnly 表现覆盖为红色 |
| `std.trigger_on_collision` | 第一子节点为 1 秒载体；命中后以确定性随机方向生成第二节点 |
| `std.trigger_on_expired` | 第一子节点为 1 秒载体；消逝后沿当前方向生成第二节点 |
| `std.jump` | 服务器施加 600 cm/s 垂直速度，消耗 50 |

## 有意保留的架构差异

- 原项目按头、衣服、左右手、鞋子限制装备；MatterFlux 按用户要求统一为目标键位绑定。
  鞋型施法器仍存在，但不会硬编码占用空格键。
- 原 Lua 能直接访问 Unity Rigidbody、GameObject 和全局 Manager；MatterFlux Lua 只在
  内容加载时调用少量 builder 能力，Actor Tick 不执行 Lua。
- 原项目使用 `math.random()`；MatterFlux 的散射、洗牌和触发方向由事件 seed 确定，
  服务器和客户端可复现。
- AI 数值按当前 2.5D/三维体素场景做了距离调优；状态优先级和可观察行为保持一致，
  但没有复制参考项目依赖 Sprite/Rigidbody 的实现细节。
- Boss 环形技能使用 12 个互不重复的方向。参考 Lua 的闭区间循环可能重复 0°/360°，
  当前实现保留其明显意图并移除重复弹。

## 自动化证据

- `MatterFlux.Magic.Content.PaperMagicSpellLibraryIsComplete`
- `MatterFlux.Magic.Content.PaperMagicSpellsCompileWithEquivalentSemantics`
- `MatterFlux.Creatures.CastProgramBuildsDeterministicTimedVolleys`
- `MatterFlux.Creatures.TimedCastRunsThroughActorTimerLifecycle`
- `MatterFlux.Creatures.DefaultPackMigratesPaperMagicCatalog`
- `MatterFlux.Progression.ReferenceQuestFlowIsDeterministic`
- `MatterFlux.Progression.AuthorityComponentItemAndSaveTransaction`
- `MatterFlux.Progression.Network.DedicatedServerTwoClients`

本轮 Editor Development 构建成功；上述 `Creatures`、`Magic.Content` 与 `Progression`
测试组均为零失败。
