-- Exact PaperMagic shop wand id, interpreted by MatterFlux's deterministic
-- wand runtime instead of the original Unity equipment callback object.
content.register_wand({
	id = "std.advanced_wand",
	name = "高级法杖",
	description = "可绑定到任意目标键位；高法力回复，容量 10。",
	icon = "wand_advanced",
	capacity = 10,
	shuffle = false,
	draw_count = 1,
	cast_delay = 0.20,
	recharge_time = 0.20,
	mana_max = 100,
	mana_recharge = 100,
	spread = 0
})
