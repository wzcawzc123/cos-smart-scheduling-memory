#!/usr/bin/env python3
"""M1 影子对账脚本 v2 — resolver(ResumedActivity) 语义验收

用法: python3 reconcile.py [--start "YYYY-MM-DD HH:MM:SS"] [--end "..."] [--win 3.0]

数据源（均设备 CST 本地时间）:
  events.log      URO 事件日志: "YYYY-MM-DD HH:MM:SS ForegroundChanged gen=... src=... data=pkg"
  truth A         shadow_truth*.log: logcat epoch 行 "  <epoch> pid tid I UserAwareMgr: handleTopAppChanged: pkg, ..."
  truth B         shadow_focus*.log: "YYYY-MM-DD HH:MM:SS focus=pkg/act"（mCurrentFocus 轮询，交叉真值）

对账方法（与 v1 报告一致）: 真值事件 → 在 (0, win] 秒后找我方最近的 ForegroundChanged
  matched: 时间窗内有我方事件; 包名一致 = 两者 pkg 相同
  spurious: 我方事件在真值序列中找不到对应
"""
import argparse, re, sys, datetime as dt

TZ = dt.timezone(dt.timedelta(hours=8))  # 设备 CST，显式（教训#8）

def to_cst(s):  # "YYYY-MM-DD HH:MM:SS" -> aware datetime CST
    return dt.datetime.strptime(s, "%Y-%m-%d %H:%M:%S").replace(tzinfo=TZ)

def load_events(path, types=("ForegroundChanged",)):
    out = []
    pat = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+(\S+)\s+gen=(\d+)(\.\d+)?\s+src=(\S+)\s+data=(\S+)")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = pat.match(line.strip())
        if not m:
            continue
        if m.group(2) not in types:
            continue
        out.append({"t": to_cst(m.group(1)), "type": m.group(2),
                    "gen": int(m.group(3)), "src": m.group(5), "pkg": m.group(6)})
    return out

def load_truth_a(path):
    """logcat epoch 行 → handleTopAppChanged 事件"""
    out = []
    pat = re.compile(r"^\s*(\d{10}\.\d{3})\s+\d+\s+\d+\s+\w+\s+UserAwareMgr:\s+handleTopAppChanged:\s*([^,\s]+)")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = pat.match(line)
        if m:
            ts = dt.datetime.fromtimestamp(float(m.group(1)), TZ)
            out.append({"t": ts, "pkg": m.group(2)})
    return out

def load_truth_b(path):
    out = []
    pat = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+focus=([^/\s]+)")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = pat.match(line.strip())
        if m and m.group(2) != "focus-poll":
            out.append({"t": to_cst(m.group(1)), "pkg": m.group(2)})
    return out

def dedup(seq):
    """合并同一包名的连续重复（防抖前的抖动）"""
    out = []
    for e in seq:
        if not out or out[-1]["pkg"] != e["pkg"]:
            out.append(e)
    return out

def reconcile(truth, ours, win):
    """对称时间窗 ±win + 单调顺序配对（防交叉错配；同秒事件也能配对）"""
    matched, missed, pkg_ok, delays = [], [], 0, []
    used = set()
    oi = 0
    for tv in truth:
        # 从上次配对位置之后找第一个落在窗内的我方事件（保持时间单调）
        cand = None
        for j in range(oi, len(ours)):
            if j in used:
                continue
            d = (ours[j]["t"] - tv["t"]).total_seconds()
            if -win <= d <= win:
                cand = j
                break
        if cand is None:
            missed.append(tv)
            continue
        ov = ours[cand]
        used.add(cand)
        oi = cand + 1
        d = (ov["t"] - tv["t"]).total_seconds()
        delays.append(d)
        ok = ov["pkg"] == tv["pkg"]
        if ok:
            pkg_ok += 1
        matched.append({"truth": tv, "ours": ov, "delay": d, "pkg_ok": ok})
    spurious = [ov for i, ov in enumerate(ours) if i not in used]
    return matched, missed, spurious, pkg_ok, delays

def win_range(start, end):
    if start:
        s = to_cst(start)
        ours_all = None
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--events", default="/sdcard/Android/UnifiedRootOptimizer/log/events.log")
    ap.add_argument("--truth-a", default="/sdcard/Android/UnifiedRootOptimizer/log/shadow_truth_run4.log")
    ap.add_argument("--truth-b", default="/sdcard/Android/UnifiedRootOptimizer/log/shadow_focus_run4.log")
    ap.add_argument("--start", help="窗口起点 YYYY-MM-DD HH:MM:SS（设备本地）")
    ap.add_argument("--end", help="窗口终点")
    ap.add_argument("--win", type=float, default=3.0, help="匹配时间窗秒数，默认 3.0")
    ap.add_argument("--truth", choices=["a", "b"], default="a", help="主真值源")
    ap.add_argument("--trace", action="store_true", help="打印并排时序")
    args = ap.parse_args()

    ours = load_events(args.events)
    ta, tb = load_truth_a(args.truth_a), load_truth_b(args.truth_b)
    truth = ta if args.truth == "a" else tb

    s = to_cst(args.start) if args.start else None
    e = to_cst(args.end) if args.end else None
    inwin = lambda t: (not s or t >= s) and (not e or t <= e)
    ours = [o for o in ours if inwin(o["t"])]
    truth = [t for t in truth if inwin(t["t"])]
    truth_d, ours_d = dedup(truth), dedup(ours)

    print(f"窗口: {args.start or '起点'} ~ {args.end or '终点'}  匹配窗={args.win}s  主真值=truth-{args.truth.upper()}")
    print(f"原始: 真值 {len(truth)} / 我方 {len(ours)}   去重后: 真值 {len(truth_d)} / 我方 {len(ours_d)}")
    if not truth_d or not ours_d:
        print("!! 无数据"); sys.exit(2)

    m, miss, spur, pok, delays = reconcile(truth_d, ours_d, args.win)
    n = len(m)
    if args.trace:
        print("\n--- 并排时序 (真值A/B vs 我方) ---")
        all_ev = ([("真值", t["t"], t["pkg"]) for t in truth_d] +
                  [("我方", o["t"], o["pkg"]) for o in ours_d] +
                  [("真值B", t["t"], t["pkg"]) for t in dedup(load_truth_b(args.truth_b)) if inwin(t["t"])])
        for kind, t, p in sorted(all_ev, key=lambda x: x[1]):
            print(f"  {t:%H:%M:%S}.{t.microsecond//1000:03d} {kind:<5} {p}")
    print(f"\n--- 前台事件等价性 ---")
    print(f"时间窗匹配: {n}/{len(truth_d)} ({100*n/len(truth_d):.0f}%)   missed={len(miss)}   spurious={len(spur)}")
    if delays:
        print(f"时延(我-真值): avg {sum(delays)/len(delays):.2f}s / min {min(delays):.2f} / max {max(delays):.2f}")
    if n:
        print(f"包名一致: {pok}/{n} ({100*pok/n:.0f}%)")
    if miss:
        print(f"\nmissed ({len(miss)}):")
        for t in miss[:20]:
            print(f"  {t['t']:%H:%M:%S} {t['pkg']}")
    if spur:
        print(f"\nspurious ({len(spur)}):")
        for o in spur[:20]:
            print(f"  {o['t']:%H:%M:%S} {o['pkg']} ({o['src']})")
    bad = [x for x in m if not x["pkg_ok"]]
    if bad:
        print(f"\n包名不一致 ({len(bad)}):")
        for x in bad[:20]:
            print(f"  {x['truth']['t']:%H:%M:%S} 真值={x['truth']['pkg']} 我方={x['ours']['pkg']} Δ={x['delay']:.2f}s")

if __name__ == "__main__":
    main()
