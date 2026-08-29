spell.define({
	id = "spell.add_damage", name = "伤害增幅",
	description = "提高子树中所有投射物的伤害与冲击强度。",
	icon = "add_damage", mana_cost = 6, starter_count = 3
}, function(api)
	api.modify_projectile({ damage_add = 10, cast_delay = 0.02 })
	api.draw(1)
end)
