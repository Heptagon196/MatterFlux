# MatterFlux 可玩场景：一分钟启动

这份说明只解决一件事：在本机看到角色、随机地图、光照和可破坏物体，并立刻开始玩。

需要录制玩家操作、状态、随机种子并自动回放/截图时，请参阅
[MatterFlux 会话录制指南](MatterFlux_Session_Recording_Automation_Guide.md)。

## 最短路径

1. 关闭正在运行的 PIE，编译 `MatterFluxEditor Win64 Development`。
2. 双击 `MatterFlux.uproject`，打开 `/Game/Default`。
3. 点击编辑器上方的绿色 **Play**。

项目已把默认 PIE 网络模式设为 **Play As Listen Server**，玩家数为 1。这个窗口既是
服务器又是本地玩家，也就是常说的 Host 模式；不需要先启动独立服务器。

如果只想做完全离线的单人测试，在 Play 按钮右侧菜单中选择
**Net Mode → Standalone**。两种模式都能游玩：

| 模式 | 本机玩家 | 是否有权威服务器 | 用途 |
|---|---:|---:|---|
| Standalone | 1 | 有，当前进程就是权威 | 最快的离线单人测试 |
| Listen Server | 1 或更多 | 有，Host 同时也是玩家 | 推荐的单人/联机 Host 测试 |
| Client | 有 | 无 | 连接 Host，只发送操作请求 |
| Dedicated Server | 无 | 有 | 自动化和正式服务器，不用于单人画面预览 |

## 操作

| 输入 | 行为 |
|---|---|
| `W` / `A` / `S` / `D` 或方向键 | 按镜头朝向在地表移动 |
| `Space` | 跳跃 |
| 鼠标滚轮 | 2.5D 镜头缩放 |
| 鼠标左键 | 施放装备槽 1 的法杖；默认是带“地形切割”的伐木法杖 |
| 鼠标右键 | 施放装备槽 2 的法杖；默认是带“火焰喷流”的火舌法杖 |
| `I` 或 `Tab` | 打开/关闭法杖工作台；编辑背包、四个装备槽和法术顺序 |
| `J` | 直接打开/关闭任务日志；右上角会持续显示当前追踪任务 |
| `Q` | 施放装备槽 3 的法杖；默认是学徒法杖 |
| `E` | 施放装备槽 4 的法杖；默认是余烬法杖 |
| `R` | 请求服务器换一个随机地图 seed |
| `Escape` | 打开或关闭游戏菜单 |

工作台提供“法术编程”“法杖背包”“道具背包”“任务”“设置”五个独立页面；设置页的改动会即时应用并保存，保存游戏和返回菜单仍统一放在 `Escape` 菜单。第一次启动 Standalone 或 Listen Server 会显示开始菜单；
新游戏和加载存档期间会显示真实地图阶段与百分比。系统说明见
[MatterFlux 菜单、设置、存档与加载：UE 初学者指南](MatterFlux_Menu_Settings_Save_Beginner_Guide.md)。

角色使用 `ACharacter` 和 `UCharacterMovementComponent`，在地表 X/Y 平面移动。镜头
由 `USpringArmComponent` 和 `UCameraComponent` 构成 -45° yaw、-45° pitch 的
斜俯视 three-quarter view。项目默认使用 FXAA，并在这台镜头上显式关闭运动模糊；
这样一像素宽的地形台阶、花草、角色描边不会因为 TSR 的逐帧抖动采样而闪烁或拖影。
Lumen 和 Virtual Shadow Maps 仍然保留，因此稳定边缘并不以移除体积感和阴影层次为
代价。Sky Light 在地图完成生成后捕获一次，不再每帧重建天空 cubemap。

法杖与法术配置、工作台拖放和多人权威流程见
[MatterFlux 法杖、法术编程与 GAS：UE 初学者指南](MatterFlux_Magic_System_Beginner_Guide.md)。
任务和普通道具见
[MatterFlux 任务与道具系统：UE 初学者指南](MatterFlux_Quest_Item_System_Beginner_Guide.md)。

## 场景从哪里来

`AMatterFluxGameMode::StartPlay` 会在权威端自动生成
`AMatterFluxPlayableWorldActor`。这个 Actor 创建：

- 一块始终存在、带碰撞的土壤底层；
- 512×384 个 8 cm 地形像素；每个像素直接采样多频 Perlin 噪声并量化到
  8 cm 高度台阶，地图约 41×31 米；
- 按 64×64 像素合并的可见/碰撞地形块，以及由材质模拟在同一高度场上显示的溪流；
- 棕色树干、绿色树冠、草簇以及粉/黄/紫三色花簇的 MatterFlux mask source；
- Directional Light、Sky Light 和 Sky Atmosphere。

服务器复制 `MapSeed`。服务器和客户端都调用同一个 `BuildLevelLayout`，用
`FMath::PerlinNoise2D` 生成连续地势，并用 `FRandomStream` 生成确定性森林布局。
地形不再为每个像素建立一个 Cube/HISM instance；每个 64×64 块把顶面和裸露侧面
合并到三个 `UProceduralMeshComponent` material section，并用同一网格做精确静态
查询碰撞。水、沙、熔岩、蒸汽和石头由分块材质模拟生成，按照同一份地形顶面高度
放置，并按“材质 × 模拟区块”批量写入 ISM。未变化的区块通过稳定 hash 跳过上传，变化
区块原地批量更新实例，不会先清空整层再重建。花草树木仍各自拥有确定性的 mask、颜色和 ID，但未交互、
无碰撞的树冠、草簇和花簇会按区块合并进 `UProceduralMeshComponent`，不会各占一个
复制 Actor。树干等碰撞源保持 `AFragment2DSourceActor`；切割、火焰或相邻燃烧命中代理
时，服务器再用同一个 SourceId 和 mask 将它实体化。`R` 只在服务器更换 seed；客户端
收到 `OnRep_MapSeed` 后用相同布局重建地形与装饰代理，并接收需要交互的 source Actor。
无碰撞配置会一直传到脱落 fragment：花草树叶不生成 physics tri-mesh、凸包或 Chaos
刚体；只有树干等碰撞源和它们的碎片参与物理阻挡。

整张地图远大于常驻窗口。材质模拟使用 64×64 格块，在 9 块硬预算内以 round-robin
覆盖所有权威玩家的焦点及邻环；单人时等价于焦点附近 3×3 块；
静态地形使用 64×64 像素块，为每位玩家保留玩家中心 3×3 与镜头远端 3×3 的并集；
单个玩家的两套窗口重叠后通常是 14 块，同时提供可见网格和复杂静态碰撞。服务器
合并所有已控制 Pawn 的窗口，客户端只跟随本地玩家，避免第二位玩家离开首位玩家后
失去地形、碰撞或可破坏装饰。离开窗口的块会立即关闭
渲染和碰撞，但最多保留 48 块 LRU 热缓存，
回头时无需重新生成网格和烘焙碰撞；超过上限的最旧非激活块才会释放。
第二套窗口朝固定斜俯视镜头的远端视野 `+X/-Y` 方向偏移一块，既不把角色放在碰撞
窗口边缘，也避免为了覆盖镜头后方视野而扩成完整 5×5。
远端材质块使用 RLE 归档，远端装饰只保留确定性定义，不创建 Actor。进入窗口时只为
树干等碰撞源按帧预算生成 Actor；未交互的显示型 mask 由区块代理一次合批。受损但仍
连接地形的 source 离开窗口时也会卸载 Actor；世界 Actor 缓存当前 mask、revision 和
燃烧状态，返回时用相同 SourceId 重建，不需要永久保留每棵被砍过的树。
当前固定森林的显示代理块数低于默认 128 块热缓存上限，因此代理三角剖分在加载界面内
预热；移动跨块只切换完成网格的可见性。更大的未来地图超过该上限后自动保留按需构建，
不会为了消除尖峰而无界预建整张世界。

多人同时切割时，服务器先用伤害包围盒定位相邻 Source 区块，不再遍历整张活动地图；
合法命令进入 FIFO，每帧按固定预算执行。显示型碎片也按帧预算创建。四人同帧操作不会
丢失 mask 修改或碎片，只会把非关键表现摊到随后几帧。

运行时可输入 `stat MatterFlux`，查看 World Tick、World Rebuild、Terrain
Streaming、Decoration Streaming 和 Combustion 的 CPU 周期。

## 从命令行稳定截图

本节的 `mf.Visual.*`、`mf.UI.Capture`、`mf.Combustion.IgniteTree` 和
`mf.Player.Ability` 都由 `MatterFluxDeveloper` 注册，只存在于 Editor 和 Development
Game；Shipping 会从 Target 依赖图排除整个 DeveloperTool module。这些命令用于外部验收，
不是玩家正式作弊接口。

不需要聚焦窗口或模拟键盘。下面的命令会等待 Game viewport 就绪，再等 8 秒，执行
`HighResShot 2`，写盘后自动退出：

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\MatterFlux.uproject" -game -windowed -ResX=1280 -ResY=720 `
  '-ExecCmds=mf.Visual.Capture 8 2 1 1337 0 0' -log
```

参数依次是延迟秒数、截图倍率、是否截图后退出、可选固定地图 seed、是否打开魔法工作台、是否切到法杖背包页。最后两项均为 `0` 时只截取游戏场景；第五项为 `1`、第六项为 `0/1` 时，分别截取法术编程页或法杖背包页。图片写入
`Saved/Screenshots/WindowsEditor`。这个接口也适合以后做固定 seed 的视觉回归。

检查“画面静止时是否仍有元素闪动”可使用连续帧命令。它会等待地图生成和装饰流送
结束，再固定 seed、固定镜头连续保存 8 帧：

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\MatterFlux.uproject" -game -windowed -ResX=1280 -ResY=720 `
  -MatterFluxSkipStartMenu `
  '-ExecCmds=mf.Visual.StabilitySequence 8 0.25 1337 8 1' -log
```

参数依次为帧数、帧间隔、地图 seed、稳定等待秒数和结束后是否退出。输出位于
`Saved/Screenshots/WindowsEditor/MatterFluxStability/<时间戳>`。安装 Pillow 与 NumPy
后，可用 `Tools/AnalyzeVisualStability.py <一个或多个序列目录>` 比较地面、树木、
角色和河流的逐帧像素变化；河流本身是动态区域，判断渲染闪动时应优先看静态区域。

角色遮挡提示使用 `/Game/MatterFlux/Materials/M_PlayerOutline` 后处理材质。角色的每个
体素部件写入 `CustomDepth` 和 stencil 1；材质比较角色的 `CustomDepth` 与当前不透明
物体的 `SceneDepth`，只有角色深度更远时才把该像素判定为隐藏。随后对隐藏掩码做一次
3×3 内侧边缘提取，所以只会在遮挡物表面画出被挡住的那一段轮廓，可见身体不会被整圈
高亮。材质位于 Tonemapping 之后且优先级为 1000，因此它是世界画面的最终覆盖层，但
仍不会盖住 Slate/UMG。绝大多数非角色像素通过动态分支提前返回，邻域采样只发生在隐藏
角色像素上。

树干、岩石、草和花都是可切割的挤出 mask；它们的底部边界会生成一张水平侧面。如果
底面恰好等于地形高度，两张共面会争抢深度缓冲并产生 Z-fighting。生成器现在统一把
贴地装饰埋入半个自身 voxel：树干 6cm、岩石 5cm、草/花 3cm。树干还会按实际深度偏移
后的 XY 重新采样地表，避免跨越 8cm 地形台阶时重新共面；同一 aggregate 的整棵树共用
该高度，不会造成树干与树冠断开。

菜单 UI 使用独立截图命令；它会等待游戏视口、切换页面并把 Slate UI 包含在图片中：

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\MatterFlux.uproject" -game -windowed -ResX=1600 -ResY=900 `
  '-ExecCmds=mf.UI.Capture settings 1 1' -log
```

页面支持 `start`、`single`、`multiplayer`、`create`、`join`、`settings`、`save`、`load`、`progress` 和 `load-progress`。
后两项会发起真实地图生成或存档加载；运行 `load-progress` 时建议额外传入独立
`-UserDir`，不要覆盖日常存档。

## 点燃一棵树

在游戏控制台或 `-ExecCmds` 中运行：

```text
mf.Combustion.IgniteTree 991
```

命令会在服务器点燃第一棵生成树的树干。火会传播到树冠和附近地表，生成体素火焰、
烟雾以及木炭/灰固体残留。用于无人值守视觉验收时可一次传入截图参数：

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\MatterFlux.uproject" -game -windowed -ResX=1280 -ResY=720 `
  '-ExecCmds=mf.Combustion.IgniteTree 991 1.5 1 1' -log
```

后四个值依次是事件 seed、截图延迟、倍率、截图后是否退出。

## 大地图性能基准

自动化用固定 seed 创建 512×384 地图，生成一个被 PlayerController 控制的真实角色，
分别以 500、1200 和 2500 cm/s 连续穿过地图并跨越多个区块，然后流送装饰并点燃一棵树：

```text
Automation RunTests MatterFlux.Performance.LargeWorldStreamingMovementAndCombustion
```

测试记录每档速度的平均帧、P95、最大帧和区块边界 Tick，并要求边界 Tick 与 P95
低于 16.67 ms。瞬移不属于门禁；需要远距离传送时应在加载界面内完成目标区域准备。
该测试同时记录地面燃烧复制包大小。局部火势使用连续区间编码；只有当区间编码
不再划算时才退回完整位图，以避免 512×384 地面在每次燃烧更新都固定复制两份
24 KiB 位图。

2026-08-08 本机 Development Editor commandlet（8 cm 高度场、64×64 网格块、
14 块角色/镜头窗口并集）最终结果：

| 项目 | 优化后 |
|---|---:|
| 完整地图生成（含有界代理预热） | 1,760.19 ms |
| 玩家 500 cm/s | 431 帧、9 次边界；P95 2.22 ms，边界 Tick 最大 0.94 ms |
| 玩家 1200 cm/s | 180 帧、9 次边界；P95 2.66 ms，边界 Tick 最大 1.95 ms |
| 玩家 2500 cm/s | 87 帧、9 次边界；P95 3.25 ms，边界 Tick 最大 1.63 ms |
| 装饰 Actor 预算流送 | 61 帧完成，最慢 4.62 ms |
| 120 次移动/材质/树木与地面燃烧 Tick | 790.40 ms，约 6.59 ms/次 |
| 局部地面燃烧复制状态 | 2,379 bytes；完整双位图为 49,154 bytes |
| 常驻材质块 | 9 |
| 归档材质块 | 39 |

同一压力地图在优化前运行 180 秒仍不能完成初始化。基准阈值不是跨硬件的绝对承诺，
但会持续防止重新引入全图逐格归档、全图 HISM 或同步批量生成装饰的退化。

多人矩阵可单独运行：

```text
Automation RunTests MatterFlux.Network.Scale
```

当前固定场景覆盖 2～4 人 near/far 六种组合，全部使用真实 server/client world、GAS
输入、切割、火焰、碎片复制和跨区块移动。2026-08-08 优化后 6/6 通过；最重的四人
far 场景移动 P95 25.91 ms、动作 P95 222.39 ms、最坏帧 356.65 ms，保留 114 个峰值
碎片。优化前同类场景曾出现 867.28 ms 单帧尖峰。
本轮完整 `Automation RunTests MatterFlux` 已达到 148/148 通过；0 项失败、0 项未运行。

## 看不到角色或输入无效

先按下面顺序检查：

1. 确认打开的是 `/Game/Default`，不是空白临时关卡。
2. 确认 Play 菜单里 Spawn Player At 选择了 **Default Player Start**。
3. 单击游戏视口一次，让键盘焦点进入 PIE。
4. 若刚编译过 C++，关闭旧编辑器后重新打开；旧进程会锁住并继续加载旧 DLL。
5. Output Log 搜索 `MatterFlux`、`LogSpawn` 和 `EnhancedInput`。

如果要模拟第二个玩家，把 Play Number of Players 改为 2，并保留 Listen Server。
第一个窗口是 Host，第二个窗口是普通 Client；移动由 Character Movement 复制，
破坏和随机换图仍只由 Host 提交。

## 从外部触发玩家能力并截图

`mf.Player.Ability` 走的是真实 PlayerState ASC 和能力 GameplayTag，不是直接修改
场景的截图特例。语法为：

```text
mf.Player.Ability Cut|Flame [重复秒数] [截图延迟] [倍率] [截图后退出]
```

例如持续喷火 7 秒，在第 3 秒截图并自动退出：

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\MatterFlux.uproject" -game -windowed -ResX=1280 -ResY=720 `
  '-ExecCmds=mf.Player.Ability Flame 7 3 1 1'
```

切割把 `Flame` 改为 `Cut`。客户端普通输入只请求激活；两项能力均为
`ServerOnly`，mask、revision、燃烧状态和地面点燃仍只在 authority 上提交。刀光
和喷流由不可靠 multicast 播放短暂方块体素表现，Dedicated Server 会跳过绘制。
