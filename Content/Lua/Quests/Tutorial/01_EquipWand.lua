quest.define({
	id = "std.init_quest.equip_wand",
	description = "打开背包，把初始法杖装备到左键攻击位。",
	completed_description = "已装备法杖。",
	category = "objective",
	sort = 10,
}, function(q)
	q.equip_wand({ target_id = "std.default", equipment_slot = 0 })
	q.activation_reward("wand", "std.default", 1)
	q.activation_reward("spell", "std.default", 1)
end)
