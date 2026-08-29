-- 可热更的物质反应 DSL。这里仅把易读配置编译成 C++ 的固定数据；
-- 游戏运行期间不会逐格回调 Lua，以保证性能、存档和联机确定性。
reaction = reaction or {}

local function require_table(value, field)
    if type(value) ~= "table" then
        error("reaction.define requires table field '" .. field .. "'")
    end
    return value
end

local function permille(value, default_value)
    if value == nil then
        return default_value
    end
    return math.floor(value * 1000 + 0.5)
end

function reaction.define(definition)
    local inputs = require_table(definition.inputs, "inputs")
    local outputs = require_table(definition.outputs, "outputs")
    if definition.trigger ~= nil and definition.trigger ~= "contact" then
        error("schema 3 removed propagating reactions; use local contact rules and material thermal fields")
    end
    if definition.propagation ~= nil or definition.duration_steps ~= nil
            or definition.emission ~= nil then
        error("schema 3 removed propagation, duration_steps, and singular emission fields")
    end
    local emissions = definition.emissions or {}
    if type(emissions) ~= "table" or #emissions > 2 then
        error("reaction.define emissions must be a table with at most two results")
    end
    local first = emissions[1] or {}
    local second = emissions[2] or {}
    content.register_reaction({
        id = definition.id,
        kind = "contact",
        input_a = inputs[1],
        input_b = inputs[2],
        output_a = outputs[1],
        output_b = outputs[2],
        chance_permille = permille(definition.chance, 1000),
        energy_delta_a = definition.energy_delta_a or 0,
        energy_delta_b = definition.energy_delta_b or 0,
        emission_1_material = first.material,
        emission_1_amount = first.amount,
        emission_1_energy = first.energy,
        emission_1_source = first.source,
        emission_2_material = second.material,
        emission_2_amount = second.amount,
        emission_2_energy = second.energy,
        emission_2_source = second.source,
    })
end
