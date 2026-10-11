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

namespace uro {

// 单场景策略模板（§3.3 表）
struct ScenarioTactics {
    int gpuIdleTimer = 0;   // GPU idle_timer（0=不动；v0.22 GpuController）
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
};

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
            Scenario capped = (st.thermalLevel >= 2) ? Scenario::POWER_SAVE
                : ((cand == Scenario::FAST || cand == Scenario::PERFORMANCE)
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
        bool same = (cand == lastScenario_ &&
                     d.tactics.reclaimEnabled == lastTactics_.reclaimEnabled &&
                     d.tactics.freezeEnabled == lastTactics_.freezeEnabled &&
                     d.tactics.maxKillPerRound == lastTactics_.maxKillPerRound &&
                     d.tactics.handover == lastTactics_.handover &&
                     d.tactics.maxFreqKhz == lastTactics_.maxFreqKhz &&
                     d.tactics.depth == lastTactics_.depth &&
                     d.tactics.cooldownSec == lastTactics_.cooldownSec &&
                     d.tactics.uagUpRateUs == lastTactics_.uagUpRateUs &&
                     d.tactics.uagDownRateUs == lastTactics_.uagDownRateUs);
        d.changed = !same;
        d.generation = same ? lastGen_ : lastGen_ + 1;
        if (d.changed) {
            lastScenario_ = cand;
            lastTactics_ = d.tactics;
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
        // GAME 最高优先（§5.5 全量让权）：前台在游戏名单内即让权。
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

    std::string gameListPath_;
    bool thermalCapped_ = false;
    bool pGuardCapped_ = false;   // 电量守卫夹档边沿
    std::string profilePath_;              // App 画像文件（空 = 无画像）
    std::string why_;
    Scenario lastScenario_ = Scenario::BOOT;
    ScenarioTactics lastTactics_{};
    uint64_t lastGen_ = 0;
    uint64_t lastPressureMs_ = 0;
    uint64_t lastTouchMs_ = 0;             // 最近触摸边沿时间（Touch→PERFORMANCE，1.5s 滞回）
    uint64_t lastSwitchMs_ = 0;            // 最近前台切换时间（AmSwitch→FAST，2.5s 过渡窗）
    uint64_t leaseDeadlineMs_ = 0;
    uint64_t hysteresisMs_ = 15000;      // 压力退出滞回 15s（防震荡）
    uint64_t pressureLeaseMs_ = 60000;   // 压力策略租约 60s，过期回落
};

} // namespace uro
