quest.define({
	id = "std.init_quest",
	name = "教学任务",
	description = "学会装备法杖、编排法术并击败敌人。",
	completed_description = "已掌握法杖与法术的基本操作。",
	category = "main",
	starter = true,
	focus_on_activate = true,
	children = {
		"std.init_quest.equip_wand",
		"std.init_quest.equip_spell",
		"std.init_quest.kill_enemy",
	},
}, function(q)
	q.complete_children()
end)
