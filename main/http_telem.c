#include "http_telem.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "can_mgmt.h"
#include "telem_store.h"
#include "wifi_ap.h"
#include "rtu_diag.h"
#include "rtu_health.h"
#include "rtu_fw.h"
#include "system_fsm.h"
#include "sd_log.h"
#include "lora_rate.h"

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
".chip.btn{cursor:pointer;font:600 .72rem inherit;color:var(--fg)}"
".chip.btn.on{border-color:var(--acc);color:var(--acc);background:#123044}"
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
"<div class=sec>LoRa · pacotes no ar</div>"
"<div class=chips id=lora></div>"
"<div class=g><div class=meta id=loraMeta>…</div></div>"
"</section>"
"<section class=view id=vDiag>"
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
"<div class=g><div class=meta id=dSd>…</div></div>"
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
"let page='dat',loraMs=0,loraGap=0,loraOpts=[500,1000,2000,5000,10000];"
"function hzOf(ms){const hz=1000/ms;return (hz>=1?(hz%1?hz.toFixed(1):String(hz|0)):hz.toFixed(2))+' Hz'}"
"function paintLora(){"
" if(!$('loraMeta')||!loraMs)return;"
" const gap=loraGap?(' · último intervalo '+loraGap+' ms'):' · aguardando primeiro TX';"
" $('loraMeta').textContent='enviado a cada '+loraMs+' ms ('+hzOf(loraMs)+')'+gap;"
"}"
"function renderLora(){"
" const box=$('lora'); if(!box||!loraMs)return;"
" const key=String(loraMs)+':'+loraOpts.join(',');"
" if(box.dataset.cur!==key){"
"  box.dataset.cur=key;"
"  box.innerHTML=loraOpts.map(ms=>'<button type=button class=\"chip btn'+(ms===loraMs?' on':'')+'\" data-lora='+ms+'>'+ms+' ms</button>').join('');"
"  box.querySelectorAll('[data-lora]').forEach(b=>b.onclick=async()=>{"
"   const want=+b.dataset.lora;"
"   const r=await fetch('/api/lora',{method:'POST',headers:{'Content-Type':'text/plain'},body:String(want)});"
"   let j=null; try{j=await r.json()}catch(e){}"
"   if(j&&j.ok){loraMs=j.ms;loraGap=j.gap_ms||loraGap;if(j.opts&&j.opts.length)loraOpts=j.opts;box.dataset.cur='';renderLora();}"
"   else{$('loraMeta').textContent='período recusado';}"
"  });"
" }"
" paintLora();"
"}"
"async function loadLora(){"
" try{const r=await fetch('/api/lora',{cache:'no-store'});if(!r.ok)return;const j=await r.json();"
"  loraMs=j.ms;loraGap=j.gap_ms||0;if(j.opts&&j.opts.length)loraOpts=j.opts;"
"  const box=$('lora');if(box)box.dataset.cur='';renderLora();"
" }catch(e){}"
"}"
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
"async function loadSd(){"
" try{const r=await fetch('/api/sd',{cache:'no-store'});if(!r.ok)throw 0;const a=await r.json();"
"  if(!a.length){$('dSd').textContent='sem LOG*.BIN (ou SD off)';return}"
"  $('dSd').innerHTML=a.map(f=>'<div><a style=color:var(--acc) href=\"/api/sd/get?name='+"
"   encodeURIComponent(f.name)+'\">'+f.name+'</a> · '+fmtB(f.size)+'</div>').join('');"
" }catch(e){$('dSd').textContent='SD indisponível'}}"
"async function tick(){"
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
"    +chip('SD',h.sd?'on':'off')+chip('E32',h.e32_aux?'on':'off')"
"    +chip('LoRa '+h.lora_ok+'/'+h.lora_fail,h.lora_fail>0&&h.lora_ok===0?'off':'on');"
"   if(h.lora_ms){loraGap=+h.lora_gap_ms||0;if(loraMs!==h.lora_ms)loraMs=h.lora_ms;renderLora();}"
"   $('dHealth').textContent='up '+(h.uptime_ms/1000).toFixed(0)+'s · heap '+fmtB(h.heap)"
"    +' (min '+fmtB(h.heap_min)+') · can_rx '+h.can_rx"
"    +(h.lora_ms?(' · LoRa '+h.lora_ms+' ms'+(h.lora_gap_ms?(' gap '+h.lora_gap_ms):'')):'')"
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
"setInterval(tick,500);tick();loadSd();loadLora();setInterval(loadSd,5000);"
"document.querySelectorAll('[data-c]').forEach(b=>b.onclick=async()=>{"
" const r=await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'text/plain'},body:b.dataset.c});"
" const t=await r.text();$('st').textContent=b.dataset.c+' → '+t.slice(0,48);"
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

static esp_err_t send_lora(httpd_req_t *req)
{
    char buf[160];
    int n = lora_rate_json(buf, sizeof(buf));
    if (n <= 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t post_lora(httpd_req_t *req)
{
    char body[16];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    int n = httpd_req_recv(req, body, total);
    if (n <= 0) {
        return ESP_FAIL;
    }
    body[n] = '\0';
    uint32_t ms = 0;
    bool digits = false;
    for (int i = 0; i < n; i++) {
        char c = body[i];
        if (c == ' ' || c == '\r' || c == '\n') {
            continue;
        }
        if (c < '0' || c > '9' || ms > 100000u) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
        }
        digits = true;
        ms = ms * 10u + (uint32_t)(c - '0');
    }
    bool ok = digits && lora_rate_set_ms(ms);
    char state[160];
    int sn = lora_rate_json(state, sizeof(state));
    char resp[200];
    if (!ok || sn <= 2 || state[0] != '{') {
        httpd_resp_set_status(req, "400 Bad Request");
        snprintf(resp, sizeof(resp), "{\"ok\":false}");
    } else {
        /* state é {"ms":...} — prefixa ok sem aninhar objeto. */
        snprintf(resp, sizeof(resp), "{\"ok\":true,%s", state + 1);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_health(httpd_req_t *req)
{
    char buf[640];
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

static esp_err_t send_sd_get(httpd_req_t *req)
{
    char q[64];
    char name[24];
    name[0] = '\0';
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK) {
            name[0] = '\0';
        }
    }
    uint32_t sz = 0;
    FILE *f = sd_log_open_read(name, &sz);
    if (f == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char cd[48];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%.20s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            break;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t http_telem_start(http_cmd_fn cmd_cb)
{
    s_cmd_cb = cmd_cb;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 14;
    cfg.stack_size = 6144;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,     .handler = send_index },
        { .uri = "/api/json",     .method = HTTP_GET,     .handler = send_json },
        { .uri = "/api/diag",     .method = HTTP_GET,     .handler = send_diag },
        { .uri = "/api/health",   .method = HTTP_GET,     .handler = send_health },
        { .uri = "/api/lora",     .method = HTTP_GET,     .handler = send_lora },
        { .uri = "/api/lora",     .method = HTTP_POST,    .handler = post_lora },
        { .uri = "/api/sd",       .method = HTTP_GET,     .handler = send_sd_list },
        { .uri = "/api/sd/get",   .method = HTTP_GET,     .handler = send_sd_get },
        { .uri = "/api/packet",   .method = HTTP_GET,     .handler = send_packet },
        { .uri = "/api/cmd",      .method = HTTP_POST,    .handler = post_cmd },
        { .uri = "/api/cmd",      .method = HTTP_OPTIONS, .handler = options_cors },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "UI http://192.168.4.1/  /api/json /api/diag /api/health /api/lora /api/sd");
    return ESP_OK;
}
