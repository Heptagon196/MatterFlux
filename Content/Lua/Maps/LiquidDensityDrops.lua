-- 水平的可游玩液体测试场：X/Y 是地表平面，Z 是高度。
-- 左右两滩液体用来观察边缘铺展、接触边界和不同密度材质的表现。
map.define({
    id = "test.liquid_density_drops",
    name = "液体密度分层测试",
	min_x = -20,
	min_y = -16,
	max_x_exclusive = 21,
	max_y_exclusive = 17,
	cell_size_cm = 28,
	material_depth_cells = 0.25,
}, function(m)
	m.fill_circle("acid", -10, 1, 7)
	m.fill_circle("water", -10, -8, 2)
	m.fill_circle("water", 10, 1, 7)
	m.fill_circle("acid", 10, -8, 2)
	m.marker("player_start", 0, -13)
	m.marker("light_drop_chamber", -10, 1)
	m.marker("dense_drop_chamber", 10, 1)

	-- 地板是一张水平碰撞面；材质格铺在其上，而不是竖直展示切片。
	m.scene_box("arena.floor", "grassland", 0, 0, -0.5, 41, 33, 1, true)
	m.scene_box("arena.north", "stone", 0, 16.5, 0.75, 41, 1, 1.5, true)
	m.scene_box("arena.south", "stone", 0, -16.5, 0.75, 41, 1, 1.5, true)
	m.scene_box("arena.west", "stone", -20.5, 0, 0.75, 1, 33, 1.5, true)
	m.scene_box("arena.east", "stone", 20.5, 0, 0.75, 1, 33, 1.5, true)
	-- 稍远的验收镜头必须同时收进左右液池，以及角色、生物和通用物体。
	m.camera("camera.oblique", 30, -46, 40, 0, 0, 0, 55)
end)
