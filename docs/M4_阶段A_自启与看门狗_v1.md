# M4 阶段A — 开机自启 + 看门狗 + 崩溃残留恢复（v0.6.0）

> UnifiedRootOptimizer M4 · 2026-10-08 · 依据 §9 M4（watchdog/death attribution）、§7 可靠性设计
> 动机：M3b 把 enforce 交给面板开关后，URO 仍是临时进程——**开关已开、守护无保障**
> 退出判据（阶段A 自定）：① 重启自动拉起 ② 死亡 30s 内补拉 ③ 崩溃残留配置自动归还

## 1. 交付物

| 文件 | 职责 |
|------|------|
| `module/module.prop` | KSU 模块标识（id=uro, v0.6.0, 作者是你吗薰儿） |
| `module/service.sh` | boot_completed 后拉起 + **30s 循环看门狗**；/sdcard 晚就绪等待（COSMemory v1.1.5 教训）；512KB 日志滚动 |
| `module/uninstall.sh` | **卸载兜底**：若配置处于 DIRTY → 恢复基线；删除 enforce 开关文件 |
| `runtime/tools/pack_module.py` | 打包 zip（含 755 权限位还原） |
| `controllers.cpp` | **bridge.state 持久化**（BASELINE/DIRTY）+ probe 期崩溃恢复 |

## 2. 崩溃残留的状态机（核心）

问题：URO 在 GAME 态写了 `aggressive=false` 后被 `kill -9` → 配置停在偏离基线的值，**无人恢复**。

```
bridge.state:  BASELINE=<用户基线>   DIRTY=<是否被我方改偏离>

正常 apply:   写 aggressive 后同步 DIRTY = (目标值 != 基线)
优雅关闭:     SWITCH off → 恢复基线 → DIRTY=false
异常退出:     DIRTY 保持 true（没人来得及清）
下次 probe:   读到 DIRTY=1 → 无条件 set_aggressive(BASELINE) → DIRTY=false
              ↑ 安全动作，先于任何 enforce 判断（即使面板开关=关也执行）
卸载:         uninstall.sh 同样读 DIRTY 恢复
```

基线档案首次运行时建立（以当时配置为基线）；此后 `baselineAgg_` 从 state 文件读取，**不随我方写入漂移**。

## 3. 实测（一加11）

| 场景 | 结果 |
|------|------|
| 模块安装 | 解压到 `/data/adb/modules/uro/`，`bin/URORuntime` md5 `49f7bbd7` = 构建产物，权限 755 |
| service.sh 首拉 | `WATCHDOG start` → `bin/URORuntime` pid 18657（模块二进制，非 /data/local/tmp） |
| `kill -9` 补拉 | 09:18:34 `WATCHDOG restart (dead)` → 新 pid 20017（**30s 内**） |
| **崩溃残留恢复** | 构造 `aggressive=false + DIRTY=true` → kill -9 → 09:19:35 补拉 → `PROBE ... detail=CRASH-RECOVERY aggressive false->true OK` → **配置回基线、DIRTY=false** |
| host 单测 | 97 断言全过（bridge 30 含 crash-recovery/clean-boot 两组新用例 + m2 35 + m3 32） |

## 4. 打包与安装

```
python3 runtime/tools/pack_module.py      # → dist/uro-v0.6.0.zip (206,111B, md5 9deffe23)
zip 结构: module.prop / service.sh(755) / uninstall.sh(755) / bin/URORuntime(755)
KernelSU 原生格式（module.prop 顶层，无需 META-INF）
```

## 5. 待验证（需用户执行）

- **开机自启**：模块已就位，重启后 KSU 应执行 service.sh（铁律：重启由用户执行）
- 重启后检查：`/sdcard/Android/UnifiedRootOptimizer/log/watchdog.log` 出现 `WATCHDOG start`

## 6. 遗留

1. 看门狗只在 30s 循环内检测，最长 30s 空窗（可接受；更短需 inotify/proc 轮询，代价不值）
2. URO 异常退出时**来不及**自己恢复（正是本阶段兜底覆盖的场景）；但若**整个看门狗也被杀**（模块被禁用），残留仍会保留 → uninstall/禁用时需手动恢复（uninstall.sh 已覆盖）
3. 开机自启未实测（待重启）
