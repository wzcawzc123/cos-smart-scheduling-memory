// UnifiedRootOptimizer M1 — driver 实现 v2 (resolver: ResumedActivity 同源语义)
#include "drivers.hpp"
#include <sys/inotify.h>
#include <dirent.h>
#include <sys/system_properties.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <vector>
#include <sstream>
#include <fstream>

namespace uro {

uint64_t now_ms() {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static std::string read_all(const std::string& path) {
    std::ifstream f(path); std::string s;
    std::getline(f, s); return s;
}
static std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1);
}

// cgroup 启发式（v1，留作 dumpsys 失败时的兜底）
static std::string resolve_by_cgroup(const std::string& cgroupProcs) {
    std::ifstream f(cgroupProcs); std::string best; long bestPid = -1;
    long pid;
    while (f >> pid) {
        char buf[256] = {0};
        char p[64]; snprintf(p, sizeof p, "/proc/%ld/cmdline", pid);
        int fd = open(p, O_RDONLY); if (fd < 0) continue;
        int n = read(fd, buf, sizeof(buf) - 1); close(fd); if (n <= 0) continue;
        std::string tok(buf, strnlen(buf, n));
        if (tok.empty() || tok[0] == '/' || tok.find('.') == std::string::npos) continue;
        if (tok == "system_server") continue;
        if (pid > bestPid) { bestPid = pid; best = tok; }
    }
    return best;
}

// v2: 与 handleTopAppChanged 同源 —— 解析 ResumedActivity（事件级 fork，25 次/h 成本可忽略）
static std::string resolve_foreground(const std::string& cgroupProcs) {
    FILE* fp = popen("dumpsys activity activities 2>/dev/null | grep -m1 -E 'ResumedActivity|mResumedActivity'", "r");
    std::string pkg;
    if (fp) {
        char line[1024] = {0};
        if (fgets(line, sizeof line, fp)) {
            std::string s(line);
            auto pos = s.find("u0 ");          // "... u0 com.pkg/.Act t123}"
            if (pos != std::string::npos) {
                pos += 3;
                auto end = s.find_first_of(" \t}", pos);
                std::string tok = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                auto slash = tok.find('/');
                pkg = (slash == std::string::npos) ? tok : tok.substr(0, slash);
            }
        }
        pclose(fp);
    }
    if (pkg.empty() || pkg.find('.') == std::string::npos || pkg[0] == '/')
        return resolve_by_cgroup(cgroupProcs);   // 兜底
    return pkg;
}

// 读当前焦点窗口原始行（inotify 扳机仲裁与 focus 巡检共用）
static std::string read_focus_raw() {
    FILE* fp = popen("dumpsys window 2>/dev/null | grep -m1 'mCurrentFocus='", "r");
    std::string line;
    if (fp) {
        char buf[512];
        if (fgets(buf, sizeof buf, fp)) line = buf;
        pclose(fp);
    }
    return line;
}

// 焦点串 -> 包名。只认有包名结构的窗口，其余（null / PopupWindow / Pop-Up Window /
// ActionsDialog 等无包名值）返回空 = 本巡检周期不改变状态，保持 last-known。
static std::string pkg_from_focus(const std::string& raw) {
    // systemui 专属无点窗口名 -> 归一为真值同款包名
    for (const char* w : {"NotificationShade", "StatusBar", "NavigationBar", "Keyguard"})
        if (raw.find(w) != std::string::npos) return "com.android.systemui";
    auto u = raw.find("u0 ");                 // "Window{hash u0 com.pkg/com.act}"
    if (u == std::string::npos) return "";
    u += 3;
    auto end = raw.find_first_of(" \t}", u);
    std::string tok = raw.substr(u, end == std::string::npos ? std::string::npos : end - u);
    auto slash = tok.find('/');
    if (slash != std::string::npos) tok = tok.substr(0, slash);
    if (tok.empty() || tok.find('.') == std::string::npos || tok[0] == '/') return "";
    return tok;
}

// 焦点是否处于"非 App 态"（锁屏/通知栏/系统弹窗/无焦点）：
// 此时 ResumedActivity 仍指向底层 App，与焦点语义冲突，inotify 扳机须让位。
static bool focus_is_non_app(const std::string& raw) {
    if (raw.empty()) return false;   // 读不到焦点时不做判断（保守放行）
    for (const char* w : {"NotificationShade", "StatusBar", "NavigationBar", "Keyguard"})
        if (raw.find(w) != std::string::npos) return true;
    return pkg_from_focus(raw).empty();   // null / PopupWindow / Pop-Up Window / ActionsDialog
}

// ---------- A. ForegroundChanged（主扳机：inotify top-app） ----------
void fg_driver(EventQueue& q, DriverPaths p, FgSharedPtr fg, std::atomic<bool>& run) {
    std::string last = resolve_foreground(p.topAppCpuset);   // bootstrap 初态
    if (!last.empty() && fg->publish(last))
        q.push({EventType::ForegroundChanged, now_ms(), 0, "fg-boot", last});
    while (run) {
        int fd = inotify_init1(IN_NONBLOCK);
        if (fd < 0) { q.push({EventType::ControllerFault, now_ms(), 0, "fg-inotify", "inotify_init fail"}); sleep(5); continue; }
        int wd = inotify_add_watch(fd, p.topAppCpuset.c_str(), IN_MODIFY | IN_ATTRIB | IN_MOVED_TO);
        if (wd < 0) {
            q.push({EventType::ControllerFault, now_ms(), 0, "fg-inotify", "watch fail: " + p.topAppCpuset});
            close(fd); sleep(5); continue;
        }
        char buf[4096];
        while (run) {
            int n = read(fd, buf, sizeof buf);
            if (n > 0) {
                usleep(300 * 1000);          // 防抖 300ms（v2 参数）
                while (read(fd, buf, sizeof buf) > 0) {}
                // 双扳机仲裁：焦点处于非 App 态（锁屏/通知栏/无包名弹窗）时，
                // ResumedActivity 仍指向底层 App，会把 focus 刚建立的 systemui 态拉回。
                // 此次抑制，锁屏语义交由 focus_driver 主导（解锁后焦点回 App 即恢复正常）。
                std::string fraw = read_focus_raw();
                if (focus_is_non_app(fraw)) continue;
                std::string now = resolve_foreground(p.topAppCpuset);
                if (!now.empty() && fg->publish(now))
                    q.push({EventType::ForegroundChanged, now_ms(), 0, "fg-inotify", now});
            } else { usleep(200 * 1000); }
        }
        close(fd);
    }
}

// 屏幕状态共享（阶段①：dumpsys 巡检自适应）——screen_driver 每轮更新，
// focus_driver 据此在熄屏时完全跳过 dumpsys window（省 2% 单核常驻，§见 focus 段注释）
static std::atomic<bool> g_screen_on{true};

// ---------- A'. ForegroundChanged（补盲扳机：mCurrentFocus 巡检） ----------
// 修 M1 影子对账 v2 §2-A1：systemui 常驻 top-app cgroup，下拉通知栏/锁屏时
// cgroup 成员无变化 -> inotify 永不触发（22 条直接 missed + 8 条恢复事件下游）。
// dumpsys window 单次实测 14-20ms，1s 巡检的常驻成本约 2% 单核占空。
// 自适应（阶段①）：熄屏时无焦点语义（用户不能操作、前台不会因交互变化），
// 跳过 dumpsys、每 2s 醒一次等亮屏；恢复后第一轮 diff 自然补推焦点变化（lastRaw 保留）。
void focus_driver(EventQueue& q, DriverPaths p, FgSharedPtr fg, std::atomic<bool>& run) {
    std::string lastRaw;   // 本源上次看到的原始焦点串（避免每个周期都解析）
    while (run) {
        if (!g_screen_on.load(std::memory_order_relaxed)) {
            static bool muted = false;   // 暂停/恢复日志各打一次（防 2s 循环刷屏）
            if (!muted) { fprintf(stderr, "[URO-fg] screen-off: dumpsys巡检暂停(自适应)\n"); muted = true; }
            bool resumed = false;
            for (int i = 0; run && i < 20; ++i) {   // 熄屏期 2s 一醒（仅读共享标志）
                usleep(100 * 1000);
                if (g_screen_on.load(std::memory_order_relaxed)) { resumed = true; break; }
            }
            if (resumed) { fprintf(stderr, "[URO-fg] screen-on: 恢复 1s 巡检\n"); muted = false; }
            continue;   // 恢复后走正常轮：read_focus_raw 与 lastRaw diff 补推
        }
        std::string raw = read_focus_raw();
        if (!raw.empty() && raw != lastRaw) {
            lastRaw = raw;
            std::string pkg = pkg_from_focus(raw);
            if (!pkg.empty() && fg->publish(pkg))
                q.push({EventType::ForegroundChanged, now_ms(), 0, "fg-focus", pkg});
        }
        for (int i = 0; run && i < p.focusPollMs / 100; ++i) usleep(100 * 1000);
    }
}

// ---------- B. ModeChanged / ConfigChanged ----------
void mode_config_driver(EventQueue& q, DriverPaths p, std::atomic<bool>& run) {
    while (run) {
        int fd = inotify_init1(IN_NONBLOCK);
        if (fd < 0) { sleep(5); continue; }
        bool ok1 = inotify_add_watch(fd, p.uroDir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) >= 0;
        bool ok2 = inotify_add_watch(fd, p.cosmDir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) >= 0;
        if (!ok1 && !ok2) {
            q.push({EventType::ControllerFault, now_ms(), 0, "cfg-inotify", "no watchable config dir"});
            close(fd); sleep(10); continue;
        }
        std::string lastMode = trim(read_all(p.modeFile));   // bootstrap 初态
        if (!lastMode.empty())
            q.push({EventType::ModeChanged, now_ms(), 0, "cfg-boot", lastMode});
        char buf[8192];
        while (run) {
            int n = read(fd, buf, sizeof buf);
            if (n > 0) {
                int off = 0;
                while (off < n) {
                    auto* ev = reinterpret_cast<struct inotify_event*>(buf + off);
                    std::string name = (ev->len ? std::string(ev->name, strnlen(ev->name, ev->len)) : "");
                    off += sizeof(struct inotify_event) + ev->len;
                    if (ev->mask & (IN_IGNORED | IN_Q_OVERFLOW)) continue;
                    if (name == "mode.txt" && (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) {
                        std::string m = trim(read_all(p.modeFile));
                        if (!m.empty() && m != lastMode) {
                            lastMode = m;
                            q.push({EventType::ModeChanged, now_ms(), 0, "cfg-inotify", m});
                        }
                    } else if (!name.empty() && (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) {
                        q.push({EventType::ConfigChanged, now_ms(), 0, "cfg-inotify", name});
                    }
                }
            } else usleep(200 * 1000);
        }
        close(fd);
    }
}

// ---------- C. 阈值采样：压力 + 充电 ----------
void sampler_driver(EventQueue& q, DriverPaths p, std::atomic<bool>& run) {
    std::string lastChg; uint64_t lastFire = 0; bool primed = false;
    while (run) {
        float psi = -1.f; long mav = -1;
        { std::ifstream f("/proc/pressure/memory"); std::string line;
          if (std::getline(f, line) && line.rfind("some", 0) == 0) {
              auto pos = line.find("avg10="); if (pos != std::string::npos) psi = atof(line.c_str() + pos + 6);
          } }
        { std::ifstream f("/proc/meminfo"); std::string k; long v; std::string u;
          while (f >> k >> v >> u) if (k == "MemAvailable:") { mav = v / 1024; break; } }
        std::string chg = trim(read_all("/sys/class/power_supply/battery/status"));
        if (primed && !chg.empty() && chg != lastChg) {
            lastChg = chg;
            q.push({EventType::ChargerChanged, now_ms(), 0, "sampler", chg});
        } else if (!primed && !chg.empty()) lastChg = chg;
        primed = true;
        uint64_t t = now_ms();
        if (psi >= 0 && (psi * 10 >= p.psiThresholdTenx || (mav > 0 && mav < p.memFloorMb))) {
            if (t - lastFire >= (uint64_t)p.pressureCooldownSec * 1000) {
                lastFire = t;
                char pl[96]; snprintf(pl, sizeof pl, "psi=%.2f memAvailMb=%ld", psi, mav);
                q.push({EventType::MemoryPressureChanged, t, 0, "sampler", pl});
            }
        }
        for (int i = 0; run && i < p.pollIntervalMs / 100; ++i) usleep(100 * 1000);
    }
}

// ---------- D. ScreenChanged（property 零 fork；缺失 → DEGRADED 一次） ----------
void screen_driver(EventQueue& q, EventQueue*, std::atomic<bool>& run) {
    const prop_info* pi = __system_property_find("debug.tracing.screen_state");
    if (!pi) {
        q.push({EventType::ControllerFault, now_ms(), 0, "screen-prop", "prop missing: debug.tracing.screen_state (DEGRADED, screen disabled)"});
        while (run) usleep(500 * 1000);
        return;
    }
    int last = -2;
    while (run) {
        char name[PROP_NAME_MAX] = {0}, val[PROP_VALUE_MAX] = {0};
        __system_property_read(pi, name, val);
        int raw = atoi(val);
        // Oplus 实测语义（2026-10-08）：亮屏=2、熄屏=1（原始代码判 raw==0 为灭 → 熄屏值 1 被
        // 误判为亮，ScreenChanged(off) 永不触发）。按"2=亮，其余=灭"判定。
        int st = (raw == 2) ? 1 : 0;
        g_screen_on.store(st != 0, std::memory_order_relaxed);   // 供 focus_driver 自适应
        if (st != last) {
            if (last != -2) q.push({EventType::ScreenChanged, now_ms(), 0, "screen-prop", st ? "on" : "off"});
            last = st;
        }
        usleep((st ? 1000 : 2000) * 1000);
    }
}

// ==================== Touch driver（第2步·智能感知）====================
// 监听触摸屏 input 设备（/proc/bus/input/devices 中 Name="touchpanel" → eventN）。
// 只认 BTN_TOUCH down/up 边沿——每下触摸仅 2 个事件，天然节流（ABS 移动流全部丢弃）。
// 失败降级：设备缺失/打不开 → ControllerFault（主流程继续，触摸感知置灰，不拖垮）。
void touch_driver(EventQueue& q, std::atomic<bool>& run) {
    std::string evPath;
    {
        std::ifstream pf("/proc/bus/input/devices");
        std::string line, name;
        bool named = false;
        while (std::getline(pf, line)) {
            if (line.rfind("N: Name=", 0) == 0) { name = line; named = true; }
            else if (named && line.rfind("H: Handlers=", 0) == 0) {
                named = false;
                if (name.find("touchpanel") == std::string::npos) continue;
                auto p = line.find("event");
                if (p != std::string::npos) {
                    std::string num;
                    for (size_t i = p + 5; i < line.size() && isdigit((unsigned char)line[i]); ++i)
                        num += line[i];
                    if (!num.empty()) evPath = "/dev/input/event" + num;
                }
                if (!evPath.empty()) break;
            }
        }
    }
    if (evPath.empty()) {
        q.push({EventType::ControllerFault, now_ms(), 0, "touch", "touchpanel device not found (touch感知关闭)"});
        while (run) sleep(1);
        return;
    }
    int fd = open(evPath.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        q.push({EventType::ControllerFault, now_ms(), 0, "touch", "open " + evPath + " failed (touch感知关闭)"});
        while (run) sleep(1);
        return;
    }
    int last = -1;
    while (run) {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = poll(&pfd, 1, 100);
        if (r > 0 && (pfd.revents & POLLIN)) {
            struct input_event ie;
            while (read(fd, &ie, sizeof ie) == (ssize_t)sizeof ie) {
                if (ie.type == EV_KEY && ie.code == BTN_TOUCH) {
                    int v = ie.value ? 1 : 0;
                    if (v != last) {
                        last = v;
                        q.push({EventType::TouchChanged, now_ms(), 0, "touch-input", v ? "down" : "up"});
                    }
                }
            }
        }
    }
    close(fd);
}

// 驱动：60s 低频 Evidence 轮询（不推事件；告警走 thread.jsonl 字段 + stderr WARN）
void thread_evidence_driver(const std::string& logdir, const std::string& appoptConf,
                            const std::string& gamePath, const std::string& cpusetRoot,
                            std::atomic<bool>& run) {
    bool genChecked = false;
    while (run) {
        AppOptSummary s = read_appopt(appoptConf, gamePath, cpusetRoot);
        if (s.present) {
            if (!genChecked) { ensure_uro_gen_block(appoptConf); genChecked = true; }
            reconcile_uro_gen(appoptConf,
                "/sdcard/Android/UnifiedRootOptimizer/app_profiles.txt");   // 指挥链② 对账
            std::ostringstream o;
            o << "{\"ts\":" << now_ms() << ",\"present\":true,\"rules\":" << s.rules << ",\"groups\":{";
            for (size_t i = 0; i < s.groups.size(); ++i) {
                if (i) o << ",";
                o << "\"" << s.groups[i].first << "\":" << s.groups[i].second;
            }
            o << "}";
            if (!s.gameViolation.empty()) o << ",\"gameViolation\":\"" << s.gameViolation << "\"";
            o << "}\n";
            std::ofstream lf(logdir + "/thread.jsonl", std::ios::app);
            lf << o.str();
            if (!s.gameViolation.empty())
                fprintf(stderr, "[URO-thread] WARN 游戏包被 AppOpt 活跃规则覆盖(10-07应豁免): %s\n",
                        s.gameViolation.c_str());
        }
        for (int i = 0; run && i < 600; ++i) usleep(100 * 1000);   // 60s
    }
}

// ---------- Evidence 扩展（v0.17：FPS + 温度，T-OBS 只读）----------
// 60s 双采样：thermal → thermal.jsonl（top3+battery）；gfxinfo 前台包差分 → fps.jsonl。
// gfxinfo 是累计计数：同包做差分得窗口增量，切包重置基线（首采只记基线行）。
// 心跳定位：evidence.hb = 最后执行到的阶段（f65a014b 崩溃无现场的补救，v0.17.2）
static void ev_hb(const std::string& logdir, const char* stage) {
    std::ofstream f(logdir + "/evidence.hb", std::ios::trunc);
    f << stage << " " << now_ms() << "\n";
}

void evidence_driver(const std::string& logdir, FgSharedPtr fg, std::atomic<bool>& run) {
    std::string lastPkg;
    long lastTotal = -1, lastJanky = 0;
    while (run) {
        ev_hb(logdir, "loop-start");
        {
            ev_hb(logdir, "thermal-begin");
            std::string tj = thermal_json("/sys/class/thermal");
            std::string bj = battery_json("/sys/class/power_supply");   // M5 电量分母
            std::ofstream lf(logdir + "/thermal.jsonl", std::ios::app);
            lf << "{\"ts\":" << now_ms() << "," << tj << "," << bj << "}\n";
            ev_hb(logdir, "thermal-done");
        }
        {
            std::string pkg = fg->get();
            if (!pkg.empty()) {
                ev_hb(logdir, "fps-popen-begin");
                std::string cmd = "dumpsys gfxinfo " + pkg + " 2>/dev/null";
                FILE* fp = popen(cmd.c_str(), "r");
                if (fp) {
                    char buf[4096];
                    size_t n = fread(buf, 1, sizeof buf - 1, fp);
                    buf[n] = 0;
                    pclose(fp);
                    ev_hb(logdir, "fps-parsed");
                    long total = -1, janky = 0;
                    if (parse_gfxinfo(buf, total, janky)) {
                        if (pkg != lastPkg || lastTotal < 0) {
                            if (pkg != lastPkg) {
                                std::ofstream lf(logdir + "/fps.jsonl", std::ios::app);
                                lf << "{\"ts\":" << now_ms() << ",\"pkg\":\"" << pkg
                                   << "\",\"reset\":1,\"total\":" << total << "}\n";
                            }
                            lastPkg = pkg; lastTotal = total; lastJanky = janky;
                        } else if (total >= lastTotal) {
                            long dt = total - lastTotal, dj = janky - lastJanky;
                            std::ofstream lf(logdir + "/fps.jsonl", std::ios::app);
                            lf << "{\"ts\":" << now_ms() << ",\"pkg\":\"" << pkg
                               << "\",\"frames\":" << dt << ",\"janky\":" << dj << "}\n";
                            lastTotal = total; lastJanky = janky;
                        }
                    }
                }
            }
        }
        ev_hb(logdir, "loop-end");
        for (int i = 0; run && i < 600; ++i) usleep(100 * 1000);   // 60s
    }
    ev_hb(logdir, "exited");
}

} // namespace uro


