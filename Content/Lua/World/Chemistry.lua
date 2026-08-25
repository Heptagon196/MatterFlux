reaction.define {
    id = "water_lava_quench",
    trigger = "contact",
    inputs = { "water", "lava" },
    outputs = { "steam", "stone" },
    chance = 1.0,
}

reaction.define {
    id = "fire_water_extinguish",
    trigger = "contact",
    inputs = { "fire", "water" },
    outputs = { "empty", "water" },
    chance = 1.0,
}

-- 腐蚀是守恒的一次接触反应，不是像燃烧一样的自传播过程。一个酸液格在
-- 溶解一个固体格后被消耗，不产生新的酸性材料格；反应不会复制酸，也不会
-- 沿 Source mask 自行蔓延。这里故意不声明 acid+water，二者只按密度分层。
local function corrodible(id, target)
    reaction.define {
        id = id,
        trigger = "contact",
        inputs = { "acid", target },
        outputs = { "empty", "empty" },
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
