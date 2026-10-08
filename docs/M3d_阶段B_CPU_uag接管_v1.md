# 阶段B-CPU：uag 升频延迟纳入场景决策（v0.8.0）

> UnifiedRootOptimizer 阶段B 第二刀 · 2026-10-08 · 依据 §5.2 / §3.4 / M0 能力矩阵
> 结论：**POWERSAVE → 三簇 2500us、常态回基线，真机往返 10 秒闭环**

## 1. 侦察决定范围（这刀砍什么、不砍什么）

| 发现 | 影响 |
|---|---|
| **CoreTurboScheduler 未部署在设备上**（16 模块无它） | "接管"实为**移植其能力**，不存在双写抢地盘——风险面大减 |
| CT 的 `SDM8G2.ini` 四档想切 `conservative/walt` governor | **违反 §3.4 铁律**（本机必须保持 uag），照抄即错 |
| ParamSchedPath 3 需改 6 缺失（M0 已证） | CT 的 uag 调优在本机**路径根本对不上** |
| cpuset 收紧（foreground 0-6 vs 现值 0-7） | M0 明确标**掉帧风险未实验** → 不收 |
| uclamp（top-app min=13 现值 vs conf 0/10） | 语义未核（M0：接入前先 dry-run）→ 不收 |
| 频率上限（SmallCoreMaxFreq 等） | Oplus ~6s 动态钳制的地盘 → 不收 |

**选定目标**：`up_rate_limit_us`（升频延迟）——CT 四档里唯一有场景差异的 uag 参数
（powersave 2500 / balance 2000 / performance 1000 / fast 0），设备现值 **0**、Oplus 性能模式不碰 → **URO 唯一写者**。
实测写入通路：`echo 1 → readback=1 → echo 0 恢复` 全通。

## 2. 场景映射

```
POWERSAVE → 三簇 up_rate_limit_us = 2500   ← CT powersave 档原值（升频变慢=省电）
DAILY     → -1 → 回基线（设备原值 0）
GAME      → handover 撤权 → 回基线（不与游戏助手/内核抢）
```

- `intersect` 取 **max**：该值越大越限制升频（越保守），约束求交应取更限制的一端
- 频率上限/cpuset/uclamp 留占位与遗留，等专项实验

## 3. 实现

- `CpuPolicy`/`ScenarioTactics` 增 `uagUpRateUs`（-1=不干预），判重与 `materially_different` 均覆盖
- `CpuController` 重写：枚举 `policy*/uag/up_rate_limit_us`（policy0..9 探测，本机命中 3 簇）
- 独立档案 `cpu.state`：`DIRTY= / UP:policy0=0 / UP:policy3=0 / UP:policy7=0`（各簇基线分别记）
- 崩溃恢复：DIRTY=1 → 启动全簇写回各自基线 → CLEAN（与 Memory 同款状态机）
- `desire()` 返回空（职责分离：场景值经 `resolve(base)` 注入，与 M3 一致）

## 4. 修的两个坑

1. 缺 `<map>` 头（std::map 未声明）
2. `make_cpu_controller` 新旧签名并存 → 重载歧义（旧无参声明+定义残留未删）

## 5. 验收

**单测 120 断言全过**（bridge 53 + m2 35 + m3 32；新增 `uag-takeover` 12 条）

**真机（enforce=1，真实 sysfs）**：
```
基线建档:  DIRTY=false  UP:policy0=0 UP:policy3=0 UP:policy7=0
POWERSAVE: 0/0/0 → 2500/2500/2500   DIRTY=true（基线保持 0 不漂移）
balance:   2500×3 → 0/0/0           DIRTY=false
```
往返 10 秒完成，三簇 readback 全部一致。

## 6. 遗留

1. `cpuset` 收紧 —— 需 M0 挂账的掉帧实验，未做
2. `uclamp` 语义核实（top-app min=13 现值从何而来）
3. 频率上限三档 profile（Oplus 动态钳制下是否仍有意义需实测）
4. CT `fast` 档在 URO 无对应场景——DAILY 未按 performance 细分
5. LaunchBoost（CT 内置 false 关闭）——事件驱动+TTL+Handover 门控，待 M4 观测就绪后做
