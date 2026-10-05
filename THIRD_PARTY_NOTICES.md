# THIRD_PARTY_NOTICES

本项目的执行底座与设计基线引用了以下开源项目。衍生分发须保留其版权与许可证声明（GPL-3.0 义务）。

| 上游 | 仓库 | 许可证 | 关系 | 采用内容 |
|------|------|--------|------|----------|
| MW_CpuTurboScheduler (CpuTurboScheduler) | github.com/MoWei-2077/MW_CpuTurboScheduler | GPL-3.0 | 间接上游（经 CoreTurboScheduler fork） | 调度参数表（conf/*.ini）、调度逻辑设计 |
| CoreTurboScheduler | github.com/Geometry1103/CoreTurboScheduler | GPL-3.0 | **直接上游（fork 基线，107 commits @ 2026-05-17）** | CPU/GPU 执行底座、场景识别、构建链（CMake/CI）、v4.1 修复 |
| COSMemory | github.com/wzcawzc123/COSMemory | GPL-3.0 | 本组织项目（origin: fork of OneB1ank/A1Memory） | Memory Controller：keepAlive/reclaim/freeze/guard/WebUI |
| A1Memory | github.com/OneB1ank/A1Memory | GPL-3.0 | COSMemory 的原始上游 | 内存管理基础实现 |

## 重要声明

- 上游 MW_CpuTurboScheduler 的 **release 二进制（v3.5）对应源码未公开**，与仓库源码不是同一套代码（见设计文档 §2.4）。本项目**不使用、不反编译、不包含**该二进制及其产物。
- 引用上游行为结论时，一律以仓库源码路径 + 目标设备实测为准，不以 README 描述为准。
- 本文件随每次依赖变更同步更新。

---
作者：是你吗薰儿
