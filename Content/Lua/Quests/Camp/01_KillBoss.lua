quest.define({
	id = "std.side_kill_boss",
	name = "最终任务：营地右侧",
	description = "越过营地右侧高石排，击败最终区域的两个小首领。",
	completed_description = "任务完成，参考项目 Demo 任务链结束。",
	category = "side",
	sort = 110,
	focus_on_activate = true,
	prerequisites = { "std.init_quest_buy" },
}, function(q)
	q.spawn_creature("std.test_boss", "creature.std.test_boss.0")
	q.spawn_creature("std.test_boss", "creature.std.test_boss.1")
	q.kill_enemies({ target_level = 2, target_count = 2 })
end)
