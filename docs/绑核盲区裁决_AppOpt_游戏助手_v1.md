# 绑核盲区专项探测裁决 — AppOpt × 游戏助手 × Oplus 三方

> UnifiedRootOptimizer M0 附加产出 · v1.0 · 2026-10-07
> 实验：affinity_probe v2，200ms 采样，00:25:29–00:32:21（约 7min，覆盖游戏进/三模式切换/挂后台退桌面）
> 证据：`docs/evidence/game_fp/affinity_probe2_1007_0025.log`（52 个 AFF-DIFF 块 + 41 FREQ 块）
> 背景：AppOpt 2.2.6 eBPF 在位（core_ctl 已二改还原原厂）；v1 轮因 diff 格式 grep 失配作废（教训入 §4）

## 1. 裁决结论

**冲突真实存在，但主角不是游戏助手，是 AppOpt ↔ Oplus 系统调度。**

三方在绑核域的实际角色：

| 参与方 | 行为 | 证据 |
|--------|------|------|
| **AppOpt** | 自建 `/dev/cpuset/AppOpt/{0-2,0-6,0-7,3-7,7}` 子树，按 `applist.conf`（`check_interval=2s`）把线程**移入自己的 cpuset 组**（不是原生 sched_setaffinity——修正先前判断） | 王者规则实存：`UnityMain=hp-core`、`UnityGfxDeviceW=p-core,hp-core`；现测 `UnityMain cpuset=/AppOpt/7`、mask=7；组内任务 0-6:401 / 0-7:42 / 7:11 / 3-7:5 |
| **Oplus 系统调度** | 周期性把线程**迁出** AppOpt 组回 0-7，时刻与 FREQ 事件（5-7s 节奏的 scaling_max 动态）同 tick | 多处同毫秒共现：`00:31:42.601 / 00:32:02.449 / 00:32:17.009` 皆 FREQ+AFF 同戳，方向为 `7 → 0-7`（释放） |
| **游戏助手** | **绑核域无独立指纹**：三次模式切换时刻的 AFF 块与常规 cadence 无差异；其已知动作仍在频率域（Handover 已覆盖） | 模式 pin 时刻 AFF 块 n=5~6，与非模式时刻的常规块同形态 |

**翻烙饼量化**：UnityMain 在 7 分钟内被翻转 **28 次**（`7 → 0-7` 由 Oplus 同步 FREQ 动作完成，`0-7 → 7` 由 AppOpt ≤2s 内重钉），持续到挂后台之后——**规则永不稳态**。

## 2. 后果评估

1. **AppOpt 的游戏规则实际失效**：钉核 → 被释放 → 重钉，循环中"钉在 hp-core"的意图从未稳定成立
2. **额外扰动**：主线程每 5-15s 被迁移一次 cpuset，迁移本身打断调度局部性——**对帧平整度是净负面**（钉核想帮忙，实际在帮倒忙）
3. 非游戏域无此现象（AppOpt 的 401 个 0-6 任务未见翻烙饼模式）——冲突集中在**系统调度活跃 + AppOpt 规则重**的交叠区，游戏进程首当其冲

## 3. 处置决定

1. **不 hook 游戏助手**（第三次确证：它不是绑核域主角，频率域用 Handover 已够）
2. **处理点在 AppOpt 侧 —— 游戏进程豁免**：注释掉 `applist.conf` 的 `com.tencent.tmgp.sgame` 块（或按作者文档用豁免语法），消除互抢；AppOpt 对微信/QQ 等非游戏规则继续生效（那片区域无冲突证据）
3. 不动 Oplus oiface（Oplus 自家调度，停用影响未知，不轻动）
4. **M3 输入源登记**：AppOpt 规则表 + Oplus 迁移行为 + 游戏助手频率脉冲，三者皆为 Constraint Engine 约束源（Phase B 集成对象）

## 4. 方法论教训（累积）

1. **busybox grep 不支持 `\|` BRE 交替** —— 本轮再次踩中（一度误判"AppOpt 无游戏规则"）；**此后所有设备端 grep 一律用 `-E`**
2. **`/system/bin/diff` 输出 unified 格式（`+`/`-`），不是普通格式（`<`/`>`）** —— v1 轮全军覆没的原因；探测类脚本必须先在活体进程上自测过滤管道
3. **亲和性读数要配 cgroup 归属才有归因力**：`mask=3-7` 可能是 cpuset 迁移也可能是显式钉核，`/proc/tid/cgroup` 的 cpuset 行是判据
4. 测试设计要考虑"挂后台不杀进程"的现实：进程不死则事件驱动的采样器不自停，需显式超时或手动收（本轮靠手动收，两轮皆然）

## 5. 对 AppOpt 集成状态的修正记录

| 先前说法 | 修正后 |
|----------|--------|
| "不写 sysfs、不碰 cpuset" | 它**自建** `/dev/cpuset/AppOpt` 子树（不改 Oplus 现有组，但通过 cpuset 迁移参与同一资源域） |
| "与游戏助手可能冲突" | 实测**与 Oplus 系统调度冲突**；游戏助手在绑核域无独立动作 |
| "零重叠" | 非游戏区近零重叠；**游戏区高频互抢**（豁免处理后归零） |
