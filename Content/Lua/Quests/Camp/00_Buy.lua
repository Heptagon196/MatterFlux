quest.define({
	id = "std.init_quest_buy",
	name = "进入营地",
	description = "进入营地，用击败敌人获得的金币购买任意商品。",
	completed_description = "已完成第一次交易。",
	category = "main",
	sort = 100,
	focus_on_activate = true,
	prerequisites = { "std.init_quest" },
}, function(q)
	q.spend_item({ target_id = "std.coin", target_count = 1 })
end)
