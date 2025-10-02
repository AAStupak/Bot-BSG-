/**
 * ESP32 firmware for BSG object control
 * -------------------------------------
 * Features:
 *  - Wi-Fi configuration portal (AP SSID: BSG-OBJECT, password: 1234)
 *  - Web UI for network setup and relay state monitoring
 *  - REST API at /api/state (GET returns status, POST toggles relay)
 *  - Optional shared secret for securing API requests
 *  - Persistent storage of Wi-Fi credentials, API secret, and relay state
 *  - Automatic reconnection watchdog and mDNS announcement (object.local)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// === Hardware configuration ===
static const uint8_t RELAY_PIN = 23;          // GPIO that drives the contactor relay
static const bool RELAY_ACTIVE_HIGH = true;   // set to false if your relay is active LOW
static const char *AP_SSID = "BSG-OBJECT";   // access-point SSID for configuration
static const char *AP_PASSWORD = "1234";     // WPA2 password for the AP
static const char *MDNS_NAME = "object";     // device will be reachable as http://object.local

// === Globals ===
Preferences prefs;
WebServer server(80);
String wifiSsid;
String wifiPassword;
String apiSecret;
bool relayState = false;
unsigned long lastReconnectAttempt = 0;

// === Helpers ===
void applyRelay(bool on) {
  relayState = on;
  bool level = RELAY_ACTIVE_HIGH ? on : !on;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
}

void persistRelayState(bool on) {
  applyRelay(on);
  prefs.putBool("relay", on);
}

String currentStateJson() {
  return String("{\"ok\":true,\"state\":\"") + (relayState ? "on" : "off") + "\"}";
}

String parseJsonValue(const String &payload, const String &key) {
  String needle = String("\"") + key + "\":";
  int start = payload.indexOf(needle);
  if (start < 0) return "";
  start += needle.length();
  while (start < (int)payload.length() && (payload[start] == ' ' || payload[start] == '"')) {
    if (payload[start] == '"') {
      start++;
      int end = payload.indexOf('"', start);
      if (end < 0) return "";
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

void sendJson(int code, const String &body) {
  server.send(code, "application/json", body);
}

void handleApiGet() {
  sendJson(200, currentStateJson());
}

void handleApiPost() {
  String body = server.arg("plain");
  if (body.isEmpty()) {
    sendJson(400, "{\"ok\":false,\"error\":\"empty_body\"}");
    return;
  }

  String providedSecret = parseJsonValue(body, "secret");
  if (apiSecret.length() > 0 && providedSecret != apiSecret) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }

  String stateValue = parseJsonValue(body, "state");
  stateValue.toLowerCase();
  bool requested = (stateValue == "on" || stateValue == "1" || stateValue == "true");

  persistRelayState(requested);
  sendJson(200, currentStateJson());
}

String htmlEscape(const String &raw) {
  String safe;
  safe.reserve(raw.length());
  for (char c : raw) {
    switch (c) {
      case '&': safe += "&amp;"; break;
      case '<': safe += "&lt;"; break;
      case '>': safe += "&gt;"; break;
      case '"': safe += "&quot;"; break;
      case '\'': safe += "&#39;"; break;
      default: safe += c; break;
    }
  }
  return safe;
}

String renderNetworkOptions() {
  String html;
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    html += "<option value>Не найдено сетей</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      html += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
      html += htmlEscape(WiFi.SSID(i));
      html += " (" + String(WiFi.RSSI(i)) + " dBm)";
      html += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "" : " 🔒";
      html += "</option>";
    }
  }
  WiFi.scanDelete();
  return html;
}

String renderHtml() {
  String ipInfo = WiFi.isConnected() ? WiFi.localIP().toString() : String("192.168.4.1");
  String mode = WiFi.isConnected() ? "STA" : "AP";
  String html = F("<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\"><title>BSG Object Control</title>"
                     "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                     "<style>body{font-family:Arial,Helvetica,sans-serif;background:#0f172a;color:#f8fafc;margin:0;padding:0;}"
                     "main{max-width:720px;margin:40px auto;padding:24px;background:#1e293b;border-radius:18px;box-shadow:0 20px 45px rgba(15,23,42,0.4);}"
                     "h1{font-size:28px;margin:0 0 16px;}h2{margin-top:32px;}fieldset{border:1px solid rgba(148,163,184,0.4);border-radius:12px;padding:18px;margin-bottom:20px;}"
                     "label{display:block;margin-bottom:8px;font-weight:600;}input,select{width:100%;padding:10px 12px;border-radius:10px;border:1px solid rgba(148,163,184,0.4);background:#0f172a;color:#f8fafc;}"
                     "button{display:inline-flex;align-items:center;gap:8px;border:none;border-radius:999px;padding:12px 22px;font-size:15px;font-weight:600;cursor:pointer;transition:.2s;}"
                     ".primary{background:#38bdf8;color:#0f172a;}.danger{background:#f97316;color:#0f172a;}.muted{background:#334155;color:#e2e8f0;}"
                     "p,li{line-height:1.55;}ul{padding-left:18px;}footer{margin-top:32px;font-size:13px;color:#94a3b8;text-align:center;}</style></head><body><main>");
  html += F("<h1>💡 Пульт объекта</h1><p>Режим: <b>");
  html += mode;
  html += F("</b> • IP: <code>");
  html += ipInfo;
  html += F("</code></p><fieldset><legend>Текущее состояние</legend><p style=\"font-size:20px;margin-bottom:16px;\">");
  html += relayState ? "💡 Свет включен" : "🌑 Свет выключен";
  html += F("</p><form method=\"post\" action=\"/toggle\"><input type=\"hidden\" name=\"desired\" value=\"");
  html += relayState ? "off" : "on";
  html += F("\"><button class=\"");
  html += relayState ? "danger">💤 Выключить" : "primary">💡 Включить";
  html += F("</button></form></fieldset><fieldset><legend>Настройка Wi-Fi</legend><form method=\"post\" action=\"/save\">");
  html += F("<label for=\"ssid\">Выберите сеть</label><select id=\"ssid\" name=\"ssid\">\n");
  html += renderNetworkOptions();
  html += F("</select><label for=\"password\">Пароль Wi-Fi</label><input id=\"password\" type=\"password\" name=\"password\" placeholder=\"Введите пароль\" value=\"\">");
  html += F("<label for=\"secret\">Секрет для API (опционально)</label><input id=\"secret\" name=\"secret\" placeholder=\"Совместное слово для бота\" value=\"");
  html += F("<button class=\"primary\" style=\"margin-top:16px;\" type=\"submit\">💾 Сохранить настройки</button></form>");
  html += F("<form method=\"post\" action=\"/forget\" style=\"margin-top:12px;\"><button class=\"muted\" type=\"submit\">♻️ Забыть сеть</button></form></fieldset>");
  html += F("<fieldset><legend>API</legend><p>• GET <code>/api/state</code> — текущее состояние (JSON).<br>• POST <code>/api/state</code> с телом <code>{\"state\":\"on|off\",\"secret\":\"...\"}</code> — изменить состояние.</p>");
  html += F("<p>Текущий секрет: <code>");
  html += apiSecret.length() ? htmlEscape(apiSecret) : String("не задан");
  html += F("</code></p></fieldset>");
  html += F("<footer>BSG Object Control • ESP32</footer></main></body></html>");
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", renderHtml());
}

void handleToggle() {
  String desired = server.arg("desired");
  desired.toLowerCase();
  bool turnOn = (desired == "on");
  persistRelayState(turnOn);
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleForget() {
  wifiSsid = "";
  wifiPassword = "";
  prefs.putString("ssid", "");
  prefs.putString("password", "");
  server.send(200, "text/html", "<meta http-equiv='refresh' content='0; url=/'><p>Wi-Fi настройки очищены.</p>");
  delay(1000);
  ESP.restart();
}

void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  String secret = server.arg("secret");
  ssid.trim();
  password.trim();
  secret.trim();

  if (ssid.length() == 0) {
    server.send(400, "text/html", "<p>Укажите SSID.</p><a href='/'>&larr; Назад</a>");
    return;
  }

  wifiSsid = ssid;
  wifiPassword = password;
  apiSecret = secret;
  prefs.putString("ssid", wifiSsid);
  prefs.putString("password", wifiPassword);
  prefs.putString("secret", apiSecret);

  server.send(200, "text/html", "<meta http-equiv='refresh' content='2; url=/'><p>Настройки сохранены. Переподключение...</p>");
  delay(1200);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

void startHttpServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/toggle", HTTP_POST, handleToggle);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/forget", HTTP_POST, handleForget);
  server.on("/api/state", HTTP_GET, handleApiGet);
  server.on("/api/state", HTTP_POST, handleApiPost);
  server.onNotFound(handleNotFound);
  server.begin();
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[AP] SSID: %s | IP: %s\n", AP_SSID, ip.toString().c_str());
  startHttpServer();
}

bool connectToStation() {
  if (wifiSsid.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.printf("[WiFi] Connecting to %s...\n", wifiSsid.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    if (!MDNS.begin(MDNS_NAME)) {
      Serial.println("[mDNS] Failed to start service");
    } else {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[mDNS] http://%s.local\n", MDNS_NAME);
    }
    startHttpServer();
    return true;
  }
  Serial.println("[WiFi] Connection failed, switching to AP mode");
  return false;
}

void loadPreferences() {
  prefs.begin("objectctl", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("password", "");
  apiSecret = prefs.getString("secret", "");
  relayState = prefs.getBool("relay", false);
  applyRelay(relayState);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(RELAY_PIN, OUTPUT);
  applyRelay(false);
  loadPreferences();
  Serial.println("BSG Object Control booting...");

  if (!connectToStation()) {
    startAccessPoint();
  }
}

void loop() {
  server.handleClient();
  MDNS.update();

  if (WiFi.getMode() == WIFI_STA && wifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 10000) {
      lastReconnectAttempt = now;
      Serial.println("[WiFi] Attempting reconnect...");
      WiFi.disconnect();
      WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    }
  }
}
