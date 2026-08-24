structure.define({
	id = "structure.house.two_storey",
	generator = "two_storey_house",
}, {
	-- 连通只用于容忍体素取整留下的小缝；结构身份仍来自 Source role。
	contact_tolerance_cm = 12.0,
	floor_snap_height_cm = 28.0,
	preferred_floor_padding_cm = 90.0,
	preferred_floor_vertical_range_cm = 420.0,
	exit_grace_seconds = 0.18,
	fade_speed = 4.5,
	wall_opacity = 0.055,
	roof_opacity = 0.025,
})
