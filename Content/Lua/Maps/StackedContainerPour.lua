-- 两个同 X/Y、不同高度的悬空容器同步倾倒。
-- 这是可玩的水平测试地图；远处随机世界不会参与该夹具。
map.define({
    id = "test.stacked_container_pour",
    name = "叠层容器同步倾倒",
    min_x = -24,
    min_y = -18,
    max_x_exclusive = 25,
    max_y_exclusive = 19,
    cell_size_cm = 24,
    material_depth_cells = 0.25,
}, function(m)
    m.marker("player_start", -15, -11)

    -- 水平可行走地面和接液池，池壁可碰撞。
    m.scene_box("arena.floor", "grassland", 0, 0, -0.5, 49, 37, 1, true)
    m.scene_box("catch.floor", "stone", 7, 0, 0.25, 15, 13, 0.5, true)
    m.scene_box("catch.left", "stone", 7, -6.25, 1.5, 15, 0.5, 3, true)
    m.scene_box("catch.right", "stone", 7, 6.25, 1.5, 15, 0.5, 3, true)
    m.scene_box("catch.back", "stone", 13.75, 0, 1.5, 0.5, 13, 3, true)

    local shared_motion = {
        start_step = 12,
        tilt_steps = 36,
        tilt_degrees = 72,
        pour_cells_per_step = 8,
    }
    m.tilting_container({
        id = "water.lower",
        container_material = "stone",
        liquid_material = "water",
        center_x = 0,
        center_y = 0,
        center_z = 11,
        inner_width = 7,
        inner_depth = 5,
        inner_height = 5,
        start_step = shared_motion.start_step,
        tilt_steps = shared_motion.tilt_steps,
        tilt_degrees = shared_motion.tilt_degrees,
        pour_cells_per_step = shared_motion.pour_cells_per_step,
    })
    m.tilting_container({
        id = "acid.upper",
        container_material = "stone",
        liquid_material = "acid",
        center_x = 0,
        center_y = 0,
        center_z = 21,
        inner_width = 7,
        inner_depth = 5,
        inner_height = 5,
        start_step = shared_motion.start_step,
        tilt_steps = shared_motion.tilt_steps,
        tilt_degrees = shared_motion.tilt_degrees,
        pour_cells_per_step = shared_motion.pour_cells_per_step,
    })

    m.camera("camera.oblique", 34, -42, 31, 4, 0, 10, 48)
end)
