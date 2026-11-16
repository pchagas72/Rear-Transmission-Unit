#include "http_telem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "can_mgmt.h"
#include "telem_store.h"
#include "wifi_ap.h"
#include "rtu_cfg.h"
#include "rtu_diag.h"
#include "rtu_health.h"
#include "rtu_fw.h"
#include "system_fsm.h"
#include "sd_log.h"

static const char *TAG = "HTTP_TELEM";
static http_cmd_fn s_cmd_cb;
static httpd_handle_t s_server;

/*
 * Uma página, duas views (Dados | Diag) via botão — menos flash que 2 HTML.
 * CSS/JS compartilhados; poll /api/json + /api/diag.
 */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html lang=pt-BR><head>"
"<meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<meta name=theme-color content=#0b0f14><title>Mangue RTU</title>"
"<style>"
":root{--bg:#0b0f14;--panel:#141a22;--line:#243041;--fg:#e8eef5;--mut:#8a9aab;"
"--ok:#2fd67b;--warn:#f0a020;--bad:#ef5a5a;--acc:#3db8ff;--kill:#c62828;--go:#1b7a45}"
"*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--fg);"
"font:14px/1.35 system-ui,sans-serif}"
".top{display:flex;align-items:center;justify-content:space-between;gap:.5rem;"
"padding:.65rem .85rem;border-bottom:1px solid var(--line);background:#0e1319;"
"position:sticky;top:0;z-index:5}"
".brand{font-weight:700;letter-spacing:.04em}.brand small{display:block;color:var(--mut);"
"font-weight:500;font-size:.68rem;letter-spacing:.08em;text-transform:uppercase}"
".live{display:flex;align-items:center;gap:.4rem;font-size:.72rem;color:var(--mut)}"
".dot{width:.5rem;height:.5rem;border-radius:50%;background:var(--bad)}.dot.on{background:var(--ok)}"
".tabs{display:flex;gap:.35rem}"
".tabs button{border:1px solid var(--line);background:#1a222c;color:var(--mut);"
"border-radius:8px;padding:.4rem .7rem;font:600 .72rem inherit}"
".tabs button.on{color:var(--fg);border-color:var(--acc)}"
"main{padding:.75rem;padding-bottom:5.2rem;display:grid;gap:.65rem}"
".view{display:none}.view.on{display:grid;gap:.65rem}"
".hero{display:grid;grid-template-columns:1fr 1fr;gap:.55rem}"
".g{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:.7rem .85rem}"
".g .l{color:var(--mut);font-size:.65rem;text-transform:uppercase;letter-spacing:.08em}"
".g .n{font-size:2.1rem;font-weight:700;font-variant-numeric:tabular-nums;line-height:1.1;margin:.1rem 0}"
".g .u{color:var(--mut);font-size:.72rem}"
".row{display:grid;grid-template-columns:repeat(2,1fr);gap:.55rem}"
"@media(min-width:520px){.row{grid-template-columns:repeat(4,1fr)}}"
".m .n{font-size:1.3rem}"
".bar{height:.3rem;background:#1c2530;border-radius:99px;overflow:hidden;margin-top:.35rem}"
".bar>i{display:block;height:100%;width:0;background:var(--acc)}"
".bar.warn>i{background:var(--warn)}.bar.hot>i{background:var(--bad)}"
".sec{color:var(--mut);font-size:.65rem;text-transform:uppercase;letter-spacing:.1em}"
".chips{display:flex;flex-wrap:wrap;gap:.35rem}"
".chip{padding:.3rem .55rem;border-radius:999px;background:#1a222c;border:1px solid var(--line);"
"font-size:.7rem;font-weight:600}.chip.on{border-color:var(--ok);color:var(--ok)}"
".chip.off{border-color:var(--bad);color:var(--bad)}.chip.warn{border-color:var(--warn);color:var(--warn)}"
".chip.neu{color:var(--mut)}"
".meta{font-size:.75rem;color:var(--mut);font-variant-numeric:tabular-nums}"
"table{width:100%;border-collapse:collapse;font-size:.75rem;font-variant-numeric:tabular-nums}"
"th,td{text-align:left;padding:.35rem .4rem;border-bottom:1px solid var(--line)}"
"th{color:var(--mut);font-weight:600;font-size:.65rem;text-transform:uppercase}"
"td.ok{color:var(--ok)}td.bad{color:var(--bad)}td.warn{color:var(--warn)}"
".dock{position:fixed;left:0;right:0;bottom:0;padding:.55rem .75rem calc(.55rem + env(safe-area-inset-bottom));"
"background:linear-gradient(180deg,transparent,#0b0f14 28%);display:grid;"
"grid-template-columns:repeat(3,1fr);gap:.35rem}"
".dock button{border:0;border-radius:9px;padding:.55rem .2rem;font:600 .65rem/1.1 inherit;color:#fff;background:#243040}"
".dock .go{background:var(--go)}.dock .kill{background:var(--kill)}"
".dock .led{background:#1b5e7a}.dock .ledoff{background:#4a5560}"
".sna{color:var(--bad)!important;font-size:1.1rem!important}"
".gloss{font-size:.68rem;color:var(--mut);line-height:1.45;columns:2;column-gap:.85rem}"
".gloss b{color:var(--fg);font-weight:600}"
".tok{display:flex;gap:.35rem;flex-wrap:wrap;align-items:center}"
".tok input{flex:1;min-width:8rem;background:#1a222c;border:1px solid var(--line);"
"border-radius:8px;color:var(--fg);padding:.4rem .55rem;font:inherit}"
".tok button{border:1px solid var(--line);background:#1a222c;color:var(--fg);"
"border-radius:8px;padding:.4rem .7rem;font:600 .72rem inherit}"
".sdact{display:flex;flex-wrap:wrap;gap:.35rem;margin-bottom:.45rem}"
".sdact button{border:1px solid var(--line);background:#1a222c;color:var(--fg);"
"border-radius:8px;padding:.4rem .7rem;font:600 .72rem inherit}"
".sdact .bad{border-color:var(--bad);color:var(--bad)}"
".sdrow{display:flex;align-items:center;justify-content:space-between;gap:.5rem;"
"padding:.2rem 0;border-bottom:1px solid var(--line)}"
".sdrow a{color:var(--acc);text-decoration:none;font-weight:600}"
".sdrow .sz{color:var(--mut);font-size:.7rem;margin-left:.35rem}"
".sdrow .era{color:var(--bad);background:0;border:0;padding:0;font:600 .72rem inherit;"
"cursor:pointer;text-decoration:underline}"
"</style></head><body>"
"<header class=top>"
"<div class=brand>MANGUE<small>RTU · " WIFI_AP_SSID "</small></div>"
"<div class=tabs>"
"<button type=button id=bDat class=on>Dados</button>"
"<button type=button id=bDiag>Diag</button>"
"</div>"
"<div class=live><span class=dot id=dot></span><span id=st>…</span></div>"
"</header>"
"<main>"
"<section class=\"view on\" id=vDat>"
"<div class=hero>"
"<div class=g><div class=l>Velocidade</div><div class=n id=spd>—</div><div class=u>km/h</div></div>"
"<div class=g><div class=l>RPM</div><div class=n id=rpm>—</div><div class=u>motor</div></div>"
"</div>"
"<div class=sec>Energia & temperatura</div>"
"<div class=row>"
"<div class=g m><div class=l>Bateria</div><div class=n id=v>—</div><div class=u>V · seq <span id=seq>—</span></div>"
"<div class=bar id=bv><i></i></div></div>"
"<div class=g m><div class=l>GPS speed</div><div class=n id=gsp>—</div><div class=u>km/h</div></div>"
"<div class=g m><div class=l>CVT</div><div class=n id=cvt>—</div><div class=u>°C</div>"
"<div class=bar id=bc><i></i></div></div>"
"<div class=g m><div class=l>Motor</div><div class=n id=eng>—</div><div class=u>°C</div>"
"<div class=bar id=be><i></i></div></div>"
"</div>"
"<div class=sec>Dinâmica</div>"
"<div class=row>"
"<div class=g m><div class=l>Roll</div><div class=n id=roll>—</div><div class=u>°</div></div>"
"<div class=g m><div class=l>Pitch</div><div class=n id=pitch>—</div><div class=u>°</div></div>"
"</div>"
"<div class=sec>GPS</div>"
"<div class=g><div class=l>Coordenadas</div>"
"<div class=n style=font-size:1rem id=gps>—</div></div>"
"<div class=sec>Links</div><div class=chips id=lnk></div>"
"<div class=sec>CAN</div><div class=g><div class=meta id=can>—</div></div>"
"</section>"
"<section class=view id=vDiag>"
"<div class=sec>API token</div>"
"<div class=g><div class=tok>"
"<input id=tok type=password autocomplete=off placeholder=\"token (cmd/SD/cfg)\">"
"<button type=button id=bTok>Salvar</button>"
"</div><div class=meta id=dCfg style=margin-top:.4rem>—</div></div>"
"<div class=sec>Health</div>"
"<div class=chips id=dHp></div>"
"<div class=g><div class=meta id=dHealth>—</div></div>"
"<div class=sec>Estado RTU</div>"
"<div class=chips id=dSt></div>"
"<div class=g><div class=meta id=dMeta>—</div></div>"
"<div class=sec>Canais LOC / QF</div>"
"<div class=g style=overflow:auto><table>"
"<thead><tr><th>CH</th><th>DTC</th><th>QF</th><th>miss</th><th>hit</th><th>last_ms</th></tr></thead>"
"<tbody id=dTb></tbody></table></div>"
"<div class=sec>SD logs</div>"
"<div class=g>"
"<div class=sdact>"
"<button type=button id=bSdDl>Download all</button>"
"<button type=button id=bSdEr class=bad>Erase all</button>"
"</div>"
"<div class=meta id=dSd>…</div></div>"
"<div class=sec>Telemetria (snapshot)</div>"
"<div class=g><div class=meta id=dTelem>—</div></div>"
"<div class=sec>Siglas</div>"
"<div class=g><div class=gloss>"
"<div><b>DTC</b> Diagnostic Trouble Code</div>"
"<div><b>QF</b> Qualifier / qualidade do sinal</div>"
"<div><b>LOC</b> Loss Of Communication</div>"
"<div><b>CH</b> Canal monitorado</div>"
"<div><b>FSM</b> Finite State Machine</div>"
"<div><b>LIMP</b> Modo degradado (limp)</div>"
"<div><b>SNA</b> Signal Not Available</div>"
"<div><b>TEC</b> TX Error Counter (CAN)</div>"
"<div><b>REC</b> RX Error Counter (CAN)</div>"
"<div><b>RTU</b> Rear Transmission Unit</div>"
"<div><b>FCU</b> Front Control Unit</div>"
"<div><b>RCU</b> Rear Control Unit</div>"
"<div><b>IMU</b> Inertial Measurement Unit</div>"
"<div><b>CVT</b> Continuously Variable Transmission</div>"
"<div><b>NTC</b> termistor (temp. motor)</div>"
"<div><b>DBG</b> LED de debug (CAN 0x702)</div>"
"<div><b>OTA</b> requer tabela two_ota (+espaço)</div>"
"</div></div>"
"</section>"
"</main>"
"<nav class=dock id=dock>"
"<button class=go data-c=start>START</button>"
"<button class=kill data-c=kill>KILL</button>"
"<button data-c=light_on>FAROL</button>"
"<button class=led data-c=led_on>DBG ON</button>"
"<button class=ledoff data-c=led_off>DBG OFF</button>"
"<button data-c=light_off>FAROL OFF</button>"
"</nav>"
"<script>"
"const $=id=>document.getElementById(id);"
"const CS={0:'STOPPED',1:'RUNNING',2:'BUS_OFF',3:'RECOVERING'};"
"const FS={0:'BOOT',1:'SELF',2:'RUN',3:'ERR',4:'DOWN'};"
"const F_LED=1<<15;"
"const TK='rtu_api_token';"
"let page='dat';"
"function getTok(){return localStorage.getItem(TK)||'';}"
"function setTok(v){localStorage.setItem(TK,v||'');$('tok').value=v||'';}"
"$('tok').value=getTok()||'manguebaja';"
"$('bTok').onclick=()=>{setTok($('tok').value.trim());$('st').textContent='token salvo';};"
"function authH(){const t=getTok()||$('tok').value.trim();return t?{'X-RTU-Token':t}:{};}"
"function show(p){page=p;$('vDat').className='view'+(p==='dat'?' on':'');"
"$('vDiag').className='view'+(p==='diag'?' on':'');"
"$('bDat').className=p==='dat'?'on':'';$('bDiag').className=p==='diag'?'on':'';"
"$('dock').style.display=p==='dat'?'grid':'none'}"
"$('bDat').onclick=()=>show('dat');$('bDiag').onclick=()=>show('diag');"
"function bar(id,pct,mode){const e=$(id);if(!e)return;e.className='bar'+(mode?' '+mode:'');"
"e.firstChild.style.width=Math.max(0,Math.min(100,pct||0))+'%';}"
"function chip(t,cls){return '<span class=\"chip '+cls+'\">'+t+'</span>';}"
"function dtcCls(s){return s==='confirmed'?'bad':s==='pending'?'warn':'ok'}"
"function qfCls(s){return s==='valid'?'ok':s==='faulted'?'bad':s==='temp'?'neu':'warn'}"
"function engShow(raw){"
" const en=(+raw)/100;const bad=!isFinite(en)||en<0||en>180||raw===-1;"
" const el=$('eng');"
" if(bad){el.textContent='SNA';el.className='n sna';bar('be',0,'hot');return true;}"
" el.className='n';el.textContent=en.toFixed(1);"
" bar('be',(en/120)*100,en>100?'hot':en>85?'warn':'');return false;}"
"function fmtB(n){n=+n||0;return n>1048576?(n/1048576).toFixed(1)+'M':n>1024?(n/1024).toFixed(0)+'K':n+'B'}"
"function tokQ(){return encodeURIComponent(getTok()||$('tok').value.trim())}"
"let sdBusy=false;"
"function setSdBusy(on,msg){sdBusy=!!on;['bSdDl','bSdEr'].forEach(id=>{const e=$(id);if(e)e.disabled=!!on;});"
" if(msg){$('st').textContent=msg;$('st').style.color=on?'var(--warn)':'var(--ok)';}}"
"async function dlOne(name){"
" const r=await fetch('/api/sd/get?name='+encodeURIComponent(name)+'&token='+tokQ(),{cache:'no-store'});"
" if(!r.ok)throw new Error('HTTP '+r.status);"
" const b=await r.blob();const u=URL.createObjectURL(b);"
" const a=document.createElement('a');a.href=u;a.download=name;document.body.appendChild(a);a.click();a.remove();"
" setTimeout(()=>URL.revokeObjectURL(u),2e3);}"
"async function loadSd(){"
" if(sdBusy)return;"
" try{const r=await fetch('/api/sd',{cache:'no-store'});if(!r.ok)throw 0;const a=await r.json();"
"  if(!a.length){$('dSd').textContent='sem LOG*.BIN (ou SD off)';return}"
"  const t=tokQ();"
"  $('dSd').innerHTML=a.map(f=>'<div class=sdrow><span><a href=\"#\" data-dl=\"'+f.name+'\">'+f.name+"
"   '</a><span class=sz>'+fmtB(f.size)+'</span></span>'+"
"   '<button type=button class=era data-era=\"'+f.name+'\">apagar</button></div>').join('');"
"  $('dSd').querySelectorAll('[data-dl]').forEach(a=>a.onclick=async e=>{"
"   e.preventDefault();if(sdBusy)return;"
"   setSdBusy(true,'BOX · baixando '+a.dataset.dl+' (LoRa/SD log pausados)');"
"   try{await dlOne(a.dataset.dl);$('st').textContent='ok '+a.dataset.dl;}catch(err){$('st').textContent='falha download';}"
"   finally{setSdBusy(false);}});"
"  $('dSd').querySelectorAll('[data-era]').forEach(b=>b.onclick=()=>eraseOne(b.dataset.era));"
" }catch(e){$('dSd').textContent='SD indisponível'}}"
"async function eraseOne(name){"
" if(sdBusy||!confirm('Apagar '+name+'?'))return;"
" setSdBusy(true,'BOX · apagando '+name+'…');"
" try{const r=await fetch('/api/sd/erase?name='+encodeURIComponent(name)+'&token='+tokQ(),"
"  {method:'POST',headers:authH()});"
"  const t=await r.text();$('st').textContent='erase '+name+' → '+(r.ok?'ok':t.slice(0,48));"
" }finally{setSdBusy(false);loadSd();}}"
"$('bSdDl').onclick=async()=>{"
" if(sdBusy)return;"
" try{const r=await fetch('/api/sd',{cache:'no-store'});if(!r.ok)throw 0;const a=await r.json();"
"  if(!a.length){$('st').textContent='SD vazio';return}"
"  setSdBusy(true,'BOX · download 0/'+a.length+' (LoRa/SD pausados)');"
"  for(let i=0;i<a.length;i++){"
"   setSdBusy(true,'BOX · download '+(i+1)+'/'+a.length+' · '+a[i].name);"
"   await dlOne(a[i].name);}"
"  $('st').textContent='download all ok ('+a.length+')';"
" }catch(e){$('st').textContent='download all falhou'}finally{setSdBusy(false);loadSd();}};"
"$('bSdEr').onclick=async()=>{"
" if(sdBusy||!confirm('Apagar TODOS os LOG*.BIN do SD?'))return;"
" setSdBusy(true,'BOX · erase all (LoRa/SD pausados)…');"
" try{const r=await fetch('/api/sd/erase_all?token='+tokQ(),{method:'POST',headers:authH()});"
"  const j=await r.json().catch(()=>({}));"
"  $('st').textContent=r.ok?('erase all · '+((j&&j.n!=null)?j.n:'?')+' file(s)'):('erase all falhou');"
" }finally{setSdBusy(false);loadSd();}};"
"async function loadCfg(){"
" try{const r=await fetch('/api/cfg',{cache:'no-store'});if(!r.ok)throw 0;const c=await r.json();"
"  $('dCfg').textContent='SSID '+c.ssid+' · token_len '+c.token_len+' · lora_ch 0x'+c.lora_ch.toString(16)"
"   +' · ap_pass_len '+c.ap_pass_len;"
" }catch(e){$('dCfg').textContent='cfg indisponível'}}"
"async function tick(){"
" if(sdBusy)return;"
" try{"
"  const [rj,rd,rh]=await Promise.all(["
"   fetch('/api/json',{cache:'no-store'}),"
"   fetch('/api/diag',{cache:'no-store'}),"
"   fetch('/api/health',{cache:'no-store'})]);"
"  if(!rj.ok)throw 0;"
"  const j=await rj.json();"
"  const d=rd.ok?await rd.json():null;"
"  const h=rh.ok?await rh.json():null;"
"  const live=j.age_ms<5000;"
"  $('dot').className='dot'+(live?' on':'');"
"  $('st').textContent=live?('LIVE '+j.age_ms+'ms'):'SEM SINAL';"
"  $('st').style.color=live?'var(--ok)':'var(--warn)';"
"  const cs=CS[j.can_state]||('S'+j.can_state);"
"  $('can').innerHTML='rx '+j.can_rx+' · last 0x'+(j.can_last_id>>>0).toString(16)"
"   +' · TEC '+j.can_tec+' · REC '+j.can_rec+' · <b style=color:'+(j.can_state===1?'var(--ok)':'var(--bad)')+'>'+cs+'</b>';"
"  if(j.ok){"
"   $('seq').textContent=j.seq;$('spd').textContent=j.speed;$('rpm').textContent=j.rpm;"
"   $('v').textContent=j.voltage.toFixed(2);$('gsp').textContent=(j.gps_speed||0).toFixed(1);"
"   const cv=j.cvt_temp/100;"
"   $('cvt').textContent=cv.toFixed(1);"
"   const engBad=engShow(j.eng_temp);"
"   $('roll').textContent=j.roll;$('pitch').textContent=j.pitch;"
"   $('gps').textContent=j.latitude.toFixed(5)+', '+j.longitude.toFixed(5);"
"   bar('bv',(j.voltage/16)*100,j.voltage<11.5?'hot':j.voltage<12.2?'warn':'');"
"   bar('bc',(cv/120)*100,cv>100?'hot':cv>85?'warn':'');"
"   const led=!!(j.flags&F_LED)||!!j.led_debug;"
"   $('lnk').innerHTML=chip('FCU',j.fcu_link_down?'off':'on')+chip('RCU',j.rcu_link_down?'off':'on')"
"    +chip('GPS',j.gps_ok?'on':'off')+chip('DBG LED',led?'on':'off')"
"    +chip('NTC',engBad?'off':'on')+chip('flags 0x'+j.flags.toString(16),'neu');"
"  }else{$('lnk').innerHTML=chip('aguardando','neu')}"
"  if(h){"
"   $('dHp').innerHTML=chip('fw '+h.fw,'neu')+chip('boot '+h.boot,'neu')"
"    +chip('rst '+h.rst,(h.rst==='BROWNOUT'||h.rst.indexOf('WDT')>=0||h.rst==='PANIC')?'off':'on')"
"    +chip('SD',h.sd_busy?'warn':(h.sd?'on':'off'))+chip('E32',h.e32_aux?'on':'off')"
"    +chip('LoRa '+h.lora_ok+'/'+h.lora_fail,h.lora_fail>0&&h.lora_ok===0?'off':'on')"
"    +(h.sd_busy?chip('BOX/SD','warn'):'');"
"   $('dHealth').textContent='up '+(h.uptime_ms/1000).toFixed(0)+'s · heap '+fmtB(h.heap)"
"    +' (min '+fmtB(h.heap_min)+') · can_rx '+h.can_rx"
"    +(h.sd_path?' · '+h.sd_path:'');"
"  }"
"  if(d){"
"   const led=j.ok&&(!!(j.flags&F_LED)||!!j.led_debug);"
"   $('dSt').innerHTML=chip('FSM '+(FS[j.fsm]||j.fsm),'neu')"
"    +chip('LIMP',d.limp?'off':'on')+chip('DBG',led?'on':'off')"
"    +chip('busoff_gate',d.busoff_gate?'warn':'on')"
"    +chip('tick '+d.tick,'neu');"
"   let rows='';"
"   (d.ch||[]).forEach(c=>{"
"    rows+='<tr><td>'+c.id+'</td><td class='+dtcCls(c.dtc)+'>'+c.dtc"
"     +'</td><td class='+qfCls(c.qf)+'>'+c.qf+'</td><td>'+c.miss+'</td><td>'+c.hit"
"     +'</td><td>'+c.last_ms+'</td></tr>';});"
"   $('dTb').innerHTML=rows;"
"   $('dMeta').textContent='TEC '+j.can_tec+' · REC '+j.can_rec+' · state '+cs"
"    +' · age '+j.age_ms+'ms · FCU '+(FS[j.fcu_state]||j.fcu_state)"
"    +' · RCU '+(FS[j.rcu_state]||j.rcu_state)"
"    +' · CAN cmd 0x702 DBG';"
"   if(j.ok){const en=j.eng_temp/100;const es=(!isFinite(en)||en<0||j.eng_temp===-1)?'SNA':en.toFixed(1);"
"    $('dTelem').textContent='V='+j.voltage.toFixed(2)+' spd='+j.speed"
"    +' rpm='+j.rpm+' cvt='+(j.cvt_temp/100).toFixed(1)+' eng='+es"
"    +' · roll='+j.roll+' pitch='+j.pitch"
"    +' · flags 0x'+j.flags.toString(16)+' · DBG '+(led?'ON':'OFF');}"
"   else{$('dTelem').textContent='sem pacote';}"
"  }"
" }catch(e){$('st').textContent='ERRO';$('st').style.color='var(--bad)';$('dot').className='dot'}"
"}"
"setInterval(tick,500);tick();loadSd();loadCfg();setInterval(loadSd,5000);"
"document.querySelectorAll('[data-c]').forEach(b=>b.onclick=async()=>{"
" const r=await fetch('/api/cmd',{method:'POST',headers:Object.assign({'Content-Type':'text/plain'},authH()),"
"  body:b.dataset.c});"
" const t=await r.text();$('st').textContent=(r.status===401?'401 ':'')+b.dataset.c+' → '+t.slice(0,48);"
"});"
"</script></body></html>";

static esp_err_t send_json(httpd_req_t *req)
{
    telemetry_packet_t pkt;
    can_bus_stats_t can;
    char buf[900];
    uint32_t age = telem_store_age_ms();
    can_get_bus_stats(&can);
    int fsm = (int)fsm_state();

    if (!telem_store_copy(&pkt)) {
        snprintf(buf, sizeof(buf),
                 "{\"ok\":false,\"age_ms\":%lu,\"ssid\":\"%s\",\"fw\":\"%s\",\"schema\":%d,\"fsm\":%d,"
                 "\"can_rx\":%lu,\"can_last_id\":%lu,\"can_tec\":%lu,\"can_rec\":%lu,\"can_state\":%d,"
                 "\"fcu_link_down\":%s,\"rcu_link_down\":%s}",
                 (unsigned long)age, WIFI_AP_SSID, RTU_FW_VER, RTU_FW_SCHEMA, fsm,
                 (unsigned long)can.rx_frames, (unsigned long)can.last_rx_id,
                 (unsigned long)can.tx_error_counter, (unsigned long)can.rx_error_counter,
                 can.state,
                 rtu_diag_fcu_down_confirmed() ? "true" : "false",
                 rtu_diag_rcu_down_confirmed() ? "true" : "false");
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"age_ms\":%lu,\"seq\":%u,\"voltage\":%.3f,\"soc\":%u,"
                 "\"rpm\":%u,\"speed\":%u,\"gps_speed\":%.2f,"
                 "\"cvt_temp\":%d,\"eng_temp\":%d,\"roll\":%d,\"pitch\":%d,"
                 "\"acc_x\":%d,\"acc_y\":%d,\"acc_z\":%d,"
                 "\"dps_x\":%d,\"dps_y\":%d,\"dps_z\":%d,"
                 "\"flags\":%u,\"latitude\":%.7f,\"longitude\":%.7f,"
                 "\"fcu_state\":%u,\"rcu_state\":%u,\"per\":%u,\"ber\":%u,"
                 "\"fcu_link_down\":%s,\"rcu_link_down\":%s,\"gps_ok\":%s,\"led_debug\":%s,"
                 "\"eng_ok\":%s,\"crc\":%u,\"fsm\":%d,\"limp\":%s,"
                 "\"fw\":\"%s\",\"schema\":%d,"
                 "\"can_rx\":%lu,\"can_last_id\":%lu,\"can_tec\":%lu,\"can_rec\":%lu,\"can_state\":%d}",
                 (unsigned long)age, pkt.seq, (double)pkt.voltage, pkt.soc,
                 pkt.rpm, pkt.speed, (double)pkt.gps_speed,
                 (int)pkt.cvt_temp, (int)pkt.eng_temp, (int)pkt.roll, (int)pkt.pitch,
                 (int)pkt.acc_x, (int)pkt.acc_y, (int)pkt.acc_z,
                 (int)pkt.dps_x, (int)pkt.dps_y, (int)pkt.dps_z,
                 pkt.flags, (double)pkt.latitude, (double)pkt.longitude,
                 pkt.fcu_state, pkt.rcu_state, pkt.per, pkt.ber,
                 rtu_diag_fcu_down_confirmed() ? "true" : "false",
                 rtu_diag_rcu_down_confirmed() ? "true" : "false",
                 (pkt.flags & FLAG_GPS_OK) ? "true" : "false",
                 (pkt.flags & FLAG_LED_DEBUG) ? "true" : "false",
                 (pkt.eng_temp >= 0 && pkt.eng_temp <= 18000) ? "true" : "false",
                 pkt.crc8, fsm, fsm_limp() ? "true" : "false",
                 RTU_FW_VER, RTU_FW_SCHEMA,
                 (unsigned long)can.rx_frames, (unsigned long)can.last_rx_id,
                 (unsigned long)can.tx_error_counter, (unsigned long)can.rx_error_counter,
                 can.state);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_diag(httpd_req_t *req)
{
    char buf[768];
    int n = rtu_diag_json(buf, sizeof(buf));
    if (n <= 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t send_health(httpd_req_t *req)
{
    char buf[512];
    int n = rtu_health_json(buf, sizeof(buf));
    if (n <= 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t send_sd_list(httpd_req_t *req)
{
    char buf[1200];
    int n = sd_log_list_json(buf, sizeof(buf));
    if (n <= 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static bool extract_token(httpd_req_t *req, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) {
        return false;
    }
    out[0] = '\0';

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "X-RTU-Token");
    if (hdr_len > 0 && hdr_len < out_sz) {
        if (httpd_req_get_hdr_value_str(req, "X-RTU-Token", out, out_sz) == ESP_OK) {
            return out[0] != '\0';
        }
    }

    hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len > 0 && hdr_len < 96) {
        char auth[96];
        if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) == ESP_OK) {
            const char *p = auth;
            if ((p[0] == 'B' || p[0] == 'b') &&
                (p[1] == 'e' || p[1] == 'E') &&
                (p[2] == 'a' || p[2] == 'A') &&
                (p[3] == 'r' || p[3] == 'R') &&
                (p[4] == 'e' || p[4] == 'E') &&
                (p[5] == 'r' || p[5] == 'R') &&
                p[6] == ' ') {
                p += 7;
            }
            while (*p == ' ') {
                p++;
            }
            strncpy(out, p, out_sz - 1);
            out[out_sz - 1] = '\0';
            if (out[0]) {
                return true;
            }
        }
    }

    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "token", out, out_sz) == ESP_OK && out[0]) {
            return true;
        }
    }
    return false;
}

static esp_err_t require_token(httpd_req_t *req)
{
    char tok[RTU_CFG_TOKEN_MAX];
    if (!extract_token(req, tok, sizeof(tok)) || !rtu_cfg_token_ok(tok)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        (void)httpd_resp_send(req, "{\"ok\":false,\"err\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

static esp_err_t send_cfg(httpd_req_t *req)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"ssid\":\"%s\",\"ap_pass_len\":%u,\"token_len\":%u,\"lora_ch\":%u}",
             WIFI_AP_SSID,
             (unsigned)strlen(rtu_cfg_ap_pass()),
             (unsigned)strlen(rtu_cfg_api_token()),
             (unsigned)rtu_cfg_lora_chan());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_cfg(httpd_req_t *req)
{
    esp_err_t auth = require_token(req);
    if (auth != ESP_OK) {
        return auth;
    }

    char body[160];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"bad len\"}", HTTPD_RESP_USE_STRLEN);
    }
    int n = httpd_req_recv(req, body, total);
    if (n <= 0) {
        return ESP_FAIL;
    }
    body[n] = '\0';

    char ap_pass[RTU_CFG_PASS_MAX] = { 0 };
    char token[RTU_CFG_TOKEN_MAX] = { 0 };
    int lora = -1;
    char *p = body;
    while (p && *p) {
        char *eq = strchr(p, '=');
        char *amp = strchr(p, '&');
        if (eq == NULL) {
            break;
        }
        *eq = '\0';
        char *val = eq + 1;
        if (amp) {
            *amp = '\0';
        }
        if (strcmp(p, "ap_pass") == 0) {
            strncpy(ap_pass, val, sizeof(ap_pass) - 1);
        } else if (strcmp(p, "token") == 0) {
            strncpy(token, val, sizeof(token) - 1);
        } else if (strcmp(p, "lora_ch") == 0) {
            lora = (int)strtol(val, NULL, 0);
        }
        p = amp ? amp + 1 : NULL;
    }

    esp_err_t err = rtu_cfg_set(ap_pass[0] ? ap_pass : NULL,
                                token[0] ? token : NULL,
                                lora);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"invalid\"}", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req,
                           "{\"ok\":true,\"note\":\"ap_pass aplica no próximo boot\"}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_sd_get(httpd_req_t *req)
{
    esp_err_t auth = require_token(req);
    if (auth != ESP_OK) {
        return auth;
    }

    if (sd_log_busy()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"sd busy\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    char q[128];
    char name[24];
    name[0] = '\0';
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK) {
            name[0] = '\0';
        }
    }

    /* Box: pausa log SD + LoRa — SPI exclusivo para o transfer. */
    sd_log_busy_enter("download");
    uint32_t sz = 0;
    FILE *f = sd_log_open_read(name, &sz);
    if (f == NULL) {
        sd_log_busy_exit();
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    char cd[48];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%.20s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    /* Chunks grandes; yield raro só p/ WDT — resto da RTU já está pausada. */
    char chunk[2048];
    size_t n;
    unsigned bursts = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            break;
        }
        if ((++bursts & 31u) == 0u) {
            vTaskDelay(1);
        }
    }
    fclose(f);
    esp_err_t err = httpd_resp_send_chunk(req, NULL, 0);
    sd_log_busy_exit();
    return err;
}

static esp_err_t post_sd_erase(httpd_req_t *req)
{
    esp_err_t auth = require_token(req);
    if (auth != ESP_OK) {
        return auth;
    }

    if (sd_log_busy()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"sd busy\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    char q[128];
    char name[24];
    name[0] = '\0';
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        (void)httpd_query_key_value(q, "name", name, sizeof(name));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    sd_log_busy_enter("erase");
    bool ok = (name[0] != '\0') && sd_log_erase(name);
    sd_log_busy_exit();
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"erase\"}", HTTPD_RESP_USE_STRLEN);
    }
    char resp[80];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"erased\":\"%.20s\"}", name);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_sd_erase_all(httpd_req_t *req)
{
    esp_err_t auth = require_token(req);
    if (auth != ESP_OK) {
        return auth;
    }

    if (sd_log_busy()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"sd busy\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    sd_log_busy_enter("erase_all");
    int n = sd_log_erase_all();
    sd_log_busy_exit();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (n < 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"sd\"}", HTTPD_RESP_USE_STRLEN);
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"n\":%d}", n);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_packet(httpd_req_t *req)
{
    uint8_t frame[TELEM_FRAME_LEN];
    if (!telem_store_copy_frame(frame)) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)frame, TELEM_FRAME_LEN);
}

static esp_err_t send_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_cmd(httpd_req_t *req)
{
    esp_err_t auth = require_token(req);
    if (auth != ESP_OK) {
        return auth;
    }

    char body[64];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "bad len", HTTPD_RESP_USE_STRLEN);
    }
    int n = httpd_req_recv(req, body, total);
    if (n <= 0) {
        return ESP_FAIL;
    }
    body[n] = '\0';
    while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r' || body[n - 1] == ' ')) {
        body[--n] = '\0';
    }
    for (int i = 0; i < n; i++) {
        if (body[i] >= 'A' && body[i] <= 'Z') {
            body[i] = (char)(body[i] - 'A' + 'a');
        }
    }
    bool ok = s_cmd_cb && s_cmd_cb(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"cmd\":\"%.40s\"}", ok ? "true" : "false", body);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t options_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-RTU-Token, Authorization");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t http_telem_start(http_cmd_fn cmd_cb)
{
    s_cmd_cb = cmd_cb;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    /* Default 7; SoftAP + poll×3 + 1 download já aperta. LRU evita accept stuck. */
    cfg.max_open_sockets = 7;
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 8;
    cfg.send_wait_timeout = 8;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",               .method = HTTP_GET,     .handler = send_index },
        { .uri = "/api/json",       .method = HTTP_GET,     .handler = send_json },
        { .uri = "/api/diag",       .method = HTTP_GET,     .handler = send_diag },
        { .uri = "/api/health",     .method = HTTP_GET,     .handler = send_health },
        { .uri = "/api/cfg",        .method = HTTP_GET,     .handler = send_cfg },
        { .uri = "/api/cfg",        .method = HTTP_POST,    .handler = post_cfg },
        { .uri = "/api/cfg",        .method = HTTP_OPTIONS, .handler = options_cors },
        { .uri = "/api/sd",         .method = HTTP_GET,     .handler = send_sd_list },
        { .uri = "/api/sd/get",     .method = HTTP_GET,     .handler = send_sd_get },
        { .uri = "/api/sd/erase",   .method = HTTP_POST,    .handler = post_sd_erase },
        { .uri = "/api/sd/erase",   .method = HTTP_OPTIONS, .handler = options_cors },
        { .uri = "/api/sd/erase_all", .method = HTTP_POST,  .handler = post_sd_erase_all },
        { .uri = "/api/sd/erase_all", .method = HTTP_OPTIONS, .handler = options_cors },
        { .uri = "/api/packet",     .method = HTTP_GET,     .handler = send_packet },
        { .uri = "/api/cmd",        .method = HTTP_POST,    .handler = post_cmd },
        { .uri = "/api/cmd",        .method = HTTP_OPTIONS, .handler = options_cors },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "UI http://192.168.4.1/  auth on cmd/sd/cfg/erase");
    return ESP_OK;
}
