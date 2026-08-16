spell.define({
	id = "spell.add_damage", name = "伤害增幅",
	description = "提高本次施法中下一个动作的伤害或切割强度。",
	icon = "add_damage", mana_cost = 6, starter_count = 3
}, function(api)
	api.modify_projectile({ damage_add = 10, cast_delay = 0.02 })
	api.draw(1)
end)
