# MatterFlux / Unreal Engine Resources

## Knowledge

- [Lua 5.4 Reference Manual](https://www.lua.org/manual/5.4/manual.html)
  Lua 官方手册。用于确认嵌入 API、受保护调用、内存分配器、调试钩子和标准库行为。
- [UE 5.8 Data Assets](https://dev.epicgames.com/documentation/en-us/unreal-engine/data-assets-in-unreal-engine)
  Epic 官方文档。用于理解 `UDataAsset`、`UPrimaryDataAsset` 和资源包元数据。
- [UE 5.8 Asset Management](https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-management-in-unreal-engine)
  Epic 官方文档。用于理解 Primary/Secondary Asset、软引用、异步加载和 Asset Bundle。
- [UE Actor Property Replication](https://dev.epicgames.com/documentation/unreal-engine/replicate-actor-properties-in-unreal-engine)
  Epic 官方文档。用于理解 `ReplicatedUsing`、RepNotify 和 `DOREPLIFETIME`。
- [UE 5.8 Cooking and Chunks](https://dev.epicgames.com/documentation/en-us/unreal-engine/cooking-content-and-creating-chunks-in-unreal-engine)
  Epic 官方文档。用于后续把已验证的内容包做成可签名、可回滚的发布分块。
- [Tencent UnLua](https://github.com/Tencent/UnLua)
  完整 UE/Lua 绑定方案的官方仓库。MatterFlux 已实际验证其当前开发分支不能直接通过 UE 5.8 编译，因此本项目只嵌入 Lua 语言内核，不采用其全反射绑定层。

## Wisdom (Communities)

- [Epic Developer Community Forums](https://forums.unrealengine.com/)
  适合核对引擎版本迁移、打包和复制系统的实际踩坑。
- [Lua-L mailing list](https://www.lua.org/lua-l.html)
  Lua 官方社区入口，适合讨论嵌入、安全沙箱和 C API 的边界问题。

