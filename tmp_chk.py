#!/usr/bin/env python3
import json, datetime
rows = []
for l in open('/sdcard/pw.txt', encoding='utf-8'):
    l = l.strip()
    if not l: continue
    try: rows.append(json.loads(l))
    except Exception: pass
def hm(t): return datetime.datetime.fromtimestamp(t/1000 + 8*3600).strftime('%H:%M')
print('power rows', len(rows), '| span', hm(rows[0]['ts']), '->', hm(rows[-1]['ts']))
# 放电段（连续 Discharging，>=8min 计入）
segs, cur = [], None
for r in rows:
    k = 'D' if r.get('chg') == 'Discharging' else 'C'
    if cur and cur[0] == k: cur[1].append(r)
    else:
        if cur: segs.append(cur)
        cur = [k, [r]]
if cur: segs.append(cur)
tot = 0
for k, rs in segs:
    dur = (rs[-1]['ts'] - rs[0]['ts']) / 3.6e6
    if k == 'D' and dur >= 0.13:
        rate = (rs[0]['pct'] - rs[-1]['pct']) / dur
        tot += dur
        print('  D %s->%s %.2fh %d%%->%d%% %.1f%%/h' % (hm(rs[0]['ts']), hm(rs[-1]['ts']), dur, rs[0]['pct'], rs[-1]['pct'], rate))
    elif k == 'C' and dur >= 0.13:
        print('  C %s->%s %.2fh %d%%->%d%%' % (hm(rs[0]['ts']), hm(rs[-1]['ts']), dur, rs[0]['pct'], rs[-1]['pct']))
print('有效放电累计: %.2f h' % tot)

# 桥健康：telemetry 最近 5 条含 memory/reclaim 的 ctrl + 最近20条概况
tl = []
for l in open('/sdcard/tl.txt', encoding='utf-8'):
    l = l.strip()
    if not l: continue
    try: tl.append(json.loads(l))
    except Exception: pass
print('\n[bridge] 最后8条 telemetry:')
for r in tl[-8:]:
    ctrl = ','.join(r.get('ctrl', []))[:60]
    print('  %s %-13s applied=%s why=%-28s ctrl=%s' % (
        hm(r['ts']), r['scenario'], r.get('applied'), r.get('why', '')[:28], ctrl))
