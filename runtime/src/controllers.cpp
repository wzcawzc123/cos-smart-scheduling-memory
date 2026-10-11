// UnifiedRootOptimizer M2′ — Memory / CPU Controller 实现 + GPU / Thermal 占位
// 设计文档 v1.1 §5.1 §5.2；退出条件 = 缺失节点均可降级，不导致主进程退出。
#include "drivers.hpp"
#include "controller.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <map>
#include <unistd.h>

namespace uro {

// 共用：把多节点探测结果折叠成 CtrlReport（任一 required 缺失 → Degraded）
static CtrlReport fold(const std::vector<NodeCap>& caps, const char* what) {
    CtrlReport r{CtrlState::Active, ""};
    std::ostringstream oss;
    int bad = 0;
    for (auto& c : caps) {
        if (!c.exists) { oss << c.path << ":missing(" << c.reason << ") "; bad++; }
        else if (!c.readable) { oss << c.path << ":unreadable "; bad++; }
    }
    if (bad) {
        r.state = CtrlState::Degraded;
        r.detail = std::string(what) + " missing " + std::to_string(bad) + "/" +
                   std::to_string(caps.size()) + " nodes — skip this class: " + oss.str();
    }
    return r;
}

// ============================ Memory Controller ============================
// §5.1 基于 COSMemory：KeepAlive/Reclaim/Freeze 保留；快照由 Event Collector 共享。
// M2′ 阶段只落"接口 + 降级"，策略细节归 M3。
class MemoryController final : public Controller {
public:
    // policyFile 为空 → 相对 ad 根（测试 fake sysfs）；否则用注入的绝对路径
    // cmosJson 为空 → 不启用 COSMemory 桥接（能力降级）
    // stateFile: 桥接状态（BASELINE/DIRTY）持久化——崩溃残留恢复的依据（M4 watchdog 配套）
    explicit MemoryController(std::string policyFile = "", std::string cmosJson = "",
                              std::string stateFile = "")
        : policyFile_(std::move(policyFile)), cmosJson_(std::move(cmosJson)),
          stateFile_(std::move(stateFile)) {}

    const char* name() const override { return "memory"; }

    CtrlReport probe(SysfsAdapter& ad) override {
        std::vector<NodeCap> caps{
            ad.probe("/proc/pressure/memory"),   // PSI 双条件之一
            ad.probe("/proc/meminfo"),           // MemAvailable 来源
        };
        r_ = fold(caps, "memory");
        // 写目标：策略文件（供 COSMemory 桥接读取）
        policyPath_ = policyFile_.empty() ? ad.full("policy.memory.txt") : policyFile_;
        if (r_.state == CtrlState::Active) {
            // 只校验目标父目录可写；不创建文件（创建留给真正的 apply）
            auto slash = policyPath_.find_last_of('/');
            std::string parent = (slash == std::string::npos) ? "." : policyPath_.substr(0, slash);
            if (::access(parent.c_str(), W_OK) != 0) {
                r_.state = CtrlState::Degraded;
                r_.detail = "policy target dir not writable: " + parent;
            }
        }
        // ---- COSMemory 桥接能力探测（§5.1）----
        // 桥接文件缺失/不可读 → 桥接能力降级，但主 Controller 仍可 Active（策略快照照写）
        if (!cmosJson_.empty()) {
            NodeCap cj = ad.probe(cmosJson_);
            if (!cj.exists || !cj.readable) {
                bridge_ = false;
                bridgeWhy_ = "cosmem json unavailable: " + cj.path + " " + cj.reason;
            } else {
                bridge_ = true;
                // ---- 崩溃残留一致性检查（M4：异常退出兜底）----
                // DIRTY=1 → 上次改偏离未恢复 → 全量字段写回基线档案（无条件，先于 enforce 判断）
                auto curAgg = read_aggressive(ad);
                auto curDepth = read_reclaim_field(ad, "depth");
                auto curCool = read_reclaim_field(ad, "cooldownSec");
                bool firstRun = !read_state(ad).has_value();
                BridgeState st = read_state(ad).value_or(BridgeState{});
                if (firstRun) {   // 首次运行：以当前配置建立基线档案
                    st.baselineAgg = curAgg.value_or(true);
                    st.dirty = false;
                }
                if (firstRun || st.depth.empty() || st.cool < 0) {
                    // 档案缺字段（含 v0.6.0 旧格式 BASELINE/DIRTY 两行的升级路径）→ 用当前值补齐，
                    // 否则 depth/cooldown 因无基线而静默永不接管。
                    // 注意保留 dirty 标志（补档案与恢复是顺序关系，不能互斥）
                    if (firstRun) { st.baselineAgg = curAgg.value_or(true); st.dirty = false; }
                    if (st.depth.empty()) st.depth = curDepth.value_or("\"cached\"");
                    if (st.cool < 0) {
                        auto cv = curCool.value_or("60");
                        st.cool = cv.empty() ? 60 : atoi(cv.c_str());
                    }
                    mark_state(ad, st);
                }
                if (st.dirty) {
                    std::string rd, rec;
                    WriteOutcome ro = set_aggressive(ad, st.baselineAgg, false, &rd);
                    rec = "CRASH-RECOVERY agg->" + std::string(st.baselineAgg ? "true" : "false");
                    if (!st.depth.empty()) {
                        std::string d2;
                        if (set_reclaim_field(ad, "depth", st.depth, false, &d2) == WriteOutcome::Ok)
                            rec += " depth->" + st.depth;
                    }
                    if (st.cool >= 0) {
                        std::string d2;
                        if (set_reclaim_field(ad, "cooldownSec", std::to_string(st.cool), false, &d2) == WriteOutcome::Ok)
                            rec += " cool->" + std::to_string(st.cool);
                    }
                    r_.detail = rec + " " + outcome_name(ro);
                    st.dirty = false;
                    mark_state(ad, st);
                }
                baselineAgg_ = st.baselineAgg;
                baselineDepth_ = st.depth;
                baselineCool_ = st.cool;
            }
            if (r_.state == CtrlState::Active && !bridge_) {
                r_.detail = (r_.detail.empty() ? "" : r_.detail + "; ") + bridgeWhy_;
                // 不降级整个 Controller——快照写入仍可用，仅桥接能力缺失
            }
        }
        state_ = r_.state;
        return r_;
    }

    void on_event(const Event& e, const GlobalState& st) override {
        if (e.type == EventType::ModeChanged) mode_ = e.payload;
        else if (e.type == EventType::MemoryPressureChanged) pressured_ = true;
        else if (e.type == EventType::ForegroundChanged) pressured_ = false; // 退出压力态
        mode_ = st.mode.empty() ? mode_ : st.mode;
    }

    EffectivePolicy desire() const override {
        // 职责分离（M3）：reclaim/freeze/maxKill 是**场景决策**，由 PolicyManager 经
        // resolve(base) 注入；Controller 若重复表达，会被 intersect 的 OR 合并顶回，
        // 出现"场景要求暂停回收、Controller 要求回收"的互相覆盖（§9 退出条件反例）。
        // 此处只返回空基底，约束维度由 CpuController 等硬件侧提供。
        return EffectivePolicy{};
    }

    CtrlReport apply(const EffectivePolicy& eff, SysfsAdapter& ad, bool dryRun) override {
        std::ostringstream oss;
        oss << "reclaim=" << eff.memory.reclaimEnabled
            << " freeze=" << eff.memory.freezeEnabled
            << " maxKill=" << eff.memory.maxKillPerRound;
        std::string detail;
        // 写策略快照（桥接口）；dry-run 不落盘
        WriteOutcome o = ad.write(policyPath_, oss.str(), dryRun, &detail);
        CtrlReport r{CtrlState::Active, std::string(outcome_name(o)) + ": " + detail};
        if (o == WriteOutcome::Missing || o == WriteOutcome::Denied) {
            r.state = CtrlState::Degraded;
            r.detail = "policy target unavailable — memory class skipped: " + detail;
            state_ = r.state;
            last_ = r.detail;
            return r;
        }

        // ---- COSMemory 桥接（§5.1）：reclaim.aggressive 是引擎每轮热读的开关 ----
        // 语义（防"互相覆盖"，§9 退出条件）：
        //   reclaimEnabled=false            → 强制 false（GAME/BOOT 主动暂停回收）
        //   reclaimEnabled && maxKill>0     → true（压力/省电场景要求激进）
        //   reclaimEnabled && maxKill==0    → 恢复用户基线（常规态不干预面板配置）
        if (bridge_ && !cmosJson_.empty()) {
            bool aggT;
            if (!eff.memory.reclaimEnabled)      aggT = false;
            else if (eff.memory.maxKillPerRound > 0) aggT = true;
            else                                 aggT = baselineAgg_;
            // two-phase 事务：写前先记意图(PREPARE)——此点后任何崩溃都可由
            // probe 读 DIRTY 恢复；旧时序(写后才记)存在"写成功~记账间崩溃=标记丢失"窗口
            if (!dryRun) {
                BridgeState pre = read_state(ad).value_or(BridgeState{});
                pre.baselineAgg = baselineAgg_;
                if (!baselineDepth_.empty()) pre.depth = baselineDepth_;
                if (baselineCool_ >= 0) pre.cool = baselineCool_;
                pre.dirty = true;
                pre.phase = "PREPARE";
                mark_state(ad, pre);
            }
            std::string bdet;
            bool chg = false;
            WriteOutcome bo = set_aggressive(ad, aggT, dryRun, &bdet, &chg);
            r.detail += std::string(" | bridge aggressive->") + (aggT ? "true" : "false") +
                        " " + outcome_name(bo) + " (" + bdet + ")";
            if (bo == WriteOutcome::Missing || bo == WriteOutcome::Denied) {
                bridge_ = false;   // 桥接能力降级，主 Controller 保持 Active
                bridgeWhy_ = "bridge lost: " + bdet;
            } else if (!dryRun) {
                bool offAgg = (aggT != baselineAgg_);
                bool offDepth = false, offCool = false;
                // depth：场景给了就用，没给（空）= 回基线
                if (!baselineDepth_.empty()) {
                    std::string dT = eff.memory.depth.empty() ? baselineDepth_
                                                              : "\"" + eff.memory.depth + "\"";
                    std::string dd;
                    bool dchg = false;
                    auto o = set_reclaim_field(ad, "depth", dT, false, &dd, &dchg);
                    if (o == WriteOutcome::Ok) {
                        r.detail += " | depth " + dd;
                        offDepth = (dT != baselineDepth_);
                    } else { offDepth = true; r.detail += " | depth FAILED(" + dd + ")"; }
                }
                // cooldown：场景给了就用，没给 = 回基线
                if (baselineCool_ >= 0) {
                    std::string cT = eff.memory.cooldownSec >= 0
                                     ? std::to_string(eff.memory.cooldownSec)
                                     : std::to_string(baselineCool_);
                    std::string dd;
                    bool cchg = false;
                    auto o = set_reclaim_field(ad, "cooldownSec", cT, false, &dd, &cchg);
                    if (o == WriteOutcome::Ok) {
                        r.detail += " | cool " + dd;
                        offCool = (cT != std::to_string(baselineCool_));
                    } else { offCool = true; r.detail += " | cool FAILED(" + dd + ")"; }
                }
                BridgeState st = read_state(ad).value_or(BridgeState{});
                st.baselineAgg = baselineAgg_;
                st.depth = baselineDepth_;
                st.cool = baselineCool_;
                st.dirty = offAgg || offDepth || offCool;
                st.phase = "COMMIT";
                mark_state(ad, st);
                if (chg || offDepth || offCool)
                    r.detail += " [state->" + std::string(st.dirty ? "DIRTY" : "CLEAN") + "]";
            }
        } else if (!cmosJson_.empty()) {
            r.detail += " | bridge DEGRADED (" + bridgeWhy_ + ")";
        }

        state_ = r.state;
        last_ = r.detail;
        return r;
    }

    CtrlState state() const override { return state_; }
    std::string status() const override {
        return std::string("memory ") + ctrl_state_name(state_) +
               " mode=" + mode_ + " " + last_;
    }

    // ---- 桥接原语（可被测试直接驱动）----
    std::optional<bool> read_aggressive(SysfsAdapter& ad) const {
        if (cmosJson_.empty()) return std::nullopt;
        auto txt = ad.read_all(cmosJson_);
        if (!txt) return std::nullopt;
        auto p = txt->find("\"aggressive\"");
        if (p == std::string::npos) return std::nullopt;
        auto colon = txt->find(':', p);
        if (colon == std::string::npos) return std::nullopt;
        auto t = txt->find("true", colon);
        auto f = txt->find("false", colon);
        if (t != std::string::npos && (f == std::string::npos || t < f)) return true;
        if (f != std::string::npos) return false;
        return std::nullopt;
    }

    // ---- 桥接状态持久化（多字段基线档案 + DIRTY）----
    // DIRTY=1 表示"任一字段被我方改偏离基线且未恢复"；崩溃后由 probe 读到并全量恢复。
    struct BridgeState {
        bool baselineAgg = true;
        bool dirty = false;
        std::string depth;   // 空 = 无档案（首次）
        int cool = -1;
        std::string phase;   // PREPARE(写前意图) / COMMIT(写后完成) —— two-phase 事务标记
    };
    std::optional<BridgeState> read_state(SysfsAdapter& ad) const {
        if (stateFile_.empty()) return std::nullopt;
        auto txt = ad.read_all(stateFile_);
        if (!txt) return std::nullopt;
        if (txt->find("BASELINE=") == std::string::npos) return std::nullopt;
        BridgeState s;
        auto g = [&](const char* k, std::string& out) {
            auto p = txt->find(k);
            if (p == std::string::npos) return;
            auto e = txt->find('\n', p);
            out = txt->substr(p + strlen(k), (e == std::string::npos ? txt->size() : e) - p - strlen(k));
        };
        std::string v;
        g("BASELINE=", v); s.baselineAgg = (v == "true");
        v.clear(); g("DIRTY=", v); s.dirty = (v == "true");
        v.clear(); g("DEPTH=", v); s.depth = v;
        v.clear(); g("COOL=", v); s.cool = v.empty() ? -1 : atoi(v.c_str());
        v.clear(); g("PHASE=", v); s.phase = v.empty() ? "COMMIT" : v;
        return s;
    }
    WriteOutcome mark_state(SysfsAdapter& ad, const BridgeState& s) {
        if (stateFile_.empty()) return WriteOutcome::Missing;
        std::string t = std::string("BASELINE=") + (s.baselineAgg ? "true" : "false") +
                        "\nDIRTY=" + (s.dirty ? "true" : "false") +
                        "\nDEPTH=" + s.depth +
                        "\nCOOL=" + std::to_string(s.cool) +
                        "\nPHASE=" + (s.phase.empty() ? "COMMIT" : s.phase) + "\n";
        std::string d;
        return ad.write(stateFile_, t, /*dryRun=*/false, &d);   // 状态文件永远真写
    }

    // ---- reclaim 节内任意字段读写（jsonLit 为 JSON 字面量：带引号的串或裸数字）----
    std::optional<std::string> read_reclaim_field(SysfsAdapter& ad, const char* key) const {
        auto txt = ad.read_all(cmosJson_);
        if (!txt) return std::nullopt;
        std::string k = std::string("\"") + key + "\"";
        auto p = txt->find(k);
        if (p == std::string::npos) return std::nullopt;
        auto colon = txt->find(':', p + k.size());
        if (colon == std::string::npos) return std::nullopt;
        auto v = colon + 1;
        while (v < txt->size() && (*txt)[v] == ' ') ++v;
        auto end = txt->find_first_of(",}\n", v);
        std::string val = txt->substr(v, (end == std::string::npos ? txt->size() : end) - v);
        // 去尾部空白
        auto t = val.find_last_not_of(" \t\r");
        return t == std::string::npos ? std::string("") : val.substr(0, t + 1);
    }

    WriteOutcome set_reclaim_field(SysfsAdapter& ad, const char* key,
                                   const std::string& jsonLit, bool dryRun,
                                   std::string* detail, bool* changed = nullptr) {
        if (changed) *changed = false;
        auto cur = read_reclaim_field(ad, key);
        if (!cur) { if (detail) *detail = std::string(key) + " key not found"; return WriteOutcome::Missing; }
        if (*cur == jsonLit) {
            if (detail) *detail = std::string(key) + " already " + jsonLit + " (no-op)";
            return WriteOutcome::Ok;
        }
        std::string neu;
        {   // 读原始字节（字节保真：改别的配置一字不动，含尾换行）
            std::ifstream rf(ad.full(cmosJson_), std::ios::binary);
            std::ostringstream ss; ss << rf.rdbuf(); neu = ss.str();
        }
        if (neu.empty()) { if (detail) *detail = "raw read failed"; return WriteOutcome::Missing; }
        std::string k = std::string("\"") + key + "\"";
        auto p = neu.find(k);
        if (p == std::string::npos) { if (detail) *detail = "key lost"; return WriteOutcome::Missing; }
        auto colon = neu.find(':', p + k.size());
        auto v = colon + 1;
        while (v < neu.size() && (neu[v] == ' ')) ++v;
        auto end = neu.find_first_of(",}\n", v);
        if (colon == std::string::npos || end == std::string::npos) {
            if (detail) *detail = "value span not parseable"; return WriteOutcome::Mismatch;
        }
        neu.replace(v, end - v, jsonLit);
        WriteOutcome o = ad.write(cmosJson_, neu, dryRun, detail);
        if (o != WriteOutcome::Ok) return o;
        auto back = read_reclaim_field(ad, key);
        if (!back || *back != jsonLit) {
            if (detail) *detail = std::string(key) + " readback mismatch";
            return WriteOutcome::Mismatch;
        }
        if (changed) *changed = true;
        if (detail) *detail = std::string(key) + ": " + std::string(cur->empty() ? "?" : *cur) + " -> " + jsonLit;
        return o;
    }

    // 设置 aggressive：幂等（同值不写）+ 原子替换 + 读回校验。changed 输出实际落盘与否
    WriteOutcome set_aggressive(SysfsAdapter& ad, bool target, bool dryRun,
                                std::string* detail, bool* changed = nullptr) {
        if (changed) *changed = false;
        auto cur = read_aggressive(ad);
        if (!cur) {
            if (detail) *detail = "aggressive key not found";
            return WriteOutcome::Missing;
        }
        if (*cur == target) {
            if (detail) *detail = "already " + std::string(target ? "true" : "false") + " (no-op)";
            return WriteOutcome::Ok;
        }
        auto txt = ad.read_all(cmosJson_);     // 已 trim，仅用于定位与校验
        if (!txt) { if (detail) *detail = "read failed"; return WriteOutcome::Missing; }
        // 读原始字节（含尾换行）——改别人配置必须字节保真，除目标字段外一字不动
        std::string neu;
        {
            std::ifstream rf(ad.full(cmosJson_), std::ios::binary);
            std::ostringstream ss; ss << rf.rdbuf(); neu = ss.str();
        }
        if (neu.empty()) { if (detail) *detail = "raw read failed"; return WriteOutcome::Missing; }
        auto p = neu.find("\"aggressive\"");
        if (p == std::string::npos) { if (detail) *detail = "key lost"; return WriteOutcome::Missing; }
        auto colon = neu.find(':', p);
        auto t = neu.find("true", colon), f = neu.find("false", colon);
        if (t != std::string::npos && (f == std::string::npos || t < f))
            neu.replace(t, 4, target ? "true" : "false");
        else if (f != std::string::npos)
            neu.replace(f, 5, target ? "true" : "false");
        else { if (detail) *detail = "value not parseable"; return WriteOutcome::Mismatch; }

        WriteOutcome o = ad.write(cmosJson_, neu, dryRun, detail);
        if (o != WriteOutcome::Ok) return o;
        // 读回独立确认（幂等 + 语义正确，而非仅字节相等）
        auto back = read_aggressive(ad);
        if (!back || *back != target) {
            if (detail) *detail = "readback semantic mismatch";
            return WriteOutcome::Mismatch;
        }
        if (changed) *changed = true;
        if (detail) *detail = std::string(*cur ? "true" : "false") + " -> " +
                              (target ? "true" : "false");
        return o;
    }

private:
    CtrlState state_ = CtrlState::Probing;
    CtrlReport r_;
    std::string policyFile_, policyPath_, cmosJson_, stateFile_, mode_ = "balance", last_;
    bool pressured_ = false;
    bool bridge_ = false;
    std::string bridgeWhy_ = "not probed";
    bool baselineAgg_ = false;
    std::string baselineDepth_;   // 含 JSON 引号，如 "\"cached\""；空 = 无档案
    int baselineCool_ = -1;
};

// ============================= CPU Controller =============================
// §5.2 基于 CoreTurboScheduler。铁律：只在策略边界写入（本 apply 仅由边界事件触发），
// 不轮询覆写 scaling_max_freq；本机 governor=uag 默认不切换。
class CpuController final : public Controller {
public:
    // cpuStateFile：uag 参数基线档案（UPRATE:<policy>=<值> / DIRTY），空 = 不接管 uag
    explicit CpuController(std::string cpuStateFile = "")
        : stateFile_(std::move(cpuStateFile)) {}

    const char* name() const override { return "cpu"; }

    CtrlReport probe(SysfsAdapter& ad) override {
        std::vector<NodeCap> caps{
            ad.probe("/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"),
            ad.probe("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq"),
            ad.probe("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"),
        };
        // cpuset 属"可选能力"：缺失只降级 cpuset 子类，不拖垮整个 CPU Controller
        NodeCap cs = ad.probe("/dev/cpuset/top-app/cpus");
        r_ = fold(caps, "cpu");
        if (r_.state == CtrlState::Active) {
            if (!cs.exists) { cpusetOk_ = false; r_.detail = "cpuset absent — cpuset class skipped"; }
            else cpusetOk_ = cs.writable;
            if (auto g = ad.read("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"))
                governor_ = *g;   // 本机 uag：仅记录，不干预（§5.2）
        }
        // ---- uag 参数接管探测（阶段B-CPU + 阶段2）：枚举各簇 up/down 并建基线 ----
        if (r_.state == CtrlState::Active && !stateFile_.empty()) {
            uagNodes_.clear();
            for (int i = 0; i < 10; ++i) {
                std::string pol = "policy" + std::to_string(i);
                std::string pre = "/sys/devices/system/cpu/cpufreq/" + pol + "/uag/";
                std::string up = pre + "up_rate_limit_us";
                if (!ad.probe(up).exists) continue;
                std::string dn = pre + "down_rate_limit_us";
                std::string hi = pre + "hispeed_load";
                std::string tl = pre + "target_loads";
                uagNodes_.push_back({pol, up, ad.probe(dn).exists ? dn : "",
                                     ad.probe(hi).exists ? hi : "",
                                     ad.probe(tl).exists ? tl : ""});
            }
            if (uagNodes_.empty()) {
                r_.detail += " uag=ABSENT";
            } else {
                auto st = read_cpu_state(ad);
                bool dirty = st && st->dirty;
                std::map<std::string,int> base = st ? st->base : std::map<std::string,int>{};
                std::map<std::string,int> baseD = st ? st->baseDown : std::map<std::string,int>{};
                std::map<std::string,int> baseH = st ? st->baseHi : std::map<std::string,int>{};
                std::map<std::string,std::string> baseTl = st ? st->baseTl
                                                              : std::map<std::string,std::string>{};
                auto seed = [&]() {
                    for (auto& n : uagNodes_) {
                        if (!base.count(n.policy))
                            if (auto v = ad.read(n.upPath)) base[n.policy] = atoi(v->c_str());
                        if (!n.downPath.empty() && !baseD.count(n.policy))
                            if (auto v = ad.read(n.downPath)) baseD[n.policy] = atoi(v->c_str());
                        if (!n.hiPath.empty() && !baseH.count(n.policy))
                            if (auto v = ad.read(n.hiPath)) baseH[n.policy] = atoi(v->c_str());
                        if (!n.tlPath.empty() && !baseTl.count(n.policy))
                            if (auto v = ad.read(n.tlPath)) baseTl[n.policy] = *v;
                    }
                };
                bool hiMissing = false, tlMissing = false;
                for (auto& n : uagNodes_) {
                    if (!n.hiPath.empty() && !baseH.count(n.policy)) hiMissing = true;
                    if (!n.tlPath.empty() && !baseTl.count(n.policy)) tlMissing = true;
                }
                bool needSeed = base.empty() || baseD.empty() || hiMissing || tlMissing;
                if (needSeed) seed();   // 补档与恢复是顺序关系，不是互斥（depth 案教训）
                if (dirty) {
                    std::string rec = "CRASH-RECOVERY uag->";
                    std::string d;
                    for (auto& n : uagNodes_) {
                        auto it = base.find(n.policy);
                        if (it != base.end() &&
                            ad.write(n.upPath, std::to_string(it->second), false, &d) == WriteOutcome::Ok)
                            rec += n.policy + ".up=" + std::to_string(it->second) + " ";
                        if (!n.downPath.empty()) {
                            auto it2 = baseD.find(n.policy);
                            if (it2 != baseD.end() &&
                                ad.write(n.downPath, std::to_string(it2->second), false, &d) == WriteOutcome::Ok)
                                rec += n.policy + ".dn=" + std::to_string(it2->second) + " ";
                        }
                        if (!n.hiPath.empty()) {
                            auto it3 = baseH.find(n.policy);
                            if (it3 != baseH.end() &&
                                ad.write(n.hiPath, std::to_string(it3->second), false, &d) == WriteOutcome::Ok)
                                rec += n.policy + ".hi=" + std::to_string(it3->second) + " ";
                        }
                        if (!n.tlPath.empty()) {
                            auto it4 = baseTl.find(n.policy);
                            if (it4 != baseTl.end() &&
                                ad.write(n.tlPath, it4->second, false, &d) == WriteOutcome::Ok)
                                rec += n.policy + ".tl=" + it4->second + " ";
                        }
                    }
                    CpuState cs; cs.base = base; cs.baseDown = baseD; cs.baseHi = baseH; cs.baseTl = baseTl; cs.dirty = false;
                    save_cpu_state(ad, cs);
                    r_.detail += " | " + rec;
                } else if (needSeed) {
                    CpuState cs; cs.base = base; cs.baseDown = baseD; cs.baseHi = baseH; cs.baseTl = baseTl; cs.dirty = false;
                    save_cpu_state(ad, cs);
                    r_.detail += " uag-seed(up+down)";
                }
                hiBase_ = baseH;
                tlBase_ = baseTl;
                uagBase_ = base;
                downBase_ = baseD;
                r_.detail += " uagNodes=" + std::to_string(uagNodes_.size());
            }
        }
        state_ = r_.state;
        return r_;
    }

    void on_event(const Event& e, const GlobalState& st) override {
        if (e.type == EventType::ModeChanged) { mode_ = e.payload; dirty_ = true; }
        if (e.type == EventType::ForegroundChanged && st.generation) dirty_ = true;
    }

    EffectivePolicy desire() const override {
        // 职责分离：uag/频率约束由场景层（PolicyManager）经 resolve(base) 注入
        EffectivePolicy d;
        d.cpu.launchBoost = false;
        return d;
    }

    CtrlReport apply(const EffectivePolicy& eff, SysfsAdapter& ad, bool dryRun) override {
        CtrlReport r{CtrlState::Active, ""};
        std::string detail;
        WriteOutcome o = WriteOutcome::DryRun;
        if (eff.cpu.maxFreq > 0) {
            o = ad.write("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq",
                         std::to_string(eff.cpu.maxFreq), dryRun, &detail);
        }
        // readback mismatch 在本机是已知常态（Oplus ~6s 动态钳制，M0 能力矩阵实测）
        r.detail = std::string("gov=") + governor_ + " maxWrite=" + outcome_name(o) +
                   (detail.empty() ? "" : " (" + detail + ")") +
                   (cpusetOk_ ? "" : " cpuset=SKIP");

        // ---- uag up/down_rate_limit（阶段B-CPU + 阶段2）：-1=回基线，否则写场景值 ----
        if (!uagNodes_.empty() && !uagBase_.empty()) {
            bool offAny = false;
            if (!dryRun) {   // two-phase：写前记意图（同 Memory 侧，零崩溃窗口）
                CpuState pre;
                pre.base = uagBase_; pre.baseDown = downBase_;
                pre.dirty = true; pre.phase = "PREPARE";
                save_cpu_state(ad, pre);
            }
            auto runGroup = [&](const char* tag, const std::string UagNode::*fld,
                                int effVal, const std::map<std::string,int>& baseM) {
                for (auto& n : uagNodes_) {
                    const std::string& path = n.*fld;
                    if (path.empty()) continue;
                    auto bit = baseM.find(n.policy);
                    if (effVal < 0 && bit == baseM.end()) continue;   // 无基线时 -1 不写（防 0 灾难值）
                    int baseV = bit != baseM.end() ? bit->second : 0;
                    int target = (effVal >= 0) ? effVal : baseV;
                    std::string dd;
                    WriteOutcome uo = ad.write(path, std::to_string(target), dryRun, &dd);
                    if (uo == WriteOutcome::Ok || uo == WriteOutcome::DryRun) {
                        r.detail += std::string(" | ") + tag + ":" + n.policy + "=" +
                                    std::to_string(target) + "(" + outcome_name(uo) + ")";
                        if (target != baseV) offAny = true;
                    } else {
                        r.detail += std::string(" | ") + tag + ":" + n.policy + "=" + outcome_name(uo);
                        if (uo != WriteOutcome::DryRun) offAny = true;
                    }
                }
            };
            runGroup("up", &UagNode::upPath, eff.cpu.uagUpRateUs, uagBase_);
            runGroup("dn", &UagNode::downPath, eff.cpu.uagDownRateUs, downBase_);
            runGroup("hi", &UagNode::hiPath, eff.cpu.uagHispeedLoad, hiBase_);
            auto runGroupStr = [&](const char* tag, const std::string UagNode::*fld,
                                   int effVal, const std::map<std::string,std::string>& baseM) {
                for (auto& n : uagNodes_) {
                    const std::string& path = n.*fld;
                    if (path.empty()) continue;
                    auto bit = baseM.find(n.policy);
                    if (effVal < 0 && bit == baseM.end()) continue;   // 无基线不写
                    std::string baseStr = bit != baseM.end() ? bit->second : std::string();
                    std::string target = (effVal >= 0) ? tl_set_first(baseStr, effVal) : baseStr;
                    if (target.empty()) continue;
                    std::string dd;
                    ad.write(path, target, dryRun, &dd);
                    r.detail += std::string(" | ") + tag + ":" + n.policy + "=" + target;
                }
            };
            // target_loads：实测非独占（Oplus 熄屏改写频率点 + 首段写入被覆盖），**彻底断开**：
            // 连基线与回写都不做，避免与 Oplus 争用；通路与档案结构保留备用。
            // runGroupStr("tl", &UagNode::tlPath, eff.cpu.uagTargetLoadsFirst, tlBase_);
            if (!dryRun) {
                CpuState cs; cs.base = uagBase_; cs.baseDown = downBase_; cs.baseHi = hiBase_; cs.baseTl = tlBase_;
                cs.dirty = offAny; cs.phase = "COMMIT";
                save_cpu_state(ad, cs);
            }
            if (eff.cpu.uagUpRateUs >= 0 || eff.cpu.uagDownRateUs >= 0 || offAny)
                r.detail += " [cpuState->" + std::string(offAny ? "DIRTY" : "CLEAN") + "]";
        }

        if (o == WriteOutcome::Missing || o == WriteOutcome::Denied) {
            r.state = CtrlState::Degraded;
            r.detail += " — cpu class degraded";
        }
        state_ = r.state;
        last_ = r.detail;
        dirty_ = false;
        return r;
    }

    CtrlState state() const override { return state_; }
    std::string status() const override {
        return std::string("cpu ") + ctrl_state_name(state_) + " " + last_;
    }

private:
    struct UagNode { std::string policy, upPath, downPath, hiPath, tlPath; };
    struct CpuState { std::map<std::string,int> base, baseDown, baseHi;
        std::map<std::string,std::string> baseTl; bool dirty = false; std::string phase; };

    std::optional<CpuState> read_cpu_state(SysfsAdapter& ad) const {
        if (stateFile_.empty()) return std::nullopt;
        auto txt = ad.read_all(stateFile_);
        if (!txt || txt->find("DIRTY=") == std::string::npos) return std::nullopt;
        CpuState s;
        std::istringstream is(*txt);
        std::string line;
        while (std::getline(is, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "DIRTY") s.dirty = (v == "true");
            else if (k == "PHASE") s.phase = v;
            else if (k.rfind("UP:", 0) == 0) s.base[k.substr(3)] = atoi(v.c_str());
            else if (k.rfind("DOWN:", 0) == 0) s.baseDown[k.substr(5)] = atoi(v.c_str());
            else if (k.rfind("HI:", 0) == 0) s.baseHi[k.substr(3)] = atoi(v.c_str());
            else if (k.rfind("TL:", 0) == 0) s.baseTl[k.substr(3)] = v;
        }
        return s;
    }
    WriteOutcome save_cpu_state(SysfsAdapter& ad, const CpuState& cs) {
        if (stateFile_.empty()) return WriteOutcome::Missing;
        std::ostringstream os;
        os << "DIRTY=" << (cs.dirty ? "true" : "false") << "\n"
           << "PHASE=" << (cs.phase.empty() ? "COMMIT" : cs.phase) << "\n";
        for (auto& kv : cs.base) os << "UP:" << kv.first << "=" << kv.second << "\n";
        for (auto& kv : cs.baseDown) os << "DOWN:" << kv.first << "=" << kv.second << "\n";
        for (auto& kv : cs.baseHi) os << "HI:" << kv.first << "=" << kv.second << "\n";
        for (auto& kv : cs.baseTl) os << "TL:" << kv.first << "=" << kv.second << "\n";
        std::string d;
        return ad.write(stateFile_, os.str(), false, &d);   // 档案永远真写
    }

    CtrlState state_ = CtrlState::Probing;
    CtrlReport r_;
    std::string mode_ = "balance", governor_ = "?", last_, stateFile_;
    bool cpusetOk_ = false, dirty_ = false;
    int maxHint_ = -1;
    std::vector<UagNode> uagNodes_;
    std::map<std::string,int> hiBase_;
    std::map<std::string,std::string> tlBase_;
    std::map<std::string,int> uagBase_;
    std::map<std::string,int> downBase_;
};

// ========================== GPU / Thermal 占位（M2′）==========================
// §9 M2′：GPU/Thermal 仅留接口占位——类在、接口在、恒 Degraded(placeholder)，
// 不参与求交（registry.resolve 跳过 Degraded），实现留给 M2/M4。
class GpuController final : public Controller {
public:
    const char* name() const override { return "gpu"; }
    CtrlReport probe(SysfsAdapter& ad) override {
        r_ = ad.exists(kIdle)
            ? CtrlReport{CtrlState::Active, "idle_timer 行为参数(非上限) 档位联动"}
            : CtrlReport{CtrlState::Degraded, "idle_timer node absent — gpu skipped"};
        return r_;
    }
    void on_event(const Event&, const GlobalState&) override {}
    EffectivePolicy desire() const override { return {}; }
    CtrlReport apply(const EffectivePolicy& eff, SysfsAdapter& ad, bool dryRun) override {
        if (r_.state != CtrlState::Active) return r_;
        int want = eff.gpu.idleTimer;
        if (want <= 0) return r_;
        if (want == lastWrote_) return r_;
        if (dryRun) { r_.detail = "dry-run idle_timer=" + std::to_string(want); return r_; }
        ad.write(kIdle, std::to_string(want), false, nullptr);
        lastWrote_ = want;
        r_.detail = "idle_timer=" + std::to_string(want);
        return r_;
    }
    CtrlState state() const override { return r_.state; }
    std::string status() const override {
        return std::string("gpu ") + ctrl_state_name(r_.state) + " " + r_.detail;
    }
private:
    static constexpr const char* kIdle = "/sys/class/kgsl/kgsl-3d0/idle_timer";
    CtrlReport r_{CtrlState::Probing, ""};
    int lastWrote_ = -1;
};

class ThermalPlaceholder final : public Controller {
public:
    const char* name() const override { return "thermal"; }
    CtrlReport probe(SysfsAdapter&) override {
        // §5.4 T-OBS 观测优先；M2′ 仅占位
        r_ = {CtrlState::Degraded, "M2′ placeholder — thermal T-OBS deferred"};
        return r_;
    }
    void on_event(const Event&, const GlobalState&) override {}
    EffectivePolicy desire() const override { return {}; }
    CtrlReport apply(const EffectivePolicy&, SysfsAdapter&, bool) override { return r_; }
    CtrlState state() const override { return r_.state; }
    std::string status() const override {
        return std::string("thermal ") + ctrl_state_name(r_.state) + " placeholder";
    }
private:
    CtrlReport r_{CtrlState::Probing, ""};
};

ControllerPtr make_memory_controller(const std::string& policyFile,
                                     const std::string& cmosJson,
                                     const std::string& stateFile) {
    return std::make_unique<MemoryController>(policyFile, cmosJson, stateFile);
}
ControllerPtr make_cpu_controller(const std::string& cpuStateFile) {
    return std::make_unique<CpuController>(cpuStateFile);
}
ControllerPtr make_gpu_placeholder()   { return std::make_unique<GpuController>(); }
ControllerPtr make_thermal_placeholder(){ return std::make_unique<ThermalPlaceholder>(); }

} // namespace uro
