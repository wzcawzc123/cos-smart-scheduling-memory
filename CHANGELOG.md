# Changelog

版本规范：`大.中.小`——功能迭代升第二位、小修升第三位、大重构升第一位；versionCode 每次 +1。
发布件归档：`/storage/emulated/0/性能调度模块/`（新包根目录，旧包挪 `旧版/`）。

## v0.29.0（versionCode 2900）— 看板「App 画像编辑器」（内联 Vue，纯前端）
- **新能力**：KSU WebUI 看板新增「App 画像（可编辑）」卡，不再需要手工改 `app_profiles.txt`
  - 表格列 pattern / down / up / cpuset + 中文语义（跟随档位 / 固定 μs / 小核·中核·大核·全核 / 核号）
  - 增 / 改 / 删（**两击确认**）/ **↑↓ 调优先级**（文件顺序 = 匹配优先级）
  - 前端校验：包名非空且无空白与 `|#`、速率 ≥ -1 或留空、cpuset 字符集、**只有包名的空字段行直接拦掉**
  - **原子写**：备份 `app_profiles.txt.uro_bak_<epoch>`（保留最近 3 份）→ 同目录 tmp → `chown/chmod` 对齐原属主 → `mv` 重命名替换 → **回读逐字节比对**
  - 卡内回显 **指挥链② 生成结果**（读 AppOpt `applist.conf` 的 URO-GEN 区块），写完 65s 自动再刷一眼
- Vue 3.5.43 `vue.global.prod.js` **内联**进单文件 index.html（KSU WebUI 本地环境无外网依赖）；26.9KB → 218.6KB
- **真机全链验收**（一加 11 / ColorOS 16，v0.29.0 部署后实测）：
  - 新增 `com.ss.android.ugc.aweme|-1|-1|hp-core` → 文件 2 行、属主权限保持 `u0_a296:media_rw 660`、备份 32B = 旧内容 ✓
  - 删除（两击确认）→ 回 1 行、新备份 71B = 两行版 ✓
  - 指挥链②：`com.coolapk.market=p-core`、`com.ss.android.ugc.aweme=hp-core` 均在 60s 内进入 URO-GEN 区块 ✓
- **踩坑定案**（已修，写进前端注释）：
  1. Vue `:disabled="a || b"` 在 a、b 皆空时求值为**空字符串**，布尔型 DOM prop 经 `includeBooleanAttr('')` 判为 **true**
     → 按钮永久 disabled，且浏览器**连 click 事件都不派发**（症状="点了没反应、无日志"）→ 必须 `!!` 收敛成布尔
  2. KSU WebUI 的 WebView 内容**不进无障碍树**（节点恒 0）→ 验收只能截图/OCR 定位 + shell `input text` 注入
  3. 运行时既有坑（本次在前端兜住）：四字段行 down/up 留空 → `atoi("…|cpuset")=0` 被当成"固定 0μs"
     → 编辑器写回四字段行时把空速率统一补 `-1`
- 测试：离线假桥 **29 断言全过**（`runtime/tools/webui_profile_test.mjs`）+ `node --check` 三段脚本 + 真机截图
- **不改任何调度代码**（纯前端）；设计/坑记录见 `docs/看板画像编辑器_v0.29.0.md`

## v0.28.1（versionCode 2801）— L5-α 只读洞察（看板 + 离线工具）
- 看板新增「L5 洞察」卡：最近 200 条决策的**档位分布**与 **Top 3 App 占比**
  （tel 段 tail 14→200；纯前端统计，**不改任何策略**）
- 新增离线分析工具 `tools/l5_insight.py`（Linux 侧全量统计：档位/前台包/why 关键词/
  fps 哨兵与 jank 分位/温度分位/GPU/审计事件计数）——首跑 12.8h/2855 条样本
- 首跑洞察（开发期样本）：PERFORMANCE 47.1%｜抖音 57% 前台｜jank 中位 9.4%/P90 38.3%｜
  温度中位 53°/峰值 96°｜电量回喂 469 次
- 结论：**样本=开发期使用，代表性有限**；L5 定位为"只读建议层"，真 ML 路线评估为不划算

## v0.28.0（versionCode 2800）— KEEPADJ 接入（keepAlive.adj 档位联动）+ hispeed_freq 负结果
- **KEEPADJ**：复用 `set_reclaim_field("adj")`（COSMemory 引擎零改动）——档位映射
  PERF/FAST=100（强保前台）、BAL=200（默认）、PS=250（放宽）、压力=300（多回收）、GAME=不动
- 真机闭环：`adj 200 → 250(熄屏PS) → 200(亮屏)` ✓
- **关键坑（本轮实弹抓到）**：`materially_different` 字段比较表**不含新加的 keepAliveAdj**
  → 值变了却不被判定为"实质变化" → **apply 被幂等门跳过**（首版部署 adj 纹丝不动）
  → 修：比较表补字段。**教训：加 EffectivePolicy 字段必须同步更新 materially_different**
- **hispeed_freq 判定（负结果）**：写 1700000 → 亮屏保持 → **熄屏被 Oplus 清零(0)** →
  亮屏被重设(1536000) → **非独占，不接入**。至此 uag 富矿定论：真正安全可用的只有
  up/down_rate_limit_us 与 hispeed_load 三项
- 构建插曲：qemu 链接期偶发崩溃（`mremap_chunk` 已知问题），清 .o 重跑成功
- 单测 199 全过（[keepadj] PERF=100/PS=250）

## v0.27.0（versionCode 2700）— 看板补齐 GPU/调参两行
- Evidence 采样卡新增两行：**GPU**（busy% · MHz · idle_timer，含帧率哨兵 `⚠jank高` 标记）
  与 **CPU 调参**（hispeed_load 当前值 · up_rate）
- batch 加 `gpu`（tail gpu.jsonl）与 `hi`（读 uag hispeed_load）两段；node --check 通过
- 真机数据通路验证：`gpuBusy:13/220MHz/idle80`、`hispeed_load=90`
- 至此看板覆盖：档位/总闸/时间线/写入证据 + Evidence 六行（电量/热点/帧率/AppOpt/GPU/调参）

## v0.26.0（versionCode 2600）— uag 第二项实验（负结果：target_loads 非独占，主动断开）+ 原子部署
- **target_loads 实验与回滚（诚实负结果）**：
  · 实现了完整通路（多段保留纯函数 `tl_set_first`、字符串档案 `TL:`、runGroupStr）
  · 真机实测发现**非独占**：Oplus 熄屏时改写频率点段（2112000→1056000），URO 写入被覆盖
  · 判定实验（20s 亮屏窗口）未覆盖"熄屏时 Oplus 写入"场景 —— **实验设计漏洞**已记教训
  · 处置：**彻底断开**（连基线回写也不做），保留通路与档案结构备用；不制造节点争用
  · 终验：`hi 90→95→90`（联动在岗）、`tl` 全程原样（归 Oplus）
- **原子部署工具** `runtime/tools/deploy.sh`：kill→tmp→chmod→mv(rename 原子)→等拉起→验证；
  实测两次 10s 拉起，消除 cp 截断窗口（watchdog 自杀根因之二）
- 单测 +5（[tl-poly] 多段/单段/空/负值），199 全过

## v0.25.0（versionCode 2500）— uag 富矿第一项：hispeed_load 档位联动 + watchdog 自杀修复
- **hispeed_load 接入**（设计 v1 照单）：UagNode.hiPath、probe 探测+基线（cpu.state 新增 `HI:` 行）、
  apply runGroup、tactics 映射（PERF/FAST=80 更早跳频、PS=95 更保守、BAL=-1 回基线）、main 映射
- **真机闭环**：BAL=90 → 熄屏 PS=95 → 亮屏回 90（三簇、HI: 基线 3 条入档）
- **线上 bug 修复两处（本次实弹抓到）**：
  ① 基线补档条件漏 hi（`needSeed` 只看 up/down，老 cpu.state 非空即跳过 → hi 基线空 → 回基线写 0
     灾难值，已手动止血复位）→ 加 `hiMissing` 检测
  ② `runGroup` 无基线保护：`-1` 且无基线时写 0 → 改为跳过不写（同时保护 up/down 路径）
- **watchdog 自杀根因定罪**：service.sh 两处 `exit 1`（bin 缺失/无执行位）+ 我部署时 `cp` 存在
  「被截断窗口」→ watchdog 恰在窗口内循环即自杀（实测两轮）→ 改「记日志+下轮重试」自愈；
  部署流程改原子替换（后续）
- 单测 +3（[uag-hi] PERF=80/PS=95/BAL=-1），194 全过
- 测试教训：PERF 租约 + touch 刷新 + FAST 窗口三重时间窗叠加，测试须推到窗口外（9000ms）

## v0.24.0（versionCode 2400）— L4 完成：温度趋势预判 + 帧率哨兵
- **温度趋势预判**：`next_thermal_level` 加 `trendDeg` 参数——接近阈值（t1-6）且 5min 升温
  >=4°C → 视作已达 t1 提前一级（防撞温度墙）；driver 侧 5 采样 ring 算趋势
- **帧率哨兵**（观测优先，不自动改策略）：`fps_sentry_trigger`（连续 3 窗口 jank>15%）→
  fps.jsonl 行加 `"sentry":1` 标记（供看板/后续分析）
- 单测 +5（[l4-sentry] 趋势提前/无趋势不提前/即时阈值/哨兵触发/中断不触发），194 全过
- **L4 反馈闭环三环齐**：电量守卫（回喂夹档）+ 温度趋势（预测）+ 帧率哨兵（观测）

## v0.23.0（versionCode 2300）— L4 反馈闭环第一环：电量守卫（Evidence→State→Policy）
- **数据回喂链**：`read_battery_state` → evidence_driver 60s 检测（pct 变≥2 或充电态变）→
  推 `ChargerChanged`（payload "Charging:56" 扩展格式，兼容旧纯字符串）→ `GlobalState.batteryPct`
- **电量守卫夹档**（decide，热夹档旁）：低电+放电 → 收性能（≤PCT: FAST/PERF→BALANCE、
  ≤10%: POWER_SAVE）；**充电中不干预**；GAME 让权；`uro.conf` POWER_GUARD/POWER_GUARD_PCT（默认开/20）
- 审计：`Decision.powerCapped` + events.log `POWER-GUARD-CAPPED` 行（同 thermalCapped 模式）
- 验证：host [power-guard] 4 条（15%夹/8%PS/充电豁免/why）；真机实证
  `ChargerChanged src=battery-guard data=Charging:56 [state-changed]`（回喂链路通）+
  充电中无夹档（设计符合）；放电低电实弹待自然场景
- L4 后续：温度趋势预判、帧率哨兵（观测优先）

## v0.22.0（versionCode 2200）— GpuController 实装（idle_timer 档位联动）
- `GpuPlaceholder` → **真 GpuController**：probe 走 adapter.exists（fake root 友好）、
  apply 写 idle_timer（幂等 lastWrote_、dryRun 支持、0=不定零写入）
- 档位映射（tactics_for）：PERF/FAST=120（减升降频抖动）、BALANCE=80、POWER_SAVE=50、
  GAME/压力=0（让权）；EffectivePolicy.gpu.idleTimer 新字段
- **真机端到端实弹**：熄屏 POWER_SAVE→`idle_timer=50`、亮屏 BALANCE→`80`、
  telemetry `gpu:ACTIVE`（DEGRADED→ACTIVE）；189 断言
- 测试兼容修复：m2 两处旧 placeholder 预期随实装更新（probe 尊重 fake root）

## v0.21.0（versionCode 2100）— GPU 层侦察 + 观测落盘（C 组第一项第一步）
- **侦察定案（Adreno 740 / kgsl-3d0）**：安全区 = `idle_timer`（行为参数）；
  禁区 = `max_pwrlevel`/`max_gpuclk`/`max_clock_mhz`（上限）、devfreq governor（策略）、`force_*`（调试）
- **地盘判定实验**：idle_timer 写 120 → 45s 无系统回写 → 安全可控（已恢复 80）
- `gpu_json`（busy% + cur MHz + idle_timer）→ **gpu.jsonl 60s 观测**（只读零风险，实测出数）
- **待办**：GpuController 实装（idle_timer 档位联动，需 2PC+注册链，独立工程）
- **事故与修复**：曾发现 watchdog（uro/service.sh）中断、手动启动时参数误传（`--bridge-enforce`
  被当 logdir）致证据停摆 → 已用 daemon 恢复 service.sh + 正确启动，gpu.jsonl 正常；
  中断根因未定（模块开机自启机制本身完好，观察下次重启）
- 单测 +3（[gpu]），189 全过

## v0.20.0（versionCode 2000）— Thermal 干预（T-OBS → 真 Detector）
- 温度进决策：`read_max_temp` + `next_thermal_level`（迟滞 T1/T2/6°C）→ 边沿推
  `ThermalChanged` → `GlobalState.thermalLevel`（0/1/2）→ **decide 夹档**（不新开写入路径）
- 夹档语义：L1 压 FAST/PERF→BALANCE、L2→POWER_SAVE；画像也压不过（热=安全底线）；
  GAME 让权不干预（系统 thermal-engine 管）；`uro.conf` 可配 THERMAL_ENABLE/T1/T2（热读）
- 审计：`Decision.thermalCapped` + events.log `THERMAL-CAPPED` 行（堵幂等门盲点）
- 真机触发验收：T1=40 实测 `level=1 temp=47`；fg 切换事件发生而无 FAST 行 =
  FAST 被夹成 BALANCE 幂等（三重铁证）；阈值已恢复默认 78/85
- M5-v2 仪器：batterystats 全量 471KB + 摘要入档（Estimated power 基线：
  CPU 1361mAh/screen 752/总放电 2825mAh）；单测 +4（实跑 190 全过）

## v0.19.1（versionCode 1901）— 看板接 Evidence 四源展示
- batch 加 pw/th/fps/thr 四段 + Evidence 采样卡（电量/CPU热点/帧率/AppOpt规则）
- `node --check` 语法 + KSU WebUI 真机截图验收四行真数据；同步设备 module.prop

## v0.19.0（versionCode 1900）— AppOpt 指挥链②：画像核组生成（URO-GEN 真规则）
- 画像扩展第四字段 `pattern|down|up|cpuset`（e-core/p-core/hp-core/组名）；
  `reconcile_uro_gen` 60s 对账 → 生成 `<pkg>=<核组>` 写入 URO-GEN 区块
- 对账语义：行集相同不动（幂等）；**EXEMPT 兼容**（标记行 strip 后比较=豁免态不被复活、
  重写时保持标记）；区块外用户规则零改动（原子写+备份）
- 通配符画像保守跳过（AppOpt 通配语义未验，不生成+静默计数）；无画像文件=安全 0
- 单测 +10（第四字段/区块边界/幂等/EXEMPT兼容/热改重生成），186 全过
- 真机三步闭环：无画像幂等空区块 → 注入画像 60s 生成 `com.coolapk.market=p-core`
  → 删画像 60s 区块复空、conf 110 行零损坏

## v0.18.0（versionCode 1800）— AppOpt 指挥链①：GAME 自动豁免
- `exempt_game_rules` / `restore_exempted`（appopt.cpp，原子写 tmp+rename、首次备份、幂等）：
  进 GAME 把活跃游戏规则注释并打 `# [URO-EXEMPT]` 标记；出 GAME 按标记还原
- **只碰活跃行**：用户手改的注释行永不触碰（10-07 王者手动豁免语义的自动化）
- 挂钩 `run_policy` 的 changed 门（天然边沿）+ `uro.conf` 开关 `APPOPT_GAME_EXEMPT`
  （默认开，=0 关）；plog 记 `APPOPT-EXEMPT +N/restore N`
- 单测 +9（豁免/标记/幂等/不动用户行/还原/还原幂等），176 全过
- 真机：部署零误伤（conf 标记 0、110 行完整、四档正常）；GAME 闭环待下次游戏触发验收

## v0.17.2（versionCode 1702）— 电量 Evidence 转正 + 崩溃心跳定位
- `evidence_driver` 心跳定位器 `ev_hb`（8 处阶段标记 → `evidence.hb`）：
  f65a014b 崩溃无现场的补救——再崩可读最后阶段精确定位
- `battery_json` status 截断防御（sysfs 异常兜底）
- 复现观察：部署后 25+ 分钟零崩溃（历史崩溃在 11 分钟）、电量行连续、
  心跳随深睡暂停/唤醒恢复（18min gap=suspend 正常行为）
- **根因仍未定罪**（25min 未触发复现条件）——长期观察中，心跳装置值守
- 临时 `power_sampler.sh` 与内置电量双轨并行（转正验收后再择一）

## v0.17.1（versionCode 1701）— M5 收官 + 文档补账
- 内嵌 COSMemory 联动包升级 v1.1.5 → **v1.2.1**（存在判定逻辑不变，用户确认定稿）
- M5 正式报告归档 `docs/M5_报告_v1.md`：A/B/M0 三方对照，pct 与电流口径矛盾如实呈现，
  按协议判「样本不足只出观察」——不宣布方向性结论；架构工程目标全达成（enforce 双向 100%、零事务事故）
- CHANGELOG 补 v0.15-v0.17 欠账；README 功能清单更新

## v0.17.0（versionCode 1700）— Evidence 扩展（温度/FPS/电量）
- 新 `evidence.cpp`（host 可测）+ `evidence_driver(t8)`：60s 双采样
- 温度：thermal_zone top3 + battery → `thermal.jsonl`；FPS：gfxinfo 前台包差分 → `fps.jsonl`
- 电量（M5 分母）：`battery_json` capacity/status/current_now 拼入 thermal 行；
  崩溃版实机循环后由独立 `power_sampler.sh`（service.sh 幂等拉起）补位
- 单测 167 全过；M5 协议 `docs/M5_收益实测协议_v1.md` 落档

## v0.16.0（versionCode 1600）— ThreadController Phase-1 + COSMemory 联装
- AppOpt 路线 a（零改代码）：`appopt.cpp` 纯逻辑 + `thread_evidence_driver(t7)`
- ① Thread Evidence：60s 轮询五组 cpuset → `thread.jsonl`（首测 rules=66）
- ② 约束源解析（#// 注释语义同作者）③ GAME 豁免校验只读告警 ④ URO-GEN 空框架+备份
- COSMemory 联动 `customize.sh`：目录存在即跳过、缺失才 `ksud module install`、失败绝不 abort

## v0.15.0（versionCode 1500）— 两阶段事务 + 屏幕自适应
- 双状态文件 `bridge.state`/`cpu.state`：写前 PREPARE → 写 → 写后 COMMIT，崩溃任意点 probe 归还
- `focus_driver` 熄屏跳过 1s dumpsys 巡检（省 2% 单核）、2s 醒一次，亮屏恢复（`g_screen_on`）
- 独立 `MemoryController`/`CpuController` 2PC 单测 53 条

## v0.14.0（versionCode 1400）— App 画像
- `app_profiles.txt` 通配符定制：`pattern|down|up`（-1 跟随档位、热改热生效）
- 优先级 `画像 > 档位 > 基线`；GAME / POWER_SAVE 安全档不接受画像覆盖
- why 审计追加 `profile=<pattern>`；单测 +11（141 总）
- 真机：`io.github.mangi.eta|200000` 命中实证（applied=1 首写 → 后续幂等）

## v0.13.0（versionCode 1300）— AmSwitch → FAST
- 前台切换 2.5s 过渡窗 → FAST（降频迟滞 150ms），**压过 Touch**（CT 原版优先级）
- 空闲 tick 回落；优先级链 `GAME>熄屏>压力>BOOT>FAST>Touch>BALANCE`
- 测试适配 4 处（FG 窗口改变旧预期）

## v0.12.1（versionCode 1201）— PERFORMANCE 档补完
- 空包终结：`down_rate_limit_us 0→80000`（80ms，governor 通用 rate_limit 语义）
- `cpu.state` 增 `DOWN:` 档案 + 升级 seed 补档 + 崩溃归还包括 down
- 修「补档与崩溃恢复 else-if 互斥」（单测抓出，depth 案同款重犯）
- 真机四轮点屏：往返全部 `applied=1`

## v0.12.0（versionCode 1200）— Touch → PERFORMANCE
- 新 `touch_driver`（touchpanel → BTN_TOUCH 边沿，节流）
- 亮屏 + 1.5s 滞回；回落由空闲 tick 触发（Touch 后无事件可等）
- 修幂等门吞审计：telemetry 增 `applied` 字段（1=真写入、0=仅切档）
- 修 C++ 单引号多字符字面量 bug（JSON 字面量输出成十进制）→ `-Werror=multichar` 设防

## v0.11.0（versionCode 1100）— 四档自治 + WebUI 看板
- 四档模型：`POWER_SAVE/BALANCE/PERFORMANCE/FAST` + GAME/压力两正交态
- **去系统 mode 依赖**：mode_config_driver 停用；省电/高性能模式双实测双向零影响
- 新增 `webroot/` 看板（ksu.ts 桥 + 奶油粉 Nexus 样式 + Solar 图标 + enforce 总闸）
- 熄屏自动省电（v0.10）等前置：`debug.tracing.screen_state` 亮=2 熄=1 语义修正

## v0.10.0 及更早
- v0.10.0 场景自动映射（熄屏→省电、亮屏→均衡）
- v0.9.0 telemetry 证据链 + 死亡归因（`--attribute`，UNKNOWN 不伪造）
- v0.8.0 CPU uag 接管（`up_rate_limit`）
- v0.7.0 Memory 深度接管（`depth`/`cooldownSec` + 多字段档案）
- v0.6.0 KSU 模块化（自启/看门狗/崩溃残留归还）
