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
	reaction.define {
		id = target .. "_corrosion_acid",
		trigger = "propagating",
		inputs = { target, "acid" },
		outputs = { "empty", "acid" },
		chance = 1.0,
		duration_steps = 3,
		propagation = { chance = 0.78 },
		emission = { material = "acid_gas", chance = 0.86 },
	}
end

corrodible("acid_wood_corrosion", "wood")
corrodible("acid_leaf_corrosion", "leaf")
corrodible("acid_grass_corrosion", "grass")
corrodible("acid_grassland_corrosion", "grassland")
corrodible("acid_pink_flower_corrosion", "flower_pink")
corrodible("acid_gold_flower_corrosion", "flower_gold")
corrodible("acid_blue_flower_corrosion", "flower_blue")
corrodible("acid_soil_corrosion", "soil")
corrodible("acid_stone_corrosion", "stone")
corrodible("acid_sand_corrosion", "sand")

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

-- 单格燃烧时间应短于整棵树的传播时间：火沿相邻格推进形成火线，已经
-- 烧过的格及时熄灭，避免树烧完后还长期罩着一层红色火焰体素。
combustible("wood_burn", "wood", "charcoal", 0.72, 3, 0.68)
combustible("leaf_burn", "leaf", "ash", 1.0, 3, 0.82)
combustible("grass_burn", "grass", "ash", 0.18, 5, 0.76)
combustible("grassland_burn", "grassland", "ash", 0.025, 8, 0.42)
combustible("pink_flower_burn", "flower_pink", "ash", 0.88, 6, 0.72)
combustible("gold_flower_burn", "flower_gold", "ash", 0.88, 6, 0.72)
combustible("blue_flower_burn", "flower_blue", "ash", 0.88, 6, 0.72)
