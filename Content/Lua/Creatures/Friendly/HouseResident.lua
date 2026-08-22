-- 用于室内活动与可视化验收的普通住民。没有玩家感知和攻击分支，
-- 因此会稳定沿房屋提供的跨楼层巡逻路线移动。
creature.define({
	id = "std.house_resident",
	name = "房屋住民",
	faction = "friendly",
	level = "normal",
	health = 100,
	width = 62,
	height = 142,
	density = 0.65, -- 普通人会在水中保持头部露出。
	color_r = 0.34, color_g = 0.56, color_b = 0.88, color_a = 1,
}, function(ai)
	ai.tree({
		sight = { range = 0, memory_seconds = 0 },
		locomotion = { speed = 240 },
		root = ai.action("patrol", {
			turn_seconds = 5,
			pause_seconds = 0,
		}),
	})
end)
