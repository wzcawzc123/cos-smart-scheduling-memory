#!/usr/bin/env python3
# L5 只读洞察：从已有 Evidence 聚合规律（不改任何策略）
import json, collections, glob, os, time

L = '/sdcard/Android/UnifiedRootOptimizer/log'

def rows(path, limit=None):
    out = []
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.strip()
                if not line.startswith('{'): continue
                try: out.append(json.loads(line))
                except Exception: pass
    except FileNotFoundError:
        pass
    return out[-limit:] if limit else out

print('=== L5 只读洞察报告 ===')
tel = rows(f'{L}/telemetry.jsonl')
print(f'telemetry: {len(tel)} 条')
if tel:
    t0, t1 = tel[0].get('ts', 0), tel[-1].get('ts', 0)
    print(f'  时间范围: {time.strftime("%m-%d %H:%M", time.localtime(t0))} → {time.strftime("%m-%d %H:%M", time.localtime(t1))}'
          f'（{round((t1-t0)/3600, 1)} 小时）')

    # 档位分布
    sc = collections.Counter(r.get('scenario', '?') for r in tel)
    total = sum(sc.values()) or 1
    print('\n[档位分布]')
    for k, v in sc.most_common():
        print(f'  {k:<18} {v:>5} 条  {v*100/total:5.1f}%')

    # Top 前台包
    fg = collections.Counter(r.get('fg', '?') for r in tel if r.get('fg') and r['fg'] != '(unknown)')
    print('\n[Top 前台包（按决策条数≈使用时长权重）]')
    for k, v in fg.most_common(8):
        print(f'  {k:<38} {v:>5} 条')

    # why 触发分布（热/电/游戏）
    why = collections.Counter()
    for r in tel:
        w = r.get('why', '')
        for tag in ['thermal', 'power-guard', 'game', 'touch', 'app-switch', 'fg=', 'pressure', 'boss']:
            if tag in w: why[tag] += 1
    print('\n[决策原因（含关键词计数）]')
    for k, v in why.most_common():
        print(f'  {k:<14} {v:>5}')

# fps 哨兵与 jank
fps = rows(f'{L}/fps.jsonl')
if fps:
    sen = sum(1 for r in fps if r.get('sentry'))
    jl = []
    for r in fps:
        fr, jk = r.get('frames', 0), r.get('janky', 0)
        if fr and fr > 30: jl.append(jk * 100 / fr)
    print(f'\n[fps] {len(fps)} 窗口 · sentry 标记 {sen} 次')
    if jl:
        jl.sort()
        print(f'  jank 率: 中位 {jl[len(jl)//2]:.1f}% · P90 {jl[int(len(jl)*0.9)]:.1f}% · 最差 {jl[-1]:.1f}%')

# thermal
th = rows(f'{L}/thermal.jsonl')
if th:
    temps = [r['top'][0]['c'] for r in th if r.get('top')]
    pcts = [r['pct'] for r in th if r.get('pct', -1) >= 0]
    if temps:
        temps.sort()
        print(f'\n[thermal] {len(th)} 采样 · top1 温度: 中位 {temps[len(temps)//2]}° · P95 {temps[int(len(temps)*0.95)]}° · 峰值 {temps[-1]}°')
    if pcts:
        print(f'  电量采样 {len(pcts)} 次 · 当前 {pcts[-1]}%')

# gpu
gpu = rows(f'{L}/gpu.jsonl')
if gpu:
    b = [r['gpuBusy'] for r in gpu if r.get('gpuBusy', -1) >= 0]
    if b:
        b.sort()
        print(f'\n[gpu] {len(gpu)} 采样 · busy 中位 {b[len(b)//2]}% · P95 {b[int(len(b)*0.95)]}%')

# events 触发计数
print('\n[审计事件计数]')
try:
    ev = open(f'{L}/events.log', encoding='utf-8', errors='replace').read()
    for k in ['THERMAL-CAPPED', 'POWER-GUARD-CAPPED', 'ThermalChanged', 'ChargerChanged']:
        print(f'  {k:<22} {ev.count(k)} 次')
except FileNotFoundError:
    print('  events.log 不存在')
