-- MatterFlux engine-level simulation settings.
-- Keep global mechanics here; content definitions belong in
-- MatterFluxContent.lua.

-- Detached mask components smaller than this cell count are discarded instead
-- of becoming replicated physics actors. A source may request a larger
-- threshold, but never a smaller one.
content.configure_fragmentation(4)

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

local projectile_fields = {
	"damage", "speed", "lifetime", "radius", "impact_material",
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

	function api.cut(parameters)
		if has_projectile or terminal_kind ~= nil then
			error("a spell may declare only one primary action", 2)
		end
		terminal_kind = "cut"
		copy_fields(compiled, parameters, {
			"damage", "range", "radius", "thickness", "cast_delay",
			"recharge_time"
		}, "cut")
	end

	function api.flame(parameters)
		if has_projectile or terminal_kind ~= nil then
			error("a spell may declare only one primary action", 2)
		end
		terminal_kind = "flame"
		copy_fields(compiled, parameters, {
			"range", "radius", "end_radius", "impact_material",
			"cast_delay", "recharge_time"
		}, "flame")
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
			error("world and avatar actions cannot own child operations yet", 2)
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
