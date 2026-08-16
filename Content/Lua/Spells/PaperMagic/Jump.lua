spell.define({
	id = "std.jump", name = "跳跃",
	description = "为施法者施加向上的冲量。",
	icon = "paper/jump", mana_cost = 50, starter_count = 2
}, function(api)
	api.impulse({ vertical_impulse = 600 })
end)
