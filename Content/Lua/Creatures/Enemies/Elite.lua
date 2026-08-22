creature.define({
	id = "std.elite_patrol",
	name = "巡逻精英",
	faction = "hostile",
	level = "elite",
	health = 50,
	width = 75,
	height = 170,
	density = 0.90, -- 精英只能在较深水域接近中性浮力。
	color_r = 0.88, color_g = 0.12, color_b = 0.08, color_a = 1,
}, function(ai)
	ai.tree({
		sight = { range = 1300, memory_seconds = 15 },
		locomotion = { speed = 360 },
		root = ai.selector({
			ai.sequence({
				ai.condition("target_too_close", { distance = 300 }),
				ai.action("retreat"),
			}),
			ai.sequence({
				ai.condition("target_in_attack_range", { distance = 850 }),
				ai.condition("attack_ready"),
				ai.action("attack", { cooldown = 2, spell = "std.default" }),
			}),
			ai.sequence({
				ai.condition("has_target"),
				ai.action("chase"),
			}),
			ai.action("patrol", { turn_seconds = 5, pause_seconds = 1 }),
		}),
	})
	ai.drop("std.coin", 5000)
	ai.spawn_on_quest("std.init_quest.kill_enemy", 1, 850)
end)
