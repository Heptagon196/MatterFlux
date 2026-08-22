dialogue.define({
	id = "dialogue.camp_merchant",
	name = "营地商人",
	start = "welcome",
}, function(d)
	d.node({
		id = "welcome",
		text = "这是营地商店。旅人，要看看法术和补给吗？",
		options = {
			{ text = "看看商品", shop_id = "std.template_merchant" },
			{ text = "你知道这里发生了什么吗？", next = "rumor" },
			{ text = "再见", close = true },
		},
	})
	d.node({
		id = "rumor",
		text = "右边的精英和首领守着道路。先熟悉法杖编程，再去挑战它们。",
		next = "welcome",
	})
end)
