spell.define({
	id = "spell.heavy_orb", name = "重力法球",
	description = "缓慢、昂贵，但破坏力很强。",
	icon = "heavy_orb", mana_cost = 28, starter_count = 2
}, function(api)
	api.projectile({ damage = 34, speed = 620, lifetime = 3.2, radius = 34, cast_delay = 0.20, recharge_time = 0.12 })
end)
