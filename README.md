# cos智能调度与内存管理（UnifiedRootOptimizer）

> Android Root 系统级资源协同调度框架 —— **COSMemory × CoreTurboScheduler 统一策略引擎方案**
>
> 一个框架、多个执行器、一个策略大脑。

## 项目定位

不是把两个 Root 模块简单打包，而是建立统一的"系统资源策略编排层"：

- **Memory Controller**：基于 [COSMemory](https://github.com/wzcawzc123/COSMemory)（进程生命周期 / 内存压力 / 白名单保护 / 死亡归因）
- **CPU/GPU Controller**：基于 [CoreTurboScheduler](https://github.com/Geometry1103/CoreTurboScheduler)（CPUFreq / Governor / Cpuset / 核心在线 / GPU / 场景识别）
- **统一层**：Event Bus / State Manager / Policy Manager / Constraint Engine（约束求交仲裁 + generation/lease）

## 核心设计原则

1. **约束求交仲裁**：多方只提交期望值，唯一出口算最终值，杜绝 last-writer-wins 双写震荡
2. **写完即走**：策略边界事件触发一次写入即返回，禁止循环对写内核 governor（uag 共存铁律）
3. **游戏助手让权**：GAME 场景 Control Handover，全量交还控制权，不做 hook
4. **热控观测优先**：T-OBS 只读先行，T-INT 受限干预须设备实测放量
5. **失败归因**：任何回收/死亡动作必须有 action_id 证据链，证据不足输出 UNKNOWN
6. **可回滚**：dry-run → shadow → enforce 三阶段放量；baseline 快照 + lease 自动回落

## 仓库结构

```
docs/
├── UnifiedRootOptimizer_设计文档_开发计划_验收标准_v1.1.md   # 当前设计基线（修订版）
└── UnifiedRootOptimizer_设计文档_开发计划_验收标准_v1.0.docx  # 原始版本存档
THIRD_PARTY_NOTICES.md   # 上游来源与许可证声明
LICENSE                  # GPL-3.0
```

## 当前状态

- **v1.1 设计基线**（2026-10-05）：完成上游审计、目标设备能力初测（一加11 / SM8550 / ColorOS 16）、里程碑裁剪与 P0 分级
- 下一步：**M0 阶段 0** —— 节点能力矩阵 + 基线测量（只读探测）
- 0.x-dev 范围 = M0 + M1 + M3（精简版），GPU/热控闭环顺延

## 目标设备基线

| 项 | 值 |
|----|----|
| 设备 | 一加11 (PHB110) / SM8550 / ColorOS 16 |
| 内核 | 5.15.180-android13-8-o-01179 |
| Root | KernelSU 3.3.0（LKM 模式） |
| Governor | uag / walt / conservative / schedutil … |

## 合规

本项目基于 GPL-3.0 开源组件构建，衍生分发须保留上游版权与许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

---

**作者：是你吗薰儿**
