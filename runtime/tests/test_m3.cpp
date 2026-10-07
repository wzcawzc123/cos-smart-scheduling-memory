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

    // DAILY：普通 App
    d = pm.decide(st_with("com.tencent.mm", "balance"),
                  mk(EventType::ForegroundChanged, "com.tencent.mm"), 2000);
    CHECK(d.scenario == Scenario::DAILY, "normal fg -> DAILY");
    CHECK(!d.tactics.handover, "DAILY -> no handover");

    // POWERSAVE：mode 驱动
    d = pm.decide(st_with("com.tencent.mm", "powersave"),
                  mk(EventType::ModeChanged, "powersave"), 3000);
    CHECK(d.scenario == Scenario::POWERSAVE, "mode=powersave -> POWERSAVE");
    CHECK(d.tactics.freezeEnabled, "POWERSAVE -> freeze on");
    CHECK(d.tactics.maxKillPerRound == 3, "POWERSAVE maxKill=3");

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

    // 切 powersave → generation +1
    st.mode = "powersave";
    Decision d4 = pm.decide(st, mk(EventType::ModeChanged, "powersave"), 2000);
    CHECK(d4.changed, "mode change -> changed");
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

    // 超过滞回窗口后才回落
    Decision later = pm.decide(st, mk(EventType::ForegroundChanged, "com.tencent.mm"),
                               now + 1000 + 16000);
    CHECK(later.scenario == Scenario::DAILY, "after hysteresis -> falls back to DAILY");
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

    // 过期 → 清压力态 → 下次 decide 回落
    pm.set_pressure_time(0);
    Decision back = pm.decide(st, mk(EventType::ForegroundChanged, "com.tencent.mm"),
                              100000 + 61000);
    CHECK(back.scenario == Scenario::DAILY, "expired lease -> fallback scenario");

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
    return fail == 0 ? 0 : 1;
}
