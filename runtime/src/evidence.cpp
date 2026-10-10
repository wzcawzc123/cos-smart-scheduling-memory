// Evidence 扩展（FPS/温度，v0.17）——纯逻辑，host 可测；driver 线程在 drivers.cpp。
// 温度：扫 thermal_zone* 的 type+temp(mC)，取 top3 热点 + battery；T-OBS 只读，无干预。
// FPS：dumpsys gfxinfo <fg_pkg> 累计计数差分（每 60s 窗口），按包差分、切包重置基线。
#include "drivers.hpp"
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace uro {

// 扫 thermal_root 下 thermal_zone* → JSON 片段；battery 单列（type 含 battery/batt，无则 -999）
std::string thermal_json(const std::string& thermalRoot) {
    std::vector<std::pair<std::string, int>> all;   // (type, 摄氏度)
    int batt = -999;
    DIR* d = opendir(thermalRoot.c_str());
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name.rfind("thermal_zone", 0) != 0) continue;
            std::ifstream ft(thermalRoot + "/" + name + "/type");
            std::ifstream fv(thermalRoot + "/" + name + "/temp");
            if (!ft.is_open() || !fv.is_open()) continue;
            std::string type; long mc = 0;
            ft >> type; fv >> mc;
            if (type.empty()) continue;
            int c = (int)(mc / 1000);
            if (type.find("batt") != std::string::npos ||
                type.find("Batt") != std::string::npos) batt = c;
            all.push_back({type, c});
        }
        closedir(d);
    }
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::ostringstream o;
    o << "\"battery\":" << batt << ",\"top\":[";
    for (size_t i = 0; i < all.size() && i < 3; ++i) {
        if (i) o << ",";
        o << "{\"t\":\"" << all[i].first << "\"," << "\"c\":" << all[i].second << "}";
    }
    o << "]";
    return o.str();
}

// 电量（M5 分母）：capacity% + 充放电状态 + current_now(mA，作瞬时参考，抖动大不作主指标)
std::string battery_json(const std::string& psRoot) {
    std::ifstream fc(psRoot + "/battery/capacity");
    if (!fc.is_open()) return "\"pct\":-1";   // 降级：字段保留但 -1
    long pct = -1, ma = -1;
    std::string st = "Unknown";
    fc >> pct;
    std::ifstream fs(psRoot + "/battery/status");
    if (fs.is_open()) fs >> st;
    if (st.size() > 32) st = st.substr(0, 32);   // sysfs 异常防御
    std::ifstream fi(psRoot + "/battery/current_now");
    if (fi.is_open()) fi >> ma;
    std::ostringstream o;
    o << "\"pct\":" << pct << ",\"chg\":\"" << st << "\",\"mA\":" << ma;
    return o.str();
}

// dumpsys gfxinfo 输出解析：Total/Janky 两行
bool parse_gfxinfo(const std::string& text, long& total, long& janky) {
    auto a = text.find("Total frames rendered:");
    if (a == std::string::npos) return false;
    total = atol(text.c_str() + a + 22);
    auto b = text.find("Janky frames:");
    janky = (b == std::string::npos) ? 0 : atol(text.c_str() + b + 13);
    return true;
}

} // namespace uro
