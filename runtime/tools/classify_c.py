import sys
sys.path.insert(0, "/workspace/cos-sched-memory/runtime/tools")
from reconcile import load_events, load_truth_a, dedup, to_cst
s = to_cst("2026-10-07 13:46:00")
truth = [t for t in dedup(load_truth_a("/sdcard/Android/UnifiedRootOptimizer/log/shadow_truth_run4.log")) if t["t"] >= s]
ours = [o for o in dedup(load_events("/sdcard/Android/UnifiedRootOptimizer/log/events.log")) if o["t"] >= s]
WIN = 3.0
used, oi = set(), 0
missed = []
for tv in truth:
    cand = None
    for j in range(oi, len(ours)):
        if j in used:
            continue
        if -WIN <= (ours[j]["t"] - tv["t"]).total_seconds() <= WIN:
            cand = j
            break
    if cand is None:
        missed.append(tv)
    else:
        used.add(cand)
        oi = cand + 1
def nbrs(t):
    i = truth.index(t)
    p = truth[i-1]["t"] if i > 0 else None
    n = truth[i+1]["t"] if i+1 < len(truth) else None
    return p, n
print("C类逐条核查（前后真值都 >=6s 的漏检，前15s 内是否有 systemui）")
for t in missed:
    if t["pkg"] == "com.android.systemui":
        continue
    p, n = nbrs(t)
    if p and (t["t"] - p).total_seconds() < 6:
        continue
    if n and (n - t["t"]).total_seconds() < 6:
        continue
    win = [x for x in truth if 0 <= (t["t"] - x["t"]).total_seconds() <= 15]
    has = any(x["pkg"] == "com.android.systemui" for x in win)
    print("  %s %-22s 前15s含systemui=%s" % (t["t"].strftime("%H:%M:%S"), t["pkg"], has))
    for x in win:
        print("      %s %s" % (x["t"].strftime("%H:%M:%S"), x["pkg"]))
