// UnifiedRootOptimizer v0.33 — 参数外置（场景配方覆盖）测试
// 覆盖 docs/参数外置_v0.33_spec.md §6 验收 1-3（host，无需设备）
// 构建: g++ -std=c++20 -Iinclude src/adapter.cpp src/controllers.cpp src/appopt.cpp src/evidence.cpp tests/test_tactics.cpp -o /tmp/t
#include "../include/tactics_conf.hpp"
#include "../include/policy_manager.hpp"
#include <cstdio>
#include <string>

using namespace uro;

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static Event mk(EventType t, const std::string& payload) {
    Event e; e.type = t; e.ts_ms = 0; e.generation = 0; e.source = "test"; e.payload = payload;
    return e;
}
static GlobalState st_with(const std::string& fg, bool screenOn) {
    GlobalState s; s.foregroundPackage = fg; s.screenOn = screenOn; s.ts_ms = 0;
    return s;
}

int main() {
    // ---------- A. 解析：空与注释 ----------
    {
        TacticsConf c = parse_tactics_conf("");
        CHECK(c.ok, "A1 空文本 -> ok");
        CHECK(c.applied == 0, "A2 空文本 -> applied=0");
        CHECK(c.byScenario.empty(), "A3 空文本 -> 无场景");
    }
    {
        std::string t = "# comment\r\n// comment2\r\n\r\n[POWER_SAVE]\r\nuagUp=2500\r\n";
        TacticsConf c = parse_tactics_conf(t);
        CHECK(c.ok, "A4 注释+CRLF -> ok");
        CHECK(c.applied == 1, "A5 注释+CRLF -> 1 字段");
        CHECK(c.byScenario[Scenario::POWER_SAVE]["uagUp"] == "2500", "A6 值解析正确");
    }
    // ---------- B. 多场景合法配置 ----------
    {
        std::string t =
            "[POWER_SAVE]\nreclaim=1\nfreeze=1\nmaxKill=3\nuagUp=2500\ngpuIdle=50\n"
            "[PERFORMANCE]\nuagHi=80\ngpuIdle=120\ncpuClamp=1\nmaxFreqK=2400000\n"
            "[FAST]\nuagDown=150000\n";
        TacticsConf c = parse_tactics_conf(t);
        CHECK(c.ok, "B1 多场景 -> ok");
        CHECK(c.applied == 10, "B2 字段计数");
        CHECK(c.byScenario.size() == 3, "B3 三个场景");
        CHECK(c.byScenario[Scenario::PERFORMANCE]["maxFreqK"] == "2400000", "B4 maxFreqK 值");
        CHECK(c.byScenario[Scenario::POWER_SAVE].size() == 5, "B5 POWER_SAVE 字段数=5");
    }
    // ---------- C. 整份拒绝（spec §4） ----------
    struct Bad { const char* text; const char* msg; };
    Bad bads[] = {
        {"[UNKNOWN]\nreclaim=1\n",              "C1 未知场景"},
        {"[BALANCE]\nnotafield=1\n",            "C2 未知字段"},
        {"[BALANCE]\nmaxKill=99\n",             "C3 值越界"},
        {"[BALANCE]\ncpuClamp=2\n",             "C4 布尔非0/1"},
        {"[BALANCE]\ngpuIdle=abc\n",            "C5 非数字"},
        {"reclaim=1\n[BALANCE]\n",              "C6 字段在段前"},
        {"[BALANCE]\ndepth=deep\n",             "C7 depth 非法"},
        {"[BALANCE]\nmaxFreqK=299999\n",        "C8 maxFreqK 越界"},
        {"[BALANCE\nreclaim=1\n",               "C9 段头缺 ]"},
        {"[BALANCE]\nreclaim\n",                "C10 缺 ="},
        {"[BALANCE]\nuagDown=2000000\n",        "C11 uagDown 越界"},
        {"[BALANCE]\nmaxKill=\n",               "C12 空值"},
    };
    for (const Bad& b : bads) {
        TacticsConf c = parse_tactics_conf(b.text);
        CHECK(!c.ok, b.msg);
        CHECK(c.errorLine > 0, "   行号已记录");
    }
    // 边界：maxFreqK 合法值
    {
        TacticsConf c1 = parse_tactics_conf("[BALANCE]\nmaxFreqK=-1\n");
        CHECK(c1.ok, "D1 maxFreqK=-1 合法");
        TacticsConf c2 = parse_tactics_conf("[BALANCE]\nmaxFreqK=300000\n");
        CHECK(c2.ok, "D2 maxFreqK=300000 合法");
        TacticsConf c3 = parse_tactics_conf("[BALANCE]\ndepth=cached\n");
        CHECK(c3.ok, "D3 depth=cached 合法");
        TacticsConf c4 = parse_tactics_conf("[BALANCE]\ndepth=\n");
        CHECK(c4.ok, "D4 depth 空合法（不干预）");
    }

    // ---------- E. 集成：覆盖真的进入 Decision.tactics ----------
    {
        std::string t = "[POWER_SAVE]\nuagUp=7777\ngpuIdle=42\nmaxKill=7\n";
        TacticsConf c = parse_tactics_conf(t);
        CHECK(c.ok, "E1 集成配置 ok");
        PolicyManager pm;
        pm.set_tactics_override(c.byScenario);
        CHECK(pm.tactics_override_scenarios() == 1, "E2 覆盖表已装载");
        Decision d = pm.decide(st_with("com.tencent.mm", false),
                               mk(EventType::ScreenChanged, "off"), 600000);
        CHECK(d.scenario == Scenario::POWER_SAVE, "E3 熄屏 -> POWER_SAVE");
        CHECK(d.tactics.uagUpRateUs == 7777, "E4 覆盖生效 uagUp");
        CHECK(d.tactics.gpuIdleTimer == 42, "E5 覆盖生效 gpuIdle");
        CHECK(d.tactics.maxKillPerRound == 7, "E6 覆盖生效 maxKill");
        CHECK(d.tactics.freezeEnabled, "E7 未覆盖字段保持内置默认 freeze=1");
    }
    // 未覆盖的场景不受影响（回归 spec §6 验收1）
    {
        std::string t = "[POWER_SAVE]\nuagUp=7777\n";
        TacticsConf c = parse_tactics_conf(t);
        PolicyManager pm;
        pm.set_tactics_override(c.byScenario);
        Decision d = pm.decide(st_with("com.tencent.mm", true),
                               mk(EventType::ConfigChanged, "idle-tick"), 600000);
        CHECK(d.scenario == Scenario::BALANCE, "F1 亮屏 -> BALANCE");
        CHECK(d.tactics.uagUpRateUs == -1, "F2 BALANCE 未被覆盖 -> 默认 -1");
        CHECK(d.tactics.gpuIdleTimer == 80, "F3 BALANCE 默认 gpuIdle=80 保持不变");
    }
    // 无覆盖时行为不变（向后兼容）
    {
        PolicyManager pm;
        Decision d = pm.decide(st_with("com.tencent.mm", false),
                               mk(EventType::ScreenChanged, "off"), 600000);
        CHECK(d.scenario == Scenario::POWER_SAVE, "G1 无覆盖 -> POWER_SAVE");
        CHECK(d.tactics.uagUpRateUs == 2500, "G2 内置默认 uagUp=2500 保留");
    }
    // GAME 让权语义不被配置破坏（可覆盖但默认必须保持 handover）
    {
        PolicyManager pm;
        Decision d = pm.decide(st_with("com.tencent.mm", true),
                               mk(EventType::ConfigChanged, "idle-tick"), 600000);
        CHECK(!d.tactics.handover, "H1 非 GAME 无 handover");
    }

    std::printf("结果: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
