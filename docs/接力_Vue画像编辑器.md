# 接力任务：URO 画像编辑器（Vue 前端）

> **✅ 已完成（2026-10-11，v0.29.0）** —— 本文不再作为开工单，仅留档。落地与验收见
> `docs/看板画像编辑器_v0.29.0.md` + CHANGELOG v0.29.0 + commit `ae16c69` + release `v0.29.0`。
> 离线回归：`node runtime/tools/webui_profile_test.mjs`（29 断言，无需设备）。
> 下一棒在 CHANGELOG/记忆里：B 组三件（GAME 闭环验收 / A2 复测 / cpuset 矩阵）。

> 建立 2026-10-11 · 前任 Eta 交接 · 项目 UnifiedRootOptimizer（`/workspace/cos-sched-memory`）
> **新会话开工前请先读本文 + MEMORY.md 的 2026-10-11 各节**

## 一句话任务

在 URO 的 KSU WebUI 看板里加「**画像编辑器**」：可视化增删改 App 画像，替代手工编辑文本文件。

## 背景（必读）

- **URO** = Android Root 系统级调度框架（策略大脑），当前 **v0.28.1**；设备一加11 / ColorOS16；KSU + LSPosed
- **看板** = KSU WebUI，**单文件** `module/webroot/index.html`（原生 JS，约 27KB）——本任务要引入 Vue
- **画像文件** = `/sdcard/Android/UnifiedRootOptimizer/app_profiles.txt`
  - 格式：`pattern|down|up|cpuset`（四字段，后两可省/可空；以 `#` 开头为注释）
  - `pattern`：包名，**支持通配符**（如 `com.x.*`）
  - `down`/`up`：uag 调频延迟微秒（`-1`=跟随档位）
  - `cpuset`：`e-core`/`p-core`/`hp-core` 或核组（空=不生成规则）
  - 前三字段被 Policy **热读**（优先级：画像 > 档位 > 基线）；第四字段被**指挥链②** 60s 对账生成 AppOpt 规则
- 文件**不存在也是合法状态**（=无画像）

## 技术约束（都是踩过的坑，务必遵守）

1. **KSU WebUI 桥**（`br.exec`，实现见 index.html 中 `readCmd`/`splitSeg`）
   - `ksu.exec` **只回传最后一行** → 多行输出必须 `| base64 | tr -d '\n'` 再由前端解码
   - 分号聚合的大命令会**整包丢数据** → 一律拆**单行命令**，`Promise.allSettled` 并发
   - 命令数组 `join(';')` 时**每段不得带尾分号**（否则 `;;` 语法错 → 整条失败）
   - 分段标记模式：`'echo @@SEG:name@@;命令'` → 前端 `splitSeg` 解析成对象
2. **Vue 引入方式（关键决策）**
   - KSU WebUI 是本地环境，**外网 CDN 不可靠** → **内联 `vue.global.prod.js`**（约 130KB，无构建步骤，`Vue.createApp`）
   - 内联进 `index.html`（保持单文件）；文件体积增至 ~160KB 可接受
   - 若验证发现 WebUI 可稳定联网，可改 CDN + 内联兜底（**先验证再选**）
3. **写文件必须原子 + 备份**（画像会被 60s 对账读，半写文件危险）
   - 读：`base64 <file>`（一次拿全量）
   - 写：`cp` 备份 → `echo <b64> | base64 -d > file.tmp` → `mv` 原子替换
   - 保持原属主/权限（先 `ls -l` 看现状）
4. **前端校验**（写前拦）
   - pattern：非空、无空白字符；down/up：整数或 `-1`；cpuset：`e-core|p-core|hp-core|^\d+-\d+$`
5. **风格**：奶油粉 Nexus 风 + Solar 图标 sprite（已内联在 index.html，用 `<use href="#i-xxx"/>`）
   - 参考真源码：COSMemory 面板（先在 `/workspace/COSMemory/` 下 grep 定位 `panel` 目录的 index.html/style）
6. **验收与发版流程**
   - `node --check`（抽 `<script>` 段校验语法）→ 部署：`/sdcard` 中转（linux 侧看不到 `/data/adb`）→ `cp` 到 `/data/adb/modules/uro/webroot/index.html`
   - KSU 打开看板 → 真机截图验收（`observe_screen` + `launch_app me.weishu.kernelsu` → 模块 → URO → 打开）
   - 发版：`module.prop` 版本+1（纯 UI 增强→第三位；新能力→第二位）+ `CHANGELOG.md` + `python3 runtime/tools/pack_module.py`
   - 发布件：`/storage/emulated/0/性能调度模块/`（新版入根，旧版挪 `旧版/`）→ `git commit && push` → 设备 `module.prop` 同步

## 交付清单

1. `index.html` 内联 Vue + 新增「画像」区域（Tab 或独立卡片）
2. 表格：展示现有画像行（pattern / down / up / cpuset + 中文语义提示）
3. 增/删/改表单 + 校验 + 原子写回（含备份提示）
4. 真机截图验收 + 发版（**v0.29.0**）+ CHANGELOG + Release（`gh release create`）
5. **纯 UI 任务：不改任何调度代码**

## 关键路径

| 用途 | 路径 |
|---|---|
| 仓库 | `/workspace/cos-sched-memory`（github: wzcawzc123/cos-smart-scheduling-memory） |
| 看板源 | `module/webroot/index.html` |
| 画像（设备） | `/sdcard/Android/UnifiedRootOptimizer/app_profiles.txt` |
| 发布文件夹 | `/storage/emulated/0/性能调度模块/` |
| 工具 | `runtime/tools/deploy.sh`（原子部署）、`runtime/tools/pack_module.py` |
| 记忆 | MEMORY.md「2026-10-11」各节（项目全史 + 各轮教训） |

## 不要做的事（铁律）

- 不碰 governor / 频率上限 / 关核 / Oplus 声明为非独占的节点（target_loads、hispeed_freq）
- 本任务不碰调度逻辑（纯前端）
- 中文交互、诚实报告（失败/负结果如实写，不美化）
- 部署一律用 `deploy.sh`（原子替换，防 watchdog 自杀）
