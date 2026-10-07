#!/usr/bin/env python3
"""missed/spurious 分类定性"""
import sys
sys.path.insert(0, "/workspace/cos-sched-memory/runtime/tools")
from reconcile import load_events, load_truth_a, load_truth_b, dedup, to_cst, TZ

WIN = 3.0
s = to_cst("2026-10-07 13:46:00")
truth = [t for t in dedup(load_truth_a("/sdcard/Android/UnifiedRootOptimizer/log/shadow_truth_run4.log")) if t["t"] >= s]
ours = [o for o in dedup(load_events("/sdcard/Android/UnifiedRootOptimizer/log/events.log")) if o["t"] >= s]

used, oi = set(), 0
matched, missed = [], []
for tv in truth:
    cand = None
    for j in range(oi, len(ours)):
        if j in used: continue
        if -WIN <= (ours[j]["t"] - tv["t"]).total_seconds() <= WIN:
            cand = j; break
    if cand is None:
        missed.append(tv)
    else:
        used.add(cand); oi = cand + 1
        matched.append((tv, ours[cand], (ours[cand]["t"] - tv["t"]).total_seconds()))
spurious = [o for i, o in enumerate(ours) if i not in used]

def neighbors(t):
    i = truth.index(t)
    prev = truth[i-1]["t"] if i > 0 else None
    nxt = truth[i+1]["t"] if i + 1 < len(truth) else None
    return ((t["t"] - prev).total_seconds() if prev else 999,
            (nxt - t["t"]).total_seconds() if nxt else 999)

cat = {}
for t in missed:
    gp, gn = neighbors(t)
    if t["pkg"] == "com.android.systemui":
        k = "A. systemui 盲区（cgroup 无变化）"
    elif gp < 6 or gn < 6:
        k = "B. 快速切换序列（前后真值<6s，防抖坍缩）"
    else:
        k = "C. 单独事件漏检（需人工核）"
    cat.setdefault(k, []).append(t)

print(f"真值 {len(truth)} / 我方 {len(ours)} / matched {len(matched)} / missed {len(missed)} / spurious {len(spurious)}")
print("\n=== missed 分类 ===")
for k in sorted(cat):
    print(f"{k}: {len(cat[k])}")
    if k.startswith("C"):
        for t in cat[k]:
            gp, gn = neighbors(t)
            print(f"    {t['t']:%H:%M:%S} {t['pkg']} (前{gp:.0f}s/后{gn:.0f}s)")

print("\n=== spurious 分组 ===")
sp = {}
for o in spurious: sp.setdefault(o["pkg"], []).append(o)
for p, lst in sorted(sp.items(), key=lambda x: -len(x[1])):
    print(f"  {p}: {len(lst)}  " + ", ".join(f"{o['t']:%H:%M:%S}({o['src']})" for o in lst))

print("\n=== 包名不一致明细 ===")
bad = [(t, o, d) for t, o, d in matched if t["pkg"] != o["pkg"]]
print(f"共 {len(bad)}")
for t, o, d in bad:
    print(f"    {t['t']:%H:%M:%S} 真值={t['pkg']:<26} 我方={o['pkg']:<26} Δ={d:+.2f}s")

print("\n=== 状态等价口径 ===")
def seq_at(seq, t):
    cur = None
    for e in seq:
        if e["t"] <= t: cur = e["pkg"]
        else: break
    return cur
all_t = sorted([e["t"] for e in truth] + [e["t"] for e in ours])
agree = disagree = 0
first_dis = None
for t in all_t:
    a, b = seq_at(truth, t), seq_at(ours, t)
    if a is None or b is None: continue
    if a == b: agree += 1
    else:
        disagree += 1
        if first_dis is None: first_dis = (t, a, b)
print(f"状态采样点一致 {agree} / 不一致 {disagree} ({100*agree/max(1,agree+disagree):.1f}%)")
if first_dis: print(f"首个不一致: {first_dis[0]:%H:%M:%S} 真值={first_dis[1]} 我方={first_dis[2]}")
print(f"末态: 真值={truth[-1]['pkg']} 我方={ours[-1]['pkg']}")
