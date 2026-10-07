# COSMemory 真实桥接 — reclaim.aggressive 热开关接入（v0.5.0，含受控 enforce 实弹）

> UnifiedRootOptimizer · 2026-10-08 · 依据 v1.1 §5.1（Memory Controller 基于 COSMemory）/ §9.2（dry-run→shadow→enforce 逐项放量）
> 结论：**桥接通路全绿，真机实弹闭环，字节保真终验通过（恢复后与原文件 md5 逐字节相同）**

## 1. 接口选型：为什么是 `reclaim.aggressive`

| COSMemory 接口 | 是否热读 | 能否桥接 |
|---|---|---|
| `config/memory.json` → `reclaim.aggressive` | ✅ `reclaim_cycle()` **每轮 sed 读取**，改完立即生效 | **选定** |
| `memory.json` → `maxKillPerRound` | ❌ 引擎实际用 env `MAX_KILL`，不读该字段 | 放弃（列遗留） |
| `freeze.enabled` | ❌ freeze 执行门在 `guard.conf`（`FREEZE_ENABLED=1`，COSGuard 领域） | 放弃（避免跨模块打架） |
| `force_reclaim` 旗标 | ✅ 面板一键释放通道 | 未用（属 COSMemory 自有 UX） |

`aggressive=false` 时 `reclaim_cycle` 直接 `return 0` —— 正是 §5.5 "暂停主动 reclaim" 的现成开关，无需改 COSMemory 一行代码。

## 2. 语义设计：三条映射规则（防"互相覆盖"）

```
reclaimEnabled=false          → aggressive=false   （GAME/BOOT：显式暂停主动回收）
reclaimEnabled && maxKill>0   → aggressive=true    （压力/省电：场景要求激进）
reclaimEnabled && maxKill==0  → 恢复用户基线        （DAILY：不干预面板配置）
```

第三条是关键：**常规态交还基线**，URO 只在场景明确需要时偏离，退出即恢复——否则每次运行都会覆盖用户在面板里的手动配置，正是 §9 退出条件"无互相覆盖"要防的。

配套：`probe()` 首次读基线存 `baselineAgg_`；`set_aggressive` 幂等（同值不写）+ 原子替换 + **写后语义级读回校验**（重新解析 aggressive 值，而非仅字节相等）。

## 3. 真机实弹（一加11 / COSMemory v0.6 引擎在跑）

放量开关 `--bridge-enforce`（§9.2 逐 Controller 单独放量；CPU 侧 `maxFreq` 恒 -1 不产生写入）：

```
01:08:39 APPLY gen=1 BOOT  reclaim=0  → memory.json -> OK | readback verified  (写 false)
01:08:39 APPLY gen=2 DAILY  reclaim=1 → memory.json -> OK | readback verified  (恢复基线 true)
01:08:54 SCENARIO=GAME gen=3 handover=1 why=game fg=com.tencent.tmgp.sgame
          HANDOVER — cpu/gpu withdraw (zero writes); memory bridge pauses reclaim
01:08:54 APPLY gen=3 reclaim=0         → memory.json -> OK | readback verified  (写 false)
   …退出游戏…
01:10:31 final: memory ACTIVE … bridge aggressive->true OK (false -> true)
```

| 验收点 | 结果 |
|---|---|
| 进游戏 → aggressive | `true` → **`false`**（md5 `3d639e08`→`bbe11715`） |
| 退出游戏 → aggressive | **`false` → `true`**（md5 回到 `3d639e08`） |
| **字节保真** | 恢复后 md5 **与原始备份完全相同**（含尾换行） |
| HANDOVER 段 cpufreq 写入 | **0 条**（maxFreq=-1，不与游戏助手打架） |
| COSMemory 引擎 | 全程存活（pid 11372），PSI 0.04 / MemAvailable 4788MB 下无 kill 触发 |
| dry-run 模式 | md5 恒 `3d639e08` 不变（TRACE 显示 `would write: {...(len=528)`） |

**风险控制**：实验前备份 `memory.json.uro.bak`；内存 4788MB、PSI 0.04 远高于 floor 1024/阈值 5.0，即使 aggressive=true 也不满足 `reclaim_should_fire` 的 kill 条件——全程零进程回收风险；起点终点配置一致。

## 4. 过程中修掉的四个缺陷

1. **互相覆盖（真机现场抓到）**：`MemoryController.desire()` 仍返回 `reclaimEnabled=true`（M2′ 遗留，当时无场景层），被 `intersect()` 的 OR 合并顶回 → 场景要求暂停回收被 Controller 覆盖，GAME 时不产生 APPLY。**修**：职责分离——reclaim/freeze/maxKill 归场景层，Controller desire 返回空。回归测试 `test_no_override` 锁死。
2. **dry-run 日志污染**：TRACE detail 把整个 528B 多行 JSON 写进 policy.log。**修**：detail 截断到首行/120 字符 + `len=`。
3. **字节不保真**：`read_all` trim 尾部导致写回丢尾换行（528→527B）。**修**：set_aggressive 改读原始字节替换；write 的 readback 比较时才 trim（格式噪音不参与语义比较）。测试断言"写后 size 差异恰为 `true(4)→false(5)` 的 +1"。
4. **C 字符串相加**编译错误 ×2、`set_aggressive` 指针/值参数不匹配——均当场修复。

> 另有两条环境教训：**linux chroot 看不到 `/data/adb`**（只挂了 `/data/local/tmp` 和 `/storage`），设备配置操作必须 `environment=android`；**`grep 'tmp/uro$'` 行尾锚定抓不到带参数的进程**，导致 enforce 实例存活期间 cp 报 `Text file busy`。

## 5. 测试

```
test_bridge  23 passed (含 no-override 互相覆盖回归 + 字节保真断言)
test_m2      35 passed
test_m3      32 passed
合计 90 断言 0 failed
```

## 6. 与 spec 对照 & 遗留

| spec | 状态 |
|---|---|
| §5.1 Memory Controller 基于 COSMemory | ✅ 经 aggressive 热开关真实驱动引擎 |
| §5.5 GAME 暂停主动 reclaim | ✅ 实弹：进游戏即写 false，退出恢复 |
| §9.2 dry-run → enforce 逐项放量 | ✅ `--bridge-enforce` 单开关放量，可随时撤回 |
| §9 无互相覆盖 | ✅ 基线恢复 + `test_no_override` 回归 |

遗留：
1. **`maxKillPerRound` 未真正下发**——引擎读 env `MAX_KILL` 而非 json 字段；要接需改 COSMemory 引擎（另一仓库、独立版本节奏）
2. **freeze 桥接**未做（执行门在 `guard.conf`，属 COSGuard 领域，需三方协调）
3. **enforce 长期开关**：实弹已验证但当前默认仍是 dry-run；是否长期启用 `--bridge-enforce` 待你决定
4. COSMemory 侧感知"URO 正在接管"的显示（面板状态栏）——可选 UX
