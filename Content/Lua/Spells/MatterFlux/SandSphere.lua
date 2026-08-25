spell.define({
	id = "spell.sand_sphere", name = "悬空沙球",
	description = "在施法方向前方的上空生成大量真实沙粒；沙粒从生成起便受重力落下。",
	icon = "sand_spray", mana_cost = 24, starter_count = 2
}, function(api)
	api.projectile({
		damage = 0, speed = 1, lifetime = 5.0, radius = 100,
		gravity_scale = 1.0, body_material = "sand", material_amount = 2048,
		spawn_forward_offset = 80, spawn_height_offset = 300,
		spawn_stationary = true,
		cast_delay = 0.25, recharge_time = 0.20
	})
end)
