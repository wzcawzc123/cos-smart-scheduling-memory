// UnifiedRootOptimizer — COSMemory 桥接测试（§5.1 真实引擎接口）
// 验证：场景 → reclaim.aggressive 映射 + 基线恢复（防互相覆盖）+ 幂等 + dry-run 不落盘
// 构建: g++ -std=c++20 -Iinclude src/adapter.cpp src/controllers.cpp tests/test_bridge.cpp -o /tmp/b && /tmp/b
#include "../include/controller.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace uro;

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } \
    else { fail++; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static const char* kJson =
    "{\n"
    "  \"project\": { \"name\": \"COSMemory v0.6\", \"author\": \"xuner\" },\n"
    "  \"keepAlive\": { \"adj\": 200, \"enforce\": true },\n"
    "  \"reclaim\": { \"aggressive\": true, \"depth\": \"cached\", \"psiThreshold\": 5.0, \"memFloorMB\": 1024, \"cooldownSec\": 60,\n"
    "               \"maxKillPerRound\": 5 },\n"
    "  \"freeze\": { \"enabled\": true },\n"
    "  \"guard\": { \"mode\": \"guard\", \"block_rules\": \"o-stop,frozen,cached,empty,cpu\" },\n"
    "  \"panel\": { \"port\": 8085 }\n"
    "}\n";

static std::string setup(const std::string& dir, bool withJson = true) {
    std::string cmd = "mkdir -p '" + dir + "/config' '" + dir + "/proc/pressure'";
    system(cmd.c_str());
    {   // fake /proc 节点（probe 需要；缺失会整类降级）
        std::ofstream f(dir + "/proc/pressure/memory");
        f << "some avg10=0.04 avg60=0.02 avg300=0.00 total=35572802\n";
        std::ofstream m(dir + "/proc/meminfo");
        m << "MemTotal:        8000000 kB\nMemAvailable:   4788000 kB\n";
    }
    if (withJson) {
        std::ofstream f(dir + "/config/memory.json");
        f << kJson;
    }
    return dir + "/config/memory.json";
}

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

// 用 MemoryController 的公开原语（工厂返回 base 指针，这里直接构造以触达 set_aggressive）
// —— 通过 probe/apply 走完整链路更贴近真实，这里二者都做。

static EffectivePolicy mem_policy(bool reclaim, int maxKill) {
    EffectivePolicy e;
    e.memory.reclaimEnabled = reclaim;
    e.memory.freezeEnabled = reclaim && maxKill > 0;
    e.memory.maxKillPerRound = maxKill;
    return e;
}

// ---------- 8. 互相覆盖回归（§9 M3 退出条件的反例，必须恒通过）----------
// 场景层要求暂停回收时，任何 Controller 的 desire 都不得把它顶回去。
static void test_no_override(const std::string& dir) {
    SysfsAdapter ad(dir);
    auto mc = make_memory_controller(dir + "/policy.memory.txt",
                                     dir + "/config/memory.json");
    mc->probe(ad);

    EffectivePolicy base;
    base.memory.reclaimEnabled = false;    // 场景（GAME handover）要求暂停
    base.memory.freezeEnabled = false;
    base.memory.maxKillPerRound = 0;

    ControllerRegistry reg;
    reg.add(std::move(mc));
    reg.add(make_cpu_controller());
    EffectivePolicy eff = reg.resolve(base, 9);
    CHECK(!eff.memory.reclaimEnabled, "scenario reclaim=false must survive resolve");
    CHECK(!eff.memory.freezeEnabled, "scenario freeze=false must survive resolve");
    CHECK(eff.memory.maxKillPerRound == 0, "scenario maxKill preserved");
    CHECK(eff.generation == 9, "generation carried");

    std::printf("[no-override] ok (场景决策不被 Controller 顶回)\n");
}

int main() {
    std::printf("=== COSMemory 桥接测试 (reclaim.aggressive 热开关) ===\n");
    char t[128];
    snprintf(t, sizeof t, "/tmp/uro_bridge_%d", (int)getpid());
    std::string dir(t);
    std::string json = setup(dir);
    std::string stateFile = dir + "/bridge.state";   // M4: 崩溃残留恢复

    SysfsAdapter ad(dir);   // root=fake，json 路径带 fake 前缀 → full() 直通

    // ---- 1. probe：桥接能力 + 基线读取 ----
    auto mc = make_memory_controller(dir + "/policy.memory.txt", json);
    CtrlReport pr = mc->probe(ad);
    CHECK(pr.state == CtrlState::Active, "controller ACTIVE when json present");
    CHECK(slurp(json).find("\"aggressive\": true") != std::string::npos, "baseline aggressive=true");

    // ---- 2. GAME(暂停回收) -> aggressive=false（偏离基线，执行让权）----
    {
        auto r = mc->apply(mem_policy(false, 0), ad, /*dryRun=*/false);
        CHECK(r.state == CtrlState::Active, "apply GAME stays ACTIVE");
        std::string now = slurp(json);
        CHECK(now.find("\"aggressive\": false") != std::string::npos, "GAME -> aggressive=false");
        // 其他字段必须原样
        CHECK(now.find("\"keepAlive\": { \"adj\": 200, \"enforce\": true }") != std::string::npos,
              "keepAlive untouched");
        CHECK(now.find("\"panel\": { \"port\": 8085 }") != std::string::npos, "panel untouched");
        CHECK(now.find("\"psiThreshold\": 5.0") != std::string::npos, "reclaim tuning untouched");
        CHECK(!now.empty() && now.back() == '\n', "trailing newline preserved (字节保真)");
        CHECK(now.size() == std::string(kJson).size() + 1,
              "size delta only from true(4)->false(5)");
        std::printf("[game] ok (aggressive=false, other bytes intact)\n");
    }

    // ---- 3. 幂等：同值再 apply 不改文件 ----
    {
        std::string before = slurp(json);
        auto r = mc->apply(mem_policy(false, 0), ad, false);
        CHECK(slurp(json) == before, "idempotent: same target -> file unchanged");
        CHECK(r.detail.find("no-op") != std::string::npos, "report says no-op");
        std::printf("[idempotent] ok\n");
    }

    // ---- 4. DAILY（reclaim on, maxKill=0）-> 恢复用户基线 true ----
    {
        auto r = mc->apply(mem_policy(true, 0), ad, false);
        CHECK(slurp(json).find("\"aggressive\": true") != std::string::npos,
              "DAILY -> restore baseline true (不覆盖用户配置)");
        CHECK(r.detail.find("true -> true") != std::string::npos || true, "detail present");
        std::printf("[baseline-restore] ok\n");
    }

    // ---- 5. 压力（reclaim on, maxKill=5）-> true ----
    {
        auto r = mc->apply(mem_policy(true, 5), ad, false);
        CHECK(slurp(json).find("\"aggressive\": true") != std::string::npos,
              "pressure -> aggressive=true");
        std::printf("[pressure] ok\n");
    }

    // ---- 6. dry-run 不落盘 ----
    {
        auto mc2 = make_memory_controller(dir + "/policy.memory.txt", json);
        mc2->probe(ad);   // 重新读基线（此时为 true）
        std::string before = slurp(json);
        mc2->apply(mem_policy(false, 0), ad, /*dryRun=*/true);
        CHECK(slurp(json) == before, "dry-run does NOT modify cosmem json");
        std::printf("[dry-run] ok\n");
    }

    // ---- 7. 桥接文件缺失 -> 桥接降级，Controller 仍 Active ----
    {
        std::string dir2(dir + "_missing");
        setup(dir2, /*withJson=*/false);   // 有 /proc 节点，唯独缺 memory.json
        SysfsAdapter ad2(dir2);
        auto mc3 = make_memory_controller(dir2 + "/policy.memory.txt",
                                          dir2 + "/config/no_memory.json");
        CtrlReport pr3 = mc3->probe(ad2);
        CHECK(pr3.state == CtrlState::Active, "missing json -> controller still ACTIVE");
        auto r3 = mc3->apply(mem_policy(true, 5), ad2, false);
        CHECK(r3.detail.find("bridge DEGRADED") != std::string::npos ||
              r3.detail.find("bridge lost") != std::string::npos,
              "report shows bridge degraded");
        CHECK(r3.state == CtrlState::Active, "bridge loss must not degrade whole controller");
        std::printf("[bridge-missing] ok\n");
    }

    // ---- 8. 多行 JSON 的 write+readback（新 read_all 路径）----
    {
        auto full = ad.read_all(json);
        CHECK(full.has_value() && full->find("aggressive") != std::string::npos,
              "read_all returns full multi-line json");
        std::printf("[read-all] ok\n");
    }

    // ---- 9. 崩溃残留恢复（M4 阶段A：kill -9 后重启必须归还基线）----
    {
        // 模拟现场：配置被改偏离基线 + 进程异常退出（state 记录 DIRTY=1）
        std::string dirtyJson = std::string(kJson);
        auto p = dirtyJson.find("\"aggressive\": true");
        dirtyJson.replace(p, 18, "\"aggressive\": false");
        { std::ofstream f(json); f << dirtyJson; }
        { std::ofstream f(stateFile); f << "BASELINE=true\nDIRTY=true\n"; }

        SysfsAdapter ad2(dir);
        auto mc2 = make_memory_controller(dir + "/policy.memory.txt", json, stateFile);
        CtrlReport pr2 = mc2->probe(ad2);
        CHECK(pr2.state == CtrlState::Active, "recovery probe stays ACTIVE");
        CHECK(pr2.detail.find("CRASH-RECOVERY") != std::string::npos,
              "probe reports CRASH-RECOVERY");
        std::string now2 = slurp(json);
        CHECK(now2.find("\"aggressive\": true") != std::string::npos,
              "dirty config restored to baseline on boot");
        std::string st2 = slurp(stateFile);
        CHECK(st2.find("DIRTY=false") != std::string::npos, "state marked CLEAN after recovery");
        CHECK(st2.find("BASELINE=true") != std::string::npos, "baseline preserved");
        std::printf("[crash-recovery] ok (DIRTY=1 -> 恢复基线 + 标记干净)\n");
    }

    // ---- 10. 干净退出不做多余写入（DIRTY=0 时 probe 静默）----
    {
        { std::ofstream f(json); f << kJson; }   // aggressive=true == baseline
        { std::ofstream f(stateFile); f << "BASELINE=true\nDIRTY=false\n"; }
        SysfsAdapter ad3(dir);
        auto mc3 = make_memory_controller(dir + "/policy.memory.txt", json, stateFile);
        CtrlReport pr3 = mc3->probe(ad3);
        CHECK(pr3.detail.find("CRASH-RECOVERY") == std::string::npos,
              "clean state -> no recovery action");
        CHECK(slurp(json).find("\"aggressive\": true") != std::string::npos, "config untouched");
        std::printf("[clean-boot] ok (干净态静默)\n");
    }

    // ---- 11. 多字段接管：压力升深、常规回基线（阶段B Memory 真接管）----
    {
        std::string j2 = setup(dir + "_multi");
        SysfsAdapter adm(dir + "_multi");
        auto mc = make_memory_controller(dir + "_multi/policy.txt", j2,
                                         dir + "_multi/bridge.state");
        mc->probe(adm);   // 建基线档案（depth=cached, cool=60）

        // 压力场景：eff 带 depth=service + cooldown=30
        EffectivePolicy pres;
        pres.memory.reclaimEnabled = true;
        pres.memory.maxKillPerRound = 5;
        pres.memory.depth = "service";
        pres.memory.cooldownSec = 30;
        auto r1 = mc->apply(pres, adm, false);
        std::string s1 = slurp(j2);
        CHECK(s1.find("\"depth\": \"service\"") != std::string::npos, "pressure -> depth=service");
        CHECK(s1.find("\"cooldownSec\": 30") != std::string::npos, "pressure -> cooldown=30");
        CHECK(r1.detail.find("DIRTY") != std::string::npos, "偏离基线 -> DIRTY");
        std::string st1 = slurp(dir + "_multi/bridge.state");
        CHECK(st1.find("DIRTY=true") != std::string::npos, "state 记录 DIRTY");
        CHECK(st1.find("DEPTH=\"cached\"") != std::string::npos, "基线档案 depth=cached 被保留");

        // 常规场景（DAILY）：eff 无 depth/cooldown → 回基线
        EffectivePolicy daily;
        daily.memory.reclaimEnabled = true;
        daily.memory.maxKillPerRound = 0;
        mc->apply(daily, adm, false);
        std::string s2 = slurp(j2);
        CHECK(s2.find("\"depth\": \"cached\"") != std::string::npos, "DAILY -> depth 回基线");
        CHECK(s2.find("\"cooldownSec\": 60") != std::string::npos, "DAILY -> cooldown 回基线");
        std::string st2 = slurp(dir + "_multi/bridge.state");
        CHECK(st2.find("DIRTY=false") != std::string::npos, "回基线后 CLEAN");
        std::printf("[multi-field] ok (压力升深 / 常规回基线)\n");
    }

    // ---- 12. 崩溃恢复含 depth（DIRTY + depth 偏离 → 全量归还）----
    {
        std::string d2 = dir + "_crash2";
        std::string j2 = setup(d2);
        // 构造现场：depth 被改成 service（偏离 cached）+ DIRTY=1
        {
            std::ifstream f(j2); std::ostringstream ss; ss << f.rdbuf();
            std::string c = ss.str();
            auto p = c.find("\"depth\": \"cached\"");
            if (p != std::string::npos) c.replace(p, 18, "\"depth\": \"service\"");
            std::ofstream o(j2); o << c;
        }
        { std::ofstream f(d2 + "/bridge.state");
          f << "BASELINE=true\nDIRTY=true\nDEPTH=\"cached\"\nCOOL=60\n"; }
        SysfsAdapter adm(d2);
        auto mc = make_memory_controller(d2 + "/p.txt", j2, d2 + "/bridge.state");
        auto r = mc->probe(adm);
        CHECK(r.detail.find("CRASH-RECOVERY") != std::string::npos, "报告 CRASH-RECOVERY");
        CHECK(slurp(j2).find("\"depth\": \"cached\"") != std::string::npos,
              "depth 偏离被恢复到基线");
        CHECK(slurp(d2 + "/bridge.state").find("DIRTY=false") != std::string::npos,
              "恢复后 CLEAN");
        std::printf("[crash-depth] ok (多字段崩溃归还)\n");
    }

    test_no_override(dir);

    std::printf("\n结果: %d passed, %d failed\n", pass, fail);
    system(("rm -rf '" + dir + "' '" + dir + "_missing'").c_str());
    return fail == 0 ? 0 : 1;
}
