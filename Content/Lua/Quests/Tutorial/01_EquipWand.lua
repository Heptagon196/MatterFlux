quest.define({
	id = "std.init_quest.equip_wand",
	description = "打开背包，在任意目标键位装备一根法杖。",
	completed_description = "已装备法杖。",
	category = "objective",
	sort = 10,
}, function(q)
	q.equip_wand({})
end)
