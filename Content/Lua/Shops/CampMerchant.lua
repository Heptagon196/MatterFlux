shop.define({
	id = "std.template_merchant",
	name = "营地商店",
}, function(s)
	s.offer({ kind = "item", product_id = "std.heal_item", product_count = 1, cost_item = "std.coin", cost_count = 100, limit = -1 })
	s.offer({ kind = "spell", product_id = "std.default", product_count = 1, cost_item = "std.coin", cost_count = 100, limit = -1 })
	s.offer({ kind = "spell", product_id = "std.add_damage", product_count = 1, cost_item = "std.coin", cost_count = 100, limit = 10 })
	s.offer({ kind = "spell", product_id = "std.circle_trail", product_count = 1, cost_item = "std.coin", cost_count = 100, limit = 10 })
	s.offer({ kind = "spell", product_id = "std.jump", product_count = 1, cost_item = "std.coin", cost_count = 100, limit = 10 })
	s.offer({ kind = "spell", product_id = "std.double_cast", product_count = 1, cost_item = "std.coin", cost_count = 200, limit = 10 })
	s.offer({ kind = "spell", product_id = "std.triple_cast", product_count = 1, cost_item = "std.coin", cost_count = 300, limit = 10 })
	s.offer({ kind = "spell", product_id = "std.trigger_on_collision", product_count = 1, cost_item = "std.coin", cost_count = 300, limit = 10 })
	s.offer({ kind = "spell", product_id = "std.trigger_on_expired", product_count = 1, cost_item = "std.coin", cost_count = 300, limit = 10 })
	s.offer({ kind = "wand", product_id = "std.advanced_wand", product_count = 1, cost_item = "std.coin", cost_count = 500, limit = 10 })
end)
