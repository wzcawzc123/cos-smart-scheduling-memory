import pathlib
p = pathlib.Path('/workspace/cos-sched-memory/module/webroot/app.js')
s = p.read_text(encoding='utf-8')
n0 = len(s)
rep = []

# ① bridge.state / cpu.state 是多行文件，必须走 rd()
rep.append(("rd(CMDT.prop), rd(CMDT.conf), sh(CMDT.bridge), sh(CMDT.cpus), sh(CMDT.hb), rd(CMDT.last), sh(CMDT.mode)",
            "rd(CMDT.prop), rd(CMDT.conf), rd(CMDT.bridge), rd(CMDT.cpus), sh(CMDT.hb), rd(CMDT.last), sh(CMDT.mode)"))

# ② 副标题措辞
rep.append(("+ (s.sinceMs ? ' · 当前档 ' + fmtDur(Date.now() - s.sinceMs) : '');",
            "+ (s.sinceMs ? ' · 最近决策 ' + fmtDur(Date.now() - s.sinceMs) + ' 前' : '');"))

# ③ uPlot 时间轴按秒 + 双 Y 轴
rep.append(("  function buildLines(el, height, xs, series, opt) {",
            "  // uPlot 的 time 刻度按 Unix 秒解释（不是毫秒）→ 前端统一换算\n"
            "  function xsec(ms) { return Number(ms) / 1000; }\n"
            "  function buildLines(el, height, xs, series, opt) {"))

rep.append(("""    var ax = axisStyle(), s = [{ label: '时间' }], i;
    for (i = 0; i < series.length; i++) {
      s.push({
        label: series[i].label, stroke: series[i].color, width: series[i].w || 2,
        fill: series[i].fill ? hexA(series[i].color, series[i].fill) : undefined,
        points: { show: false }, spanGaps: true
      });
    }""",
"""    var ax = axisStyle(), s = [{ label: '时间' }], i, dual = false;
    for (i = 0; i < series.length; i++) {
      var it = series[i];
      if (it.axis === 2) dual = true;
      s.push({
        label: it.label, stroke: it.color, width: it.w || 2,
        fill: it.fill ? hexA(it.color, it.fill) : undefined,
        points: { show: false }, spanGaps: true,
        scale: it.axis === 2 ? 'y2' : 'y'
      });
    }
    var axes = [
      Object.assign({}, ax, { space: opt.space || 46 }),
      Object.assign({}, ax, { size: 40, space: 30 })
    ];
    if (dual) axes.push(Object.assign({}, ax, { scale: 'y2', side: 1, size: 44, space: 30 }));
    var scales = { x: { time: true }, y: {}, y2: {} };"""))

rep.append(("""      scales: { x: { time: true }, y: opt.pct ? { range: [0, 100] } : {} },
      axes: [
        Object.assign({}, ax, { space: opt.space || 46 }),
        Object.assign({}, ax, { size: 34, space: 30 })
      ],
      series: s""",
"""      scales: scales, axes: axes, series: s"""))

# ④ 各图：x 换秒 + 第二条序列上副轴
rep.append(("""{ key: 'c-temp', page: 'trend', h: 168, data: function () { var t = trend.value.temp; return [t.map(function (x) { return normTs(x[0]); }), t.map(function (x) { return x[1]; })]; }, series: [{ label: '电池温度 °C', color: '--red', fill: .16 }] },""",
"""{ key: 'c-temp', page: 'trend', h: 168, data: function () { var t = trend.value.temp; return [t.map(function (x) { return xsec(normTs(x[0])); }), t.map(function (x) { return x[1]; })]; }, series: [{ label: '电池温度 °C', color: '--red', fill: .16 }] },"""))

rep.append(("""            return [f.map(function (x) { return normTs(x[0]); }), f.map(function (x) { return pctOf(x[2], x[1]); }), f.map(function (x) { return x[1]; })];
          }, series: [{ label: '卡顿率 %', color: '--amber', fill: .14 }, { label: '帧数/分', color: '--blue', w: 1.4 }]""",
"""            return [f.map(function (x) { return xsec(normTs(x[0])); }), f.map(function (x) { return pctOf(x[2], x[1]); }), f.map(function (x) { return x[1]; })];
          }, series: [{ label: '卡顿率 %', color: '--amber', fill: .14 }, { label: '帧数/分', color: '--blue', w: 1.4, axis: 2 }]"""))

rep.append(("""            return [m.map(function (x) { return normTs(x[0]); }), m.map(function (x) { return x[1]; }), m.map(function (x) { return x[2]; })];
          }, series: [{ label: '可用内存 MB', color: '--green', fill: .14 }, { label: 'PSI 10s', color: '--teal', w: 1.4 }]""",
"""            return [m.map(function (x) { return xsec(normTs(x[0])); }), m.map(function (x) { return x[1]; }), m.map(function (x) { return x[2]; })];
          }, series: [{ label: '可用内存 MB', color: '--green', fill: .14 }, { label: 'PSI 10s', color: '--teal', w: 1.4, axis: 2 }]"""))

rep.append(("""            return [g.map(function (x) { return normTs(x[0]); }), g.map(function (x) { return x[1]; }), g.map(function (x) { return x[2]; })];
          }, series: [{ label: 'GPU 占用 %', color: '--violet', fill: .14 }, { label: '频率 MHz', color: '--blue', w: 1.4 }]""",
"""            return [g.map(function (x) { return xsec(normTs(x[0])); }), g.map(function (x) { return x[1]; }), g.map(function (x) { return x[2]; })];
          }, series: [{ label: 'GPU 占用 %', color: '--violet', fill: .14 }, { label: '频率 MHz', color: '--blue', w: 1.4, axis: 2 }]"""))

rep.append(("""            return [p.map(function (x) { return normTs(x[0]); }), p.map(function (x) { return x[1]; }), p.map(function (x) { return -x[2]; })];
          }, series: [{ label: '电量 %', color: '--green', fill: .12 }, { label: '放电电流 mA（取负）', color: '--red', w: 1.4 }]""",
"""            return [p.map(function (x) { return xsec(normTs(x[0])); }), p.map(function (x) { return x[1]; }), p.map(function (x) { return -x[2]; })];
          }, series: [{ label: '电量 %', color: '--green', fill: .12 }, { label: '放电电流 mA（取负）', color: '--red', w: 1.4, axis: 2 }]"""))

rep.append(("""                return { label: s.label, color: cssv(s.color), w: s.w, fill: s.fill, values: data[k + 1] };""",
"""                return { label: s.label, color: cssv(s.color), w: s.w, fill: s.fill, axis: s.axis, values: data[k + 1] };"""))

for old, new in rep:
    if old not in s:
        raise SystemExit('锚点未命中:\n' + old[:120])
    s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print(f'app.js {n0} -> {len(s)} bytes；xsec {s.count("xsec(")} 次；副轴 {s.count("axis: 2")} 处')
