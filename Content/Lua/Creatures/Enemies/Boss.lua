creature.define({
	id = "std.test_boss",
	name = "营地首领",
	faction = "hostile",
	level = "boss",
	health = 50,
	width = 150,
	height = 300,
	wait_for_first_sight = true,
	density = 1.40, -- 首领体型和装备使其稳定下沉。
	color_r = 0.72, color_g = 0.04, color_b = 0.12, color_a = 1,
}, function(ai)
	ai.tree({
		sight = { range = 1500, memory_seconds = 15 },
		locomotion = { speed = 380 },
		root = ai.selector({
			ai.sequence({
				ai.condition("target_too_close", { distance = 320 }),
				ai.action("retreat"),
			}),
			ai.sequence({
				ai.condition("target_in_attack_range", { distance = 950 }),
				ai.condition("skill_ready"),
				ai.action("skill", {
					cooldown = 10, spell = "std.default",
					projectiles = 12, radial = true,
					projectile_interval = 0.2, recovery = 1,
					horizontal_impulse = 500, vertical_impulse = 600,
					override_color = true,
					color_r = 1, color_g = 0, color_b = 0, color_a = 1,
				}),
			}),
			ai.sequence({
				ai.condition("target_in_attack_range", { distance = 950 }),
				ai.condition("attack_ready"),
				ai.action("attack", {
					cooldown = 2, spell = "std.default",
					projectiles = 2, spread_degrees = 10,
				}),
			}),
			ai.sequence({
				ai.condition("has_target"),
				ai.action("chase"),
			}),
			ai.action("patrol", { turn_seconds = 3, pause_seconds = 1 }),
		}),
	})
	ai.drop("std.coin", 50000)
end)
