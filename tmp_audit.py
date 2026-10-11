#!/usr/bin/env python3
B = '/workspace/cos-sched-memory/runtime/'

# 1) Decision 加 thermalCapped 字段
p = B + 'include/policy.hpp'
s = open(p, encoding='utf-8').read()
import re
m = re.search(r'(struct Decision \{)', s)
assert m, 'Decision'
# 找 Decision 内已有字段如 why/generation，插在结尾前——用 why 行做锚
anchor = None
for cand in ['std::string why;', 'bool changed = false;', 'uint64_t leaseUntilMs']:
    if cand in s: anchor = cand; break
assert anchor, 'field anchor: ' + str([c for c in ['why','changed','lease'] if c in s])
s = s.replace(anchor, anchor + '\n    bool thermalCapped = false;   // 本轮被 Thermal 夹档（审计用，v0.20）', 1)
open(p, 'w', encoding='utf-8').write(s)
print('policy.hpp OK, anchor=', anchor)

# 2) decide 夹档时置位
p = B + 'include/policy_manager.hpp'
pm = open(p, encoding='utf-8').read()
old = 'if (capped != cand) { cand = capped; why_ = "thermal-capped " + why_; }'
assert old in pm, 'clamp'
pm = pm.replace(old, 'if (capped != cand) { cand = capped; why_ = "thermal-capped " + why_; thermalCapped_ = true; }', 1)
# Decision 构造处回填字段（d.scenario 赋值后）
old2 = '''        Decision d;
        d.scenario = cand;
        d.tactics = tactics_for(cand, st);'''
assert old2 in pm
pm = pm.replace(old2, '''        Decision d;
        d.scenario = cand;
        d.thermalCapped = thermalCapped_; thermalCapped_ = false;
        d.tactics = tactics_for(cand, st);''', 1)
# 成员声明
old3 = 'std::string profilePath_;'
assert old3 in pm
pm = pm.replace(old3, 'bool thermalCapped_ = false;   // 夹档边沿（decide 内清零）\n    ' + old3, 1)
open(p, 'w', encoding='utf-8').write(pm)
print('pm OK')

# 3) main run_policy：夹档时写 events.log（plog SCENARIO 行后）
p = B + 'src/main.cpp'
m = open(p, encoding='utf-8').read()
old4 = '''        if (dec.tactics.handover) {'''
assert old4 in m
m = m.replace(old4, '''        if (dec.thermalCapped)
            elog.line(std::string(ts) + " THERMAL-CAPPED scenario=" + scenario_name(dec.scenario) +
                      " level=" + std::to_string(st.thermalLevel) + " fg=" + st.foregroundPackage);
        if (dec.tactics.handover) {''', 1)
open(p, 'w', encoding='utf-8').write(m)
print('main OK')
