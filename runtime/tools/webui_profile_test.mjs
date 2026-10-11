// 离线假桥测试（看板画像编辑器）：把 module/webroot/index.html 里的画像脚本跑在内存文件系统上，
// 不需要设备、不需要 KSU 桥。用法：node runtime/tools/webui_profile_test.mjs
// 覆盖：解析 / 新增 / 编辑 / 删除两击 / 上下移 / -1 补位 / 备份 / 回读校验 / 错误分支
import fs from 'node:fs';

const F = '/sdcard/Android/UnifiedRootOptimizer/app_profiles.txt';   // 假桥里的“磁盘”路径（不落真盘）
const GEN = '/data/adb/modules/AppOpt/applist.conf';
let file = null;              // 初始：文件不存在（合法状态）
let gen = '# ---- URO-GEN-BEGIN ----\n# ---- URO-GEN-END ----\n';
let backups = [];
let failNext = null;          // 模拟某步命令失败
const log = [];

const metaLine = () => file === null ? '' :
  `${Buffer.byteLength(file)}|10296|1023|660|2026-10-11 09:45:03.000000000 +0800`;

function runInner(cmd) {
  if (failNext && cmd.includes(failNext)) { failNext = null; return { out: '', ok: false }; }
  if (cmd.startsWith('cat ' + F)) return { out: file === null ? '' : file, ok: true };
  if (cmd.startsWith('stat -c')) return { out: metaLine(), ok: file !== null };
  if (cmd.startsWith('sed -n')) return { out: gen, ok: true };
  if (cmd.startsWith('cp -f ' + F + ' ' + F + '.uro_bak_')) { backups.push(cmd.split(' ')[3]); return { out: '', ok: true }; }
  if (cmd.startsWith('echo ') && cmd.includes('base64 -d > ' + F + '.urotmp')) {
    const b64 = cmd.slice(5, cmd.indexOf(' | base64 -d')).trim();
    globalThis.__tmp = Buffer.from(b64, 'base64').toString('utf-8');
    return { out: '', ok: true };
  }
  if (cmd.startsWith('chown ') || cmd.startsWith('chmod ')) return { out: '', ok: true };
  if (cmd.startsWith('mv -f ' + F + '.urotmp ' + F)) { file = globalThis.__tmp; return { out: '', ok: true }; }
  if (cmd.startsWith('ls -1t ' + F + '.uro_bak_')) return { out: '', ok: true };
  return { out: '', ok: false };
}

// 浏览器环境最小替身（app.js 用到 window/document/localStorage/matchMedia）
globalThis.window = globalThis;
globalThis.document = {
  getElementById: () => ({ style: {}, textContent: '', value: '' }),
  querySelector: () => null,
  addEventListener: () => {},
  documentElement: {},
  hidden: false
};
globalThis.addEventListener = () => {};
globalThis.scrollTo = () => {};
globalThis.matchMedia = () => ({ matches: false, addEventListener: () => {} });
globalThis.localStorage = { getItem: () => null, setItem: () => {} };
// 假桥的 exec：读命令走 base64 通道（模拟 ksu.exec），写命令带 && echo ok/err
async function execFake(cmd) {
  log.push(cmd);
  if (cmd.startsWith('(')) {                      // 成组 + base64 通道
    const inner = cmd.slice(1, cmd.lastIndexOf(') | base64'));
    const r = runInner(inner);
    return Buffer.from(r.out, 'utf-8').toString('base64');
  }
  const m = cmd.match(/^(.*) && echo ok \|\| echo err$/s);
  const r = runInner(m ? m[1] : cmd);
  return m ? (r.ok ? 'ok' : 'err') : r.out;
}
globalThis.ksu = { exec: execFake };
globalThis.br = globalThis.ksu;
globalThis.readCmd = async (_br, cmd) => {
  const out = await _br.exec('(' + cmd + ') | base64 | tr -d "\\n"');
  return out ? Buffer.from(out.trim(), 'base64').toString('utf-8') : '';
};

let api = null;
globalThis.Vue = {
  ref: (v) => ({ value: v }),
  computed: (fn) => ({ get value() { return fn(); } }),
  nextTick: (fn) => Promise.resolve().then(fn),
  onMounted: (fn) => { globalThis.__mounted = fn; },
  createApp: (o) => ({ mount: () => { api = o.setup(); globalThis.__mounted && globalThis.__mounted(); } })
};

// v0.30.0：看板已拆成多文件，画像逻辑在 module/webroot/app.js 里
const src = fs.readFileSync(new URL('../../module/webroot/app.js', import.meta.url), 'utf-8');
console.log('载入 app.js（' + src.length + ' 字符）');
eval(src);
const wait = () => new Promise(r => setTimeout(r, 30));
await wait();

const PF = api.pf;            // v0.30.0：画像子系统挂在 app.pf 下（一组 ref）
const ok = [], bad = [];
const t = (name, cond, extra = '') => (cond ? ok : bad).push(name + (extra ? ' :: ' + extra : ''));

t('初始挂载 = 无画像', api !== null && PF.meta.value === null && PF.ruleRows.value.length === 0);

// 1) 新增（cpuset 有值、速率留空）→ 四字段且空速率补 -1
PF.form.value = { pattern: 'com.coolapk.market', down: '', up: '', cpuset: 'p-core' };
PF.submit();
await wait();
t('新增写回（补齐 -1）', file === 'com.coolapk.market|-1|-1|p-core\n', JSON.stringify(file));
t('首写无备份（原来文件不存在）', backups.length === 0, JSON.stringify(backups));
t('新增后无错误消息', PF.msgCls.value === 'ok', PF.msg.value);

// 2) 第二条 + 校验拦截
PF.form.value = { pattern: 'com.eg.android.AlipayGphone', down: '8000', up: '-1', cpuset: '' };
PF.submit();
await wait();
t('第二条写回（3 字段）', file === 'com.coolapk.market|-1|-1|p-core\ncom.eg.android.AlipayGphone|8000|-1\n',
  JSON.stringify(file));
t('已有文件时产生备份', backups.length === 1, JSON.stringify(backups));

PF.form.value = { pattern: 'bad pkg', down: '', up: 'x', cpuset: 'p-core' };
t('空白包名被拦', PF.formErr.value.includes('空白'));
PF.form.value = { pattern: 'com.a', down: '-5', up: '', cpuset: '' };
t('down<-1 被拦', PF.formErr.value.includes('不能小于 -1'));
PF.form.value = { pattern: 'com.a', down: '', up: '', cpuset: 'p core' };
t('cpuset 空格被拦', PF.formErr.value.includes('cpuset'));
PF.form.value = { pattern: 'com.a|b', down: '', up: '', cpuset: '' };
t('包名含 | 被拦', PF.formErr.value.includes('不能含'));

PF.form.value = { pattern: 'com.only', down: '', up: '', cpuset: '' };
t('空字段行被拦', PF.formErr.value.includes('至少要填一个字段'));

PF.form.value = { pattern: 'com.x.*', down: '2000', up: '', cpuset: '' };
t('通配通过校验', PF.formErr.value === '');
PF.submit();
await wait();
const wild = PF.ruleRows.value.filter(r => r.it.pattern === 'com.x.*')[0];
t('通配行带告警', !!wild && wild.warn.includes('通配'), JSON.stringify(file.split('\n')));

// 3) 非画像行（注释/其他）保留 + 行内注释保留
file = '# 头部注释\ncom.foo=0-3\ncom.bar|5000|9000|e-core  # 尾注\n';
await PF.load();
t('解析：1 条画像 + 2 其他行', PF.ruleRows.value.length === 1 && PF.otherCount.value === 2,
  JSON.stringify([PF.ruleRows.value.length, PF.otherCount.value]));
t('行内注释被保留', PF.ruleRows.value[0].it.note === '# 尾注');
t('文件元信息解析', PF.meta.value && PF.meta.value.perm === '660' &&
  PF.meta.value.owner === '10296:1023' && PF.meta.value.mtime === '2026-10-11 09:45',
  JSON.stringify(PF.meta.value));
PF.form.value = { pattern: 'com.new', down: '-1', up: '-1', cpuset: 'hp-core' };
PF.submit();
await wait();
t('写回保留注释/其他行', file ===
  '# 头部注释\ncom.foo=0-3\ncom.bar|5000|9000|e-core # 尾注\ncom.new|-1|-1|hp-core\n', JSON.stringify(file));

// 4) 老格式 a|||c：告警 + 一轮写回后补 -1（运行时 atoi 坑）
file = 'com.old|||p-core\n';
await PF.load();
t('四字段空速率告警', PF.ruleRows.value[0].warn.includes('atoi'));
PF.form.value = { pattern: 'com.old2', down: '500', up: '', cpuset: '' };
PF.submit();
await wait();
t('空速率被补 -1', file === 'com.old|-1|-1|p-core\ncom.old2|500\n', JSON.stringify(file));

// 5) 上下移（文件顺序=优先级）
file = 'a.pkg|1\nb.pkg|2\n';
await PF.load();
PF.move(0, 1);
await wait();
t('下移交换顺序', file === 'b.pkg|2\na.pkg|1\n', JSON.stringify(file));
PF.move(1, 1);                                   // 边界：已是最后一行 → 不动
t('边界不动', file === 'b.pkg|2\na.pkg|1\n', JSON.stringify(file));

// 6) 删除：两击确认
PF.del(0);
t('首击只进待确认', file === 'b.pkg|2\na.pkg|1\n' && PF.pendingDel.value === 0, JSON.stringify(file));
PF.del(0);
await wait();
t('二击删除并写回', file === 'a.pkg|1\n', JSON.stringify(file));

// 7) 编辑
PF.edit(0);
t('编辑预填', PF.form.value.pattern === 'a.pkg' && PF.editIndex.value === 0);
PF.form.value = { pattern: 'a.pkg', down: '3000', up: '', cpuset: 'e-core' };
PF.submit();
await wait();
t('编辑写回（沿用行内注释位）', file === 'a.pkg|3000|-1|e-core\n', JSON.stringify(file));
t('提交后表单复位', PF.editIndex.value === -1 && PF.form.value.pattern === '');

// 8) 写失败分支：mv 失败 → 报错且不吞异常
file = 'keep.pkg|1\n';
await PF.load();
failNext = 'mv -f';
PF.form.value = { pattern: 'new.pkg', down: '1', up: '', cpuset: '' };
PF.submit();
await wait();
t('写失败被识别', PF.msgCls.value === 'err' && PF.msg.value.includes('写入失败'), PF.msg.value);
t('失败时磁盘内容未被破坏', file === 'keep.pkg|1\n', JSON.stringify(file));

// 9) 删空 → 空文件
file = 'only.pkg|1\n';
await PF.load();
PF.del(0); PF.del(0);
await wait();
t('删空后文件为空串', file === '', JSON.stringify(file));

console.log('PASS ' + ok.length + ' / FAIL ' + bad.length);
ok.forEach(x => console.log('  ✔ ' + x));
bad.forEach(x => console.log('  ✘ ' + x));
console.log('shell calls: ' + log.length);
process.exit(bad.length ? 1 : 0);
