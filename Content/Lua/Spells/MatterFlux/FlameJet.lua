spell.define({
	id = "spell.flame_jet", name = "火焰喷流",
	description = "向前喷出火焰，点燃可燃材质和地表。",
	icon = "flame_jet", mana_cost = 7, starter_count = 2
}, function(api)
	api.flame({ range = 800, radius = 45, end_radius = 180, impact_material = "fire", cast_delay = 0.08 })
end)
