# cos 智能调度与内存管理（UnifiedRootOptimizer）

> Android Root 系统级资源协同调度框架 —— COSMemory × 线程优化的**统一策略大脑**
>
> 一个框架、多个执行器、一个策略大脑。当前 **v0.29.0**：四档自治 + 智能感知 + **AppOpt 指挥链**（GAME 豁免 / 画像核组生成）+ **Thermal 夹档**（含趋势预判）+ **GPU/KEEPADJ 档位联动** + **L4 反馈闭环**（电量守卫 / 温度趋势 / 帧率哨兵）+ uag hispeed_load + Evidence 六源 + 看板（含**画像可视化编辑器**），已实机运行于一加 11（ColorOS 16）。

## 它是什么

URO 不替换你的模块，而是接管**决策**：

```
                URO（决策：判档 → 生成策略 → 约束求交 → 写入 → 归还）
                    │
     ┌──────────────┼──────────────────┐
     ↓              ↓                  ↓
  COSMemory      内核 uag          GPU/热控
 （执行器，      （直接写，         （占位，
  名单/防线不动）  URO 唯一写者）      M5 接入）
```

- **内存**：URO 写 `memory.json`（aggressive/depth/cooldown），COSMemory 引擎照常执行
- **CPU**：URO 直接写 `uag/up·down_rate_limit_us`（设备现值 0、无认领、可逆）
- **线程优化（CoreTurboScheduler）**：不部署本体，只搬设计（场景机/画像），URO 自己实现

## 四档自治（第1步）

| 档位 | 触发（全自身感知，不读系统 mode） | 参数 |
|---|---|---|
| `POWER_SAVE` | 熄屏 | 回收+冻结+maxKill3 + 升频延迟 2500μs |
| `BALANCE` | 亮屏默认 | **完全不写**（=你的基线） |
| `PERFORMANCE` | 触摸（1.5s 滞回） | 降频迟滞 80ms |
| `FAST` | 切 App（2.5s 窗，压过 Touch） | 降频迟滞 150ms |

两个正交开关：**GAME = 全量让权**（零写入）、**MEMORY_PRESSURE = 彻底回收叠加**（15s 滞回）。
优先级链：`GAME > 熄屏 > 压力 > BOOT > FAST > Touch > BALANCE`，全部真机实测。

## 智能感知与画像（第2步）

- **touch_driver**：监听 `touchpanel` 设备 `BTN_TOUCH` 边沿（每下触摸仅 2 事件，天然节流）
- **App 画像**：`log/app_profiles.txt` 每行 `通配符|down|up|cpuset`，**画像 > 档位 > 基线**（首个命中生效，文件顺序即优先级）：

```text
# -1 = 跟随档位；# 注释；热改热生效
com.tencent.mm*|200000|-1      # 微信系：降频迟滞 200ms
*.speedtest*|250000|500        # 测速类：更粘
com.coolapk.market|-1|-1|p-core  # 第四字段：指挥链② 60s 生成 AppOpt 核组规则
```

- **看板可视化编辑（v0.29.0）**：KSU WebUI → 「App 画像（可编辑）」卡增/改/删/调序，原子写 + 自动备份，
  写前校验、写后回读；第四字段填好后 60s 内可在卡内看到指挥链② 生成的 `包名=核组`
- 通配画像只走策略热读，**指挥链② 保守跳过**（AppOpt 通配语义未验）

安全边界：GAME（让权）与 POWER_SAVE（省电）**不接受画像覆盖**。

## 执行器接入与 Evidence（v0.15-v0.17）

```text
URO（决策大脑）
 ├─ memory.json 四旋钮 ──▶ COSMemory 引擎   ← 决策级接入（2PC：PREPARE→写→COMMIT）
 ├─ uag rate 参数 ──────▶ CPU 三簇           ← 档位化 up/down 迟滞
 ├─ 观察 AppOpt ────────▶ thread.jsonl       ← 路线a：66规则/五组cpuset/豁免校验/URO-GEN框架
 └─ Evidence 采样（60s）─▶ thermal+power.jsonl（温度top3/电量pct/chg/mA）
                          fps.jsonl（gfxinfo 前台差分）thread.jsonl（cpuset组成员）
```

- **总闸**：`uro.conf` 的 `BRIDGE_ENFORCE`（热读；A/B 实测 946/946 零泄漏）
- **COSMemory 联装**：刷入包内嵌联动（目录存在即跳过、缺失才装、失败不 abort）
- **M5 实测**：A/B/M0 三方报告见 `docs/M5_报告_v1.md`（口径矛盾如实呈现，不出方向性结论）

## 可靠性与可观测

- KSU 模块自启 + 30s 看门狗 + **崩溃残留自动归还基线**（`bridge.state`/`cpu.state` DIRTY 状态机）+ 卸载兜底
- **telemetry.jsonl**：每次档位/写入一条（`action_id`/gen/scenario/why/**applied**——applied=1 真写入、0 仅切档）
- **死亡归因** `--attribute <pid>`：证据不足输出 `UNKNOWN`（exit 2），绝不伪造
- WebUI 看板（模块自带 `webroot/`）：当前档/状态点/总闸/时间线/写入证据/**画像编辑器**——与 COSMemory 面板同源（桥=ksu.ts、样式=奶油粉 Nexus、图标=Solar sprite）；画像卡内联 Vue 3（无外网依赖）

## 与系统的关系（双向隔离实测）

系统省电/高性能模式开关实测 8-25s：**telemetry 零新增、我们的节点纹丝不动**——地盘不相交（不碰 governor/频率上限/PowerHAL），判档不读 mode。不 hook 系统，系统模式留作兜底。

## 安装与使用

```bash
# 发布包：手机 /storage/emulated/0/性能调度模块/uro-vX.Y.Z.zip（KernelSU 直接刷入）
# 看板：KSU 管理器 → 模块 → UnifiedRootOptimizer → 打开
# 总闸：看板或 COSMemory 面板的「统一调度桥接开关」（同一 uro.conf，关=瞬间归还基线）
# 审计：cat /sdcard/Android/UnifiedRootOptimizer/log/telemetry.jsonl
```

## 开发与测试

```bash
# 全量单测（host 构建，无需设备）—— 当前 167 断言
sh runtime/tests/run_test.sh            # all | bridge | m2 | m3

# NDK 构建（需 binfmt/qemu，见 docs/M0_构建链验证_v1.md）
export ANDROID_NDK=... && runtime/build.sh

# 打包模块（module/ + 二进制 → dist/uro-vX.Y.Z.zip）
python3 runtime/tools/pack_module.py
```

## 路线图

| 阶段 | 内容 | 状态 |
|---|---|---|
| A 可靠性 | 自启/看门狗/崩溃归还/卸载兜底 | ✅ |
| C 可观测 | telemetry + 死亡归因 | ✅ |
| 第1步 四档收敛 | 四档自治、去系统 mode、WebUI 看板 | ✅ |
| 架构定案实施 | AppOpt 路线a（ThreadController P1）+ COSMemory 1+2 联装 | ✅ |
| Evidence 扩展 | 温度 / FPS / 电量三源采样 + M5 协议与报告 | ✅ |
| AppOpt 指挥链 | URO-GEN 真规则 + GAME 档协调 | ⬜ 下一阶段 |
| 第2步 智能感知 | Touch/AmSwitch/App 画像（CT 能力搬运） | ✅ |
| 第3步 D | 测试套件 + 0.x 正式发布 | 🔄 本阶段 |
| 第4步 E | cpuset 掉帧实验 / T-OBS 热观测 / **M5 收益实测** | ⬜ |

**性能收益（帧率/功耗）以 M5 对比数据为准**——M0 基线已留存（日常 117min/468 帧、待机 6.09h 耗 3.0%），当前一切参数效果属设计意图，未实测前不作承诺。

## 文档

- 设计基线：`docs/UnifiedRootOptimizer_设计文档_开发计划_验收标准_v1.1.md`
- 里程碑报告：`docs/M0_*` ~ `docs/M4_*`（含阶段A/C、四档、画像等验收记录）
- 上游与许可：`THIRD_PARTY_NOTICES.md` · LICENSE = GPL-3.0
