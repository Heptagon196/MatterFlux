spell.define({
	id = "spell.terrain_cut", name = "地形切割",
	description = "沿施法方向切开可破坏的地形和树木。",
	icon = "terrain_cut", mana_cost = 10, starter_count = 2
}, function(api)
	api.cut({ damage = 1200, range = 520, radius = 60, thickness = 30, cast_delay = 0.10 })
end)
