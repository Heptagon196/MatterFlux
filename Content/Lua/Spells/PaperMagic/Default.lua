spell.define({
	id = "std.default", name = "圆形子弹",
	description = "生成一个圆形子弹。",
	icon = "paper/default", mana_cost = 10, starter_count = 4
}, function(api)
	api.projectile({ damage = 10, speed = 1500, lifetime = 3.0, radius = 14 })
end)
