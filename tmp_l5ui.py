#!/usr/bin/env python3
p = '/workspace/cos-sched-memory/module/webroot/index.html'
s = open(p, encoding='utf-8').read()

# 1) tel 段 tail 从 14 → 200（时间线仍只用尾部；洞察卡需要更多样本）
old1 = "'echo @@SEG:tel@@;tail -14 '+L+'/log/telemetry.jsonl 2>/dev/null',"
assert old1 in s, 'tel'
s = s.replace(old1, "'echo @@SEG:tel@@;tail -200 '+L+'/log/telemetry.jsonl 2>/dev/null',", 1)

# 2) HTML：Evidence 卡后加洞察卡
old2 = '<div class="card" id="evid"><div class="row"><span class="val">读取中…</span></div></div>'
assert old2 in s, 'evid'
s = s.replace(old2, old2 + """

<div class="sec"><svg class="si si-sec" viewBox="0 0 24 24"><use href="#i-progress-fill"/></svg>L5 洞察（只读）</div>
<div class="card" id="ins"><div class="row"><span class="val">读取中…</span></div></div>""", 1)

# 3) 渲染：洞察统计（最近 200 条决策）
old3 = "      }catch(e){}\n  }catch(e){fail('读取失败: '+e)}"
assert old3 in s, 'render tail'
s = s.replace(old3, """      }catch(e){}
    // L5 洞察（只读统计：档位占比 + Top App；不改任何策略）
    try{
      const rs=(st_||'').split('\\n').filter(l=>l.trim().startsWith('{')).map(l=>{try{return JSON.parse(l)}catch{return null}}).filter(Boolean);
      const sc={}, fg={};
      rs.forEach(r=>{ sc[r.scenario]=(sc[r.scenario]||0)+1;
        if(r.fg&&r.fg!=='(unknown)') fg[r.fg]=(fg[r.fg]||0)+1; });
      const tot=rs.length||1;
      const order=['PERFORMANCE','BALANCE','FAST','POWER_SAVE','GAME','MEMORY_PRESSURE','BOOT'];
      const dist=order.filter(k=>sc[k]).map(k=>k.replace('_','')+' '+Math.round(sc[k]*100/tot)+'%').join(' · ');
      const top=Object.entries(fg).sort((a,b)=>b[1]-a[1]).slice(0,3)
        .map(([k,v])=>String(k).split('.').pop()+' '+Math.round(v*100/tot)+'%').join(' · ');
      document.getElementById('ins').innerHTML=
        '<div class="row"><span class="name">档位分布</span><span class="val">'+esc(dist||'-')+'</span></div>'+
        '<div class="row"><span class="name">Top App</span><span class="val">'+esc(top||'-')+'</span></div>'+
        '<div class="row"><span class="name">样本</span><span class="val">'+tot+' 条决策（≈最近'+Math.round(tot/4)+'分钟）</span></div>';
    }catch(e){}
  }catch(e){fail('读取失败: '+e)}""", 1)
open(p, 'w', encoding='utf-8').write(s)
print('patched', len(s.encode()))
