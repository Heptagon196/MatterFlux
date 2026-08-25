quest.define({
	id = "std.init_quest.kill_enemy",
	description = "击败封闭训练区内出现的全部三个敌人。",
	completed_description = "已获得腿部法杖和跳跃法术，可以越过高石排。",
	category = "objective",
	sort = 30,
	prerequisites = { "std.init_quest.equip_spell" },
}, function(q)
	q.spawn_creature("std.slime", "creature.std.slime.0")
	q.spawn_creature("std.slime", "creature.std.slime.1")
	q.spawn_creature("std.elite_patrol", "creature.std.elite_patrol.0")
	q.kill_enemies({ target_count = 3 })
	q.reward("wand", "std.default_shoe", 1, 4)
	q.reward("spell", "std.jump", 1)
end)
