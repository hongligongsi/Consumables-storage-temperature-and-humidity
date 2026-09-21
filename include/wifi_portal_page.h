#pragma once

// WiFiManager inserts this fragment at the end of <head> on every captive
// portal page.  Keep it self contained: the setup AP has no Internet access.
static const char WIFI_PORTAL_CUSTOM_HEAD[] PROGMEM = R"CHWIFI(
<style>
body.chamber{background:#08100d;color:#eef8f3;font-family:system-ui,-apple-system,"Segoe UI",sans-serif;margin:0;padding:18px}
body.chamber .wrap{display:block;min-width:0;max-width:560px;margin:0 auto;text-align:left}
body.chamber .ch-brand{background:#13201b;border:1px solid #294137;border-radius:14px;padding:16px;margin:0 0 14px}
body.chamber .ch-brand strong{display:block;color:#eef8f3;font-size:1.35rem;font-weight:600;margin-bottom:5px}
body.chamber .ch-brand span{color:#91a59b}
body.chamber h1,body.chamber h3{color:#eef8f3}
body.chamber a{color:#eef8f3}
body.chamber .network-row{background:#13201b;border:1px solid #294137;border-radius:9px;padding:10px 12px;margin:7px 0;min-height:24px}
body.chamber a[data-ssid]{display:inline-block;max-width:58%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;vertical-align:middle}
body.chamber .net-security{display:inline-block;color:#91a59b;background:#1c3028;border-radius:999px;padding:2px 7px;margin-left:7px;font-size:.78rem;vertical-align:middle}
body.chamber label{color:#c9d7d0;font-weight:600}
body.chamber input{background:#09130f;color:#eef8f3;border:1px solid #36574a;border-radius:8px;padding:11px;margin:6px 0 12px}
body.chamber input:focus{border-color:#00d9ef;outline:2px solid rgba(0,217,239,.25)}
body.chamber button{background:#245b65;border-radius:8px;font-weight:600}
body.chamber button:hover{background:#2f7280}
body.chamber .msg{background:#13201b;color:#eef8f3;border-color:#294137;border-left-color:#00d9ef;border-radius:8px}
body.chamber .msg.D{border-left-color:#ff6464}
body.chamber .msg.S{border-left-color:#4ace4a}
body.chamber .q{color:#91a59b}
body.chamber .q[role=img]{filter:invert(1)}
body.chamber .ch-help{color:#91a59b;margin:-6px 0 10px;font-size:.9rem}
body.chamber .ch-section{color:#00d9ef;font-size:1rem;font-weight:600;margin:18px 0 8px}
body.chamber #ch-connect-progress{background:#13201b;border-left:5px solid #ffa600;border-radius:8px;padding:13px;margin:12px 0;color:#eef8f3}
@media(max-width:420px){body.chamber{padding:10px}body.chamber a[data-ssid]{max-width:50%}}
</style>
<script>
document.addEventListener('DOMContentLoaded',function(){
  var zh=document.body.classList.contains('zh');
  var wrap=document.querySelector('.wrap');
  if(!wrap)return;

  var brand=document.createElement('div');
  brand.className='ch-brand';
  brand.innerHTML='<strong>'+(zh?'耗材仓网络配置':'Filament Chamber WiFi Setup')+'</strong><span>'+(zh?'连接 2.4 GHz WiFi，设备不支持 5 GHz':'Connect to a 2.4 GHz network; 5 GHz is not supported')+'</span>';
  wrap.insertBefore(brand,wrap.firstChild);

  // The captive portal landing page only contains a menu. Start an async scan
  // first, then send the user to the full network form.
  if(location.pathname==='/'){
    var note=document.createElement('div');
    note.className='msg';
    note.textContent=zh?'正在扫描附近 WiFi，首次扫描约需 8 秒…':'Scanning nearby WiFi networks; the first scan takes about 8 seconds…';
    brand.insertAdjacentElement('afterend',note);
    setTimeout(function(){location.replace('/wifi');},8500);
  }

  var replacements=zh?[
    ['Configure WiFi (No scan)','手动配置 WiFi'],['Configure WiFi','配置 WiFi'],
    ['Show Password','显示密码'],['Password','WiFi 密码'],
    ['Saving Credentials','正在保存网络凭据'],['Trying to connect ESP to network.','正在连接所选 WiFi。'],
    ['If it fails reconnect to AP to try again','如果连接失败，请重新连接配置热点后再试'],
    ['Authentication failure','密码错误或认证失败'],['AP not found','未找到该 WiFi'],
    ['Could not connect','连接失败，请检查信号和密码'],['Not connected','尚未连接'],
    ['Connected','已连接'],['No AP set','尚未配置 WiFi'],['with IP','设备地址'],
    ['Refresh','重新扫描'],['Save','保存并连接'],['Back','返回'],
    ['SSID','WiFi 名称（SSID）']
  ]:[];
  if(replacements.length){
    var walker=document.createTreeWalker(wrap,NodeFilter.SHOW_TEXT);
    var nodes=[];while(walker.nextNode())nodes.push(walker.currentNode);
    nodes.forEach(function(node){
      var value=node.nodeValue;
      replacements.forEach(function(pair){value=value.split(pair[0]).join(pair[1]);});
      node.nodeValue=value;
    });
  }

  var ssid=document.getElementById('s');
  if(ssid){
    var help=document.createElement('div');
    help.className='ch-help';
    help.textContent=zh?'从上方列表选择，或在此手动输入隐藏网络名称。':'Choose a network above, or manually enter a hidden SSID.';
    ssid.insertAdjacentElement('afterend',help);
  }

  document.querySelectorAll('a[data-ssid]').forEach(function(link){
    var row=link.parentElement;
    row.classList.add('network-row');
    var secure=!!row.querySelector('.q.l');
    var badge=document.createElement('span');
    badge.className='net-security';
    badge.textContent=secure?(zh?'加密':'Secured'):(zh?'开放':'Open');
    link.insertAdjacentElement('afterend',badge);
    var quality=row.querySelector('.q:not(.h)');
    if(quality)quality.setAttribute('aria-label',(zh?'信号强度 ':'Signal strength ')+quality.textContent.trim());
  });

  var form=document.querySelector("form[action='wifisave']");
  if(form){
    var section=document.createElement('div');
    section.className='ch-section';
    section.textContent=zh?'手动输入':'Manual entry';
    form.insertBefore(section,form.firstChild);
    form.addEventListener('submit',function(){
      var old=document.getElementById('ch-connect-progress');if(old)old.remove();
      var progress=document.createElement('div');
      progress.id='ch-connect-progress';progress.setAttribute('role','status');
      progress.textContent=zh?'正在保存并连接，请稍候…':'Saving and connecting, please wait…';
      form.insertBefore(progress,form.firstChild);
      var button=form.querySelector("button[type='submit']");if(button)button.disabled=true;
    });
  }

  var pageText=wrap.textContent||'';
  if(pageText.indexOf('正在保存网络凭据')>=0||pageText.indexOf('Saving Credentials')>=0){
    setTimeout(function(){location.replace('/wifi');},17000);
  }
});
</script>
)CHWIFI";
