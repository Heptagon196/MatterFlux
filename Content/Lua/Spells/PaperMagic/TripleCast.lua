spell.define({
	id = "std.triple_cast", name = "三重释放",
	description = "释放三个子节点法术，并增加散射。",
	icon = "paper/triple_cast", mana_cost = 15, starter_count = 3
}, function(api)
	api.draw(3, { spread = 10 })
end)
