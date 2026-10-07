// UnifiedRootOptimizer M2′ — Memory / CPU Controller 实现 + GPU / Thermal 占位
// 设计文档 v1.1 §5.1 §5.2；退出条件 = 缺失节点均可降级，不导致主进程退出。
#include "controller.hpp"
#include <fstream>
#include <sstream>
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
    explicit MemoryController(std::string policyFile = "", std::string cmosJson = "")
        : policyFile_(std::move(policyFile)), cmosJson_(std::move(cmosJson)) {}

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
                // 读基线 aggressive（首次）：URO 只在场景需要时偏离，退出必须恢复基线
                auto cur = read_aggressive(ad);
                if (cur.has_value()) baselineAgg_ = *cur;
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
            bool target;
            if (!eff.memory.reclaimEnabled)      target = false;
            else if (eff.memory.maxKillPerRound > 0) target = true;
            else                                 target = baselineAgg_;
            std::string bdet;
            WriteOutcome bo = set_aggressive(ad, target, dryRun, &bdet);
            r.detail += std::string(" | bridge aggressive->") + (target ? "true" : "false") +
                        " " + outcome_name(bo) + " (" + bdet + ")";
            if (bo == WriteOutcome::Missing || bo == WriteOutcome::Denied) {
                bridge_ = false;   // 桥接能力降级，主 Controller 保持 Active
                bridgeWhy_ = "bridge lost: " + bdet;
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

    // 设置 aggressive：幂等（同值不写）+ 原子替换 + 读回校验
    WriteOutcome set_aggressive(SysfsAdapter& ad, bool target, bool dryRun, std::string* detail) {
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
        if (detail) *detail = std::string(*cur ? "true" : "false") + " -> " +
                              (target ? "true" : "false");
        return o;
    }

private:
    CtrlState state_ = CtrlState::Probing;
    CtrlReport r_;
    std::string policyFile_, policyPath_, cmosJson_, mode_ = "balance", last_;
    bool pressured_ = false;
    bool bridge_ = false;
    std::string bridgeWhy_ = "not probed";
    bool baselineAgg_ = false;
};

// ============================= CPU Controller =============================
// §5.2 基于 CoreTurboScheduler。铁律：只在策略边界写入（本 apply 仅由边界事件触发），
// 不轮询覆写 scaling_max_freq；本机 governor=uag 默认不切换。
class CpuController final : public Controller {
public:
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
        state_ = r_.state;
        return r_;
    }

    void on_event(const Event& e, const GlobalState& st) override {
        if (e.type == EventType::ModeChanged) { mode_ = e.payload; dirty_ = true; }
        if (e.type == EventType::ForegroundChanged && st.generation) dirty_ = true;
    }

    EffectivePolicy desire() const override {
        EffectivePolicy d;
        if (mode_ == "powersave") {          // 边界条件：只提交上下限，微调权归 uag
            d.cpu.maxFreq = maxHint_;        // M3 接能力矩阵后填真实档位
        }
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
    CtrlState state_ = CtrlState::Probing;
    CtrlReport r_;
    std::string mode_ = "balance", governor_ = "?", last_;
    bool cpusetOk_ = false, dirty_ = false;
    int maxHint_ = -1;
};

// ========================== GPU / Thermal 占位（M2′）==========================
// §9 M2′：GPU/Thermal 仅留接口占位——类在、接口在、恒 Degraded(placeholder)，
// 不参与求交（registry.resolve 跳过 Degraded），实现留给 M2/M4。
class GpuPlaceholder final : public Controller {
public:
    const char* name() const override { return "gpu"; }
    CtrlReport probe(SysfsAdapter&) override {
        r_ = {CtrlState::Degraded, "M2′ placeholder — GPU Controller not implemented"};
        return r_;
    }
    void on_event(const Event&, const GlobalState&) override {}
    EffectivePolicy desire() const override { return {}; }
    CtrlReport apply(const EffectivePolicy&, SysfsAdapter&, bool) override { return r_; }
    CtrlState state() const override { return r_.state; }
    std::string status() const override {
        return std::string("gpu ") + ctrl_state_name(r_.state) + " placeholder";
    }
private:
    CtrlReport r_{CtrlState::Probing, ""};
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
                                     const std::string& cmosJson) {
    return std::make_unique<MemoryController>(policyFile, cmosJson);
}
ControllerPtr make_cpu_controller()    { return std::make_unique<CpuController>(); }
ControllerPtr make_gpu_placeholder()   { return std::make_unique<GpuPlaceholder>(); }
ControllerPtr make_thermal_placeholder(){ return std::make_unique<ThermalPlaceholder>(); }

} // namespace uro
