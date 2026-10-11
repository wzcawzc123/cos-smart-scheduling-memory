# uag 参数富矿·接入设计 v1（2026-10-11，待实装）

> 侦察结论：一加11 三簇调速器均为 Oplus 自研 **uag**，参数 16 项；URO 目前只用 2 项
> （up/down_rate_limit_us，阶段B-CPU）。本文件定义剩余可挖参数的接入方案。

## 1. 参数全景（三簇实测值）

| 参数 | policy0 | policy3 | policy7 | 分类 |
|---|---|---|---|---|
| up_rate_limit_us | 2500 | 2500 | 2500 | ✅ 已用 |
| down_rate_limit_us | 80000/150000 | 同 | 同 | ✅ 已用 |
| **hispeed_load** | 90 | 90 | 90 | ✅ 行为（判定实验过） |
| **target_loads** | 80 | `80 2112000:95` | `80 2361600:95` | ✅ 行为（**多段语法！**） |
| **hispeed_freq** | 1344000 | 1536000 | 1708800 | ⚠️ 灰区（近频率控制，需授权） |
| stall_aware / reduce_pct_of_stall / break_freq_margin | 有 | 有 | 有 | ✅ 行为（未验） |
| soft_limit_enable / soft_limit_freq | 有 | 有 | 有 | ⚠️ 灰区 |
| cobuck_enable / multi_tl_enable / report_policy | 有 | 有 | 有 | ❌ 平台开关 |

## 2. 判定实验记录（写→20s 无回写→恢复）

- `hispeed_load` 90→95→20s 稳定→恢复 90 ✓（2026-10-11）
- `target_loads` 80→85→20s 稳定→恢复 80 ✓（2026-10-11）
- 结论：**uag 行为参数不在 Oplus 守护回写名单**（与 idle_timer 同性质）

## 3. 档位映射设计（拟）

| 档位 | hispeed_load | target_loads | 语义 |
|---|---|---|---|
| PERFORMANCE / FAST | 80 | 70 | 更早跳频、更激进 |
| BALANCE | -1（回基线 90） | -1（回基线 80） | 默认 |
| POWER_SAVE | 95 | 88 | 更晚跳频、更保守 |
| GAME / MEMORY_PRESSURE | -1（不动） | -1（不动） | 让权 |

## 4. 改动清单（四文件 + 2PC schema）

1. `controllers.cpp`：`UagNode` 加 `hiPath`/`tlPath`；probe 段 push_back 加两路径 + 建
   `hiBase_`/`tlBase_` 基线（照 base/baseD 模式）；apply 段加两行
   `runGroup("hi", &UagNode::hiPath, eff.cpu.uagHispeedLoad, hiBase_)` 与 tl 同款
2. `policy.hpp`：`CpuPolicy` 加 `uagHispeedLoad`/`uagTargetLoads`（-1=基线）
3. `policy_manager.hpp`：`ScenarioTactics` 加两字段 + tactics_for 赋值（上表）
4. `main.cpp`：base 映射两行
5. **2PC schema**：`cpu.state` 的 PREPARE 记录加 `baseHi`/`baseTl`（`save_cpu_state`/`pre` 结构）

## 5. 风险与注意

- **target_loads 多段语法**：p3/p7 带 `2112000:95` 段——**写入必须整串保留段**（不能只写单值），
  否则破坏 Oplus 的频率点策略；实现时按"基线串替换首段"处理
- **hispeed_freq 灰区**：写它=直接指定跳频目标频率（最接近"频率控制"）——**需用户明确授权**再动
- **区间安全**：hispeed_load 建议限幅 [50,99]、target_loads 首段 [50,95]（防极端值）
- 实装后**每档真机验证**（BALANCE 应回基线 90/80 原样）
