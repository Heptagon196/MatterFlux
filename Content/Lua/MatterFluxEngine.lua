-- MatterFlux engine-level simulation settings.
-- Keep global mechanics here; content definitions belong in
-- MatterFluxContent.lua.

-- Detached mask components smaller than this cell count are discarded instead
-- of becoming replicated physics actors. A source may request a larger
-- threshold, but never a smaller one.
content.configure_fragmentation(4)

local register_material_compiled = content.register_material
content.register_material = nil

-- Public spell-authoring API. Content scripts describe behavior by calling a
-- small set of engine capabilities. The raw flat registration table is kept
-- private because it is only the C++ compiler's wire format.
local register_spell_compiled = content.register_spell
content.register_spell = nil

spell = {}

local function copy_fields(target, source, allowed, context)
	if source == nil then
		return
	end
	if type(source) ~= "table" then
		error("spell capability arguments must be tables", 3)
	end
	local allowed_lookup = {}
	for _, key in ipairs(allowed) do
		allowed_lookup[key] = true
	end
	for key, value in pairs(source) do
		if not allowed_lookup[key] then
			error("unknown " .. context .. " field '" .. tostring(key) .. "'", 3)
		end
		target[key] = value
	end
end

-- 材质作者使用命名字段；C++ 仍只接收加载时编译后的不可变表。
-- 光学字段对所有材质都合法，但只有 liquid 阶段会走透明材质。
material = {}

function material.define(definition)
	if type(definition) ~= "table" then
		error("material.define expects a definition table", 2)
	end
	local compiled = {}
	copy_fields(compiled, definition, {
		"id", "density", "hardness",
		"color_r", "color_g", "color_b", "color_a",
		"phase", "mobility", "dispersion", "lifetime_steps",
		"shallow_opacity", "deep_opacity", "opacity_depth"
	}, "material")
	register_material_compiled(compiled)
end

-- 结构脚本选择一个有界 C++ 生成能力，并声明材质切面的策略。
-- 运行时不会在 Actor Tick 中执行 Lua；这里的数据在加载时编译、校验。
local register_structure_compiled = content.register_structure
content.register_structure = nil
structure = {}

function structure.define(metadata, cutaway)
	if type(metadata) ~= "table" then
		error("structure.define expects a metadata table", 2)
	end
	local compiled = {}
	copy_fields(compiled, metadata, { "id", "generator" }, "structure metadata")
	copy_fields(compiled, cutaway, {
		"contact_tolerance_cm", "floor_snap_height_cm",
		"preferred_floor_padding_cm",
		"preferred_floor_vertical_range_cm", "exit_grace_seconds",
		"fade_speed", "wall_opacity", "roof_opacity"
	}, "structure cutaway")
	register_structure_compiled(compiled)
end

-- 自定义地图只描述有界的确定性填充操作。Lua 不直接创建 Actor，
-- 游戏、自动化测试和截图工具都通过同一份编译结果构建材料世界。
local register_custom_map_compiled = content.register_custom_map
content.register_custom_map = nil
map = {}

function map.define(metadata, build_map)
	if type(metadata) ~= "table" then
		error("map.define expects a metadata table", 2)
	end
	if type(build_map) ~= "function" then
		error("map.define expects a builder function", 2)
	end
	local compiled = {
		stamps = {}, markers = {}, scene_boxes = {}, cameras = {},
		pour_containers = {},
	}
	copy_fields(compiled, metadata, {
		"id", "name", "min_x", "min_y",
		"max_x_exclusive", "max_y_exclusive",
		"cell_size_cm", "material_depth_cells"
	}, "custom map metadata")
	local api = {}
	function api.fill_rectangle(material_id, min_x, min_y, max_x, max_y)
		compiled.stamps[#compiled.stamps + 1] = {
			shape = "rectangle", material = material_id,
			min_x = min_x, min_y = min_y, max_x = max_x, max_y = max_y,
		}
	end
	function api.fill_circle(material_id, center_x, center_y, radius)
		compiled.stamps[#compiled.stamps + 1] = {
			shape = "circle", material = material_id,
			center_x = center_x, center_y = center_y, radius = radius,
		}
	end
	function api.marker(id, x, y)
		compiled.markers[#compiled.markers + 1] = { id = id, x = x, y = y }
	end
	function api.scene_box(id, material_id, center_x, center_y, center_z,
			size_x, size_y, size_z, collision)
		compiled.scene_boxes[#compiled.scene_boxes + 1] = {
			id = id, material = material_id,
			center_x = center_x, center_y = center_y, center_z = center_z,
			size_x = size_x, size_y = size_y, size_z = size_z,
			collision = collision == true,
		}
	end
	function api.camera(id, location_x, location_y, location_z,
			target_x, target_y, target_z, field_of_view)
		compiled.cameras[#compiled.cameras + 1] = {
			id = id,
			location_x = location_x, location_y = location_y,
			location_z = location_z,
			target_x = target_x, target_y = target_y, target_z = target_z,
			field_of_view = field_of_view,
		}
	end
	function api.tilting_container(definition)
		local compiled_container = {}
		copy_fields(compiled_container, definition, {
			"id", "container_material", "liquid_material",
			"center_x", "center_y", "center_z",
			"inner_width", "inner_depth", "inner_height",
			"start_step", "tilt_steps", "tilt_degrees",
			"pour_cells_per_step",
		}, "tilting container")
		compiled.pour_containers[#compiled.pour_containers + 1] =
			compiled_container
	end
	build_map(api)
	register_custom_map_compiled(compiled)
end

local projectile_fields = {
	"damage", "speed", "lifetime", "radius", "body_material",
	"material_amount", "visual_shape", "gravity_scale",
	"cast_delay", "recharge_time"
}
local modifier_fields = {
	"damage_add", "damage_multiplier", "speed_multiplier",
	"lifetime_multiplier", "spread", "override_color", "color_r",
	"color_g", "color_b", "color_a", "orbit_radius", "cast_delay",
	"recharge_time"
}

function spell.define(metadata, build_program)
	if type(metadata) ~= "table" then
		error("spell.define expects a metadata table", 2)
	end
	if type(build_program) ~= "function" then
		error("spell.define expects a program function", 2)
	end

	local compiled = {}
	copy_fields(compiled, metadata, {
		"id", "name", "description", "icon", "mana_cost", "starter_count"
	}, "spell metadata")

	local has_projectile = false
	local has_modifier = false
	local has_trigger = false
	local terminal_kind = nil
	local explicit_draw = false
	local api = {}

	function api.projectile(parameters)
		if has_projectile or terminal_kind ~= nil then
			error("a spell may declare only one primary action", 2)
		end
		has_projectile = true
		copy_fields(compiled, parameters, projectile_fields, "projectile")
	end

	function api.modify_projectile(parameters)
		if has_modifier then
			error("a spell may declare only one projectile modifier", 2)
		end
		has_modifier = true
		copy_fields(compiled, parameters, modifier_fields, "modifier")
	end

	function api.draw(count, parameters)
		if explicit_draw then
			error("a spell may declare only one draw operation", 2)
		end
		if type(count) ~= "number" then
			error("api.draw expects a numeric card count", 2)
		end
		explicit_draw = true
		compiled.draw_count = count
		copy_fields(compiled, parameters, {
			"spread", "cast_delay", "recharge_time"
		}, "draw")
	end

	function api.trigger(parameters)
		if has_trigger then
			error("a spell may declare only one trigger", 2)
		end
		has_trigger = true
		copy_fields(compiled, parameters, {
			"trigger_event", "trigger_draw_count",
			"trigger_random_direction", "carrier_lifetime"
		}, "trigger")
	end

	function api.impulse(parameters)
		if has_projectile or terminal_kind ~= nil then
			error("a spell may declare only one primary action", 2)
		end
		terminal_kind = "jump"
		copy_fields(compiled, parameters, {
			"vertical_impulse", "cast_delay", "recharge_time"
		}, "impulse")
	end

	build_program(api)

	if terminal_kind ~= nil then
		if has_trigger or has_modifier or explicit_draw then
			error("avatar actions cannot own child operations yet", 2)
		end
		compiled.kind = terminal_kind
	elseif has_projectile then
		if has_modifier or explicit_draw then
			error("projectile spells cannot also modify or directly draw cards", 2)
		end
		compiled.kind = has_trigger and "trigger" or "projectile"
		if has_trigger and compiled.trigger_draw_count == nil then
			compiled.trigger_draw_count = 1
		end
	elseif has_trigger then
		if has_modifier then
			error("trigger modifiers cannot also modify projectile fields", 2)
		end
		compiled.kind = "trigger_modifier"
		compiled.draw_count = compiled.draw_count or 2
	elseif has_modifier then
		compiled.kind = "modifier"
		compiled.draw_count = compiled.draw_count or 1
	elseif explicit_draw and compiled.draw_count >= 2 then
		compiled.kind = "multicast"
	else
		error("spell program does not contain an executable capability", 2)
	end

	register_spell_compiled(compiled)
end

-- Item authoring keeps behavior declarative. C++ validates and executes the
-- compiled capability on the authoritative player state.
local register_item_compiled = content.register_item
content.register_item = nil
item = {}

function item.define(metadata, build_behavior)
	local compiled = {}
	copy_fields(compiled, metadata, {
		"id", "name", "description", "icon", "category",
		"max_stack", "starter_count"
	}, "item metadata")
	local behavior_defined = false
	local api = {}
	local function set_behavior(action, magnitude, consume_count, gameplay_event)
		if behavior_defined then
			error("an item may declare only one use behavior", 3)
		end
		behavior_defined = true
		compiled.use_action = action
		compiled.use_magnitude = magnitude or 0
		compiled.consume_count = consume_count or 1
		compiled.gameplay_event = gameplay_event
	end
	function api.restore_health(amount, consume_count)
		set_behavior("restore_health", amount, consume_count)
	end
	function api.restore_wand_mana(amount, consume_count)
		set_behavior("restore_wand_mana", amount, consume_count)
	end
	function api.gameplay_event(tag, magnitude, consume_count)
		set_behavior("gameplay_event", magnitude, consume_count, tag)
	end
	if build_behavior ~= nil then
		if type(build_behavior) ~= "function" then
			error("item.define behavior must be a function", 2)
		end
		build_behavior(api)
	end
	compiled.use_action = compiled.use_action or "none"
	compiled.consume_count = compiled.consume_count or 0
	register_item_compiled(compiled)
end

-- Quest scripts describe a graph and capabilities. They cannot directly
-- mutate players, inventories, actors, or save data.
local register_quest_compiled = content.register_quest
content.register_quest = nil
quest = {}

function quest.define(metadata, build_quest)
	if type(build_quest) ~= "function" then
		error("quest.define expects a builder function", 2)
	end
	local compiled = {}
	copy_fields(compiled, metadata, {
		"id", "name", "description", "completed_description",
		"category", "sort", "optional", "starter",
		"focus_on_activate", "prerequisites", "children"
	}, "quest metadata")
	compiled.activation_rewards = {}
	compiled.completion_rewards = {}
	local objective_defined = false
	local api = {}
	local function set_objective(kind, parameters)
		if objective_defined then
			error("a quest may declare only one objective", 3)
		end
		objective_defined = true
		compiled.objective = kind
		copy_fields(compiled, parameters, {
			"target_id", "target_count", "target_level", "equipment_slot"
		}, "quest objective")
	end
	function api.complete_children()
		set_objective("complete_children")
	end
	function api.equip_wand(parameters)
		set_objective("equip_wand", parameters)
	end
	function api.equip_spell(parameters)
		set_objective("equip_spell", parameters)
	end
	function api.kill_enemies(parameters)
		set_objective("kill_enemies", parameters)
	end
	function api.spend_item(parameters)
		set_objective("spend_item", parameters)
	end
	function api.never()
		set_objective("never")
	end
	local function add_reward(target, kind, id, quantity, equipment_slot)
		target[#target + 1] = {
			kind = kind,
			id = id,
			quantity = quantity or 1,
			equipment_slot = equipment_slot or -1,
		}
	end
	function api.activation_reward(kind, id, quantity, equipment_slot)
		add_reward(compiled.activation_rewards, kind, id, quantity, equipment_slot)
	end
	function api.reward(kind, id, quantity, equipment_slot)
		add_reward(compiled.completion_rewards, kind, id, quantity, equipment_slot)
	end
	build_quest(api)
	if not objective_defined then
		error("quest program does not contain an objective", 2)
	end
	register_quest_compiled(compiled)
end

-- 生物脚本只负责“声明”决策树：加载时会编译成不可变数据，游戏运行时由
-- 服务器上的 C++ 解释器按固定频率执行，不会在每个 Actor Tick 中调用 Lua。
local register_creature_compiled = content.register_creature
content.register_creature = nil
creature = {}

function creature.define(metadata, build_ai)
	if type(metadata) ~= "table" or type(build_ai) ~= "function" then
		error("creature.define expects metadata and an AI builder", 2)
	end
	local compiled = {}
	copy_fields(compiled, metadata, {
		"id", "name", "faction", "level", "health", "width", "height", "density",
		"color_r", "color_g", "color_b", "color_a", "dialogue_id", "shop_id"
	}, "creature metadata")
	local behavior_defined = false -- 是否已经提交唯一的行为树。
	local configured_actions = {} -- 当前扁平运行时格式不允许同类动作使用两套参数。
	local api = {}
	local function copy_mapped(source, mapping, context)
		if source == nil then return end
		if type(source) ~= "table" then
			error(context .. " parameters must be a table", 4)
		end
		for key, value in pairs(source) do
			local target = mapping[key]
			if target == nil then
				error("unknown " .. context .. " field '" .. tostring(key) .. "'", 4)
			end
			if compiled[target] ~= nil and compiled[target] ~= value then
				error(context .. " conflicts with another behavior-tree node", 4)
			end
			compiled[target] = value
		end
	end
	-- selector/sequence 都是组合节点，children 的排列顺序就是运行时优先级。
	local function composite(kind, children)
		if type(children) ~= "table" or #children == 0 then
			error("behavior-tree composites require a non-empty child array", 3)
		end
		for index, child in ipairs(children) do
			if type(child) ~= "table" or child.kind == nil then
				error("behavior-tree child " .. tostring(index) .. " is not a node", 3)
			end
		end
		return { kind = kind, children = children }
	end
	-- 条件和动作只能使用 C++ 注册过的白名单名称，不能保存 Lua 回调。
	local function leaf(kind, name, parameters)
		if type(name) ~= "string" or name == "" then
			error("behavior-tree leaves require a non-empty name", 3)
		end
		if kind == "condition" then
			if name == "target_too_close" then
				copy_mapped(parameters, { distance = "retreat_range" }, name)
			elseif name == "target_in_attack_range" then
				copy_mapped(parameters, { distance = "attack_range" }, name)
			elseif parameters ~= nil then
				copy_mapped(parameters, {}, name)
			end
		elseif parameters ~= nil then
			if configured_actions[name] then
				error("behavior-tree action '" .. name .. "' is configured more than once", 3)
			end
			configured_actions[name] = true
			if name == "patrol" then
				copy_mapped(parameters, {
					turn_seconds = "patrol_turn", pause_seconds = "patrol_pause"
				}, name)
			elseif name == "attack" or name == "skill" then
				local prefix = name .. "_"
				copy_mapped(parameters, {
					cooldown = prefix .. "cooldown", spell = prefix .. "spell",
					projectiles = prefix .. "projectiles", spread_degrees = prefix .. "spread",
					projectile_interval = prefix .. "projectile_interval",
					recovery = prefix .. "recovery", radial = prefix .. "radial",
					horizontal_impulse = prefix .. "horizontal_impulse",
					vertical_impulse = prefix .. "vertical_impulse",
					color_r = prefix .. "color_r", color_g = prefix .. "color_g",
					color_b = prefix .. "color_b", color_a = prefix .. "color_a",
					override_color = prefix .. "override_color"
				}, name)
			else
				copy_mapped(parameters, {}, name)
			end
		end
		return { kind = kind, name = name }
	end
	function api.selector(children) return composite("selector", children) end
	function api.sequence(children) return composite("sequence", children) end
	function api.condition(name, parameters) return leaf("condition", name, parameters) end
	function api.action(name, parameters) return leaf("action", name, parameters) end
	function api.tree(definition)
		if behavior_defined then
			error("a creature may define only one AI behavior", 2)
		end
		if type(definition) ~= "table" then
			error("ai.tree expects a behavior-tree definition", 2)
		end
		copy_fields({}, definition, { "sight", "locomotion", "root" }, "behavior tree")
		copy_mapped(definition.sight, {
			range = "perception_range", memory_seconds = "target_memory"
		}, "behavior-tree sight")
		copy_mapped(definition.locomotion, {
			speed = "move_speed"
		}, "behavior-tree locomotion")
		local root = definition.root
		if type(root) ~= "table" or root.kind == nil then
			error("ai.tree requires one root behavior node", 2)
		end
		behavior_defined = true
		compiled.ai = "behavior_tree"
		compiled.behavior_tree = root
	end
	function api.drop(item_id, count)
		compiled.drop_item = item_id
		compiled.drop_count = count
	end
	function api.spawn_on_quest(quest_id, count, distance)
		compiled.spawn_quest = quest_id
		compiled.spawn_count = count
		compiled.spawn_distance = distance
	end
	build_ai(api)
	if not behavior_defined then
		error("creature program does not contain an AI behavior", 2)
	end
	register_creature_compiled(compiled)
end

local register_dialogue_compiled = content.register_dialogue
content.register_dialogue = nil
dialogue = {}

function dialogue.define(metadata, build_dialogue)
	if type(metadata) ~= "table" or type(build_dialogue) ~= "function" then
		error("dialogue.define expects metadata and a builder", 2)
	end
	local compiled = { nodes = {} }
	copy_fields(compiled, metadata, { "id", "name", "start" }, "dialogue metadata")
	local api = {}
	function api.node(node)
		if type(node) ~= "table" then
			error("dialogue.node expects a table", 2)
		end
		local copy = {}
		copy_fields(copy, node, {
			"id", "text", "next", "shop_id", "close", "options"
		}, "dialogue node")
		compiled.nodes[#compiled.nodes + 1] = copy
	end
	build_dialogue(api)
	register_dialogue_compiled(compiled)
end

local register_shop_compiled = content.register_shop
content.register_shop = nil
shop = {}

function shop.define(metadata, build_shop)
	if type(metadata) ~= "table" or type(build_shop) ~= "function" then
		error("shop.define expects metadata and a builder", 2)
	end
	local compiled = { offers = {} }
	copy_fields(compiled, metadata, { "id", "name" }, "shop metadata")
	local api = {}
	function api.offer(parameters)
		local offer = {}
		copy_fields(offer, parameters, {
			"kind", "product_id", "product_count",
			"cost_item", "cost_count", "limit"
		}, "shop offer")
		compiled.offers[#compiled.offers + 1] = offer
	end
	build_shop(api)
	register_shop_compiled(compiled)
end
