quest.define({
	id = "std.init_quest.kill_enemy",
	description = "使用法术击败敌人。",
	completed_description = "已击败敌人并获得新的跳跃法术。",
	category = "objective",
	sort = 30,
	prerequisites = { "std.init_quest.equip_spell" },
}, function(q)
	q.kill_enemies({ target_count = 3 })
	q.reward("spell", "std.jump", 1)
end)
