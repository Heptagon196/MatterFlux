spell.define({
	id = "std.set_color_red", name = "红色",
	description = "将子节点投射物染成红色。",
	icon = "paper/set_color_red", mana_cost = 10, starter_count = 3
}, function(api)
	api.modify_projectile({
		override_color = true, color_r = 1, color_g = 0, color_b = 0, color_a = 1
	})
	api.draw(1)
end)
