// UnifiedRootOptimizer M3 — PolicyManager / 场景 / Handover 测试
// §9 M3 退出条件：无策略震荡、无互相覆盖。全部在 host 上跑，无需设备。
// 构建: g++ -std=c++20 -Iinclude src/adapter.cpp src/controllers.cpp tests/test_m3.cpp -o /tmp/t && /tmp/t
#include "../include/policy_manager.hpp"
#include "../include/controller.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace uro;

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static std::string tmpdir() {
    char b[128];
    snprintf(b, sizeof b, "/tmp/uro_m3_%d", (int)getpid());
    mkdir(b, 0755);
    return b;
}

static Event mk(EventType t, const std::string& payload, const std::string& src = "test") {
    Event e; e.type = t; e.ts_ms = 0; e.generation = 0; e.source = src; e.payload = payload;
    return e;
}
static GlobalState st_with(const std::string& fg, const std::string& mode, uint64_t gen = 5) {
    GlobalState s; s.foregroundPackage = fg; s.mode = mode; s.generation = gen; return s;
}

// ---------- 1. 场景判定（§3.3 / §5.5）----------
static void test_scenario(const std::string& dir) {
    std::string list = dir + "/game_apps.txt";
    { std::ofstream f(list); f << "# 游戏名单\ncom.tencent.tmgp.sgame\ncom.miHoYo.Yuanshen\n"; }
    PolicyManager pm;
    pm.set_game_list_path(list);

    // GAME：前台命中名单
    Decision d = pm.decide(st_with("com.tencent.tmgp.sgame", "balance"),
                           mk(EventType::ForegroundChanged, "com.tencent.tmgp.sgame"), 1000);
    CHECK(d.scenario == Scenario::GAME, "fg in game list -> GAME");
    CHECK(d.tactics.handover, "GAME -> handover=true (§5.5)");
    CHECK(!d.tactics.reclaimEnabled, "GAME -> reclaim paused (仅白名单保护)");
    CHECK(d.tactics.maxFreqKhz < 0, "GAME -> no CPU clamp (baseline 撤权)");

    // AmSwitch：刚切 App = FAST 过渡（2.5s 窗口；CT 场景机同款"最激进档"）
    d = pm.decide(st_with("com.tencent.mm", "balance"),
                  mk(EventType::ForegroundChanged, "com.tencent.mm"), 2000);
    CHECK(d.scenario == Scenario::FAST, "fg switch -> FAST (AmSwitch 2.5s)");
    CHECK(d.tactics.uagDownRateUs == 150000, "FAST -> down 150ms");
    // 窗口过期后回 BALANCE（空闲 tick 触发的真实路径）
    d = pm.decide(st_with("com.tencent.mm", "balance"),
                  mk(EventType::ConfigChanged, "idle-tick"), 5000);
    CHECK(d.scenario == Scenario::BALANCE, "switch window expired -> BALANCE");
    CHECK(!d.tactics.handover, "DAILY -> no handover");

    // ---- 熄屏自动省电（场景自动映射：亮屏日用 / 熄屏省电）----
    {
        GlobalState st;
        st.screenOn = false;
        Event e{};
        e.type = EventType::ScreenChanged;
        e.payload = "off";
        e.ts_ms = 1000;

        auto d = pm.decide(st, e, 1000);
        CHECK(d.scenario == Scenario::POWER_SAVE, "screen off -> POWERSAVE (自动)");
        CHECK(d.why.find("screen off") != std::string::npos, "why 记录熄屏触发");

        // 熄屏压过 GAME（挂机游戏交由省电接管；进程安全：reclaim 不杀前台）
        GlobalState st2;
        st2.screenOn = false;
        st2.foregroundPackage = "com.tencent.tmgp.sgame";
        auto d2 = pm.decide(st2, e, 2000);
        CHECK(d2.scenario == Scenario::POWER_SAVE, "screen off + game fg -> POWERSAVE 优先");

        // 亮屏 + 游戏 → GAME 照常（不误伤）
        GlobalState st3;
        st3.screenOn = true;
        st3.foregroundPackage = "com.tencent.tmgp.sgame";
        auto d3 = pm.decide(st3, e, 3000);
        CHECK(d3.scenario == Scenario::GAME, "screen on + game -> GAME 照常");
        std::printf("[screen-auto] ok (熄屏省电/压过GAME/亮屏恢复)\n");
    }

    // ---- App 画像（第2步·CT 遗产）：通配符覆盖 > 档位；GAME/POWER_SAVE 不应用 ----
    {
        PolicyManager pp;
        std::string pf = dir + "/app_profiles.txt";
        { std::ofstream f(pf);
          f << "# pattern|down|up  (-1=跟随档位)\n"
               "com.tencent.mm*|200000|-1\n"      // 微信系：down 强制 200ms
               "org.example.?pp|300000|500\n"      // ? 单字符匹配
               "*.speedtest*|250000|5000\n"; }     // 段通配
        pp.set_app_profiles_path(pf);

        GlobalState st; st.screenOn = true;
        Event td{}; td.type = EventType::TouchChanged; td.payload = "down";

        // 画像 > 档位：PERFORMANCE 档 down=80000 被画像 200000 覆盖
        st.foregroundPackage = "com.tencent.mm";
        auto a = pp.decide(st, td, 10000);
        CHECK(a.scenario == Scenario::PERFORMANCE, "touch in mm -> PERFORMANCE");
        CHECK(a.tactics.uagDownRateUs == 200000, "profile overrides level (200000)");
        CHECK(a.why.find("profile=") != std::string::npos, "why 记录画像命中");
        CHECK(a.tactics.uagUpRateUs == -1, "profile -1 -> 跟随档位");

        // 通配语义：? 与 * 段匹配
        st.foregroundPackage = "org.example.zpp";
        auto b = pp.decide(st, td, 13000);
        CHECK(b.tactics.uagDownRateUs == 300000, "? wildcard match (300000)");
        st.foregroundPackage = "com.nperf.speedtest";
        auto c = pp.decide(st, td, 16000);
        CHECK(c.tactics.uagDownRateUs == 250000, "* wildcard match (250000)");

        // 无匹配 → 档位值（80000）
        st.foregroundPackage = "com.other.app";
        auto d = pp.decide(st, td, 19000);
        CHECK(d.tactics.uagDownRateUs == 80000, "no profile -> level value 80000");
        CHECK(d.why.find("profile=") == std::string::npos, "no profile tag in why");

        // POWER_SAVE 不应用画像（安全档不被破解）
        st.screenOn = false;
        st.foregroundPackage = "com.tencent.mm";
        auto e = pp.decide(st, td, 22000);
        CHECK(e.scenario == Scenario::POWER_SAVE, "screen off -> POWER_SAVE");
        CHECK(e.tactics.uagDownRateUs < 0, "POWER_SAVE 画像不应用");

        // 文件缺失降级
        PolicyManager pn;
        pn.set_app_profiles_path(dir + "/no_such_profiles.txt");
        st.screenOn = true;
        auto f = pn.decide(st, td, 1000);
        CHECK(f.tactics.uagDownRateUs == 80000, "missing file -> level only");
        std::printf("[profile] ok (覆盖/通配/降级/安全档)\n");
    }

    // ---- Touch 感知（第2步）：down→PERFORMANCE、1.5s 滞回回落、GAME/熄屏优先 ----
    {
        PolicyManager pt;
        { std::string gl = dir + "/touch_game.txt";
          std::ofstream f(gl); f << "com.tencent.tmgp.sgame\n"; pt.set_game_list_path(gl); }
        GlobalState st;
        st.screenOn = true;
        st.foregroundPackage = "com.tencent.mm";
        Event td{};
        td.type = EventType::TouchChanged;
        td.payload = "down";
        auto a = pt.decide(st, td, 10000);
        CHECK(a.scenario == Scenario::PERFORMANCE, "touch -> PERFORMANCE");
        CHECK(a.why.find("touch") != std::string::npos, "why=touch");
        Event idle{};
        idle.type = EventType::ConfigChanged;
        idle.source = "idle-tick";
        auto b = pt.decide(st, idle, 11400);
        CHECK(b.scenario == Scenario::PERFORMANCE, "within 1.5s hold PERFORMANCE");
        auto c = pt.decide(st, idle, 11600);
        CHECK(c.scenario == Scenario::BALANCE, "touch timeout -> BALANCE 回落");
        st.foregroundPackage = "com.tencent.tmgp.sgame";
        auto d = pt.decide(st, td, 12000);
        CHECK(d.scenario == Scenario::GAME, "touch in game -> GAME wins");
        st.foregroundPackage = "com.tencent.mm";
        st.screenOn = false;
        auto e2 = pt.decide(st, td, 13000);
        CHECK(e2.scenario == Scenario::POWER_SAVE, "touch while screen off -> POWER_SAVE");
        std::printf("[touch] ok (PERFORMANCE/滞回/GAME优先/熄屏优先)\n");
    }

    // v0.11 四档自治：系统 mode.txt 不再影响档位（用户不依赖系统省电/高性能设置）
    // （时间放在 FG 后 2.5s+ 以免撞 AmSwitch FAST 窗口）
    d = pm.decide(st_with("com.tencent.mm", "powersave"),
                  mk(EventType::ModeChanged, "powersave"), 5000);
    CHECK(d.scenario == Scenario::BALANCE, "system mode IGNORED (v0.11 四档自治)");

    // MEMORY_PRESSURE：压力事件
    d = pm.decide(st_with("com.tencent.mm", "balance"),
                  mk(EventType::MemoryPressureChanged, "psi=8.0 memAvailMb=600"), 4000);
    CHECK(d.scenario == Scenario::MEMORY_PRESSURE, "pressure event -> MEMORY_PRESSURE");
    CHECK(d.tactics.reclaimEnabled && d.tactics.maxKillPerRound == 5, "pressure -> reclaim up");
    CHECK(d.leaseUntilMs == 4000 + 60000, "pressure policy carries 60s lease");

    std::printf("[scenario] ok\n");
}

// ---------- 2. GAME 压倒压力场景（§5.5 全量让权不被覆盖）----------
static void test_game_wins(const std::string& dir) {
    std::string list = dir + "/game2.txt";
    { std::ofstream f(list); f << "com.tencent.tmgp.sgame\n"; }
    PolicyManager pm;
    pm.set_game_list_path(list);
    pm.set_pressure_time(1000);   // 正处于压力滞回窗口

    Decision d = pm.decide(st_with("com.tencent.tmgp.sgame", "balance"),
                           mk(EventType::MemoryPressureChanged, "psi=9"), 5000);
    CHECK(d.scenario == Scenario::GAME, "GAME outranks MEMORY_PRESSURE");
    CHECK(d.tactics.handover, "handover wins over pressure reclaim");

    std::printf("[game-wins] ok\n");
}

// ---------- 3. generation 单调 + 幂等（§4.4）----------
static void test_generation() {
    PolicyManager pm;
    GlobalState st = st_with("com.android.launcher", "balance");
    Event e = mk(EventType::ForegroundChanged, "com.android.launcher");

    Decision d1 = pm.decide(st, e, 1000);
    uint64_t g1 = d1.generation;
    // 相同输入重放 → 不 +1（幂等，防震荡核心）
    Decision d2 = pm.decide(st, e, 1010);
    Decision d3 = pm.decide(st, e, 1020);
    CHECK(!d2.changed && d2.generation == g1, "identical re-decide -> no new generation");
    CHECK(!d3.changed, "3rd identical re-decide -> still unchanged");

    // 熄屏进省电档 → generation +1（v0.11：档位切换驱动代数，系统 mode 已解耦）
    st.screenOn = false;
    Decision d4 = pm.decide(st, mk(EventType::ScreenChanged, "off"), 2000);
    CHECK(d4.changed, "screen off -> POWER_SAVE -> changed");
    CHECK(d4.generation == g1 + 1, "generation +1 exactly once");
    CHECK(d4.generation > g1, "generation monotonic");

    std::printf("[generation] ok (monotonic + idempotent)\n");
}

// ---------- 4. 防震荡：压力事件风暴不产生 apply 风暴 ----------
static void test_no_oscillation() {
    PolicyManager pm;
    pm.set_hysteresis_ms(15000);
    GlobalState st = st_with("com.tencent.mm", "balance");

    int changedCnt = 0;
    uint64_t now = 10000;
    // 20 次压力事件连发（100ms 间隔 = 模拟抖动）
    for (int i = 0; i < 20; ++i) {
        Decision d = pm.decide(st, mk(EventType::MemoryPressureChanged, "psi=6"), now);
        if (d.changed) changedCnt++;
        pm.set_pressure_time(now);
        now += 100;
    }
    CHECK(changedCnt <= 1, "pressure storm -> at most 1 decision change");

    // 压力刚发生，立刻回 DAILY 应被滞回挡住
    Decision back = pm.decide(st, mk(EventType::ForegroundChanged, "com.tencent.mm"), now + 1000);
    CHECK(back.scenario == Scenario::MEMORY_PRESSURE, "hysteresis blocks immediate exit");

    // 超过滞回窗口后才回落（用 idle 事件避免刷 AmSwitch FAST 窗口）
    Decision later = pm.decide(st, mk(EventType::ConfigChanged, "idle-tick"),
                               now + 1000 + 16000);
    CHECK(later.scenario == Scenario::BALANCE, "after hysteresis -> falls back to DAILY");
    CHECK(later.changed, "fallback produces one generation bump");

    std::printf("[no-oscillation] ok\n");
}

// ---------- 5. 租约过期回落（§4.4 TTL）----------
static void test_lease() {
    PolicyManager pm;
    pm.set_pressure_lease_ms(60000);
    GlobalState st = st_with("com.tencent.mm", "balance");
    Decision d = pm.decide(st, mk(EventType::MemoryPressureChanged, "psi=7"), 100000);
    pm.note_lease(d.leaseUntilMs);

    CHECK(!pm.lease_expired(100000 + 59000), "lease alive before deadline");
    CHECK(pm.lease_expired(100000 + 61000), "lease expired after deadline");

    // 过期 → 清压力态 → 下次 decide 回落（idle 事件避免刷 AmSwitch FAST 窗口）
    pm.set_pressure_time(0);
    Decision back = pm.decide(st, mk(EventType::ConfigChanged, "idle-tick"),
                              100000 + 61000);
    CHECK(back.scenario == Scenario::BALANCE, "expired lease -> fallback scenario");

    std::printf("[lease] ok\n");
}

// ---------- 6. resolve(base)：场景基底叠加 Controller 约束 ----------
static void test_resolve_base() {
    SysfsAdapter ad("/nonexistent/x");     // 所有 Controller 都降级
    ControllerRegistry reg;
    reg.add(make_memory_controller());
    reg.add(make_cpu_controller());
    reg.add(make_gpu_placeholder());
    reg.probe_all(ad);

    EffectivePolicy base;
    base.memory.reclaimEnabled = true;
    base.memory.freezeEnabled = true;
    base.memory.maxKillPerRound = 5;
    EffectivePolicy eff = reg.resolve(base, 42);

    CHECK(eff.generation == 42, "resolve carries generation");
    CHECK(eff.memory.maxKillPerRound == 5, "base tactics preserved through resolve");
    CHECK(eff.memory.reclaimEnabled, "base reclaim preserved");

    // 全部 Degraded 时不能把基底抹掉（互相覆盖 = 退出条件反例）
    CHECK(eff.memory.freezeEnabled, "degraded controllers must not override base");

    std::printf("[resolve-base] ok\n");
}

// ---------- 7. 名单缺失降级（GAME 不可触发，不崩溃）----------
static void test_game_list_missing() {
    PolicyManager pm;
    pm.set_game_list_path("/nonexistent/game_apps.txt");
    Decision d = pm.decide(st_with("com.tencent.tmgp.sgame", "balance"),
                           mk(EventType::ForegroundChanged, "com.tencent.tmgp.sgame"), 1000);
    CHECK(d.scenario != Scenario::GAME, "missing game list -> GAME disabled (degrade)");
    CHECK(!d.tactics.handover, "no handover without list");

    std::printf("[game-list-missing] ok\n");
}

int main() {
    std::printf("=== M3 PolicyManager 测试（§9: 无震荡 / 无互相覆盖）===\n");
    std::string dir = tmpdir();
    test_scenario(dir);
    test_game_wins(dir);
    test_generation();
    test_no_oscillation();
    test_lease();
    test_resolve_base();
    test_game_list_missing();

    std::printf("\n结果: %d passed, %d failed\n", pass, fail);
    std::string cmd = "rm -rf '" + dir + "'";
    system(cmd.c_str());
        // ---- [thermal] T-OBS→干预：夹档 ----
    {
        using namespace uro;
        PolicyManager pm2;
        Event e{EventType::TouchChanged, 1000, 1, "t", ""};
        GlobalState st2; st2.screenOn = true; st2.foregroundPackage = "com.x";
        // 无热：触摸 -> PERFORMANCE
        auto d1 = pm2.decide(st2, e, 1000);
        // 级1：PERFORMANCE 被夹到 BALANCE、why 含 thermal
        st2.thermalLevel = 1;
        auto d2 = pm2.decide(st2, e, 2000);
        CHECK(d2.scenario == Scenario::BALANCE, "L1夹PERF->BALANCE");
        CHECK(d2.why.find("thermal") != std::string::npos, "why标注thermal");
        // 级2：POWER_SAVE
        st2.thermalLevel = 2;
        auto d3 = pm2.decide(st2, e, 3000);
        CHECK(d3.scenario == Scenario::POWER_SAVE, "L2夹->POWER_SAVE");
        // 解除：级0 恢复
        st2.thermalLevel = 0;
        auto d4 = pm2.decide(st2, e, 4000);
        CHECK(d4.scenario == Scenario::PERFORMANCE, "解除恢复PERF");
        std::printf("[thermal] ok (夹档L1/L2/解除/why)\n");
    }

    // ---- [power-guard] L4 电量守卫（Evidence 回喂）夹档 ----
    {
        using namespace uro;
        PolicyManager pm3;
        Event e{EventType::TouchChanged, 1000, 1, "t", ""};
        GlobalState st3; st3.screenOn = true; st3.foregroundPackage = "com.x";
        st3.batteryPct = 15;
        auto d2 = pm3.decide(st3, e, 2000);
        CHECK(d2.scenario == Scenario::BALANCE, "低电15%夹PERF->BALANCE");
        CHECK(d2.why.find("power-guard") != std::string::npos, "why标注power-guard");
        st3.batteryPct = 8;
        auto d3 = pm3.decide(st3, e, 3000);
        CHECK(d3.scenario == Scenario::POWER_SAVE, "极低8%->POWER_SAVE");
        st3.batteryPct = 15; st3.charging = true;
        auto d4 = pm3.decide(st3, e, 4000);
        CHECK(d4.scenario == Scenario::PERFORMANCE, "充电中不夹");
        std::printf("[power-guard] ok (15%%夹/8%%PS/充电豁免/why)\n");
    }

return fail == 0 ? 0 : 1;
}
