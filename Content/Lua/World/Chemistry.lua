reaction.define {
    id = "water_lava_quench",
    trigger = "contact",
    inputs = { "water", "lava" },
    outputs = { "steam", "stone" },
    chance = 1.0,
}

-- 接触腐蚀与燃烧共用同一个反应 DSL。酸本身保留，被腐蚀格转成会扩散的酸雾；
-- 这里故意不声明 acid+water，二者只按密度分层，不发生化学反应。
local function corrodible(id, target)
    reaction.define {
        id = id,
        trigger = "contact",
        inputs = { "acid", target },
        outputs = { "acid", "acid_gas" },
        chance = 1.0,
    }
end

corrodible("acid_wood_corrosion", "wood")
corrodible("acid_leaf_corrosion", "leaf")
corrodible("acid_grass_corrosion", "grass")
corrodible("acid_grassland_corrosion", "grassland")
corrodible("acid_pink_flower_corrosion", "flower_pink")
corrodible("acid_gold_flower_corrosion", "flower_gold")
corrodible("acid_blue_flower_corrosion", "flower_blue")

local function combustible(id, fuel, residue, spread, duration, smoke)
    reaction.define {
        id = id,
        trigger = "propagating",
        inputs = { fuel, "fire" },
        outputs = { residue, "fire" },
        chance = 1.0,
        duration_steps = duration,
        propagation = { chance = spread },
        emission = { material = "smoke", chance = smoke },
    }
end

combustible("wood_burn", "wood", "charcoal", 0.72, 18, 0.68)
combustible("leaf_burn", "leaf", "ash", 1.0, 7, 0.82)
combustible("grass_burn", "grass", "ash", 0.18, 5, 0.76)
combustible("grassland_burn", "grassland", "ash", 0.025, 8, 0.42)
combustible("pink_flower_burn", "flower_pink", "ash", 0.88, 6, 0.72)
combustible("gold_flower_burn", "flower_gold", "ash", 0.88, 6, 0.72)
combustible("blue_flower_burn", "flower_blue", "ash", 0.88, 6, 0.72)
