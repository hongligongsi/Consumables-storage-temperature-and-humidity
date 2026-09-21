#pragma once
#include <Arduino.h>

// 独立于文件系统的轻量管理页，直接随固件存入 Flash。
static const char WEB_MANAGEMENT_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Chamber 管理</title><style>
:root{color-scheme:dark;--bg:#08100d;--card:#13201b;--line:#294137;--a:#00d9ef;--ok:#4ace4a;--warn:#ffa600;--text:#eef8f3;--muted:#91a59b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px system-ui,-apple-system,sans-serif}.wrap{max-width:920px;margin:auto;padding:18px}h1{font-size:24px;margin:0 0 4px}.sub{color:var(--muted);margin-bottom:16px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:12px}.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:15px}.card h2{font-size:17px;margin:0 0 12px;color:var(--a)}.stats{display:grid;grid-template-columns:1fr 1fr;gap:7px}.stats span:nth-child(odd){color:var(--muted)}label{display:block;color:var(--muted);margin:9px 0 3px}input,select{width:100%;padding:9px;border:1px solid var(--line);border-radius:7px;background:#09130f;color:var(--text)}.check{display:flex;gap:8px;align-items:center;color:var(--text)}.check input{width:auto}.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}button{border:0;border-radius:7px;padding:9px 13px;background:#245b65;color:white;cursor:pointer}button.warn{background:#8a5700}button.danger{background:#8b2727}#msg{min-height:22px;color:var(--ok);margin:10px 0}.wide{grid-column:1/-1}@media(max-width:520px){.wrap{padding:10px}.stats{grid-template-columns:1fr 1.2fr}}
</style></head><body><main class="wrap"><h1>耗材仓网络管理</h1>
<div class="sub">chamber.local · 固件 <span id="fw">--</span></div><div id="msg"></div>
<section class="grid"><article class="card"><h2>实时状态</h2><div class="stats">
<span>运行状态</span><b id="state">--</b><span>仓温</span><b id="temp">--</b><span>湿度</span><b id="hum">--</b><span>IP</span><b id="ip">--</b><span>SSID</span><b id="ssid">--</b><span>信号</span><b id="rssi">--</b><span>运行时间</span><b id="uptime">--</b><span>断线次数</span><b id="drops">--</b><span>最近断线</span><b id="reason">--</b></div>
<div class="row"><button onclick="action('toggleSystem')">启停系统</button><button onclick="action('toggleLight')">灯光</button><button onclick="action('toggleManualExhaust')">强排气</button></div></article>
<article class="card"><h2>NTP 与时区</h2><label class="check"><input id="ntpEnabled" type="checkbox">启用 NTP</label><label>POSIX 时区</label><input id="timezone" placeholder="CST-8"><label>NTP 服务器 1</label><input id="ntpServer1"><label>NTP 服务器 2</label><input id="ntpServer2"><div class="row"><button onclick="saveTime()">保存时间设置</button></div></article>
<article class="card"><h2>静态 IPv4</h2><label class="check"><input id="staticIpEnabled" type="checkbox">使用静态 IP（保存后重连）</label><label>IP 地址</label><input id="staticIp"><label>网关</label><input id="staticGateway"><label>子网掩码</label><input id="staticSubnet"><label>DNS 1</label><input id="staticDns1"><label>DNS 2</label><input id="staticDns2"><div class="row"><button onclick="saveNetwork()">保存网络设置</button></div></article>
<article class="card"><h2>MQTT 与 OTA</h2><label class="check"><input id="mqttEnabled" type="checkbox">启用 MQTT</label><label>Broker</label><input id="mqttBroker"><label>端口</label><input id="mqttPort" type="number" min="1" max="65535"><label>主题前缀</label><input id="mqttTopicPrefix"><label class="check"><input id="otaEnabled" type="checkbox">启用 OTA</label><label>本机 OTA 密码</label><input id="otaPassword" readonly><div class="row"><button onclick="saveServices()">保存服务设置</button></div></article>
<article class="card wide"><h2>维护</h2><p class="sub">重新配网会清除已保存的 Wi‑Fi 并重启设备。</p><button class="danger" onclick="resetWifi()">清除 Wi‑Fi 并重新配网</button></article></section></main>
<script>
const $=id=>document.getElementById(id), msg=(s,bad=false)=>{const e=$('msg');e.textContent=s;e.style.color=bad?'#ff6868':'#4ace4a'};
async function api(url,opt){const r=await fetch(url,opt);const t=await r.text();if(!r.ok)throw Error(t||('HTTP '+r.status));return t?JSON.parse(t):{}}
function body(o){return new URLSearchParams(Object.entries(o).map(([k,v])=>[k,String(v)]))}
async function loadSettings(){const s=await api('/api/settings');for(const k of ['ntpEnabled','staticIpEnabled','mqttEnabled','otaEnabled'])$(k).checked=!!s[k];for(const k of ['timezone','ntpServer1','ntpServer2','staticIp','staticGateway','staticSubnet','staticDns1','staticDns2','mqttBroker','mqttPort','mqttTopicPrefix','otaPassword'])$(k).value=s[k]??''}
async function poll(){try{const s=await api('/api/state');$('fw').textContent=s.firmware;$('state').textContent=s.state;$('temp').textContent=s.chamberC==null?'--':s.chamberC.toFixed(1)+' °C';$('hum').textContent=s.humidity==null?'--':s.humidity.toFixed(1)+' %';$('ip').textContent=s.ip||'--';$('ssid').textContent=s.ssid||'--';$('rssi').textContent=s.rssi==null?'--':s.rssi+' dBm';$('uptime').textContent=Math.floor(s.uptimeSeconds/3600)+'h '+Math.floor(s.uptimeSeconds%3600/60)+'m';$('drops').textContent=s.disconnectCount;$('reason').textContent=s.lastDisconnectReason||'--'}catch(e){msg('状态读取失败：'+e.message,true)}}
async function post(data){try{await api('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body(data)})}catch(e){msg('保存失败：'+e.message,true);return}msg('已保存并生效');setTimeout(()=>loadSettings().catch(()=>{}),800)}
function saveTime(){post({ntpEnabled:$('ntpEnabled').checked,timezone:$('timezone').value,ntpServer1:$('ntpServer1').value,ntpServer2:$('ntpServer2').value})}
function saveNetwork(){post({staticIpEnabled:$('staticIpEnabled').checked,staticIp:$('staticIp').value,staticGateway:$('staticGateway').value,staticSubnet:$('staticSubnet').value,staticDns1:$('staticDns1').value,staticDns2:$('staticDns2').value})}
function saveServices(){post({mqttEnabled:$('mqttEnabled').checked,mqttBroker:$('mqttBroker').value,mqttPort:$('mqttPort').value,mqttTopicPrefix:$('mqttTopicPrefix').value,otaEnabled:$('otaEnabled').checked})}
async function action(name){try{await api('/api/action',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body({name})});msg('操作成功')}catch(e){msg('操作失败：'+e.message,true)}}
async function resetWifi(){if(!confirm('确定清除 Wi‑Fi 并重启？'))return;try{await fetch('/api/wifi/reset',{method:'POST'});msg('设备正在重启，请连接 FilamentChamber-Setup')}catch(e){msg('设备正在重启')}}
loadSettings().then(()=>setTimeout(poll,150)).catch(e=>msg('读取设置失败：'+e.message,true));setInterval(poll,3000);
</script></body></html>)HTML";
