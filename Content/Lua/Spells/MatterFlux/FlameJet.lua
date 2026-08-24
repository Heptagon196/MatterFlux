spell.define({
	id = "spell.flame_jet", name = "火焰喷流",
	description = "发射一团无重力火焰；火焰进入物质世界后会自然引燃可燃物。",
	icon = "flame_jet", mana_cost = 7, starter_count = 2
}, function(api)
	api.projectile({ damage = 0, speed = 940, lifetime = 0.85, radius = 45, body_material = "fire", material_amount = 5, cast_delay = 0.08 })
end)
