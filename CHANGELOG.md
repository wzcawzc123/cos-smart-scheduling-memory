# Changelog

版本规范：`大.中.小`——功能迭代升第二位、小修升第三位、大重构升第一位；versionCode 每次 +1。
发布件归档：`/storage/emulated/0/性能调度模块/`（新包根目录，旧包挪 `旧版/`）。

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
