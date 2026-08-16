# Mission: 用 MatterFlux 学会制作可扩展的 UE 游戏

## Why
以 MatterFlux 这个 Noita 风格 2.5D 项目为练习，掌握能真正用于单机、联机和持续扩展的 UE C++ 工程方法，而不只是拼出一次性演示。

## Success looks like
- 能解释角色、场景、碎片模拟、Lua 内容层和网络权威各自的职责
- 能独立添加一种材质、地形装饰或敌人定义，并验证热重载结果
- 能用 Editor/Game/Server 构建和 Automation 测试定位问题
- 能判断一项功能应该写在 C++、Lua、Data Asset 还是复制状态中

## Constraints
- 面向 UE 初学者，示例必须对应当前仓库中的真实代码
- 核心模拟要求确定性、服务器权威和可测试性
- 配置需要快速迭代，但不能让脚本任意访问系统或 UObject

## Out of scope
- 本阶段不教授完整 Lua 语言
- 本阶段不实现正式的签名热更新/CDN 发布服务
- 当前可玩场景仍是验证场景，不代表最终 Noita 风格体素美术管线

