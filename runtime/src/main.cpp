// UnifiedRootOptimizer M1 — Runtime 主程序（SHADOW 只读模式）
// 构建: NDK r26d + android-29 + arm64-v8a (构建链见 docs/M0_构建链验证_v1.md)
#include "event.hpp"
#include "state.hpp"
#include "drivers.hpp"
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
        "[URO-M2] UnifiedRootOptimizer Runtime v0.2.0 (SHADOW, read-only, focus-补盲)\n"
        "[URO-M2] fg=%s focusPoll=%dms mode=%s uroDir=%s cosmDir=%s\n"
        "[URO-M2] log=%s/events.log\n",
        p.topAppCpuset.c_str(), p.focusPollMs, p.modeFile.c_str(), p.uroDir.c_str(),
        p.cosmDir.c_str(), logdir);

    std::thread t1(fg_driver, std::ref(q), p, fg, std::ref(g_run));
    std::thread t5(focus_driver, std::ref(q), p, fg, std::ref(g_run));
    std::thread t2(mode_config_driver, std::ref(q), p, std::ref(g_run));
    std::thread t3(sampler_driver, std::ref(q), p, std::ref(g_run));
    std::thread t4(screen_driver, std::ref(q), nullptr, std::ref(g_run));

    Event e;
    while (g_run) {
        if (!q.pop(e, 500)) continue;
        bool changed = sm.apply(e, st);
        e.generation = st.generation;
        e.ts_ms = e.ts_ms ? e.ts_ms : now_ms();
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
    fprintf(stderr, "[URO-M2] stopped. final gen=%llu fg=%s mode=%s\n",
            (unsigned long long)st.generation, st.foregroundPackage.c_str(), st.mode.c_str());
    return 0;
}
