// UnifiedRootOptimizer — 四档调度模型（v0.11 起，用户拍板：四档自治、不依赖系统设置）
//
// 结构（两档位 + 两个正交态）：
//   档位（输出，决定参数包）：POWER_SAVE / BALANCE / PERFORMANCE / FAST
//   撤权态：GAME         —— 打游戏时 URO 整体撤出（§5.5，不是档位，方向相反）
//   叠加态：MEMORY_PRESSURE —— 内存压力回收叠加在任意档位上（滞回退出后回当前档）
//
// 判定信号（全为 URO 自采事件，不读系统 mode.txt）：
//   熄屏→POWER_SAVE；亮屏默认→BALANCE；(PERFORMANCE/FAST 输入源待第2步：Touch/AmSwitch)
//
// 版本沿革：v0.10 及以前为场景模型（DAILY/POWERSAVE/…），v0.11 收敛为四档。
#pragma once
#include <string>

namespace uro {

enum class Scenario : uint8_t {
    BOOT, BALANCE, POWER_SAVE, PERFORMANCE, FAST,
    GAME, MEMORY_PRESSURE
    // 保留位（M5/T-OBS 用）：THERMAL、VIDEO
};

inline const char* scenario_name(Scenario s) {
    switch (s) {
        case Scenario::BOOT:             return "BOOT";
        case Scenario::BALANCE:          return "BALANCE";
        case Scenario::POWER_SAVE:       return "POWER_SAVE";
        case Scenario::PERFORMANCE:      return "PERFORMANCE";
        case Scenario::FAST:             return "FAST";
        case Scenario::GAME:             return "GAME";
        case Scenario::MEMORY_PRESSURE:  return "MEMORY_PRESSURE";
    }
    return "?";
}

// 优先级（数值大者优先）：GAME 必须绝对压倒其他（§5.5 全量让权，不协商）。
inline int scenario_priority(Scenario s) {
    switch (s) {
        case Scenario::GAME:            return 100;  // 让权最高
        case Scenario::MEMORY_PRESSURE: return 70;   // 压力立即处置
        case Scenario::BOOT:            return 20;
        case Scenario::POWER_SAVE:      return 15;
        case Scenario::FAST:            return 14;
        case Scenario::PERFORMANCE:     return 13;
        case Scenario::BALANCE:         return 10;
    }
    return 0;
}

inline bool scenario_more_urgent(Scenario a, Scenario b) {
    return scenario_priority(a) > scenario_priority(b);
}

// 是否为四档之一（档位态）；GAME/MEMORY_PRESSURE/BOOT 属正交态或启动态
inline bool is_level(Scenario s) {
    return s == Scenario::POWER_SAVE || s == Scenario::BALANCE ||
           s == Scenario::PERFORMANCE || s == Scenario::FAST;
}

} // namespace uro
