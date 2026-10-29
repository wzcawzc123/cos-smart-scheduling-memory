// UnifiedRootOptimizer M3 — PolicyManager（设计文档 v1.1 §3.3 场景模型 / §4.4 generation+lease /
// §5.5 Control Handover）
//
// 职责：GlobalState → 场景判定 → 场景策略 → EffectivePolicy（含 handover 标志）→ generation。
// 退出条件相关约束（§9 M3）：无策略震荡（滞回+判重）、无互相覆盖（Handover 全量让权）。
#pragma once
#include "state.hpp"
#include "policy.hpp"
#include "scenario.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include <cstdlib>
#include <optional>

namespace uro {

// 单场景策略模板（§3.3 表）
struct ScenarioTactics {
    int gpuIdleTimer = 0;
    int gpuPwrMin = -1;      // v0.33 kgsl min_pwrlevel（0=最高频；-1=不干预/回基线）
    int gpuPwrMax = -1;      // v0.33 kgsl max_pwrlevel（-1=不干预/回基线）
    int gpuBusSplit = -1;    // v0.33 kgsl bus_split（0/1；-1=不干预/回基线）
    int gpuDefPwr = -1;      // v0.33第6批 kgsl default_pwrlevel
    int gpuMinBw = -1;       // v0.33第6批 kgsl minbw_timer（总线空闲计时）
    int gpuForceClk = -1;    // v0.33第6批 kgsl force_clk_on（0/1）
    int gpuForceBus = -1;    // v0.33第6批 kgsl force_bus_on（0/1）
    int cpuMinFreqK = -1;    // v0.33第6批 scaling_min_freq（-1=不干预/回基线）
    int maxFreqK3 = -1;      // v0.35 per-cluster 上限：policy3
    int maxFreqK7 = -1;      // v0.35 per-cluster 上限：policy7
    int uagSoftLimitK = -1;  // v0.33第6批 uag/soft_limit_freq（软上限）
    int gpuThermalPwr = -1;  // v0.36 kgsl thermal_pwrlevel
    int gpuMinClkMhz = -1;   // v0.36 kgsl min_clock_mhz
    int gpuMaxClkMhz = -1;   // v0.36 kgsl max_clock_mhz
    int uclampMin = -1;      // v0.36 sched_util_clamp_min（0..1024）
    int uclampMax = -1;      // v0.36 sched_util_clamp_max（0..1024）
    int uclampRt = -1;       // v0.36 sched_util_clamp_min_rt_default（0..1024）
    int uagHispeedLoad = -1;   // uag hispeed_load（-1=回基线）
    int uagTargetLoadsFirst = -1;   // uag target_loads 首段（-1=回基线）
    int keepAliveAdj = -1;   // keepAlive.adj 档位（-1=不动）   // GPU idle_timer（0=不动；v0.22 GpuController）
    bool reclaimEnabled = true;
    bool freezeEnabled = false;
    int  maxKillPerRound = 0;
    bool cpuClamp = false;        // 是否提交 CPU 上限约束
    int  maxFreqKhz = -1;
    bool handover = false;        // §5.5：全量让权，撤销本框架全部写入
    std::string depth;            // 空 = 不干预（交还基线）
    int cooldownSec = -1;         // -1 = 不干预
    int uagUpRateUs = -1;         // -1 = 不干预（回基线）；CPU 侧升频延迟
    int uagDownRateUs = -1;       // -1 = 不干预（回基线）；CPU 侧降频迟滞（PERFORMANCE 档）
    // v0.40 线程与分组（对齐 Uperf 的线程亲和 / 主流模块的 cpuset 管理）
    bool threadPin = false;       // 是否把前台 UI/Render 线程绑到非小核
    int  threadPinMask = -1;      // CPU 位掩码（bit i = cpu i）；-1 且 threadPin=1 → 默认 0xF8
    int  bgCpusMask = -1;         // background / system-background 组允许的 CPU（-1 不干预）
    int  topCpusMask = -1;        // top-app 组允许的 CPU（-1 不干预）
};

// v0.41（审查 F-01）：完整策略指纹。
// Decision.changed 过去只比较 9 个字段，导致"只改了 uclamp/GPU/线程/cpuset 等字段"
// 时热重载读到了新配置却不重新下发。这里把 ScenarioTactics 所有会影响执行的字段
// 拼成稳定指纹，changed 判定改用指纹比较（新增字段必须同步加进本函数）。
inline std::string tactics_fingerprint(const ScenarioTactics& t) {
    std::ostringstream o;
    o << t.gpuIdleTimer << ',' << t.gpuPwrMin << ',' << t.gpuPwrMax << ',' << t.gpuBusSplit
      << ',' << t.gpuDefPwr << ',' << t.gpuMinBw << ',' << t.gpuForceClk << ',' << t.gpuForceBus
      << ',' << t.cpuMinFreqK << ',' << t.maxFreqK3 << ',' << t.maxFreqK7 << ',' << t.uagSoftLimitK
      << ',' << t.gpuThermalPwr << ',' << t.gpuMinClkMhz << ',' << t.gpuMaxClkMhz
      << ',' << t.uclampMin << ',' << t.uclampMax << ',' << t.uclampRt
      << ',' << t.uagHispeedLoad << ',' << t.uagTargetLoadsFirst << ',' << t.keepAliveAdj
      << ',' << (int)t.reclaimEnabled << ',' << (int)t.freezeEnabled << ',' << t.maxKillPerRound
      << ',' << (int)t.cpuClamp << ',' << t.maxFreqKhz << ',' << (int)t.handover
      << ',' << t.cooldownSec << ',' << t.uagUpRateUs << ',' << t.uagDownRateUs
      << ',' << (int)t.threadPin << ',' << t.threadPinMask << ',' << t.bgCpusMask << ',' << t.topCpusMask
      << '|' << t.depth;
    return o.str();
}

// 策略决策输出
struct Decision {
    Scenario scenario = Scenario::BALANCE;
    ScenarioTactics tactics;
    uint64_t generation = 0;
    bool changed = false;         // 相对上次决策实质变化（驱动 apply 边界）
    uint64_t leaseUntilMs = 0;    // 租约到期点；0 = 无租约
    std::string why;              // 触发原因（telemetry）
    bool thermalCapped = false;
    bool powerCapped = false;     // 本轮被电量守卫夹档（审计用，v0.23）   // 本轮被 Thermal 夹档（审计用，v0.20）
};

class PolicyManager {
public:
    // 游戏名单：每行一个包名（# 注释）。名单不存在 → GAME 场景不可触发（安全降级）。
    void set_game_list_path(std::string p) { gameListPath_ = std::move(p); }
    // App 画像（第2步·CT 遗产）：每行 "通配符包名|down|up"（-1=跟随档位；# 注释；
    // 与 game_list 同款生命周期——每次现读、热改热生效）。优先级：画像 > 档位 > 基线。
    void set_app_profiles_path(std::string p) { profilePath_ = std::move(p); }

    // ---- 参数外置（v0.33 第1批）：外部场景配方覆盖 ----
    // 空 map = 完全走内置默认（向后兼容）。只覆盖配置里出现的字段。
    void set_tactics_override(std::map<Scenario, std::map<std::string, std::string>> m) {
        ovr_ = std::move(m);
    }
    size_t tactics_override_scenarios() const { return ovr_.size(); }

    // ---- v0.34 手动档位（WebUI 切换）----
    // nullopt = AUTO（自动判定）；有值 = 强制该场景。
    // 注意：安全阀（滞回/电量守卫/热夹档）仍然生效——手动只覆盖"场景判定"这一步。
    void set_manual_scenario(std::optional<Scenario> s) { manual_ = s; }
    std::optional<Scenario> manual_scenario() const { return manual_; }

    void apply_tactics_override(ScenarioTactics& t, Scenario s) const {
        auto it = ovr_.find(s);
        if (it == ovr_.end()) return;
        for (const auto& kv : it->second) {
            const std::string& k = kv.first;
            const std::string& v = kv.second;
            long n = std::atol(v.c_str());
            if (k == "reclaim")            t.reclaimEnabled = (n != 0);
            else if (k == "freeze")        t.freezeEnabled = (n != 0);
            else if (k == "maxKill")       t.maxKillPerRound = (int)n;
            else if (k == "depth")         t.depth = v;
            else if (k == "cooldown")      t.cooldownSec = (int)n;
            else if (k == "uagUp")         t.uagUpRateUs = (int)n;
            else if (k == "uagDown")       t.uagDownRateUs = (int)n;
            else if (k == "uagHi")         t.uagHispeedLoad = (int)n;
            else if (k == "uagTl")         t.uagTargetLoadsFirst = (int)n;
            else if (k == "gpuIdle")       t.gpuIdleTimer = (int)n;
            else if (k == "gpuMinPwr")     t.gpuPwrMin = (int)n;
            else if (k == "gpuMaxPwr")     t.gpuPwrMax = (int)n;
            else if (k == "gpuSplit")      t.gpuBusSplit = (int)n;
            else if (k == "gpuDefPwr")     t.gpuDefPwr = (int)n;
            else if (k == "gpuMinBw")      t.gpuMinBw = (int)n;
            else if (k == "gpuForceClk")   t.gpuForceClk = (int)n;
            else if (k == "gpuForceBus")   t.gpuForceBus = (int)n;
            else if (k == "cpuMinK")       t.cpuMinFreqK = (int)n;
            else if (k == "uagSoftK")      t.uagSoftLimitK = (int)n;
            else if (k == "gpuThermalPwr") t.gpuThermalPwr = (int)n;
            else if (k == "gpuMinClkMhz")  t.gpuMinClkMhz = (int)n;
            else if (k == "gpuMaxClkMhz")  t.gpuMaxClkMhz = (int)n;
            else if (k == "uclampMin")     t.uclampMin = (int)n;
            else if (k == "uclampMax")     t.uclampMax = (int)n;
            else if (k == "uclampRt")      t.uclampRt = (int)n;
            else if (k == "keepAdj")       t.keepAliveAdj = (int)n;
            else if (k == "cpuClamp")      t.cpuClamp = (n != 0);
            else if (k == "maxFreqK")      t.maxFreqKhz = (int)n;
            else if (k == "maxFreqK3")     t.maxFreqK3 = (int)n;
            else if (k == "maxFreqK7")     t.maxFreqK7 = (int)n;
            else if (k == "handover")      t.handover = (n != 0);
            // v0.40 线程与分组
            else if (k == "threadPin")     t.threadPin = (n != 0);
            else if (k == "threadPinMask") t.threadPinMask = (int)n;
            else if (k == "bgCpus")        t.bgCpusMask = (int)n;
            else if (k == "topCpus")       t.topCpusMask = (int)n;
        }
    }

    // ---- App 画像引擎 ----
    static bool glob_match(const char* p, const char* s) {
        if (!*p) return !*s;
        if (*p == '*') {
            for (const char* q = s;; ++q) {
                if (glob_match(p + 1, q)) return true;
                if (!*q) return false;
            }
        }
        if (*s && (*p == '?' || *p == *s)) return glob_match(p + 1, s + 1);
        return false;
    }
    static std::string trim_ws(const std::string& t) {
        auto a = t.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        auto b = t.find_last_not_of(" \t\r\n");
        return t.substr(a, b - a + 1);
    }
    // 应用画像：匹配前台包名 → 覆盖 uag 参数；GAME（让权）与 POWER_SAVE（熄屏省电）
    // 不应用——画像不能破解让权与省电这两个安全档（其余档位均可覆盖，CT 同款语义）
    void apply_profile(Decision& d, const std::string& pkg) const {
        if (profilePath_.empty() || pkg.empty()) return;
        if (d.scenario == Scenario::GAME || d.scenario == Scenario::POWER_SAVE) return;
        std::ifstream f(profilePath_);
        if (!f.is_open()) return;          // 文件缺失 → 无画像（降级）
        std::string line;
        while (std::getline(f, line)) {
            auto h = line.find('#');
            if (h != std::string::npos) line = line.substr(0, h);
            auto bar = line.find('|');
            if (bar == std::string::npos) continue;
            std::string pat = trim_ws(line.substr(0, bar));
            if (pat.empty()) continue;
            if (!glob_match(pat.c_str(), pkg.c_str())) continue;
            auto bar2 = line.find('|', bar + 1);
            std::string dS = trim_ws(line.substr(bar + 1,
                                     (bar2 == std::string::npos ? line.size() : bar2) - bar - 1));
            std::string uS = bar2 == std::string::npos ? "" : trim_ws(line.substr(bar2 + 1));
            bool hit = false;
            if (!dS.empty()) { int v = atoi(dS.c_str()); if (v >= 0) { d.tactics.uagDownRateUs = v; hit = true; } }
            if (!uS.empty()) { int v = atoi(uS.c_str()); if (v >= 0) { d.tactics.uagUpRateUs = v; hit = true; } }
            if (hit) {
                d.why += " profile=" + pat;
                break;                      // 首个匹配生效（文件顺序即优先级）
            }
        }
    }

    // 主判定：由事件驱动调用（不轮询）。hysteresisMs 用于压力场景退出滞回。
    Decision decide(const GlobalState& st, const Event& e, uint64_t now_ms) {
        // Touch 感知（第2步）：down/up 边沿都刷新触摸时间（1.5s 滞回窗口由空闲 tick 触发回落）
        if (e.type == EventType::TouchChanged) lastTouchMs_ = now_ms;
        // AmSwitch 感知（第2步·CT 搬运）：前台切换刷新过渡窗口（2.5s FAST，空闲 tick 回落）
        if (e.type == EventType::ForegroundChanged) lastSwitchMs_ = now_ms;
        Scenario cand = pick_scenario(st, e, now_ms);

        // v0.34 手动档（WebUI 切换）：非 AUTO 时强制该场景。
        // 只覆盖"场景判定"这一步——下面的滞回/电量守卫/热夹档等安全阀仍然生效（安全优先）。
        // 游戏让权优先（§5.5）：自动判定为 GAME 时手动档不生效
        if (manual_ && cand != Scenario::GAME) {
            cand = *manual_;
            why_ = std::string("manual:") + scenario_name(*manual_);
        }

        // --- 滞回（防震荡）：压力场景退出需要持续无压力达 hysteresisMs ---
        if (cand == Scenario::MEMORY_PRESSURE && lastPressureMs_ &&
            now_ms - lastPressureMs_ > hysteresisMs_) {
            cand = st.screenOn ? Scenario::BALANCE : Scenario::POWER_SAVE;
        }

        // L4 电量守卫（Evidence 回喂）：低电+放电 → 收性能；充电中不干预；GAME 让权
        {
            bool pgOn = true; int pgPct = 20;
            { std::ifstream f("/sdcard/Android/UnifiedRootOptimizer/uro.conf"); std::string ln;
              while (std::getline(f, ln)) {
                  if (ln.rfind("POWER_GUARD=0", 0) == 0) pgOn = false;
                  else if (ln.rfind("POWER_GUARD_PCT=", 0) == 0) pgPct = std::atoi(ln.c_str() + 16);
              } }
            if (pgOn && st.batteryPct >= 0 && !st.charging && st.batteryPct <= pgPct &&
                cand != Scenario::GAME && cand != Scenario::POWER_SAVE) {
                Scenario capped = (st.batteryPct <= 10) ? Scenario::POWER_SAVE
                    : ((cand == Scenario::FAST || cand == Scenario::PERFORMANCE)
                           ? Scenario::BALANCE : cand);
                if (capped != cand) { cand = capped; why_ = "power-guard " + why_; pGuardCapped_ = true; }
            }
        }

        // Thermal 夹档（T-OBS→干预）：热=安全底线（画像/FAST/PERF 都压不过）；
        // GAME 让权不干预（热由系统 thermal-engine 管）；压力态本身已是省电态不动。
        if (st.thermalLevel > 0 && cand != Scenario::GAME &&
            cand != Scenario::MEMORY_PRESSURE && cand != Scenario::POWER_SAVE) {
            // L2 只压到 BALANCE（v0.31 修正）：系统 thermal-engine 已在做硬件降频，
            // URO 再压 POWER_SAVE = 重复惩罚（实测 95° 用户交互体感极差）
            Scenario capped = ((cand == Scenario::FAST || cand == Scenario::PERFORMANCE)
                                   ? Scenario::BALANCE : cand);
            if (capped != cand) { cand = capped; why_ = "thermal-capped " + why_; thermalCapped_ = true; }
        }

        Decision d;
        d.scenario = cand;
        d.thermalCapped = thermalCapped_; thermalCapped_ = false;
        d.powerCapped = pGuardCapped_; pGuardCapped_ = false;
        d.tactics = tactics_for(cand, st);
        apply_profile(d, st.foregroundPackage);   // App 画像：通配符覆盖（画像 > 档位 > 基线）

        // --- generation：实质变化才 +1（§4.4 单调，幂等重放不产生新代）---
        // v0.41（审查 F-01）：改用完整策略指纹比较，任何影响执行的字段变化都会触发下发
        bool same = (cand == lastScenario_ &&
                     tactics_fingerprint(d.tactics) == lastTacticFp_);
        d.changed = !same;
        d.generation = same ? lastGen_ : lastGen_ + 1;
        if (d.changed) {
            lastScenario_ = cand;
            lastTactics_ = d.tactics;
            lastTacticFp_ = tactics_fingerprint(d.tactics);
            lastGen_ = d.generation;
        }

        // --- 租约：压力场景与 LaunchBoost 类策略带 TTL，过期自动回落（§4.4）---
        if (cand == Scenario::MEMORY_PRESSURE)
            d.leaseUntilMs = now_ms + pressureLeaseMs_;
        d.why = why_ + d.why;   // d.why 此时仅含 apply_profile 追加的 " profile=..."（画像审计）
        return d;
    }

    // 租约到期检查（由事件循环的空闲 tick 调用，非写入轮询——只读判断）
    // 返回 true 表示租约过期需要重新决策
    bool lease_expired(uint64_t now_ms) const {
        return leaseDeadlineMs_ > 0 && now_ms >= leaseDeadlineMs_;
    }
    void note_lease(uint64_t until) { leaseDeadlineMs_ = until; }

    void set_pressure_time(uint64_t t) { lastPressureMs_ = t; }
    Scenario last_scenario() const { return lastScenario_; }
    uint64_t generation() const { return lastGen_; }

    void set_hysteresis_ms(uint64_t ms) { hysteresisMs_ = ms; }
    void set_pressure_lease_ms(uint64_t ms) { pressureLeaseMs_ = ms; }

private:
    Scenario pick_scenario(const GlobalState& st, const Event& e, uint64_t now_ms) {
        // 游戏让权优先（§5.5）：自动判定为 GAME 时手动档不生效
        // 仅亮屏时成立——熄屏时"在玩游戏"无意义，交由下面的省电分支接管
        // （挂机游戏进程在前台不会被回收杀掉，freeze 只冻结后台，安全）。
        if (st.screenOn && is_game(st.foregroundPackage)) {
            why_ = "game fg=" + st.foregroundPackage;
            return Scenario::GAME;
        }
        // 熄屏 = 用户不在看 → 自动省电档（亮屏日用/熄屏省电 的自动映射核心）
        if (!st.screenOn) {
            why_ = "screen off -> powersave";
            return Scenario::POWER_SAVE;
        }
        // 压力事件置位（退出走滞回）
        if (e.type == EventType::MemoryPressureChanged) {
            lastPressureMs_ = now_ms;
            why_ = "psi/mem pressure: " + e.payload;
            return Scenario::MEMORY_PRESSURE;
        }
        if (lastPressureMs_ && now_ms - lastPressureMs_ <= hysteresisMs_) {
            why_ = "pressure hold (hysteresis)";
            return Scenario::MEMORY_PRESSURE;
        }
        if (st.generation <= 1 && e.source.find("boot") != std::string::npos) {
            why_ = "bootstrap"; return Scenario::BOOT;
        }
        // AmSwitch 感知（CT 场景机同款优先级：过渡期压过 Touch——刚切 App 时手指
        // 必然还在屏上，若 Touch 先判会抖成 PERF->FAST 两次切换）：
        // 前台切换后 2.5s 内 = FAST（启动过渡放开；CT fast 档同语义"最激进"档）
        if (lastSwitchMs_ && now_ms - lastSwitchMs_ < 2500) {
            why_ = "app-switch fg=" + st.foregroundPackage;
            return Scenario::FAST;
        }
        // Touch 感知（第2步·CT 搬运）：亮屏 + 1.5s 内有触摸边沿 → PERFORMANCE 档。
        // 当前为空包档（参数=基线），本期先通"感知→档位→telemetry→看板"链路；
        // hispeed_freq 等参数待专项实测后放量（Oplus 私有语义，不猜不写）。
        // 局限：按住滑动 >1.5s 会中途回落、up 时回升（PERFORMANCE 与 BALANCE 同参，
        // 切换不产生写入，仅 telemetry 多两行）——持续活跃检测留二期。
        if (lastTouchMs_ && now_ms - lastTouchMs_ < 1500) {
            why_ = "touch";
            return Scenario::PERFORMANCE;
        }
        // v0.11 四档自治：不再读系统 mode.txt（用户从不手动切系统省电/高性能，判档全走自身感知）
        why_ = "fg=" + st.foregroundPackage;
        return Scenario::BALANCE;
    }

    ScenarioTactics tactics_for(Scenario s, const GlobalState& st) {
        ScenarioTactics t;
        switch (s) {
            case Scenario::GAME:            // §5.5 全量让权
                t.handover = true;
                t.reclaimEnabled = false;   // 仅白名单保护，暂停主动回收
                t.freezeEnabled = false;
                t.maxKillPerRound = 0;
                t.cpuClamp = false;         // 撤销全部上限 → baseline
                t.maxFreqKhz = -1;
                break;
            case Scenario::MEMORY_PRESSURE:
                t.reclaimEnabled = true;    // 提升回收优先级
                t.freezeEnabled = true;
                t.maxKillPerRound = 5;
                t.depth = "service";        // 彻底档：连空服务一起清（引擎热读）
                t.cooldownSec = 30;         // 缩短冷却：压力期允许更频繁重触发
                t.cpuClamp = false;         // §3.3 允许降级但 M3 不主动 clamp
                break;
            case Scenario::POWER_SAVE:
                t.reclaimEnabled = true;
                t.freezeEnabled = true;
                t.maxKillPerRound = 3;
                // CT SDM8G2 powersave 档原值：三簇升频延迟 0→2500us（升频变慢=省电）
                t.uagUpRateUs = 2500;
                break;
            case Scenario::BOOT:
                t.reclaimEnabled = false;   // 保守保护关键进程
                t.freezeEnabled = false;
                t.maxKillPerRound = 0;
                break;
            case Scenario::PERFORMANCE:
                // PERFORMANCE：降频迟滞 80ms（down_rate_limit 0->80000us）——触摸停止后
                // 频率粘在高位 80ms 不急降，跟手性提升；退出回基线（立即可降）。
                // 语义 = governor 通用 rate_limit（两次调频动作最小间隔），现值 0、无人认领。
                t.reclaimEnabled = true;
                t.freezeEnabled = false;
                t.maxKillPerRound = 0;
                t.uagDownRateUs = 80000;
                break;
            case Scenario::FAST:
                // FAST（AmSwitch 启动过渡）：降频迟滞 150ms（比 PERF 更粘）——CT fast 档
                // 同语义"最激进"档；2.5s 窗口后空闲 tick 自动回落。
                t.reclaimEnabled = true;
                t.freezeEnabled = false;
                t.maxKillPerRound = 0;
                t.uagDownRateUs = 150000;
                break;
            case Scenario::BALANCE:
            default:
                t.reclaimEnabled = true;
                t.freezeEnabled = false;
                t.maxKillPerRound = 0;
                break;
        }
        // GPU idle_timer 档位（v0.22：行为参数、非上限；GAME/压力 0=不动）
        switch (s) {
            case Scenario::PERFORMANCE:
            case Scenario::FAST:       t.gpuIdleTimer = 120; break;
            case Scenario::BALANCE:    t.gpuIdleTimer = 80;  break;
            case Scenario::POWER_SAVE: t.gpuIdleTimer = 50;  break;
            default:                   t.gpuIdleTimer = 0;   break;
        }
        // uag hispeed_load 档位（v0.25：升频阈值——PERF/FAST 更早跳频、PS 更晚；-1=回基线）
        switch (s) {
            case Scenario::PERFORMANCE:
            case Scenario::FAST:       t.uagHispeedLoad = 80; break;
            case Scenario::POWER_SAVE: t.uagHispeedLoad = 95; break;
            default:                   t.uagHispeedLoad = -1; break;
        }
        // uag target_loads：**实测回滚**（v0.26）——非独占节点：Oplus 熄屏时改写频率点段
        // （2112000 -> 1056000），URO 写入被覆盖且有打架风险。默认一律不写（-1）；
        // 保留控制器通路与档案结构（Oplus 行为若变化可重新启用）。
        t.uagTargetLoadsFirst = -1;
        // keepAlive.adj 档位（v0.28：保活目标 adj——越小越保；源自 COSMemory keepAlive 段）
        switch (s) {
            case Scenario::PERFORMANCE:
            case Scenario::FAST:            t.keepAliveAdj = 100; break;   // 强保前台
            case Scenario::BALANCE:         t.keepAliveAdj = 200; break;   // 默认
            case Scenario::POWER_SAVE:      t.keepAliveAdj = 250; break;   // 放宽保活
            case Scenario::MEMORY_PRESSURE: t.keepAliveAdj = 300; break;   // 多回收
            default:                        t.keepAliveAdj = -1;    break;  // GAME 不看/不动
        }
        // v0.33 参数外置：外部配方覆盖（未配置的字段 = 保持以上内置默认）
        apply_tactics_override(t, s);
        return t;
    }

    bool is_game(const std::string& pkg) const {
        if (pkg.empty()) return false;
        if (gameListPath_.empty()) return false;
        std::ifstream f(gameListPath_);
        if (!f.is_open()) return false;     // 名单缺失 → 不触发 GAME（降级）
        std::string line;
        while (std::getline(f, line)) {
            auto h = line.find('#');
            if (h != std::string::npos) line = line.substr(0, h);
            // trim
            auto a = line.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            auto b = line.find_last_not_of(" \t\r\n");
            line = line.substr(a, b - a + 1);
            if (line == pkg) return true;
        }
        return false;
    }

    // ---- 场景配方覆盖（v0.33 参数外置）：场景 → 字段名 → 原样值 ----
    std::map<Scenario, std::map<std::string, std::string>> ovr_;
    std::optional<Scenario> manual_;   // v0.34 manual scenario (nullopt = AUTO)

    std::string gameListPath_;
    bool thermalCapped_ = false;
    bool pGuardCapped_ = false;   // 电量守卫夹档边沿
    std::string profilePath_;              // App 画像文件（空 = 无画像）
    std::string why_;
    Scenario lastScenario_ = Scenario::BOOT;
    ScenarioTactics lastTactics_{};
    std::string lastTacticFp_;   // v0.41：上次下发策略的完整指纹（审查 F-01）
    uint64_t lastGen_ = 0;
    uint64_t lastPressureMs_ = 0;
    uint64_t lastTouchMs_ = 0;             // 最近触摸边沿时间（Touch→PERFORMANCE，1.5s 滞回）
    uint64_t lastSwitchMs_ = 0;            // 最近前台切换时间（AmSwitch→FAST，2.5s 过渡窗）
    uint64_t leaseDeadlineMs_ = 0;
    uint64_t hysteresisMs_ = 15000;      // 压力退出滞回 15s（防震荡）
    uint64_t pressureLeaseMs_ = 60000;   // 压力策略租约 60s，过期回落
};

} // namespace uro
