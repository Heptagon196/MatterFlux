spell.define({
	id = "std.add_damage", name = "增加伤害",
	description = "为子节点法术增加 7 点伤害。",
	icon = "paper/add_damage", mana_cost = 10, starter_count = 3
}, function(api)
	api.modify_projectile({ damage_add = 7 })
	api.draw(1)
end)
