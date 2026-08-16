quest.define({
	id = "std.default.kill1_special",
	description = "击败 1 个精英敌人。",
	category = "objective",
	sort = 20,
	optional = true,
}, function(q)
	q.kill_enemies({ target_level = 1, target_count = 1 })
end)
