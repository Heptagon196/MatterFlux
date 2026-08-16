spell.define({
	id = "spell.spark_bolt", name = "闪光弹",
	description = "快速而稳定的凝光飞弹。",
	icon = "spark_bolt", mana_cost = 8, starter_count = 8
}, function(api)
	api.projectile({ damage = 12, speed = 1250, lifetime = 2.0, radius = 13, cast_delay = 0.04 })
end)
