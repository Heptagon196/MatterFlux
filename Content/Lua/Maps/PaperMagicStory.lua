-- PaperMagic demo 的线性故事关卡。所有关键角色使用稳定 marker，
-- 任务系统只决定何时生成它们，不再围绕玩家随机选择位置。
map.define({
	id = "story.paper_magic",
	name = "纸境序章",
	min_x = -6,
	min_y = -9,
	max_x_exclusive = 49,
	max_y_exclusive = 10,
	cell_size_cm = 100,
	material_depth_cells = 0.25,
}, function(m)
	-- 这是自定义地图编译器所需的有界模拟底图。游戏中的可见地形、
	-- 河流和自然装饰由故事固定种子走标准程序化生成管线。
	m.fill_rectangle("grassland", -6, -9, 48, 9)

	-- 玩家和三个教学敌人都在左侧封闭训练区内。
	m.marker("player_start", -2, 0)
	m.marker("creature.std.slime.0", 0, -3)
	m.marker("creature.std.slime.1", 2, 3)
	m.marker("creature.std.elite_patrol.0", 4, 0)
	m.marker("story.left_barrier", 6, 0)
	m.marker("story.right_barrier", 24, 0)

	-- 紧凑营地只保留一个商人和一栋固定房屋。
	m.marker("creature.std.merchant_base.0", 11, -5)
	m.spawn_creature("std.merchant_base", "creature.std.merchant_base.0")
	m.marker("structure.house.two_storey.0", 17, 2)
	-- 最终区域保持最大尺寸，两个小首领分列河流右侧。
	m.marker("creature.std.test_boss.0", 39, -4)
	m.marker("creature.std.test_boss.1", 42, 4)

	-- 自然环境不再使用固定 scene_box；这里只保留剧情区域边界。
	m.scene_box("story.north_wall", "stone", 21.5, 9.5, 1, 55, 1, 2, true)
	m.scene_box("story.south_wall", "stone", 21.5, -9.5, 1, 55, 1, 2, true)
	m.scene_box("story.west_wall", "stone", -6.5, 0, 1, 1, 19, 2, true)
	m.scene_box("story.east_wall", "stone", 49.5, 0, 1, 1, 19, 2, true)

	-- 两排连续石头将训练区、营地和首领区分开。0.75 米高于角色的
	-- 默认跨步高度，但低于 600 冲量在 1.8 倍重力下约 1.02 米的顶点。
	local rock_y = { -8.4, -6.0, -3.6, -1.2, 1.2, 3.6, 6.0, 8.4 }
	local rock_width = { 1.5, 1.7, 1.5, 1.8, 1.6, 1.7, 1.5, 1.8 }
	for i = 1, #rock_y do
		m.scene_box(string.format("story.left_rocks.%02d", i - 1),
			"stone", 6, rock_y[i], 0.375, rock_width[i], 2.6, 0.75, true)
		m.scene_box(string.format("story.right_rocks.%02d", i - 1),
			"stone", 24, rock_y[i], 0.375, rock_width[#rock_width - i + 1], 2.6, 0.75, true)
	end
	m.camera("story.overview", 22, -32, 28, 22, 0, 0, 50)
end)
