content.register_wand({
	id = "wand.cutting", name = "伐木法杖",
	description = "左手法杖；初始装有地形切割法术。",
	icon = "wand_cutting", capacity = 4, shuffle = false,
	draw_count = 1, cast_delay = 0.12, recharge_time = 0.20,
	mana_max = 100, mana_recharge = 30, spread = 0,
	starter_slot = 0, starter_deck = { "spell.terrain_cut" }
})
