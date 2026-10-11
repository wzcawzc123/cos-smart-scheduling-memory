// UnifiedRootOptimizer M1 — StateManager (设计文档 v1.1 §3.2/§4.4)
#pragma once
#include "event.hpp"
#include <string>
#include <cstdio>

namespace uro {

struct GlobalState {
    std::string foregroundPackage = "(unknown)";
    bool  screenOn      = true;
    bool  charging      = false;
    float psiCpu10      = 0.f;   // PSI cpu some avg10
    float psiMem10      = 0.f;
    long  memAvailMb    = -1;
    std::string mode    = "(none)";   // 情景模式
    int     thermalLevel = 0;   // 0=正常 1=高温(≤BALANCE) 2=过热(≤POWER_SAVE)（T-OBS→干预）
    uint64_t generation = 0;
    uint64_t ts_ms      = 0;
};

class StateManager {
public:
    // 应用事件；状态确实变化时 generation++ 并返回 true
    bool apply(const Event& e, GlobalState& st) {
        bool changed = false;
        switch (e.type) {
            case EventType::ForegroundChanged:
                if (st.foregroundPackage != e.payload) { st.foregroundPackage = e.payload; changed = true; }
                break;
            case EventType::ScreenChanged:
                if (st.screenOn != (e.payload == "on")) { st.screenOn = (e.payload == "on"); changed = true; }
                break;
                        case EventType::ThermalChanged: {
                // payload="level:temp"（evidence_driver 边沿推送）
                int lv = 0;
                size_t c = e.payload.find(':');
                if (c != std::string::npos) lv = std::atoi(e.payload.substr(0, c).c_str());
                if (lv < 0) lv = 0; if (lv > 2) lv = 2;
                if (lv != st.thermalLevel) { st.thermalLevel = lv; changed = true; }
                break;
            }
case EventType::ChargerChanged:
                if (st.charging != (e.payload != "Discharging")) { st.charging = (e.payload != "Discharging"); changed = true; }
                break;
            case EventType::ModeChanged:
                if (st.mode != e.payload) { st.mode = e.payload; changed = true; }
                break;
            case EventType::MemoryPressureChanged: {
                float p = 0.f; long m = -1;
                sscanf(e.payload.c_str(), "psi=%f memAvailMb=%ld", &p, &m);
                bool hi = (p >= 5.0f) || (m >= 0 && m < 1024);
                st.psiMem10 = p; st.memAvailMb = m;
                changed = hi;  // 越阈值才算状态跃迁
                break;
            }
            default:
                changed = false;   // 信息类事件不改状态
                break;
        }
        if (changed) st.generation++;
        return changed;
    }
};

} // namespace uro
