# 阶段B-Memory：reclaim 深度与冷却纳入场景决策（v0.7.0）

> UnifiedRootOptimizer 阶段B 第一刀 · 2026-10-08 · 依据 §5.1 / §3.3（MEMORY_PRESSURE=提升回收优先级）
> 定位：M3b 只下发了 `aggressive` 一个旋钮；本期把 `depth`、`cooldownSec` 也收进场景决策——**决策上收、执行器保留**

## 1. 可接管范围盘点

| COSMemory 参数 | 引擎读取方式 | 决策 |
|---|---|---|
| `reclaim.aggressive` | `reclaim_cycle` 每轮 sed **热读** | ✅ 已接管（M3b） |
| `reclaim.depth` | 每轮 sed 热读 | ✅ **本期接管** |
| `reclaim.cooldownSec` | 每轮 sed 热读 | ✅ **本期接管** |
| `psiThreshold`/`memFloorMB` | 每轮 sed 热读 | 字段占位已留，本期不用（阈值下调收益有限且易过度触发） |
| `maxKillPerRound`/`keepAlive.adj` | **引擎启动 env**（`MAX_KILL`/`KEEPADJ_TARGET`） | ❌ 需改 COSMemory 仓库 → **记遗留** |
| 名单/白名单 | 用户配置入口 | ❌ 用户资产 |
| freeze 执行门 | `/data/system/cosmem/guard.conf`（COSGuard 领域） | ❌ 跨模块 |

## 2. 场景映射（与 aggressive 同款"不覆盖用户配置"原则）

```
MEMORY_PRESSURE → depth="service"(彻底) + cooldownSec=30   ← §3.3 提升回收优先级
其余场景        → 空/-1 → 写回基线                          ← 常规态交还用户配置
GAME            → reclaim=false（已有，不变）
```

## 3. 实现

- `MemoryPolicy` 增 `depth/cooldownSec`（空/-1=不干预）；`ScenarioTactics` 同步 + **判重覆盖**（否则 depth 变化不产生 generation）
- `set_reclaim_field(key, jsonLit)`：节内任意字段替换——读原始字节（字节保真）→ 定位 → 替换 → 写 → 读回校验 → 幂等
- `BridgeState` 多字段档案 `BASELINE/DIRTY/DEPTH/COOL`；**DIRTY = 任一字段偏离**
- apply 后逐字段算目标（场景给了就用、没给回基线）→ 写回 → 统一判 DIRTY
- 崩溃恢复：DIRTY → agg+depth+cool 全量归还（单测+真机双验）

## 4. 过程中修的两个缺陷

1. **`else if` 互斥**：「补档案」与「崩溃恢复」写成互斥 → 旧格式 state（DIRTY=1 缺 DEPTH/COOL）走补档案、**恢复被跳过**。改顺序执行，单测当场红 3 条。
2. **档案缺失静默失效**：旧格式 state 缺字段 → `baselineDepth_` 恒空 → depth **永不接管且无报错**。补建修复（干净态当前值必为基线，安全）。

## 5. 验收

**单测 108 断言**（bridge 41 + m2 35 + m3 32；新增 `multi-field`、`crash-depth`）

**真机（真实可达场景：新格式档案 + 偏离 + kill -9 + watchdog 拉起）**：
```
PROBE ctrl=memory state=ACTIVE detail=CRASH-RECOVERY agg->true depth->"cached" cool->60 OK
json:   depth=cached  cooldownSec=60        ← 三字段全部归还
state:  DIRTY=false DEPTH="cached" COOL=60  ← 档案保持正确、转干净
```

### 被测试暴露但现实中不可达的组合（如实记录）

先构造"旧格式 state + depth 已偏离"现场 → `CRASH-RECOVERY depth->"service"`，**污染值固化为基线**（档案缺失只能用当前值补）。分析：旧版 v0.6.0 无 depth 写入能力，升级时 depth 必为原值 → 该组合不可能出现；正常路径档案在首次 apply 前建立，且 apply 写 depth 有 `!baselineDepth_.empty()` 前置（无档案不写）。改用真实可达场景复测通过。

局限入档：若未来出现「档案被删 + 配置已偏离」，该字段只能固化现值——用户原值已无处可查。

## 6. 遗留

1. `MAX_KILL`/`KEEPADJ` 接管需改 COSMemory 引擎（另一仓库、独立版本节奏）
2. `psiThreshold`/`memFloorMB` 占位未启用
3. 压力场景**正向写入**真机未端到端（设备 4788MB/PSI 0.04 难自然触发）——单测覆盖、写入通路与 aggressive 同构（后者真机验过）
4. 引擎侧执行效果（TELEM `depth=service` 行）待真实压力场景观察
