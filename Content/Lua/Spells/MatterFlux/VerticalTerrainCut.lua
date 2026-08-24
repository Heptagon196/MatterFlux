spell.define({
	id = "spell.vertical_terrain_cut", name = "纵向地形切割",
	description = "向前发射竖直切割面，命中后纵向切开地形和树木。",
	icon = "terrain_cut", mana_cost = 10, starter_count = 1
}, function(api)
	api.projectile({ damage = 12, speed = 1040, lifetime = 0.5, radius = 60, visual_shape = "vertical_plane", cast_delay = 0.10 })
end)
