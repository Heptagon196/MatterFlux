spell.define({
	id = "spell.spark_trigger", name = "触发式闪光弹",
	description = "飞弹命中时再施放一张载荷法术。",
	icon = "spark_trigger", mana_cost = 18, starter_count = 2
}, function(api)
	api.projectile({ damage = 7, speed = 1050, lifetime = 2.2, radius = 15, cast_delay = 0.12 })
	api.trigger({ trigger_event = "impact", trigger_draw_count = 1 })
end)
