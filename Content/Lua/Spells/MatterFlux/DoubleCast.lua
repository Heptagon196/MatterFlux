spell.define({
	id = "spell.double_cast", name = "双重施法",
	description = "额外抽取两张法术卡。",
	icon = "double_cast", mana_cost = 5, starter_count = 2
}, function(api)
	api.draw(2, { cast_delay = 0.05 })
end)
