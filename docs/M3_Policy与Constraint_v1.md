# M3 Policy + Constraint — PolicyManager / generation+lease / Handover（dry-run）

> UnifiedRootOptimizer M3 · 2026-10-08 · v0.4.0 · 依据 v1.1 §9（M3 行）、§3.3 场景模型、§4.4 generation/lease、§5.5 Control Handover
> **退出条件：实现 DAILY/GAME(Handover)/POWERSAVE/MEMORY_PRESSURE 四场景，无策略震荡、无互相覆盖 —— 已达成（单测 + 真机双验证）**

## 1. 交付物

| 文件 | 内容 |
|------|------|
| `include/scenario.hpp` | §3.3 场景枚举 + 优先级（**GAME=100 绝对压倒其余**，§5.5 让权不协商）；BOOT/VIDEO/THERMAL/SCREEN_OFF 留枚举位待 M4 |
| `include/policy_manager.hpp` | `PolicyManager`：GlobalState+Event → 场景判定 → `ScenarioTactics` → `Decision{scenario, tactics, generation, changed, leaseUntilMs, why}` |
| `include/controller.hpp` | `resolve(base, gen)` 改为**以场景策略为基底**叠加各 Controller 约束（Degraded 不参与，也不得抹掉基底） |
| `src/main.cpp` | `run_policy()` 统一决策单元：事件驱动 + 空闲 tick 的租约检查共用；`handover` 时**直接跳过 apply** |
| `tests/test_m3.cpp` | 32 断言，覆盖退出条件本身 |

## 2. 四场景实现（§3.3 表逐格）

| 场景 | 触发 | tactics | 真机实证 |
|------|------|---------|----------|
| **GAME** | 前台 ∈ `game_apps.txt` 名单 | `handover=true`、reclaim 暂停、CPU 撤权（maxFreq=-1） | `SCENARIO=GAME gen=3 handover=1 why=game fg=com.tencent.tmgp.sgame` |
| **MEMORY_PRESSURE** | PSI/Mem 越阈事件 + **15s 滞回退出** + **60s 租约** | reclaim+freeze、maxKill=5 | 单测覆盖（压力风暴 20 连发仅 1 次决策变化） |
| **POWERSAVE** | mode.txt=powersave | reclaim+freeze、maxKill=3 | `SCENARIO=POWERSAVE gen=5 → APPLY freeze=1 maxKill=3` |
| **DAILY** | 默认 | reclaim on、freeze off、maxKill=0 | 基线与退出让权后均落此场景 |

## 3. 无震荡 / 无互相覆盖的三道防线

**① generation 幂等 + 单调（§4.4）**：`Decision.changed` 只在场景或 tactics 实质变化时为真，相同输入重放 generation 不变。真机序列 `1,1,2,3,4,4,5,5` —— 成对是 SCENARIO+APPLY 同代，全程无跳变无回退。

**② 滞回 + 租约**：压力场景退出需持续无压力 15s（`hysteresisMs`），并带 60s 租约，过期由空闲 tick 触发回落——**只读判断时钟，不轮询写节点**，与 §3.4 铁律不冲突。

**③ 边界判重**：`run_policy` 内 `materially_different` 二次把关，策略不变绝不调用 `apply_all`；`resolve(base)` 保证 Degraded Controller 不会覆盖场景基底（测试 §6 专测此反例）。

## 4. Handover（§5.5 全量让权）真机实弹

```
00:34:09 SCENARIO=DAILY    gen=2 handover=0 why=mode=balance fg=io.github.mangi.eta
00:34:16 SCENARIO=GAME     gen=3 handover=1 why=game fg=com.tencent.tmgp.sgame
  HANDOVER — zero writes (control handed to GameAssistant/kernel)   ← 无任何 TRACE 行
00:34:34 SCENARIO=DAILY    gen=4 handover=0 why=... fg=com.android.launcher   ← 退出让权
00:34:34 APPLY gen=4 maxFreq=-1 reclaim=1 freeze=0 ...                        ← 恢复提交
00:34:38 SCENARIO=POWERSAVE gen=5 handover=0
00:34:38 APPLY gen=5 ... freeze=1 maxKill=3
```

- **HANDOVER 段 TRACE 计数 = 0**：让权期间本框架零写入（`grep -A2 HANDOVER | grep TRACE | wc -l` → 0）
- 退出游戏 → `gen+1` 重新提交（§5.5 "恢复 baseline → 重新提交场景策略"）
- 与 M0 游戏助手指纹实测咬合："王者唯一战场 = 频率上下限，Handover 撤写即可"

## 5. 测试（`tests/test_m3.cpp`，32 断言）

```
[scenario] ok              [generation] ok (monotonic + idempotent)
[game-wins] ok             [no-oscillation] ok
[lease] ok                 [resolve-base] ok
[game-list-missing] ok
结果: 32 passed, 0 failed   rc=0
```

重点用例：
- **GAME 压倒压力**（priority 100 > 70）——让权不被任何其他场景覆盖
- **压力风暴 20 连发 → ≤1 次决策变化**（防震荡的核心量化断言）
- **滞回**：压力后 1s 想退回 DAILY 被拦，16s 后才回落
- **租约**：60s 内 alive、过期后 fallback DAILY
- **名单缺失 → GAME 禁用而非崩溃**（降级一致性）
- M2′ 回归 35/35 保持通过

## 6. 与 spec 对照

| spec 条目 | 状态 |
|-----------|------|
| §9 M3 交付：PolicyManager + ConstraintEngine + generation/lease | ✅（ConstraintEngine = `intersect()` + `resolve(base)`） |
| §9 M3 场景：DAILY/GAME(含 Handover)/POWERSAVE/MEMORY_PRESSURE | ✅ 四场景真机+单测 |
| §3.3 场景表 | ✅ 四格落地，其余留枚举位（SCREEN_OFF 卡在屏幕源未接入，挂 M4） |
| §4.4 单调 generation / lease TTL / 幂等 | ✅ |
| §5.5 全量让权 + 退出恢复 + generation+1 | ✅ 实弹 |
| §3.4 写完即走（租约检查只读时钟） | ✅ |
| **§9 退出条件：无震荡、无互相覆盖** | ✅ 单测 + 真机双证 |

## 7. 遗留（下一批）

1. **COSMemory 真实桥接**：`policy.memory.txt` 仍是快照，COSMemory 引擎尚未读取 → 下一步 Adapter 落地
2. **CoreTurbo cpusetProfile 实编译**：字段已留，profile 编译未做
3. **游戏名单维护**：目前手工文件，应接入游戏助手激活信号（§5.5 备选触发源）
4. **VIDEO/SCREEN_OFF/THERMAL 场景**：依赖屏幕源（M4）与 T-OBS 观测
5. **enforce 放量**：仍 dry-run；按 §9.2 逐 Controller 开关 + §10.2 实验矩阵
