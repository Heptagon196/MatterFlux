spell.define({
	id = "spell.sand_spray", name = "喷沙术",
	description = "向前连续喷出短程沙团；沙团命中后进入材质世界并堆积、滑落。",
	icon = "sand_spray", mana_cost = 6, starter_count = 4
}, function(api)
	api.projectile({ damage = 0, speed = 1100, lifetime = 0.72, radius = 18, gravity_scale = 1.0, body_material = "sand", material_amount = 5, cast_delay = 0.03 })
end)
