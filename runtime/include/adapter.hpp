// UnifiedRootOptimizer M2′ — Adapter：节点能力探测 / 写前校验 / 写后 readback / 降级
// 设计文档 v1.1 §5.2（CPUFreq 写前校验 + readback）、§3.4（写完即走）、§9 M2′（缺失节点降级）
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdio>

namespace uro {

// 写入结果分类——降级判定的唯一依据
enum class WriteOutcome {
    Ok,          // 写入且 readback 一致
    DryRun,      // dry-run：未落盘（SHADOW 模式默认）
    Missing,     // 节点不存在 → 该能力整类 skip
    Denied,      // EACCES/EPERM → 记录并降级（§5.3 权限失败应记录并降级）
    Mismatch,    // 写入成功但 readback 不一致（被 ROM/governor 拦截）
    Empty        // 传入值为空，拒绝写
};

inline const char* outcome_name(WriteOutcome o) {
    switch (o) {
        case WriteOutcome::Ok:       return "OK";
        case WriteOutcome::DryRun:   return "DRYRUN";
        case WriteOutcome::Missing:  return "MISSING";
        case WriteOutcome::Denied:   return "DENIED";
        case WriteOutcome::Mismatch: return "MISMATCH";
        case WriteOutcome::Empty:    return "EMPTY";
    }
    return "?";
}

// 单个 sysfs/proc 节点的探测结果
struct NodeCap {
    std::string path;
    bool exists = false;
    bool readable = false;
    bool writable = false;
    std::string reason;   // 不可用原因（用于 telemetry / ControllerFault）
};

// 能力探测与读写。所有路径可注入（测试用 fake sysfs 目录）。
class SysfsAdapter {
public:
    explicit SysfsAdapter(std::string root = "") : root_(std::move(root)) {}

    NodeCap probe(const std::string& relPath) const;
    bool exists(const std::string& relPath) const { return probe(relPath).exists; }

    // 读首行（trim 后）；失败返回 nullopt
    std::optional<std::string> read(const std::string& relPath) const;

    // 读全部内容（trim 尾部空白）；用于多行目标（如 JSON）的 readback 校验
    std::optional<std::string> read_all(const std::string& relPath) const;

    // 写入 + readback 校验。dryRun=true 只记录不落盘（SHADOW 铁律）。
    WriteOutcome write(const std::string& relPath, const std::string& value,
                       bool dryRun, std::string* detail = nullptr) const;

    // 组合路径：root_ 为空（真实运行）原样返回；root_ 非空时
    //   - 已带 root_ 前缀 → 直通（避免双拼，如 MemoryController 缓存的 policyPath_）
    //   - 其余（含 /proc、/sys 等真实绝对路径）→ 拼进 fake root（测试隔离靠它）
    std::string full(const std::string& relPath) const {
        if (root_.empty()) return relPath;
        if (relPath.rfind(root_ + "/", 0) == 0) return relPath;
        return root_ + "/" + relPath;
    }
    const std::string& root() const { return root_; }

    // 最近一次探测/写入的调和记录（telemetry 用）
    struct Trace { std::string path, op, value, result, detail; };
    const std::vector<Trace>& traces() const { return traces_; }
    void clear_traces() { traces_.clear(); }

private:
    std::string root_;
    mutable std::vector<Trace> traces_;
};

} // namespace uro
