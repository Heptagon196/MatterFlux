item.define({
	id = "std.heal_item",
	name = "治疗药剂",
	description = "恢复 30 点生命。右键或 Enter 使用。",
	icon = "paper/default_item",
	category = "consumable",
	max_stack = 20,
	starter_count = 2,
}, function(use)
	use.restore_health(30, 1)
end)
