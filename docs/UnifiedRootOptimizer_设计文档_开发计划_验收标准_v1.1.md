# UnifiedRootOptimizer

## Android Root 系统级资源协同调度框架
### COSMemory × CoreTurboScheduler · 统一策略引擎方案

**文档属性**：版本 **v1.1**；状态：设计基线（修订版）；评审对象：架构、实现、测试、发布四条线。
**基线依据**：2026-10-05 对上游 MW_CpuTurboScheduler、CoreTurboScheduler fork 及目标设备（一加11 / ColorOS 16）的实测审计 + COSMemory v1.1.5 诊断基线。

---

## v1.1 修订记录

| # | 位置 | 修订内容 | 依据 |
|---|------|----------|------|
| R1 | §2.1 | 补充 CoreTurboScheduler fork 实测证据（CI、打包缺失、WebUI 移除、验证深度） | 2026-10-05 仓库实拉 + diff |
| R2 | §2.4（新增） | 记录上游"发布包二进制与仓库源码不一致"的审计结论 | release zip 二进制字符串比对 |
| R3 | §2.5（新增） | 目标设备能力初测基线（一加11 / SM8550 / ColorOS 16） | 设备只读探测 |
| R4 | §3.4 / §5.2（新增） | **写完即走铁律**：Controller 禁止在循环中与内核 governor 对写 | uag 自适应调速器存在，用户态高频写入必然震荡 |
| R5 | §5.5（新增） | **游戏助手让权（Control Handover）**：GAME 场景下策略引擎整体让权 | 冲突根治方案，替代 hook |
| R6 | §5.4 | 热控降级为"观测优先、干预后置" | Oplus thermal HAL 为 ROM 闭环，用户态干预受限且易冲突 |
| R7 | §9 | 里程碑裁剪：0.x 范围 = M0+M1+M3（精简版）；GPU/热控闭环后移 | 单开发者 + AI 协作的实际吞吐 |
| R8 | §9.1 | M0 首动作明确为**节点能力矩阵 + 基线测量**（阶段 0） | 先测量后立项原则 |
| R9 | §11.1 | P0 分级：P0-02/03/07 即刻硬闸，其余项提升至 1.0-RC 门槛 | 阶段性放量 |
| R10 | §14 | 风险矩阵新增：NDK 构建链缺失、双源不一致 | 环境与源码审计结论 |
| R11 | 附录 B | 参考资料链接修正为实测地址与 commit 数 | 核查修正 |

---

## 1. 执行摘要

本项目的目标不是把两个 Root 模块简单打包，而是建立一套统一的"系统资源策略编排层"。COSMemory 继续负责进程生命周期、内存压力和白名单保护；CPU/GPU 执行底座（基于 CoreTurboScheduler）继续负责 CPUFreq、Governor、Cpuset、核心在线状态、GPU 与启动加速。两者通过统一状态和策略接口协同工作。

**核心原则**：先统一决策，再统一执行；先建立可回滚的安全骨架，再扩展智能策略；任何单项控制失效都不应导致整个守护进程退出。

### 1.1 项目边界

- 面向 Root Android 设备的用户态资源策略框架，优先支持 ARM64 与常见 Linux/Android sysfs、procfs、cpuset 机制。
- 目标不是替代厂商内核调度器，也不承诺对所有 ROM/内核实现相同效果。
- 不把"应用永不死亡"作为承诺，重点是降低框架主动回收/误杀概率，并提供可解释的死亡归因。
- 所有策略必须有"检测 → 决策 → 应用 → 验证 → 回滚/降级"的闭环。
- **v1.1 新增**：框架的第一目标设备为一加11（SM8550 / ColorOS 16 / KSU 3.3.0 LKM），0.x 阶段不追求多设备通用性；通用性由 Capability Adapter 在设计上预留，由 Phase E 设备档案兑现。

### 1.2 设计结论

| 问题 | 结论 | 工程动作 |
|------|------|----------|
| 两个模块能否直接合并？ | 不建议 | 保留两个执行器，新增统一 Policy Manager |
| 是否要统一 App/场景识别？ | 必须 | 单一 State Manager，避免重复 dumpsys/重复 watcher |
| 是否需要策略冲突仲裁？ | 必须 | Constraint Engine + policy generation |
| 是否需要热控闭环？ | 必须，但**观测优先、干预后置**（v1.1 R6） | 先建只读 Thermal Telemetry，干预限于 clamp 上限且需设备实测放量 |
| 是否要解释"QQ 为什么死"？ | 必须 | Death Attribution + evidence chain |
| 是否保留 COSGuard？ | 保留 | 作为进程生命周期旁路防线，不并入 CPU daemon |
| 用户态与内核 governor 如何共存？ | **写完即走**（v1.1 R4） | Controller 禁止高频循环对写频率节点 |
| 与游戏助手冲突如何根治？ | **场景让权**（v1.1 R5） | GAME 场景 Control Handover，不做 hook |

---

## 2. 当前基线与审计结论

本章把公开仓库状态与设备实测转成工程基线。它不是发布认证，而是后续改造时的"已知起点"。

### 2.1 CoreTurboScheduler 现状

仓库：`Geometry1103/CoreTurboScheduler`，fork 自 `MoWei-2077/MW_CpuTurboScheduler`，GPL-3.0，**107 commits**（2026-10-05 实测核对一致），创建于 2026-05-14，最后推送 2026-05-17。

**v1.1 实测补充（R1）**：

| 维度 | 实测事实 | 工程含义 |
|------|----------|----------|
| 构建链 | 新增 CMakeLists.txt + build.sh + GitHub Actions CI，**5 次构建全绿** | 上游"作者本机硬编码路径不可复现"问题已修复；M0 可直接继承 |
| 编译修复 | `open(O_CREAT)` 缺 mode、FreqWriter Policy 命名空间遮蔽 | 有真实编译验证 |
| 死代码 | 已删除上游编不过的 `Demo/controller.hpp` 与重复 `utils.hpp` | 上游已知编译障碍已清 |
| 新能力 | `SceneDetector.hpp` 场景识别（含 game_heavy）、GPU 守护（CTS_GPU v4.0）、多设备 config（hm/scx/std） | 与文档设想的 CPU/GPU 执行底座吻合 |
| v4.1 修复 | CHANGES_v4.1.md 记录 6+ 真问题：JSON 严格类型致配置静默失效、`currentMatch` 无锁并发、**退出游戏大核不关**、配置重载重复启动守护、GPU 每 3s 无条件重写（改为缓存+变化才写+15s 兜底） | 最后一条与本项目"写完即走"铁律同源，方向正确 |
| **打包** | **仍无 magisk/ 模块目录，无任何 release/tag** | 它目前是"能编译的源码"而非"能刷的模块"，打包链路须自建（见 M0） |
| 验证深度 | CI 仅证明编译通过；三天 AI 辅助开发（PR 分支 `claude/*`），零设备实测证据 | §2.1 的谨慎判断维持："必须以实际代码路径和设备实验结果为准" |
| WebUI | fork 已删除上游 webroot 三件套 | 统一框架若需面板，沿用 COSMemory 面板体系重建 |

**审计判断**：CoreTurboScheduler 适合作为 CPU/GPU 执行底座（可编译、有场景识别、修复记录真实），但不可将 README 中的"智能热控"视为已形成完整闭环。

### 2.2 COSMemory v1.1.5 现状

现有诊断包显示：COSMemory v1.1.5 运行于 OnePlus/ColorOS 16 基线设备，Linux 5.15.180 Android 内核；当前能力包括 keepAlive、reclaim、freeze、guard、tuning、日志、WebUI，以及 COSGuard 的 system_server/LSPosed 旁路能力。

诊断中可以直接看到 KEEPADJ 动作（如对微信和 QQ 调整 oom_adj/oom_score_adj），以及 RECLAIM、PASS_NO_RULE、BLOCK 等可归因日志。该证据说明系统已具备"执行 + 记录"基础，但也暴露了事件重复、规则解释和死亡原因判定问题。

| 现有机制 | 价值 | 融合后的归属 |
|----------|------|--------------|
| KEEPADJ / 白名单 | 保护重点进程 | Memory Controller |
| RECLAIM / PSI / MemAvailable | 释放内存 | Memory Controller |
| FREEZE | 后台节流 | Memory Controller |
| COSGuard | 拦截部分 system_server 侧 kill 路径 | Guard Adapter |
| CPUFreq / Governor | 频率控制 | CPU Controller |
| Cpuset / CoreOnline | 资源分配 | CPU Controller |
| GPUFreq / Boost | 图形资源调节 | GPU Controller |
| 场景 / AppProfile | 策略输入 | 统一 State/Policy 层 |

### 2.3 明确的已知问题

- 事件采集存在重复建设风险：多个 watcher/polling task 都可能获取前台 App、屏幕状态、配置变化。融合后必须收敛为单一事件总线。
- CoreTurbo 中某些低级监控失败路径不应直接导致整个 scheduler 退出；必须改成降级模式。
- GPU/sysfs 写入必须限制副作用，尤其避免不必要的权限放宽。
- 热控不能停留在变量或接口预留；但在 Oplus ROM 上必须先解决"能否安全干预"（见 §5.4 降级决策）。
- 当前"进程消失"不能直接等同于"系统杀进程"；需要证据链和死亡归因。

### 2.4 上游发布包与仓库源码差异（v1.1 新增，R2）

对 `MoWei-2077/MW_CpuTurboScheduler` 的 release 二进制与仓库源码比对结论：

- release v3.5 的 `MW_CpuSpeedController`（1.1MB 静态 stripped ELF）内**不含** `config.json` / `mode.txt` 字符串，配置接口为 `config.ini` + `MW_CpuSpeedController/config.txt`；仓库源码则围绕 `CTS/mode.txt` + `config.json` 构建——**两者不是同一套代码**。
- 仓库缺少 `magisk/` 打包目录（git 历史从未提交）；release 内含而仓库删除的 SoC 配置有 MTK9000/9200/9400Plus。
- **工程结论**：可用于参考的只有**参数表（conf/*.ini）与公开源码的调度逻辑**；release 二进制行为未审计、不作为本项目执行底座。CoreTurboScheduler fork 基于仓库源码线，属于"新一代架构"，与 release 二进制无对应关系——引用其行为时必须以实际代码路径为准。

### 2.5 目标设备能力初测基线（v1.1 新增，R3）

| 项 | 实测值 |
|----|--------|
| 设备 / SoC | 一加11 (PHB110) / SM8550 / kalama |
| 内核 | 5.15.180-android13-8-**o-01179**-g2b658b8c77d0（Oplus 定制，2026-07 构建） |
| Root | KernelSU 3.3.0，**LKM 模式**（kernelsu.ko 运行时加载，boot 未改动） |
| CPU 簇 | policy0 / policy3 / policy7（与 SDM8G2 参数表结构吻合） |
| 可用 governor | **uag**、walt、conservative、powersave、performance、schedutil |
| 频率上限 | 1785600 / 2803200 / 3187200 kHz |
| cpuset | `/dev/cpuset` 存在（audio-app/background/… 分组齐全） |
| **缺失节点** | `policyX/scheduler/`（不存在）、`/dev/stune`（无）、`perfmgr` Feas 接口（无）、`cpu_boost` 模块（无） |
| 厂商模块 | 527 个已加载（msm_drm、触摸、相机等），vermagic 挂 5.15.180-o-01179 |
| 待测（M0 补齐） | PSI(`/proc/pressure/*`)、thermal zone 节点、KGSL GPU 节点、top-app cgroup 读取通路、电流/功耗采样点 |

**含义**：上游参数表中约三成节点在本机不存在；Capability Adapter 的 skip/fallback 不是可选项，是必需项。

---

## 3. 总体架构设计

系统采用六层结构：事件采集层、全局状态层、策略编排层、资源执行层、内核适配层、遥测与反馈层。COSMemory 和 CoreTurboScheduler 不在同一层争夺决策权。

```
用户/系统事件 → Event Collector → State Manager → Policy Manager
    → Constraint Engine → Memory Controller / CPU Controller / GPU·Thermal Controller
    → Kernel Adapter (sysfs/procfs/cpuset/guard bridge) → Linux Kernel / SoC
    → Telemetry → Evidence → Feedback → State Manager
```

### 3.1 分层职责

| 层 | 职责 | 严禁承担的职责 |
|----|------|----------------|
| Event Collector | 采集原始事件，去抖，时间戳 | 直接修改 CPU/内存策略 |
| State Manager | 保存唯一全局状态 | 执行 sysfs/procfs 写入 |
| Policy Manager | 把状态转成逻辑策略 | 直接调用 shell 命令 |
| Constraint Engine | 合并 App/场景/thermal/battery/kernel 上限 | 自行采集系统状态 |
| Controller | 执行具体资源策略，验证成功 | 改变高层决策 |
| Kernel Adapter | 处理设备差异与节点路径 | 承担业务逻辑 |
| Telemetry | 记录执行结果与证据链 | 反向直接写策略 |

### 3.2 统一状态模型

```cpp
struct GlobalState {
    string foregroundPackage;
    Scene scene;
    ScreenState screen;
    bool charging;
    int cpuLoad;
    double memAvailableMb;
    double psiSome10;
    MemoryPressure memoryPressure;
    ThermalState thermal;
    TouchState touch;
    uint64_t generation;
    uint64_t timestampMs;
};
```

### 3.3 场景模型

| 场景 | 触发条件示例 | 内存策略 | CPU/GPU 策略 |
|------|--------------|----------|--------------|
| BOOT | 开机完成后稳定期 | 保守保护关键进程 | Balance |
| DAILY | 普通前台使用 | 正常回收 + 白名单保护 | Balance |
| GAME | 识别到游戏包名/游戏场景 | 保护游戏，谨慎回收 | **Control Handover（§5.5）** |
| VIDEO/CAMERA | 视频播放/拍摄 | 减少非关键后台干扰 | 稳定性能，不做过激 core off |
| MEMORY_PRESSURE | MemAvailable/PSI 达阈值 | 提升回收优先级，绕过保护项 | 性能可适度降级以维持系统稳定 |
| THERMAL | 超过热阈值 | 保持前台，削减后台压力 | 按 thermal budget clamp（观测优先） |
| SCREEN_OFF | 熄屏稳定后 | 深度后台管理 | PowerSave / 低唤醒 |

### 3.4 与内核调速器共存：写完即走铁律（v1.1 新增，R4）

目标设备 governor 为 **uag**（Oplus 自适应调速器，毫秒级负载闭环）。用户态任何高频（≥100ms 级）频率写入都会与其形成双写震荡，是策略失稳的最大单一来源。铁律：

1. Controller **只在策略边界事件**（场景切换、generation 变更、thermal 档位变化）触发时写入频率/governor，写完即返回；
2. **禁止**常驻循环中轮询负载并覆写 `scaling_max_freq` 等节点；
3. 频率微调权归内核 governor；用户态只提交"上限/下限/参数集"这类**边界条件**；
4. 若确需持续干预（如 LaunchBoost TTL），必须带租约到期回落 + 与 uag 行为对照的实测验收；
5. 违反本铁律的功能不得进入 enforce 阶段。

---

## 4. 核心接口与数据契约

第一版不追求微服务化，所有组件仍可编译进同一个守护进程/服务。但接口必须先抽象，以便未来拆分进程或替换执行器。

### 4.1 EventBus

```cpp
enum class EventType {
    ForegroundChanged, ScreenChanged, TouchChanged,
    MemoryPressureChanged, ThermalChanged, ChargerChanged,
    ConfigChanged, ModeChanged, ProcessDeath, ControllerFault
};
struct Event {
    EventType type;
    uint64_t ts;
    uint64_t generation;
    std::string source;
    std::string payload;
};
```

### 4.2 Policy 输出契约

```cpp
struct MemoryPolicy {
    int protectedAdj;
    bool reclaimEnabled;
    bool freezeEnabled;
    int maxKillPerRound;
    std::vector<std::string> protectedPackages;
};
struct CpuPolicy {
    std::string governor;
    int minFreq;
    int maxFreq;
    std::vector<int> onlineCores;
    std::string cpusetProfile;
    bool launchBoost;
};
struct GpuPolicy {
    int minFreq;
    int maxFreq;
    bool boost;
};
struct EffectivePolicy {
    MemoryPolicy memory;
    CpuPolicy cpu;
    GpuPolicy gpu;
    uint64_t generation;
    uint64_t leaseUntilMs;
};
```

### 4.3 策略冲突仲裁

必须采用约束求交，不采用"最后写入者生效"。例如游戏请求 CPU max=3.2GHz、thermal 上限 2.4GHz、kernel 上限 3.0GHz，则最终 effective_max = 2.4GHz。所有模块只提交"期望值/约束"，最终值由 Constraint Engine 计算。

```
effectiveMax = min(
    appRequestedMax,
    sceneRequestedMax,
    thermalMax,
    batteryMax,
    kernelSupportedMax
);
```

### 4.4 Policy Generation / Lease

- 每次全局策略变化生成单调递增 generation，Controller 只允许应用最新 generation。
- 关键策略带 lease/TTL，Controller 崩溃或策略过期时自动恢复安全默认值。
- 策略应用必须可重复执行（idempotent），重复写入不能造成状态漂移。
- 配置热重载先 parse → validate → compile policy → dry-run → commit，禁止半成品配置覆盖当前运行态。

---

## 5. 模块设计

### 5.1 Memory Controller（基于 COSMemory）

| 子模块 | 保留/重构 | 设计要求 |
|--------|-----------|----------|
| KeepAlive | 保留 | 白名单、protected state、写入前后校验 |
| Reclaim | 保留 | PSI + MemAvailable 双条件，cooldown，maxKillPerRound |
| Freeze | 保留 | 只操作明确允许的后台进程，支持 dry-run |
| Process Snapshot | 统一由 Event Collector 共享，减少重复扫描 |
| COSGuard Bridge | 保留 | 作为 system_server 侧防线，与主守护解耦 |
| Kill/Death Attribution | 新增 | 任何回收/死亡动作必须记录证据链 |

### 5.2 CPU Controller（基于 CoreTurboScheduler）

| 子模块 | 设计要求 |
|--------|----------|
| CPUFreq | 节点探测、能力缓存、写前校验、写后 readback；**受 §3.4 写完即走铁律约束** |
| Governor | 支持设备能力探测；本机为 uag 时默认**不切换**，仅在能力矩阵确认后放量；缺失 governor 时跳过，不导致主进程退出 |
| Cpuset | 统一 profile 编译；foreground/top-app 等路径由 Adapter 管理 |
| CoreOnline | 必须有现场状态快照和恢复快照，场景退出一定 restore（针对 v4.1 已修复的"退出游戏大核不关"，须有回归测试固化） |
| LaunchBoost | 事件驱动 + TTL；防止重复启动/延迟回收；**必须受 Control Handover 状态门控** |
| Scheduler tunables | 按能力矩阵应用；本机 `policyX/scheduler/` 不存在 → 整类 skip + telemetry |

### 5.3 GPU Controller

- KGSL 节点必须动态探测，不允许写死单一路径。
- 频率不变化不重复写；后台必须有较低写频率（继承 v4.1 的缓存+变化才写+15s 兜底策略）。
- 禁止无必要地将 sysfs 节点权限放宽到 0666；权限失败应记录并降级。
- GPU boost 必须纳入 thermal budget，不能独立于温控运行。

### 5.4 Thermal Controller（v1.1 降级：观测优先、干预后置，R6）

**降级理由**：ColorOS 16 的热控是 Oplus thermal HAL + 内核热区的 ROM 级闭环；用户态直写 thermal 节点大概率被 SELinux 拒绝、或与原厂策略形成对抗震荡。

分两阶段：

- **阶段 T-OBS（0.x 默认，必须先做）**：只读采集 thermal zone → 进入 GlobalState 与 Telemetry，**不做任何写入**。产出热画像数据，验证传感器可靠性。
- **阶段 T-INT（需设备实测放量，逐项开关）**：仅通过 Constraint Engine 输出 CPU/GPU max clamp 与 boost 禁用（写的是 cpufreq 上限这类本就归我们的节点），不碰 thermal zone 本体；传感器读数失效（恒定/断供，参考 lux=-1 案教训）→ 立即 fail-safe 回 T-OBS。

热档位定义保留：

```
< T1              NORMAL
T1..T2            WARM
T2..T3            HOT
>= T3             CRITICAL
每档输出：CPU max clamp / GPU max clamp / background memory pressure / boost allow / recovery hysteresis
```

### 5.5 游戏助手让权 Control Handover（v1.1 新增，R5）

**问题**：Oplus 游戏助手（Game Space）的性能调度与本框架写入同一批 cpufreq/GPU 节点，双写必然打架（表现为：游戏助手性能模式失效，或本框架被 boost 顶掉后回写形成震荡）。

**决策**：不做 hook，采用场景让权：

```
场景识别 = GAME（游戏包名名单 或 游戏助手激活信号）
  → Policy Manager 输出 handover=true
  → Memory Controller：仅保留白名单保护，暂停主动 reclaim（谨慎模式）
  → CPU/GPU Controller：撤销本框架全部上限与参数写入，
     进入 baseline 快照态，控制权整体交还游戏助手与内核
  → 本框架降级为观察者（Telemetry 继续记录，不写入）
退出游戏
  → 恢复 baseline 快照 → 重新提交场景策略（generation+1）
```

要点：

1. 让权是**全量让权**，不是"互相协商"——协商等于打架；
2. 前台包名/游戏名单检测是 State Manager 的基础能力，增量成本≈0；
3. 游戏助手行为指纹采集列入 M0（进/出游戏前后 sysfs 快照 diff + logcat），作为让权范围的实测依据；
4. 若实测发现让权后仍有残余冲突，再评估 hook 方案——hook 是最后手段，不是首选（脆弱、随系统更新失效）。

### 5.6 Death Attribution

这是本项目针对实际使用问题最值得新增的能力。它不承诺"阻止所有死亡"，而是把死亡变成可解释事件。

| 归因 | 证据来源 | 发布要求 |
|------|----------|----------|
| COSMemory_RECLAIM | 回收日志 + PID/package + action id | 必须可 100% 对应 |
| COSGuard_BLOCKED | guard telemetry + hook event | 必须可追溯 |
| SYSTEM_LMKD | 系统日志/LMKD 信息 + 时间窗口 | 无法确认时不得伪造 |
| AMS_KILL | system_server/AMS 证据 | 需要时间窗口关联 |
| CRASH | logcat tombstone/异常 | 需支持 unknown fallback |
| APP_SELF_EXIT | 只能在证据足够时标记 | 证据不足必须为 UNKNOWN |

---

## 6. 运行时链路

### 6.1 前台 App 切换

```
ForegroundChanged(package=A)
  → StateManager.update()
  → SceneDetector => GAME?
  → PolicyManager.evaluate()
  → ConstraintEngine.resolve()
  → Commit generation=42
     ├─ MemoryController.apply(memoryPolicy)
     ├─ CpuController.apply(cpuPolicy)
     └─ GpuController.apply(gpuPolicy)
  → Readback / verify
  → Telemetry(success/failure)
```

### 6.2 内存压力链路

```
PSI rises + MemAvailable below floor
  → MemoryPressure=HIGH
  → PolicyManager switches reclaim strategy
  → Candidate filter: protected / foreground / leased / recently restored -> skip
  → reclaim depth + maxKillPerRound
  → execute
  → record action_id + target + reason + result
```

### 6.3 温度升高链路（v1.1 更新）

```
ThermalSensor (T-OBS 采集)
  → ThermalState=WARM/HOT/CRITICAL
  → ConstraintEngine（T-INT 放量后才产生写入）
  → clamp CPU max / GPU max / disable sustained boost
  → keep foreground app protection
  → monitor recovery with hysteresis
（T-OBS 阶段：到 StateManager 为止，无写入）
```

---

## 7. 进程、线程与可靠性设计

目标：避免"每个功能自己起线程、自己读状态、自己写策略"。统一框架应尽量减少常驻轮询，并把高频事件变成事件驱动。

| 组件 | 建议线程/机制 | 备注 |
|------|----------------|------|
| Main Runtime | 1 | 生命周期和故障恢复 |
| Event Collector | 1~2 | 统一输入；inotify/netlink 优先，polling 兜底 |
| Policy Worker | 1 | 串行生成策略，避免竞争 |
| Controller Worker | 1 | 串行提交 sysfs/procfs，集中仲裁 |
| Telemetry | 1 低频 | 批量写日志/指标 |
| COSGuard | 独立 | LSPosed/system_server 生命周期，不进入主进程线程池 |

### 7.1 所有故障的统一等级

| 等级 | 行为 |
|------|------|
| FATAL | 仅配置损坏、内存安全错误等无法恢复情况；受控退出并让宿主拉起 |
| DEGRADED | 节点缺失、inotify 失败、某个 governor 不存在；关闭对应能力，主服务继续运行 |
| RETRY | 临时 I/O/权限异常；指数退避，限制重试频率 |
| IGNORE | 无效事件、重复事件、过期 generation |

---

## 8. 安全与权限设计

- 配置输入、包名、路径、命令参数必须禁止 shell 拼接；优先使用直接文件 I/O 和 execve/固定 argv。
- sysfs/procfs 写入只能进入白名单节点；Adapter 不允许任意路径写入。
- 所有 Root 操作必须有 source=module/component 标签，便于审计。
- 避免 chmod 0666；对必须调整权限的节点只允许最小必要模式，并在离开策略时恢复。
- WebUI 与本地控制接口必须绑定本机、认证或随机 token；默认不监听 0.0.0.0。
- 配置更新采用原子 rename；失败配置不能破坏 last-known-good。
- 日志必须限制大小并禁止记录敏感内容；错误信息中避免泄露完整命令行或路径中的用户数据。

### 8.1 安全验收阻断项

| 阻断项 | 通过标准 |
|--------|----------|
| 命令注入 | 静态检查 + Fuzz 配置，不存在可控命令拼接 |
| 任意路径写 | Adapter 仅允许注册路径集合 |
| 权限扩大 | 无无理由的 0666/777；有审计记录 |
| 远程控制面 | 默认仅本地；未认证接口不得暴露网络 |
| 配置回滚 | 坏配置不会让主服务进入不可恢复状态 |

---

## 9. 开发计划（v1.1 裁剪版，R7/R8）

**0.x-dev 范围 = M0 + M1 + M3（精简版）**。M2 的 GPU/Thermal Controller、M4 的完整观测体系、M5 性能闭环顺延为后续里程碑（对应 §15 Phase C~E）。每个里程碑都有独立可运行版本，避免一次性重写导致无法回滚。

| 里程碑 | 建议周期 | 交付物 | 关键动作 | 退出条件 |
|--------|----------|--------|----------|----------|
| **M0 基线冻结** | 1~2 天 | 节点能力矩阵 + 基线测量报告 + 回滚脚本 | **首动作 = 阶段 0 探测**（见 9.1）；构建链（NDK/CMake）验证；打包链路自建 | 不改变现有策略；能力矩阵覆盖全部计划写入路径 |
| **M1 统一运行时** | 3~5 天 | EventBus + StateManager + Runtime | 统一 App/屏幕/配置事件，移除重复 watcher | 行为等价于原版本 |
| M2′ Controller 抽象（内存 + CPU） | 4~7 天 | Memory/CPU Controller 接口 + Adapter | COSMemory/CoreTurbo 接入；GPU/Thermal 仅留接口占位 | 所有缺失节点均可降级 |
| **M3 Policy + Constraint（精简）** | 5~8 天 | PolicyManager + ConstraintEngine + generation/lease | 实现 DAILY/GAME（含 Handover）/POWERSAVE/MEMORY_PRESSURE | 无策略震荡、无互相覆盖 |
| M4 可靠性与可观测 | 4~7 天 | Death Attribution + telemetry + watchdog | 故障归因、回滚、审计日志 | 关键故障可解释 |
| M5 热控与性能闭环 | 7~14 天 | Thermal T-INT 放量 + LaunchBoost TTL + FPS 指标 | 设备实验、功耗/性能验证 | 达到发布阈值 |

### 9.1 M0 基线冻结清单（R8 更新）

1. **首动作：节点能力矩阵 + 基线测量（阶段 0，只读）**
   - 探测：cpufreq 全参数、uag/walt 可调节点、PSI、thermal zone、KGSL、cpuset/cpuctl、top-app 通路、电流采样点；
   - 与上游 SDM8G2 参数表逐条对表，产出"可用 / 需改 / 缺失"清单；
   - 游戏助手行为指纹：进/出游戏前后 sysfs 快照 diff + logcat；
   - 基线测量：日常 2h、游戏 30/60min 的帧率-温度-频率轨迹、待机耗电构成。
2. 固定一台主测试机作为 golden device（一加11），后续扩展兼容设备。
3. 保存 ROM/内核版本、CPU cluster、cpufreq policy、governor、cpuset、KGSL、PSI、swap/ZRAM 能力清单。
4. 固定 COSMemory 当前配置与白名单；默认名单中所有 KILL 规则继续遵循"逐条实测，未测不发布"。
5. 建立一键卸载、一键恢复原生策略、一键收集诊断包。
6. **构建链验证**：安装 Android NDK，跑通 CMake+clang 的 aarch64 静态构建与 CI 等价产物（R10 关联风险）。

### 9.2 M1-M3 开发原则

- 先实现 **dry-run**：策略只计算不写入，用于观察冲突和场景切换。
- 再实现 **shadow mode**：策略实际计算并记录"如果执行会怎样"，继续不写入。
- 最后切到 **enforce**：每个 Controller 单独开关，逐项放量。

---

## 10. 测试与验证体系

### 10.1 自动化测试

| 层级 | 覆盖内容 | 门槛 |
|------|----------|------|
| Unit | JSON、策略合并、constraint、generation、hysteresis | 关键模块覆盖率 ≥ 80% |
| Property/Fuzz | 配置类型、包名、路径、异常数值 | 持续 1e5 级输入不崩溃 |
| Integration | Controller → Adapter → fake sysfs | 成功/失败/节点不存在全覆盖 |
| Runtime | 事件风暴、重复事件、配置热重载 | 无死锁、无线程泄漏、无异常退出 |
| Soak | 24h / 72h 稳定性 | 无累计线程、内存、日志泄漏 |

### 10.2 设备实验矩阵

| 场景 | 操作 | 观测指标 | 必须通过 |
|------|------|----------|----------|
| 日常 | 微信/QQ/浏览 2h | App 存活、CPU、温度、功耗 | 无模块主动误杀 |
| 游戏冷启动 | 启动→进入主界面→退出 | 启动时间、Boost、核心状态 | 退出后全部恢复 |
| 游戏长时 | 30~60min | FPS/FrameTime、温度、频率 | 无持续过热失控 |
| **游戏助手共存** | 开启游戏助手性能模式游玩 | 帧率、双方写入日志 | **让权生效，无双写震荡（v1.1 R5）** |
| 低内存 | 人为制造内存压力 | PSI、MemAvailable、reclaim | 保护项不被模块主动回收 |
| 熄屏 | 30~60min | 频繁唤醒、CPU、功耗 | 进入低功耗状态 |
| 配置热重载 | 连续替换 valid/invalid JSON | generation、错误日志 | 坏配置不影响旧策略 |
| 控制器故障 | 隐藏 governor/sysfs/inotify | 降级状态 | 主服务继续运行 |
| 恢复 | 卸载/重启/服务崩溃 | 原生节点状态 | 资源回到 safe baseline |

---

## 11. 最终验收标准

以下标准用于判断"可以发版"，不是"功能大概能用"。数值阈值是工程目标值，具体设备若无法可靠测量，必须记录方法和理由，不得用主观感觉替代。

### 11.1 P0 发布阻断项（v1.1 分级，R9）

**即时硬闸（0.x-dev 起任何时刻不得违反）：**

| 编号 | 类别 | 标准 | 量化门槛 |
|------|------|------|----------|
| P0-02 | 策略回滚 | 游戏/高性能/热控策略退出后，CPU/GPU/CoreOnline/Cpuset 回到对应 baseline | 100% 场景通过 |
| P0-03 | 保护项安全 | 白名单进程不会被 Memory Controller 主动 KILL/RECLAIM；每次动作可追踪 | 0 次模块误杀 |
| P0-07 | 死亡归因 | 模块主动动作均有 action_id；无证据不得声称 APP_SELF_EXIT | 审计链完整 |

**1.0-RC 前必须关闭：**

| 编号 | 类别 | 标准 | 量化门槛 |
|------|------|------|----------|
| P0-01 | 服务稳定 | 任一非 FATAL 的节点/监控失败不得导致主服务退出 | 0 次异常退出 |
| P0-04 | 策略仲裁 | 不存在 Controller 之间互相覆盖形成频繁震荡 | 30min 测试无循环震荡 |
| P0-05 | 配置安全 | 坏 JSON、缺字段、错误类型不能让服务进入坏状态 | 100% 回归通过 |
| P0-06 | 权限安全 | 不存在任意路径写、命令注入和无理由大范围 chmod | 安全审计 0 Critical |
| P0-08 | 卸载恢复 | 模块卸载/服务停止后无残留强制策略 | 100% baseline 恢复 |

### 11.2 P1 质量门槛

| 编号 | 类别 | 标准 | 量化门槛 |
|------|------|------|----------|
| P1-01 | 模式切换 | 前台 App/场景切换后策略收敛，不产生明显抖动 | ≤ 500ms 目标 |
| P1-02 | 服务资源 | 主服务常驻资源可控 | RSS ≤ 16MB 目标；峰值 ≤ 24MB |
| P1-03 | 空闲开销 | 熄屏稳定后守护开销很低 | 设备基线对比 CPU/唤醒无显著回归 |
| P1-04 | 日志 | 不产生无限增长日志 | 单文件 ≤ 10MB，自动滚动 |
| P1-05 | 热控 | 超过上限后性能资源按 budget 限制，并可恢复 | 无持续 thermal runaway |
| P1-06 | 长期稳定 | 持续运行无线程/FD/内存泄漏 | 24h 必过；72h 建议必过 |

### 11.3 P2 优化目标

- 加入 FPS/FrameTime 或 SurfaceFlinger 指标后，对游戏进行 workload-adaptive 调度，而非只依赖静态 AppProfile。
- 建立设备级 capability matrix 和默认 profile 自动生成，减少人工配置。
- 建立"实际性能 / 功耗 / 温度"三目标评分，逐设备做策略参数优化。

---

## 12. 发布、回滚与版本治理

### 12.1 版本策略

| 版本 | 内容 | 是否允许生产用户 |
|------|------|------------------|
| 0.x-dev | 架构改造、dry-run、实验策略 | 仅开发机 |
| 0.x-beta | 核心 Controller 稳定、限定设备 | 小规模测试 |
| 1.0-RC | P0/P1 全部通过 | 候选用户 |
| 1.0 | P0/P1 关闭、回滚可靠、至少 3 类设备 | 正式发布 |
| 1.x | 新增策略只增量发布，默认保守 | 按变更级别 |

版本号遵循语义三段 `大.中.小` 与 versionCode +1 惯例；展示层版本号从单一源头（module.prop 等）动态注入。

### 12.2 回滚机制

```
rollback order:
1. stop new policy evaluation
2. restore current generation baseline snapshot
3. disable boost / aggressive policy
4. restore CPU/GPU/core/cpuset state
5. restore memory policy
6. verify readback
7. mark DEGRADED
8. keep telemetry alive
```

### 12.3 Release Gate

- 源代码、配置、构建脚本、LICENSE、NOTICE/来源说明同步提交。
- 变更日志必须区分：功能、修复、行为变化、已知限制、回滚说明。
- 每个 Release 附带 capability report、测试设备表、关键指标结果、已知问题。
- 出现任何 P0 回归，自动退回上一个 stable policy bundle。

---

## 13. 开源与合规

- CoreTurboScheduler（`Geometry1103/CoreTurboScheduler`）标注 GPL-3.0，明确说明其为 `MoWei-2077/MW_CpuTurboScheduler` 的 Fork。以其代码为基础继续分发，必须保留上游版权、许可证与源代码提供义务。
- COSMemory v1.1.5 标注 GPLv3（origin: fork of OneB1ank/A1Memory）。
- 上游 MW release 二进制对应的源码未公开（§2.4）——**不得将 release 二进制或其反编译产物纳入本项目**。
- 工程要求：不得因"内部 Fork"删除上游版权与许可证说明；仓库维护 `THIRD_PARTY_NOTICES.md`，记录 upstream、commit、修改点和许可证。

---

## 14. 风险矩阵（v1.1 更新，R10）

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| 厂商内核节点不一致 | 高 | 高 | Capability Adapter + skip/fallback + device matrix（§2.5 已实证三成节点缺失） |
| 策略震荡导致耗电/卡顿 | 中 | 高 | generation + hysteresis + minimum dwell time + **写完即走铁律（§3.4）** |
| 与游戏助手双写冲突 | 高 | 高 | **Control Handover 全量让权（§5.5）** + 行为指纹实测 |
| 热控失效 | 中 | 极高 | T-OBS 先行；传感器失效 fail-safe；干预仅限 clamp |
| 白名单误判 | 中 | 高 | 默认保守、证据链、shadow mode、逐条实测 |
| sysfs 权限/SELinux | 高 | 中 | 能力探测与降级，禁止粗暴 chmod |
| 线程/轮询开销 | 中 | 中 | 统一 event collector，低频 telemetry |
| 更新后回滚失败 | 低 | 极高 | baseline snapshot + startup restore + release rollback test |
| **NDK 构建链缺失** | 高 | 中 | M0 显式安装验证；CI 等价产物兜底；必要时 M1 原型先行 shell 化 |
| **上游源码与发布包双源不一致** | 已证实 | 中 | 只引用仓库源码线；行为结论必须以代码路径 + 设备实验为准（§2.4） |

---

## 15. 未来路线图

- **Phase A**：Unified Runtime，先解决"两个系统各自判断、各自监听"。
- **Phase B**：Constraint Engine，把 CPU/GPU/内存/温度统一成约束问题。
- **Phase C**：Thermal Budget，从 T-OBS 升级到 T-INT 持续预算。
- **Phase D**：Workload Adaptive，引入 FrameTime、触控、CPU/GPU util 等真实 workload 指标。
- **Phase E**：Device Profiles，自动生成不同 SoC/ROM 的安全策略上限。
- **Phase F**：解释系统，把每一次性能或回收决策变成可读的 evidence chain。

---

## 16. 全局审查结果

本次文档级全局审查以"架构一致性、模块边界、失败模式、安全、可观测、测试、发布"七个维度执行。审查重点是确认：没有一处把 COSMemory/CPU Scheduler 当成彼此的依赖，也没有让 Controller 反向控制 Policy。

| 维度 | 结论 | 审查说明 |
|------|------|----------|
| 架构边界 | 通过 | 两个执行器独立，统一层只负责状态和策略 |
| 状态源 | 通过 | 唯一 StateManager，禁止重复 App/Screen 状态源 |
| 策略仲裁 | 通过 | constraint + generation，不用 last-writer-wins |
| 故障处理 | 通过/需落地 | DEGRADED/RETRY/IGNORE 已定义；CoreTurbo 低级监控失败须按此重构 |
| 热控 | 部分完成 | T-OBS 设计完成；T-INT 待设备实测放量（v1.1 降级决策） |
| 死亡归因 | 部分完成 | COSMemory 已有动作日志，待统一 action_id/evidence chain |
| 内核共存 | **通过设计（v1.1）** | 写完即走铁律 + Control Handover 已入设计 |
| 安全 | 通过设计 | 命令注入、任意路径写、权限扩大进入安全门禁 |
| 测试 | 通过 | 单元、集成、Soak、场景、**游戏助手共存**、回滚全覆盖 |
| 发布 | 通过 | P0 分级 + P1/P2 与版本门禁已定义 |

**最终判断**：当前最值得先做的不是再堆几个"性能功能"，而是完成 **M0 能力矩阵与基线测量 → Unified Runtime → Policy/Constraint Engine → 可靠回滚**。做完这几件事，COSMemory 与 CoreTurboScheduler 才真正成为一个系统，而不是两个模块并排运行。

---

## 附录 A：工程验收 Checklist

| 序号 | 检查项 | 状态 |
|------|--------|------|
| 1 | 所有 controller 支持 capability probe | 待验收 |
| 2 | 所有控制写入均有 readback 或明确不可读说明 | 待验收 |
| 3 | 所有策略都有 generation | 待验收 |
| 4 | 所有高风险策略都有 TTL / lease | 待验收 |
| 5 | 任一单项节点缺失不会导致主服务退出 | 待验收 |
| 6 | 退出游戏后 CoreOnline/Cpuset/Freq/GPU 全量恢复 | 待验收 |
| 7 | 白名单永远不会进入本模块主动 reclaim/kill 候选 | 待验收 |
| 8 | 配置热重载采用 last-known-good 原子提交 | 待验收 |
| 9 | 日志有大小上限、滚动和错误等级 | 待验收 |
| 10 | Death Attribution 在证据不足时输出 UNKNOWN | 待验收 |
| 11 | WebUI 默认不暴露公网 | 待验收 |
| 12 | 删除/卸载后恢复原始策略 | 待验收 |
| 13 | 24h soak 通过 | 待验收 |
| 14 | P0 全部关闭 | 待验收 |
| 15 | 发布包包含 LICENSE / NOTICE / CHANGELOG / rollback 脚本 | 待验收 |
| 16 | **写完即走铁律无违反（无循环对写频率）** | **v1.1 新增** |
| 17 | **GAME 场景 Handover 后本框架零写入** | **v1.1 新增** |
| 18 | **M0 能力矩阵覆盖全部计划写入路径** | **v1.1 新增** |

## 附录 B：参考基线与资料

- CoreTurboScheduler：https://github.com/Geometry1103/CoreTurboScheduler — 公开 Fork，**107 commits**（2026-10-05 实测核对），GPL-3.0，最后推送 2026-05-17。
- MW_CpuTurboScheduler：https://github.com/MoWei-2077/MW_CpuTurboScheduler — 上游项目，101 commits，releases V3.0/V3.5。
- CoreTurboScheduler 变更记录：仓库内 `CHANGES_v4.1.md`。
- COSMemory：https://github.com/wzcawzc123/COSMemory — 诊断基线 v1.1.5。
- 设备基线：一加11 / SM8550 / ColorOS 16 / Kernel 5.15.180-android13-8-o-01179 / KSU 3.3.0 LKM（见 §2.5）。

## 附录 C：术语表

| 术语 | 定义 |
|------|------|
| Policy | 对资源控制意图的描述，不等于直接写入内核 |
| Constraint | 对 Policy 的安全/设备/温度/电源边界 |
| Generation | 一组有效策略的唯一版本号 |
| Lease/TTL | 策略在没有新续租时自动过期 |
| Baseline | 模块未接管时或上一稳定策略的设备状态 |
| Capability | 当前设备真实存在并可控制的内核能力 |
| Death Attribution | 对进程死亡原因进行证据化归因 |
| Fail-safe | 发生未知故障时宁可少做优化，也不执行高风险动作 |
| **Control Handover** | **GAME 场景下本框架全量撤出控制权，交还游戏助手与内核（v1.1）** |
| **写完即走** | **策略边界事件触发一次写入即返回，禁止循环对写内核 governor（v1.1）** |
| **T-OBS / T-INT** | **热控两阶段：只读观测 / 受限干预（v1.1）** |
