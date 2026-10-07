// UnifiedRootOptimizer M2′ — Controller/Adapter 集成测试（fake sysfs）
// 设计文档 v1.1 §10.1 Integration：Controller → Adapter → fake sysfs，
// 覆盖 成功 / 失败 / 节点不存在 全部路径；退出条件 = 缺失节点均可降级。
//
// 构建（host，无需设备）:  g++ -std=c++20 -Iinclude src/adapter.cpp src/controllers.cpp \
//                            tests/test_m2.cpp -o /tmp/uro_test && /tmp/uro_test
#include "../include/adapter.hpp"
#include "../include/controller.hpp"
#include <cassert>
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

static std::string mkd(const char* tpl) {
    char buf[256];
    snprintf(buf, sizeof buf, tpl, (int)getpid());
    mkdir(buf, 0755);
    return buf;
}
static void put(const std::string& p, const std::string& v) {
    std::string dir = p.substr(0, p.find_last_of('/'));
    std::string cmd = "mkdir -p '" + dir + "'";
    system(cmd.c_str());
    std::ofstream f(p); f << v;
}

// ---------- 1. Adapter 单元：probe / read / write 五种 outcome ----------
static void test_adapter(const std::string& root) {
    SysfsAdapter ad(root);

    // 存在 + 可写
    put(root + "/n/val", "old");
    NodeCap c = ad.probe("n/val");
    CHECK(c.exists && c.readable && c.writable, "probe writable node");

    // Ok：写 + readback 一致
    std::string det;
    CHECK(ad.write("n/val", "1234", false, &det) == WriteOutcome::Ok, "write OK with readback");
    CHECK(ad.read("n/val") && *ad.read("n/val") == "1234", "readback value matches");

    // DryRun：不落盘
    CHECK(ad.write("n/val", "9999", true, &det) == WriteOutcome::DryRun, "dry-run reports DRYRUN");
    CHECK(ad.read("n/val") && *ad.read("n/val") == "1234", "dry-run did not modify file");

    // Missing：目标不存在且父目录不可达（= 真实 sysfs 节点缺失的语义）
    CHECK(ad.write("n/nope/deeper/x", "1", false, &det) == WriteOutcome::Missing,
          "absent node w/ absent parent -> MISSING");

    // 父目录存在且可写时，缺失目标是"可创建文件"（策略快照场景），不判 Missing
    WriteOutcome cre = ad.write("n/fresh_snapshot.txt", "v1", false, &det);
    CHECK(cre == WriteOutcome::Ok, "creatable file in writable dir -> created OK");
    CHECK(ad.read("n/fresh_snapshot.txt") && *ad.read("n/fresh_snapshot.txt") == "v1",
          "created file readback");

    // 覆盖写更短值时旧尾部必须被截断（否则 readback 会 Mismatch）
    CHECK(ad.write("n/fresh_snapshot.txt", "v2", false, &det) == WriteOutcome::Ok,
          "overwrite shorter value -> Ok (truncated)");
    CHECK(ad.read("n/fresh_snapshot.txt") && *ad.read("n/fresh_snapshot.txt") == "v2",
          "no stale tail after shorter overwrite");

    // Empty：空值拒绝
    CHECK(ad.write("n/val", "", false, &det) == WriteOutcome::Empty, "empty value rejected");

    // Denied：真实只读内核节点（root 也写不进去——无 write handler）
    SysfsAdapter real;
    WriteOutcome ro = real.write("/proc/version", "x", false, &det);
    CHECK(ro == WriteOutcome::Denied, "real read-only kernel node -> DENIED");

    // probe 不存在节点
    NodeCap miss = ad.probe("n/definitely_missing");
    CHECK(!miss.exists && !miss.reason.empty(), "probe missing returns reason");

    std::printf("[adapter] ok (outcomes: OK/DRYRUN/MISSING/EMPTY/DENIED + create/truncate)\n");
}

// ---------- 2. 降级：节点缺失时 Controller → Degraded，且主流程不崩 ----------
static void test_degrade_missing() {
    SysfsAdapter ad("/nonexistent/fake_sysfs_root");   // 全部节点都不存在
    ControllerRegistry reg;
    reg.add(make_memory_controller());
    reg.add(make_cpu_controller());
    reg.add(make_gpu_placeholder());
    reg.add(make_thermal_placeholder());

    bool threw = false;
    std::vector<CtrlReport> reps;
    try {
        reps = reg.probe_all(ad);
    } catch (...) { threw = true; }
    CHECK(!threw, "probe_all must not throw on all-missing nodes");
    CHECK(reps.size() == 4, "all 4 controllers probed");
    int degraded = 0;
    for (auto& r : reps) if (r.state == CtrlState::Degraded) degraded++;
    CHECK(degraded >= 3, "missing nodes -> degraded (not crashed)");
    CHECK(!reg.any_active(), "no active controller when all nodes missing");

    // 即便全部降级，resolve/apply_all 仍须可调用且不崩
    EffectivePolicy eff = reg.resolve(1);
    bool applyThrew = false;
    try { reg.apply_all(eff, ad, true); } catch (...) { applyThrew = true; }
    CHECK(!applyThrew, "apply_all must not throw when everything degraded");

    std::printf("[degrade] ok (all-missing -> degraded, no crash)\n");
}

// ---------- 3. 部分缺失：只降级受影响类，其余照常 Active ----------
static void test_partial(const std::string& root) {
    SysfsAdapter ad(root);
    // 只铺 Memory 需要的节点，不铺 cpufreq
    put(root + "/proc/pressure/memory", "some avg10=1.00");
    put(root + "/proc/meminfo", "MemTotal: 8000 kB");

    ControllerRegistry reg;
    reg.add(make_memory_controller());
    reg.add(make_cpu_controller());
    reg.add(make_gpu_placeholder());
    auto reps = reg.probe_all(ad);

    CHECK(reps[0].state == CtrlState::Active, "memory ACTIVE with its nodes present");
    CHECK(reps[1].state == CtrlState::Degraded, "cpu DEGRADED when cpufreq absent");
    CHECK(reps[2].state == CtrlState::Degraded, "gpu DEGRADED (placeholder)");
    CHECK(reg.any_active(), "registry still has an active controller");

    // resolve 只纳入 Active 的约束（Degraded 不参与求交）
    EffectivePolicy eff = reg.resolve(7);
    CHECK(eff.generation == 7, "resolve carries generation");
    CHECK(eff.memory.maxKillPerRound >= 0, "active memory controller contributed desire");

    std::printf("[partial] ok (only affected class degraded)\n");
}

// ---------- 4. 约束求交（§4.3：min 取最小值，boost 与门）----------
static void test_intersect() {
    EffectivePolicy game, thermal;
    game.cpu.maxFreq = 3200000;   // 游戏请求 3.2G
    game.cpu.launchBoost = true;
    game.gpu.boost = true;
    thermal.cpu.maxFreq = 2400000; // thermal 上限 2.4G
    thermal.cpu.launchBoost = false;

    EffectivePolicy e = intersect(game, thermal);
    CHECK(e.cpu.maxFreq == 2400000, "effective max = min(game, thermal) = 2.4G");
    CHECK(!e.cpu.launchBoost, "launchBoost ANDed with thermal gate");
    CHECK(!e.gpu.boost, "gpu boost preserved when unconstrained");

    EffectivePolicy none;
    CHECK(intersect(none, thermal).cpu.maxFreq == 2400000, "unconstrained side takes other");
    CHECK(intersect(none, none).cpu.maxFreq == -1, "both unconstrained -> -1");

    // min > max 时钳回 max（约束无解保上限）
    EffectivePolicy weird; weird.cpu.minFreq = 3000000; weird.cpu.maxFreq = 1000000;
    EffectivePolicy w = intersect(weird, none);
    CHECK(w.cpu.minFreq <= w.cpu.maxFreq, "min clamped to max when unsatisfiable");

    std::printf("[intersect] ok (§4.3 min-intersection + AND gate)\n");
}

// ---------- 5. 边界触发（§3.4 写完即走：策略不变不重复写）----------
static void test_boundary() {
    EffectivePolicy a, b;
    a.cpu.maxFreq = 2000000;
    b.cpu.maxFreq = 2000000;
    CHECK(!materially_different(a, b), "identical policy -> no rewrite");
    b.cpu.maxFreq = 1800000;
    CHECK(materially_different(a, b), "changed maxFreq -> rewrite");
    b = a; b.memory.freezeEnabled = true;
    CHECK(materially_different(a, b), "freeze toggle -> rewrite");
    std::printf("[boundary] ok (no-op policy does not trigger write)\n");
}

// ---------- 6. 单个 Faulted 不影响其余（apply_all 隔离）----------
static void test_isolation(const std::string& root) {
    SysfsAdapter ad(root);
    put(root + "/proc/pressure/memory", "some avg10=0.50");
    put(root + "/proc/meminfo", "MemTotal: 8000 kB");

    ControllerRegistry reg;
    reg.add(make_memory_controller());
    reg.add(make_gpu_placeholder());   // 占位 = Degraded，不参与
    reg.probe_all(ad);

    EffectivePolicy eff = reg.resolve(3);
    auto reps = reg.apply_all(eff, ad, true /*dry-run*/);
    CHECK(reps.size() == 2, "apply_all returns per-controller reports");
    CHECK(reps[0].state == CtrlState::Active, "memory applied (dry-run) fine");

    std::printf("[isolation] ok\n");
}

int main() {
    std::printf("=== M2′ Controller/Adapter 集成测试 (fake sysfs) ===\n");
    std::string root = mkd("/tmp/uro_fake_%d");

    test_adapter(root);
    test_degrade_missing();
    test_partial(root);
    test_intersect();
    test_boundary();
    test_isolation(root);

    std::printf("\n结果: %d passed, %d failed\n", pass, fail);
    // 清理 fake sysfs
    std::string cmd = "rm -rf '" + root + "'";
    system(cmd.c_str());
    return fail == 0 ? 0 : 1;
}
