spell.define({
	id = "std.circle_trail", name = "圆形轨迹",
	description = "让子节点投射物沿圆形轨迹飞行，并将持续时间延长一倍。",
	icon = "paper/circle_trail", mana_cost = 10, starter_count = 3
}, function(api)
	api.modify_projectile({ lifetime_multiplier = 2.0, orbit_radius = 300 })
	api.draw(1)
end)
