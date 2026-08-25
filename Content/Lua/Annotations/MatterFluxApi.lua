---@meta MatterFlux

-- 本文件只供 Lua Language Server 读取。MatterFlux 运行时不会把
-- Annotations 目录加入确定性内容包。修改 C++ 注册接口或
-- MatterFluxEngine.lua 的公开 DSL 后，需要同步更新这里的声明。

---@alias MatterFluxContentId string 内容定义的稳定 ID；通常使用“命名空间.名称”格式。

---@alias MatterFluxMaterialPhase
---| 'static' # 静态固体，不参与逐格流动。
---| 'powder' # 粉末，会受重力影响并堆积。
---| 'liquid' # 液体，会流动并填充低处。
---| 'gas' # 气体，会扩散和上浮。

---@class MatterFluxMaterialDefinition
---@field id MatterFluxContentId 物质稳定 ID。
---@field density number 密度；用于浮力、下沉和物质排序。
---@field hardness number 硬度；供切割和破坏规则使用。
---@field color_r number 基础颜色红色分量，范围 0～1。
---@field color_g number 基础颜色绿色分量，范围 0～1。
---@field color_b number 基础颜色蓝色分量，范围 0～1。
---@field color_a number 基础颜色 Alpha，范围 0～1。
---@field phase? MatterFluxMaterialPhase 物态，默认 static。
---@field mobility? integer 每步移动意愿，范围 0～255。
---@field dispersion? integer 横向扩散意愿，范围 0～255。
---@field movement_resistance? number 浸入该物质后的移动阻力倍率，范围 0～8；液体和粉末使用。
---@field lifetime_steps? integer 存在的固定模拟步数，范围 0～255；0 表示不会自行消散。
---@field shallow_opacity? number 浅水不透明度，范围 0～1。
---@field deep_opacity? number 深水不透明度，范围 0～1，且不小于 shallow_opacity。
---@field opacity_depth? number 达到深水不透明度所需的视线内液体深度，单位厘米。

---@class MatterFluxMaterialNamespace
---@field define fun(definition: MatterFluxMaterialDefinition) 注册物质；命名字段在加载时编译和校验。

---@alias MatterFluxSpellTriggerEvent
---| 'impact' # 载体碰撞时触发子法术。
---| 'expired' # 载体寿命结束时触发子法术。

---@alias MatterFluxProjectileVisualShape
---| 'orb' # 默认紧凑投射物。
---| 'plane' # 与地面平行的横向切割面。
---| 'vertical_plane' # 垂直于地面、沿飞行方向延展并以薄边前进的纵向切割面。

---@alias MatterFluxItemCategory
---| 'material' # 普通材料或货币。
---| 'quest' # 任务专用物品。
---| 'consumable' # 可主动使用并消耗的物品。

---@alias MatterFluxQuestCategory
---| 'main' # 主线任务。
---| 'side' # 支线任务。
---| 'objective' # 作为父任务组成部分的目标任务。

---@alias MatterFluxRewardKind
---| 'item' # 奖励物品。
---| 'spell' # 奖励法术卡。
---| 'wand' # 奖励法杖。

---@alias MatterFluxCreatureFaction
---| 'friendly' # 友好阵营，可用于 NPC。
---| 'hostile' # 敌对阵营，会攻击玩家。
---| 'neutral' # 中立阵营。

---@alias MatterFluxCreatureLevel
---| 'normal' # 普通单位。
---| 'elite' # 精英单位。
---| 'boss' # 首领单位。

---@alias MatterFluxCreatureBehaviorCondition
---| 'has_visible_target' # 当前决策帧能够直接看见目标。
---| 'has_target' # 当前看见目标，或者目标仍在记忆时间内。
---| 'target_too_close' # 可见目标进入配置的后退距离。
---| 'target_in_attack_range' # 可见目标进入配置的攻击距离。
---| 'attack_ready' # 普通攻击冷却已经结束。
---| 'skill_ready' # 已配置技能且技能冷却已经结束。

---@alias MatterFluxCreatureBehaviorAction
---| 'passive' # 停止移动并保持被动。
---| 'patrol' # 按配置的间隔转向和停顿巡逻。
---| 'chase' # 向当前目标移动。
---| 'retreat' # 背向当前可见目标移动。
---| 'attack' # 执行配置的普通攻击法术序列。
---| 'skill' # 执行配置的技能法术序列。

---@alias MatterFluxShopProductKind
---| 'item' # 出售物品。
---| 'spell' # 出售法术卡。
---| 'wand' # 出售法杖。

---@class MatterFluxSpellMetadata
---@field id MatterFluxContentId 法术的稳定唯一 ID，存档和网络状态都通过它引用。
---@field name string UI 中显示的法术名称。
---@field description? string 鼠标悬停提示中显示的法术说明。
---@field icon? string 图标资源键；由 UI 资源解析器解释。
---@field mana_cost? number 每次执行该法术消耗的法力值。
---@field starter_count? integer 新角色初始背包中赠送的数量。

---@class MatterFluxProjectileParameters
---@field damage? number 投射物命中造成的基础伤害。
---@field speed? number 投射物初速度，单位为 UE 单位/秒。
---@field lifetime? number 投射物最大存在时间，单位为秒。
---@field radius? number 投射物碰撞半径，单位为 UE 单位。
---@field gravity_scale? number 飞行阶段承受的世界重力比例；命中并转入材质世界后不再生效。
---@field body_material? MatterFluxContentId 从生成首帧起进入物质模拟的真实粒子材质 ID；留空时使用普通非物质投射物。
---@field material_amount? integer 生成的守恒物质粒子数量，范围 1～4096，默认 1；粒子独立运动并在接触后转入地表模拟。
---@field visual_shape? MatterFluxProjectileVisualShape 投射物外形；默认 orb。
---@field spawn_forward_offset? number 沿本次施法方向追加的生成距离，单位为 UE 单位。
---@field spawn_height_offset? number 相对施法者向上追加的生成高度，单位为 UE 单位。
---@field spawn_stationary? boolean 生成时不带水平初速度，仅由重力等后续运动驱动。
---@field cast_delay? number 对当前轮施法间隔的增量，单位为秒。
---@field recharge_time? number 对法杖充能时间的增量，单位为秒。

---@class MatterFluxProjectileModifierParameters
---@field damage_add? number 在乘算前附加到后续投射物的伤害值。
---@field damage_multiplier? number 后续投射物的伤害倍率，1 表示不变。
---@field speed_multiplier? number 后续投射物的速度倍率，1 表示不变。
---@field lifetime_multiplier? number 后续投射物的寿命倍率，1 表示不变。
---@field spread? number 对后续投射物散布角的增量，单位为度。
---@field override_color? boolean 是否覆盖后续投射物的显示颜色。
---@field color_r? number 覆盖颜色的红色分量，通常为 0～1。
---@field color_g? number 覆盖颜色的绿色分量，通常为 0～1。
---@field color_b? number 覆盖颜色的蓝色分量，通常为 0～1。
---@field color_a? number 覆盖颜色的不透明度，通常为 0～1。
---@field orbit_radius? number 轨道修饰半径，单位为 UE 单位；0 表示不使用轨道。
---@field cast_delay? number 对当前轮施法间隔的增量，单位为秒。
---@field recharge_time? number 对法杖充能时间的增量，单位为秒。

---@class MatterFluxDrawParameters
---@field spread? number 本次多重抽取附加的散布角，单位为度。
---@field cast_delay? number 本次多重抽取附加的施法间隔，单位为秒。
---@field recharge_time? number 本次多重抽取附加的充能时间，单位为秒。

---@class MatterFluxTriggerParameters
---@field trigger_event? MatterFluxSpellTriggerEvent 子法术的触发时机；默认在碰撞时触发。
---@field trigger_draw_count? integer 触发时从法术序列中读取的子法术数量。
---@field trigger_random_direction? boolean 是否为每个子法术生成随机方向。
---@field carrier_lifetime? number 触发载体的寿命覆盖值，单位为秒；0 表示使用投射物寿命。

---@class MatterFluxImpulseParameters
---@field vertical_impulse? number 施加给施法者的垂直冲量。
---@field cast_delay? number 施法后的额外间隔，单位为秒。
---@field recharge_time? number 对法杖充能时间的增量，单位为秒。

---@class MatterFluxSpellBuilder
---@field projectile fun(parameters: MatterFluxProjectileParameters) 声明一个基础投射物动作。
---@field modify_projectile fun(parameters: MatterFluxProjectileModifierParameters) 修改随后读取到的投射物法术。
---@field draw fun(count: integer, parameters?: MatterFluxDrawParameters) 同时读取指定数量的后续法术。
---@field trigger fun(parameters: MatterFluxTriggerParameters) 为投射物或后续法术声明触发器。
---@field impulse fun(parameters: MatterFluxImpulseParameters) 声明一次作用于施法者的冲量动作。

---@class MatterFluxSpellNamespace
---@field define fun(metadata: MatterFluxSpellMetadata, build_program: fun(api: MatterFluxSpellBuilder)) 注册一个法术；回调只在内容包加载时编译，不会在每帧执行。

---@class MatterFluxWandDefinition
---@field id MatterFluxContentId 法杖型号的稳定唯一 ID。
---@field name string UI 中显示的法杖名称。
---@field description? string 鼠标悬停提示中显示的法杖说明。
---@field icon? string 图标资源键。
---@field capacity? integer 法杖可容纳的法术槽数量。
---@field shuffle? boolean 每轮施法前是否随机打乱法术顺序。
---@field draw_count? integer 每次施法默认读取的法术数量。
---@field cast_delay? number 同一轮连续施法之间的基础间隔，单位为秒。
---@field recharge_time? number 一轮结束后的基础充能时间，单位为秒。
---@field mana_max? number 法杖的法力上限。
---@field mana_recharge? number 法杖每秒恢复的法力值。
---@field spread? number 法杖基础散布角，单位为度。
---@field starter_count? integer 新角色初始背包中赠送的该型号法杖数量。
---@field starter_slot? integer 初始自动装备的目标键位槽；负数表示不自动装备。
---@field starter_deck? MatterFluxContentId[] 初始装入法杖的法术 ID，按执行顺序排列。

---@class MatterFluxItemMetadata
---@field id MatterFluxContentId 物品类型的稳定唯一 ID。
---@field name string UI 中显示的物品名称。
---@field description? string 鼠标悬停提示中显示的物品说明。
---@field icon? string 图标资源键。
---@field category? MatterFluxItemCategory 物品分类；默认按普通材料处理。
---@field max_stack? integer 单个背包槽允许堆叠的最大数量。
---@field starter_count? integer 新角色初始背包中赠送的数量。

---@class MatterFluxItemBuilder
---@field restore_health fun(amount: number, consume_count?: integer) 使用后恢复生命；consume_count 是每次消耗数量。
---@field restore_wand_mana fun(amount: number, consume_count?: integer) 使用后恢复当前法杖法力。
---@field gameplay_event fun(tag: MatterFluxContentId, magnitude?: number, consume_count?: integer) 向权威 GAS 发送指定事件及数值。

---@class MatterFluxItemNamespace
---@field define fun(metadata: MatterFluxItemMetadata, build_behavior?: fun(api: MatterFluxItemBuilder)) 注册物品及其唯一使用行为；无回调时为不可主动使用物品。

---@class MatterFluxQuestMetadata
---@field id MatterFluxContentId 任务的稳定唯一 ID，存档进度通过它关联。
---@field name? string UI 中显示的任务名称；目标型子任务可以省略。
---@field description string 任务未完成时显示的说明。
---@field completed_description? string 任务完成后显示的说明。
---@field category MatterFluxQuestCategory 主线、支线或目标分类。
---@field sort? integer 同级任务的显示顺序，数值较小者优先。
---@field optional? boolean 是否为不阻塞父任务完成的可选目标。
---@field starter? boolean 是否在创建新角色时自动激活。
---@field focus_on_activate? boolean 激活后是否自动成为任务追踪焦点。
---@field prerequisites? MatterFluxContentId[] 自动激活前必须全部完成的前置任务 ID。
---@field children? MatterFluxContentId[] 该任务直接包含的子任务 ID。

---@class MatterFluxQuestObjectiveParameters
---@field target_id? MatterFluxContentId 目标物品、法术、法杖或生物 ID。
---@field target_count? integer 完成目标所需的累计数量。
---@field target_level? integer 击杀目标允许的最低生物等级。
---@field equipment_slot? integer 指定装备键位；负数或省略表示任意槽位。

---@class MatterFluxQuestBuilder
---@field complete_children fun() 所有必需子任务完成后完成当前任务。
---@field equip_wand fun(parameters?: MatterFluxQuestObjectiveParameters) 追踪装备指定法杖的行为。
---@field equip_spell fun(parameters?: MatterFluxQuestObjectiveParameters) 追踪把指定法术装入法杖的行为。
---@field kill_enemies fun(parameters?: MatterFluxQuestObjectiveParameters) 追踪击杀指定敌人或等级敌人的数量。
---@field spend_item fun(parameters?: MatterFluxQuestObjectiveParameters) 追踪消费指定物品的数量，例如商店付款。
---@field never fun() 当前任务不靠事件直接完成，通常作为纯容器父任务。
---@field spawn_creature fun(creature_id: MatterFluxContentId, marker_id: MatterFluxContentId) 任务激活时在当前地图的指定稳定 marker 生成一个生物。
---@field activation_reward fun(kind: MatterFluxRewardKind, id: MatterFluxContentId, quantity?: integer, equipment_slot?: integer) 任务激活时由服务器发放奖励。
---@field reward fun(kind: MatterFluxRewardKind, id: MatterFluxContentId, quantity?: integer, equipment_slot?: integer) 任务完成时由服务器发放奖励。

---@class MatterFluxQuestNamespace
---@field define fun(metadata: MatterFluxQuestMetadata, build_quest: fun(api: MatterFluxQuestBuilder)) 注册任务图节点、唯一目标和奖励。

---@class MatterFluxCreatureMetadata
---@field id MatterFluxContentId 生物类型的稳定唯一 ID。
---@field name string UI 和交互提示中显示的名称。
---@field faction MatterFluxCreatureFaction 生物与玩家及其他生物的阵营关系。
---@field level MatterFluxCreatureLevel 普通、精英或首领等级。
---@field health? number 最大生命值。
---@field width? number 碰撞和占位宽度，单位为 UE 单位。
---@field height? number 碰撞和占位高度，单位为 UE 单位。
---@field density? number 生物体积密度；低于所在液体材质密度时上浮，高于时下沉。
---@field wait_for_first_sight? boolean 为 true 时，在首次看到玩家前保持静止；首次看到后永久按行为树运行。
---@field color_r? number 基础显示颜色的红色分量，通常为 0～1。
---@field color_g? number 基础显示颜色的绿色分量，通常为 0～1。
---@field color_b? number 基础显示颜色的蓝色分量，通常为 0～1。
---@field color_a? number 基础显示颜色的不透明度，通常为 0～1。
---@field dialogue_id? MatterFluxContentId 与该生物交互时打开的对话 ID；它属于生物身份，不属于 AI 决策。
---@field shop_id? MatterFluxContentId 与该生物交互时直接打开的商店 ID；它属于生物身份，不属于 AI 决策。

---@class MatterFluxCreatureSight
---@field range number 服务器搜索可见目标的最大距离，单位为 UE 单位。
---@field memory_seconds? number 丢失视野后继续记忆目标的秒数。

---@class MatterFluxCreatureLocomotion
---@field speed number 这棵树的基础移动速度，单位为 UE 单位/秒。

---@class MatterFluxCreatureDistanceCondition
---@field distance number 该条件使用的距离阈值，单位为 UE 单位。

---@class MatterFluxCreaturePatrolAction
---@field turn_seconds? number 巡逻时改变方向的间隔。
---@field pause_seconds? number 巡逻转向时停顿的时长。

---@class MatterFluxCreatureCastAction
---@field cooldown? number 两次执行该动作之间的最短间隔，单位为秒。
---@field spell? MatterFluxContentId 该动作释放的法术 ID。
---@field projectiles? integer 一次动作发射的投射物数量。
---@field spread_degrees? number 投射物覆盖角度，单位为度。
---@field projectile_interval? number 连续投射物之间的间隔，单位为秒。
---@field recovery? number 发射序列结束后的恢复时间，单位为秒。
---@field radial? boolean 是否把投射物均匀分布到整圆。
---@field horizontal_impulse? number 释放时施加给自身的水平冲量。
---@field vertical_impulse? number 释放时施加给自身的垂直冲量。
---@field override_color? boolean 是否覆盖投射物颜色。
---@field color_r? number 覆盖颜色的红色分量，通常为 0～1。
---@field color_g? number 覆盖颜色的绿色分量，通常为 0～1。
---@field color_b? number 覆盖颜色的蓝色分量，通常为 0～1。
---@field color_a? number 覆盖颜色的不透明度，通常为 0～1。

---@class MatterFluxCreatureBehaviorNode
---@field private kind string 由 Builder 创建的节点类型；内容脚本不应直接修改。
---@field private name? string 条件或动作的白名单名称。
---@field private children? MatterFluxCreatureBehaviorNode[] 组合节点按作者顺序保存的子节点。

---@class MatterFluxCreatureBehaviorTree
---@field sight? MatterFluxCreatureSight 根部感知服务参数。
---@field locomotion? MatterFluxCreatureLocomotion 这棵树共享的基础移动能力。
---@field root MatterFluxCreatureBehaviorNode 唯一根节点。

---@class MatterFluxCreatureBuilder
---@field selector fun(children: MatterFluxCreatureBehaviorNode[]): MatterFluxCreatureBehaviorNode 按顺序尝试分支，选择第一个条件满足的动作子树。
---@field sequence fun(children: MatterFluxCreatureBehaviorNode[]): MatterFluxCreatureBehaviorNode 依次检查条件，最后一个子节点必须是动作子树。
---@field condition fun(name: MatterFluxCreatureBehaviorCondition, parameters?: MatterFluxCreatureDistanceCondition): MatterFluxCreatureBehaviorNode 创建只读条件；距离参数只允许用于对应距离条件。
---@field action fun(name: MatterFluxCreatureBehaviorAction, parameters?: MatterFluxCreaturePatrolAction|MatterFluxCreatureCastAction): MatterFluxCreatureBehaviorNode 创建服务器权威动作，动作参数与使用位置保持在一起。
---@field tree fun(definition: MatterFluxCreatureBehaviorTree) 提交完整行为树；树在内容包加载时编译，运行时不再调用 Lua。
---@field drop fun(item_id: MatterFluxContentId, count?: integer) 设置死亡时掉落的物品 ID 和数量。

---@class MatterFluxCreatureNamespace
---@field define fun(metadata: MatterFluxCreatureMetadata, build_ai: fun(api: MatterFluxCreatureBuilder)) 注册生物数据并把受限 AI 描述编译为 C++ 可解释程序。

---@class MatterFluxDialogueMetadata
---@field id MatterFluxContentId 对话图的稳定唯一 ID。
---@field name string 编辑器和调试界面中显示的对话名称。
---@field start MatterFluxContentId 开始对话时进入的首个节点 ID。

---@class MatterFluxDialogueOption
---@field text string 玩家看到的选项文本。
---@field next? MatterFluxContentId 选择后跳转到的节点 ID。
---@field shop_id? MatterFluxContentId 选择后打开的商店 ID。
---@field close? boolean 选择后是否立即关闭对话。

---@class MatterFluxDialogueNode
---@field id MatterFluxContentId 节点在当前对话图中的唯一 ID。
---@field text string NPC 在该节点显示的正文。
---@field next? MatterFluxContentId 无选项节点自动跳转到的下一节点 ID。
---@field shop_id? MatterFluxContentId 进入节点时打开的商店 ID。
---@field close? boolean 进入节点后是否结束对话。
---@field options? MatterFluxDialogueOption[] 玩家可以选择的分支；省略时使用 next 自动推进。

---@class MatterFluxDialogueBuilder
---@field node fun(node: MatterFluxDialogueNode) 向当前对话图添加一个节点。

---@class MatterFluxDialogueNamespace
---@field define fun(metadata: MatterFluxDialogueMetadata, build_dialogue: fun(api: MatterFluxDialogueBuilder)) 注册一个经过引用和可达性校验的对话图。

---@class MatterFluxShopMetadata
---@field id MatterFluxContentId 商店配置的稳定唯一 ID。
---@field name string UI 标题中显示的商店名称。

---@class MatterFluxShopCategory
---@field id MatterFluxContentId 页签在该商店内使用的稳定唯一 ID。
---@field name string 页签上显示的名称。

---@class MatterFluxShopOffer
---@field kind MatterFluxShopProductKind 商品是物品、法术卡还是法杖。
---@field product_id MatterFluxContentId 售出内容的 ID。
---@field product_count? integer 每次购买获得的数量。
---@field cost_item MatterFluxContentId 用作货币的物品 ID。
---@field cost_count? integer 每次购买消耗的货币数量。
---@field limit? integer 每名玩家可购买次数；负数表示不限次数。
---@field category? MatterFluxContentId 报价所属的自定义页签；省略时仅出现在“全部”。

---@class MatterFluxShopBuilder
---@field category fun(parameters: MatterFluxShopCategory) 追加一个有序的自定义分类页签。
---@field offer fun(parameters: MatterFluxShopOffer) 向商店商品列表中追加一个报价。

---@class MatterFluxShopNamespace
---@field define fun(metadata: MatterFluxShopMetadata, build_shop: fun(api: MatterFluxShopBuilder)) 注册商店及其固定报价列表。

---@class MatterFluxCustomMapMetadata
---@field id MatterFluxContentId 地图的稳定唯一 ID；游戏、测试和截图命令都通过它查询同一份布局。
---@field name string 调试界面中显示的地图名称。
---@field min_x integer 地图包含的最小 X 格坐标。
---@field min_y integer 地图包含的最小 Y 格坐标。
---@field max_x_exclusive integer 地图不包含的最大 X 格坐标；宽度最多 512 格。
---@field max_y_exclusive integer 地图不包含的最大 Y 格坐标；高度最多 512 格。
---@field cell_size_cm? number 一格对应的 UE 世界厘米数，范围 1～1000；默认 28。
---@field material_depth_cells? number 二维材质截面在三维场景中的挤出深度，单位为格；默认 3。

---@class MatterFluxCustomMapBuilder
---@field fill_rectangle fun(material_id: MatterFluxContentId, min_x: integer, min_y: integer, max_x: integer, max_y: integer) 用材质填充闭区间矩形；后声明的填充覆盖先声明的填充。
---@field fill_circle fun(material_id: MatterFluxContentId, center_x: integer, center_y: integer, radius: integer) 用材质填充圆形截面；半径范围为 1～128 格。
---@field marker fun(id: MatterFluxContentId, x: integer, y: integer) 添加稳定命名位置，供出生点、镜头和自动化断言查询。
---@field spawn_creature fun(creature_id: MatterFluxContentId, marker_id: MatterFluxContentId) 地图开始时在指定稳定 marker 生成一个常驻生物；任务波次应由 Quest Builder 声明。
---@field scene_box fun(id: MatterFluxContentId, material_id: MatterFluxContentId, center_x: number, center_y: number, center_z: number, size_x: number, size_y: number, size_z: number, collision?: boolean) 添加水平游戏场景中的静态盒；中心和尺寸均以地图格为单位，可选择启用碰撞。
---@field camera fun(id: MatterFluxContentId, location_x: number, location_y: number, location_z: number, target_x: number, target_y: number, target_z: number, field_of_view: number) 添加透视验收相机；位置和目标以地图格为单位。
---@field tilting_container fun(definition: MatterFluxTiltingContainerDefinition) 添加一个装满液体、按固定步数倾斜的三维容器；测试与游戏表现共用同一配置。

---@class MatterFluxTiltingContainerDefinition
---@field id MatterFluxContentId 容器稳定 ID。
---@field container_material MatterFluxContentId 容器壁材质 ID。
---@field liquid_material MatterFluxContentId 初始填满容器的液体材质 ID。
---@field center_x number 容器中心 X，单位为地图格。
---@field center_y number 容器中心 Y，单位为地图格。
---@field center_z number 容器中心 Z，单位为地图格。
---@field inner_width integer 内腔沿 X 的格数，范围 2～16。
---@field inner_depth integer 内腔沿 Y 的格数，范围 2～16。
---@field inner_height integer 内腔沿 Z 的格数，范围 2～16。
---@field start_step integer 开始倾斜的固定模拟步。
---@field tilt_steps integer 从水平到目标角度使用的固定模拟步数。
---@field tilt_degrees number 最终绕 Y 轴倾斜角度，范围 1～89 度。
---@field pour_cells_per_step integer 达到溢出角后每步释放的液体格数。

---@class MatterFluxCustomMapNamespace
---@field define fun(metadata: MatterFluxCustomMapMetadata, build_map: fun(api: MatterFluxCustomMapBuilder)) 注册有界确定性材料地图；回调只在内容加载时编译。

---@class MatterFluxStructureMetadata
---@field id MatterFluxContentId 结构模板稳定 ID。
---@field generator 'two_storey_house' 有界 C++ 几何生成能力；Lua 只选择能力，不在 Tick 中创建 Actor。

---@class MatterFluxStructureCutaway
---@field contact_tolerance_cm? number 当前 RuntimeMask 之间允许视为接触的小缝容差。
---@field floor_snap_height_cm? number 脚底吸附到真实 Floor Source 表面的最大高度差。
---@field preferred_floor_padding_cm? number 楼梯井和边缘滞回时扩张上一个 Floor Source 的水平范围。
---@field preferred_floor_vertical_range_cm? number 楼梯移动时保留上一个 Floor Source 的垂直范围。
---@field exit_grace_seconds? number 离开所有材料地板后恢复外立面的短暂防抖时间。
---@field fade_speed? number 墙体投影每秒透明度变化速度。
---@field wall_opacity? number 连通墙体的目标透明度。
---@field roof_opacity? number 连通屋顶墙体的目标透明度。

---@class MatterFluxStructureNamespace
---@field define fun(metadata: MatterFluxStructureMetadata, cutaway?: MatterFluxStructureCutaway) 注册结构生成能力和通用材质切面策略。

---@class MatterFluxContentApi
---@field set_manifest fun(pack_id: MatterFluxContentId, revision: integer, schema_version: integer) 设置内容包 ID、修订号和 C++ 接口版本；每个内容包只能调用一次。
---@field configure_fragmentation fun(min_detached_area_pixels: integer) 设置脱离 mask 后生成物理碎片所需的最小像素面积。
---@field register_material fun(definition: MatterFluxMaterialDefinition) 引擎内部材质编译接口；内容脚本应使用 material.define。旧位置参数仍兼容。
---@field register_reaction fun(definition: table) 引擎内部的扁平编译格式；内容脚本应使用 reaction.define。旧的 6 个位置参数接触反应仍兼容。
---@field register_decorator fun(id: MatterFluxContentId, generator_id: MatterFluxContentId, material_id: MatterFluxContentId, spawn_weight: number, min_count: integer, max_count: integer, enable_collision?: boolean) 注册地形装饰生成器参数；只有确实阻挡角色的装饰才应启用碰撞。
---@field register_entity fun(id: MatterFluxContentId, behavior: MatterFluxContentId, max_health: number, move_speed: number) 注册旧式轻量场景实体；新 NPC 和敌人优先使用 creature.define。
---@field register_wand fun(definition: MatterFluxWandDefinition) 注册法杖底盘、初始法术序列和新角色赠送配置。
---@field register_spell fun(definition: table) 引擎内部的扁平编译格式；内容脚本应使用 spell.define。
---@field register_item fun(definition: table) 引擎内部的扁平编译格式；内容脚本应使用 item.define。
---@field register_quest fun(definition: table) 引擎内部的扁平编译格式；内容脚本应使用 quest.define。
---@field register_creature fun(definition: table) 引擎内部的扁平编译格式；内容脚本应使用 creature.define。
---@field register_dialogue fun(definition: table) 引擎内部的扁平编译格式；内容脚本应使用 dialogue.define。
---@field register_shop fun(definition: table) 引擎内部的扁平编译格式；内容脚本应使用 shop.define。
---@field register_structure fun(definition: table) 引擎内部的结构编译格式；内容脚本应使用 structure.define。
---@field register_custom_map fun(definition: table) 引擎内部的地图编译格式；内容脚本应使用 map.define。

---@type MatterFluxContentApi
---@diagnostic disable-next-line: missing-fields
content = {}

---@type MatterFluxMaterialNamespace
---@diagnostic disable-next-line: missing-fields
material = {}

---@class MatterFluxReactionPropagation
---@field chance number 每个固定步向四邻域传播的概率，范围 0～1。

---@class MatterFluxReactionEmission
---@field material MatterFluxContentId 反应活跃时产生的物质。
---@field chance number 每个固定步产生该物质事件的概率，范围 0～1。

---@class MatterFluxReactionDefinition
---@field id MatterFluxContentId 稳定规则 ID。
---@field trigger 'contact'|'propagating' 接触立即变换，或在格子上持续并向邻域传播。
---@field inputs MatterFluxContentId[] 两个输入；传播反应中依次为反应物和激活物。
---@field outputs MatterFluxContentId[] 两个输出；可使用 empty 表示清空。
---@field chance? number 触发概率，范围 0～1，默认 1。
---@field duration_steps? integer 传播反应持续的固定步数。
---@field propagation? MatterFluxReactionPropagation 传播参数。
---@field emission? MatterFluxReactionEmission 活跃期间产生的副产物。

---@class MatterFluxReactionNamespace
---@field define fun(definition: MatterFluxReactionDefinition) 将易读规则编译为本地确定性反应数据。

---@type MatterFluxReactionNamespace
reaction = {}

---@type MatterFluxSpellNamespace
---@diagnostic disable-next-line: missing-fields
spell = {}

---@type MatterFluxItemNamespace
---@diagnostic disable-next-line: missing-fields
item = {}

---@type MatterFluxQuestNamespace
---@diagnostic disable-next-line: missing-fields
quest = {}

---@type MatterFluxCreatureNamespace
---@diagnostic disable-next-line: missing-fields
creature = {}

---@type MatterFluxDialogueNamespace
---@diagnostic disable-next-line: missing-fields
dialogue = {}

---@type MatterFluxShopNamespace
---@diagnostic disable-next-line: missing-fields
shop = {}

---@type MatterFluxStructureNamespace
---@diagnostic disable-next-line: missing-fields
structure = {}

---@type MatterFluxCustomMapNamespace
---@diagnostic disable-next-line: missing-fields
map = {}
