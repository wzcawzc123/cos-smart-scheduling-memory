// UnifiedRootOptimizer M1 — Runtime 主程序（SHADOW 只读模式）
// 构建: NDK r26d + android-29 + arm64-v8a (构建链见 docs/M0_构建链验证_v1.md)
#include "event.hpp"
#include "state.hpp"
#include "drivers.hpp"
#include "controller.hpp"
#include "policy_manager.hpp"
#include <csignal>
#include <cstring>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <sys/stat.h>

using namespace uro;

static std::atomic<bool> g_run{true};
static void on_stop(int) { g_run = false; }

// 事件日志：单文件 10MB 上限，滚动一次（P1-04 契约）
class EventLog {
public:
    explicit EventLog(const std::string& path) : path_(path) {
        mkdir_recursive(path);
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && st.st_size > 10 * 1024 * 1024)
            rename(path.c_str(), (path + ".1").c_str());
        f_.open(path, std::ios::app);
    }
    void line(const std::string& s) {
        if (!f_.is_open()) return;
        f_ << s << '\n'; f_.flush();
    }
private:
    static void mkdir_recursive(const std::string& p) {
        auto pos = p.find_last_of('/'); std::string dir = p.substr(0, pos);
        std::string acc;
        std::stringstream ss(dir); std::string seg;
        while (std::getline(ss, seg, '/')) { acc += "/" + seg; mkdir(acc.c_str(), 0755); }
    }
    std::string path_; std::ofstream f_;
};

int main(int argc, char** argv) {
    const char* logdir = (argc > 1) ? argv[1] : "/sdcard/Android/UnifiedRootOptimizer/log";
    signal(SIGINT, on_stop); signal(SIGTERM, on_stop);

    EventQueue q;
    GlobalState st;
    StateManager sm;
    EventLog elog(std::string(logdir) + "/events.log");
    auto fg = std::make_shared<FgShared>();   // inotify / focus 两扳机共享去重

    DriverPaths p;   // 默认路径（可后续外置配置）
    fprintf(stderr,
        "[URO-M2′] UnifiedRootOptimizer Runtime v0.3.0 (SHADOW dry-run, focus+Controller)\n"
        "[URO-M2′] fg=%s focusPoll=%dms mode=%s uroDir=%s cosmDir=%s\n"
        "[URO-M2′] log=%s/events.log\n",
        p.topAppCpuset.c_str(), p.focusPollMs, p.modeFile.c_str(), p.uroDir.c_str(),
        p.cosmDir.c_str(), logdir);

    // ---- M2′ Controller 层：能力探测（缺失节点逐个降级，不阻断启动）----
    SysfsAdapter ad;
    ControllerRegistry reg;
    reg.add(make_memory_controller(std::string(logdir) + "/policy.memory.txt"));
    reg.add(make_cpu_controller());
    reg.add(make_gpu_placeholder());
    reg.add(make_thermal_placeholder());
    auto probeReps = reg.probe_all(ad);
    EventLog plog(std::string(logdir) + "/policy.log");
    for (size_t i = 0; i < probeReps.size(); ++i) {
        std::string line = "PROBE ctrl=" + std::string(reg.at(i).name()) +
                           " state=" + ctrl_state_name(probeReps[i].state) +
                           (probeReps[i].detail.empty() ? "" : " detail=" + probeReps[i].detail);
        plog.line(line);
        fprintf(stderr, "[URO-M2′] %s\n", line.c_str());
    }
    fprintf(stderr, "[URO-M2′] dryRun=1 (SHADOW — 不写任何系统节点)\n");

    // ---- M3 PolicyManager：场景判定 + generation + lease ----
    PolicyManager pm;
    pm.set_game_list_path(std::string(logdir) + "/game_apps.txt");

    std::thread t1(fg_driver, std::ref(q), p, fg, std::ref(g_run));
    std::thread t5(focus_driver, std::ref(q), p, fg, std::ref(g_run));
    std::thread t2(mode_config_driver, std::ref(q), p, std::ref(g_run));
    std::thread t3(sampler_driver, std::ref(q), p, std::ref(g_run));
    std::thread t4(screen_driver, std::ref(q), nullptr, std::ref(g_run));

    Event e;
    EffectivePolicy lastApplied;
    bool appliedOnce = false;

    // ---- 策略执行单元（事件驱动与租约回落共用；§3.4 写完即走）----
    auto run_policy = [&](const Event& ev, uint64_t now) {
        Decision dec = pm.decide(st, ev, now);
        pm.note_lease(dec.leaseUntilMs);
        if (!dec.changed) return;

        EffectivePolicy base;
        base.memory.reclaimEnabled  = dec.tactics.reclaimEnabled;
        base.memory.freezeEnabled   = dec.tactics.freezeEnabled;
        base.memory.maxKillPerRound = dec.tactics.maxKillPerRound;
        base.cpu.maxFreq = dec.tactics.cpuClamp ? dec.tactics.maxFreqKhz : -1;
        EffectivePolicy eff = reg.resolve(base, dec.generation);

        char ts[32]; time_t s = (time_t)(now / 1000); struct tm tv{};
        localtime_r(&s, &tv); strftime(ts, sizeof ts, "%F %T", &tv);
        plog.line(std::string(ts) + " SCENARIO=" + scenario_name(dec.scenario) +
                  " gen=" + std::to_string(dec.generation) +
                  " handover=" + (dec.tactics.handover ? "1" : "0") +
                  " why=" + dec.why);

        if (dec.tactics.handover) {
            // §5.5 全量让权：本框架零写入，降级为观察者
            plog.line("  HANDOVER — zero writes (control handed to GameAssistant/kernel)");
            lastApplied = EffectivePolicy{};
            appliedOnce = true;
            return;
        }
        if (appliedOnce && !materially_different(eff, lastApplied)) return;  // 幂等

        auto reps = reg.apply_all(eff, ad, /*dryRun=*/true);   // SHADOW：永不落盘
        std::string l = std::string(ts) + " APPLY gen=" + std::to_string(eff.generation) +
                        " maxFreq=" + std::to_string(eff.cpu.maxFreq) +
                        " reclaim=" + (eff.memory.reclaimEnabled ? "1" : "0") +
                        " freeze=" + (eff.memory.freezeEnabled ? "1" : "0") +
                        " maxKill=" + std::to_string(eff.memory.maxKillPerRound);
        for (size_t i = 0; i < reps.size() && i < reg.size(); ++i)
            l += " [" + std::string(reg.at(i).name()) + "=" + ctrl_state_name(reps[i].state) + "]";
        plog.line(l);
        for (auto& t : ad.traces())
            plog.line("  TRACE " + t.op + " " + t.path + " -> " + t.result +
                      (t.detail.empty() ? "" : " | " + t.detail));
        ad.clear_traces();
        appliedOnce = true;
        lastApplied = eff;
    };

    while (g_run) {
        if (!q.pop(e, 500)) {
            // 空闲 tick：只做租约到期判断（读时钟，非写入轮询；§4.4 TTL 回落）
            if (pm.lease_expired(now_ms())) {
                pm.set_pressure_time(0);            // 压力租约过期 → 清压力态
                run_policy(e, now_ms());            // 重新决策 → 回落 DAILY/POWERSAVE
                plog.line("LEASE expired -> scenario fallback");
            }
            continue;
        }
        bool changed = sm.apply(e, st);
        e.generation = st.generation;
        e.ts_ms = e.ts_ms ? e.ts_ms : now_ms();

        for (size_t i = 0; i < reg.size(); ++i) reg.at(i).on_event(e, st);
        if (e.type == EventType::MemoryPressureChanged) pm.set_pressure_time(e.ts_ms);
        run_policy(e, e.ts_ms);

        char ts[32];
        time_t s = e.ts_ms / 1000; struct tm tmv{};
        localtime_r(&s, &tmv);
        strftime(ts, sizeof ts, "%F %T", &tmv);
        char pl[8];
        snprintf(pl, sizeof pl, ".%03u", (unsigned)(e.ts_ms % 1000));
        char gen[48];
        snprintf(gen, sizeof gen, "gen=%llu%s", (unsigned long long)e.generation, pl);
        elog.line(std::string(ts) + " " + etype_name(e.type) + " " + gen +
                  " src=" + e.source + " data=" + e.payload +
                  (changed ? " [state-changed]" : ""));
    }

    g_run = false; q.stop();
    t1.join(); t5.join(); t2.join(); t3.join(); t4.join();
    fprintf(stderr, "[URO-M2′] stopped. final gen=%llu fg=%s mode=%s\n",
            (unsigned long long)st.generation, st.foregroundPackage.c_str(), st.mode.c_str());
    for (size_t i = 0; i < reg.size(); ++i)
        fprintf(stderr, "[URO-M2′] final: %s\n", reg.at(i).status().c_str());
    return 0;
}
