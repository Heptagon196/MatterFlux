# MatterFlux UI 对齐与布局规范（UE 初学者版）

本文记录 MatterFlux 运行时 UI 的布局入口、文字对齐规则和视觉验收方法。目标不是让所有文字机械地居中，而是让“控件中的短文字”严格居中，让“需要连续阅读的正文”保持左对齐。

## 1. 当前运行时 UI 在哪里

项目的运行时 Slate UI 有三个页面入口和一个共享设置模块：

- `Source/MatterFlux/Private/UI/MatterFluxShellSlate.cpp`：开始菜单、单人/多人菜单、存档列表、加入房间、暂停菜单和操作进度；`MatterFluxShellWidget.cpp` 只保留 UMG adapter。
- `Source/MatterFlux/Private/Magic/MatterFluxMagicWorkbenchSlate.cpp`：法术编程、法杖背包、道具背包和任务页；`MatterFluxMagicWorkbenchWidget.cpp` 只保留 UMG adapter。
- `Source/MatterFlux/Private/Progression/MatterFluxQuestTrackerWidget.cpp`：右上角任务追踪 HUD。
- `Source/MatterFlux/Private/UI/MatterFluxSettingsPanel.h/.cpp`：唯一的设置 UI 行为实现，集中处理读取、即时预览、应用、保存和局部刷新。
- `Source/MatterFlux/Private/UI/MatterFluxPaperStyle.h/.cpp`：唯一的黑白主题实现，集中提供颜色 token、字体、按钮状态和双层描边面板。

共享的黑白进度条位于 `Source/MatterFlux/Private/UI/MatterFluxProgressBar.h`，它没有文字，只负责绘制轨道和填充。

## 2. 为什么只写 Justification(Center) 仍然会歪

Slate 的布局至少有两层：

1. 父控件决定给子控件多少空间，以及把子控件放在左、右还是中央。
2. `STextBlock` 决定文字在自己拿到的空间内如何排版。

因此下面的代码并不保证文字在按钮中居中：

```cpp
SNew(SButton)
[
    SNew(STextBlock)
    .Justification(ETextJustify::Center)
]
```

如果文本块只拿到自己的最小宽度，它在这点宽度里“居中”没有任何视觉效果。MatterFlux 的短文本控件使用完整写法：

```cpp
SNew(SButton)
.ContentPadding(FMargin(10.0f, 0.0f))
.HAlign(HAlign_Center)
.VAlign(VAlign_Center)
[
    SNew(STextBlock)
    .Text(...)
    .Justification(ETextJustify::Center)
]
```

也就是：父槽负责控件几何中心，文本块负责文字排版中心。

## 3. MatterFlux 的统一规则

以下内容必须水平、垂直居中：

- 普通按钮、禁用按钮和底部主操作按钮；
- 顶栏页签、帮助按钮和关闭按钮；
- `+`、`−`、保存、复制、重命名、删除等短操作；
- 输入框中的存档名和服务器地址；
- 下拉框当前值与下拉菜单选项；
- 设置页的数值显示和开关文字；
- 法杖统计卡片的标题与数值；
- 法术槽中心徽标、法术树节点关系标签；
- 空存档列表的占位状态。

以下内容保持左对齐：

- 页面标题和分区标题；
- 物品、法杖和任务的长描述；
- 任务目标列表与任务追踪 HUD；
- 存档名下方的种子、时间等明细；
- 操作进度的阶段说明。

长文本左对齐能让每一行拥有相同起点，中文段落也更容易扫读。标题左对齐还能建立稳定的信息层级。

## 4. 固定尺寸与行高

开始菜单的通用按钮固定为 38 像素高。设置值区域同样以 38 像素为基准，整条设置行高为 48 像素。这样不同字号、中文回退字体和数字不会改变每一行的视觉中心。

法术槽统一使用 58×58 像素。法术背包、法术树和空闲容量区都复用这个尺寸，父容器不能再次缩放槽位。

输入框由外层 `SBox` 固定高度并设置 `VAlign_Center`。`SEditableText` 自己只设置文字的 `Justification`，不要假定输入控件会自动垂直居中。

## 5. 按下按钮为什么会跳一下

Slate 的部分按钮样式会给按下态增加不同的 padding，用来模拟传统按钮被压下的效果。MatterFlux 使用硬朗的黑白像素风格，这种 1 像素偏移会让文字看起来忽左忽右。

Shell 的按钮样式明确使用：

```cpp
.SetNormalPadding(FMargin(0.0f))
.SetPressedPadding(FMargin(0.0f))
```

悬停和按下仍通过背景灰度变化反馈，不移动文字。

## 6. 下拉框如何让文字真的位于整体中心

如果用横向盒子把“当前值”和“▼”并排，当前值只能在扣除箭头后的剩余区域内居中，看起来会向左偏。

MatterFlux 使用 `SOverlay`：

- 当前值占满整个控件并居中；
- 箭头作为另一层靠右显示；
- 两者互不挤压。

这个方法也适用于“中心标题 + 右侧徽标”一类控件。

## 7. 任务 HUD 为什么不居中

任务 HUD 是阅读区域，不是按钮。任务名、说明、目标和计数保持左对齐。这里的正确性体现在统一的左边界、目标间距和自动换行，而不是把每一行放到面板中央。

## 8. 外部截图验收

Shell 页面可以从命令行切换并截图：

```text
mf.UI.Capture settings 2 1
mf.UI.Capture create 2 1
mf.UI.Capture save 2 1
mf.UI.Capture join 2 1
```

法术工作台可以指定页面：

```text
mf.Visual.Capture 2 1 1 1337 1 0 0 nested spell
mf.Visual.Capture 2 1 1 1337 1 0 0 nested quest
mf.Visual.Capture 2 1 1 1337 1 0 0 nested settings
```

截图位于当前 `UserDir` 的 `Saved/Screenshots/WindowsEditor`。固定使用 1600×900，便于前后版本逐像素比较。

视觉检查时依次确认：

1. 文字中心是否与黑色边框的几何中心一致；
2. 同一行的按钮是否拥有相同高度和基线；
3. 禁用态是否仍居中；
4. 按下按钮时文字是否发生位移；
5. 下拉箭头是否没有挤偏当前值；
6. 法术树标签、槽位和连接线中心是否仍一致；
7. 长文本是否保留稳定的左边界并正确换行。

## 9. 本轮验证结果

- `MatterFluxEditor Win64 Development` 构建成功；实际使用 MSVC 14.44.35222。
- 已在 1600×900 下检查 Shell 设置页、创建房间页、真实存档页、工作台设置页、复杂法术树页和任务页。
- `MatterFlux.Save`、`MatterFlux.Settings`、`MatterFlux.Magic.ProgramLayout` 与参考任务流程共 10 项自动化测试全部通过。
- Shell 与工作台不再各自维护设置控件；两处都通过零配置的 `SNew(SMatterFluxSettingsPanel)` 使用同一实现。

以后新增控件时，先判断它是“短操作控件”还是“阅读区域”，再套用对应规则。不要用全局强制居中来修复局部布局问题。
