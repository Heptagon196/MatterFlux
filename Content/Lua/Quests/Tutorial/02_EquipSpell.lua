quest.define({
	id = "std.init_quest.equip_spell",
	description = "在已装备法杖上安装一个攻击法术。",
	completed_description = "已装备法术。",
	category = "objective",
	sort = 20,
	prerequisites = { "std.init_quest.equip_wand" },
}, function(q)
	q.equip_spell({})
	q.activation_reward("spell", "std.default", 5)
	q.activation_reward("spell", "std.set_color_red", 1)
	q.activation_reward("spell", "std.double_cast", 2)
	q.activation_reward("spell", "std.circle_trail", 1)
end)
