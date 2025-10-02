#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <time.h>
#include <sys/time.h>

// ================= Hardware configuration =================
static const uint8_t RELAY_PIN = 23;            // GPIO that drives the contactor
static const bool RELAY_ACTIVE_HIGH = true;     // set to false if the relay is active low

// Status LED and buttons that manage the configuration portal / factory reset.
// Adjust these pins to match the actual wiring of the board.
static const uint8_t STATUS_LED_PIN = 2;        // On many ESP32 devkits GPIO2 drives the built-in LED
static const bool STATUS_LED_ACTIVE_LOW = true; // Flip to false if your LED is wired active-high
static const uint8_t HOLD_BUTTON_PIN = 0;       // Long press (BOOT button on most devkits)
static const uint8_t CONFIRM_BUTTON_PIN = 4;    // Secondary button to confirm factory reset

// ================= Portal defaults =================
static const char *DEFAULT_AP_SSID = "SRC-Link";
static const char *DEFAULT_AP_PASS = "12345678";
static const uint16_t DNS_PORT = 53;

// ================= Globals =================
Preferences prefs;
DNSServer dnsServer;
WebServer server(80);

String savedSSID;
String savedPASS;
String apSsid;
String apPass;
String savedLang = "ru";
String adminPass = "1234";
String sessionToken;
int tzOffsetMin = 180;

bool relayState = false;
String relayUpdatedAt;
String relayUpdatedBy;

bool statusLedState = false;
bool factoryResetArmed = false;
unsigned long holdStartMs = 0;
unsigned long blinkTickerMs = 0;
unsigned long factoryArmDeadlineMs = 0;
bool lastHoldLevel = true;
bool lastConfirmLevel = true;

static const uint32_t FACTORY_HOLD_MS = 4000;      // hold duration to arm reset
static const uint32_t FACTORY_BLINK_MS = 250;      // blink cadence while armed
static const uint32_t FACTORY_CONFIRM_MS = 15000;  // window to confirm reset once armed

// ================= Helpers =================
static inline String trimCopy(String value) {
  value.trim();
  return value;
}

static inline String isoTimestamp() {
  struct tm now_tm;
  time_t now = time(nullptr);
  if (now < 1000) {
    unsigned long ms = millis();
    char buf[32];
    snprintf(buf, sizeof(buf), "1970-01-01T00:%02lu:%02luZ", (ms / 60000UL) % 60UL, (ms / 1000UL) % 60UL);
    return String(buf);
  }
  time_t local = now + tzOffsetMin * 60;
  gmtime_r(&local, &now_tm);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
           now_tm.tm_year + 1900,
           now_tm.tm_mon + 1,
           now_tm.tm_mday,
           now_tm.tm_hour,
           now_tm.tm_min,
           now_tm.tm_sec);
  return String(buf);
}

static inline void applyRelay(bool on) {
  relayState = on;
  bool level = RELAY_ACTIVE_HIGH ? on : !on;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
}

static inline void writeStatusLed(bool on) {
  bool level = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(STATUS_LED_PIN, level ? HIGH : LOW);
  statusLedState = on;
}

static inline void toggleStatusLed() {
  writeStatusLed(!statusLedState);
}

static void cancelFactoryArm() {
  factoryResetArmed = false;
  factoryArmDeadlineMs = 0;
  holdStartMs = 0;
  blinkTickerMs = 0;
  writeStatusLed(false);
}

static void performFactoryReset() {
  cancelFactoryArm();
  prefs.clear();
  savedSSID = "";
  savedPASS = "";
  savedLang = "ru";
  adminPass = "1234";
  apSsid = DEFAULT_AP_SSID;
  apPass = DEFAULT_AP_PASS;
  tzOffsetMin = 180;
  relayState = false;
  relayUpdatedBy = "";
  relayUpdatedAt = "";
  prefs.putString("admpass", adminPass);
  prefs.putString("apSsid", apSsid);
  prefs.putString("apPass", apPass);
  prefs.putString("lang", savedLang);
  prefs.putInt("tzMin", tzOffsetMin);
  prefs.putBool("relayOn", relayState);
  prefs.putString("relayBy", relayUpdatedBy);
  prefs.putString("relayAt", relayUpdatedAt);
  applyRelay(false);
  delay(50);
  ESP.restart();
}

static void persistRelayState(bool on, const String &actor, const String &timestamp) {
  applyRelay(on);
  prefs.putBool("relayOn", on);
  prefs.putString("relayBy", actor);
  prefs.putString("relayAt", timestamp);
  relayUpdatedBy = actor;
  relayUpdatedAt = timestamp;
}

static String currentStateJson() {
  String json = "{\"ok\":true";
  json += ",\"state\":\"";
  json += relayState ? "on" : "off";
  json += "\"";
  if (relayUpdatedAt.length()) {
    json += ",\"updated_at\":\"";
    json += relayUpdatedAt;
    json += "\"";
  }
  if (relayUpdatedBy.length()) {
    json += ",\"updated_by\":\"";
    json += relayUpdatedBy;
    json += "\"";
  }
  json += "}";
  return json;
}

static String parseJsonValue(const String &payload, const String &key) {
  String needle = String("\"") + key + "\":";
  int start = payload.indexOf(needle);
  if (start < 0) {
    return "";
  }
  start += needle.length();
  while (start < (int)payload.length() && (payload[start] == ' ' || payload[start] == '"')) {
    if (payload[start] == '"') {
      start++;
      int end = payload.indexOf('"', start);
      if (end < 0) {
        return "";
      }
      return payload.substring(start, end);
    }
    start++;
  }
  int end = start;
  while (end < (int)payload.length() && payload[end] != ',' && payload[end] != '}' && payload[end] != ' ') {
    end++;
  }
  return payload.substring(start, end);
}

static inline bool isAuthorized() {
  if (!sessionToken.length()) {
    return false;
  }
  if (!server.hasHeader("Cookie")) {
    return false;
  }
  String cookie = server.header("Cookie");
  return cookie.indexOf("SL_AUTH=" + sessionToken) >= 0;
}

static inline void sendJson(int code, const String &body) {
  server.send(code, "application/json", body);
}

static inline void setAuthCookie() {
  server.sendHeader("Set-Cookie", "SL_AUTH=" + sessionToken + "; Max-Age=86400; Path=/", true);
}

static void handleCaptiveRedirect() {
  IPAddress ip = WiFi.softAPIP();
  String target = String("http://") + ip.toString() + "/login";
  server.sendHeader("Location", target, true);
  server.send(302, "text/html", "<html><head><meta http-equiv='refresh' content='0; url=/login'></head><body>Redirecting…</body></html>");
}
// ================= Localization =================
static String baseCss() {
  return
      ":root{--bg:#050b10;--panel:#0c1722;--fg:#e6edf6;--mut:#8f9bae;--line:#1d9bf0;--line-soft:rgba(29,155,240,.35);--accent:#1d9bf0}" \
      "*{box-sizing:border-box}html,body{margin:0;padding:0;background:var(--bg);color:var(--fg);" \
      "font-family:'Inter',-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,'Noto Sans',Ubuntu,Cantarell,'Helvetica Neue',Arial,sans-serif;" \
      "-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale}body{min-height:100vh}" \
      ".wrap{max-width:1120px;margin:0 auto;padding:18px 20px}" \
      "header{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap;margin-bottom:18px}" \
      "h1{margin:0;font-size:18px;font-weight:800;letter-spacing:.18em;text-transform:uppercase;color:#f3f7fb}" \
      ".kv{font-size:12px;color:var(--mut)}" \
      ".grid{display:grid;gap:16px;grid-template-columns:1fr}" \
      "@media(min-width:780px){.grid{grid-template-columns:1fr 1fr}}" \
      "@media(min-width:1080px){.grid{grid-template-columns:2fr 1fr}}" \
      ".card{position:relative;background:var(--panel);padding:18px;border:1px solid var(--line-soft);box-shadow:0 18px 40px rgba(0,0,0,.35)}" \
      ".titleBar{position:absolute;left:16px;top:-14px;padding:.35em 1.2em;border:1px solid var(--line);background:#07111b;color:#dce9f7;font-weight:700;letter-spacing:.24em;text-transform:uppercase;font-size:11px}" \
      "label{display:block;font-size:11px;color:#9fb0c7;margin:8px 0 4px;text-transform:uppercase;letter-spacing:.28em}" \
      "input,select,button{appearance:none;border:1px solid var(--line-soft);background:#09131d;color:var(--fg);padding:8px 12px;font-size:14px;width:100%;outline:none;border-radius:4px}" \
      "input::placeholder{color:#5f768f}" \
      "input:focus-visible,select:focus-visible{border-color:var(--line);box-shadow:0 0 0 2px rgba(29,155,240,.25)}" \
      "input:disabled,select:disabled,button:disabled{opacity:.55;cursor:not-allowed}" \
      ".btn{width:auto;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;gap:8px;text-transform:uppercase;letter-spacing:.28em;font-weight:700;" \
      "background:#0c1d2a;color:#dce9f7;border:1px solid var(--line-soft);padding:9px 16px;font-size:12px;text-decoration:none;border-radius:4px;transition:background .15s ease,border-color .15s ease}" \
      ".btn:hover{border-color:var(--line);background:#102436}" \
      ".btn-prim{background:#112c3f;color:#f1f7fd;border-color:var(--line)}" \
      ".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}" \
      "#langSeg{display:flex;gap:6px;border:1px solid var(--line-soft);border-radius:4px;padding:2px;background:#0b1824}" \
      ".list{border:1px solid var(--line-soft);max-height:320px;overflow:auto;margin-top:10px;background:#08121c;border-radius:4px}" \
      ".item{display:flex;gap:10px;justify-content:space-between;align-items:center;padding:10px 14px;border-bottom:1px solid rgba(29,155,240,.12)}" \
      ".item:last-child{border-bottom:none}" \
      ".item:hover{background:#0f1d29}" \
      ".item.selected{border:1px solid var(--line);background:#12293d}" \
      ".item .left{display:flex;gap:10px;align-items:center;min-width:0}" \
      ".lock{width:13px;height:13px;display:inline-block;color:#49a9ff}" \
      ".ssid{font-weight:700;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#eaf2fb;font-size:14px}" \
      ".meta{display:flex;gap:8px;align-items:center;font-size:12px;color:#7f93ab}" \
      ".centerWrap{min-height:100vh;display:grid;place-items:center;padding:32px}" \
      ".cardCenter{max-width:420px;width:100%;padding:28px 24px;background:var(--panel);border:1px solid var(--line-soft);box-shadow:0 24px 44px rgba(0,0,0,.45);position:relative}" \
      ".togglePanel{display:flex;flex-direction:column;gap:22px;padding-top:28px}" \
      ".powerState{font-size:22px;font-weight:700;letter-spacing:.08em;text-transform:uppercase}" \
      ".toggleWrap{display:flex;align-items:center;gap:16px;flex-wrap:wrap}" \
      ".toggleSquare{position:relative;width:160px;height:48px;padding:0;border:1px solid var(--line-soft);background:#081520;color:var(--mut);text-transform:uppercase;letter-spacing:.2em;font-weight:700;font-size:11px;display:flex;align-items:center;justify-content:center;border-radius:6px;transition:border-color .18s ease,color .18s ease}" \
      ".toggleSquare .toggleLabel{z-index:1;font-size:11px;letter-spacing:.2em;text-transform:uppercase}" \
      ".toggleSquare .toggleKnob{position:absolute;top:5px;left:5px;width:calc(50% - 10px);height:calc(100% - 10px);border-radius:4px;border:1px solid var(--line);background:rgba(29,155,240,.18);transform:translateX(0);transition:transform .18s ease,background .18s ease}" \
      ".toggleSquare.is-on{color:#f1f7fd;border-color:var(--line)}" \
      ".toggleSquare:disabled{opacity:.65;cursor:wait}" \
      ".toggleSquare.is-on .toggleKnob{transform:translateX(100%);background:rgba(29,155,240,.6)}" \
      ".statusNote{font-size:13px;color:#7f93ab;line-height:1.5}" \
      "footer{margin-top:40px;font-size:11px;color:#5f728a;text-align:center;letter-spacing:.24em;text-transform:uppercase}";
}


static String i18nHead(const String &pageKey) {
  String s = "<script>var __PAGE='" + pageKey + "',__DEF_LANG='" + savedLang + "';</script>";
  s += R"JS(
<script>
var I={
 ru:{
  title_home:"SRKLink • Главная",title_settings:"SRKLink • Настройки",title_login:"SRKLink • Вход",
  home_btn:"Главная",settings_btn:"Настройки",logout:"Выйти",
  wifi:"Wi-Fi",scan:"Сканировать",none:"Сети не найдены",ssid:"SSID",pass:"Пароль",save:"Сохранить и подключиться",
  disconnect:"Отключиться",forget:"Забыть Wi-Fi",connectedNow:"Подключено к",needDisconnect:"Чтобы подключиться к другой сети, сначала отключитесь.",
  object_panel:"Пульт объекта",object_on:"Свет включен",object_off:"Свет выключен",
  object_turn_on:"Включить",object_turn_off:"Выключить",object_updated:"Обновлено",
  history_none:"История отсутствует",
  login:"Вход",enter:"Войти",wrong:"Неверный пароль",login_pass_ph:"Пароль для входа",
  security:"Безопасность",newPass:"Новый пароль входа (1–8)",changePass:"Сменить пароль",
  apCreds:"Портал (AP)",apSsid:"AP SSID",apPass:"AP Пароль (8–63)",
  saved:"Сохранено",fail:"Ошибка",
  timeScreen:"Время",ntp:"Синхр. NTP",fromPhone:"С телефона",tz:"Часовой пояс (мин)",customTime:"Своё время",setCustom:"Установить",
  status_block:"Текущее состояние",
  actor_bot:"Телеграм",
  object_refresh:"Обновить",
  forget_hint:"Для подключения к другой сети сначала отключитесь."
 },
 uk:{
  title_home:"SRKLink • Головна",title_settings:"SRKLink • Налаштування",title_login:"SRKLink • Вхід",
  home_btn:"Головна",settings_btn:"Налаштування",logout:"Вийти",
  wifi:"Wi-Fi",scan:"Сканувати",none:"Мережі не знайдені",ssid:"SSID",pass:"Пароль",save:"Зберегти та підключитись",
  disconnect:"Відʼєднатись",forget:"Забути Wi-Fi",connectedNow:"Підключено до",needDisconnect:"Щоб підключитись до іншої мережі, спершу відʼєднайтесь.",
  object_panel:"Пульт об'єкта",object_on:"Світло увімкнено",object_off:"Світло вимкнено",
  object_turn_on:"Увімкнути",object_turn_off:"Вимкнути",object_updated:"Оновлено",
  history_none:"Історія відсутня",
  login:"Вхід",enter:"Увійти",wrong:"Невірний пароль",login_pass_ph:"Пароль для входу",
  security:"Безпека",newPass:"Новий пароль входу (1–8)",changePass:"Змінити пароль",
  apCreds:"Портал (AP)",apSsid:"AP SSID",apPass:"AP Пароль (8–63)",
  saved:"Збережено",fail:"Помилка",
  timeScreen:"Час",ntp:"Синхр. NTP",fromPhone:"З телефону",tz:"Часовий пояс (хв)",customTime:"Власний час",setCustom:"Встановити",
  status_block:"Поточний стан",
  actor_bot:"Telegram",
  object_refresh:"Оновити",
  forget_hint:"Щоб підключитись до іншої мережі, спершу відʼєднайтесь."
 },
 en:{
  title_home:"SRKLink • Home",title_settings:"SRKLink • Settings",title_login:"SRKLink • Login",
  home_btn:"Home",settings_btn:"Settings",logout:"Logout",
  wifi:"Wi-Fi",scan:"Scan",none:"No networks found",ssid:"SSID",pass:"Password",save:"Save & Connect",
  disconnect:"Disconnect",forget:"Forget Wi-Fi",connectedNow:"Connected to",needDisconnect:"To join another network, please disconnect first.",
  object_panel:"Object control",object_on:"Light is ON",object_off:"Light is OFF",
  object_turn_on:"Turn on",object_turn_off:"Turn off",object_updated:"Updated",
  history_none:"No history yet",
  login:"Login",enter:"Enter",wrong:"Wrong password",login_pass_ph:"Login password",
  security:"Security",newPass:"New login password (1–8)",changePass:"Change password",
  apCreds:"Portal (AP)",apSsid:"AP SSID",apPass:"AP Password (8–63)",
  saved:"Saved",fail:"Error",
  timeScreen:"Time",ntp:"Sync NTP",fromPhone:"Set from phone",tz:"Time zone (min)",customTime:"Custom time",setCustom:"Set",
  status_block:"Current status",
  actor_bot:"Telegram",
  object_refresh:"Refresh",
  forget_hint:"Disconnect first to join another network."
 }
};
function __setTitle(L){
  var t=I[L]||I.ru, map={home:t.title_home,settings:t.title_settings,login:t.title_login};
  document.title = map[__PAGE] || "SRKLink";
  var ttl=document.getElementById('ttl'); if(ttl) ttl.textContent = map[__PAGE] || "SRKLink";
}
function __applyLang(L){
  var t=I[L]||I.ru; document.documentElement.setAttribute('lang',L);
  document.querySelectorAll('[data-i18n]').forEach(el=>{ var k=el.getAttribute('data-i18n'); if(t[k]) el.textContent=t[k]; });
  document.querySelectorAll('[data-i18n-ph]').forEach(el=>{ var k=el.getAttribute('data-i18n-ph'); if(t[k]) el.setAttribute('placeholder',t[k]); });
  __setTitle(L); try{ localStorage.setItem('srk_lang',L);}catch(e){}
}
(function preLang(){ var L="ru"; try{L=localStorage.getItem('srk_lang')||L;}catch(e){} window.__LANG=L; document.documentElement.setAttribute('lang',L); })();
document.addEventListener('DOMContentLoaded',function(){ __applyLang(window.__LANG||"ru");
  var seg=document.getElementById('langSeg'); if(seg){ var cur=window.__LANG||"ru";
    seg.querySelectorAll('[data-lang-btn]').forEach(btn=>{
      if(btn.getAttribute('data-lang-btn')===cur) btn.classList.add('active');
      btn.addEventListener('click', function(){ var L=this.getAttribute('data-lang-btn');
        seg.querySelectorAll('[data-lang-btn]').forEach(b=>b.classList.toggle('active', b===this));
        __applyLang(L); fetch('/lang',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'code='+encodeURIComponent(L)}).catch(()=>{}); });
    }); }
});
</script>
)JS";
  return s;
}

// ================= HTML helpers =================
static String htmlHeader(const String &title, const String &pageKey) {
  String s = "<!doctype html><html lang='" + savedLang + "'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>";
  s += "<title>" + title + "</title><style>" + baseCss() + "</style>" + i18nHead(pageKey) + "</head><body>";
  s += "<div class='wrap'><header>";
  s += "<h1 id='ttl' data-i18n='title_" + pageKey + "'>" + title + "</h1>";
  s += "<div class='row'>";
  s += "<div class='row' id='langSeg'>";
  s += "<button type='button' data-lang-btn='ru' class='btn'>РУС</button>";
  s += "<button type='button' data-lang-btn='uk' class='btn'>УКР</button>";
  s += "<button type='button' data-lang-btn='en' class='btn'>EN</button>";
  s += "</div>";
  s += "<a href='/' class='btn'><span data-i18n='home_btn'>Home</span></a>";
  s += "<a href='/settings' class='btn btn-prim'><span data-i18n='settings_btn'>Settings</span></a>";
  s += "<form method='POST' action='/logout'><button class='btn' type='submit'><span data-i18n='logout'>Logout</span></button></form>";
  s += "</div></header>";
  return s;
}

static String htmlHeaderLogin(const String &title) {
  String s = "<!doctype html><html lang='" + savedLang + "'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>" + title + "</title><style>" + baseCss() + "</style>" + i18nHead("login") + "</head><body>";
  s += "<main class='centerWrap'><div class='card cardCenter'>";
  s += "<div class='titleBar' id='ttl' data-i18n='title_login'>" + title + "</div>";
  s += "<div class='row' id='langSeg'>";
  s += "<button type='button' data-lang-btn='ru' class='btn'>РУС</button>";
  s += "<button type='button' data-lang-btn='uk' class='btn'>УКР</button>";
  s += "<button type='button' data-lang-btn='en' class='btn'>EN</button>";
  s += "</div>";
  s += "</div></main>";
  return s;
}

static String htmlFooter() {
  return "<footer>SRKLink Control</footer></div></body></html>";
}
static void pageLogin(bool wrong) {
  String s = htmlHeaderLogin("SRKLink • Login");
  s += "<script>document.body.style.opacity='1'</script>";
  s += "<script>(function(){var c=document.querySelector('.cardCenter');var f=document.createElement('div');";
  s += "f.innerHTML=\"<form method='POST' action='/login'>";
  s += "<label data-i18n='login'>Login</label>";
  s += "<input name='pass' type='password' autofocus data-i18n-ph='login_pass_ph' placeholder='Login password'>";
  s += "<div class='row' style='margin-top:8px'><button class='btn btn-prim' type='submit'><span data-i18n='enter'>Enter</span></button></div>";
  if (wrong) {
    s += "<div class='kv' style='color:#f55;margin-top:8px' data-i18n='wrong'>Wrong password</div>";
  }
  s += "</form>\"; c.appendChild(f);})();</script>";
  s += htmlFooter();
  server.send(200, "text/html; charset=utf-8", s);
}

static void pageHome() {
  if (!isAuthorized()) {
    pageLogin(false);
    return;
  }
  String s = htmlHeader("SRKLink • Home", "home");
  s += "<div class='grid'>";
  s += "<div class='card togglePanel'><div class='titleBar' data-i18n='object_panel'>Пульт объекта</div>";
  s += "<div class='powerState' id='powerState'>—</div>";
  s += "<div class='statusNote' id='powerMeta'>—</div>";
  s += "<div class='toggleWrap'><button class='toggleSquare' id='toggleBtn'><span class='toggleLabel' id='toggleLabel'>—</span><span class='toggleKnob'></span></button>";
  s += "<button class='btn' id='refreshBtn' data-i18n='refresh'>Refresh</button></div>";
  s += "</div>";
  s += "<div class='card'><div class='titleBar' data-i18n='status_block'>Статус</div>";
  s += "<div class='kv' id='powerSummary'>—</div>";
  s += "</div></div>";

  s += R"HTML(
<script>
const $=id=>document.getElementById(id);
let currentState='off';
const powerState=$('powerState');
const powerMeta=$('powerMeta');
const powerSummary=$('powerSummary');
const toggleBtn=$('toggleBtn');
const toggleLabel=$('toggleLabel');
const refreshBtn=$('refreshBtn');
function text(key){var L=document.documentElement.lang||'ru';return (I[L]&&I[L][key])||(I['ru'][key])||key;}
function actorName(d){if(!d) return '—'; if(d.updated_by_name) return d.updated_by_name; if(d.updated_by) return d.updated_by; return '—';}
function formatMeta(d){if(!d||!d.updated_at){return '—';} var actor=actorName(d); return text('object_updated')+': '+d.updated_at+(actor&&actor!=='—' ? ' • '+actor : '');}
function formatSummary(d){var actor=actorName(d); var time=(d&&d.updated_at)?d.updated_at:'—'; var stateLabel=currentState==='on'?text('object_on'):text('object_off'); return stateLabel+' • '+actor+' • '+time;}
function applyState(d){d=d||{}; currentState=(d.state==='on')?'on':'off'; var isOn=currentState==='on'; powerState.textContent=isOn?text('object_on'):text('object_off'); toggleLabel.textContent=isOn?text('object_turn_off'):text('object_turn_on'); toggleBtn.classList.toggle('is-on',isOn); powerMeta.textContent=formatMeta(d); if(powerSummary){ powerSummary.textContent=formatSummary(d);} }
async function loadState(){ try{ const r=await fetch('/api/state'); if(!r.ok) throw new Error('HTTP '+r.status); const data=await r.json(); applyState(data); }catch(e){ powerMeta.textContent=text('fail')+': '+e.message; if(powerSummary){ powerSummary.textContent='—'; } } }
async function toggle(){ const desired=currentState==='on'?'off':'on'; toggleBtn.disabled=true; try{ const r=await fetch('/api/state',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({state:desired,actor:'Web UI'})}); if(!r.ok) throw new Error('HTTP '+r.status); const data=await r.json(); applyState(data); }catch(e){ powerMeta.textContent=text('fail')+': '+e.message; } toggleBtn.disabled=false; }
refreshBtn.addEventListener('click',loadState);
toggleBtn.addEventListener('click',toggle);
loadState();
</script>
)HTML";
  s += htmlFooter();
  server.send(200, "text/html; charset=utf-8", s);
}

static void pageSettings() {
  if (!isAuthorized()) {
    pageLogin(false);
    return;
  }
  String s = htmlHeader("SRKLink • Settings", "settings");
  s += "<div class='grid'>";
  s += "<div class='card'><div class='titleBar' data-i18n='wifi'>Wi-Fi</div>";
  s += "<div id='connRow' class='kv' style='margin-top:18px'>—</div>";
  s += "<div class='row'><button class='btn btn-prim' id='scanBtn'><span data-i18n='scan'>Scan</span></button><span id='scanStatus' class='kv'></span></div>";
  s += "<div id='list' class='list'></div>";
  s += "<label data-i18n='ssid'>SSID</label><input id='ssid' placeholder='SSID'>";
  s += "<label data-i18n='pass'>Пароль</label><input id='pass' type='password' placeholder='Wi-Fi password'>";
  s += "<div class='row' style='margin-top:8px'><button class='btn btn-prim' id='saveBtn'><span data-i18n='save'>Save & Connect</span></button>";
  s += "<button class='btn' id='disconnectBtn'><span data-i18n='disconnect'>Disconnect</span></button>";
  s += "<button class='btn' id='forgetBtn'><span data-i18n='forget'>Forget Wi-Fi</span></button></div>";
  s += "<div class='kv' id='reachHint' style='margin-top:6px'>—</div>";
  s += "</div>";
  s += "<div class='card'><div class='titleBar' data-i18n='timeScreen'>Time</div>";
  s += "<div class='row' style='margin-top:18px'><button class='btn' id='ntpBtn'><span data-i18n='ntp'>Sync NTP</span></button>";
  s += "<button class='btn' id='fromBrowserBtn'><span data-i18n='fromPhone'>Set from phone</span></button></div>";
  s += "<label data-i18n='tz'>Time zone (min)</label><input id='tzMin' type='number' step='1' placeholder='180' style='max-width:160px'>";
  s += "<label data-i18n='customTime'>Custom time</label><input id='customDt' type='datetime-local'>";
  s += "<div class='row' style='margin-top:8px'><button class='btn btn-prim' id='setCustomBtn'><span data-i18n='setCustom'>Set</span></button></div>";
  s += "<div id='nowLbl' class='kv' style='margin-top:8px'>—</div>";
  s += "</div>";
  s += "<div class='card'><div class='titleBar' data-i18n='security'>Security</div>";
  s += "<label data-i18n='newPass'>New login password (1–8)</label><input id='newPass' type='password' maxlength='8' placeholder='Login password'>";
  s += "<div class='row' style='margin-top:8px'><button class='btn btn-prim' id='changePassBtn'><span data-i18n='changePass'>Change password</span></button></div>";
  s += "<label data-i18n='apSsid'>AP SSID</label><input id='apSsid' maxlength='31' placeholder='SRC-Link'>";
  s += "<label data-i18n='apPass'>AP Password (8–63)</label><input id='apPass' type='password' maxlength='63' placeholder='********'>";
  s += "<div class='row' style='margin-top:8px'><button class='btn' id='setAPBtn'>Save AP</button></div>";
  s += "<div id='statusBox' class='kv' style='white-space:pre-wrap;margin-top:8px'>—</div>";
  s += "</div></div>";
  s += R"HTML(
<script>
const $=id=>document.getElementById(id);
const list=$('list'), scanBtn=$('scanBtn'), scanStatus=$('scanStatus');
const ssid=$('ssid'), pass=$('pass');
const saveBtn=$('saveBtn'), disconnectBtn=$('disconnectBtn'), connRow=$('connRow'), reachHint=$('reachHint');
const LOCK_SVG="<svg class='lock' viewBox='0 0 24 24' aria-hidden='true'><path fill='currentColor' d='M7 11V8a5 5 0 0 1 10 0v3h1a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2v-7a2 2 0 0 1 2-2h1Zm2 0h6V8a3 3 0 0 0-6 0v3Z'/></svg>";
let scanTimer=null;
async function post(u,b){ const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}); if(!r.ok) throw new Error('HTTP '+r.status); return r.text(); }
function bars(r){ return (r>=-55)?3:(r>=-70)?2:(r>-90)?1:0; }
function renderScan(d){
  list.innerHTML='';
  if(!(d.networks&&d.networks.length)){
    list.innerHTML='<div class="item" data-i18n="none">No networks found</div>';
    return;
  }
  d.networks.forEach(ap=>{
    const enc=ap.enc||'Unknown';
    const row=document.createElement('div'); row.className='item';
    row.innerHTML=`<div class="left">${(enc!=='Open')?LOCK_SVG:''}<div class="ssid">${ap.ssid||'&lt;hidden&gt;'}</div></div><div class="meta">${enc} · RSSI: ${ap.rssi} dBm · ${bars(ap.rssi)}/3</div>`;
    row.addEventListener('click',()=>{ if(ap.ssid){ ssid.value=ap.ssid; document.querySelectorAll('.item').forEach(x=>x.classList.remove('selected')); row.classList.add('selected'); } });
    list.appendChild(row);
  });
  scanStatus.textContent='Found: '+(d.count||0);
}
async function scanStart(){ scanStatus.textContent='…'; await fetch('/scan?start=1'); if(scanTimer) clearInterval(scanTimer);
  scanTimer=setInterval(async()=>{ const r=await fetch('/scan'); const d=await r.json(); if(d.running) return; clearInterval(scanTimer); scanTimer=null; renderScan(d); }, 400);
}
async function refresh(){
  const r=await fetch('/api/status'); const d=await r.json();
  $('statusBox').textContent=`Mode: ${d.mode}\nSSID: ${d.ssid}\nAP SSID: ${d.apSsid}\nAP IP: ${d.apIp}\nSTA IP: ${d.staIp}\nRSSI: ${d.rssi}\nLang: ${d.lang}\nTZ(min): ${d.tzMin}`;
  $('tzMin').value=(d.tzMin??180);
  $('apSsid').value=d.apSsid||'';
  $('apPass').value=d.apPass||'';
  $('nowLbl').textContent=d.epoch?new Date((d.epoch+(d.tzMin||0)*60)*1000).toLocaleString():'—';
  if(d.mode==='sta'){
    connRow.innerHTML=`<b data-i18n="connectedNow">Connected to</b>: ${d.ssid}. <span data-i18n="needDisconnect">To join another network, please disconnect first.</span>`;
    saveBtn.disabled=true; ssid.disabled=true; pass.disabled=true; disconnectBtn.disabled=false;
    reachHint.innerHTML=`STA IP: <b>${d.staIp||'-'}</b>`;
  } else {
    connRow.textContent=''; saveBtn.disabled=false; ssid.disabled=false; pass.disabled=false; disconnectBtn.disabled=true;
    reachHint.innerHTML=`Portal: <b>http://${d.apIp||'192.168.4.1'}/</b>`;
  }
}
$('saveBtn').onclick=async()=>{ await post('/save','ssid='+encodeURIComponent(ssid.value)+'&pass='+encodeURIComponent(pass.value||'')); setTimeout(refresh,600); };
$('disconnectBtn').onclick=async()=>{ await post('/disconnect',''); setTimeout(refresh,600); };
$('forgetBtn').onclick=async()=>{ await post('/forget',''); alert('Cleared'); setTimeout(refresh,400); };
$('ntpBtn').onclick=async()=>{ await post('/setTime','ntp=1'); setTimeout(refresh,700); };
$('fromBrowserBtn').onclick=async()=>{ const e=Math.floor(Date.now()/1000); await post('/setTime','epoch='+e); setTimeout(refresh,300); };
$('setCustomBtn').onclick=async()=>{ const v=$('customDt').value; if(v){ const epoch=Math.floor(new Date(v).getTime()/1000); await post('/setTime','epoch='+epoch); setTimeout(refresh,400);} };
$('tzMin').addEventListener('change', async()=>{ const off=$('tzMin').value||0; await post('/timezone','minutes='+off); setTimeout(refresh,300); });
$('changePassBtn').onclick=async()=>{ await post('/changePass','newpass='+encodeURIComponent(($('newPass').value)||'')); alert('OK'); $('newPass').value=''; };
$('setAPBtn').onclick=async()=>{ await post('/apcreds','ssid='+encodeURIComponent(($('apSsid').value)||'')+'&pass='+encodeURIComponent(($('apPass').value)||'')); alert('AP updated'); setTimeout(refresh,400); };
scanBtn.onclick=scanStart;
refresh();
</script>
)HTML";
  s += htmlFooter();
  server.send(200, "text/html; charset=utf-8", s);
}
// ================= Auth =================
static String genToken() {
  char buf[33];
  for (int i = 0; i < 16; ++i) {
    uint8_t x = (uint8_t)(esp_random() & 0xFF);
    sprintf(buf + 2 * i, "%02x", x);
  }
  buf[32] = 0;
  return String(buf);
}

static void handleLogin() {
  String p = trimCopy(server.hasArg("pass") ? server.arg("pass") : "");
  String adm = trimCopy(adminPass);
  if (p == adm) {
    sessionToken = genToken();
    setAuthCookie();
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Location", "/", true);
    server.send(303, "text/plain", "OK");
  } else {
    pageLogin(true);
  }
}

static void handleLogout() {
  sessionToken = genToken();
  server.sendHeader("Set-Cookie", "SL_AUTH=; Max-Age=0; Path=/", true);
  server.sendHeader("Location", "/login", true);
  server.send(303, "text/plain", "bye");
}

static void handleChangePass() {
  if (!isAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  String np = trimCopy(server.hasArg("newpass") ? server.arg("newpass") : "");
  if (np.length() < 1 || np.length() > 8) {
    server.send(400, "text/plain; charset=utf-8", "length 1..8");
    return;
  }
  adminPass = np;
  prefs.putString("admpass", adminPass);
  server.send(200, "text/plain; charset=utf-8", "OK");
}

static void handleLang() {
  String l = trimCopy(server.hasArg("code") ? server.arg("code") : "");
  l.toLowerCase();
  if (l != "ru" && l != "uk" && l != "en") {
    server.send(400, "text/plain", "bad lang");
    return;
  }
  savedLang = l;
  prefs.putString("lang", savedLang);
  server.send(200, "application/json", String("{\"lang\":\"") + savedLang + "\"}");
}

// ================= Status & time =================
static void handleGetStatus() {
  time_t now = time(nullptr);
  IPAddress apIp = WiFi.softAPIP();
  IPAddress staIp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
  String json = "{";
  json += "\"epoch\":" + String((now > 1000) ? now : 0) + ",";
  json += "\"tzMin\":" + String(tzOffsetMin) + ",";
  json += "\"mode\":\"" + String((WiFi.status() == WL_CONNECTED) ? "sta" : "ap") + "\",";
  json += "\"ssid\":\"" + String((WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "") + "\",";
  json += "\"apIp\":\"" + apIp.toString() + "\",";
  json += "\"staIp\":\"" + staIp.toString() + "\",";
  json += "\"rssi\":" + String((WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0) + ",";
  json += "\"lang\":\"" + savedLang + "\",";
  json += "\"apSsid\":\"" + apSsid + "\",";
  json += "\"apPass\":\"" + apPass + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

static void handleSetTime() {
  if (!isAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  bool ok = false;
  if (server.hasArg("ntp") && server.arg("ntp") == "1") {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    ok = true;
  }
  if (server.hasArg("epoch")) {
    time_t e = (time_t)server.arg("epoch").toInt();
    if (e > 0) {
      struct timeval tv = {.tv_sec = e, .tv_usec = 0};
      settimeofday(&tv, nullptr);
      ok = true;
    }
  }
  server.send(200, "text/plain; charset=utf-8", ok ? "OK" : "NOOP");
}

static void handleTimezone() {
  if (!isAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  if (!server.hasArg("minutes")) {
    server.send(400, "text/plain", "minutes required");
    return;
  }
  tzOffsetMin = server.arg("minutes").toInt();
  prefs.putInt("tzMin", tzOffsetMin);
  server.send(200, "text/plain", "OK");
}

// ================= Wi-Fi helpers =================
static String encToStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
#ifdef WIFI_AUTH_WPA3_PSK
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
#endif
    default: return "Unknown";
  }
}

static void ensureDns() {
  dnsServer.stop();
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
}

static void handleScan() {
  if (!isAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  if (server.hasArg("start")) {
    WiFi.mode(WIFI_AP_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    server.send(200, "application/json", "{\"running\":true}");
    return;
  }
  if (n < 0) {
    server.send(200, "application/json", "{\"count\":0,\"networks\":[]}");
    return;
  }
  String json = "{\"count\":" + String(n) + ",\"networks\":[";
  for (int i = 0; i < n; ++i) {
    if (i) json += ",";
    String ss = WiFi.SSID(i);
    ss.replace("\\", "\\\\");
    ss.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ss + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"enc\":\"" + encToStr((wifi_auth_mode_t)WiFi.encryptionType(i)) + "\"}";
  }
  json += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

static bool tryConnect(const String &ssid, const String &pass, uint32_t timeoutMs) {
  if (!ssid.length()) {
    return false;
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.setAutoReconnect(true);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void handleSave() {
  if (!isAuthorized()) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    server.send(409, "text/plain; charset=utf-8", "disconnect first");
    return;
  }
  String ssid = trimCopy(server.arg("ssid"));
  String pass = trimCopy(server.arg("pass"));
  if (!ssid.length()) {
    server.send(400, "text/plain; charset=utf-8", "SSID empty");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  savedSSID = ssid;
  savedPASS = pass;
  server.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'><pre>Saved. Trying to connect...</pre>");
  delay(200);
  if (tryConnect(ssid, pass, 12000)) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSsid.c_str(), apPass.c_str());
    ensureDns();
  } else {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSsid.c_str(), apPass.c_str());
    ensureDns();
  }
}

static void handleDisconnect() {
  if (!isAuthorized()) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  WiFi.setAutoReconnect(false);
  esp_wifi_disconnect();
  server.send(200, "text/plain", "OK");
}

static void handleForget() {
  if (!isAuthorized()) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  prefs.remove("ssid");
  prefs.remove("pass");
  savedSSID = "";
  savedPASS = "";
  WiFi.setAutoReconnect(false);
  esp_wifi_disconnect();
  server.send(200, "text/plain; charset=utf-8", "Cleared");
}

static void handleChangeAPCreds() {
  if (!isAuthorized()) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  String ns = trimCopy(server.hasArg("ssid") ? server.arg("ssid") : "");
  String np = trimCopy(server.hasArg("pass") ? server.arg("pass") : "");
  if (!ns.length()) {
    server.send(400, "text/plain; charset=utf-8", "AP SSID empty");
    return;
  }
  if (np.length() && (np.length() < 8 || np.length() > 63)) {
    server.send(400, "text/plain; charset=utf-8", "AP PASS 8..63 or empty");
    return;
  }
  apSsid = ns;
  if (np.length() >= 8) {
    apPass = np;
  }
  prefs.putString("apSsid", apSsid);
  prefs.putString("apPass", apPass);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPdisconnect(true);
  delay(120);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  ensureDns();
  server.send(200, "text/plain; charset=utf-8", "OK");
}
// ================= Object control API =================
static void handleApiStateGet() {
  sendJson(200, currentStateJson());
}

static void handleApiStatePost() {
  String body = server.arg("plain");
  if (!body.length()) {
    sendJson(400, "{\"ok\":false,\"error\":\"empty_body\"}");
    return;
  }
  String stateValue = parseJsonValue(body, "state");
  stateValue.toLowerCase();
  bool requested = (stateValue == "on" || stateValue == "1" || stateValue == "true");
  String actor = parseJsonValue(body, "actor");
  if (!actor.length()) {
    actor = "API";
  }
  String when = parseJsonValue(body, "timestamp");
  if (!when.length()) {
    when = isoTimestamp();
  }
  persistRelayState(requested, actor, when);
  sendJson(200, currentStateJson());
}

// ================= Routing =================
static void handleNotFound() {
  if (!isAuthorized()) {
    pageLogin(false);
    return;
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

static void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  ensureDns();
  const char *headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);

  server.on("/login", HTTP_GET, []() { pageLogin(false); });
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_POST, handleLogout);

  server.on("/generate_204", HTTP_ANY, handleCaptiveRedirect);
  server.on("/gen_204", HTTP_ANY, handleCaptiveRedirect);
  server.on("/fwlink", HTTP_ANY, handleCaptiveRedirect);
  server.on("/connecttest.txt", HTTP_ANY, handleCaptiveRedirect);
  server.on("/ncsi.txt", HTTP_ANY, handleCaptiveRedirect);
  server.on("/hotspot-detect.html", HTTP_ANY, []() {
    server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='0; url=/login'></head><body>Redirecting…</body></html>");
  });

  server.on("/", []() { pageHome(); });
  server.on("/settings", []() { pageSettings(); });

  server.on("/api/state", HTTP_GET, handleApiStateGet);
  server.on("/api/state", HTTP_POST, handleApiStatePost);
  server.on("/api/status", HTTP_GET, handleGetStatus);

  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/disconnect", HTTP_POST, handleDisconnect);
  server.on("/forget", HTTP_POST, handleForget);

  server.on("/setTime", HTTP_POST, handleSetTime);
  server.on("/timezone", HTTP_POST, handleTimezone);

  server.on("/changePass", HTTP_POST, handleChangePass);
  server.on("/apcreds", HTTP_POST, handleChangeAPCreds);
  server.on("/lang", HTTP_POST, handleLang);
  server.on("/api/reboot", HTTP_POST, []() {
    if (!isAuthorized()) {
      server.send(401, "text/plain", "unauthorized");
      return;
    }
    server.send(200, "text/plain", "Rebooting");
    delay(150);
    ESP.restart();
  });

  server.onNotFound(handleNotFound);
  server.begin();
  if (MDNS.begin("srklink")) {
    MDNS.addService("http", "tcp", 80);
  }
}

// ================= Setup & loop =================
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  applyRelay(false);

  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);
  pinMode(HOLD_BUTTON_PIN, INPUT_PULLUP);
  pinMode(CONFIRM_BUTTON_PIN, INPUT_PULLUP);

  prefs.begin("wifi", false);
  savedSSID = prefs.getString("ssid", "");
  savedPASS = prefs.getString("pass", "");
  savedLang = prefs.getString("lang", "ru");
  adminPass = prefs.getString("admpass", "1234");
  if (!adminPass.length()) {
    adminPass = "1234";
    prefs.putString("admpass", adminPass);
  }
  apSsid = prefs.getString("apSsid", DEFAULT_AP_SSID);
  apPass = prefs.getString("apPass", DEFAULT_AP_PASS);
  if (apSsid.length() < 1) apSsid = DEFAULT_AP_SSID;
  if (apPass.length() < 8) apPass = DEFAULT_AP_PASS;
  tzOffsetMin = prefs.getInt("tzMin", 180);
  relayState = prefs.getBool("relayOn", false);
  relayUpdatedBy = prefs.getString("relayBy", "");
  relayUpdatedAt = prefs.getString("relayAt", "");
  applyRelay(relayState);

  sessionToken = "";

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  ensureDns();
  if (savedSSID.length()) {
    WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
    WiFi.setAutoReconnect(true);
  }

  startPortal();
}

void loop() {
  bool holdLevel = digitalRead(HOLD_BUTTON_PIN);
  bool confirmLevel = digitalRead(CONFIRM_BUTTON_PIN);
  unsigned long nowMs = millis();

  if (!factoryResetArmed) {
    if (!holdLevel) {
      if (!holdStartMs) {
        holdStartMs = nowMs;
      } else if (nowMs - holdStartMs >= FACTORY_HOLD_MS) {
        factoryResetArmed = true;
        factoryArmDeadlineMs = nowMs + FACTORY_CONFIRM_MS;
        blinkTickerMs = nowMs;
        writeStatusLed(true);
      }
    } else {
      holdStartMs = 0;
    }
  }

  if (factoryResetArmed) {
    if (nowMs - blinkTickerMs >= FACTORY_BLINK_MS) {
      blinkTickerMs = nowMs;
      toggleStatusLed();
    }
    if (nowMs > factoryArmDeadlineMs) {
      cancelFactoryArm();
    } else {
      bool holdPressedEdge = !holdLevel && lastHoldLevel;
      bool confirmPressedEdge = !confirmLevel && lastConfirmLevel;
      if (holdPressedEdge || confirmPressedEdge) {
        performFactoryReset();
      }
    }
  }

  lastHoldLevel = holdLevel;
  lastConfirmLevel = confirmLevel;

  dnsServer.processNextRequest();
  server.handleClient();
}
