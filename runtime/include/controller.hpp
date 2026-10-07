// UnifiedRootOptimizer M2′ — Controller 抽象（设计文档 v1.1 §5 / §9 M2′ 退出条件）
//
// 退出条件（§9 表格）：Memory/CPU Controller 接口 + Adapter 落地，
// GPU/Thermal 仅留接口占位，**所有缺失节点均可降级**（不导致主进程退出）。
#pragma once
#include "event.hpp"
#include "state.hpp"
#include "policy.hpp"
#include "adapter.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace uro {

// Controller 生命周期与状态
enum class CtrlState : uint8_t { Probing, Active, Degraded, Faulted };

inline const char* ctrl_state_name(CtrlState s) {
    switch (s) {
        case CtrlState::Probing:  return "PROBING";
        case CtrlState::Active:   return "ACTIVE";
        case CtrlState::Degraded: return "DEGRADED";
        case CtrlState::Faulted:  return "FAULTED";
    }
    return "?";
}

// 统一故障等级（§7.1）：Degraded = 降级继续；Faulted = 该 Controller 停用但主进程存活
struct CtrlReport {
    CtrlState state = CtrlState::Probing;
    std::string detail;   // 降级/故障原因（进 telemetry）
};

class Controller {
public:
    virtual ~Controller() = default;

    virtual const char* name() const = 0;

    // 能力探测：探测该 Controller 需要的全部节点。
    // 缺失 → state=Degraded（整类 skip + 原因），**不得抛异常、不得退出进程**
    virtual CtrlReport probe(SysfsAdapter& ad) = 0;

    // 事件驱动更新内部期望（§3.4：只响应策略边界事件，不轮询覆写）
    virtual void on_event(const Event& e, const GlobalState& st) = 0;

    // 当前期望的约束（供 Constraint Engine 求交）
    virtual EffectivePolicy desire() const = 0;

    // 执行已求交的策略。dryRun=true 只记录不写（SHADOW 默认）。
    // 仅在策略边界（generation/mode/thermal 档位变化）被调用，写完即走。
    virtual CtrlReport apply(const EffectivePolicy& eff, SysfsAdapter& ad, bool dryRun) = 0;

    virtual CtrlState state() const = 0;
    virtual std::string status() const = 0;   // telemetry 摘要
};

using ControllerPtr = std::unique_ptr<Controller>;

// 工厂（实现见 src/controllers.cpp）：Memory/CPU 为 M2′ 实装，GPU/Thermal 为接口占位。
// cmosJson = COSMemory config/memory.json 路径（空 = 不启用桥接，桥接能力降级）
ControllerPtr make_memory_controller(const std::string& policyFile = "",
                                     const std::string& cmosJson = "");
ControllerPtr make_cpu_controller();
ControllerPtr make_gpu_placeholder();
ControllerPtr make_thermal_placeholder();

// 集中注册：probe 全部（逐个 try，单个降级不影响其他），按边界事件 apply。
// GPU/Thermal 仅占位（M2′：接口在、实现空转 → 恒 Degraded/占位标记）。
class ControllerRegistry {
public:
    void add(ControllerPtr c) { ctrls_.push_back(std::move(c)); }
    size_t size() const { return ctrls_.size(); }
    Controller& at(size_t i) const { return *ctrls_[i]; }

    // 逐个探测，单个 Controller 故障不得影响其余（§9 退出条件核心）
    std::vector<CtrlReport> probe_all(SysfsAdapter& ad) {
        std::vector<CtrlReport> reps;
        for (auto& c : ctrls_) {
            CtrlReport r;
            try {
                r = c->probe(ad);
            } catch (const std::exception& ex) {
                r.state = CtrlState::Faulted;
                r.detail = std::string("probe exception: ") + ex.what();
            } catch (...) {
                r.state = CtrlState::Faulted;
                r.detail = "probe exception: unknown";
            }
            reps.push_back(r);
        }
        return reps;
    }

    // 求交全部 Active Controller 的约束；Degraded/Faulted 的不参与
    EffectivePolicy resolve(uint64_t generation) const {
        return resolve(EffectivePolicy{}, generation);
    }

    // 求交：以场景策略为基底（PolicyManager 的 tactics），叠加各 Active Controller 的约束。
    // Degraded/Faulted 的不参与。Handover（GAME 让权）时基底已标 handover，
    // 调用方据此跳过 apply —— §5.5 全量让权，本框架零写入。
    EffectivePolicy resolve(const EffectivePolicy& base, uint64_t generation) const {
        EffectivePolicy acc = base;
        acc.generation = generation;
        for (auto& c : ctrls_) {
            if (c->state() == CtrlState::Degraded || c->state() == CtrlState::Faulted)
                continue;
            EffectivePolicy d = c->desire();
            acc = intersect(acc, d);
            acc.generation = generation;
        }
        return acc;
    }

    // 边界 apply：Degraded/Faulted 一律跳过（与 resolve 一致——降级即该类能力不可用，
    // 不得继续执行写入）；其余逐个执行，单个失败不影响其他
    std::vector<CtrlReport> apply_all(const EffectivePolicy& eff,
                                      SysfsAdapter& ad, bool dryRun) {
        std::vector<CtrlReport> reps;
        for (auto& c : ctrls_) {
            if (c->state() == CtrlState::Faulted) {
                reps.push_back({CtrlState::Faulted, "skipped: faulted"});
                continue;
            }
            if (c->state() == CtrlState::Degraded) {
                reps.push_back({CtrlState::Degraded, "skipped: degraded"});
                continue;
            }
            CtrlReport r;
            try {
                r = c->apply(eff, ad, dryRun);
            } catch (const std::exception& ex) {
                r.state = CtrlState::Faulted;
                r.detail = std::string("apply exception: ") + ex.what();
            } catch (...) {
                r.state = CtrlState::Faulted;
                r.detail = "apply exception: unknown";
            }
            reps.push_back(r);
        }
        return reps;
    }

    bool any_active() const {
        for (auto& c : ctrls_) if (c->state() == CtrlState::Active) return true;
        return false;
    }

private:
    std::vector<ControllerPtr> ctrls_;
};

} // namespace uro
