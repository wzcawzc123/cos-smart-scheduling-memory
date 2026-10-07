# M2′ Controller 抽象 — 接口 + Adapter + 四 Controller（dry-run 阶段）

> UnifiedRootOptimizer M2′ · 2026-10-07 · v0.3.0 · commit 见 git log
> 依据：设计文档 v1.1 §5（模块设计）、§4.2（Policy 契约）、§4.3（约束求交）、§3.4（写完即走）、§9（M2′ 退出条件）、§10.1（Integration 测试）
> **退出条件：Memory/CPU Controller 接口 + Adapter 落地，GPU/Thermal 接口占位，所有缺失节点均可降级 —— 已达成**

## 1. 交付物

| 文件 | 内容 |
|------|------|
| `include/policy.hpp` | §4.2 契约（MemoryPolicy/CpuPolicy/GpuPolicy/EffectivePolicy）+ §4.3 `intersect()` 约束求交 + `materially_different()` 边界判断 |
| `include/adapter.hpp` + `src/adapter.cpp` | `SysfsAdapter`：probe / read / **write=写前校验+写后 readback** / 六种 `WriteOutcome` / dryRun 永不落盘 |
| `include/controller.hpp` | `Controller` 抽象（probe/on_event/desire/apply/status）+ `CtrlState`(Probing/Active/Degraded/Faulted) + `ControllerRegistry`（逐个 try 隔离、跳过 Degraded 求交、单个 Faulted 不影响其余） |
| `src/controllers.cpp` | `MemoryController`（§5.1 PSI+MemAvailable、策略快照桥接口）、`CpuController`（§5.2 cpufreq/cpuset、governor=uag 只记录不切换）、`GpuPlaceholder`/`ThermalPlaceholder`（§9 占位） |
| `src/main.cpp` | 接线：事件 → 各 `on_event` → 状态跃迁且策略**实质变化**才 `resolve()+apply_all(dryRun=true)`；PROBE/APPLY/TRACE 三类行写 `policy.log` |
| `tests/test_m2.cpp` + `tests/run_test.sh` | fake sysfs 集成测试，host g++ 编译，**35 断言全过** |

## 2. 关键设计决定

1. **dry-run 恒定**（§9.2 阶段一）：`apply_all(..., dryRun=true)` 硬编码，SHADOW 阶段任何路径都写不进系统节点。
2. **边界触发**（§3.4 写完即走）：只在 `StateChanged && materially_different(eff, lastApplied)` 时 apply；策略不变绝不重复写，杜绝轮询覆写 `scaling_max_freq` 与 uag 形成双写震荡。
3. **降级即跳过**：`resolve()` 跳过 Degraded/Faulted，其约束不参与求交；`apply_all` 对 Faulted 只记 "skipped"，异常全部 catch 在 registry 内 → **单 Controller 故障不会拖垮主进程**。
4. **GPU/Thermal 占位语义**：类在、接口在、`probe()` 恒返回 `Degraded(placeholder)`，与"节点缺失"共用降级通道，M2/M4 接手时替换实现即可，registry 无需改动。
5. **uag 共存**：`CpuController` 探测到 governor 后仅记录不干预（§5.2 "本机 uag 默认不切换"）；频率微调权归内核，用户态只提交上下限这类边界条件。

## 3. 测试（§10.1 Integration：Controller → Adapter → fake sysfs）

```
$ sh runtime/tests/run_test.sh
[adapter]        ok (outcomes: OK/DRYRUN/MISSING/EMPTY/DENIED + create/truncate)
[degrade]        ok (all-missing -> degraded, no crash)
[partial]        ok (only affected class degraded)
[intersect]      ok (§4.3 min-intersection + AND gate)
[boundary]       ok (no-op policy does not trigger write)
[isolation]      ok
结果: 35 passed, 0 failed     rc=0
```

覆盖：全节点缺失（不崩、`any_active()==false`、resolve/apply_all 仍可调用）、部分缺失（只降级受影响类）、约束求交（3.2G/2.4G → 2.4G、boost AND 门、min>max 钳回）、边界判重、只读内核节点 → DENIED、可创建文件 vs 节点缺失的区分、短值覆盖写截断。

**测试抓出的 3 个真缺陷**（都在修完后才转绿）：

1. `write()` 把"父目录可写的不存在文件"误判 `Missing` → 策略快照这类首次写入即创建的目标被误降级。修法：区分"节点缺失（父目录不可达）"与"可创建文件"。
2. 我为绝对路径加的"直通"**破坏了 fake sysfs 隔离**——`/proc/pressure/memory` 穿透到真机，导致"全缺失"测试里 memory 误判 ACTIVE。回滚：`root_` 非空时一律拼进 fake root。
3. 缓存的 `policyPath_` 已带 fake 前缀，`full()` 再拼一次 → 双拼路径判 `Missing`。修法：已带前缀则直通。

> 教训：#2 是"为了真实场景的一个便利改动，悄悄破坏了测试的隔离假设"——测试当时就红了，否则这个穿透会一直潜伏到真实运行时才暴露。

## 4. 真机验收（一加11 / ColorOS 16）

### 4.1 能力探测（`policy.log`）

```
PROBE ctrl=memory state=ACTIVE
PROBE ctrl=cpu    state=ACTIVE
PROBE ctrl=gpu    state=DEGRADED detail=M2′ placeholder — GPU Controller not implemented
PROBE ctrl=thermal state=DEGRADED detail=M2′ placeholder — thermal T-OBS deferred
dryRun=1 (SHADOW — 不写任何系统节点)
```

### 4.2 边界 APPLY（mode 连改 3 次 → 4 次跃迁）

```
20:20:59 APPLY gen=1 maxFreq=-1 reclaim=0 freeze=0 [memory=ACTIVE][cpu=ACTIVE][gpu=DEGRADED][thermal=DEGRADED]
20:21:03 APPLY gen=3 maxFreq=-1 reclaim=1 freeze=0 ...
20:21:06 APPLY gen=4 maxFreq=-1 reclaim=1 freeze=1 ...   ← powersave
20:21:09 APPLY gen=5 maxFreq=-1 reclaim=0 freeze=0 ...   ← performance
  TRACE write .../policy.memory.txt -> DRYRUN | would write: reclaim=1 freeze=1 maxKill=3
```

- 4 条 TRACE **全部 DRYRUN**，无一条 `OK`
- `policy.memory.txt` **未被创建**（`No such file or directory`）
- CPU 侧 `maxFreq=-1` → 根本不产生 cpufreq 写意图
- 写前/写后 cpufreq 对照：`1459200 2803200 3187200` → `1785600 2803200 3187200`，policy0 变化**非我方所为**（TRACE 中无 cpufreq 写意图），是 uag governor 的动态钳制——与 M0 能力矩阵"scaling_max 被 Oplus ~6s 粒度动态钳制"完全吻合

## 5. 与 spec 逐条对照

| spec 条目 | 状态 |
|-----------|------|
| §4.2 Policy 契约 | ✅ 结构体逐字段实现 |
| §4.3 约束求交（非最后写入者生效） | ✅ `intersect()` + 单测 |
| §3.4 写完即走、禁轮询覆写 | ✅ 边界触发 + `materially_different` 判重 + 单测 |
| §5.1 Memory（PSI+MemAvailable、快照共享、dry-run） | ✅ 接口与探测落地；真实 reclaim/freeze 执行待 M3 接 COSMemory |
| §5.2 CPU（节点探测、写前校验+readback、uag 不切换、cpuset 可 skip） | ✅ |
| §5.3/§5.4 GPU/Thermal 占位 | ✅ Degraded(placeholder) |
| §9 M2′ 退出条件：缺失节点均可降级 | ✅ 单测 + 真机双验证 |
| §10.1 Integration 测试 | ✅ 35 断言 |
| §9.2 dry-run → shadow → enforce | 当前 = dry-run 阶段（恒定） |

## 6. 遗留（下一步）

1. **COSMemory 真实桥接**：`policy.memory.txt` 只是快照，COSMemory 引擎还没读它 → M3 的 Adapter 落地
2. **CoreTurbo profile 接入**：`cpusetProfile`/`onlineCores` 字段已留，实际编译 profile 归 M3
3. **GPU/Thermal 实装**：接口已通，M2 替换 placeholder 即可（Thermal 先做 T-OBS 只读）
4. **enforce 放量**：需按 §9.2 逐 Controller 开关 + §10.2 设备实验矩阵
5. 政策文件写入路径已验证可创建（单测），实际 enforce 时首写会真正落盘
