shop.define({
	id = "std.template_merchant",
	name = "营地商店",
}, function(s)
	s.category({ id = "supplies", name = "补给" })
	s.category({ id = "projectile", name = "投射物" })
	s.category({ id = "modifier", name = "修饰" })
	s.category({ id = "multicast", name = "多重释放" })
	s.category({ id = "trigger", name = "载体触发" })
	s.category({ id = "trigger_modifier", name = "触发修饰" })
	s.category({ id = "jump", name = "位移" })
	s.category({ id = "wand", name = "法杖" })

	s.offer({ kind = "item", product_id = "std.heal_item", product_count = 1, cost_item = "std.coin", cost_count = 100, limit = -1, category = "supplies" })

	local spell_offers = {
		{ "std.default", 100, -1, "projectile" },
		{ "spell.spark_bolt", 120, 10, "projectile" },
		{ "spell.heavy_orb", 300, 10, "projectile" },
		{ "spell.flame_jet", 180, 10, "projectile" },
		{ "spell.water_spray", 160, 10, "projectile" },
		{ "spell.sand_spray", 160, 10, "projectile" },
		{ "spell.acid_spray", 220, 10, "projectile" },
		{ "spell.terrain_cut", 240, 10, "projectile" },
		{ "spell.vertical_terrain_cut", 240, 10, "projectile" },
		{ "std.circle_trail", 100, 10, "modifier" },
		{ "std.set_color_red", 100, 10, "modifier" },
		{ "spell.add_damage", 140, 10, "modifier" },
		{ "spell.accelerate", 140, 10, "modifier" },
		{ "std.double_cast", 200, 10, "multicast" },
		{ "spell.double_cast", 200, 10, "multicast" },
		{ "std.triple_cast", 300, 10, "multicast" },
		{ "spell.spark_trigger", 300, 10, "trigger" },
		{ "std.trigger_on_collision", 300, 10, "trigger_modifier" },
		{ "std.trigger_on_expired", 300, 10, "trigger_modifier" },
		{ "std.jump", 100, 10, "jump" },
	}
	for _, offer in ipairs(spell_offers) do
		s.offer({
			kind = "spell",
			product_id = offer[1],
			product_count = 1,
			cost_item = "std.coin",
			cost_count = offer[2],
			limit = offer[3],
			category = offer[4],
		})
	end

	s.offer({ kind = "wand", product_id = "std.advanced_wand", product_count = 1, cost_item = "std.coin", cost_count = 500, limit = 10, category = "wand" })
end)
