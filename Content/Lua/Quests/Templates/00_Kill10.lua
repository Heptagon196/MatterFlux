quest.define({
	id = "std.default.kill10",
	description = "击败 10 个普通敌人。",
	category = "objective",
	sort = 10,
}, function(q)
	q.kill_enemies({ target_level = 0, target_count = 10 })
end)
