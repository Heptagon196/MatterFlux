quest.define({
	id = "std.side_kill_boss",
	name = "清理营地右侧",
	description = "击败营地右侧的两个强敌。",
	completed_description = "任务完成，参考项目 Demo 任务链结束。",
	category = "side",
	sort = 110,
	focus_on_activate = true,
	prerequisites = { "std.init_quest_buy" },
}, function(q)
	q.kill_enemies({ target_level = 2, target_count = 2 })
end)
