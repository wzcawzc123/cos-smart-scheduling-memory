
// ===== 桥（复用 COSMemory 面板 composables/ksu.ts 实现要点）=====
function detectBridge(){
  const w=globalThis;
  for(const o of [w.ksu,w.WebUIX,w.mmrl]) if(o&&typeof o.exec==='function') return {exec:async c=>String(await o.exec(c)??'')};
  if(w.ksu&&typeof w.ksu.execAsync==='function') return {exec:c=>new Promise((res,rej)=>{try{w.ksu.execAsync(c,r=>res(r??''))}catch(e){rej(e)}})};
  return null;
}
function b64d(s){s=s.replace(/\s+/g,'');if(!s)return'';const bin=atob(s);return new TextDecoder('utf-8').decode(Uint8Array.from(bin,ch=>ch.charCodeAt(0)))}
async function readCmd(br,cmd){
  // 成组再管道（避免 || 短路明文直出）；base64 保多行安全
  const out=await br.exec('('+cmd+') | base64 | tr -d "\\n"');
  if(!out.trim())return'';
  try{return b64d(out)}catch{return''}
}
function splitSeg(d){const p=d.split(/@@SEG:([A-Za-z0-9_-]+)@@/g),o={};for(let i=1;i<p.length;i+=2)o[p[i]]=(p[i+1]??'').trim();return o}
const G=k=>(typeof k==='string'?k:'');   // 段缺失防御（undefined 不炸）

const L='/sdcard/Android/UnifiedRootOptimizer';
// 数组 join(';')：每段不带尾分号，避免拼出 ';;'（sh 语法错误 → 整条命令失败）
const batch=[
  'echo @@SEG:prop@@;cat /data/adb/modules/uro/module.prop 2>/dev/null',
  'echo @@SEG:conf@@;cat '+L+'/uro.conf 2>/dev/null',
  'echo @@SEG:tel@@;tail -200 '+L+'/log/telemetry.jsonl 2>/dev/null',
  'echo @@SEG:bridge@@;cat '+L+'/log/bridge.state 2>/dev/null',
  'echo @@SEG:cpu@@;cat '+L+'/log/cpu.state 2>/dev/null',
  'echo @@SEG:wd@@;tail -3 '+L+'/log/watchdog.log 2>/dev/null',
  'echo @@SEG:json@@;grep -oE "\\"(aggressive|depth|cooldownSec)\\": [^,}]*" /data/adb/modules/COSMemory/config/memory.json 2>/dev/null',
  'echo @@SEG:uag@@;for p in 0 3 7;do echo -n p$p=;cat /sys/devices/system/cpu/cpufreq/policy$p/uag/up_rate_limit_us 2>/dev/null;echo;done',
  'echo @@SEG:pw@@;tail -1 '+L+'/log/power.jsonl 2>/dev/null',
  'echo @@SEG:th@@;tail -1 '+L+'/log/thermal.jsonl 2>/dev/null',
  'echo @@SEG:fps@@;tail -3 '+L+'/log/fps.jsonl 2>/dev/null',
  'echo @@SEG:thr@@;tail -1 '+L+'/log/thread.jsonl 2>/dev/null',
  'echo @@SEG:gpu@@;tail -1 '+L+'/log/gpu.jsonl 2>/dev/null',
  'echo @@SEG:hi@@;cat /sys/devices/system/cpu/cpufreq/policy0/uag/hispeed_load 2>/dev/null'
].join(';');

const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const LVICON={POWER_SAVE:'#i-smartphone-fill',BALANCE:'#i-smartphone-fill',PERFORMANCE:'#i-cpu-fill',FAST:'#i-sparkling-fill',GAME:'#i-gamepad-fill',MEMORY_PRESSURE:'#i-refresh-fill',BOOT:'#i-sparkling-fill'};
const LVL={POWER_SAVE:'ps',BALANCE:'bl',PERFORMANCE:'pf',FAST:'pf',GAME:'gs',MEMORY_PRESSURE:'ps',BOOT:'bl'};

let br=null, armed=false;
function fail(m){const e=document.getElementById('err');e.style.display='block';e.textContent=m}

async function refresh(){
  if(!br){fail('未检测到 WebUI 桥（ksu/WebUIX/mmrl）');return}
  try{
    const seg=splitSeg(await readCmd(br,batch));
    const sp=G(seg.prop), sc=G(seg.conf), st_=G(seg.tel), sb=G(seg.bridge), scpu=G(seg.cpu);
    document.getElementById('ver').textContent=((sp.match(/version=(.*)/)||[])[1]||'')+' · '+
      ((sp.match(/description=(.*)/)||[])[1]||'');
    // 当前档 = 最后一条 telemetry
    const rows=(st_||'').split('\n').filter(l=>l.trim().startsWith('{')).map(l=>{try{return JSON.parse(l)}catch{return null}}).filter(Boolean);
    const cur=rows[rows.length-1];
    if(cur){
      const lv=document.getElementById('lvl');
      lv.textContent=cur.scenario;
      const iu=document.getElementById('liu'); if(iu&&LVICON[cur.scenario]) iu.setAttribute('href',LVICON[cur.scenario]);
      lv.className='t '+(LVL[cur.scenario]||'bl');
      document.getElementById('why').textContent=(cur.why||'')+(cur.handover?' · 撤权中':'');
    }
    // enforce 总闸
    const en=(sc.match(/BRIDGE_ENFORCE=(\d)/)||[])[1]==='1';
    armed=en;
    document.getElementById('ent').textContent=en?'ON':'OFF';
    document.getElementById('ent').className='pill '+(en?'ok':'');
    document.getElementById('sw').className='st-switch'+(en?' on':'');
    // 基线 / 看门狗 → 顶部状态点
    const bd=(sb.match(/DIRTY=(\w+)/)||[])[1]||'?';
    const cd=(scpu.match(/DIRTY=(\w+)/)||[])[1]||'?';
    const clean=(bd==='false'&&cd==='false');
    document.getElementById('base').innerHTML=clean?'<span class="pill ok">CLEAN</span>':
      '<span class="pill warn">DIRTY（内存:'+esc(bd)+' / CPU:'+esc(cd)+'）</span>';
    const wlines=G(seg.wd).split('\n').filter(Boolean);
    const last=wlines[wlines.length-1]||'';
    const wdOk=!last.includes('halt');
    document.getElementById('wd').innerHTML=wdOk?'<span class="pill ok">正常</span>':'<span class="pill bad">HALT</span>';
    document.getElementById('hdot').className='dot '+(clean&&wdOk?'on':'warn');
    // 时间线
    document.getElementById('tl').innerHTML=rows.slice(-8).reverse().map(r=>{
      const t=new Date(r.ts*1000);
      const hh=String(t.getHours()).padStart(2,'0')+':'+String(t.getMinutes()).padStart(2,'0')+':'+String(t.getSeconds()).padStart(2,'0');
      return '<li><time>'+hh+'</time><b class="'+(LVL[r.scenario]||'bl')+'">'+esc(r.scenario)+'</b><span title="'+esc(r.why)+'">'+esc(r.why||'')+'</span></li>';
    }).join('')||'<li>暂无记录</li>';
    // 写入证据
    const j=G(seg.json).replace(/"/g,'').replace(/:\s*/g,' = ');
    const u=G(seg.uag).replace(/\s+/g,' · ').trim();
    document.getElementById('ev').innerHTML=
      '<div class="row"><span class="name">memory.json</span><span class="val">'+esc(j||'-')+'</span></div>'+
      '<div class="row"><span class="name">uag up_rate</span><span class="val">'+esc(u||'-')+'</span></div>'+
      '<div class="row"><span class="name">最新 action_id</span><span class="val">'+esc(cur?cur.action_id:'-')+'</span></div>';
    // Evidence 采样（power/thermal/fps/thread 四源，60s 落盘 5s 展示）
    try{
      const pw=JSON.parse(G(seg.pw)||'{}'), th=JSON.parse(G(seg.th)||'{}');
      const frows=(G(seg.fps)||'').split('\n').filter(l=>l.trim().startsWith('{')).map(l=>{try{return JSON.parse(l)}catch{return null}}).filter(Boolean);
      const thr=JSON.parse(G(seg.thr)||'{}');
      const chgMap={Discharging:'放电',Charging:'充电',Full:'满电'};
      const bat=(th.battery!=null&&th.battery>-999)?th.battery+'°':'-';
      const top=(th.top||[]).map(t=>t.t+' '+t.c+'°').join(' · ')||'-';
      const pwr=(pw.pct!=null)?(pw.pct+'% · '+(chgMap[pw.chg]||pw.chg||'?')+' '+(pw.mA!=null&&pw.mA!==-1?pw.mA+'mA':'')):'采样中';
      let fpsLine='-';
      const lf=frows[frows.length-1];
      if(lf){ if(lf.reset){fpsLine='基线重置 '+String(lf.pkg||'').slice(0,28)}
        else{const dt=lf.frames||0; fpsLine=Math.round(dt/60)+' fps · jank '+(dt?((lf.janky||0)*100/dt).toFixed(1):'0')+'%';} }
      const rules=thr.present?(thr.rules+' 规则 · 组 '+Object.keys(thr.groups||{}).join('/')):'未发现 AppOpt';
      document.getElementById('evid').innerHTML=
        '<div class="row"><span class="name">电量</span><span class="val">'+esc(pwr)+' · 电池 '+esc(bat)+'</span></div>'+
        '<div class="row"><span class="name">CPU 热点</span><span class="val">'+esc(top)+'</span></div>'+
        '<div class="row"><span class="name">帧率(60s窗)</span><span class="val">'+esc(fpsLine)+'</span></div>'+
        '<div class="row"><span class="name">AppOpt</span><span class="val">'+esc(rules)+'</span></div>';
      // v0.27：GPU 观测 + CPU 调参（hispeed_load）两行
      try{
        const gp=JSON.parse(G(seg.gpu)||'{}');
        const hi=(G(seg.hi)||'').trim();
        let sentryTag='';
        if(lf&&lf.sentry) sentryTag=' · <b style="color:#c0392b">⚠jank高</b>';
        document.getElementById('evid').innerHTML +=
          '<div class="row"><span class="name">GPU</span><span class="val">'+
            esc((gp.gpuBusy!=null?gp.gpuBusy+'%':'?')+' · '+(gp.gpuClkMhz!=null?gp.gpuClkMhz+'MHz':'?')+
            ' · idle '+(gp.gpuIdleTimer!=null?gp.gpuIdleTimer:'?'))+sentryTag+'</span></div>'+
          '<div class="row"><span class="name">CPU 调参</span><span class="val">'+
            esc('hispeed_load '+(hi||'?')+' · up 2500us')+'</span></div>';
      }catch(e){}
    }catch(e){}
    // L5 洞察（只读统计：档位占比 + Top App；不改任何策略）
    try{
      const rs=(st_||'').split('\n').filter(l=>l.trim().startsWith('{')).map(l=>{try{return JSON.parse(l)}catch{return null}}).filter(Boolean);
      const sc={}, fg={};
      rs.forEach(r=>{ sc[r.scenario]=(sc[r.scenario]||0)+1;
        if(r.fg&&r.fg!=='(unknown)') fg[r.fg]=(fg[r.fg]||0)+1; });
      const tot=rs.length||1;
      const order=['PERFORMANCE','BALANCE','FAST','POWER_SAVE','GAME','MEMORY_PRESSURE','BOOT'];
      const dist=order.filter(k=>sc[k]).map(k=>k.replace('_','')+' '+Math.round(sc[k]*100/tot)+'%').join(' · ');
      const top=Object.entries(fg).sort((a,b)=>b[1]-a[1]).slice(0,3)
        .map(([k,v])=>String(k).split('.').pop()+' '+Math.round(v*100/tot)+'%').join(' · ');
      const el=document.getElementById('ins');
      if(el) el.innerHTML=
        '<div class="row"><span class="name">档位分布</span><span class="val">'+esc(dist||'-')+'</span></div>'+
        '<div class="row"><span class="name">Top App</span><span class="val">'+esc(top||'-')+'</span></div>'+
        '<div class="row"><span class="name">样本</span><span class="val">'+tot+' 条决策</span></div>';
    }catch(e){}
  }catch(e){fail('读取失败: '+e)}
}

// 总闸：开启需二次确认（与 COSMemory 面板交互一致），关闭直接执行
let confirmT=null;
document.getElementById('sw').addEventListener('click',async()=>{
  if(!br){fail('未检测到 WebUI 桥');return}
  const swEl=document.getElementById('sw');
  if(armed){ // 关闭
    await br.exec('echo BRIDGE_ENFORCE=0 > '+L+'/uro.conf');
    setTimeout(refresh,300); return;
  }
  if(!confirmT){ // 开启：进入确认态
    swEl.classList.add('arming');
    document.getElementById('ent').textContent='再点一次确认';
    document.getElementById('ent').className='pill warn';
    confirmT=setTimeout(()=>{confirmT=null;refresh()},2500); return;
  }
  clearTimeout(confirmT); confirmT=null;
  await br.exec('echo BRIDGE_ENFORCE=1 > '+L+'/uro.conf');
  setTimeout(refresh,300);
});

br=detectBridge();
refresh();
setInterval(refresh,5000);
