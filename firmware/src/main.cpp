#include <Arduino.h>
/*
 * ESP8266 WiFi 配置 + AI 对话 固件 (v2)
 *
 * 功能:
 *   1. 上电先开启 SoftAP 热点 "ESP8266-Web" (密码 12345678), 始终可用,
 *      兼做回退 (Station 连不上时不至于失联)。
 *   2. 网页 / 提供 2.4GHz WiFi 扫描列表, 任选一个输密码连接; 默认留空
 *      (15s 超时, 未配置时跳过), 选择持久化到 flash (EEPROM 区)。
 *   3. 连入局域网后, 网页提供简单的 AI 对话窗口; 远端模型 API
 *      (OpenAI 兼容 /v1/chat/completions) 的地址 / Key / 模型名均可在
 *      网页配置并持久化。默认留空, 首次上电在网页填写 (例如 llama.cpp
 *      的 OpenAI 兼容服务)。
 *   4. 引脚控制: 网页面板直接操作, 或在 AI 对话里用自然语言说
 *      ("让引脚 5 高电平持续 2 秒" / "让 LED 闪烁"), 模型按约定协议
 *      输出 [PIN]{...}[/PIN] 指令, 固件解析执行并在回复中反馈结果。
 *
 * ============ 历史「坑」的根因(务必保留此注释) ============
 * - board = esp01_1m -> variant generic -> LED_BUILTIN 被定义为 1,
 *   而 GPIO1 正是 UART0 的 TXD。【绝不】引用 LED_BUILTIN, 全程不碰
 *   GPIO1(TX)/GPIO3(RX)。板上可见 LED 用 GPIO2。
 * - 源文件必须是 src/main.cpp (不是 .ino): .ino 预处理器会把函数原型
 *   插进跨行原始字符串, 编译不报错但网页 JS 全废。
 * - 抓串口必须先 setDTR(False) (见 monitor_app.py), 否则芯片进下载模式。
 * - ESP8266 只支持 2.4GHz WiFi; loop() 里禁止长 delay() (会阻塞 HTTP)。
 * ========================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// ---------------- 配置 ----------------
static const char *AP_SSID = "ESP8266-Web";
static const char *AP_PASS = "12345678";  // SoftAP 回退热点, 密码需 >= 8 位
static const char *MDNS_NAME = "esp8266-web";

// 默认 STA: 只能 2.4GHz (5GHz 网络 ESP8266 硬件扫不到, 不会出现在列表)。
// 默认留空: 首次上电在热点网页选择 WiFi, 之后持久化生效。
static const char *DEF_STA_SSID = "";
static const char *DEF_STA_PASS = "";

// 默认远端模型 API (OpenAI 兼容 /v1)。默认留空: 首次上电在网页填写
// 地址 / Key / 模型名, 之后持久化生效。例: llama.cpp --port 8000 服务。
static const char *DEF_API_URL   = "";
static const char *DEF_API_KEY   = "";
static const char *DEF_API_MODEL = "";

// 绝不使用 LED_BUILTIN(==1==TXD)! 板上可见 LED 在 GPIO2。
static const uint8_t LED_PIN = 2;
static const bool    LED_ACTIVE_LOW = true;

// ---------------- 全局 ----------------
ESP8266WebServer server(80);

// 配置持久化: 用 framework 自带的 EEPROM 仿真库 (此 framework 版本没有
// Preferences)。本板 1MB 布局 (eagle.flash.1m256.ld) 的 EEPROM 区在
// flash 物理偏移 0xFB000 (4KB) (_EEPROM_start = 0x402FB000, 减去 flash
// 映射基址 0x40200000), 位于 app 镜像与 SPIFFS 区之后, esptool
// write_flash 0x0 不会碰它, 断电/重烧录都保留。
struct __attribute__((packed)) CfgBlob {
  uint32_t magic;    // 0x43505345 "ESPC"
  uint8_t  version;  // 1
  uint8_t  pad[3];
  char ssid[33];
  char pass[65];
  char url[97];
  char key[97];
  char model[97];
};
static const uint32_t CFG_MAGIC = 0x43505345;
static void cfgSave();

String cfgSsid     = DEF_STA_SSID;
String cfgPass     = DEF_STA_PASS;
String cfgApiUrl   = DEF_API_URL;
String cfgApiKey   = DEF_API_KEY;
String cfgApiModel = DEF_API_MODEL;

// ---------------- 引脚控制 (页面 + AI 对话共用) ----------------
// ESP8266 有 GPIO 0..16。本模块 (ESP-01) 可用输出引脚: 2,3,4,5,15,16
//   0  = flash 片选 CS, 驱动低电平会关掉 flash 导致芯片崩溃
//   1  = UART TXD, 动了就废掉串口输出
//   2  = 板载 LED (低电平点亮)
//   16 = RTC 引脚, 只能输出不能读
//   6..14 = flash 数据/时钟引脚或本模块未引出
enum { PIN_IDLE = 0, PIN_HIGH, PIN_LOW, PIN_BLINK, PIN_HEARTBEAT };

struct PinCtl {
  uint8_t  mode;        // PIN_*
  bool     level;       // 当前电平 (闪烁翻转用)
  uint32_t intervalMs;  // 闪烁翻转间隔
  uint32_t untilMs;     // 非 0: 到点自动恢复低电平 (PIN_HIGH/PIN_BLINK)
  uint32_t lastToggle;  // 上次翻转的 millis()
};
static PinCtl g_pins[17];

static const bool PIN_OK[17] = {
  false, false, true, true, true, true,   // 0..5
  false, false, false, false, false,      // 6..10
  false, false, false, false,             // 11..14
  true, true                              // 15,16
};
static const char *PIN_NOTES[17] = {
  "是 flash 片选(CS), 驱动会使芯片崩溃",
  "是串口 TXD, 控制会打乱串口输出",
  "板载 LED", "", "", "",
  "flash 数据引脚", "flash 数据引脚", "flash 引脚",
  "本模块未引出", "本模块未引出",
  "flash 数据引脚", "flash 数据引脚", "flash 数据引脚", "flash 时钟引脚",
  "", "只能输出, 不能读取"
};

static void pinApply(uint8_t pin, uint8_t mode, uint32_t intervalMs, uint32_t untilMs);
static String execPinCmd(const String &json);
static void pinTick();

static unsigned long g_bootMs        = 0;
static unsigned long g_lastBeat      = 0;
static bool          g_ledLevel      = false;
static bool          g_staConnecting = false;  // 页面发起的连接请求进行中
static unsigned long g_staT0         = 0;

// ---------------- 首页 (PROGMEM, 整段替换即可) ----------------
static const char PAGE_INDEX[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 WiFi + AI 控制台</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;color:#e8eef7;background:#0b1220;
background-image:radial-gradient(900px 480px at 12% -12%,#17365a,transparent),
radial-gradient(700px 380px at 100% 0%,#1e2b49,transparent);
font:15px/1.6 system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans SC",sans-serif}
.wrap{max-width:820px;margin:0 auto;padding:26px 16px 56px}
h1{margin:0;font-size:21px}
h1 small{font-size:12px;color:#7f97b3;font-weight:400}
.sub{margin:6px 0 22px;color:#8ea6c2;font-size:13px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}
.card{background:#111c2c;border:1px solid #21334a;border-radius:14px;padding:13px 15px}
.k{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:#7f97b3}
.v{margin-top:3px;font-size:21px;font-weight:650;font-variant-numeric:tabular-nums;word-break:break-all}
.v small{font-size:13px;font-weight:500;color:#9fb4cc}
.panel{margin-top:22px;background:#111c2c;border:1px solid #21334a;border-radius:14px;padding:16px}
.panel h2{margin:0 0 12px;font-size:14px;font-weight:600;color:#a9c0da}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
button{font:inherit;font-weight:600;padding:10px 16px;border-radius:11px;cursor:pointer;
border:1px solid #2b4568;background:#16273c;color:#e8eef7;transition:background .15s}
button:hover{background:#1d3450}
button:disabled{opacity:.5;cursor:wait}
button.danger{border-color:#5a2b35;background:#2a1419}
button.danger:hover{background:#3a1c23}
input{font:inherit;padding:9px 12px;border-radius:11px;border:1px solid #2b4568;
background:#0e1929;color:#e8eef7;outline:none}
input:focus{border-color:#3d6ea8}
.hint{color:#7f97b3;font-size:12.5px;margin-top:10px;line-height:1.55}
.banner{margin-top:14px;padding:11px 14px;border-radius:12px;background:#0f2b1c;
border:1px solid #1f5c38;color:#8fe3b1;font-size:13.5px}
.hidden{display:none}
.cur{font-size:13.5px;color:#c6d6ea;margin-bottom:10px}
.netlist{margin-top:12px;max-height:230px;overflow:auto;border:1px solid #21334a;
border-radius:12px;padding:6px}
.net{display:flex;align-items:center;gap:9px;padding:7px 9px;border-radius:9px;cursor:pointer;font-size:14px}
.net:hover{background:#16273c}
.net em{margin-left:auto;font-style:normal;color:#7f97b3;font-size:12px;white-space:nowrap}
.net i{width:8px;height:8px;border-radius:50%;background:#3a4c63;flex:none}
.net i.strong{background:#39d98a}.net i.mid{background:#e8c34a}.net i.weak{background:#b3574a}
.cfg{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
.cfg label{display:flex;flex-direction:column;gap:5px;font-size:12px;color:#8ea6c2}
.cfg label.wide{grid-column:1/-1}
.cfg .savebtn{align-self:end}
.chat{height:260px;overflow:auto;background:#0a111d;border:1px solid #21334a;
border-radius:12px;padding:12px;display:flex;flex-direction:column;gap:9px}
.m{max-width:82%;padding:8px 12px;border-radius:12px;font-size:14px;white-space:pre-wrap;word-break:break-word}
.m.user{align-self:flex-end;background:#1d3a5c;border:1px solid #2c5580}
.m.ai{align-self:flex-start;background:#13233a;border:1px solid #23405e}
.m.sys{align-self:center;background:transparent;color:#7f97b3;font-size:12.5px}
</style>
</head>
<body>
<div class="wrap">
  <h1>ESP8266 控制台 <small>WiFi 配置 + AI 对话</small></h1>
  <div class="sub" id="sub">正在读取状态…</div>
  <div class="grid">
    <div class="card"><div class="k">运行时长</div><div class="v" id="uptime">–</div></div>
    <div class="card"><div class="k">可用内存</div><div class="v" id="heap">–</div></div>
    <div class="card"><div class="k">信号强度</div><div class="v" id="rssi">–</div></div>
    <div class="card"><div class="k">局域网 IP</div><div class="v" id="ip">–</div></div>
  </div>
  <div id="apBanner" class="banner hidden"></div>

  <div class="panel">
    <h2>① WiFi 连接 (仅 2.4GHz)</h2>
    <div class="cur">当前状态: <span id="curWifi">–</span></div>
    <div class="row">
      <button onclick="doScan()">扫描网络</button>
      <button class="danger" onclick="doDisconnect()">断开 STA</button>
      <span style="font-size:12px;color:#7f97b3">回退热点 "ESP8266-Web" (密码 12345678) 始终可用</span>
    </div>
    <div id="netlist" class="netlist"><div class="hint" style="padding:6px">正在首次扫描… (约 3 秒)</div></div>
    <div class="row" style="margin-top:10px">
      <input id="pass" type="password" placeholder="所选网络的密码 (开放网络留空)" style="flex:1;min-width:180px">
      <button onclick="doConnect()">连接所选 WiFi</button>
    </div>
    <div class="hint">首次使用请先在此选择 2.4GHz 网络并输入密码 (选择会记住, 断电重连自动生效)。ESP8266 硬件只支持 2.4GHz, 扫不到 5GHz 网络。</div>
  </div>

  <div class="panel">
    <h2>② AI 对话 (需要 ESP 已连入局域网)</h2>
    <div class="cfg">
      <label class="wide">模型 API 地址 (OpenAI 兼容, 以 /v1 结尾)
        <input id="cfgUrl" placeholder="http://&lt;模型服务器IP&gt;:8000/v1">
      </label>
      <label>API Key
        <input id="cfgKey" placeholder="可留空">
      </label>
      <label>模型名 / 模型 ID
        <input id="cfgModel" placeholder="模型名">
      </label>
      <button class="savebtn" onclick="saveCfg()">保存模型配置</button>
    </div>
    <div id="chat" class="chat"><div class="m sys">对话历史保存在浏览器里, 刷新即清空; 单次回复上限约 512 token。</div></div>
    <div class="row" style="margin-top:10px">
      <input id="msg" placeholder="输入消息, Enter 发送" style="flex:1;min-width:200px">
      <button id="sendBtn" onclick="doSend()">发送</button>
    </div>
  </div>

  <div class="panel">
    <h2>③ 引脚控制 (也可以直接在上面的 AI 对话里说)</h2>
    <div class="row">
      <select id="pinSel" style="padding:9px 12px;border-radius:11px;border:1px solid #2b4568;background:#0e1929;color:#e8eef7">
        <option value="2">引脚 2 (板载 LED)</option>
        <option value="3">引脚 3</option>
        <option value="4">引脚 4</option>
        <option value="5">引脚 5</option>
        <option value="15">引脚 15</option>
        <option value="16">引脚 16 (只能输出)</option>
      </select>
      <button onclick="pinDo('high')">高电平</button>
      <button onclick="pinDo('low')">低电平</button>
      <button onclick="pinDo('blink')">闪烁</button>
      <button onclick="pinDo('off')">关闭</button>
    </div>
    <div class="row" style="margin-top:10px">
      <input id="pinInt" type="number" value="500" min="20" max="30000" placeholder="闪烁间隔 ms" style="width:140px">
      <input id="pinDur" type="number" min="100" max="3600000" placeholder="持续 ms (空=保持)" style="width:180px">
      <span style="font-size:12px;color:#7f97b3">持续时间到点后自动恢复低电平</span>
    </div>
    <div class="hint">当前状态: <span id="pinState" style="color:#8fe3b1">—</span><br>
    AI 对话里也可以直接说: "让引脚 5 高电平持续 2 秒"、"让 LED 每 300 毫秒闪一次"、"关闭 LED"、"恢复 LED 心跳"。
    引脚 0 是 flash 片选、引脚 1 是串口 TX, 驱动会使芯片崩溃/打乱串口, 已锁定; 6~14 本模块未引出或为 flash 引脚。</div>
  </div>
</div>
<script>
var hist=[{role:'system',content:'你是一个运行在 ESP8266 小硬件上的简洁助手, 可以用自然语言控制板子引脚(高/低电平、闪烁、LED 状态), 请用中文简短回答。'}];
function el(id){return document.getElementById(id);}
function esc(t){return String(t).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');}
function fmt(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
 return (d?d+'天 ':'')+(h?h+'小时 ':'')+m+'分 '+(s%60)+'秒';}
function refresh(){
 fetch('/api/status').then(function(r){return r.json()}).then(function(j){
  el('uptime').textContent=fmt(j.uptime);
  el('heap').innerHTML=j.heap+' <small>B</small>';
  el('rssi').innerHTML=(j.wifi==='connected'?j.rssi+' <small>dBm</small>':'—');
  el('ip').textContent=(j.wifi==='connected'?j.ip:'未连入局域网');
  el('curWifi').textContent=(j.wifi==='connected'?'已连接 '+j.ssid:(j.wifi==='connecting'?'连接中… (超时约 20 秒)':'未连接, 仅热点模式 (192.168.4.1)'));
  el('sub').textContent='模式 '+j.mode+' · MAC '+j.mac;
  var b=el('apBanner');
  if(location.hostname==='192.168.4.1'&&j.wifi==='connected'){
   b.textContent='已连入局域网! 你现在也可以直接用 '+j.ip+' 访问本页面 (热点 192.168.4.1 继续可用)。';
   b.classList.remove('hidden');
  } else {b.classList.add('hidden');}
 }).catch(function(){el('sub').textContent='连接中断, 重试中…';});
 refreshPins();
}}
function doScan(){
 el('netlist').innerHTML='<div class="hint" style="padding:6px">正在扫描… (约 3 秒)</div>';
 fetch('/api/wifi/scan').then(function(r){return r.json()}).then(function(j){
  if(!j.networks||!j.networks.length){el('netlist').innerHTML='<div class="hint" style="padding:6px">未扫到任何 2.4GHz 网络</div>';return;}
  var h='';
  j.networks.forEach(function(n){
   var sig=n.rssi>=-60?'strong':(n.rssi>=-72?'mid':'weak');
   h+='<label class="net"><input type="radio" name="net" value="'+esc(n.ssid)+'"><i class="'+sig+'"></i><span>'+esc(n.ssid)+'</span><em>'+n.rssi+' dBm</em></label>';
  });
  el('netlist').innerHTML=h;
 }).catch(function(){el('netlist').innerHTML='<div class="hint" style="padding:6px">扫描失败, 请重试</div>';});
}
function doConnect(){
 var sel=document.querySelector('input[name=net]:checked');
 if(!sel){alert('请先"扫描网络"并勾选一个');return;}
 var body='ssid='+encodeURIComponent(sel.value)+'&pass='+encodeURIComponent(el('pass').value);
 el('curWifi').textContent='连接中… (超时约 20 秒)';
 fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
 .then(function(r){return r.json()}).then(function(j){
  if(j.error){alert('连接失败: '+j.error);el('curWifi').textContent='连接失败';}
 }).catch(function(){alert('请求失败');});
}
function doDisconnect(){
 fetch('/api/wifi/disconnect',{method:'POST'}).then(function(r){return r.json()}).then(function(j){
  if(j.error){alert(j.error);}
 }).catch(function(){alert('请求失败');});
}
function pinDo(action){
 var pin=el('pinSel').value;
 var iv=parseInt(el('pinInt').value)||500;
 var du=parseInt(el('pinDur').value)||0;
 var body='pin='+pin+'&action='+action+'&interval_ms='+iv+'&duration_ms='+du;
 fetch('/api/pin',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
 .then(function(r){return r.json()}).then(function(j){
   if(j.error){alert(j.error);} else {refreshPins();}
 }).catch(function(){alert('请求失败');});
}
function refreshPins(){
 fetch('/api/pins').then(function(r){return r.json()}).then(function(j){
   var names={high:'高电平',low:'低电平',blink:'闪烁',heartbeat:'心跳闪烁'};
   var act=[];
   Object.keys(j.pins).forEach(function(p){
     var v=j.pins[p],i=v.indexOf(':');
     var m=i>=0?v.substring(0,i):v;
     if(m==='idle'){return;}
     var s='pin'+p+'='+(names[m]||m);
     if(m==='blink'&&i>=0){s+=' @'+v.substring(i+1)+'ms';}
     act.push(s);
   });
   el('pinState').textContent=act.length?act.join(', '):'无活动引脚';
 }).catch(function(){});
}
function loadCfg(){
 fetch('/api/chat/config').then(function(r){return r.json()}).then(function(j){
  el('cfgUrl').value=j.url||'';el('cfgKey').value=j.key||'';el('cfgModel').value=j.model||'';
 }).catch(function(){});
}
function saveCfg(){
 var body='url='+encodeURIComponent(el('cfgUrl').value.trim())
  +'&key='+encodeURIComponent(el('cfgKey').value)
  +'&model='+encodeURIComponent(el('cfgModel').value.trim());
 fetch('/api/chat/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
 .then(function(r){return r.json()}).then(function(j){
  alert(j.error?('保存失败: '+j.error):'模型配置已保存');
 }).catch(function(){alert('请求失败');});
}
function addMsg(cls,txt){
 var d=document.createElement('div');d.className='m '+cls;d.textContent=txt;
 var c=el('chat');c.appendChild(d);c.scrollTop=c.scrollHeight;return d;
}
function doSend(){
 var t=el('msg').value.trim();
 if(!t||el('sendBtn').disabled){return;}
 el('msg').value='';
 addMsg('user',t);
 hist.push({role:'user',content:t});
 if(hist.length>21){hist=hist.slice(-20);}
 var wait=addMsg('sys','思考中… (本地 27B 模型, 约几秒~十几秒)');
 el('sendBtn').disabled=true;
 var body='history='+encodeURIComponent(JSON.stringify(hist));
 fetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
 .then(function(r){return r.json()}).then(function(j){
  wait.remove();
  if(j.error){addMsg('sys','错误: '+j.error);return;}
  addMsg('ai',j.reply);
  hist.push({role:'assistant',content:j.reply});
 }).catch(function(e){wait.remove();addMsg('sys','请求失败: '+e);})
 .then(function(){el('sendBtn').disabled=false;});
}
el('msg').onkeydown=function(e){if(e.key==='Enter'){doSend();}};
refresh();setInterval(refresh,2000);loadCfg();doScan();
</script>
</body>
</html>
)HTML";

// ---------------- 小工具 ----------------
static void jsonEsc(const String &s, String &out) {
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", (unsigned)c);
      out += b;
    } else {
      out += c;
    }
  }
}

static const char *statusName(int s) {
  switch (s) {
    case 0: return "IDLE";
    case 1: return "NO_SSID_AVAIL(扫不到该SSID: 多半是5GHz或名字写错)";
    case 2: return "SCAN_COMPLETED";
    case 3: return "CONNECTED";
    case 4: return "CONNECT_FAILED(密码/认证失败?)";
    case 5: return "CONNECTION_LOST";
    case 6: return "WRONG_PASSWORD/其他";
    case 7: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

// ---------------- 状态 JSON ----------------
static void sendStatus() {
  bool staOk = (WiFi.status() == WL_CONNECTED);
  String ip = (staOk ? WiFi.localIP() : WiFi.softAPIP()).toString();
  const char *wifi = staOk ? "connected" : (g_staConnecting ? "connecting" : "disconnected");
  String ssid = staOk ? WiFi.SSID() : String();
  String json;
  json.reserve(300);
  json = "{\"uptime\":";
  json += String((unsigned long)((millis() - g_bootMs) / 1000UL));
  json += ",\"heap\":";
  json += String((unsigned)ESP.getFreeHeap());
  json += ",\"rssi\":";
  json += String(staOk ? (int)WiFi.RSSI() : -1);
  json += ",\"ip\":\"";
  jsonEsc(ip, json);
  json += "\",\"ap_ip\":\"192.168.4.1\",\"mac\":\"";
  jsonEsc(WiFi.macAddress(), json);
  json += "\",\"mode\":\"";
  json += (staOk ? "APSTA" : "AP");
  json += "\",\"led\":";
  json += String((unsigned)g_pins[LED_PIN].mode);
  json += ",\"ssid\":\"";
  jsonEsc(ssid, json);
  json += "\",\"wifi\":\"";
  json += wifi;
  json += "\"}";
  server.send(200, "application/json", json);
}

static void handleLed() {
  if (server.hasArg("mode")) {
    int m = (int)constrain(server.arg("mode").toInt(), 0, 2);
    if (m == 0) pinApply(LED_PIN, PIN_HEARTBEAT, 0, 0);
    else if (m == 1) pinApply(LED_PIN, PIN_HIGH, 0, 0);
    else pinApply(LED_PIN, PIN_LOW, 0, 0);
  }
  sendStatus();
}

static void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "404 Not Found");
}

// ---------------- WiFi 扫描 / 连接 / 断开 ----------------
static void handleScan() {
  int n = WiFi.scanNetworks();  // 阻塞 2~4s, 单用户设备可接受
  if (n < 0) {
    server.send(500, "application/json", "{\"error\":\"scan failed\",\"code\":" + String(n) + "}");
    return;
  }
  String seen[16];
  int seenN = 0, added = 0;
  String json;
  json.reserve(1400);
  json = "{\"networks\":[";
  for (int i = 0; i < n && added < 15; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool dup = false;
    for (int k = 0; k < seenN; k++) {
      if (seen[k] == ssid) { dup = true; break; }
    }
    if (dup) continue;
    if (seenN < 16) seen[seenN++] = ssid;
    if (added++) json += ",";
    json += "{\"ssid\":\"";
    jsonEsc(ssid, json);
    json += "\",\"rssi\":";
    json += String((int)WiFi.RSSI(i));
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

static void handleWifiConnect() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400, "application/json", "{\"error\":\"SSID 为空或超过 32 字符\"}");
    return;
  }
  if (pass.length() > 64) {
    server.send(400, "application/json", "{\"error\":\"密码超过 64 字符\"}");
    return;
  }
  cfgSsid = ssid;
  cfgPass = pass;
  cfgSave();

  g_staConnecting = true;
  g_staT0 = millis();
  WiFi.disconnect(true);  // 清掉当前配置, AP 保持
  delay(100);
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : (const char *)NULL);
  Serial.printf("[..] 网页请求连接 \"%s\" (20s 超时, 结果见 [OK]/[ERR] 与 /api/status)\n",
                ssid.c_str());

  String out;
  out.reserve(96);
  out = "{\"state\":\"connecting\",\"ssid\":\"";
  jsonEsc(ssid, out);
  out += "\"}";
  server.send(202, "application/json", out);
}

static void handleWifiDisconnect() {
  g_staConnecting = false;
  WiFi.disconnect(true);  // 只断 STA, AP 热点保持
  Serial.println("[..] 网页请求断开 STA (热点保持)");
  server.send(200, "application/json", "{\"state\":\"disconnected\"}");
}

// ---------------- 模型 API 配置 ----------------
static void handleChatConfigGet() {
  String out;
  out.reserve(288);
  out = "{\"url\":\"";
  jsonEsc(cfgApiUrl, out);
  out += "\",\"key\":\"";
  jsonEsc(cfgApiKey, out);
  out += "\",\"model\":\"";
  jsonEsc(cfgApiModel, out);
  out += "\"}";
  server.send(200, "application/json", out);
}

static void handleChatConfigSet() {
  String url = server.arg("url");
  String key = server.arg("key");
  String model = server.arg("model");
  if (url.length() > 96 || key.length() > 96 || model.length() > 96) {
    server.send(400, "application/json", "{\"error\":\"配置字段超过 96 字符\"}");
    return;
  }
  while (url.endsWith("/")) {
    url = url.substring(0, url.length() - 1);
  }
  cfgApiUrl = url;
  cfgApiKey = key;
  cfgApiModel = model;
  cfgSave();
  Serial.printf("[..] 模型 API 配置已更新: %s (model=%s)\n", url.c_str(), model.c_str());
  handleChatConfigGet();  // 返回保存后的配置
}

// ---------------- AI 引脚控制指令协议 ----------------
// 服务端在 messages 数组最前面注入这条协议系统消息 (客户端自带的那条
// system 保留, 两条 system 并存对模型无害)。要求模型在需要控制引脚时
// 输出一行 [PIN]{...}[/PIN] 指令, 本固件解析执行并在回复中反馈结果。
static const char PIN_PROTOCOL[] PROGMEM =
  "你是 ESP8266 开发板的控制助手, 可以通过文字控制板子引脚。 "
  "可操作的引脚: 2 (板载LED), 3, 4, 5, 15, 16 (其中 16 只能输出)。 "
  "当用户要求操作引脚(输出高/低电平、保持一段时间、闪烁、LED 常亮/闪烁/关闭/心跳)时, "
  "你的回复中必须包含一行控制指令, 格式为: "
  "[PIN]{\"pin\":引脚号,\"action\":\"high|low|blink|off|heartbeat\",\"interval_ms\":闪烁翻转间隔毫秒,\"duration_ms\":保持毫秒}[/PIN] "
  "然后再用一句简短中文说明。 "
  "规则: "
  "1) action 含义: high=输出高电平, low/off=输出低电平(off 等同 low), blink=闪烁, heartbeat=仅 pin2 恢复 1Hz 心跳闪烁; "
  "2) 用户指定了持续时间(如 \"2秒\"、\"1.5秒\")时必须填 duration_ms(单位毫秒), 到时自动恢复低电平; 未指定则省略该字段; "
  "3) blink 的 interval_ms 为翻转间隔, 缺省 500, 范围 20~30000; "
  "4) 用户要求列表之外的引脚时, 说明该引脚不可操作, 不要输出 [PIN] 指令; "
  "5) 指令是否执行成功以系统反馈为准, 不要假装已执行; "
  "6) 与引脚控制无关的问题正常回答, 不要输出 [PIN] 指令。 "
  "请用中文简短回答。";

// ---------------- AI 对话转发 ----------------
static void handleChat() {
  String history = server.arg("history");  // JSON 数组: [{role,content},...]
  if (history.length() == 0 || history.length() > 4000) {
    server.send(400, "application/json", "{\"error\":\"history 参数缺失或超过 4000 字符\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    server.send(409, "application/json",
                "{\"error\":\"ESP 未连入局域网(当前仅热点模式), 无法访问远端模型 API\"}");
    return;
  }
  if (cfgApiUrl.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"未配置模型 API 地址\"}");
    return;
  }

  String fullUrl = cfgApiUrl + "/chat/completions";
  // messages = [协议 system, ...客户端历史...]。纯字符串拼接, 不解析 JSON,
  // 省掉一份文档的堆开销; 客户端自带的 system 消息原样保留。
  String body = "{\"model\":\"";
  jsonEsc(cfgApiModel, body);
  body += "\",\"messages\":[{";
  body += "\"role\":\"system\",\"content\":\"";
  jsonEsc(PIN_PROTOCOL, body);
  body += "\"},";
  if (history.length() >= 2 && history[0] == '[' && history[history.length() - 1] == ']') {
    body += history.substring(1, history.length() - 1);
  } else {
    body += history;
  }
  body += "],\"stream\":false,\"max_tokens\":512}";

  HTTPClient http;
  WiFiClient httpClient;
  if (!http.begin(httpClient, fullUrl)) {
    server.send(400, "application/json", "{\"error\":\"模型 API 地址格式无效\"}");
    return;
  }
  http.setTimeout(60000);  // socket 读写超时 (本 framework 无 setConnectTimeout)
  http.addHeader("Content-Type", "application/json");
  if (cfgApiKey.length() > 0) {
    http.setAuthorization("Bearer " + cfgApiKey);
  }

  int code = http.POST(body);
  if (code <= 0) {
    http.end();
    String err;
    err.reserve(220);
    err = "{\"error\":\"无法连接模型服务: ";
    jsonEsc(http.errorToString(code), err);
    err += "\",\"url\":\"";
    jsonEsc(fullUrl, err);
    err += "\"}";
    server.send(502, "application/json", err);
    return;
  }

  String resp = http.getString();
  http.end();

  if (code != 200) {
    String err;
    err.reserve(280);
    err = "{\"error\":\"模型接口 HTTP ";
    err += String(code);
    err += ": \"";
    int cut = resp.length() > 200 ? 200 : (int)resp.length();
    jsonEsc(resp.substring(0, cut), err);
    err += "\"}";
    server.send(502, "application/json", err);
    return;
  }

  DynamicJsonDocument doc(4096);  // ArduinoJson v6: JsonDocument 是抽象基类
  DeserializationError e = deserializeJson(doc, resp);
  if (e) {
    server.send(502, "application/json", "{\"error\":\"无法解析模型响应 JSON\"}");
    return;
  }
  String reply = doc["choices"][0]["message"]["content"] | "";
  if (reply.length() == 0) {
    reply = doc["choices"][0]["message"]["reasoning_content"] | "";
  }
  if (reply.length() > 2500) {
    reply = reply.substring(0, 2500);
  }
  long tokens = doc["usage"]["total_tokens"] | 0;

  // 及时释放大对象, 压低峰值堆占用
  resp = String();
  body = String();
  history = String();

  // 解析并执行模型回复里的引脚控制指令
  {
    int s = reply.indexOf("[PIN]");
    int e2 = (s >= 0) ? reply.indexOf("[/PIN]", s) : -1;
    if (s >= 0 && e2 > s) {
      String cmd = reply.substring(s + 5, e2);
      int cut = e2 + 6;
      if (cut < (int)reply.length() && reply[cut] == '\n') cut++;
      reply = reply.substring(0, s) + reply.substring(cut);  // 去掉指令行本身
      reply += execPinCmd(cmd);
    } else {
      // 兜底: 模型没带标记但回复里有含 pin 和 action 的 JSON 对象,
      // 取第一个 { ... } 尝试执行
      int a = reply.indexOf("\"pin\"");
      int b = reply.indexOf("\"action\"");
      if (a >= 0 && b >= 0) {
        int brace = reply.lastIndexOf('{', min(a, b));
        if (brace >= 0) {
          int depth = 0, end = -1;
          for (int i = brace; i < (int)reply.length(); i++) {
            if (reply[i] == '{') depth++;
            else if (reply[i] == '}') {
              depth--;
              if (depth == 0) { end = i + 1; break; }
            }
          }
          if (end > brace) {
            reply += execPinCmd(reply.substring(brace, end));
          }
        }
      }
    }
  }

  // 输出 JSON 手工拼装 (ArduinoJson v6 的 JsonDocument 是抽象类, 且再分配
  // 一份文档会白占 ~4KB 堆)
  String outStr;
  outStr.reserve(reply.length() + 64);
  outStr = "{\"reply\":\"";
  jsonEsc(reply, outStr);
  outStr += "\",\"tokens\":";
  outStr += String(tokens);
  outStr += "}";
  server.send(200, "application/json", outStr);
}

// ---------------- LED ----------------
static void ledWrite(bool on) {
  g_ledLevel = on;
  digitalWrite(LED_PIN, (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}

// ---------------- 引脚控制实现 ----------------
static void pinApply(uint8_t pin, uint8_t mode, uint32_t intervalMs, uint32_t untilMs) {
  PinCtl &c = g_pins[pin];
  if (pin != LED_PIN) pinMode(pin, OUTPUT);
  c.mode = mode;
  c.intervalMs = (mode == PIN_HEARTBEAT) ? 500UL : intervalMs;
  c.untilMs = untilMs;
  c.lastToggle = millis();
  if (mode == PIN_HIGH) {
    c.level = true;
    if (pin == LED_PIN) ledWrite(true); else digitalWrite(pin, HIGH);
  } else if (mode == PIN_LOW || mode == PIN_IDLE) {
    c.level = false;
    if (pin == LED_PIN) ledWrite(false); else digitalWrite(pin, LOW);
  } else {  // PIN_BLINK / PIN_HEARTBEAT: 从低电平起, 间隔到后翻转
    c.level = false;
  }
}

// loop() 里调用: 非阻塞地驱动所有引脚 (闪烁翻转 / 心跳 / 到时恢复)
static void pinTick() {
  for (int p = 0; p < 17; p++) {
    PinCtl &c = g_pins[p];
    if (c.mode == PIN_BLINK) {
      if (c.untilMs && (long)(millis() - c.untilMs) >= 0) {
        pinApply(p, PIN_LOW, 0, 0);  // 到时: 停闪恢复低电平
      } else if ((long)(millis() - c.lastToggle) >= (long)c.intervalMs) {
        c.lastToggle = millis();
        c.level = !c.level;
        if (p == LED_PIN) ledWrite(c.level);
        else digitalWrite(p, c.level ? HIGH : LOW);
      }
    } else if (c.mode == PIN_HEARTBEAT) {
      if (millis() - c.lastToggle >= 500UL) {  // 1Hz 心跳
        c.lastToggle = millis();
        c.level = !c.level;
        ledWrite(c.level);
      }
    } else if (c.mode == PIN_HIGH && c.untilMs && (long)(millis() - c.untilMs) >= 0) {
      pinApply(p, PIN_LOW, 0, 0);  // 到时: 恢复低电平
    }
  }
}

// 解析并执行一条引脚控制指令 JSON, 返回给调用方展示的反馈文本 (以 "\n\n" 开头)
static String execPinCmd(const String &json) {
  String out;
  out.reserve(128);
  DynamicJsonDocument doc(384);
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
    out = "\n\n⚠️ 指令解析失败: 不是合法的 JSON";
    Serial.println(out);
    return out;
  }
  int pin = doc["pin"] | -1;
  String action = doc["action"] | "";
  if (pin < 0 || pin > 16) {
    out = "\n\n⚠️ 指令无效: 引脚号超出范围 (0~16)";
    Serial.println(out);
    return out;
  }
  if (!PIN_OK[pin]) {
    out = "\n\n⚠️ 引脚 ";
    out += String(pin);
    out += " 不可操作: ";
    out += PIN_NOTES[pin];
    Serial.println(out);
    return out;
  }
  uint32_t interval = doc["interval_ms"] | 500UL;
  if (interval < 20) interval = 20;
  if (interval > 30000) interval = 30000;
  uint32_t dur = doc["duration_ms"] | 0UL;
  if (dur > 3600000) dur = 3600000;
  uint32_t until = dur ? (millis() + dur) : 0;

  const char *name = (pin == LED_PIN) ? "板载 LED" : "引脚";
  if (action == "high") {
    pinApply(pin, PIN_HIGH, 0, until);
    out = "\n\n⚡ 已执行: ";
    out += name;
    out += String(pin);
    out += " 高电平";
    if (dur) {
      out += " (约 ";
      out += dur;
      out += " ms 后自动恢复低电平)";
    }
  } else if (action == "low" || action == "off") {
    pinApply(pin, PIN_LOW, 0, 0);
    out = "\n\n⚡ 已执行: ";
    out += name;
    out += String(pin);
    out += " 低电平";
  } else if (action == "blink") {
    pinApply(pin, PIN_BLINK, interval, until);
    out = "\n\n⚡ 已执行: ";
    out += name;
    out += String(pin);
    out += " 闪烁 (每 ";
    out += interval;
    out += " ms 翻转一次";
    if (dur) {
      out += ", 约 ";
      out += dur;
      out += " ms 后停止";
    }
    out += ")";
  } else if (action == "heartbeat") {
    if (pin != LED_PIN) {
      out = "\n\n⚠️ 心跳闪烁 (heartbeat) 只对 pin 2 (板载 LED) 可用";
      Serial.println(out);
      return out;
    }
    pinApply(pin, PIN_HEARTBEAT, 0, 0);
    out = "\n\n⚡ 已执行: 板载 LED 恢复 1Hz 心跳闪烁";
  } else {
    out = "\n\n⚠️ 未知动作: ";
    out += action;
    out += " (只支持 high / low / blink / off / heartbeat)";
    Serial.println(out);
    return out;
  }
  Serial.println(out);
  return out;
}

// POST /api/pin  pin=&action=high|low|blink|off|heartbeat&interval_ms=&duration_ms=
static void handlePinSet() {
  int pin = server.arg("pin").toInt();
  String action = server.arg("action");
  long interval = server.arg("interval_ms").toInt();
  long dur = server.arg("duration_ms").toInt();

  String json;
  json.reserve(96);
  json = "{\"pin\":";
  json += String(pin);
  json += ",\"action\":\"";
  json += action;
  json += "\"";
  if (interval > 0) {
    json += ",\"interval_ms\":";
    json += String(interval);
  }
  if (dur > 0) {
    json += ",\"duration_ms\":";
    json += String(dur);
  }
  json += "}";

  String res = execPinCmd(json);
  bool ok = res.startsWith("\n\n⚡");
  String out;
  out.reserve(res.length() + 48);
  out = ok ? "{\"ok\":true,\"result\":\"" : "{\"ok\":false,\"error\":\"";
  String body = res.substring(3);  // 去掉开头的 "\n\n"
  jsonEsc(body, out);
  out += "\"}";
  server.send(ok ? 200 : 400, "application/json", out);
}

// GET /api/pins -> {"pins":{"0":"idle","2":"heartbeat",...,"15":"blink:500"}}
static void handlePinsGet() {
  static const char *MODE_NAMES[5] = {"idle", "high", "low", "blink", "heartbeat"};
  String json;
  json.reserve(180);
  json = "{\"pins\":{";
  for (int p = 0; p < 17; p++) {
    if (p) json += ",";
    json += "\"";
    json += String(p);
    json += "\":\"";
    json += MODE_NAMES[g_pins[p].mode];
    if (g_pins[p].mode == PIN_BLINK) {
      json += ":";
      json += String(g_pins[p].intervalMs);
    }
    json += "\"";
  }
  json += "}}";
  server.send(200, "application/json", json);
}

// ---------------- 启动 ----------------
static void loadCfg() {
  EEPROM.begin(sizeof(CfgBlob));
  CfgBlob b;
  EEPROM.get(0, b);
  if (b.magic == CFG_MAGIC && b.version == 1) {
    cfgSsid = String(b.ssid);
    cfgPass = String(b.pass);
    cfgApiUrl = String(b.url);
    cfgApiKey = String(b.key);
    cfgApiModel = String(b.model);
  }
  // magic 不匹配(全新 flash)时保持上面的默认值, 不写回
}

static void cfgSave() {
  CfgBlob b;
  memset(&b, 0, sizeof(b));
  b.magic = CFG_MAGIC;
  b.version = 1;
  cfgSsid.toCharArray(b.ssid, sizeof(b.ssid));
  cfgPass.toCharArray(b.pass, sizeof(b.pass));
  cfgApiUrl.toCharArray(b.url, sizeof(b.url));
  cfgApiKey.toCharArray(b.key, sizeof(b.key));
  cfgApiModel.toCharArray(b.model, sizeof(b.model));
  EEPROM.put(0, b);
  EEPROM.commit();
  Serial.println("[..] 配置已写入 flash (EEPROM 区 0xFB000)");
}

static void tryStaBoot() {
  if (cfgSsid.length() == 0) {
    Serial.println("[..] 未配置 STA WiFi, 跳过自动连接 (请通过热点网页选择)");
    return;
  }
  Serial.printf("尝试连接 WiFi \"%s\" ", cfgSsid.c_str());
  WiFi.begin(cfgSsid.c_str(), cfgPass.length() ? cfgPass.c_str() : (const char *)NULL);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000UL) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[OK] WiFi 已连接");
    Serial.printf("[OK] IP 地址: http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("[OK] 也可尝试: http://%s.local/\n", MDNS_NAME);
    Serial.printf("[OK] RSSI=%d dBm  MAC=%s\n", (int)WiFi.RSSI(),
                  WiFi.macAddress().c_str());
  } else {
    Serial.printf("[ERR] \"%s\" 连接失败: status=%d %s (回退热点仍可用)\n",
                  cfgSsid.c_str(), (int)WiFi.status(), statusName((int)WiFi.status()));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  g_bootMs = millis();

  pinMode(LED_PIN, OUTPUT);
  ledWrite(false);
  pinApply(LED_PIN, PIN_HEARTBEAT, 0, 0);  // 默认 1Hz 心跳

  Serial.println();
  Serial.println("================================================");
  Serial.println(" ESP8266 Web Console  --- 热点 + STA + AI 对话");
  Serial.printf(" chip id=%08x  flash=%uKB  free heap=%u\n", ESP.getChipId(),
                ESP.getFlashChipRealSize() / 1024, ESP.getFreeHeap());
  Serial.println("================================================");

  loadCfg();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.hostname(MDNS_NAME);

  // 1) 上电先开热点 (始终可用, 兼做回退)
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[OK] 回退热点已启动: SSID \"%s\" 密码 %s (AP 地址 192.168.4.1)\n",
                AP_SSID, AP_PASS);

  // 2) 尝试连入已保存的 STA WiFi (15s 超时)
  tryStaBoot();

  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE_INDEX); });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/led", HTTP_GET, handleLed);
  server.on("/api/wifi/scan", HTTP_GET, handleScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/disconnect", HTTP_POST, handleWifiDisconnect);
  server.on("/api/chat", HTTP_POST, handleChat);
  server.on("/api/chat/config", HTTP_GET, handleChatConfigGet);
  server.on("/api/chat/config", HTTP_POST, handleChatConfigSet);
  server.on("/api/pin", HTTP_POST, handlePinSet);
  server.on("/api/pins", HTTP_GET, handlePinsGet);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[OK] HTTP 服务已启动 (端口 80)");
  Serial.println("[OK] 串口每 5 秒打印一次心跳, 用于确认固件在运行");
}

void loop() {
  server.handleClient();
  MDNS.update();
  pinTick();

  // 网页发起的 STA 连接: 20s 超时看门狗 (非阻塞)
  if (g_staConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      g_staConnecting = false;
      Serial.printf("[OK] WiFi 已连接 \"%s\"  IP 地址: http://%s/\n",
                    cfgSsid.c_str(), WiFi.localIP().toString().c_str());
    } else if (millis() - g_staT0 >= 20000UL) {
      g_staConnecting = false;
      Serial.printf("[ERR] 连接 \"%s\" 超时(20s): status=%d %s\n", cfgSsid.c_str(),
                    (int)WiFi.status(), statusName((int)WiFi.status()));
    }
  }

  if (millis() - g_lastBeat >= 5000UL) {
    g_lastBeat = millis();
    bool staOk = (WiFi.status() == WL_CONNECTED);
    String ip = (staOk ? WiFi.localIP() : WiFi.softAPIP()).toString();
    Serial.printf("[beat] uptime=%lus heap=%u rssi=%d ip=%s led=%u\n",
                  (unsigned long)((millis() - g_bootMs) / 1000UL),
                  (unsigned)ESP.getFreeHeap(), staOk ? (int)WiFi.RSSI() : -1,
                  ip.c_str(), (unsigned)g_pins[LED_PIN].mode);
  }
  delay(2);
}
