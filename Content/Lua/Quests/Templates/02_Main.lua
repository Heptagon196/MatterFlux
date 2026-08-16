quest.define({
	id = "std.default",
	name = "模板任务",
	description = "击败指定敌人。",
	category = "main",
	children = { "std.default.kill10", "std.default.kill1_special" },
}, function(q)
	q.complete_children()
end)
