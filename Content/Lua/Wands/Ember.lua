content.register_wand({
	id = "wand.ember", name = "余烬法杖",
	description = "用于远距离点燃目标的备用法杖。",
	icon = "wand_ember", capacity = 6, shuffle = false,
	draw_count = 1, cast_delay = 0.14, recharge_time = 0.35,
	mana_max = 120, mana_recharge = 30, spread = 3,
	starter_slot = 3, starter_deck = { "spell.ember_bolt" }
})
