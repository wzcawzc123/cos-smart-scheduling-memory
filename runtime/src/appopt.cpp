// ThreadController Phase-1 纯逻辑（AppOpt 路线 a）——不依赖 Android 头，host 可测。
// 编译入 host 套件（run_test.sh）与 NDK 主构建。
#include "drivers.hpp"
#include <dirent.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace uro {

static std::string trim_local(const std::string& t) {
    auto a = t.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = t.find_last_not_of(" \t\r\n");
    return t.substr(a, b - a + 1);
}

AppOptSummary read_appopt(const std::string& confPath, const std::string& gamePath,
                          const std::string& cpusetRoot) {
    AppOptSummary s;
    std::vector<std::string> active;
    {
        std::ifstream cf(confPath);
        if (cf.is_open()) {
            s.present = true;
            std::string line;
            while (std::getline(cf, line)) {
                auto h = line.find('#');
                if (h != std::string::npos) line = line.substr(0, h);
                if (line.rfind("//", 0) == 0) line.clear();
                else { auto c = line.find("//"); if (c != std::string::npos) line = line.substr(0, c); }
                line = trim_local(line);
                if (line.empty()) continue;
                active.push_back(line);
                s.rules++;
            }
        }
    }
    if (!active.empty()) {   // ③ 游戏豁免校验（只读）
        std::ifstream gf(gamePath);
        std::string g;
        while (std::getline(gf, g)) {
            auto h = g.find('#');
            if (h != std::string::npos) g = g.substr(0, h);
            g = trim_local(g);
            if (g.empty()) continue;
            for (auto& r : active)
                if (r.find(g) != std::string::npos) { s.gameViolation = g; break; }
            if (!s.gameViolation.empty()) break;
        }
    }
    DIR* d = opendir(cpusetRoot.c_str());   // ① cpuset 组成员快照
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name.empty() || name == "." || name == "..") continue;
            if (name.find_first_not_of("0123456789-") != std::string::npos) continue;
            std::ifstream tf(cpusetRoot + "/" + name + "/tasks");
            if (!tf.is_open()) continue;
            int n = 0; std::string tl;
            while (std::getline(tf, tl)) if (!tl.empty()) n++;
            s.groups.push_back({name, n});
            s.present = true;
        }
        closedir(d);
    }
    return s;
}

// ④ URO-GEN 区块框架（幂等；纯注释、零行为；本期不生成规则）
void ensure_uro_gen_block(const std::string& confPath) {
    std::ifstream f(confPath);
    if (!f.is_open()) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string c = ss.str();
    f.close();
    if (c.find("URO-GEN-BEGIN") != std::string::npos) return;   // 已有框架
    std::ifstream chk(confPath + ".bak_uro_gen");
    if (!chk.is_open()) {   // 备份一次（参照 10-07 .bak_20261007 惯例）
        std::ofstream bo(confPath + ".bak_uro_gen");
        bo << c;
    }
    std::ofstream o(confPath, std::ios::app);
    o << "\n# ---- URO-GEN-BEGIN (UnifiedRootOptimizer 规则代理区块：URO 只在本标记内追加，勿删标记行) ----\n"
         "# ---- URO-GEN-END ----\n";
}


// ===== AppOpt 指挥链①：GAME 自动豁免（10-07 手动处置的自动化）=====
// 语义：把含游戏包的活跃规则行注释掉并打 [URO-EXEMPT] 标记；出 GAME 按标记还原。
// 只碰：活跃行（未注释）且包含 game_apps 列表中包名的行；用户手改的注释行永不触碰。
// 幂等：已标记行跳过；原子写（tmp+rename）防 AppOpt 读到半写文件；首次操作前备份。
static const char* kExemptMark = "# [URO-EXEMPT] ";

static std::vector<std::string> read_lines(const std::string& f) {
    std::vector<std::string> out;
    std::ifstream in(f);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

static bool atomic_write(const std::string& f, const std::vector<std::string>& lines) {
    std::string tmp = f + ".uro_tmp";
    { std::ofstream o(tmp, std::ios::trunc);
      if (!o.is_open()) return false;
      for (auto& l : lines) o << l << "\n"; }
    return std::rename(tmp.c_str(), f.c_str()) == 0;
}

static void backup_once(const std::string& f) {
    std::ifstream chk(f + ".uro_exempt.bak");
    if (chk.is_open()) return;
    std::vector<std::string> lines = read_lines(f);
    std::ofstream bo(f + ".uro_exempt.bak", std::ios::trunc);
    for (auto& l : lines) bo << l << "\n";
}

int exempt_game_rules(const std::string& confPath, const std::string& gameListPath) {
    std::vector<std::string> pkgs;
    for (auto& l : read_lines(gameListPath)) {
        auto h = l.find('#');
        if (h != std::string::npos) l = l.substr(0, h);
        l = trim_local(l);
        if (!l.empty()) pkgs.push_back(l);
    }
    if (pkgs.empty()) return 0;
    auto lines = read_lines(confPath);
    if (lines.empty()) return 0;
    int n = 0;
    for (auto& l : lines) {
        if (l.rfind(kExemptMark, 0) == 0) continue;        // 已标记
        if (l.find('#') == 0) continue;                     // 用户注释行不动
        std::string t = l;
        auto h = t.find('#'); if (h != std::string::npos) t = t.substr(0, h);
        if (trim_local(t).empty()) continue;
        for (auto& pkg : pkgs) {
            if (l.find(pkg) != std::string::npos) {
                l = std::string(kExemptMark) + l;
                ++n;
                break;
            }
        }
    }
    if (n > 0) { backup_once(confPath); atomic_write(confPath, lines); }
    return n;
}

int restore_exempted(const std::string& confPath) {
    auto lines = read_lines(confPath);
    if (lines.empty()) return 0;
    int n = 0;
    for (auto& l : lines) {
        if (l.rfind(kExemptMark, 0) == 0) {
            l = l.substr(std::strlen(kExemptMark));
            ++n;
        }
    }
    if (n > 0) atomic_write(confPath, lines);
    return n;
}


// ===== AppOpt 指挥链②：画像 → URO-GEN 区块规则生成 =====
// 画像格式 v0.14: pattern|down|up ；扩展第四字段: pattern|down|up|cpuset
// cpuset ∈ AppOpt 组别名(e-core/p-core/hp-core) 或组名(0-7/3-7/...)；空=不生成。
// 对账语义：区块内行集 == 目标行集则不动；带 * 通配的画像保守跳过(AppOpt 通配语义未验)。
// EXEMPT 兼容：区块内被 # [URO-EXEMPT] 标记的行 strip 后视为已存在 → 不复活豁免。
struct ProfileRule { std::string pattern; std::string cpuset; };

static std::vector<ProfileRule> parse_profile_rules(const std::string& profilesPath) {
    std::vector<ProfileRule> out;
    for (auto& line : read_lines(profilesPath)) {
        std::string l = line;
        auto h = l.find('#'); if (h != std::string::npos) l = l.substr(0, h);
        l = trim_local(l);
        if (l.empty()) continue;
        std::vector<std::string> f;
        size_t pos = 0;
        while (true) {
            size_t bar = l.find('|', pos);
            if (bar == std::string::npos) { f.push_back(l.substr(pos)); break; }
            f.push_back(l.substr(pos, bar - pos));
            pos = bar + 1;
        }
        if (f.size() < 4) continue;                       // 无第四字段
        ProfileRule r{trim_local(f[0]), trim_local(f[3])};
        if (r.pattern.empty() || r.cpuset.empty()) continue;
        out.push_back(r);
    }
    return out;
}

int reconcile_uro_gen(const std::string& confPath, const std::string& profilesPath) {
    auto rules = parse_profile_rules(profilesPath);
    auto lines = read_lines(confPath);
    if (lines.empty()) return 0;

    // 定位区块
    int b = -1, e = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("URO-GEN-BEGIN") != std::string::npos) b = (int)i;
        else if (lines[i].find("URO-GEN-END") != std::string::npos) e = (int)i;
    }
    if (b < 0 || e < 0 || e <= b) return 0;               // 无区块（ensure 负责建）

    // 目标行集（带通配跳过统计）
    std::vector<std::string> want;
    int skipped = 0;
    for (auto& r : rules) {
        if (r.pattern.find('*') != std::string::npos) { ++skipped; continue; }
        want.push_back(r.pattern + "=" + r.cpuset);
    }

    // 现状（strip EXEMPT 标记后比较，保留豁免状态）
    std::vector<std::string> have;
    bool anyMarked = false;
    for (int i = b + 1; i < e; ++i) {
        std::string l = lines[i];
        if (l.rfind(kExemptMark, 0) == 0) { l = l.substr(std::strlen(kExemptMark)); anyMarked = true; }
        l = trim_local(l);
        if (!l.empty()) have.push_back(l);
    }

    if (have == want) return 0;                           // 幂等：无变化（含豁免态保持）

    // 重写区块内容（保留豁免态：目标行若在现状中被标记 → 写回时带标记）
    std::vector<std::string> next;
    for (auto& w : want) {
        std::string out = w;
        for (int i = b + 1; i < e; ++i) {
            std::string l = lines[i];
            if (l.rfind(kExemptMark, 0) == 0 &&
                trim_local(l.substr(std::strlen(kExemptMark))) == w) {
                out = l;                                  // 保持标记态
                break;
            }
        }
        next.push_back(out);
    }
    std::vector<std::string> res(lines.begin(), lines.begin() + b + 1);
    res.insert(res.end(), next.begin(), next.end());
    res.insert(res.end(), lines.begin() + e, lines.end());
    backup_once(confPath);
    atomic_write(confPath, res);
    (void)anyMarked;
    return (int)want.size();
}

} // namespace uro
