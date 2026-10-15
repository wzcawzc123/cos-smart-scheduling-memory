/* ============================================================
   UnifiedRootOptimizer 看板 v0.30.0
   - 5 分区：概览 / 趋势 / 画像 / 证据 / 关于
   - 数据：/sdcard/Android/UnifiedRootOptimizer/log/*.jsonl + 模块状态文件
   - 桥：KSU WebUI（ksu.exec 只回传最后一行 → 一律 base64 包一层）
   - 图表：uPlot（时间序列）+ 手写 SVG（占比/分布）
   纯前端；不改任何调度代码。
   ============================================================ */
(function () {
  'use strict';

  /* ---------------- A. 常量与桥 ---------------- */
  var URO = '/sdcard/Android/UnifiedRootOptimizer';
  var LOG = URO + '/log';
  var MOD = '/data/adb/modules/uro';
  var APPOPT = '/data/adb/modules/AppOpt/applist.conf';
  var PROF = URO + '/app_profiles.txt';

  function detectBridge() {
    var w = globalThis;
    var o, i, list = [w.ksu, w.WebUIX, w.mmrl];
    for (i = 0; i < list.length; i++) {
      o = list[i];
      if (o && typeof o.exec === 'function') {
        return { exec: function (c) { return Promise.resolve(o.exec(c)).then(function (r) { return String(r == null ? '' : r); }); } };
      }
    }
    if (w.ksu && typeof w.ksu.execAsync === 'function') {
      return { exec: function (c) { return new Promise(function (res, rej) { try { w.ksu.execAsync(c, function (r) { res(String(r == null ? '' : r)); }); } catch (e) { rej(e); } }); } };
    }
    return null;
  }
  var BR = detectBridge();

  function b64d(s) {
    s = String(s || '').replace(/\s+/g, '');
    if (!s) return '';
    var bin = atob(s);
    return new TextDecoder('utf-8').decode(Uint8Array.from(bin, function (ch) { return ch.charCodeAt(0); }));
  }
  function b64e(str) {
    var bytes = new TextEncoder().encode(str), bin = '', i;
    for (i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
    return btoa(bin);
  }
  // 单命令行执行（拿最后一行）
  function sh(cmd) {
    if (!BR) return Promise.resolve('');
    return BR.exec(cmd).then(function (v) { return String(v == null ? '' : v); }, function () { return ''; });
  }
  // 输出经 base64（绕开"只回最后一行"），返回完整文本
  function rd(cmd) {
    if (!BR) return Promise.resolve('');
    return BR.exec('(' + cmd + ') | base64 | tr -d "\\n"').then(function (out) {
      if (!out || !out.trim()) return '';
      try { return b64d(out); } catch (e) { return ''; }
    }, function () { return ''; });
  }

  /* ---------------- B. 纯函数（离线可测） ---------------- */
  // telemetry.jsonl 的 ts 是「秒」，其余文件是「毫秒」——统一归一化
  function normTs(v) {
    var n = Number(v) || 0;
    if (n > 0 && n < 1e11) n *= 1000;    // < 1e11 视为秒（当前 era ≈1.79e9 秒 / 1.79e12 毫秒）
    return n;
  }
  // 行文本 → CSV 数字数组（跳过解析失败的行）
  function parseCsv(text, cols) {
    var out = [], lines = String(text || '').split('\n'), i, f, row, k, bad;
    for (i = 0; i < lines.length; i++) {
      if (!lines[i].trim()) continue;
      f = lines[i].split(',');
      if (f.length < cols) continue;
      row = []; bad = false;
      for (k = 0; k < cols; k++) {
        var n = Number(f[k]);
        if (!isFinite(n)) { bad = true; break; }
        row.push(n);
      }
      if (!bad) out.push(row);
    }
    return out;
  }
  function parseJsonl(text) {
    var out = [], lines = String(text || '').split('\n'), i, o;
    for (i = 0; i < lines.length; i++) {
      if (!lines[i].trim()) continue;
      try { o = JSON.parse(lines[i]); if (o && typeof o === 'object') out.push(o); } catch (e) { /* 半行截断忽略 */ }
    }
    return out;
  }
  function downsample(arr, max) {
    if (!arr || arr.length <= max) return arr || [];
    var step = Math.ceil(arr.length / max), out = [], i;
    for (i = 0; i < arr.length; i += step) out.push(arr[i]);
    if (out[out.length - 1] !== arr[arr.length - 1]) out.push(arr[arr.length - 1]);
    return out;
  }
  function winSpec(h) {
    var tel = Math.round(h * 3600 / 12);      // telemetry ≈12s/条
    var min = Math.round(h * 3600 / 60);      // thermal/power/fps ≈60s/条
    return {
      telTail: Math.min(tel, 960), telStride: Math.max(1, Math.ceil(tel / 600)),
      minTail: Math.min(min, 1440), minStride: Math.max(1, Math.ceil(min / 360))
    };
  }
  var SCEN = {
    POWER_SAVE: { l: '省电', c: 'green', v: '--green' },
    BALANCE: { l: '均衡', c: 'grey', v: '--grey' },
    PERFORMANCE: { l: '性能', c: 'amber', v: '--amber' },
    FAST: { l: '极速', c: 'red', v: '--red' },
    GAME: { l: '游戏', c: 'violet', v: '--violet' },
    MEMORY_PRESSURE: { l: '内存压力', c: 'teal', v: '--teal' },
    BOOT: { l: '开机', c: 'grey', v: '--ink3' }
  };
  function scen(k) { return SCEN[k] || { l: k || '未知', c: 'grey', v: '--ink3' }; }
  // v0.34 手动档位（与后端 manual_mode.txt 对应）
  var MODES = [
    { k: 'AUTO', l: '自动' },
    { k: 'POWER_SAVE', l: '省电' },
    { k: 'BALANCE', l: '均衡' },
    { k: 'PERFORMANCE', l: '性能' },
    { k: 'FAST', l: '极速' }
  ];
  function fmtDur(ms) {
    var s = Math.max(0, Math.floor(ms / 1000));
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm' + (s % 60 ? ' ' + (s % 60) + 's' : '');
    return Math.floor(s / 3600) + 'h' + Math.floor((s % 3600) / 60) + 'm';
  }
  function fmtAgo(ms) {
    if (!ms) return '—';
    var d = Date.now() - ms;
    if (d < 0) d = 0;
    return d < 60000 ? Math.round(d / 1000) + 's 前' : fmtDur(d) + ' 前';
  }
  function nf(v, d) { return (v == null || !isFinite(v)) ? '—' : Number(v).toFixed(d == null ? 0 : d); }
  function pctOf(a, b) { return b > 0 ? (a / b * 100) : 0; }

  /* ---------------- C. 命令字典 ---------------- */
  function sed(script) { return "sed -n '" + script + "'"; }
  var CMDT = {
    // 状态（5s 档，轻）
    prop: 'cat ' + MOD + '/module.prop 2>/dev/null',
    conf: 'cat ' + URO + '/uro.conf 2>/dev/null',
    mode: 'cat ' + URO + '/mode.txt 2>/dev/null',
    bridge: 'cat ' + LOG + '/bridge.state 2>/dev/null',
    cpus: 'cat ' + LOG + '/cpu.state 2>/dev/null',
    hb: 'cat ' + LOG + '/evidence.hb 2>/dev/null',
    last: 'tail -1 ' + LOG + '/telemetry.jsonl 2>/dev/null',
    // 序列（趋势/sparkline）：sed 抽 CSV，体积可控
    temp: function (n, k) {
      return 'tail -' + n + ' ' + LOG + '/thermal.jsonl 2>/dev/null | awk \'NR%' + k + '==0\' | ' +
        sed('s/.*"ts":\\([0-9]*\\).*"battery":\\([0-9.]*\\).*/\\1,\\2/p');
    },
    fps: function (n, k) {
      return 'tail -' + n + ' ' + LOG + '/fps.jsonl 2>/dev/null | awk \'NR%' + k + '==0\' | ' +
        sed('s/.*"ts":\\([0-9]*\\).*"frames":\\([0-9]*\\),"janky":\\([0-9]*\\).*/\\1,\\2,\\3/p');
    },
    mem: function (n, k) {
      return 'tail -' + n + ' ' + LOG + '/telemetry.jsonl 2>/dev/null | awk \'NR%' + k + '==0\' | ' +
        sed('s/.*"ts":\\([0-9]*\\).*"memAvailMb":\\([0-9]*\\).*"psiMem10":\\([0-9.]*\\).*/\\1,\\2,\\3/p');
    },
    pwr: function (n, k) {
      return 'tail -' + n + ' ' + LOG + '/power.jsonl 2>/dev/null | awk \'NR%' + k + '==0\' | ' +
        sed('s/.*"ts":\\([0-9]*\\).*"pct":\\([0-9]*\\).*"mA":\\([-0-9]*\\).*/\\1,\\2,\\3/p');
    },
    gpu: function (n, k) {
      return 'tail -' + n + ' ' + LOG + '/gpu.jsonl 2>/dev/null | awk \'NR%' + k + '==0\' | ' +
        sed('s/.*"ts":\\([0-9]*\\).*"gpuBusy":\\([0-9]*\\),"gpuClkMhz":\\([0-9]*\\).*/\\1,\\2,\\3/p');
    },
    // 证据
    band: 'tail -200 ' + LOG + '/telemetry.jsonl 2>/dev/null | ' +
      sed('s/.*"ts":\\([0-9]*\\).*"scenario":"\\([A-Z_]*\\)".*"why":"\\([^"]*\\)".*"fg":"\\([^"]*\\)".*/\\1,\\2,\\3,\\4/p'),
    thread: 'tail -1 ' + LOG + '/thread.jsonl 2>/dev/null',
    gen: 'sed -n "/URO-GEN-BEGIN/,/URO-GEN-END/p" ' + APPOPT + ' 2>/dev/null'
  };

  /* ---------------- D. uPlot 辅助 ---------------- */
  function cssv(n) {
    try { return getComputedStyle(document.documentElement).getPropertyValue(n).trim() || '#888'; } catch (e) { return '#888'; }
  }
  function hexA(hex, a) {
    hex = String(hex || '').trim();
    if (hex.charAt(0) !== '#' || (hex.length !== 7 && hex.length !== 4)) return hex;
    var r, g, b;
    if (hex.length === 4) { r = parseInt(hex[1] + hex[1], 16); g = parseInt(hex[2] + hex[2], 16); b = parseInt(hex[3] + hex[3], 16); }
    else { r = parseInt(hex.substr(1, 2), 16); g = parseInt(hex.substr(3, 2), 16); b = parseInt(hex.substr(5, 2), 16); }
    return 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
  }
  function axisStyle() {
    return {
      stroke: cssv('--ink3'), grid: { stroke: cssv('--grid'), width: 1 },
      ticks: { stroke: cssv('--grid'), width: 1, size: 5 },
      font: '11px -apple-system,sans-serif', labelFont: '11px sans-serif'
    };
  }
  // uPlot 的 time 刻度按 Unix 秒解释（不是毫秒）→ 前端统一换算
  function xsec(ms) { return Number(ms) / 1000; }
  function buildLines(el, height, xs, series, opt) {
    opt = opt || {};
    var ax = axisStyle(), s = [{ label: '时间' }], i, dual = false;
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
    var scales = { x: { time: true }, y: {}, y2: {} };
    return new window.uPlot({
      width: el.clientWidth || 320, height: height,
      padding: [6, 10, 0, 4],
      cursor: { y: false, points: { size: 7 }, drag: { x: true, y: false } },
      legend: { show: series.length > 1, live: true },
      scales: scales, axes: axes, series: s
    }, [xs].concat(series.map(function (x) { return x.values; })), el);
  }
  function buildSpark(el, xs, values, color, fill) {
    var ax = axisStyle();
    return new window.uPlot({
      width: el.clientWidth || 260, height: 44,
      padding: [3, 2, 0, 2], cursor: { show: false }, legend: { show: false },
      scales: { x: { time: false }, y: { range: function (u, min, max) { return [min - (max - min) * .15, max + (max - min) * .15]; } } },
      axes: [Object.assign({}, ax, { show: false }), Object.assign({}, ax, { show: false })],
      series: [{}, { stroke: color, width: 1.6, fill: hexA(color, fill == null ? .18 : fill), points: { show: false }, spanGaps: true }]
    }, [xs, values], el);
  }

  /* ---------------- E. 画像编辑器（v0.29.0 逻辑原样保留） ---------------- */
  var CORE = { 'e-core': '小核', 'p-core': '中核', 'hp-core': '大核', 'all-core': '全核' };
  function pfParse(text) {
    var items = [], lines = String(text || '').split('\n'), i;
    for (i = 0; i < lines.length; i++) {
      var t = lines[i].replace(/\r$/, ''), h = t.indexOf('#'), body = h >= 0 ? t.slice(0, h) : t, f = body.split('|');
      if (!body.trim() || f.length < 2 || !f[0].trim()) { if (t.trim()) items.push({ kind: 'other', text: t }); continue; }
      items.push({
        kind: 'rule', pattern: (f[0] || '').trim(), down: (f[1] || '').trim(),
        up: (f[2] || '').trim(), cpuset: (f[3] || '').trim(), note: (h >= 0 ? t.slice(h).trim() : '')
      });
    }
    return items;
  }
  function pfLine(r) {
    var s;
    if (r.cpuset) s = r.pattern + '|' + (r.down || '-1') + '|' + (r.up || '-1') + '|' + r.cpuset;
    else if (r.up) s = r.pattern + '|' + (r.down || '') + '|' + r.up;
    else if (r.down) s = r.pattern + '|' + r.down;
    else s = r.pattern;
    return r.note ? s + ' ' + r.note : s;
  }
  function pfBuild(list) {
    var ls = [], i;
    for (i = 0; i < list.length; i++) ls.push(list[i].kind === 'rule' ? pfLine(list[i]) : list[i].text);
    while (ls.length && !ls[ls.length - 1].trim()) ls.pop();
    return ls.length ? ls.join('\n') + '\n' : '';
  }
  function pfGen(text) {
    var out = [], ls = String(text || '').split('\n'), i, s;
    for (i = 0; i < ls.length; i++) { s = ls[i].trim(); if (s && s.charAt(0) !== '#') out.push(s); }
    return out;
  }
  function pfSemRate(v) { var s = String(v || '').trim(); return (!s || s === '-1') ? '跟随档位' : s + 'μs 固定'; }
  function pfSemCpu(v) {
    var s = String(v || '').trim();
    if (!s) return '不生成核组规则';
    if (CORE[s]) return CORE[s];
    if (s.indexOf(',') >= 0) return s.split(',').map(function (x) { return CORE[x] || x; }).join(' + ');
    return /^\d/.test(s) ? '核号 ' + s : '自定义组';
  }
  function pfWarn(r) {
    var w = [];
    if (r.pattern.indexOf('*') >= 0) w.push('通配画像：策略热读生效，指挥链② 保守跳过（不生成核组规则）');
    if (!r.down && !r.up && !r.cpuset) w.push('三字段全空 → 该行不产生任何效果');
    if (r.cpuset && (!r.down || !r.up)) w.push('四字段行里空的 down/up 会被运行时 atoi 成 0 → 保存后自动补 -1');
    return w.join(' · ');
  }
  function pfValidate(f) {
    var p = String(f.pattern || '').trim();
    if (!p) return '包名不能为空';
    if (/\s/.test(p)) return '包名不能含空白字符';
    if (/[|#]/.test(p)) return '包名不能含 | 或 #';
    if (!String(f.down || '').trim() && !String(f.up || '').trim() && !String(f.cpuset || '').trim())
      return '至少要填一个字段（down / up / cpuset）—— 只有包名的行运行时不产生任何效果';
    var rt = function (v, n) {
      var s = String(v || '').trim();
      if (!s) return '';
      if (!/^-?\d+$/.test(s)) return n + ' 必须是整数（可留空，-1 = 跟随档位）';
      if (parseInt(s, 10) < -1) return n + ' 不能小于 -1（-1 = 跟随档位）';
      return '';
    };
    var e = rt(f.down, 'down') || rt(f.up, 'up');
    if (e) return e;
    var c = String(f.cpuset || '').trim();
    if (c && !/^[A-Za-z0-9_-]+(,[A-Za-z0-9_-]+)*$/.test(c))
      return 'cpuset 只允许字母数字 - _ 与逗号（例：p-core / e-core,p-core / 0-3）';
    return '';
  }

  /* ---------------- F. 应用 ---------------- */
  var fb = document.getElementById('fallback');
  if (typeof window.Vue === 'undefined') { if (fb) fb.style.display = 'block'; return; }
  if (!BR) {
    if (fb) { fb.style.display = 'block'; fb.textContent = '未检测到 WebUI 桥（ksu / WebUIX / mmrl）'; }
    return;
  }

  var app = window.Vue.createApp({
    setup: function () {
      var R = window.Vue.ref, C = window.Vue.computed;

      /* ---------- 导航 / 头部 ---------- */
      var tabs = [
        { k: 'overview', n: '概览', i: 'i-gauge' },
        { k: 'trend', n: '趋势', i: 'i-trend' },
        { k: 'profile', n: '画像', i: 'i-sliders' },
        { k: 'evidence', n: '证据', i: 'i-list' },
        { k: 'about', n: '关于', i: 'i-info' }
      ];
      var tab = R((function () { try { return localStorage.getItem('uro.tab') || 'overview'; } catch (e) { return 'overview'; } })());
      var scrolled = R(false);
      var busy = R(false);
      function go(k) {
        tab.value = k;
        try { localStorage.setItem('uro.tab', k); } catch (e) { }
        window.scrollTo({ top: 0, behavior: 'smooth' });
        if (k === 'overview') { loadStatus(); loadSparks(); }
        if (k === 'trend') loadTrend();
        if (k === 'evidence') loadEvidence();
        if (k === 'profile' && !pf.items.value.length) pf.load();
        window.Vue.nextTick(syncCharts);
      }

      /* ---------- 概览状态 ---------- */
      var st = R({ ver: '', desc: '', mode: '', enforce: false, dirtyB: '?', dirtyC: '?', phase: '', up: {}, down: {}, hi: {}, tl: {}, hb: 0, cur: null, ctrl: [] });
      var err = R('');
      var subline = C(function () {
        var s = st.value;
        return (s.ver || '…') + (s.mode ? ' · 用户档 ' + s.mode : '') + (s.sinceMs ? ' · 最近决策 ' + fmtDur(Date.now() - s.sinceMs) + ' 前' : '');
      });
      var curScen = C(function () { var c = st.value.cur; return c ? scen(c.scenario) : scen(''); });
      var sinceText = C(function () {
        var c = st.value.cur;
        return c ? '决策 ' + fmtAgo(normTs(c.ts)) + (c.gen ? ' · gen ' + c.gen : '') : '';
      });
      var WINLIST = [{ k: '1h', n: '1 小时' }, { k: '6h', n: '6 小时' }, { k: '24h', n: '24 小时' }];
      // 趋势卡右上角：按序列类型给出"最新一行"的可读值
      function lastOf(a, kind) {
        if (!a || !a.length) return '—';
        var r = a[a.length - 1];
        switch (kind) {
          case 'temp': return nf(r[1], 1) + ' °C';
          case 'fps': return nf(pctOf(r[2], r[1]), 1) + ' % · ' + nf(r[1], 0) + ' 帧/分';
          case 'mem': return nf(r[1], 0) + ' MB · PSI ' + nf(r[2], 1);
          case 'gpu': return nf(r[2], 0) + ' MHz · busy ' + nf(r[1], 0) + '%';
          case 'pwr': return nf(r[1], 0) + ' % · ' + nf(r[2], 0) + ' mA';
          default: return nf(r[1], 1);
        }
      }
      function pct1(v) { return (Number(v) || 0).toFixed(0); }
      function clock(v) {
        var d = new Date(normTs(v)), p = function (n) { return (n < 10 ? '0' : '') + n; };
        return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
      }
      var lamps = C(function () {
        var map = {}, i, a = st.value.ctrl || [];
        for (i = 0; i < a.length; i++) for (var k in a[i]) map[k] = a[i][k];
        return [
          { n: '内存', s: map.memory || '—', cls: cls4(map.memory) },
          { n: 'CPU', s: map.cpu || '—', cls: cls4(map.cpu) },
          { n: 'GPU', s: map.gpu || '—', cls: cls4(map.gpu) },
          { n: '热控', s: map.thermal || '—', cls: cls4(map.thermal) }
        ];
      });
      function cls4(v) {
        if (!v) return 'off';
        if (v === 'ACTIVE' || v === 'NORMAL') return 'ok';
        if (v === 'DEGRADED' || v === 'LIMITED') return 'warn';
        if (v === 'OFF' || v === 'IDLE') return 'off';
        return 'warn';
      }
      var health = C(function () {
        var s = st.value, out = [];
        var clean = (s.dirtyB === 'false' && s.dirtyC === 'false');
        out.push({ n: '基线', v: clean ? 'CLEAN' : 'DIRTY', cls: clean ? 'ok' : 'warn', m: '内存 ' + s.dirtyB + ' / CPU ' + s.dirtyC });
        out.push({ n: '看门狗', v: s.phase || '—', cls: s.phase === 'COMMIT' ? 'ok' : 'warn', m: 'phase' });
        var age = s.hb ? Date.now() - s.hb : 0;
        out.push({ n: '心跳', v: s.hb ? fmtAgo(s.hb) : '—', cls: !s.hb ? 'err' : (age < 120000 ? 'ok' : 'err'), m: 'Evidence 60s 轮询' });
        return out;
      });
      var uagRows = C(function () {
        var s = st.value, out = [], names = { 0: '小核', 3: '中核', 7: '大核' }, k;
        for (k in s.hi) out.push({ p: 'policy' + k, n: names[k] || k, up: s.up[k], down: s.down[k], hi: s.hi[k] });
        return out;
      });

      /* ---------- sparkline（概览，30s） ---------- */
      var sparks = R({ temp: { xs: [], v: [], last: null }, jank: { xs: [], v: [], last: null }, mem: { xs: [], v: [], last: null } });
      function loadStatus() {
        return Promise.all([
          rd(CMDT.prop), rd(CMDT.conf), rd(CMDT.bridge), rd(CMDT.cpus), sh(CMDT.hb), rd(CMDT.last), sh(CMDT.mode),
          rd('cat ' + URO + '/manual_mode.txt 2>/dev/null || echo AUTO')
        ]).then(function (r) {
          var prop = r[0] || '', conf = r[1] || '', bridge = r[2] || '', cpus = r[3] || '';
          var o = {
            ver: (prop.match(/^version=(.*)$/m) || [])[1] || '', desc: (prop.match(/^description=(.*)$/m) || [])[1] || '',
            mode: (r[6] || '').trim(),
            manual: ((r[7] || 'AUTO').trim().toUpperCase() || 'AUTO'), enforce: /BRIDGE_ENFORCE=(\d)/.test(conf) ? RegExp.$1 === '1' : false,
            dirtyB: (bridge.match(/DIRTY=(\w+)/) || [])[1] || '?',
            dirtyC: (cpus.match(/DIRTY=(\w+)/) || [])[1] || '?',
            phase: (cpus.match(/PHASE=(\w+)/) || [])[1] || (bridge.match(/PHASE=(\w+)/) || [])[1] || '',
            hb: normTs((r[4] || '').replace(/[^0-9]/g, '')),
            cur: null, ctrl: [], ver0: ''
          };
          var m, re = /(UP|DOWN|HI|TL):policy(\d)=([0-9 ]+\S*)/g;
          o.up = {}; o.down = {}; o.hi = {}; o.tl = {};
          while ((m = re.exec(cpus))) {
            var grp = m[1].toLowerCase(), p = m[2], val = (m[3] || '').trim();
            o[grp === 'up' ? 'up' : grp === 'down' ? 'down' : grp === 'hi' ? 'hi' : 'tl'][p] = val;
          }
          var tel = parseJsonl(r[5]);
          if (tel.length) {
            var c = tel[tel.length - 1];
            o.cur = c;
            o.ctrl = Array.isArray(c.ctrl) ? c.ctrl : [];
            o.sinceMs = normTs(c.ts);
          }
          st.value = o;
          err.value = '';
        }).catch(function (e) { err.value = '读取失败：' + e; });
      }
      function loadSparks() {
        var sp = winSpec(1);
        return Promise.all([
          rd(CMDT.temp(sp.minTail, sp.minStride)),
          rd(CMDT.fps(sp.minTail, sp.minStride)),
          rd(CMDT.mem(sp.telTail, sp.telStride))
        ]).then(function (r) {
          var t = parseCsv(r[0], 2), f = parseCsv(r[1], 3), m = parseCsv(r[2], 3);
          var o = {};
          o.temp = { xs: t.map(function (x) { return normTs(x[0]); }), v: t.map(function (x) { return x[1]; }), last: t.length ? t[t.length - 1][1] : null };
          o.jank = { xs: f.map(function (x) { return normTs(x[0]); }), v: f.map(function (x) { return pctOf(x[2], x[1]); }), last: f.length ? pctOf(f[f.length - 1][2], f[f.length - 1][1]) : null, frames: f.length ? f[f.length - 1][1] : null };
          o.mem = { xs: m.map(function (x) { return normTs(x[0]); }), v: m.map(function (x) { return x[1]; }), last: m.length ? m[m.length - 1][1] : null, psi: m.length ? m[m.length - 1][2] : null };
          sparks.value = o;
          syncCharts();
        });
      }

      /* ---------- 趋势 ---------- */
      var win = R('1h');
      var trend = R({ temp: [], jank: [], fps: [], mem: [], gpu: [], pwr: [] });
      var trendEmpty = C(function () {
        var t = trend.value;
        return !(t.temp.length || t.fps.length || t.mem.length || t.gpu.length || t.pwr.length);
      });
      function loadTrend() {
        var h = win.value === '24h' ? 24 : (win.value === '6h' ? 6 : 1), sp = winSpec(h);
        return Promise.all([
          rd(CMDT.temp(sp.minTail, sp.minStride)), rd(CMDT.fps(sp.minTail, sp.minStride)),
          rd(CMDT.mem(sp.telTail, sp.telStride)), rd(CMDT.gpu(sp.minTail, sp.minStride)),
          rd(CMDT.pwr(sp.minTail, sp.minStride))
        ]).then(function (r) {
          trend.value = {
            temp: downsample(parseCsv(r[0], 2), 800),
            fps: downsample(parseCsv(r[1], 3), 800),
            mem: downsample(parseCsv(r[2], 3), 800),
            gpu: downsample(parseCsv(r[3], 3), 800),
            pwr: downsample(parseCsv(r[4], 3), 800)
          };
          syncCharts();
        });
      }
      function setWin(w) { win.value = w; loadTrend(); }

      /* ---------- 证据 ---------- */
      var ev = R({ rows: [], counts: [], topApps: [], thread: null, gen: [] });
      var band = C(function () {
        var rows = ev.value.rows, n = rows.length;
        if (!n) return [];
        // 真机样本档位几乎逐条切换（合并连续段仍有上百段）→ 按桶取"多数档"，
        // 40 格热力条：颜色 = 该桶主导档位，深浅 = 主导档位占比（混合桶更浅）
        var buckets = Math.min(40, n), size = n / buckets, out = [];
        for (var k = 0; k < buckets; k++) {
          var from = Math.floor(k * size), to = Math.min(n, Math.floor((k + 1) * size));
          if (to <= from) continue;
          var cnt = {}, best = '', bestN = 0, i, s;
          for (i = from; i < to; i++) {
            s = rows[i][1]; cnt[s] = (cnt[s] || 0) + 1;
            if (cnt[s] > bestN) { bestN = cnt[s]; best = s; }
          }
          out.push({
            w: (to - from) / n * 100, color: 'var(' + scen(best).v + ')',
            t: scen(best).l, n: to - from, share: bestN / (to - from)
          });
        }
        return out;
      });
      var threadGroups = C(function () {
        var t = ev.value.thread;
        if (!t || !t.groups) return [];
        var out = [], total = 0, k;
        for (k in t.groups) total += t.groups[k];
        for (k in t.groups) out.push({ g: k, n: t.groups[k], pct: total ? (t.groups[k] / total * 100) : 0 });
        out.sort(function (a, b) { return b.n - a.n; });
        return out;
      });
      function loadEvidence() {
        return Promise.all([rd(CMDT.band), sh(CMDT.thread), rd(CMDT.gen)]).then(function (r) {
          var rows = [], i, cnt = {}, apps = {};
          // band CSV：ts,scenario,why,fg（字符串列，不能走数字 parseCsv）
          var lines = String(r[0] || '').split('\n');
          for (i = 0; i < lines.length; i++) {
            var f = lines[i].split(',');
            if (f.length < 4 || !/^\d+$/.test(f[0])) continue;
            rows.push(f);
            cnt[f[1]] = (cnt[f[1]] || 0) + 1;
            apps[f[3]] = (apps[f[3]] || 0) + 1;
          }
          var total = 0; for (var k in cnt) total += cnt[k];
          var counts = []; for (k in cnt) counts.push({ k: k, n: cnt[k], pct: pctOf(cnt[k], total), s: scen(k) });
          counts.sort(function (a, b) { return b.n - a.n; });
          var ta = []; for (k in apps) ta.push({ k: k || '(无)', n: apps[k], pct: pctOf(apps[k], total) });
          ta.sort(function (a, b) { return b.n - a.n; });
          var th = null; try { th = parseJsonl(r[1])[0] || null; } catch (e) { }
          ev.value = { rows: rows, counts: counts, topApps: ta.slice(0, 5), thread: th, gen: pfGen(r[2]) };
        });
      }

      /* ---------- 图表注册表 ---------- */
      var CH = {};   // key -> {u, host, build}
      var CHARTS = [
        { key: 'sp-temp', page: 'overview', h: 44, spark: true, data: function () { var s = sparks.value.temp; return [s.xs, s.v]; }, color: '--red' },
        { key: 'sp-jank', page: 'overview', h: 44, spark: true, data: function () { var s = sparks.value.jank; return [s.xs, s.v]; }, color: '--amber' },
        { key: 'sp-mem', page: 'overview', h: 44, spark: true, data: function () { var s = sparks.value.mem; return [s.xs, s.v]; }, color: '--green' },
        { key: 'c-temp', page: 'trend', h: 168, data: function () { var t = trend.value.temp; return [t.map(function (x) { return xsec(normTs(x[0])); }), t.map(function (x) { return x[1]; })]; }, series: [{ label: '电池温度 °C', color: '--red', fill: .16 }] },
        {
          key: 'c-perf', page: 'trend', h: 168, data: function () {
            var f = trend.value.fps;
            return [f.map(function (x) { return xsec(normTs(x[0])); }), f.map(function (x) { return pctOf(x[2], x[1]); }), f.map(function (x) { return x[1]; })];
          }, series: [{ label: '卡顿率 %', color: '--amber', fill: .14 }, { label: '帧数/分', color: '--blue', w: 1.4, axis: 2 }]
        },
        {
          key: 'c-mem', page: 'trend', h: 168, data: function () {
            var m = trend.value.mem;
            return [m.map(function (x) { return xsec(normTs(x[0])); }), m.map(function (x) { return x[1]; }), m.map(function (x) { return x[2]; })];
          }, series: [{ label: '可用内存 MB', color: '--green', fill: .14 }, { label: 'PSI 10s', color: '--teal', w: 1.4, axis: 2 }]
        },
        {
          key: 'c-gpu', page: 'trend', h: 168, data: function () {
            var g = trend.value.gpu;
            return [g.map(function (x) { return xsec(normTs(x[0])); }), g.map(function (x) { return x[1]; }), g.map(function (x) { return x[2]; })];
          }, series: [{ label: 'GPU 占用 %', color: '--violet', fill: .14 }, { label: '频率 MHz', color: '--blue', w: 1.4, axis: 2 }]
        },
        {
          key: 'c-pwr', page: 'trend', h: 168, data: function () {
            var p = trend.value.pwr;
            return [p.map(function (x) { return xsec(normTs(x[0])); }), p.map(function (x) { return x[1]; }), p.map(function (x) { return -x[2]; })];
          }, series: [{ label: '电量 %', color: '--green', fill: .12 }, { label: '放电电流 mA（取负）', color: '--red', w: 1.4, axis: 2 }]
        }
      ];
      function syncCharts() {
        if (typeof window.uPlot === 'undefined') return;
        var i, d;
        for (i = 0; i < CHARTS.length; i++) {
          d = CHARTS[i];
          var host = document.querySelector('[data-chart="' + d.key + '"]');
          if (!host || d.page !== tab.value) continue;
          var w = host.clientWidth || 300;
          var yes = null;
          try { yes = (d.data()[0] || []).length > 0; } catch (e) { }
          if (!yes) { if (CH[d.key]) { CH[d.key].u.destroy(); delete CH[d.key]; } continue; }
          var data = d.data();
          if (!CH[d.key] || CH[d.key].w !== w) {
            if (CH[d.key]) { try { CH[d.key].u.destroy(); } catch (e) { } }
            var u = d.spark
              ? buildSpark(host, data[0], data[1], cssv(d.color))
              : buildLines(host, d.h, data[0], d.series.map(function (s, k) {
                return { label: s.label, color: cssv(s.color), w: s.w, fill: s.fill, axis: s.axis, values: data[k + 1] };
              }), { space: 46 });
            CH[d.key] = { u: u, w: w };
          } else {
            CH[d.key].u.setData(data);
          }
        }
      }
      function rebuildCharts() {
        for (var k in CH) { try { CH[k].u.destroy(); } catch (e) { } delete CH[k]; }
        syncCharts();
      }

      /* ---------- 轮询 ---------- */
      var tFast = null, tSlow = null, pausing = false;
      function startPoll() {
        stopPoll();
        tFast = setInterval(function () {
          if (pausing) return;
          loadStatus();
        }, 5000);
        tSlow = setInterval(function () {
          if (pausing) return;
          if (tab.value === 'overview') loadSparks();
          else if (tab.value === 'trend') loadTrend();
          else if (tab.value === 'evidence') loadEvidence();
        }, 30000);
      }
      function stopPoll() { if (tFast) clearInterval(tFast); if (tSlow) clearInterval(tSlow); tFast = tSlow = null; }
      function refreshAll() {
        busy.value = true;
        Promise.all([loadStatus(), loadSparks(), tab.value === 'trend' ? loadTrend() : null, tab.value === 'evidence' ? loadEvidence() : null])
          .catch(function () { }).then(function () { busy.value = false; syncCharts(); });
      }
      var armT = null;
      function setManual(k) {
        busy.value = true;
        sh('echo ' + k + ' > ' + URO + '/manual_mode.txt').then(function () {
          return loadStatus();
        }).then(function () { busy.value = false; setTimeout(loadStatus, 500); });
      }
      function toggleEnforce() {
        if (!st.value.enforce && !armT) {           // 两击确认（关→开才需要确认）
          armT = setTimeout(function () { armT = null; loadStatus(); }, 2500);
          st.value = Object.assign({}, st.value, { arming: true });
          return;
        }
        clearTimeout(armT); armT = null;
        var target = st.value.enforce ? 0 : 1;
        busy.value = true;
        sh('echo BRIDGE_ENFORCE=' + target + ' > ' + URO + '/uro.conf').then(function () {
          return loadStatus();
        }).then(function () { busy.value = false; setTimeout(loadStatus, 400); });
      }

      /* ---------- 画像子系统 ---------- */
      var pf = {};
      (function () {
        var items = R([]), meta = R(null), genRules = R([]), genNote = R('');
        var loading = R(true), fb = R(false), msg = R(''), msgCls = R('ok'), editIndex = R(-1), pendingDel = R(-1);
        var form = R({ pattern: '', down: '', up: '', cpuset: '' });
        var presets = ['e-core', 'p-core', 'hp-core', 'all-core'];
        var formErr = C(function () { return pfValidate(form.value); });
        var dirty = C(function () { var f = form.value; return !!(f.pattern || f.down || f.up || f.cpuset); });
        var ruleRows = C(function () {
          var rows = [], k = 0;
          items.value.forEach(function (it, i) { if (it.kind !== 'rule') return; rows.push({ it: it, i: i, k: k, warn: pfWarn(it) }); k++; });
          return rows;
        });
        var otherCount = C(function () { return items.value.filter(function (i) { return i.kind !== 'rule'; }).length; });
        var fileState = C(function () { return meta.value ? '已存在' : '不存在'; });
        var badgeCls = C(function () { return meta.value ? 'ok' : 'warn'; });
        var metaLine = C(function () {
          var m = meta.value;
          if (!m) return '文件不存在 = 无画像（合法状态，等同未加任何规则）';
          return m.size + ' B · ' + m.perm + ' · ' + m.owner + ' · ' + m.mtime;
        });
        function load() {
          if (fb.value) return Promise.resolve();
          loading.value = true;
          return Promise.all([
            rd('cat ' + PROF + ' 2>/dev/null'),
            sh('stat -c "%s|%u|%g|%a|%y" ' + PROF + ' 2>/dev/null'),
            rd('sed -n "/URO-GEN-BEGIN/,/URO-GEN-END/p" ' + APPOPT + ' 2>/dev/null')
          ]).then(function (r) {
            items.value = pfParse(r[0]);
            var s = String(r[1] || '').trim();
            if (/^\d+\|\d+\|\d+\|\d+\|/.test(s)) {
              var f = s.split('|'), t = (f[4] || '').trim().split(/\s+/);
              meta.value = { size: Number(f[0]), owner: f[1] + ':' + f[2], perm: f[3], mtime: (t[0] || '') + ' ' + (t[1] || '').slice(0, 5) };
            } else { meta.value = null; }
            genRules.value = pfGen(r[2]);
            genNote.value = genRules.value.length ? '以上来自 applist.conf 的 URO-GEN 区块（URO 自动维护，勿手改）' : '区块空 = 当前没有基于画像生成的核组规则（对账周期 60s）';
          }).catch(function (e) { msg.value = '读取失败：' + e; msgCls.value = 'err'; })
            .then(function () { loading.value = false; });
        }
        function writeAll(text) {
          var b64 = b64e(text), bak = PROF + '.uro_bak_' + Math.floor(Date.now() / 1000), steps = [], f;
          if (meta.value) steps.push('cp -f ' + PROF + ' ' + bak);
          steps.push('echo ' + b64 + ' | base64 -d > ' + PROF + '.urotmp');
          if (meta.value) {
            f = meta.value.owner.split(':');
            steps.push('chown ' + f[0] + ':' + f[1] + ' ' + PROF + '.urotmp');
            steps.push('chmod ' + meta.value.perm + ' ' + PROF + '.urotmp');
          }
          steps.push('mv -f ' + PROF + '.urotmp ' + PROF);
          var chain = Promise.resolve(true);
          steps.forEach(function (c) {
            chain = chain.then(function (ok) {
              return sh(c + ' && echo ok || echo err').then(function (o) { return ok && String(o).indexOf('err') !== 0; });
            });
          });
          return chain.then(function (ok) {
            if (!ok) return { ok: false, bak: '' };
            sh('ls -1t ' + PROF + '.uro_bak_* 2>/dev/null | sed -n "4,$p" | while read x; do rm -f "$x"; done');
            return rd('cat ' + PROF + ' 2>/dev/null').then(function (back) {
              return { ok: back === text, bak: meta.value ? bak.split('/').pop() : '' };
            });
          });
        }
        function commit(next, okMsg, failMsg) {
          fb.value = true; msg.value = '';
          return writeAll(pfBuild(next)).then(function (res) {
            fb.value = false;
            if (!res.ok) {
              msg.value = failMsg || '写入失败或回读不一致 —— 文件未被可靠替换，请点右上角刷新确认';
              msgCls.value = 'err';
              return load();
            }
            items.value = next;
            pendingDel.value = -1;
            msg.value = okMsg + (res.bak ? '（备份 ' + res.bak + '）' : '') + ' · 前三字段热读即时生效；第四字段 60s 内由指挥链② 对账生成核组规则';
            msgCls.value = 'ok';
            load();
            setTimeout(function () { load(); }, 65000);
          }).catch(function (e) { fb.value = false; msg.value = '写入异常：' + e; msgCls.value = 'err'; });
        }
        function submit() {
          var e = pfValidate(form.value);
          if (e) { msg.value = e; msgCls.value = 'err'; return; }
          var next = items.value.slice();
          var draft = { kind: 'rule', pattern: form.value.pattern.trim(), down: form.value.down.trim(), up: form.value.up.trim(), cpuset: form.value.cpuset.trim(), note: '' };
          if (editIndex.value >= 0) {
            var old = next[editIndex.value];
            draft.note = (old && old.note) ? old.note : '';
            next[editIndex.value] = draft;
          } else { next.push(draft); }
          commit(next, editIndex.value >= 0 ? '修改已写入' : '已新增并写入').then(function () {
            editIndex.value = -1;
            form.value = { pattern: '', down: '', up: '', cpuset: '' };
          });
        }
        function edit(i) {
          var it = items.value[i];
          if (!it || it.kind !== 'rule') return;
          editIndex.value = i;
          form.value = { pattern: it.pattern, down: it.down, up: it.up, cpuset: it.cpuset };
          pendingDel.value = -1; msg.value = '';
        }
        function del(i) {
          if (pendingDel.value !== i) {
            pendingDel.value = i;
            msg.value = '再点一次「确认删」即删除第 ' + (i + 1) + ' 行并写回文件（写前自动备份）';
            msgCls.value = 'warn';
            return;
          }
          pendingDel.value = -1;
          var next = items.value.slice();
          next.splice(i, 1);
          if (editIndex.value === i) { editIndex.value = -1; form.value = { pattern: '', down: '', up: '', cpuset: '' }; }
          commit(next, '已删除并写回');
        }
        function move(i, dir) {
          var idx = [], k;
          for (k = 0; k < items.value.length; k++) if (items.value[k].kind === 'rule') idx.push(k);
          var at = idx.indexOf(i), to = at + dir;
          if (at < 0 || to < 0 || to >= idx.length) return;
          var next = items.value.slice(), tmp = next[idx[at]];
          next[idx[at]] = next[idx[to]]; next[idx[to]] = tmp;
          if (editIndex.value === idx[at]) editIndex.value = idx[to];
          else if (editIndex.value === idx[to]) editIndex.value = idx[at];
          pendingDel.value = -1;
          commit(next, '顺序已调整（文件顺序 = 匹配优先级）');
        }
        function reset() {
          editIndex.value = -1; pendingDel.value = -1;
          form.value = { pattern: '', down: '', up: '', cpuset: '' };
          msg.value = '';
        }
        pf = {
          items: items, meta: meta, genRules: genRules, genNote: genNote, loading: loading, busy: fb, msg: msg, msgCls: msgCls,
          editIndex: editIndex, pendingDel: pendingDel, form: form, presets: presets, formErr: formErr, dirty: dirty,
          ruleRows: ruleRows, otherCount: otherCount, fileState: fileState, badgeCls: badgeCls, metaLine: metaLine,
          semRate: pfSemRate, semCpu: pfSemCpu, load: load, submit: submit, edit: edit, del: del, move: move, reset: reset
        };
      })();

      /* ---------- 生命周期 ---------- */
      window.Vue.onMounted(function () {
        window.addEventListener('scroll', function () { scrolled.value = window.scrollY > 24; }, { passive: true });
        window.addEventListener('resize', function () { window.Vue.nextTick(rebuildCharts); });
        document.addEventListener('visibilitychange', function () { pausing = document.hidden; if (!pausing) refreshAll(); });
        if (window.matchMedia) {
          var mq = window.matchMedia('(prefers-color-scheme: dark)');
          if (mq.addEventListener) mq.addEventListener('change', function () { window.Vue.nextTick(rebuildCharts); });
        }
        loadStatus().then(function () { window.Vue.nextTick(syncCharts); });
        loadSparks();
        if (tab.value === 'trend') loadTrend();
        if (tab.value === 'evidence') loadEvidence();
        pf.load();
        startPoll();
      });

      return Object.assign({
        tab: tab, tabs: tabs, go: go, scrolled: scrolled, busy: busy, err: err,
        st: st, subline: subline, curScen: curScen, lamps: lamps, health: health, uagRows: uagRows,
        sparks: sparks, trend: trend, win: win, setWin: setWin, trendEmpty: trendEmpty,
        ev: ev, band: band, threadGroups: threadGroups,
        refreshAll: refreshAll, toggleEnforce: toggleEnforce, setManual: setManual, MODES: MODES,
        scenOf: scen, nf: nf, fmtAgo: fmtAgo, fmtDur: fmtDur, pctOf: pctOf,
        sinceText: sinceText, WINLIST: WINLIST, lastOf: lastOf, pct1: pct1, clock: clock,
        paths: { log: LOG, module: MOD, profile: PROF, appopt: APPOPT }
      }, { pf: pf });
    }
  });
  app.mount('#app');
})();
