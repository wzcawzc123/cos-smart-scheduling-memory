#!/usr/bin/env python3
B = '/workspace/cos-sched-memory/runtime/'

# 1) evidence.cpp: read_battery_state
p = B + 'src/evidence.cpp'
s = open(p, encoding='utf-8').read()
a = 'std::string battery_json(const std::string& psRoot) {'
assert a in s
s = s.replace(a, '''// L4 电量守卫输入（Evidence 回喂）：pct + 充电态
void read_battery_state(const std::string& psRoot, int& pct, bool& charging) {
    pct = -1; charging = false;
    std::ifstream fc(psRoot + "/battery/capacity");
    if (fc.is_open()) fc >> pct;
    std::ifstream fs(psRoot + "/battery/status");
    std::string st;
    if (fs.is_open()) fs >> st;
    charging = (st != "Discharging" && !st.empty());
}

''' + a, 1)
open(p, 'w', encoding='utf-8').write(s)
print('evidence OK')

# 2) hpp 声明
p = B + 'include/drivers.hpp'
h = open(p, encoding='utf-8').read()
a2 = 'std::string gpu_json(const std::string& kgslRoot);'
assert a2 in h
h = h.replace(a2, a2 + '\nvoid read_battery_state(const std::string& psRoot, int& pct, bool& charging);', 1)
open(p, 'w', encoding='utf-8').write(h)
print('hpp OK')

# 3) state.hpp: batteryPct + ChargerChanged 扩展（兼容旧纯字符串 payload）
p = B + 'include/state.hpp'
st = open(p, encoding='utf-8').read()
a3 = 'int     thermalLevel = 0;'
assert a3 in st
st = st.replace(a3, 'int     batteryPct = -1;     // L4 电量守卫输入（-1=未采）\n    ' + a3, 1)
old4 = '''            case EventType::ChargerChanged:
                if (st.charging != (e.payload != "Discharging")) { st.charging = (e.payload != "Discharging"); changed = true; }
                break;'''
assert old4 in st, 'charger case'
st = st.replace(old4, '''            case EventType::ChargerChanged: {
                // payload: "Charging" 或 "Charging:43"（状态[:电量]，兼容旧格式）
                std::string s = e.payload;
                int pct = -1;
                size_t c = s.find(':');
                if (c != std::string::npos) { pct = std::atoi(s.substr(c + 1).c_str()); s = s.substr(0, c); }
                bool chg = (s != "Discharging" && !s.empty());
                if (st.charging != chg) { st.charging = chg; changed = true; }
                if (pct >= 0 && pct != st.batteryPct) { st.batteryPct = pct; changed = true; }
                break;
            }''', 1)
open(p, 'w', encoding='utf-8').write(st)
print('state OK')

# 4) drivers.cpp: evidence_driver 电量检测块（thermal 块后）
p = B + 'src/drivers.cpp'
d = open(p, encoding='utf-8').read()
a5 = '''            // GPU 观测（v0.21，只读；idle_timer 干预待 GpuController 实装）'''
assert a5 in d
d = d.replace(a5, '''            // L4 电量守卫输入（Evidence 回喂）：pct 变化≥2 或充电态变化 → 推 ChargerChanged
            {
                static int lastPct = -2; static int lastChg = -1;
                int pct; bool chg;
                read_battery_state("/sys/class/power_supply", pct, chg);
                if (pct >= 0 && (lastPct < 0 || abs(pct - lastPct) >= 2 || (int)chg != lastChg)) {
                    if (lastPct >= 0 && abs(pct - lastPct) < 2 && (int)chg != lastChg)
                        {} // 仅充电态变化也推送
                    lastPct = pct; lastChg = (int)chg;
                    q.push(Event{EventType::ChargerChanged, now_ms(), 0, "battery",
                                 (chg ? std::string("Charging") : std::string("Discharging")) + ":" + std::to_string(pct)});
                }
            }
''' + a5, 1)
open(p, 'w', encoding='utf-8').write(d)
print('drivers OK')

# 5) policy_manager.hpp: 电量守卫夹档 + 审计字段
p = B + 'include/policy_manager.hpp'
pm = open(p, encoding='utf-8').read()
a6 = '''        // Thermal 夹档（T-OBS→干预）'''
assert a6 in pm
pm = pm.replace(a6, '''        // L4 电量守卫（Evidence 回喂）：低电+放电 → 收性能；充电中不干预；GAME 让权
        {
            bool pgOn = true; int pgPct = 20;
            { std::ifstream f("/sdcard/Android/UnifiedRootOptimizer/uro.conf"); std::string ln;
              while (std::getline(f, ln)) {
                  if (ln.rfind("POWER_GUARD=0", 0) == 0) pgOn = false;
                  else if (ln.rfind("POWER_GUARD_PCT=", 0) == 0) pgPct = std::atoi(ln.c_str() + 16);
              } }
            if (pgOn && st.batteryPct >= 0 && !st.charging && st.batteryPct <= pgPct &&
                cand != Scenario::GAME && cand != Scenario::POWER_SAVE) {
                Scenario capped = (st.batteryPct <= 10) ? Scenario::POWER_SAVE
                    : ((cand == Scenario::FAST || cand == Scenario::PERFORMANCE)
                           ? Scenario::BALANCE : cand);
                if (capped != cand) { cand = capped; why_ = "power-guard " + why_; pGuardCapped_ = true; }
            }
        }

''' + a6, 1)
old7 = '''        d.thermalCapped = thermalCapped_; thermalCapped_ = false;'''
assert old7 in pm
pm = pm.replace(old7, '''        d.thermalCapped = thermalCapped_; thermalCapped_ = false;
        d.powerCapped = pGuardCapped_; pGuardCapped_ = false;''', 1)
old8 = 'bool thermalCapped_ = false;'
assert old8 in pm
pm = pm.replace(old8, old8 + '\n    bool pGuardCapped_ = false;   // 电量守卫夹档边沿', 1)
old9 = '    bool thermalCapped = false;   // 本轮被 Thermal 夹档（审计用，v0.20）'
assert old9 in pm
pm = pm.replace(old9, old9 + '\n    bool powerCapped = false;     // 本轮被电量守卫夹档（审计用，v0.23）', 1)
open(p, 'w', encoding='utf-8').write(pm)
print('pm OK')

# 6) main.cpp: 审计日志
p = B + 'src/main.cpp'
mn = open(p, encoding='utf-8').read()
a10 = '''        if (dec.thermalCapped)'''
assert a10 in mn
mn = mn.replace(a10, '''        if (dec.powerCapped)
            elog.line(std::string(ts) + " POWER-GUARD-CAPPED scenario=" + scenario_name(dec.scenario) +
                      " pct=" + std::to_string(st.batteryPct) + " fg=" + st.foregroundPackage);
''' + a10, 1)
open(p, 'w', encoding='utf-8').write(mn)
print('main OK')

# 7) 测试（m3）
p = B + 'tests/test_m3.cpp'
t = open(p, encoding='utf-8').read()
r = t.rfind('return')
assert r > 0
t = t[:r] + '''    // ---- [power-guard] L4 电量守卫（Evidence 回喂）夹档 ----
    {
        using namespace uro;
        PolicyManager pm3;
        Event e{EventType::TouchChanged, 1000, 1, "t", ""};
        GlobalState st3; st3.screenOn = true; st3.foregroundPackage = "com.x";
        st3.batteryPct = 50;        // 高电：PERF 正常
        auto d1 = pm3.decide(st3, e, 1000);
        st3.batteryPct = 15;        // 低电放电：PERF -> BALANCE
        auto d2 = pm3.decide(st3, e, 2000);
        CHECK(d2.scenario == Scenario::BALANCE, "低电15%夹PERF->BALANCE");
        CHECK(d2.why.find("power-guard") != std::string::npos, "why标注power-guard");
        st3.batteryPct = 8;         // 极低：POWER_SAVE
        auto d3 = pm3.decide(st3, e, 3000);
        CHECK(d3.scenario == Scenario::POWER_SAVE, "极低8%->POWER_SAVE");
        st3.batteryPct = 15; st3.charging = true;   // 充电中不干预
        auto d4 = pm3.decide(st3, e, 4000);
        CHECK(d4.scenario == Scenario::PERFORMANCE, "充电中不夹");
        std::printf("[power-guard] ok (15%夹/8%PS/充电豁免/why)\\n");
    }

''' + t[r:]
open(p, 'w', encoding='utf-8').write(t)
print('m3 OK')
