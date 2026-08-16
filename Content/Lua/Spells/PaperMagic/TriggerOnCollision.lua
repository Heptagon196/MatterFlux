spell.define({
	id = "std.trigger_on_collision", name = "碰撞触发",
	description = "第一个子节点命中时，向确定性的随机方向释放第二个子节点。",
	icon = "paper/trigger_collision", mana_cost = 10, starter_count = 3
}, function(api)
	api.draw(2)
	api.trigger({ trigger_event = "impact", trigger_random_direction = true, carrier_lifetime = 1 })
end)
