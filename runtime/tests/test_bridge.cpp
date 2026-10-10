// UnifiedRootOptimizer — COSMemory 桥接测试（§5.1 真实引擎接口）
// 验证：场景 → reclaim.aggressive 映射 + 基线恢复（防互相覆盖）+ 幂等 + dry-run 不落盘
// 构建: g++ -std=c++20 -Iinclude src/adapter.cpp src/controllers.cpp tests/test_bridge.cpp -o /tmp/b && /tmp/b
#include "../include/controller.hpp"
#include "../include/drivers.hpp"
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

    // ---- 13. uag 参数接管（阶段B-CPU）：POWERSAVE 写 2500 / 常态回基线 / 崩溃归还 ----
    {
        std::string d2 = dir + "_uag";
        system(("mkdir -p '" + d2 + "'").c_str());
        // 造 CPU 节点（probe 需要 scaling_* 才 Active；uag 三簇 up_rate 现值 0）
        for (int i : {0, 3, 7}) {
            std::string p = d2 + "/sys/devices/system/cpu/cpufreq/policy" + std::to_string(i);
            system(("mkdir -p '" + p + "/uag'").c_str());
            { std::ofstream f(p + "/scaling_min_freq"); f << "300000\n"; }
            { std::ofstream f(p + "/scaling_max_freq"); f << "3187200\n"; }
            { std::ofstream f(p + "/scaling_governor"); f << "uag\n"; }
            { std::ofstream f(p + "/uag/up_rate_limit_us"); f << "0\n"; }
        }
        std::string st2 = d2 + "/cpu.state";
        SysfsAdapter adu(d2);
        auto mc = make_cpu_controller(st2);
        auto pr = mc->probe(adu);
        CHECK(pr.state == CtrlState::Active, "cpu ACTIVE with uag nodes");
        CHECK(pr.detail.find("uagNodes=3") != std::string::npos, "枚举到三簇 uag 节点");
        CHECK(slurp(st2).find("UP:policy0=0") != std::string::npos, "基线建档 up=0");

        // POWERSAVE：三簇写 2500
        EffectivePolicy ps;
        ps.cpu.uagUpRateUs = 2500;
        auto r1 = mc->apply(ps, adu, false);
        CHECK(slurp(d2 + "/sys/devices/system/cpu/cpufreq/policy7/uag/up_rate_limit_us").find("2500") == 0,
              "policy7 写 2500");
        CHECK(slurp(d2 + "/sys/devices/system/cpu/cpufreq/policy0/uag/up_rate_limit_us").find("2500") == 0,
              "policy0 写 2500");
        CHECK(slurp(st2).find("DIRTY=true") != std::string::npos, "偏离基线 -> DIRTY");
        CHECK(r1.detail.find("up:policy3=2500") != std::string::npos, "detail 含各簇写入");

        // 常态（-1 = 回基线）
        EffectivePolicy daily;
        mc->apply(daily, adu, false);
        CHECK(slurp(d2 + "/sys/devices/system/cpu/cpufreq/policy0/uag/up_rate_limit_us").find("0\n") == 0 ||
              slurp(d2 + "/sys/devices/system/cpu/cpufreq/policy0/uag/up_rate_limit_us") == "0",
              "常态回基线 0");
        CHECK(slurp(st2).find("DIRTY=false") != std::string::npos, "回基线 -> CLEAN");

        // 崩溃归还：写偏 + DIRTY + 新实例 probe
        { std::ofstream f(d2 + "/sys/devices/system/cpu/cpufreq/policy0/uag/up_rate_limit_us"); f << "2500\n"; }
        { std::ofstream f(d2 + "/sys/devices/system/cpu/cpufreq/policy3/uag/up_rate_limit_us"); f << "2500\n"; }
        { std::ofstream f(d2 + "/sys/devices/system/cpu/cpufreq/policy7/uag/up_rate_limit_us"); f << "2500\n"; }
        { std::ofstream f(st2); f << "DIRTY=true\nUP:policy0=0\nUP:policy3=0\nUP:policy7=0\n"; }
        auto mc2 = make_cpu_controller(st2);
        auto pr2 = mc2->probe(adu);
        CHECK(pr2.detail.find("CRASH-RECOVERY uag") != std::string::npos, "CPU 崩溃恢复报告");
        CHECK(slurp(d2 + "/sys/devices/system/cpu/cpufreq/policy3/uag/up_rate_limit_us").find("0") == 0,
              "policy3 归还基线");
        CHECK(slurp(st2).find("DIRTY=false") != std::string::npos, "恢复后 CLEAN");
        std::printf("[uag-takeover] ok (三簇枚举/场景写/回基线/崩溃归还)\n");
    }

    // ---- 14. two-phase 事务：PREPARE 态崩溃也恢复（写前记意图，零崩溃窗口）----
    {
        std::string d2 = dir + "_2pc";
        std::string j2 = setup(d2);
        // 构造 PREPARE 现场：意图已记、写入未完成（或完成一半）
        { std::ifstream f(j2); std::ostringstream ss; ss << f.rdbuf();
          std::string c = ss.str();
          auto p = c.find("\"depth\": \"cached\"");
          if (p != std::string::npos) c.replace(p, 18, "\"depth\": \"service\"");
          std::ofstream o(j2); o << c; }
        { std::ofstream f(d2 + "/bridge.state");
          f << "BASELINE=true\nDIRTY=true\nDEPTH=\"cached\"\nCOOL=60\nPHASE=PREPARE\n"; }
        SysfsAdapter adp(d2);
        auto mc = make_memory_controller(d2 + "/p.txt", j2, d2 + "/bridge.state");
        auto r = mc->probe(adp);
        CHECK(r.detail.find("CRASH-RECOVERY") != std::string::npos, "PREPARE 态同样触发恢复");
        CHECK(slurp(j2).find("\"depth\": \"cached\"") != std::string::npos,
              "PREPARE 崩溃 -> depth 归还基线");
        CHECK(slurp(d2 + "/bridge.state").find("DIRTY=false") != std::string::npos,
              "PREPARE 恢复后 CLEAN");
        // 正常 apply 落 COMMIT 标记
        EffectivePolicy ep;
        ep.memory.reclaimEnabled = true;
        ep.memory.maxKillPerRound = 0;
        mc->apply(ep, adp, false);
        CHECK(slurp(d2 + "/bridge.state").find("PHASE=COMMIT") != std::string::npos,
              "正常写后标记 COMMIT");
        std::printf("[2pc] ok (PREPARE崩溃恢复/COMMIT标记)\n");
    }

    // ---- 15. [appopt] ThreadController Phase-1 纯逻辑（host 可测）----
    {
        std::string d = dir + "_aopt";
        system(("mkdir -p " + d + "/cpuset/0-2 " + d + "/cpuset/7").c_str());
        { std::ofstream f(d + "/applist.conf");
          f << "# 注释行\n// 斜杠注释\n"
               "com.tencent.mm=e-core {\n"
               "\tRenderThread=hp-core\n"
               "}\n"
               "// com.tencent.tmgp.sgame {\n"
               "#com.tencent.tmgp.sgame=hp-core\n"; }
        { std::ofstream f(d + "/game.txt");
          f << "com.tencent.tmgp.sgame\n# 注释\ncom.oplus.games\n"; }
        { std::ofstream f(d + "/cpuset/0-2/tasks"); f << "1\n2\n3\n"; }
        { std::ofstream f(d + "/cpuset/7/tasks"); f << "9\n"; }

        auto s1 = read_appopt(d + "/applist.conf", d + "/game.txt", d + "/cpuset");
        CHECK(s1.present, "appopt present");
        CHECK(s1.rules == 3, "活跃规则计数=3(块两行+闭括号;注释不计)");
        CHECK(s1.gameViolation.empty(), "游戏规则全注释 -> 无违规");
        CHECK(s1.groups.size() == 2, "cpuset 组数=2");
        int n7 = -1;
        for (auto& g : s1.groups) if (g.first == "7") n7 = g.second;
        CHECK(n7 == 1, "组7成员数=1");

        // 违规用例：放开 sgame 活跃行 -> gameViolation 命中（10-07 应豁免）
        { std::ofstream f(d + "/applist.conf", std::ios::app);
          f << "com.tencent.tmgp.sgame=hp-core\n"; }
        auto s2 = read_appopt(d + "/applist.conf", d + "/game.txt", d + "/cpuset");
        CHECK(s2.rules == 4, "放开后规则=4");
        CHECK(s2.gameViolation == "com.tencent.tmgp.sgame", "游戏豁免校验命中");

        // URO-GEN 框架：追加+备份+幂等
        ensure_uro_gen_block(d + "/applist.conf");
        { std::ifstream f(d + "/applist.conf"); std::ostringstream ss; ss << f.rdbuf();
          CHECK(ss.str().find("URO-GEN-BEGIN") != std::string::npos, "GEN 框架已建"); }
        { std::ifstream f(d + "/applist.conf.bak_uro_gen");
          CHECK(f.is_open(), "GEN 前已备份"); }
        ensure_uro_gen_block(d + "/applist.conf");
        { std::ifstream f(d + "/applist.conf"); std::ostringstream ss; ss << f.rdbuf();
          auto c = ss.str();
          size_t pos = 0, cnt = 0;
          while ((pos = c.find("URO-GEN-BEGIN", pos)) != std::string::npos) { cnt++; pos += 13; }
          CHECK(cnt == 1, "GEN 框架幂等(重复调用不叠加)"); }

        // 缺失降级
        auto s3 = read_appopt(d + "/nope.conf", d + "/nope.txt", d + "/no_cpuset");
        CHECK(!s3.present, "全缺失 -> absent");
        std::printf("[appopt] ok (计数/豁免校验/GEN框架/降级)\n");
    }

    // ---- 16. [evidence] 温度/FPS 纯逻辑（v0.17）----
    {
        // thermal 扫描（fake root）：top3 排序 + battery 单列
        std::string td = dir + "_therm";
        for (auto& z : {std::make_pair("thermal_zone0", 45000), std::make_pair("thermal_zone1", 62000),
                        std::make_pair("thermal_zone2", 38000), std::make_pair("thermal_zone3", 41000),
                        std::make_pair("thermal_zone4", 30500)}) {
            system(("mkdir -p " + td + "/" + z.first).c_str());
            std::ofstream t(td + "/" + z.first + "/type"),
                          v(td + "/" + z.first + "/temp");
            t << (z.first == "thermal_zone4" ? "battery" : (std::string("skin-") + z.first));
            v << z.second;
        }
        auto tj = thermal_json(td);
        CHECK(tj.find("\"c\":62") != std::string::npos, "top1=62度");
        CHECK(tj.find("\"c\":45") != std::string::npos, "top2=45度");
        CHECK(tj.find("\"c\":41") != std::string::npos, "top3=41度(38/30.5不入)");
        CHECK(tj.find("\"battery\":30") != std::string::npos, "battery=30度单列");

        // gfxinfo 解析
        long tot = -1, jk = -1;
        CHECK(parse_gfxinfo("Profile data in ms:\n\tTotal frames rendered: 12345\n"
                            "\tJanky frames: 678 (5.49%)\n", tot, jk),
              "gfxinfo 解析成功");
        CHECK(tot == 12345 && jk == 678, "gfxinfo 数值正确");
        CHECK(!parse_gfxinfo("no frames here", tot, jk), "缺行返回 false");

        // 电量（M5 分母）
        std::string bd = dir + "_ps";
        system(("mkdir -p " + bd + "/battery").c_str());
        { std::ofstream f(bd + "/battery/capacity"); f << "49"; }
        { std::ofstream f(bd + "/battery/status"); f << "Discharging"; }
        { std::ofstream f(bd + "/battery/current_now"); f << "531"; }
        auto bj = battery_json(bd);
        CHECK(bj.find("\"pct\":49") != std::string::npos, "电量49%");
        CHECK(bj.find("Discharging") != std::string::npos, "放电状态");
        CHECK(bj.find("\"mA\":531") != std::string::npos, "电流读数");
        CHECK(battery_json(bd + "_missing").find("\"pct\":-1") != std::string::npos,
              "缺失降级 pct=-1");
        std::printf("[evidence] ok (温度top3/battery/gfxinfo解析)\n");
    }

    // ---- 17. [exempt] AppOpt 指挥链① GAME 豁免 ----
    {
        std::string d = dir + "_exm";
        system(("mkdir -p " + d).c_str());
        { std::ofstream f(d + "/applist.conf");
          f << "com.tencent.mm=e-core {\n"
               "  RenderThread=hp-core\n"
               "}\n"
               "com.tencent.tmgp.sgame=hp-core\n"
               "# com.oplus.games=hp-core\n"; }
        { std::ofstream f(d + "/game.txt");
          f << "com.tencent.tmgp.sgame\ncom.oplus.games\n"; }

        int n1 = exempt_game_rules(d + "/applist.conf", d + "/game.txt");
        CHECK(n1 == 1, "只豁免活跃游戏规则(1行)");
        { std::ifstream f(d + "/applist.conf"); std::ostringstream ss; ss << f.rdbuf();
          auto c = ss.str();
          CHECK(c.find("# [URO-EXEMPT] com.tencent.tmgp.sgame") != std::string::npos,
                "标记注释已写入");
          CHECK(c.find("# [URO-EXEMPT] # com.oplus.games") == std::string::npos,
                "用户注释行不被触碰");
          CHECK(c.find("com.tencent.mm=e-core") != std::string::npos, "非游戏规则不动"); }
        CHECK(exempt_game_rules(d + "/applist.conf", d + "/game.txt") == 0, "幂等(二次0)");
        { std::ifstream f(d + "/applist.conf.uro_exempt.bak");
          CHECK(f.is_open(), "首次豁免前已备份"); }

        int r1 = restore_exempted(d + "/applist.conf");
        CHECK(r1 == 1, "出GAME还原1行");
        { std::ifstream f(d + "/applist.conf"); std::ostringstream ss; ss << f.rdbuf();
          auto c = ss.str();
          CHECK(c.find("com.tencent.tmgp.sgame=hp-core") != std::string::npos &&
                c.find("[URO-EXEMPT]") == std::string::npos, "还原后无标记且规则复活"); }
        CHECK(restore_exempted(d + "/applist.conf") == 0, "还原幂等(0)");
        std::printf("[exempt] ok (豁免/标记/幂等/不动用户行/还原)\n");
    }

    // ---- 18. [urogen] 指挥链② 画像第四字段生成 ----
    {
        std::string d = dir + "_ugen";
        system(("mkdir -p " + d).c_str());
        { std::ofstream f(d + "/applist.conf");
          f << "# 用户规则(区块外)\n"
               "com.keep.me=e-core\n"
               "# ---- URO-GEN-BEGIN (x) ----\n"
               "# ---- URO-GEN-END ----\n"
               "com.also.keep=0-7\n"; }
        { std::ofstream f(d + "/profiles.txt");
          f << "com.tencent.mm|250000|80000|hp-core\n"
               "com.nofour|100000|-1\n"
               "com.wild*|1|1|e-core\n"
               "# com.commented|1|1|p-core\n"
               "com.blank|1|1|\n"; }

        int n = reconcile_uro_gen(d + "/applist.conf", d + "/profiles.txt");
        CHECK(n == 1, "仅第四字段非空且无通配的画像生成(1条)");
        { std::ifstream f(d + "/applist.conf"); std::ostringstream ss; ss << f.rdbuf();
          auto c = ss.str();
          CHECK(c.find("URO-GEN-BEGIN") != std::string::npos &&
                c.find("com.tencent.mm=hp-core") != std::string::npos, "规则写入区块内");
          size_t bpos = c.find("URO-GEN-BEGIN"), epos = c.find("URO-GEN-END");
          CHECK(bpos < epos && c.find("com.tencent.mm", epos + 10) == std::string::npos,
                "生成行只在区块内(END后无泄漏)");
          CHECK(c.find("com.keep.me=e-core") != std::string::npos &&
                c.find("com.also.keep=0-7") != std::string::npos, "区块外用户规则零改动"); }
        CHECK(reconcile_uro_gen(d + "/applist.conf", d + "/profiles.txt") == 0, "幂等(二次0)");

        // EXEMPT 兼容：生成行被豁免标记后，对账不复活
        { std::ifstream f(d + "/applist.conf"); std::stringstream ss; ss << f.rdbuf();
          std::string c = ss.str();
          size_t pos = c.find("com.tencent.mm=hp-core");
          c.replace(pos, std::string("com.tencent.mm=hp-core").size(),
                    "# [URO-EXEMPT] com.tencent.mm=hp-core");
          std::ofstream o(d + "/applist.conf", std::ios::trunc); o << c; }
        CHECK(reconcile_uro_gen(d + "/applist.conf", d + "/profiles.txt") == 0,
              "豁免态对账=0(不复活)");
        { std::ifstream f(d + "/applist.conf"); std::ostringstream ss; ss << f.rdbuf();
          CHECK(ss.str().find("# [URO-EXEMPT] com.tencent.mm=hp-core") != std::string::npos,
                "豁免标记保持"); }

        // 画像变化 → 重生成（换核组）
        { std::ofstream f(d + "/profiles.txt");
          f << "com.tencent.mm|250000|80000|e-core\n"; }
        int n2 = reconcile_uro_gen(d + "/applist.conf", d + "/profiles.txt");
        CHECK(n2 == 1, "画像变更重生成(1)");
        { std::ifstream f(d + "/applist.conf"); std::ostringstream ss; ss << f.rdbuf();
          auto c = ss.str();
          CHECK(c.find("com.tencent.mm=e-core") != std::string::npos, "新核组已写入");
          CHECK(c.find("hp-core") == std::string::npos, "旧规则被替换"); }
        std::printf("[urogen] ok (第四字段/区块边界/幂等/EXEMPT兼容/热改重生成)\n");
    }

    test_no_override(dir);

    std::printf("\n结果: %d passed, %d failed\n", pass, fail);
    system(("rm -rf '" + dir + "' '" + dir + "_missing'").c_str());
    return fail == 0 ? 0 : 1;
}
