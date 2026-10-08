# Changelog

版本规范：`大.中.小`——功能迭代升第二位、小修升第三位、大重构升第一位；versionCode 每次 +1。
发布件归档：`/storage/emulated/0/性能调度模块/`（新包根目录，旧包挪 `旧版/`）。

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
