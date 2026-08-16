spell.define({
	id = "spell.accelerate", name = "加速",
	description = "让下一枚飞弹飞得更快，但存在时间更短。",
	icon = "accelerate", mana_cost = 4, starter_count = 3
}, function(api)
	api.modify_projectile({ speed_multiplier = 1.45, lifetime_multiplier = 0.80 })
	api.draw(1)
end)
