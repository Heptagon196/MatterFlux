spell.define({
	id = "spell.flame_jet", name = "火焰喷流",
	description = "发射一团无重力的火焰，命中后点燃材质和地表。",
	icon = "flame_jet", mana_cost = 7, starter_count = 2
}, function(api)
	api.projectile({ damage = 0, speed = 940, lifetime = 0.85, radius = 45, body_material = "fire", impact_material = "fire", cast_delay = 0.08 })
end)
