// UnifiedRootOptimizer v0.33 — 参数外置（场景配方覆盖）测试
// 覆盖 docs/参数外置_v0.33_spec.md §6 验收 1-3（host，无需设备）
// 构建: g++ -std=c++20 -Iinclude src/adapter.cpp src/controllers.cpp src/appopt.cpp src/evidence.cpp tests/test_tactics.cpp -o /tmp/t
#include "../include/tactics_conf.hpp"
#include "../include/policy_manager.hpp"
#include "../include/controller.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;

static void wfile(const std::string& p, const std::string& v) { std::ofstream f(p); f << v; }
static std::string rfile(const std::string& p) {
    std::ifstream f(p); std::string s; std::getline(f, s); return s;
}

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

    // ---------- I. GPU 边界接管（v0.33 第3批：fake sysfs）----------
    {
        std::string root = "/tmp/uro_gpu_" + std::to_string((int)getpid());
        std::string base = root + "/sys/class/kgsl/kgsl-3d0";
        fs::remove_all(root);
        fs::create_directories(base);
        wfile(base + "/idle_timer", "80");
        wfile(base + "/min_pwrlevel", "7");
        wfile(base + "/max_pwrlevel", "0");
        wfile(base + "/bus_split", "1");

        SysfsAdapter ad(root);
        auto gpu = make_gpu_placeholder();
        CtrlReport r = gpu->probe(ad);
        CHECK(r.state == CtrlState::Active, "I1 kgsl 四节点齐 -> Active");

        // 写入边界：游戏档抬保底(min 7->5)、省电档压上限(max 0->4)
        EffectivePolicy eff;
        eff.gpu.pwrMin = 5; eff.gpu.pwrMax = 4; eff.gpu.busSplit = 0; eff.gpu.idleTimer = 120;
        gpu->apply(eff, ad, false);
        CHECK(rfile(base + "/min_pwrlevel") == "5", "I2 min_pwrlevel 写入 5（抬保底）");
        CHECK(rfile(base + "/max_pwrlevel") == "4", "I3 max_pwrlevel 写入 4（压上限）");
        CHECK(rfile(base + "/bus_split") == "0", "I4 bus_split 写入 0");
        CHECK(rfile(base + "/idle_timer") == "120", "I5 idle_timer 写入 120");

        // 幂等：同值重放不改变内容
        gpu->apply(eff, ad, false);
        CHECK(rfile(base + "/min_pwrlevel") == "5", "I6 同值重放幂等");

        // 回基线：-1 = 归还探测时快照的原值
        EffectivePolicy eff2;
        gpu->apply(eff2, ad, false);
        CHECK(rfile(base + "/min_pwrlevel") == "7", "I7 回基线 min_pwrlevel=7");
        CHECK(rfile(base + "/max_pwrlevel") == "0", "I8 回基线 max_pwrlevel=0");
        CHECK(rfile(base + "/bus_split") == "1", "I9 回基线 bus_split=1");
        CHECK(rfile(base + "/idle_timer") == "120", "I10 idle_timer 不回基线（0=不定语义不变）");

        // dry-run 不写盘
        EffectivePolicy eff3; eff3.gpu.pwrMin = 3;
        gpu->apply(eff3, ad, true);
        CHECK(rfile(base + "/min_pwrlevel") == "7", "I11 dry-run 不写盘");

        fs::remove_all(root);
        SysfsAdapter missing("/nonexistent_fake_root");
        auto gpu2 = make_gpu_placeholder();
        CHECK(gpu2->probe(missing).state == CtrlState::Degraded, "I12 无 kgsl 节点 -> Degraded");
    }

    // ---------- J. v0.33 第6批：配置字段 ----------
    {
        TacticsConf c = parse_tactics_conf(
            "[PERFORMANCE]\ncpuMinK=1200000\nuagSoftK=2400000\ngpuDefPwr=5\ngpuMinBw=50\n"
            "gpuForceClk=1\ngpuForceBus=1\n");
        CHECK(c.ok, "J1 第6批六字段 -> ok");
        CHECK(c.applied == 6, "J2 字段计数=6");
        CHECK(c.byScenario[Scenario::PERFORMANCE]["cpuMinK"] == "1200000", "J3 cpuMinK 值");
        TacticsConf c2 = parse_tactics_conf("[PERFORMANCE]\ncpuMinK=100000\n");
        CHECK(!c2.ok, "J4 cpuMinK 越界(<300000) 被拒");
        TacticsConf c3 = parse_tactics_conf("[PERFORMANCE]\ngpuForceClk=5\n");
        CHECK(!c3.ok, "J5 gpuForceClk 越界(须0/1) 被拒");
        TacticsConf c4 = parse_tactics_conf("[PERFORMANCE]\nuagSoftK=2803200\n");
        CHECK(c4.ok, "J6 uagSoftK 合法");
        TacticsConf c5 = parse_tactics_conf("[PERFORMANCE]\ngpuMinBw=-1\n");
        CHECK(c5.ok, "J7 gpuMinBw=-1 合法（回基线）");
    }

    // ---------- K. CPU 控制器：min_freq + uag soft_limit（fake sysfs）----------
    {
        std::string root = "/tmp/uro_cpu6_" + std::to_string((int)getpid());
        std::string base = root + "/sys/devices/system/cpu/cpufreq";
        fs::remove_all(root);
        for (const char* pol : {"policy0", "policy3", "policy7"}) {
            fs::create_directories(std::string(base) + "/" + pol + "/uag");
            std::string p = std::string(base) + "/" + pol;
            wfile(p + "/scaling_min_freq", "672000");
            wfile(p + "/scaling_max_freq", "2016000");
            wfile(p + "/scaling_governor", "uag");
            wfile(p + "/uag/soft_limit_freq", "2016000");
        }
        SysfsAdapter ad(root);
        auto cpu = make_cpu_controller("");
        CHECK(cpu->probe(ad).state == CtrlState::Active, "K1 CPU probe -> Active");

        EffectivePolicy eff;
        eff.cpu.minFreq = 1200000;
        eff.cpu.uagSoftLimitK = 1500000;
        cpu->apply(eff, ad, false);
        CHECK(rfile(base + "/policy0/scaling_min_freq") == "1200000", "K2 min_freq 写入");
        CHECK(rfile(base + "/policy3/scaling_min_freq") == "1200000", "K3 min_freq 三簇同写");
        CHECK(rfile(base + "/policy7/scaling_min_freq") == "1200000", "K4 min_freq policy7");
        CHECK(rfile(base + "/policy0/uag/soft_limit_freq") == "1500000", "K5 soft_limit 写入");
        cpu->apply(eff, ad, false);
        CHECK(rfile(base + "/policy0/scaling_min_freq") == "1200000", "K6 同值幂等");

        EffectivePolicy eff2;   // 全 -1 → 回基线
        cpu->apply(eff2, ad, false);
        CHECK(rfile(base + "/policy0/scaling_min_freq") == "672000", "K7 min_freq 回基线");
        CHECK(rfile(base + "/policy0/uag/soft_limit_freq") == "2016000", "K8 soft_limit 回基线");
        CHECK(rfile(base + "/policy7/uag/soft_limit_freq") == "2016000", "K9 soft_limit policy7 回基线");

        // v0.35 per-cluster 上限：三簇独立（对照组 B 的映射值）
        EffectivePolicy eff3;
        eff3.cpu.maxFreq  = 1228800;
        eff3.cpu.maxFreq3 = 1401600;
        eff3.cpu.maxFreq7 = 1708800;
        cpu->apply(eff3, ad, false);
        CHECK(rfile(base + "/policy0/scaling_max_freq") == "1228800", "K10 per-cluster policy0");
        CHECK(rfile(base + "/policy3/scaling_max_freq") == "1401600", "K11 per-cluster policy3");
        CHECK(rfile(base + "/policy7/scaling_max_freq") == "1708800", "K12 per-cluster policy7");
        cpu->apply(eff3, ad, false);
        CHECK(rfile(base + "/policy3/scaling_max_freq") == "1401600", "K13 同值幂等");
        EffectivePolicy eff4;
        cpu->apply(eff4, ad, false);
        CHECK(rfile(base + "/policy0/scaling_max_freq") == "2016000", "K14 回基线 policy0");
        CHECK(rfile(base + "/policy3/scaling_max_freq") == "2016000", "K15 回基线 policy3");
        CHECK(rfile(base + "/policy7/scaling_max_freq") == "2016000", "K16 回基线 policy7");
        fs::remove_all(root);
    }

    // ---------- L. GPU 第6批节点（fake sysfs）----------
    {
        std::string root = "/tmp/uro_gpu6_" + std::to_string((int)getpid());
        std::string base = root + "/sys/class/kgsl/kgsl-3d0";
        fs::remove_all(root);
        fs::create_directories(base);
        wfile(base + "/idle_timer", "80");
        wfile(base + "/default_pwrlevel", "7");
        wfile(base + "/minbw_timer", "10");
        wfile(base + "/force_clk_on", "0");
        wfile(base + "/force_bus_on", "0");
        SysfsAdapter ad(root);
        auto gpu = make_gpu_placeholder();
        CHECK(gpu->probe(ad).state == CtrlState::Active, "L1 GPU probe -> Active");

        EffectivePolicy eff;
        eff.gpu.defPwr = 5; eff.gpu.minBw = 50; eff.gpu.forceClk = 1; eff.gpu.forceBus = 1;
        gpu->apply(eff, ad, false);
        CHECK(rfile(base + "/default_pwrlevel") == "5", "L2 default_pwrlevel 写入");
        CHECK(rfile(base + "/minbw_timer") == "50", "L3 minbw_timer 写入");
        CHECK(rfile(base + "/force_clk_on") == "1", "L4 force_clk_on 写入");
        CHECK(rfile(base + "/force_bus_on") == "1", "L5 force_bus_on 写入");

        EffectivePolicy eff2;
        gpu->apply(eff2, ad, false);
        CHECK(rfile(base + "/default_pwrlevel") == "7", "L6 回基线 default_pwrlevel");
        CHECK(rfile(base + "/minbw_timer") == "10", "L7 回基线 minbw_timer");
        CHECK(rfile(base + "/force_clk_on") == "0", "L8 回基线 force_clk_on");
        CHECK(rfile(base + "/force_bus_on") == "0", "L9 回基线 force_bus_on");
        fs::remove_all(root);
    }

    // ---------- M. v0.34 手动档位（WebUI 切换）----------
    {
        PolicyManager pm;
        Decision d = pm.decide(st_with("com.tencent.mm", true),
                               mk(EventType::ConfigChanged, "tick"), 600000);
        CHECK(d.scenario == Scenario::BALANCE, "M1 AUTO 时正常自动判定");

        pm.set_manual_scenario(Scenario::PERFORMANCE);
        d = pm.decide(st_with("com.tencent.mm", false),
                      mk(EventType::ScreenChanged, "off"), 600000);
        CHECK(d.scenario == Scenario::PERFORMANCE, "M2 手动强制 PERFORMANCE（熄屏不回落）");
        CHECK(d.why.find("manual") != std::string::npos, "M3 why 标注 manual");

        pm.set_manual_scenario(Scenario::FAST);
        d = pm.decide(st_with("com.tencent.mm", true),
                      mk(EventType::ConfigChanged, "tick"), 610000);
        CHECK(d.scenario == Scenario::FAST, "M4 手动切 FAST");

        pm.set_manual_scenario(std::nullopt);
        d = pm.decide(st_with("com.tencent.mm", false),
                      mk(EventType::ConfigChanged, "tick"), 700000);
        CHECK(d.scenario == Scenario::POWER_SAVE, "M5 回 AUTO -> 熄屏 POWER_SAVE");
    }
    // GAME 让权优先于手动档（§5.5 铁律）
    {
        std::string dir = "/tmp/uro_manual_" + std::to_string((int)getpid());
        fs::create_directories(dir);
        std::string list = dir + "/game_apps.txt";
        { std::ofstream f(list); f << "com.tencent.tmgp.sgame\n"; }
        PolicyManager pm;
        pm.set_game_list_path(list);
        pm.set_manual_scenario(Scenario::PERFORMANCE);
        Decision d = pm.decide(st_with("com.tencent.tmgp.sgame", true),
                               mk(EventType::ForegroundChanged, "com.tencent.tmgp.sgame"), 600000);
        CHECK(d.scenario == Scenario::GAME, "M6 GAME 让权优先于手动档");
        CHECK(d.tactics.handover, "M7 GAME -> handover 保持");
        fs::remove_all(dir);
    }

    std::printf("结果: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
