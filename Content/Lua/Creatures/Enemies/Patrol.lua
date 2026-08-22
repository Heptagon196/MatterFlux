creature.define({
	id = "std.patrol",
	name = "巡逻小兵",
	faction = "hostile",
	level = "normal",
	health = 20,
	width = 65,
	height = 145,
	density = 1.15, -- 重甲巡逻者会沉到水底。
	color_r = 0.86, color_g = 0.72, color_b = 0.10, color_a = 1,
}, function(ai)
	ai.tree({
		sight = { range = 1000, memory_seconds = 15 },
		locomotion = { speed = 300 },
		root = ai.selector({
			ai.sequence({
				ai.condition("target_too_close", { distance = 260 }),
				ai.action("retreat"),
			}),
			ai.sequence({
				ai.condition("target_in_attack_range", { distance = 650 }),
				ai.condition("attack_ready"),
				ai.action("attack", { cooldown = 3, spell = "std.default" }),
			}),
			ai.sequence({
				ai.condition("has_target"),
				ai.action("chase"),
			}),
			ai.action("patrol", { turn_seconds = 5, pause_seconds = 0.5 }),
		}),
	})
	ai.drop("std.coin", 50)
end)
