// UnifiedRootOptimizer M2′ — Policy 输出契约（设计文档 v1.1 §4.2 / §4.3）
// 各 Controller 只提交"期望值/约束"，最终值由 Constraint Engine 约束求交得出。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace uro {

// ---------- §4.2 契约结构 ----------
struct MemoryPolicy {
    int protectedAdj = 0;
    bool reclaimEnabled = false;
    bool freezeEnabled = false;
    int  maxKillPerRound = 0;
    std::vector<std::string> protectedPackages;
    // ---- 阶段B：激进回收调优参数（引擎 reclaim_cycle 每轮热读，可无痛接管）----
    // 空/-1 = 不干预（交还用户基线）；仅场景明确要求时偏离
    std::string depth;        // "cached"温和 / "previous"标准 / "service"彻底
    int cooldownSec = -1;     // 触发冷却（缩短=更频繁重触发）
    int psiThreshold10x = -1; // PSI 阈值（十分位整数，5.0 → 50；占位本期未启用）
    int memFloorMb = -1;      // MemAvailable 下限 MB（占位本期未启用）
};

struct CpuPolicy {
    std::string governor;          // 空 = 不干预（本机 uag 默认不切换）
    int minFreq = -1;              // -1 = 不约束
    int maxFreq = -1;              // -1 = 不约束
    std::vector<int> onlineCores;  // 空 = 不干预
    std::string cpusetProfile;     // 空 = 不干预
    bool launchBoost = false;
    // ---- 阶段B-CPU：uag 调速参数（M0 定性"策略层能赢的真实抓手"）----
    // -1 = 不干预（回基线）。
    // uagUpRateUs：升频延迟（现值 0=最快）；uagDownRateUs：降频迟滞（现值 0=立即可降）。
    // PERFORMANCE 档 = down 调大 80ms（触摸停止后频率粘高位 80ms，更跟手；语义为
    // governor 通用 rate_limit——两次调频动作最小间隔）。频率上限不收（Oplus 地盘）。
    int uagUpRateUs = -1;
    int uagDownRateUs = -1;
};

struct GpuPolicy {
    int  minFreq = -1;
    int  maxFreq = -1;
    bool boost = false;
    int  idleTimer = 0;   // GPU 空闲降频延迟 ms（行为参数·非上限；0=不定，GAME 让权）
};

struct EffectivePolicy {
    MemoryPolicy memory;
    CpuPolicy    cpu;
    GpuPolicy    gpu;
    uint64_t generation = 0;
    uint64_t leaseUntilMs = 0;     // 租约到期回落（§3.4-4 LaunchBoost TTL）
};

// ---------- 约束求交（§4.3）----------
// 不采用"最后写入者生效"：所有模块提交期望，最终值取约束交集。
// 例：游戏请求 max=3.2G、thermal 上限 2.4G、kernel 上限 3.0G → effective = 2.4G
inline int intersect_min(int a, int b) {          // -1 = 无约束
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}
inline int intersect_max(int a, int b) {          // 取更宽松的下限
    if (a < 0) return b;
    if (b < 0) return a;
    return a > b ? a : b;
}

// 求交：desire 为目标约束，constr 为叠加约束（如 thermal clamp）
inline EffectivePolicy intersect(const EffectivePolicy& desire,
                                 const EffectivePolicy& constr) {
    EffectivePolicy e = desire;
    e.cpu.maxFreq = intersect_min(desire.cpu.maxFreq, constr.cpu.maxFreq);
    e.cpu.minFreq = intersect_max(desire.cpu.minFreq, constr.cpu.minFreq);
    e.gpu.maxFreq = intersect_min(desire.gpu.maxFreq, constr.gpu.maxFreq);
    e.gpu.minFreq = intersect_max(desire.gpu.minFreq, constr.gpu.minFreq);
    e.cpu.launchBoost = desire.cpu.launchBoost && constr.cpu.launchBoost;
    // uag 升频延迟：值越大越限制升频（越保守）→ 约束求交取更限制的一端（max）
    e.cpu.uagUpRateUs = intersect_max(desire.cpu.uagUpRateUs, constr.cpu.uagUpRateUs);
    // uag 降频迟滞：值越大越限制降频（频率更粘高位）→ 同样取 max
    e.cpu.uagDownRateUs = intersect_max(desire.cpu.uagDownRateUs, constr.cpu.uagDownRateUs);
    e.gpu.boost = desire.gpu.boost && constr.gpu.boost;
    e.memory.reclaimEnabled = desire.memory.reclaimEnabled || constr.memory.reclaimEnabled;
    e.memory.freezeEnabled  = desire.memory.freezeEnabled  || constr.memory.freezeEnabled;
    // min 一旦求交后超过 max，钳回 max（约束无解时保上限）
    if (e.cpu.maxFreq > 0 && e.cpu.minFreq > e.cpu.maxFreq) e.cpu.minFreq = e.cpu.maxFreq;
    if (e.gpu.maxFreq > 0 && e.gpu.minFreq > e.gpu.maxFreq) e.gpu.minFreq = e.gpu.maxFreq;
    return e;
}

// 策略是否与上次实质不同（§3.4：只在策略边界触发写入）
inline bool materially_different(const EffectivePolicy& a, const EffectivePolicy& b) {
    return a.cpu.governor != b.cpu.governor ||
           a.cpu.maxFreq  != b.cpu.maxFreq   ||
           a.cpu.minFreq  != b.cpu.minFreq   ||
           a.cpu.cpusetProfile != b.cpu.cpusetProfile ||
           a.cpu.onlineCores != b.cpu.onlineCores ||
           a.cpu.launchBoost != b.cpu.launchBoost ||
           a.cpu.uagUpRateUs != b.cpu.uagUpRateUs ||
           a.cpu.uagDownRateUs != b.cpu.uagDownRateUs ||
           a.gpu.maxFreq != b.gpu.maxFreq || a.gpu.minFreq != b.gpu.minFreq ||
           a.gpu.boost != b.gpu.boost ||
           a.memory.reclaimEnabled != b.memory.reclaimEnabled ||
           a.memory.freezeEnabled != b.memory.freezeEnabled ||
           a.memory.maxKillPerRound != b.memory.maxKillPerRound ||
           a.memory.protectedAdj != b.memory.protectedAdj;
}

} // namespace uro
