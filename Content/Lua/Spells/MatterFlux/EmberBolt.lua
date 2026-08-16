spell.define({
	id = "spell.ember_bolt", name = "余烬弹",
	description = "命中后点燃可燃的 MatterFlux 材质。",
	icon = "ember_bolt", mana_cost = 14, starter_count = 4
}, function(api)
	api.projectile({ damage = 8, speed = 980, lifetime = 2.4, radius = 18, impact_material = "fire", cast_delay = 0.08 })
end)
