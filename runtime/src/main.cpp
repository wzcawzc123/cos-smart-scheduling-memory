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
#include <unistd.h>
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

// ==================== Telemetry（阶段C：结构化证据链）====================
// §5.6 / P0-07：我方主动动作必须带 action_id 且可审计；低频写入（§4.5 Telemetry）。
// 轮转：超过 kTelMax 压成 .1（保留一份），避免无界增长。
static constexpr size_t kTelMax = 1u << 20;   // 1MB
static std::string json_esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c >= 0x20) o += c;
    }
    return o;
}
static void tel_append(const std::string& path, const std::string& line) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (in && (size_t)in.tellg() > kTelMax) {
        in.close();
        std::remove((path + ".1").c_str());
        std::rename(path.c_str(), (path + ".1").c_str());
    }
    std::ofstream out(path, std::ios::app);
    if (out) out << line << "\n";
}

// ==================== Death Attribution（阶段C：§5.6）====================
// 证据不足必须输出 UNKNOWN——绝不伪造（APP_SELF_EXIT 只在证据足够时才可标）。
// v1 证据源：① COSMemory guard.telemetry（RECLAIM/FREEZE/KILL，100% 对应要求）
//            ② logcat（LMKD / AMS Killing / crash；时间窗口靠 logcat 自身缓冲）
struct AttrResult { std::string cause, evidence; };
static AttrResult attribute_pid(SysfsAdapter& ad, int pid, const std::string& pkg) {
    AttrResult r{"UNKNOWN", ""};
    // ---- 证据源①：guard.telemetry 格式 epoch|pid|pkg||ACTION|detail ----
    std::string telPath = "/data/system/cosmem/guard.telemetry";
    if (auto txt = ad.read_all(telPath)) {
        std::istringstream is(*txt);
        std::string ln;
        while (std::getline(is, ln)) {
            auto p1 = ln.find('|');
            if (p1 == std::string::npos) continue;
            auto p2 = ln.find('|', p1 + 1);
            if (p2 == std::string::npos) continue;
            if (atoi(ln.substr(p1 + 1, p2 - p1 - 1).c_str()) != pid) continue;
            if (!pkg.empty() && ln.find(pkg) == std::string::npos) continue;
            r.cause = "COSMemory_RECLAIM";
            r.evidence = "guard.telemetry: " + ln;
            return r;
        }
    }
    // ---- 证据源②：logcat（按语义细化，命中 pid 但语义不明则不归因）----
    auto scan = [&](const char* tag) -> bool {
        std::string cmd = std::string("logcat -d -b ") + tag +
                          " -t 500 2>/dev/null | grep -F '" + std::to_string(pid) + "'";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return false;
        char buf[512]; std::string hit;
        while (fgets(buf, sizeof buf, f)) hit += buf;
        pclose(f);
        if (hit.empty()) return false;
        if (!pkg.empty() && hit.find(pkg) == std::string::npos) return false;
        if (hit.find("lowmemorykiller") != std::string::npos ||
            hit.find("lmkd") != std::string::npos)        r.cause = "SYSTEM_LMKD";
        else if (hit.find("Killing") != std::string::npos ||
                 hit.find("am_kill") != std::string::npos) r.cause = "AMS_KILL";
        else if (hit.find("FATAL") != std::string::npos ||
                 hit.find("tombstone") != std::string::npos ||
                 hit.find("ANR in") != std::string::npos)  r.cause = "CRASH";
        else return false;
        auto pos = hit.find('\n');
        r.evidence = "logcat[" + std::string(tag) + "]: " +
                     hit.substr(0, pos == std::string::npos ? hit.size() : pos);
        return true;
    };
    if (scan("main") || scan("system") || scan("crash")) return r;
    r.evidence = "no corroboration in guard.telemetry/logcat";
    return r;   // UNKNOWN（§5.6：无法确认时不得伪造）
}

int main(int argc, char** argv) {
    const char* logdir = (argc > 1 && strcmp(argv[1], "--attribute") != 0)
                             ? argv[1] : "/sdcard/Android/UnifiedRootOptimizer/log";
    // ---- 阶段C：--attribute <pid> [pkg]：Death Attribution 查询（不进主循环）----
    // 输出 JSON；exit 0=归因成功，2=证据不足(UNKNOWN)（§5.6：不伪造）
    if (argc > 2 && strcmp(argv[1], "--attribute") == 0) {
        int pid = atoi(argv[2]);
        std::string pkg = argc > 3 ? argv[3] : "";
        SysfsAdapter ad;
        AttrResult r = attribute_pid(ad, pid, pkg);
        std::printf("{\"pid\":%d,\"cause\":\"%s\",\"evidence\":\"%s\"}\n",
                    pid, r.cause.c_str(), json_esc(r.evidence).c_str());
        return r.cause == "UNKNOWN" ? 2 : 0;
    }
    // enforce 放量：启动参数（CI/调试用）OR 开关文件（面板写入，用户可自主选择）。
    // 开关文件在每个策略边界热读 → 面板切换后无需重启即生效（§4.4 配置热重载）。
    const char* kUroConf = "/sdcard/Android/UnifiedRootOptimizer/uro.conf";
    auto read_bridge_enforce = [](const char* p) -> bool {
        std::ifstream f(p);
        if (!f.is_open()) return false;          // 文件缺省 = 关（默认 SHADOW）
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("BRIDGE_ENFORCE=1", 0) == 0) return true;
        return false;
    };
    bool argEnforce = (argc > 2 && strcmp(argv[2], "--bridge-enforce") == 0);
    bool bridgeEnforce = argEnforce || read_bridge_enforce(kUroConf);   // 仅用于启动日志
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
    reg.add(make_memory_controller(std::string(logdir) + "/policy.memory.txt",
                                   "/data/adb/modules/COSMemory/config/memory.json",
                                   std::string(logdir) + "/bridge.state"));
    reg.add(make_cpu_controller(std::string(logdir) + "/cpu.state"));
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
    fprintf(stderr, "[URO-M2′] dryRun=%d (enforce=%s: 启动参数 %s / 面板开关文件 %s)\n",
            bridgeEnforce ? 0 : 1,
            bridgeEnforce ? "ON" : "off",
            (argc > 2 && strcmp(argv[2], "--bridge-enforce") == 0) ? "on" : "off",
            read_bridge_enforce(kUroConf) ? "on" : "off");

    // ---- M3 PolicyManager：场景判定 + generation + lease ----
    PolicyManager pm;
    pm.set_game_list_path(std::string(logdir) + "/game_apps.txt");

    std::thread t1(fg_driver, std::ref(q), p, fg, std::ref(g_run));
    std::thread t5(focus_driver, std::ref(q), p, fg, std::ref(g_run));
    // v0.11 四档自治：mode_config_driver 停用——不再读系统 mode.txt（省电/高性能设置
    // 与 URO 解耦；用户不使用系统模式，判档全走自身感知：熄屏/前台/压力）。
    // std::thread t2(mode_config_driver, std::ref(q), p, std::ref(g_run));
    std::thread t3(sampler_driver, std::ref(q), p, std::ref(g_run));
    std::thread t4(screen_driver, std::ref(q), nullptr, std::ref(g_run));
    std::thread t6(touch_driver, std::ref(q), std::ref(g_run));   // 第2步：触摸边沿

    Event e;
    EffectivePolicy lastApplied;
    bool appliedOnce = false;
    bool lastEn = false;   // 上一边界的 enforce 态（用于关闭时恢复基线）

    // ---- 策略执行单元（事件驱动与租约回落共用；§3.4 写完即走）----
    auto run_policy = [&](const Event& ev, uint64_t now) {
        Decision dec = pm.decide(st, ev, now);
        pm.note_lease(dec.leaseUntilMs);
        if (!dec.changed) return;
        // 热读面板开关（每次策略边界，非轮询）：面板一开一关立即生效。
        // 注意用启动参数 argEnforce 而非缓存值 bridgeEnforce —— 后者含启动时的文件状态，
        // 会因 || 短路让热读永不执行（端到端验证抓到的 bug）。
        bool en = argEnforce || read_bridge_enforce(kUroConf);
        if (lastEn && !en) {
            // 开关刚被面板关闭：执行最后一次写入，把我方可能改动的 aggressive 恢复用户基线，
            // 随后转入 dry-run（§9 无互相覆盖的收尾；reclaim=true+maxKill=0 即 baseline 路径）
            EffectivePolicy restore;
            restore.memory.reclaimEnabled = true;
            restore.memory.maxKillPerRound = 0;
            reg.apply_all(restore, ad, /*dryRun=*/false);
            plog.line("SWITCH off -> aggressive restored to user baseline, then dry-run");
            for (auto& t : ad.traces())
                plog.line("  TRACE " + t.op + " " + t.path + " -> " + t.result +
                          (t.detail.empty() ? "" : " | " + t.detail));
            ad.clear_traces();
        }
        lastEn = en;

        EffectivePolicy base;
        base.memory.reclaimEnabled  = dec.tactics.reclaimEnabled;
        base.memory.freezeEnabled   = dec.tactics.freezeEnabled;
        base.memory.maxKillPerRound = dec.tactics.maxKillPerRound;
        base.cpu.maxFreq = dec.tactics.cpuClamp ? dec.tactics.maxFreqKhz : -1;
        base.cpu.uagUpRateUs = dec.tactics.uagUpRateUs;   // 阶段B-CPU：uag 升频延迟
        EffectivePolicy eff = reg.resolve(base, dec.generation);

        char ts[32]; time_t s = (time_t)(now / 1000); struct tm tv{};
        localtime_r(&s, &tv); strftime(ts, sizeof ts, "%F %T", &tv);
        plog.line(std::string(ts) + " SCENARIO=" + scenario_name(dec.scenario) +
                  " gen=" + std::to_string(dec.generation) +
                  " handover=" + (dec.tactics.handover ? "1" : "0") +
                  " why=" + dec.why);

        if (dec.tactics.handover) {
            // §5.5 全量让权的准确语义：
            //   CPU/GPU → 撤销我方全部上限（maxFreq=-1 本就无约束 → 零写入，不与游戏助手打架）
            //   Memory  → "暂停主动 reclaim" 是让权的执行动作，必须真正下发（aggressive=false）
            // 故此处不跳过 apply，靠 eff 自身的约束为空来保证 CPU/GPU 零写入。
            plog.line("  HANDOVER — cpu/gpu withdraw (zero writes); memory bridge pauses reclaim");
        }
        if (appliedOnce && !materially_different(eff, lastApplied)) {
            // 档位变化但参数包相同（如空包档互切 PERFORMANCE⇄BALANCE）：不写入，
            // 但决策必须入 telemetry（时间线完整性；applied=false 一目了然）
            { static int telSkip = 0; ++telSkip;
              std::ostringstream jt2;
              jt2 << "{\"ts\":" << now / 1000
                  << ",\"action_id\":\"ts" << now / 1000 << "-" << telSkip << "-" << getpid() << "\""
                  << ",\"gen\":" << eff.generation
                  << ",\"scenario\":\"" << scenario_name(dec.scenario) << "\""
                  << ",\"why\":\"" << json_esc(dec.why) << "\""
                  << ",\"enforce\":" << (en ? 1 : 0) << ",\"dryRun\":" << (en ? 0 : 1)
                  << ",\"applied\":0"
                  << ",\"handover\":" << (dec.tactics.handover ? 1 : 0)
                  << ",\"fg\":\"" << json_esc(st.foregroundPackage) << "\"}";
              tel_append(std::string(logdir) + "/telemetry.jsonl", jt2.str());
              plog.line(std::string(ts) + " SCENARIO=" + scenario_name(dec.scenario) +
                        " gen=" + std::to_string(eff.generation) + " no-write(tel applied=0)"); }
            return;
        }

        auto reps = reg.apply_all(eff, ad, /*dryRun=*/!en);
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

        // ---- 阶段C：结构化 telemetry（幂等门槛之上 = 低频；P0-07 action_id 审计）----
        {
            static int telSeq = 0;
            ++telSeq;
            std::ostringstream jt;
            jt << "{\"ts\":" << now / 1000
               << ",\"action_id\":\"t" << now / 1000 << "-" << telSeq << "-" << getpid() << "\""
               << ",\"gen\":" << eff.generation
               << ",\"scenario\":\"" << scenario_name(dec.scenario) << "\""
               << ",\"why\":\"" << json_esc(dec.why) << "\""
               << ",\"enforce\":" << (en ? 1 : 0)
               << ",\"dryRun\":" << (en ? 0 : 1)
               << ",\"applied\":1"
               << ",\"handover\":" << (dec.tactics.handover ? 1 : 0)
               << ",\"fg\":\"" << json_esc(st.foregroundPackage) << "\""
               << ",\"memAvailMb\":" << st.memAvailMb
               << ",\"psiMem10\":" << st.psiMem10
               << ",\"reclaim\":" << (eff.memory.reclaimEnabled ? 1 : 0)
               << ",\"maxKill\":" << eff.memory.maxKillPerRound
               << ",\"uagUpRate\":" << eff.cpu.uagUpRateUs
               << ",\"ctrl\":[";
            for (size_t i = 0; i < reps.size() && i < reg.size(); ++i) {
                if (i) jt << ",";
                jt << "{\"" << reg.at(i).name() << "\":\"" << ctrl_state_name(reps[i].state) << "\"}";
            }
            jt << "]}";
            tel_append(std::string(logdir) + "/telemetry.jsonl", jt.str());
        }
    };

    while (g_run) {
        if (!q.pop(e, 500)) {
            // 空闲 tick：租约到期判断 + Touch 档位回落重判（读时钟，非写入轮询；§4.4 TTL 回落）
            if (pm.lease_expired(now_ms())) {
                pm.set_pressure_time(0);            // 压力租约过期 → 清压力态
                run_policy(e, now_ms());            // 重新决策 → 回落 DAILY/POWERSAVE
                plog.line("LEASE expired -> scenario fallback");
            }
            // Touch→PERFORMANCE 的回落没有事件可等（up 之后再无事件）——空闲 tick 是唯一触发点。
            // 用 ConfigChanged 合成事件（pick 不看它，避免旧事件副作用如刷新压力滞回）；
            // 幂等由 dec.changed 判重保证：无变化不产生 apply/telemetry。
            {
                Event idle{};
                idle.type = EventType::ConfigChanged;
                idle.ts_ms = now_ms();
                idle.generation = st.generation;
                idle.source = "idle-tick";
                run_policy(idle, now_ms());
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
    t1.join(); t5.join(); t3.join(); t4.join(); t6.join();   // t2(mode driver) 已于 v0.11 停用
    fprintf(stderr, "[URO-M2′] stopped. final gen=%llu fg=%s mode=%s\n",
            (unsigned long long)st.generation, st.foregroundPackage.c_str(), st.mode.c_str());
    for (size_t i = 0; i < reg.size(); ++i)
        fprintf(stderr, "[URO-M2′] final: %s\n", reg.at(i).status().c_str());
    return 0;
}
