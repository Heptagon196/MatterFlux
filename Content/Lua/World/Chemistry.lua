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

-- 燃烧不再是规则状态。Default.lua 中的可燃材料通过接触导热达到点燃
-- 阈值，随后转化为炭/灰并排放普通 smoke 材料元素。
