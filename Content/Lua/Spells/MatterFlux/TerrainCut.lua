spell.define({
	id = "spell.terrain_cut", name = "地形切割",
	description = "发射一枚短寿命切割弹，命中后切开地形和树木。",
	icon = "terrain_cut", mana_cost = 10, starter_count = 2
}, function(api)
	api.projectile({ damage = 12, speed = 1040, lifetime = 0.5, radius = 60, cast_delay = 0.10 })
end)
