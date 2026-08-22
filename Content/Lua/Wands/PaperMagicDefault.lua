-- PaperMagic's default weapon-slot caster, represented as a key-bindable wand.
content.register_wand({
	id = "std.default",
	name = "默认法杖",
	description = "PaperMagic 教学法杖；容量 10，法力回复较慢。",
	icon = "wand_default",
	capacity = 10,
	shuffle = false,
	draw_count = 1,
	cast_delay = 0.50,
	recharge_time = 0.50,
	mana_max = 100,
	mana_recharge = 10,
	spread = 0,
	starter_count = 1
})
