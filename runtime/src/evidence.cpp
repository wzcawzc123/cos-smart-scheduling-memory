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
// L4 电量守卫输入（Evidence 回喂）：pct + 充电态
void read_battery_state(const std::string& psRoot, int& pct, bool& charging) {
    pct = -1; charging = false;
    std::ifstream fc(psRoot + "/battery/capacity");
    if (fc.is_open()) fc >> pct;
    std::ifstream fs(psRoot + "/battery/status");
    std::string st;
    if (fs.is_open()) fs >> st;
    charging = (st != "Discharging" && !st.empty());
}

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

// Thermal Detector（T-OBS→干预）：最高热点温度（无数据返回 -1）
// GPU 观测（Adreno kgsl，只读）：busy% + cur freq MHz
std::string gpu_json(const std::string& kgslRoot) {
    auto rd = [&](const char* f) -> long {
        std::ifstream i(kgslRoot + "/" + f);
        long v = -1; if (i.is_open()) i >> v; return v;
    };
    long busy = rd("gpu_busy_percentage");
    long clk = rd("gpuclk");
    std::ostringstream o;
    o << "\"gpuBusy\":" << busy << ",\"gpuClkMhz\":" << (clk > 0 ? clk / 1000000 : -1)
      << ",\"gpuIdleTimer\":" << rd("idle_timer");
    return o.str();
}

// L4 帧率哨兵：最近 n 个窗口 jank 率全部 > 阈值 → 触发（观测告警，不自动改策略）
// uag target_loads 多段改写：只改首段，保留频率点段（"80 2112000:95" -> "70 2112000:95"）
std::string tl_set_first(const std::string& base, int first) {
    if (base.empty() || first < 0) return base;
    size_t sp = base.find(' ');
    if (sp == std::string::npos) return std::to_string(first);
    return std::to_string(first) + base.substr(sp);
}

bool fps_sentry_trigger(const int* jankPctHist, int n, int threshPct) {
    if (n <= 0) return false;
    for (int i = 0; i < n; ++i)
        if (jankPctHist[i] <= threshPct) return false;
    return true;
}

// v0.39：温度源分型。旧实现扫全部 thermal_zone 取最大值 —— 本机 zone0 是
// "pa"（功放）、还有 battery/shell_*/wireless 等，最大值可能来自非 SoC 传感器，
// 导致热级误判。改为优先只采纳 SoC 类（cpuss/cpu-*-*/gpuss），无此类时再回退。
static bool is_soc_temp_type(const std::string& t) {
    return t.rfind("cpuss", 0) == 0 || t.rfind("cpu-", 0) == 0 || t.rfind("gpuss", 0) == 0;
}
static bool is_non_soc_temp_type(const std::string& t) {
    static const char* bad[] = {"pa", "battery", "vbat", "shell_", "usb", "wireless",
                                "mmw", "zeku", "socd", "arch", "quiet", "cam", "nsp",
                                "wlan", "ddr", "disp"};
    for (const char* b : bad)
        if (t.rfind(b, 0) == 0) return true;
    return false;
}

int read_max_temp(const std::string& thermalRoot) {
    int best = -1;      // SoC 类最高温（首选）
    int fallback = -1;  // 非 SoC 回退（老机型无 cpuss/cpu-*/gpuss 命名时）
    DIR* d = opendir(thermalRoot.c_str());
    if (!d) return -1;
    struct dirent* en;
    while ((en = readdir(d)) != nullptr) {
        std::string name = en->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::string type;
        {
            std::ifstream ft(thermalRoot + "/" + name + "/type");
            if (ft.is_open()) std::getline(ft, type);
        }
        std::ifstream fv(thermalRoot + "/" + name + "/temp");
        if (!fv.is_open()) continue;
        long mc = 0; fv >> mc;
        if (mc <= -40000 || mc > 150000) continue;   // 未校准/无效传感器（如 -273000）
        int c = (int)(mc / 1000);
        if (is_soc_temp_type(type)) {
            if (c > best) best = c;
        } else if (!is_non_soc_temp_type(type)) {
            if (c > fallback) fallback = c;
        }
    }
    closedir(d);
    return best >= 0 ? best : fallback;
}

// v0.39：供 ThermalPlaceholder 判存在（按 type 找 SoC 传感器，而非假定 zone0）
std::string find_soc_zone(const std::string& thermalRoot) {
    DIR* d = opendir(thermalRoot.c_str());
    if (!d) return "";
    struct dirent* en;
    std::string hit;
    while ((en = readdir(d)) != nullptr) {
        std::string name = en->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::ifstream ft(thermalRoot + "/" + name + "/type");
        if (!ft.is_open()) continue;
        std::string type; std::getline(ft, type);
        if (is_soc_temp_type(type)) { hit = name; break; }
    }
    closedir(d);
    return hit;
}

// 热级状态机（带迟滞）：prev 级、当前温度 t、阈值 t1/t2、迟滞 hyst
// 进：t>=t1 → 1、t>=t2 → 2；出：1→0 需 t<=t1-hyst、2→1 需 t<=t2-hyst（逐级回落）
int next_thermal_level(int prev, int t, int t1, int t2, int hyst, int trendDeg) {
    if (t < 0) return prev;                       // 无传感器数据保持原级
    // L4 趋势预判：接近阈值(t1-6)且近期升温>=4°C → 视作已达 t1（提前一级，防撞温度墙）
    int teff = t;
    if (trendDeg >= 4 && t >= t1 - 6 && t < t1) teff = t1;
    if (teff >= t2) return 2;
    if (teff >= t1) return prev >= 1 ? prev : 1;   // 已在1/2不降级（热区内保持）
    // 低于 t1：逐级回落
    if (prev >= 2 && t <= t2 - hyst) return 1;
    if (prev >= 1 && t <= t1 - hyst) return 0;
    return prev;
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
