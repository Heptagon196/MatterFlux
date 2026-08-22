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
    local propagation = definition.propagation or {}
    local emission = definition.emission or {}
    content.register_reaction({
        id = definition.id,
        kind = definition.trigger or "contact",
        input_a = inputs[1],
        input_b = inputs[2],
        output_a = outputs[1],
        output_b = outputs[2],
        chance_permille = permille(definition.chance, 1000),
        propagation_permille = permille(propagation.chance, 0),
        duration_steps = definition.duration_steps or 0,
        emission_material = emission.material or "empty",
        emission_permille = permille(emission.chance, 0),
    })
end
