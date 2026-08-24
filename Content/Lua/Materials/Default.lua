-- 物质的密度同时用于通用材料模拟和浮力判断；光学字段只由液体渲染读取。
material.define({ id = "water", density = 1.00, hardness = 0.05,
    color_r = 0.12, color_g = 0.46, color_b = 0.78, color_a = 0.82,
    phase = "liquid", mobility = 255, dispersion = 220,
    -- 保留浅水层次；液面几何直接来自模拟液柱。
    shallow_opacity = 0.82, deep_opacity = 0.96, opacity_depth = 120.0 })
-- 酸比水重，因此两者接触时只发生物理分层；化学脚本中刻意没有 acid+water 规则。
material.define({ id = "acid", density = 1.22, hardness = 0.03,
	-- 森林地表已经大量使用绿色；酸改用高饱和紫红。酸的浅层不透明度
	-- 也必须足够高，否则薄液体表面与绿地混合后会重新变成灰褐色。
	color_r = 0.86, color_g = 0.08, color_b = 0.92, color_a = 0.90,
    phase = "liquid", mobility = 255, dispersion = 210,
    shallow_opacity = 0.72, deep_opacity = 0.94, opacity_depth = 125.0 })
material.define({ id = "acid_gas", density = 0.07, hardness = 0.00,
    color_r = 0.58, color_g = 0.92, color_b = 0.20, color_a = 0.58,
    phase = "gas", mobility = 245, dispersion = 250 })
material.define({ id = "soil", density = 1.45, hardness = 0.70,
    color_r = 0.34, color_g = 0.19, color_b = 0.08, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "grass", density = 0.35, hardness = 0.20,
    color_r = 0.16, color_g = 0.55, color_b = 0.18, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "grassland", density = 0.42, hardness = 0.26,
    color_r = 0.12, color_g = 0.36, color_b = 0.085, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "wood", density = 0.82, hardness = 0.80,
    color_r = 0.38, color_g = 0.18, color_b = 0.05, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "leaf", density = 0.22, hardness = 0.12,
    color_r = 0.07, color_g = 0.42, color_b = 0.10, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "stone", density = 2.80, hardness = 0.95,
    color_r = 0.30, color_g = 0.34, color_b = 0.40, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "flower_pink", density = 0.12, hardness = 0.05,
    color_r = 0.94, color_g = 0.25, color_b = 0.56, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "flower_gold", density = 0.12, hardness = 0.05,
    color_r = 1.00, color_g = 0.66, color_b = 0.08, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "flower_blue", density = 0.12, hardness = 0.05,
    color_r = 0.30, color_g = 0.42, color_b = 0.96, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "sand", density = 1.65, hardness = 0.20,
    color_r = 0.76, color_g = 0.61, color_b = 0.28, color_a = 1.00,
    phase = "powder", mobility = 255, dispersion = 0 })
material.define({ id = "steam", density = 0.10, hardness = 0.00,
    color_r = 0.82, color_g = 0.88, color_b = 0.92, color_a = 0.52,
    phase = "gas", mobility = 255, dispersion = 255 })
material.define({ id = "lava", density = 2.80, hardness = 0.15,
    color_r = 1.00, color_g = 0.16, color_b = 0.01, color_a = 1.00,
    phase = "liquid", mobility = 96, dispersion = 32,
    shallow_opacity = 0.76, deep_opacity = 0.98, opacity_depth = 70.0 })
material.define({ id = "fire", density = 0.01, hardness = 0.00,
    color_r = 1.00, color_g = 0.24, color_b = 0.01, color_a = 0.92,
    phase = "gas", mobility = 255, dispersion = 255, lifetime_steps = 6 })
material.define({ id = "smoke", density = 0.05, hardness = 0.00,
    color_r = 0.14, color_g = 0.15, color_b = 0.17, color_a = 0.66,
    phase = "gas", mobility = 210, dispersion = 235 })
material.define({ id = "charcoal", density = 0.68, hardness = 0.46,
    color_r = 0.07, color_g = 0.06, color_b = 0.055, color_a = 1.00,
    phase = "static", mobility = 0, dispersion = 0 })
material.define({ id = "ash", density = 0.22, hardness = 0.08,
    color_r = 0.16, color_g = 0.15, color_b = 0.14, color_a = 1.00,
    phase = "powder", mobility = 90, dispersion = 24 })
