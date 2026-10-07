// UnifiedRootOptimizer M2′ — SysfsAdapter 实现
// 铁律：写前校验（存在/可写）→ 写 → 写后 readback（§5.2）。dryRun 永不落盘。
#include "adapter.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace uro {

static std::string trim_copy(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

NodeCap SysfsAdapter::probe(const std::string& relPath) const {
    NodeCap c;
    c.path = full(relPath);
    struct stat st{};
    if (::stat(c.path.c_str(), &st) != 0) {
        c.reason = std::string("stat: ") + strerror(errno);
        return c;
    }
    c.exists = true;
    c.readable = (::access(c.path.c_str(), R_OK) == 0);
    c.writable = (::access(c.path.c_str(), W_OK) == 0);
    if (!c.readable) c.reason = "not readable";
    else if (!c.writable) c.reason = "not writable (read-only capability)";
    return c;
}

std::optional<std::string> SysfsAdapter::read(const std::string& relPath) const {
    std::ifstream f(full(relPath));
    if (!f.is_open()) return std::nullopt;
    std::string line;
    if (!std::getline(f, line)) return std::nullopt;
    return trim_copy(line);
}

WriteOutcome SysfsAdapter::write(const std::string& relPath, const std::string& value,
                                 bool dryRun, std::string* detail) const {
    auto log = [&](WriteOutcome o, const std::string& d) {
        traces_.push_back({relPath, "write", value, outcome_name(o), d});
        if (detail) *detail = d;
        return o;
    };

    if (value.empty()) return log(WriteOutcome::Empty, "empty value rejected");

    NodeCap cap = probe(relPath);
    if (!cap.exists) {
        // 目标不存在时区分两种情况：
        //   - 父目录不可写 → 真正的节点缺失（sysfs 节点类）→ 降级
        //   - 父目录可写   → 普通文件目标（策略快照），首次写入即创建
        auto slash = cap.path.find_last_of('/');
        std::string parent = (slash == std::string::npos) ? "." : cap.path.substr(0, slash);
        if (::access(parent.c_str(), W_OK) != 0)
            return log(WriteOutcome::Missing, "node absent and parent not writable: " + parent);
        cap.writable = true;
    }
    if (dryRun)        return log(WriteOutcome::DryRun, "would write: " + value);
    if (!cap.writable) return log(WriteOutcome::Denied, "write access denied");

    // 写前校验通过，落盘（目标不存在则创建）
    int fd = ::open(cap.path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return log(WriteOutcome::Denied, std::string("open: ") + strerror(errno));
    ssize_t n = ::write(fd, value.data(), value.size());
    int err = errno;
    // 普通文件目标（策略快照）可能残留旧尾部 → 按需截断；
    // sysfs 节点 st_size 恒为 PAGE_SIZE 且不支持 truncate，失败即忽略。
    struct stat fst{};
    if (n > 0 && fstat(fd, &fst) == 0 && fst.st_size > (off_t)n)
        (void)!::ftruncate(fd, (off_t)n);
    ::close(fd);
    if (n != (ssize_t)value.size())
        return log(WriteOutcome::Denied, std::string("write: ") + strerror(err));

    // 写后 readback（§5.2 强制；被 ROM/governor 拦截或动态钳制时暴露为 Mismatch）
    auto back = read(relPath);
    if (!back) return log(WriteOutcome::Mismatch, "readback failed");
    if (*back != value)
        return log(WriteOutcome::Mismatch, "readback '" + *back + "' != wrote '" + value + "'");
    return log(WriteOutcome::Ok, "readback verified");
}

} // namespace uro
