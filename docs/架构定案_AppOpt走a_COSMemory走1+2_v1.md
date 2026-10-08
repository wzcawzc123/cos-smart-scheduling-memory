# 架构演进定案：AppOpt 走 a / COSMemory 走 1+2

> UnifiedRootOptimizer · 架构裁决记录 v1.0 · 2026-10-08（用户拍板）
> 关联：`绑核盲区裁决_AppOpt_游戏助手_v1.md`（10-07 三方冲突裁决，本方案为其 Phase B 延伸）

## 0. 背景

用户提出完整目标架构树（COS Smart Scheduling：Core/Controllers/Detection/Evidence/Recovery/Profiles）
与运行闭环（Detection→EventBus→State→Policy→Constraint→Effective→Controllers→Transaction→
Kernel→Evidence→Feedback→State）。对照现状约六成已在，剩余按下述两命题推进。

## 1. 命题一：AppOpt → ThreadController（拍板：路线 a，零改 AppOpt 代码）

三条路线中选 **a**：约束源 + 规则代理 + 场景协调。b（改源码收编）/c（重写绑核）均不做。

### Phase-1 范围（下轮实施）
1. **Thread Evidence（读）**：低频轮询 `/dev/cpuset/AppOpt/{0-2,0-6,0-7,3-7,7}/tasks`
   → 成员数快照 → `thread.jsonl`（Evidence 层，独立于 telemetry）。
2. **约束源解析（读）**：解析 `applist.conf`（规则数/包名集合/GAME 豁免状态）
   → StateManager 输入 + telemetry 记 `rules=N`。
   - 语法已侦察（作者 http://AppOpt.suto.top）：`包名=核组`、`包名{Thread}=核组`、
     `{}` 块形式、核名 `e-core/p-core/hp-core/all-core`（逗号组合）、`#`/`//`=注释。
3. **场景协调（写，谨慎）**：GAME 档触发时校验游戏包是否被 AppOpt 规则覆盖
   （10-07 结论：游戏区应豁免）→ 仅**校验+日志+telemetry**；自动注释豁免做成开关**默认关**。
4. **规则代理框架（写，本期只搭框架）**：`applist.conf` 内维护
   `# URO-GEN-BEGIN` / `# URO-GEN-END` 标记区块（追加式，不动用户/作者规则）；
   本期不生成实际规则，通路先立。

### 降级
AppOpt 模块不存在 / conf 不可读 → ThreadController 报 `ABSENT`，其余控制器不受影响。
### 禁区（10-07 复用）
- 不 hook 游戏助手（三次确证非绑核主角）；不动 Oplus oiface；
- 设备端 grep 一律 `-E`；改 applist.conf 前备份（参照 `.bak_20261007` 惯例）。
- 现存豁免（行90-91 带署名注释）**不得覆盖**。

## 2. 命题二：COSMemory 并入（拍板：1+2，不真合并）

### 1. 联动不合并 ✅ 本方案
- URO `customize.sh` 探测 `/data/adb/modules/COSMemory` 缺失 → 用 `ksud module install <zip>`
  自动刷入（ksud 子命令已侦察存在）；已装则跳过（版本比较可选）。
- zip 来源：**内嵌 URO 包 `cosmemory/COSMemory-<ver>.zip`**（需先补打 v1.1.5 发布包——
  设备仅有 v0.8.2/v1.1.4 旧包、装机是 1.1.5）。
- 与配套 LSP 模块规范同构：安装提示不写死路径，失败只报不 abort。

### 2. 事务级并联 ✅ 已于 v0.15.0 达成（无需新工）
- 今日 two-phase 改造已把 `memory.json` 全部写入纳入 TransactionManager：
  写前 `PHASE=PREPARE` → 写 json → 写后 `PHASE=COMMIT`；崩溃任意点 probe 回基线。
- 真机已验（PREPARE 构造崩溃 → CRASH-RECOVERY 归还）。**本命题的 2 即此物**。

### 3. 真合并 ❌ 不做
COSMemory 保持独立演进与分发；仅当未来产品决策改变再评估。

## 3. 实施顺序
1. [ ] Thread Evidence 快照 + thread.jsonl
2. [ ] applist.conf 约束源解析 + telemetry rules 计数
3. [ ] GAME 档豁免校验（只读告警）+ URO-GEN 区块框架
4. [ ] 打 COSMemory v1.1.5 zip + URO customize.sh `ksud module install` 联动
5. [ ] 全量单测（run_test.sh 145+）→ NDK 构建 → 真机验收 → v0.16.0

## 4. 目标架构树落地映射（供后续 spec 化）
EventBus/StateManager/PolicyManager/EffectivePolicy/SchedulerLoop/LeaseManager = 现组件正名；
ConstraintEngine/TransactionManager = 逻辑已有待独立化（2PC 即事务层雏形）；
Detection 缺 CpuLoad/Thermal、Evidence 缺 FPS/温度/功耗、Thermal/Gpu Controller 与
Device/Power Profile = 占位待建（并入 E 阶段路线）。
