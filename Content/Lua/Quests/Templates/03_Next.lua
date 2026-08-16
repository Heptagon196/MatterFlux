quest.define({
	id = "std.default2",
	name = "模板任务 2",
	description = "用于验证前置任务自动接取。",
	category = "main",
	prerequisites = { "std.default" },
}, function(q)
	q.never()
end)
