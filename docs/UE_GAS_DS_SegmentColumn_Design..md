# UE + GAS + DS + Segment Column + Active Chunk 系统设计文档

## 1. 项目目标

构建一个：

-   支持多人联机
-   基于 Unreal Engine（UE）
-   使用 Gameplay Ability System（GAS）
-   Dedicated Server 权威架构
-   可破坏 + 流体 + 火焰 + 爆炸交互
-   Segment Column 2.5D 体素世界

核心体验： - 类 Noita 的物理连锁反应 - 类 Teardown 的结构破坏 -
多人一致性世界模拟

------------------------------------------------------------------------

## 2. 核心设计原则

### 2.1 服务器权威

所有物理模拟在 DS（Dedicated Server）执行。

### 2.2 Event Driven Simulation

不同步世界状态，只同步事件与差异。

### 2.3 Chunk-Based Partition

世界按 Chunk 划分（32×32 tiles）。

### 2.4 Segment Column Model

每个 Tile 是 Z 方向 Segment 列表。

------------------------------------------------------------------------

## 3. 世界数据结构

### Chunk

-   ChunkX / ChunkY
-   State（Active / Warm / Cold）
-   Tile数组

### Tile

-   Segment List

### Segment

-   StartZ / EndZ
-   Material
-   Flags

------------------------------------------------------------------------

## 4. Active Chunk System

### 状态

-   Active：全模拟
-   Warm：低频模拟
-   Cold：冻结

### 唤醒机制

-   Explosion
-   Fire
-   Dig
-   Liquid spawn

------------------------------------------------------------------------

## 5. 服务器模拟系统

每 Tick：

1.  处理事件队列
2.  更新 Active Chunks
3.  流体模拟
4.  火焰传播
5.  结构破坏
6.  生成 Chunk Diff

------------------------------------------------------------------------

## 6. GAS 系统职责

GAS 只负责：

-   技能触发
-   冷却/消耗
-   Gameplay Event 发送

不负责物理模拟。

------------------------------------------------------------------------

## 7. 网络同步系统

### 禁止

-   不同步 voxel grid

### 同步内容

-   VoxelEvent
-   ChunkDiff

------------------------------------------------------------------------

## 8. Event Flow

Client Input → GAS Ability → Server RPC → VoxelEvent → Simulation →
ChunkDiff → Client Apply

------------------------------------------------------------------------

## 9. Chunk Diff

记录： - Segment add/remove - Material change - Column split/merge

------------------------------------------------------------------------

## 10. Interest Management

-   Active: 0--5 chunks
-   Warm: 5--10 chunks
-   Cold: \>10 chunks

------------------------------------------------------------------------

## 11. 性能预算

-   Active chunks: 50--150
-   Simulation: 2--5ms/frame
-   Replication: event-based

------------------------------------------------------------------------

## 12. 渲染系统

-   Chunk mesh rebuild
-   GPU instancing
-   Shader fluid/fire

------------------------------------------------------------------------

## 13. 风险控制

-   Simulation budget limit
-   Event batching
-   Chunk LOD
-   Interest filtering

------------------------------------------------------------------------

## 14. Dynamic Fragment / Triangle Body System

预设 2D 可破坏物体、地形或结构断裂后脱离世界的大块实体，不直接并入 Segment Column 主文档细节。

该系统作为独立模块处理：

-   detached solid region
-   contour extraction
-   triangulation
-   simplified collision body
-   server-authoritative fragment actor
-   fragment sleep / merge / destroy lifecycle

详细设计见：

-   `docs/UE_Dynamic_Fragment_Triangulation_System.md`

------------------------------------------------------------------------

## 15. UI 系统

项目运行时 UI 使用 Unreal Engine 的 MVVM（ModelViewViewModel）框架。

UI 架构原则：

-   View 只负责 UMG 展示和输入控件
-   ViewModel 只暴露 UI 所需状态和命令
-   Model 来自 PlayerState、GameState、Subsystem 或 replicated gameplay data
-   UI 不直接修改世界模拟、Chunk、Fragment 或 GAS 内部状态
-   UI 命令通过 PlayerController / Ability / Subsystem 发起请求，再由服务器权威处理

典型数据流：

Client Input / Replicated State → Model → ViewModel FieldNotify → UMG View

典型命令流：

UMG View → ViewModel Command → PlayerController / GAS Ability → Server Event

------------------------------------------------------------------------

## 16. 总结

系统核心：

> GAS = 输入层\
> DS = 世界真相\
> Network = Event + Diff\
> Chunk = 性能边界\
> Segment Column = 空间结构
