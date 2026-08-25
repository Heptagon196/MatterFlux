spell.define({
	id = "spell.water_spray", name = "喷水术",
	description = "向前连续喷出真实水粒子；粒子受重力与接触作用并自然汇流。",
	icon = "water_spray", mana_cost = 5, starter_count = 4
}, function(api)
	api.projectile({ damage = 0, speed = 1250, lifetime = 0.90, radius = 16, gravity_scale = 0.85, body_material = "water", material_amount = 5, cast_delay = 0.02 })
end)
