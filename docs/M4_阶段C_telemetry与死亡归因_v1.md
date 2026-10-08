# 阶段C：telemetry 证据链 + Death Attribution（v0.9.0）

> UnifiedRootOptimizer 阶段C · 2026-10-08 · 依据 §5.6（Death Attribution）/ §4.5（Telemetry 1 低频）/ P0-07（action_id 审计）
> 验收项 10：**Death Attribution 在证据不足时输出 UNKNOWN —— 实机达成**

## 1. telemetry.jsonl（结构化证据链）

写入时机 = 幂等门槛之上（`materially_different` 之后）= **只有实质变化才写**（§4.5"1 低频"）。
1MB 轮转（压成 `.1` 保留一份）。单行格式：

```json
{"ts":1791432230,"action_id":"t1791432230-4-11469","gen":4,"scenario":"POWERSAVE",
 "why":"mode=powersave","enforce":1,"dryRun":0,"handover":0,"fg":"io.github.mangi.eta",
 "memAvailMb":-1,"psiMem10":0,"reclaim":1,"maxKill":3,"uagUpRate":2500,
 "ctrl":[{"memory":"ACTIVE"},{"cpu":"ACTIVE"},{"gpu":"DEGRADED"},{"thermal":"DEGRADED"}]}
```

- `action_id` = `t<ts>-<seq>-<pid>`（P0-07：主动动作可审计）
- `Decision.scenario/why` 本就是为 telemetry 预留的字段（设计期伏笔，本轮启用）

## 2. Death Attribution（§5.6）

`URORuntime --attribute <pid> [pkg]` → JSON；**exit 0=归因成功 / 2=UNKNOWN**（可脚本判断）。

| 归因 | 证据源 | 状态 |
|---|---|---|
| COSMemory_RECLAIM | `guard.telemetry`（`epoch\|pid\|pkg\|\|ACTION\|detail`） | ✅ 实证 |
| SYSTEM_LMKD / AMS_KILL / CRASH | logcat main/system/crash 语义分支 | 已实现，**待真实事件** |
| **UNKNOWN** | 无任何佐证 → 不伪造 | ✅ 实证（验收项10） |

## 3. 真机验收

```
构造证据 → --attribute 4242 com.test.pkg
{"pid":4242,"cause":"COSMemory_RECLAIM","evidence":"guard.telemetry: 1728350000|4242|...|RECLAIM|aggressive depth=cached"}  rc=0

对照     → --attribute 999999 com.none.app
{"pid":999999,"cause":"UNKNOWN","evidence":"no corroboration in guard.telemetry/logcat"}  rc=2

telemetry 5 条真实记录：BOOT/DAILY/POWERSAVE/DAILY 切换全程，
  gen4 POWERSAVE 的 uagUpRate=2500 + maxKill=3 被完整审计（阶段B 动作已入证据链）
```

## 4. 遗留与坑

1. **logcat 三分类待真实 kill**：开机以来 0 次 LMK/AMS kill（设备健康），不为测试制造事故——
   留待自然事件；UNKNOWN 路径已证明"不伪造"底线
2. host 侧编不了 `drivers.cpp`（NDK 专用头）→ main.cpp 的改动只能 NDK 构建后实机验（本轮即此）
3. 缺 `unistd.h`（getpid）导致第一次 NDK 构建失败
4. guard.telemetry 当前 0 行（内存充足未触发回收）——COSMemory_RECLAIM 的 100% 对应待自然事件复核
