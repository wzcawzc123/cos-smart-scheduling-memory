
/* ===== App 画像编辑器（Vue 3 内联 · v0.29.0）=====
   桥约束沿用看板既有实现：ksu.exec 只回传最后一行 → 一律走 readCmd（成组+base64+tr -d '\n'）；
   写文件：备份 → 同目录 tmp → chown/chmod 对齐原属主 → mv（rename 原子）→ 回读校验。

   运行时语义（判据来自 runtime/src/appopt.cpp 与 policy_manager.hpp::apply_profile）：
     · '#' 起注释；含 '|' 且首字段非空 = 画像行；其余行原样保留
     · 首字段 glob 匹配前台包名，**首个「命中且有效」的行生效** → 文件顺序 = 优先级
     · down/up：atoi 后 >= 0 才写入（-1 / 空 = 跟随档位）
     · 已知运行时坑：四字段行里 down/up 留空时，uS=atoi("…|cpuset")=0 会被当成“降频 0”
       → 本编辑器写回四字段行时统一把空的 down/up 补成 -1（等价语义，避免误改） */
(function () {
  var F = '/sdcard/Android/UnifiedRootOptimizer/app_profiles.txt';
  var GEN = '/data/adb/modules/AppOpt/applist.conf';
  var BIND = (typeof br !== 'undefined' && br) ? br
    : (typeof detectBridge === 'function' ? detectBridge() : null);

  function sh(cmd) { return BIND.exec(cmd).then(function (v) { return String(v == null ? '' : v); }); }
  function rd(cmd) { return (typeof readCmd === 'function') ? readCmd(BIND, cmd) : sh(cmd); }

  function b64e(str) {
    var bytes = new TextEncoder().encode(str), bin = '';
    for (var i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
    return btoa(bin);
  }

  /* ---------- 解析 / 回写 ---------- */
  function parse(text) {
    var items = [], lines = String(text || '').split('\n');
    for (var i = 0; i < lines.length; i++) {
      var t = lines[i].replace(/\r$/, '');
      var h = t.indexOf('#');
      var body = h >= 0 ? t.slice(0, h) : t;
      var f = body.split('|');
      if (!body.trim() || f.length < 2 || !f[0].trim()) {
        if (t.trim()) items.push({ kind: 'other', text: t });
        continue;
      }
      items.push({
        kind: 'rule',
        pattern: (f[0] || '').trim(),
        down: (f[1] || '').trim(),
        up: (f[2] || '').trim(),
        cpuset: (f[3] || '').trim(),
        note: (h >= 0 ? t.slice(h).trim() : '')
      });
    }
    return items;
  }
  function ruleLine(r) {
    var s;
    if (r.cpuset) s = r.pattern + '|' + (r.down || '-1') + '|' + (r.up || '-1') + '|' + r.cpuset;
    else if (r.up) s = r.pattern + '|' + (r.down || '') + '|' + r.up;
    else if (r.down) s = r.pattern + '|' + r.down;
    else s = r.pattern;
    if (r.note) s += ' ' + r.note;
    return s;
  }
  function buildText(list) {
    var ls = [], i;
    for (i = 0; i < list.length; i++) ls.push(list[i].kind === 'rule' ? ruleLine(list[i]) : list[i].text);
    while (ls.length && !ls[ls.length - 1].trim()) ls.pop();
    return ls.length ? ls.join('\n') + '\n' : '';
  }
  function parseGen(text) {
    var out = [], ls = String(text || '').split('\n');
    for (var i = 0; i < ls.length; i++) {
      var s = ls[i].trim();
      if (!s || s.charAt(0) === '#') continue;
      out.push(s);
    }
    return out;
  }

  /* ---------- 语义提示 / 校验 ---------- */
  var CORE = { 'e-core': '小核', 'p-core': '中核', 'hp-core': '大核', 'all-core': '全核' };
  function semRate(v) {
    var s = String(v || '').trim();
    if (!s || s === '-1') return '跟随档位';
    return s + 'μs 固定';
  }
  function semCpu(v) {
    var s = String(v || '').trim();
    if (!s) return '不生成核组规则';
    if (CORE[s]) return CORE[s];
    if (s.indexOf(',') >= 0) return s.split(',').map(function (x) { return CORE[x] || x; }).join(' + ');
    if (/^\d/.test(s)) return '核号 ' + s;
    return '自定义组';
  }
  function ruleWarn(r) {
    var w = [];
    if (r.pattern.indexOf('*') >= 0) w.push('通配画像：策略热读生效，指挥链② 保守跳过（不生成核组规则）');
    if (!r.down && !r.up && !r.cpuset) w.push('三字段全空 → 该行不产生任何效果');
    if (r.cpuset && (!r.down || !r.up)) w.push('四字段行里空的 down/up 会被运行时 atoi 成 0（误当“固定 0μs”）→ 保存后自动补 -1');
    return w.join(' · ');
  }
  function validate(f) {
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

  var fb = document.getElementById('pf-fallback');
  if (typeof Vue === 'undefined') { if (fb) fb.style.display = 'block'; return; }
  if (!BIND) {
    Vue.createApp({ template: '<div class="pf-msg err">未检测到 WebUI 桥（ksu / WebUIX / mmrl）</div>' }).mount('#pf-app');
    return;
  }

  Vue.createApp({
    setup: function () {
      var items = Vue.ref([]);
      var meta = Vue.ref(null);
      var genRules = Vue.ref([]);
      var genNote = Vue.ref('');
      var loading = Vue.ref(true);
      var busy = Vue.ref(false);
      var msg = Vue.ref('');
      var msgCls = Vue.ref('ok');
      var editIndex = Vue.ref(-1);
      var pendingDel = Vue.ref(-1);
      var form = Vue.ref({ pattern: '', down: '', up: '', cpuset: '' });

      var presets = ['e-core', 'p-core', 'hp-core', 'all-core'];
      var formErr = Vue.computed(function () { return validate(form.value); });
      var dirty = Vue.computed(function () {
        var f = form.value;
        return !!(f.pattern || f.down || f.up || f.cpuset);
      });
      var ruleRows = Vue.computed(function () {
        var rows = [], k = 0;
        items.value.forEach(function (it, i) {
          if (it.kind !== 'rule') return;
          rows.push({ it: it, i: i, k: k, warn: ruleWarn(it) });
          k++;
        });
        return rows;
      });
      var otherCount = Vue.computed(function () {
        return items.value.filter(function (i) { return i.kind !== 'rule'; }).length;
      });
      var fileState = Vue.computed(function () { return meta.value ? '已存在' : '不存在'; });
      var badgeCls = Vue.computed(function () { return meta.value ? 'ok' : 'warn'; });
      var metaLine = Vue.computed(function () {
        var m = meta.value;
        if (!m) return '文件不存在 = 无画像（合法状态，等同未加任何规则）';
        return m.size + ' B · ' + m.perm + ' · ' + m.owner + ' · ' + m.mtime;
      });

      function load() {
        if (busy.value) return Promise.resolve();
        loading.value = true;
        return Promise.all([
          rd('cat ' + F + ' 2>/dev/null'),
          sh('stat -c "%s|%u|%g|%a|%y" ' + F + ' 2>/dev/null'),
          rd('sed -n "/URO-GEN-BEGIN/,/URO-GEN-END/p" ' + GEN + ' 2>/dev/null')
        ]).then(function (r) {
          items.value = parse(r[0]);
          var st = String(r[1] || '').trim();
          if (/^\d+\|\d+\|\d+\|\d+\|/.test(st)) {
            var f = st.split('|');
            var t = (f[4] || '').trim().split(/\s+/);
            meta.value = {
              size: Number(f[0]), owner: f[1] + ':' + f[2], perm: f[3],
              mtime: (t[0] || '') + ' ' + (t[1] || '').slice(0, 5)
            };
          } else { meta.value = null; }
          genRules.value = parseGen(r[2]);
          genNote.value = genRules.value.length
            ? '以上来自本机 applist.conf 的 URO-GEN 区块（URO 自动维护，勿手改）'
            : '区块空 = 当前没有基于画像生成的核组规则（对账周期 60s）';
        }).catch(function (e) {
          msg.value = '读取失败：' + e; msgCls.value = 'err';
        }).then(function () { loading.value = false; });
      }

      function writeAll(text) {
        var b64 = b64e(text);
        var bak = F + '.uro_bak_' + Math.floor(Date.now() / 1000);
        var steps = [], f;
        if (meta.value) steps.push('cp -f ' + F + ' ' + bak);
        steps.push('echo ' + b64 + ' | base64 -d > ' + F + '.urotmp');
        if (meta.value) {
          f = meta.value.owner.split(':');
          steps.push('chown ' + f[0] + ':' + f[1] + ' ' + F + '.urotmp');
          steps.push('chmod ' + meta.value.perm + ' ' + F + '.urotmp');
        }
        steps.push('mv -f ' + F + '.urotmp ' + F);

        var chain = Promise.resolve(true);
        steps.forEach(function (c) {
          chain = chain.then(function (ok) {
            return sh(c + ' && echo ok || echo err').then(function (o) {
              return ok && String(o).indexOf('err') !== 0;
            });
          });
        });
        return chain.then(function (ok) {
          if (!ok) return { ok: false, bak: '' };
          sh('ls -1t ' + F + '.uro_bak_* 2>/dev/null | sed -n "4,$p" | while read x; do rm -f "$x"; done');
          return rd('cat ' + F + ' 2>/dev/null').then(function (back) {
            return { ok: back === text, bak: meta.value ? bak.split('/').pop() : '' };
          });
        });
      }

      function commit(next, okMsg, failMsg) {
        busy.value = true; msg.value = '';
        return writeAll(buildText(next)).then(function (res) {
          busy.value = false;
          if (!res.ok) {
            msg.value = failMsg || '写入失败或回读不一致 —— 文件未被可靠替换，请点右上角刷新确认';
            msgCls.value = 'err';
            return load();
          }
          items.value = next;
          pendingDel.value = -1;
          msg.value = okMsg + (res.bak ? '（备份 ' + res.bak + '）' : '') +
            ' · 前三字段热读即时生效；第四字段 60s 内由指挥链② 对账生成核组规则';
          msgCls.value = 'ok';
          load();                                            // 立刻回读：刷新 meta（有文件后下次写才备份）与 URO-GEN 显示
          setTimeout(function () { load(); }, 65000);         // 60s 对账后再看一眼生成结果
        }).catch(function (err) {
          busy.value = false;
          msg.value = '写入异常：' + err; msgCls.value = 'err';
        });
      }

      function submit() {
        var e = validate(form.value);
        if (e) { msg.value = e; msgCls.value = 'err'; return; }
        var next = items.value.slice();
        var draft = {
          kind: 'rule',
          pattern: form.value.pattern.trim(),
          down: form.value.down.trim(),
          up: form.value.up.trim(),
          cpuset: form.value.cpuset.trim(),
          note: ''
        };
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
        pendingDel.value = -1;
        msg.value = '';
      }

      // KSU WebView 不保证实现 onJsConfirm（confirm() 可能直接返回 false）→ 用两次点击确认
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
        editIndex.value = -1;
        pendingDel.value = -1;
        form.value = { pattern: '', down: '', up: '', cpuset: '' };
        msg.value = '';
      }

      Vue.onMounted(function () {
        // 守卫：WebView 会话恢复有回填 DOM 值的可能（回填不触发 input 事件）。
        // 真机一轮未复现（当时是占位符误读），成本极低故保留，避免模型与输入框不一致。
        ['pf-pat', 'pf-down', 'pf-up', 'pf-cpu'].forEach(function (id, k) {
          var el = document.getElementById(id);
          var key = ['pattern', 'down', 'up', 'cpuset'][k];
          if (el && el.value && !form.value[key]) form.value[key] = el.value.trim();
        });
        load();
      });

      return {
        items: items, meta: meta, genRules: genRules, genNote: genNote, loading: loading, busy: busy,
        msg: msg, msgCls: msgCls, editIndex: editIndex, pendingDel: pendingDel, form: form,
        presets: presets, formErr: formErr, dirty: dirty, ruleRows: ruleRows, otherCount: otherCount,
        fileState: fileState, badgeCls: badgeCls, metaLine: metaLine, semRate: semRate, semCpu: semCpu,
        load: load, submit: submit, edit: edit, del: del, move: move, reset: reset
      };
    }
  }).mount('#pf-app');
})();
