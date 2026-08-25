quest.define({
	id = "std.init_quest.equip_spell",
	description = "把圆形子弹装入左键法杖，然后关闭背包准备战斗。",
	completed_description = "攻击法术已就绪；关闭背包后敌人出现。",
	category = "objective",
	sort = 20,
	prerequisites = { "std.init_quest.equip_wand" },
}, function(q)
	q.equip_spell({ target_id = "std.default", equipment_slot = 0 })
end)
