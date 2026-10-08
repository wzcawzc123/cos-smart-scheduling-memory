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
    bool reclaimEnabled = true;
    bool freezeEnabled = false;
    int  maxKillPerRound = 0;
    bool cpuClamp = false;        // 是否提交 CPU 上限约束
    int  maxFreqKhz = -1;
    bool handover = false;        // §5.5：全量让权，撤销本框架全部写入
    std::string depth;            // 空 = 不干预（交还基线）
    int cooldownSec = -1;         // -1 = 不干预
    int uagUpRateUs = -1;         // -1 = 不干预（回基线）；CPU 侧升频延迟
};

// 策略决策输出
struct Decision {
    Scenario scenario = Scenario::DAILY;
    ScenarioTactics tactics;
    uint64_t generation = 0;
    bool changed = false;         // 相对上次决策实质变化（驱动 apply 边界）
    uint64_t leaseUntilMs = 0;    // 租约到期点；0 = 无租约
    std::string why;              // 触发原因（telemetry）
};

class PolicyManager {
public:
    // 游戏名单：每行一个包名（# 注释）。名单不存在 → GAME 场景不可触发（安全降级）。
    void set_game_list_path(std::string p) { gameListPath_ = std::move(p); }

    // 主判定：由事件驱动调用（不轮询）。hysteresisMs 用于压力场景退出滞回。
    Decision decide(const GlobalState& st, const Event& e, uint64_t now_ms) {
        Scenario cand = pick_scenario(st, e, now_ms);

        // --- 滞回（防震荡）：压力场景退出需要持续无压力达 hysteresisMs ---
        if (cand == Scenario::MEMORY_PRESSURE && lastPressureMs_ &&
            now_ms - lastPressureMs_ > hysteresisMs_) {
            cand = (st.mode == "powersave") ? Scenario::POWERSAVE : Scenario::DAILY;
        }

        Decision d;
        d.scenario = cand;
        d.tactics = tactics_for(cand, st);

        // --- generation：实质变化才 +1（§4.4 单调，幂等重放不产生新代）---
        bool same = (cand == lastScenario_ &&
                     d.tactics.reclaimEnabled == lastTactics_.reclaimEnabled &&
                     d.tactics.freezeEnabled == lastTactics_.freezeEnabled &&
                     d.tactics.maxKillPerRound == lastTactics_.maxKillPerRound &&
                     d.tactics.handover == lastTactics_.handover &&
                     d.tactics.maxFreqKhz == lastTactics_.maxFreqKhz &&
                     d.tactics.depth == lastTactics_.depth &&
                     d.tactics.cooldownSec == lastTactics_.cooldownSec &&
                     d.tactics.uagUpRateUs == lastTactics_.uagUpRateUs);
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
        d.why = why_;
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
        // 熄屏 = 用户不在看 → 自动省电（亮屏日用/熄屏省电 的自动映射核心）
        if (!st.screenOn) {
            why_ = "screen off -> powersave";
            return Scenario::POWERSAVE;
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
        if (st.mode == "powersave") { why_ = "mode=powersave"; return Scenario::POWERSAVE; }
        if (st.mode == "performance" || st.mode == "game") { why_ = "mode=" + st.mode; return Scenario::DAILY; }
        if (st.generation <= 1 && e.source.find("boot") != std::string::npos) {
            why_ = "bootstrap"; return Scenario::BOOT;
        }
        why_ = "mode=" + st.mode + " fg=" + st.foregroundPackage;
        return Scenario::DAILY;
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
            case Scenario::POWERSAVE:
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
            case Scenario::DAILY:
            default:
                t.reclaimEnabled = true;
                t.freezeEnabled = false;
                t.maxKillPerRound = 0;
                break;
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
    std::string why_;
    Scenario lastScenario_ = Scenario::BOOT;
    ScenarioTactics lastTactics_{};
    uint64_t lastGen_ = 0;
    uint64_t lastPressureMs_ = 0;
    uint64_t leaseDeadlineMs_ = 0;
    uint64_t hysteresisMs_ = 15000;      // 压力退出滞回 15s（防震荡）
    uint64_t pressureLeaseMs_ = 60000;   // 压力策略租约 60s，过期回落
};

} // namespace uro
