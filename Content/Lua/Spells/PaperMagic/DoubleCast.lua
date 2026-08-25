spell.define({
	id = "std.double_cast", name = "散射",
	description = "释放两个子节点法术，并增加散射。",
	icon = "paper/double_cast", mana_cost = 10, starter_count = 3
}, function(api)
	api.draw(2, { spread = 10 })
end)
