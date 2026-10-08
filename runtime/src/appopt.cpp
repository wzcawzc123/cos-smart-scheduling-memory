// ThreadController Phase-1 纯逻辑（AppOpt 路线 a）——不依赖 Android 头，host 可测。
// 编译入 host 套件（run_test.sh）与 NDK 主构建。
#include "drivers.hpp"
#include <dirent.h>
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

} // namespace uro
