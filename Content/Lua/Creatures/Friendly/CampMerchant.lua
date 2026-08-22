creature.define({
	id = "std.merchant_base",
	name = "营地商人",
	faction = "friendly",
	level = "normal",
	health = 100,
	width = 70,
	height = 160,
	density = 0.70, -- 商人装备略重，但仍可漂浮。
	dialogue_id = "dialogue.camp_merchant",
	shop_id = "std.template_merchant",
	color_r = 0.18, color_g = 0.72, color_b = 0.30, color_a = 1,
}, function(ai)
	ai.tree({
		sight = { range = 0, memory_seconds = 0 },
		locomotion = { speed = 0 },
		root = ai.action("passive"),
	})
	ai.spawn_on_quest(nil, 1, 340)
end)
