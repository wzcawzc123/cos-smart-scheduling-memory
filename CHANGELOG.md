# Changelog

版本规范：`大.中.小`——功能迭代升第二位、小修升第三位、大重构升第一位；versionCode 每次 +1。
发布件归档：`/storage/emulated/0/性能调度模块/`（新包根目录，旧包挪 `旧版/`）。

## v0.39.0（versionCode 3900）— 漂移重写守护 + 温度源治理 + WebUI 异步真正落地

### 核心：厂商会改写我方夹频（本机实测）
- 实测（一加11 / ColorOS 16）：不干预时 `policy0.scaling_max_freq` 自己就在
  `2016000 / 1785600 / 1459200` 之间摆动；写入 1670400 后 3 秒内被改回。三簇
  （policy0/3/7）均如此 —— **一次性写 `scaling_max_freq` 在 ColorOS 上形同虚设**。
- 新增 **漂移重写守护（drift reconcile）**：策略未变的空闲节拍上读回核对
  我方要求约束的标量节点（`maxF0/maxF3/maxF7`、`minFreq`、`uagSoft`、`uclamp*`），
  仅在**真漂移**时重写，零漂移零写入。
  - 周期自适应：基础 5s；检出漂移 → 2s 加速抢占；连续 6 轮无漂移退回 5s
  - 温控/低电守卫优先：`thermalLevel>0` 或电量 <20% 期间不抢写（交回厂商限制）
  - 可配：`uro.conf` 的 `RECONCILE_MS`（默认 5000，0=关闭）、`RECONCILE_FAST_MS`（默认 2000）
  - 遥测：`telemetry.jsonl` 增 `{"reconcile":1,"drift":N,"total":M}`；事件日志 `RECONCILE drift=...`
- 顺带修正：`applyScalar` 写入前按**簇硬件上限**（`cpuinfo_max_freq`）clamp，
  避免配方写超上限被内核 EINVAL 静默丢弃（例如把 2803200 写给只有 2016000 的小核簇）。

### 修复：温度源原来是“功放”不是 SoC
- `thermal_zone0` 在本机 type 是 `pa`（功放温度）。旧实现扫全部 zone 取最大值，
  会混入 battery / shell_front / wireless / 未校准(-273000) 等传感器 → 热级误判。
- 现在只采纳 SoC 类传感器（`cpuss-*` / `cpu-*-*` / `gpuss-*`）并过滤无效值；
  无此类命名时回退旧行为。`ThermalPlaceholder` 改为按 type 探测（本机命中
  `thermal_zone36` = `cpu-1-1`）。

### 修复：WebUI 桥“异步优先”其实没生效
- v0.38.0 更新说明称已改为 `execAsync` 优先，但 `detectBridge()` 原顺序是先遍历
  `[ksu, WebUIX, mmrl]` 找同步 `exec`，`execAsync` 永远轮不到 —— 点导航/切档仍卡 1~2s。
- 已把 `ksu.execAsync` 提到最前（并兼容其他实现的 `execAsync`），同步 `exec` 仅作回退。
- 模块目录自适应：读 `module.prop` 时依次尝试 `modules/uro`、`modules_update/uro`、
  `modules/UnifiedRootOptimizer`，改 id / APatch 路径不再显示空版本。

### 增强
- 看门狗熔断：10 分钟内连续崩溃 5 次 → 退避 5 分钟（避免崩溃循环烧电/刷日志）；
  稳定存活 180s 清零计数；守护日志与 daemon 日志在常驻循环内滚动（512KB / 2MB）。
- 内嵌 COSMemory 升到 **v1.3.0**（调参观测窗 / SKIP 分桶 / 看门狗熔断）；
  打包脚本不再写死版本号，取 `/workspace` 下版本号最高的 `COSMemory_v*.zip`。

### 已知限制
- `scaling_max_freq` 的“向上抬升”仍属抢写：ColorOS 改写比 2s 更快时只能部分生效。
  需要**稳定上限**时优先用 `uag/soft_limit_freq`（实测写入 12s 不被改写）；
  要更激进可把 `RECONCILE_FAST_MS` 调到 1000（会增加与厂商调度的对抗耗电）。
- 三簇仍按 `policy0/3/7` 命名（本机实测簇为 cpu0-2 / cpu3-6 / cpu7），跨 SoC 自动聚类留待下一版。

## v0.38.0（versionCode 3800）— WebUI 桥改异步（宣称）
- 更新说明：`ksu.execAsync` 优先、`exec` 仅作回退；实际 `detectBridge` 顺序写反（v0.39.0 修复）
- WebUI 单文件化 + 启动骨架屏

## v0.37.0（versionCode 3700）— 看板白屏修复
- `app.js` 裸用 `ref()`（应为 `R()`）导致 setup 抛错、Vue 挂载中断、`.page` 拿不到 `on` 类而整页隐藏
- WebUI 单文件化（内联 Vue + uPlot + CSS + JS，零外部依赖）

## v0.36.0（versionCode 3600）— GPU 补全 + uclamp + 配方编辑器
- **GPU 补到 7/9 节点**：新增 `thermal_pwrlevel`（温控档位）、`min_clock_mhz`、`max_clock_mhz`
  · 复用 v0.33 的通用 applyPwr（写入/回基线/幂等/dry-run）
  · 本机 GPU 频点表：680/615/550/475/401/348/295/220 MHz（曲线测量用）
- **uclamp 接入**：`sched_util_clamp_min` / `_max` / `_min_rt_default`（0..1024）
  · 本机节点存在（默认 1024/1024/0），性能档可抬 util 下限、省电档可压上限
  · 走 CPU 侧"独立标量组"（与 min_freq / soft_limit 同一机制，零新机制）
- **WebUI 配方编辑器**：新增「场景配方」卡片（文本框 + 重载 + 保存），直接编辑 `uro_tactics.conf`
- **README 对齐**：补 v0.31→v0.36 版本表，头部描述更新（此前停在 v0.30.0）
- 测试：302 → **312 断言 0 失败**（新增 10 条：uclamp 全局节点 + GPU 三节点）
- 兼容：全部新字段默认 `-1` → 不写 `uro_tactics.conf` 时行为与 v0.35.0 一致

## v0.35.0（versionCode 3500）— 三簇独立夹频（per-cluster）+ 对照组 B
- **per-cluster 上限**：`maxFreqK`（policy0 / 小核）+ 新增 `maxFreqK3`（中核 4 核）、
  `maxFreqK7`（大核 X3）；三簇各自独立，统一由 `cpuClamp` 总开关控制；`-1` = 回基线
  · CpuController 改用 v0.33 引入的「独立标量组」机制统一处理（写入 / 回基线 / 幂等 / dry-run）
  · 背景：此前夹频只写 policy0，导致"每簇独立上限"的方案（含所有同类项目的做法）无法落地
- **对照组 B（NovaSched 映射）**：新增 `docs/对照组B_NovaSched参考_v1.md` +
  `docs/对照组B_uro_tactics.conf`
  · 参考同类项目 NovaSched Zen Edition（作者 **ZenJooo**，GPL-3.0）公开文档中的真机逐频点
    方法论与 SM8650 四档实测值，映射到本机 SM8550 频点表（百分比 → 向下吸附真实频点）
  · 三组 A/B 实验设计：A = 我们的默认；B = NovaSched 映射值；C = 自测功耗曲线推导值（待测）
- 测试：295 → **302 断言 0 失败**（新增 7 条 per-cluster 用例：三簇独立写入/幂等/回基线）
- 兼容：不写 `maxFreqK3/K7` 时行为与 v0.34.0 一致

## v0.34.0（versionCode 3400）— 手动档位（WebUI 可切）+ 退出诊断
- **手动档位**：WebUI 新增「运行档位」卡片，五档可选：自动 / 省电 / 均衡 / 性能 / 极速
  · 落盘 `/sdcard/Android/UnifiedRootOptimizer/manual_mode.txt`，**改即热生效**（mtime 热重载，与配方同机制）
  · 安全设计：手动档**只覆盖"场景判定"这一步**——温度夹档、电量守卫等安全阀仍然生效；
    **游戏让权优先**（§5.5 铁律不破：自动判定为 GAME 时手动档不生效）
  · 重启后保留（文件存盘）；切回「自动」即恢复自适应
  · 总闸（BRIDGE_ENFORCE）关闭时按钮禁用并提示，避免误判
- **退出信号诊断**：`on_stop` 记录收到信号号（SIGHUP=1 / SIGINT=2 / SIGTERM=15），
  写入 `events.log`（`UroExitBySignal sig=N`）+ stderr
  · 背景：体检发现两天累计 46 次优雅退出；已排除模块脚本 kill、COSMemory（用 am kill）、OOM；
    多数经核实为**设备重启后 watchdog 正常补拉**，剩余两段（10-13 18:24/19:39）待信号号定位
- 测试：288 → **295 断言 0 失败**（新增 7 条手动档用例：强制/回落/why 标注/游戏让权优先）
- 兼容：无 `manual_mode.txt` 时行为与 v0.33.1 完全一致

## v0.33.1（versionCode 3301）— 熄屏识别修复（省电档复活）
- **根因**：`screen_driver` 启动时 `__system_property_find("debug.tracing.screen_state")`
  失败即**永久挂起** → 熄屏永不识别 → POWER_SAVE 永不触发
- **实锤**（2026-10-13 日用体检）：25h 零 ScreenChanged 事件、零省电档；
  prop 实为**开机后被系统写入**（晚到），旧代码没有再轮询 find
- **修复**：主循环每轮 re-find prop（处理晚到）；始终缺失 → 降级读取
  `/sys/class/backlight/panel0-backlight/brightness`（>0 = 亮屏），并记一次
  `ControllerFault(DEGRADED, fallback=backlight)`；两者都不可用则 500ms 重试，绝不挂死
- 测试：288 断言 0 失败；需真机验证（重启后应看到 ScreenChanged off → POWER_SAVE 触发）

## v0.33.0（versionCode 3300）— 参数外置 + 旋钮扩容
- **参数外置**：场景配方抽到 `/sdcard/Android/UnifiedRootOptimizer/uro_tactics.conf`
  · INI 风格（`[场景]` + `key=value`），**改完保存即热生效**（mtime 热重载），不再重编译刷模块
  · 整份接受或整份拒绝：任一行非法 → 整份不生效并记 `events.log`（`TacticsConf REJECT`）
  · 上机前自检：`sh runtime/tools/check_tactics.sh <文件>`（生产解析器校验，退出码 0 = 合法）
- **cpuClamp 解封**：配置写 `cpuClamp=1` + `maxFreqK=...` 即启用 CPU 夹频（写入通路早已存在）
- **GPU 边界**：GpuController 1/6 → 4/6 节点（`min/max_pwrlevel` + `bus_split`），
  并补上「回基线」语义（此前 `idle_timer` 只有"不写"、没有"归还"）
- **Thermal 对齐**：`ThermalPlaceholder` → 热状态观察器（status 暴露热级）；
  新增 `docs/v0.33_对齐全景.md`（逐层对照 + 未启用槽位清单 8 项）
- **旋钮扩容**：新增 6 个可调旋钮（全部默认关）
  · CPU：`cpuMinK`（抬下限）、`uagSoftK`（uag 软上限 = OPLUS 预留口）
  · GPU：`gpuDefPwr` / `gpuMinBw` / `gpuForceClk` / `gpuForceBus`
  · 勘察结论：QTI `msm_performance` 在本机为空 → 不接入
- 测试：199 → **288 断言 0 失败**
- 兼容：所有新字段默认 `-1`/空、配置默认全注释 → **行为与 v0.32.0 逐字段一致**

## v0.32.0（versionCode 3200）— 内嵌双模块（COSMemory v1.2.2 + AppOpt v2.2.6）
- **COSMemory 内嵌更新至 v1.2.2**（含 keepAlive.adj 热读修复 —— 使 URO 的 KEEPADJ 档位联动
  从"写入空转"变为真正生效；修复内容见 COSMemory 仓库 314e27c）
- **AppOpt（线程优化）内嵌**：刷 URO 一键装齐三个模块（体验诉求）
  · **合规**：AppOpt 原创 SutoLiu（GPL-3.0）——仅再分发**官方原版 zip**（零修改）、
    保留署名与许可、附来源（github.com/SutoLiu/AppOpt）
  · **存在判定**：已装则跳过（尊重用户自装/更新版本）；失败仅提示绝不 abort
  · 未内嵌时自动降级（URO 线程约束源缺失提示）
- 至此 URO 包 = 调度大脑 + 内存执行器 + 线程执行器（一键装齐，各自可单独升级）

（versionCode 3100）— 热管理防震荡 + L2 语义修正（Eta 卡顿排查副产品）
- **热级 dwell 锁定期**（drivers.cpp）：热级进入/升级后 3 分钟内**只升不降**——
  修复真实缺陷：温度在 T1 边界反复穿越导致热级震荡（实测修复前 10 分钟跳 6 次
  0↔1↔2）→ 档位秒级改写 → 调速器参数写入风暴
- **L2 语义修正**（policy_manager）：过热级不再压到 POWER_SAVE，只压到 BALANCE——
  系统 thermal-engine 已在做硬件降频，URO 再叠省电档 = 重复惩罚
- 迟滞 6°C → 8°C（减少边界穿越）
- **Eta 卡顿排查结论**（重要留档）：经完整取证（gfxinfo 官方指标 + 601 帧窗口对照 +
  exit-info + cgroup/adj + 模块配置），确认 **Eta 卡顿属其自身渲染问题**
  （中位 jank 16.9% vs 酷安 1.2%；99th 150ms；输入延迟 18.8 万次），
  **与 URO/COSMemory/AppOpt 无关**（六项排除证据）；诊断数据包交付于
  `/storage/emulated/0/ETA渲染数据/`。本轮临时兜底（THERMAL_ENABLE=0、freeze=false）
  **均已回退**，保留的仅上述两项真缺陷修复
- 单测 199 全过（[thermal] L2 断言随语义更新）

## v0.30.0（versionCode 3000）— 看板重构：五分区 + 真图表（多文件）
- **信息架构**：单列长滚动 → **底栏 5 分区**（iOS 药丸导航 + Solar 图标）
  - 概览：当前档大卡（语义色）· 指挥链四灯（内存/CPU/GPU/热控，取自 telemetry.ctrl）· 健康三灯（基线/看门狗/心跳）· 三条迷你 sparkline · 接管总闸
  - 趋势：**uPlot 真图表 5 张**（温度 / 卡顿率+帧数 / 内存+PSI / GPU 占用+频率 / 电量+电流），1h·6h·24h 切换、**双 Y 轴**、可拖动、点按出值
  - 画像：v0.29.0 编辑器整页化（逻辑零改动，回归测试保持 29 断言全绿）
  - 证据：**档位热力色带**（40 格按桶取多数档，深浅 = 主导占比）· 前台 App 占比（行式 + 进度条）· 核组线程堆叠条 · 指挥链② 生成规则 · uag 参数 · 最近 20 条决策明细
  - 关于：动态版本 / 优先级说明 / 日志路径 / 安全边界
- **工程结构**：单文件 218KB → **多文件**（已实证 KSU 支持：同机 mountify/AdGuardHome 同为 `webroot/ + assets/`）
  `index.html`(35KB 壳+模板) · `style.css`(16KB 设计系统) · `app.js`(39KB 应用) · `assets/{vue.global.prod.js, uplot.iife.min.js, uplot.min.css}`
- **设计系统**：奶油粉 Nexus 浅色 + **夜粉深色**（跟随系统，实测截图通过）；卡片/分段控件/骨架/底栏一套变量；Solar 图标 sprite 25 枚（沿用 COSMemory 面板资源 + 同套补齐 3 枚）
- **性能**：按 Tab 取数（概览仅 6 条命令）· sed 抽 CSV + awk 降采样把窗口压到 ≤800 点 · 页面不可见暂停轮询 · 图表仅在激活页构建
- **本轮真机修掉的问题**（都有据）：
  1. `bridge.state`/`cpu.state` 是**多行文件**，用 `sh()`（只回最后一行）导致基线显示 `DIRTY ?`、uag 表空 → 改 `rd()`（base64 全量）
  2. **uPlot 时间轴按 Unix 秒**，喂毫秒 → X 轴显示 1970 年；统一 `xsec()` 换算
  3. in-DOM 模板里 `<use :href="'#'+id">` **动态绑定不生效**（图标空白）→ 底栏 5 个图标改静态引用
  4. 档位色带 200 条挤成摩尔纹（真机档位逐条翻，合并连续段仍上百段）→ 按桶取多数档 + 占比定深浅
  5. 前台包名列在窄列里折行 → 改「包名整行 + 下方进度条」
- 测试：`node runtime/tools/webui_profile_test.mjs`（已适配多文件，从 app.js 载入）**29 断言全过**；`node --check` 三段脚本

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
