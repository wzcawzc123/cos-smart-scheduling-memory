// tactics_conf.hpp — 场景配方外置（v0.33 第1批）
// 纯逻辑：解析 + 校验；不依赖 PolicyManager（避免循环包含），host 可单测。
// 覆盖应用逻辑在 policy_manager.hpp 内（apply_tactics_override）。
// 依据 docs/参数外置_v0.33_spec.md §3/§4
#pragma once
#include "scenario.hpp"
#include <string>
#include <map>
#include <optional>
#include <cstdlib>
#include <cctype>
#include <sstream>

namespace uro {

// ---------- 场景名 <-> 键 ----------
inline const char* scenario_key(Scenario s) {
    switch (s) {
        case Scenario::BOOT:            return "BOOT";
        case Scenario::BALANCE:         return "BALANCE";
        case Scenario::POWER_SAVE:      return "POWER_SAVE";
        case Scenario::PERFORMANCE:     return "PERFORMANCE";
        case Scenario::FAST:            return "FAST";
        case Scenario::GAME:            return "GAME";
        case Scenario::MEMORY_PRESSURE: return "MEMORY_PRESSURE";
    }
    return "";
}

inline std::optional<Scenario> scenario_from_key(const std::string& k) {
    for (Scenario s : {Scenario::BOOT, Scenario::BALANCE, Scenario::POWER_SAVE,
                       Scenario::PERFORMANCE, Scenario::FAST, Scenario::GAME,
                       Scenario::MEMORY_PRESSURE}) {
        if (k == scenario_key(s)) return s;
    }
    return std::nullopt;
}

// ---------- 字段白名单与值域（spec §3 表） ----------
struct FieldSpec {
    const char* key;
    long lo, hi;
    bool isBool;
    bool isText;
};

inline const FieldSpec* field_specs(size_t& n) {
    static const FieldSpec kSpecs[] = {
        {"reclaim",   0, 1,       true,  false},
        {"freeze",    0, 1,       true,  false},
        {"maxKill",   0, 20,      false, false},
        {"depth",     0, 0,       false, true },
        {"cooldown",  -1, 3600,   false, false},
        {"uagUp",     -1, 1000000,false, false},
        {"uagDown",   -1, 1000000,false, false},
        {"uagHi",     -1, 100,    false, false},
        {"uagTl",     -1, 100,    false, false},
        {"gpuIdle",   0, 1000,    false, false},
        {"gpuMinPwr", -1, 20,     false, false},
        {"gpuMaxPwr", -1, 20,     false, false},
        {"gpuSplit",  -1, 1,      false, false},
        {"gpuDefPwr", -1, 20,     false, false},
        {"gpuMinBw",  -1, 10000,  false, false},
        {"gpuForceClk", -1, 1,    false, false},
        {"gpuForceBus", -1, 1,    false, false},
        {"cpuMinK",   -1, 4000000,false, false},
        {"uagSoftK",  -1, 4000000,false, false},
        {"gpuThermalPwr", -1, 20, false, false},
        {"gpuMinClkMhz", -1, 2000, false, false},
        {"gpuMaxClkMhz", -1, 2000, false, false},
        {"uclampMin", -1, 1024,   false, false},
        {"uclampMax", -1, 1024,   false, false},
        {"uclampRt",  -1, 1024,   false, false},
        {"keepAdj",   -1, 1000,   false, false},
        {"cpuClamp",  0, 1,       true,  false},
        {"maxFreqK",  -1, 4000000,false, false},
        {"maxFreqK3", -1, 4000000,false, false},
        {"maxFreqK7", -1, 4000000,false, false},
        {"handover",  0, 1,       true,  false},
    };
    n = sizeof(kSpecs) / sizeof(kSpecs[0]);
    return kSpecs;
}

inline const FieldSpec* find_field(const std::string& k) {
    size_t n = 0;
    const FieldSpec* sp = field_specs(n);
    for (size_t i = 0; i < n; ++i) if (k == sp[i].key) return &sp[i];
    return nullptr;
}

// ---------- 解析结果 ----------
struct TacticsConf {
    bool ok = false;          // 解析成功（成功才允许应用）
    bool present = false;     // 文件存在（不存在=走内置默认，不是错误）
    int  applied = 0;         // 生效字段数
    int  errorLine = 0;
    std::string error;
    std::map<Scenario, std::map<std::string, std::string>> byScenario;
};

inline std::string trim_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// 整份接受或整份拒绝（spec §4）
inline TacticsConf parse_tactics_conf(const std::string& text) {
    TacticsConf r;
    r.present = true;
    std::optional<Scenario> cur;
    std::istringstream in(text);
    std::string line;
    int ln = 0;
    while (std::getline(in, line)) {
        ++ln;
        std::string s = trim_ws(line);
        if (!s.empty() && s.back() == '\r') s.pop_back();
        if (s.empty()) continue;
        if (s[0] == '#') continue;
        if (s.size() > 1 && s[0] == '/' && s[1] == '/') continue;
        if (s[0] == '[') {
            size_t close = s.find(']');
            if (close == std::string::npos) { r.error = "段头缺少 ]"; r.errorLine = ln; return r; }
            std::string name = trim_ws(s.substr(1, close - 1));
            auto sc = scenario_from_key(name);
            if (!sc) { r.error = "未知场景: " + name; r.errorLine = ln; return r; }
            cur = sc;
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) { r.error = "缺少 = : " + s; r.errorLine = ln; return r; }
        if (!cur) { r.error = "字段出现在任何 [场景] 之前"; r.errorLine = ln; return r; }
        std::string k = trim_ws(s.substr(0, eq));
        std::string v = trim_ws(s.substr(eq + 1));
        const FieldSpec* fs = find_field(k);
        if (!fs) { r.error = "未知字段: " + k; r.errorLine = ln; return r; }
        if (fs->isText) {
            if (!v.empty() && v != "cached" && v != "service") {
                r.error = "depth 取值非法(空/cached/service): " + v; r.errorLine = ln; return r;
            }
        } else {
            if (v.empty()) { r.error = "空值: " + k; r.errorLine = ln; return r; }
            for (size_t i = 0; i < v.size(); ++i) {
                char c = v[i];
                if (i == 0 && c == '-' && v.size() > 1) continue;
                if (!std::isdigit((unsigned char)c)) {
                    r.error = "非数字值: " + k + "=" + v; r.errorLine = ln; return r;
                }
            }
            long n = std::atol(v.c_str());
            if (k == "maxFreqK" || k == "maxFreqK3" || k == "maxFreqK7" ||
                k == "cpuMinK" || k == "uagSoftK") {
                if (n != -1 && n < 300000) {
                    r.error = k + " 越界(-1 或 >=300000): " + v; r.errorLine = ln; return r;
                }
            } else if (n < fs->lo || n > fs->hi) {
                r.error = "值越界 " + k + "=[" + std::to_string(fs->lo) + "," +
                          std::to_string(fs->hi) + "]: " + v;
                r.errorLine = ln;
                return r;
            }
            if (fs->isBool && !(n == 0 || n == 1)) {
                r.error = "布尔字段须 0/1: " + k + "=" + v; r.errorLine = ln; return r;
            }
        }
        r.byScenario[*cur][k] = v;
        ++r.applied;
    }
    r.ok = true;
    return r;
}

} // namespace uro
