spell.define({
	id = "std.trigger_on_expired", name = "消逝触发",
	description = "第一个子节点消逝时，沿当前方向释放第二个子节点。",
	icon = "paper/trigger_expire", mana_cost = 10, starter_count = 3
}, function(api)
	api.draw(2)
	api.trigger({ trigger_event = "expired", carrier_lifetime = 1 })
end)
