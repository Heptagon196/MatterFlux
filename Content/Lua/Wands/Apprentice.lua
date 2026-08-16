content.register_wand({
	id = "wand.apprentice", name = "学徒法杖",
	description = "按顺序施法，适合学习法术编程。",
	icon = "wand_apprentice", capacity = 8, shuffle = false,
	draw_count = 1, cast_delay = 0.16, recharge_time = 0.48,
	mana_max = 110, mana_recharge = 28, spread = 2.5,
	starter_slot = 2,
	starter_deck = {
		"spell.double_cast", "spell.add_damage", "spell.spark_trigger",
		"spell.spark_bolt", "spell.ember_bolt"
	}
})
