-- Space is a regular equipment key. Its starter caster uses the same generic
-- wand program and GAS execution path as every other bound slot.
content.register_wand({
	id = "std.default_shoe",
	name = "默认鞋型施法器",
	description = "容量 1、回复迅速，适合安装跳跃等移动法术。",
	icon = "wand_default_shoe",
	capacity = 1,
	shuffle = false,
	draw_count = 1,
	cast_delay = 0.50,
	recharge_time = 0.50,
	mana_max = 100,
	mana_recharge = 50,
	spread = 0,
	starter_count = 1,
	starter_slot = 4,
	starter_deck = { "std.jump" }
})
