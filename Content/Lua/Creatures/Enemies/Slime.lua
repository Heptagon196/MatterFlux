creature.define({
	id = "std.slime",
	name = "史莱姆",
	faction = "hostile",
	level = "elite",
	health = 15,
	width = 90,
	height = 85,
	density = 0.35, -- 空泡状史莱姆明显漂在液面。
	color_r = 0.30, color_g = 0.86, color_b = 0.18, color_a = 1,
}, function(ai)
	ai.tree({
		sight = { range = 700, memory_seconds = 5 },
		locomotion = { speed = 220 },
		root = ai.selector({
			ai.sequence({
				ai.condition("target_in_attack_range", { distance = 300 }),
				ai.condition("attack_ready"),
				ai.action("attack", { cooldown = 2, spell = "std.default" }),
			}),
			ai.sequence({
				ai.condition("has_target"),
				ai.action("chase"),
			}),
			ai.action("patrol", { turn_seconds = 3, pause_seconds = 1 }),
		}),
	})
	ai.drop("std.coin", 5000)
	ai.spawn_on_quest("std.init_quest.kill_enemy", 2, 520)
end)
