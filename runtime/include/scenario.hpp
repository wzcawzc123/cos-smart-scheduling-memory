// UnifiedRootOptimizer M3 — 场景模型（设计文档 v1.1 §3.3）
//
// M3 必做场景（§9 表格）：DAILY / GAME（含 Handover）/ POWERSAVE / MEMORY_PRESSURE
// BOOT/VIDEO/THERMAL/SCREEN_OFF 留枚举位，触发判定待 M4（SCREEN_OFF 依赖屏幕源接入）。
#pragma once
#include <string>

namespace uro {

enum class Scenario : uint8_t {
    BOOT, DAILY, GAME, VIDEO, MEMORY_PRESSURE, THERMAL, SCREEN_OFF, POWERSAVE
};

inline const char* scenario_name(Scenario s) {
    switch (s) {
        case Scenario::BOOT:             return "BOOT";
        case Scenario::DAILY:            return "DAILY";
        case Scenario::GAME:             return "GAME";
        case Scenario::VIDEO:            return "VIDEO";
        case Scenario::MEMORY_PRESSURE:  return "MEMORY_PRESSURE";
        case Scenario::THERMAL:          return "THERMAL";
        case Scenario::SCREEN_OFF:       return "SCREEN_OFF";
        case Scenario::POWERSAVE:        return "POWERSAVE";
    }
    return "?";
}

// 优先级（数值大者优先）：GAME 必须绝对压倒其他场景（§5.5 全量让权，不协商）。
inline int scenario_priority(Scenario s) {
    switch (s) {
        case Scenario::GAME:            return 100;  // 让权最高
        case Scenario::MEMORY_PRESSURE: return 70;   // 压力需要立即处置
        case Scenario::THERMAL:         return 60;
        case Scenario::SCREEN_OFF:      return 50;
        case Scenario::POWERSAVE:       return 40;
        case Scenario::VIDEO:           return 30;
        case Scenario::BOOT:            return 20;
        case Scenario::DAILY:           return 10;
    }
    return 0;
}

inline bool scenario_more_urgent(Scenario a, Scenario b) {
    return scenario_priority(a) > scenario_priority(b);
}

} // namespace uro
