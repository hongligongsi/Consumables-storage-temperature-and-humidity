# 构建 WiFiManager 配网页仿真:原生 HTTP_STYLE + 原生 /wifi DOM + 注入片段
import re, pathlib

ROOT = pathlib.Path(r"C:/Users/hongliwang/Desktop/3-1")

# 1) WiFiManager 原生默认样式:逐行提取字符串字面量,跳过 C 注释行
lib = (ROOT / ".pio/libdeps/esp32-s3-n16r8/WiFiManager/wm_strings_en.h").read_text(encoding="utf-8")
start = lib.index("HTTP_STYLE[]")
end = lib.index('</style>";', start)
parts = []
for line in lib[start:end].splitlines():
    s = line.strip()
    if not s or s.startswith("//"):
        continue
    if s.startswith('"'):
        s = s[1:]
        if s.endswith('"'):
            s = s[:-1]
        parts.append(s)
native_css = "".join(parts)
assert "text-decoration:none" in native_css, "a rule must be present"

# 2) 注入片段(include/wifi_portal_page.h 的 R"CHWIFI(...)CHWIFI")
hdr = (ROOT / "include/wifi_portal_page.h").read_text(encoding="utf-8")
m = re.search(r'R"CHWIFI\((.*)\)CHWIFI";', hdr, re.S)
injected = m.group(1)

# 3) /wifi 页面 DOM(按 wm_strings_en.h 模板逐条拼装,与真机一致)
items = "".join(
    "<div><a href='#p' onclick='c(this)' data-ssid='{V}'>{v}</a>"
    "<div role='img' aria-label='{r}%' title='{r}%' class='q q-{q}'></div>"
    "<div class='q'>{r}%</div></div>".replace("{q}", q).replace("{r}", r).replace("{v}", v).replace("{V}", V)
    for q, r, v, V in [
        ("4", "92", "HomeWiFi", "HomeWiFi"),
        ("3", "76", "Printer-Room", "Printer-Room"),
        ("2", "58", "Workshop-IoT", "Workshop-IoT"),
        ("1", "34", "Guest", "Guest"),
    ]
)
page = f"""<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>WiFi Setup</title>
<style>{native_css}</style>
{injected}
</head><body class='chamber zh'><div class='wrap'>
<h1>WiFi Config</h1>
<div class='msg P'><strong>Not connected</strong> to HomeWiFi</div>
{items}
<br/><form action='/wifi?refresh=1' method='POST'><button name='refresh' value='1'>Refresh</button></form>
<form method='POST' action='wifisave'>
<label for='s'>SSID</label><input id='s' name='s' maxlength='32' autocorrect='off' autocapitalize='none' placeholder='HomeWiFi'><br/>
<label for='p'>Password</label><input id='p' name='p' maxlength='64' type='password' placeholder='pwd'><input type='checkbox' id='showpass' onclick='f()'> <label for='showpass'>Show Password</label><br/>
<br/><br/><button type='submit'>Save</button></form>
<hr><br/><form action='/' method='get'><button>Back</button></form>
</div></body></html>"""

out = ROOT / ".workbuddy/tmp/wifi-portal-sim.html"
out.write_text(page, encoding="utf-8")
print("written:", out, len(page), "bytes")
