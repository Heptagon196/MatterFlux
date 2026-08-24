spell.define({
	id = "spell.acid_spray", name = "酸液喷射",
	description = "喷出受重力影响的酸液；命中后腐蚀物品与地形并释放酸雾。",
	icon = "acid_spray", mana_cost = 8, starter_count = 2
}, function(api)
	api.projectile({ damage = 0, speed = 1150, lifetime = 0.90, radius = 16, gravity_scale = 0.45, body_material = "acid", material_amount = 5, cast_delay = 0.03 })
end)
