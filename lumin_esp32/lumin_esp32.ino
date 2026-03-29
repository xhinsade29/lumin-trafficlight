
//
// Wiring:
//   GPIO 23 → RED    LED (+ 220Ω → GND)
//   GPIO 22 → YELLOW LED (+ 220Ω → GND)
//   GPIO 21 → GREEN  LED (+ 220Ω → GND)
//   GPIO 2  → Onboard status LED
// ═══════════════════════════════════════════════════════════

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ──────────────────────────────────────────────────────────
// CONFIGURATION
// ──────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Chichi";
const char* WIFI_PASSWORD = "12345678";
const char* SUPABASE_URL  = "https://xjyvtiudpgpnmwbjxaux.supabase.co";
const char* SUPABASE_KEY  = "sb_publishable_Vx33MOF0kKgpHCI2h9IVPQ_cj_bpIlx";
const char* DEVICE_ID     = "ESP32_TL_001";

// ──────────────────────────────────────────────────────────
// PINS
// ──────────────────────────────────────────────────────────
#define PIN_RED     23
#define PIN_YELLOW  22
#define PIN_GREEN   21
#define PIN_STATUS  2

// ──────────────────────────────────────────────────────────
// DURATIONS (seconds) — fixed, not fetched from DB
// ──────────────────────────────────────────────────────────
#define DUR_RED     30
#define DUR_YELLOW   5
#define DUR_GREEN   25

// ──────────────────────────────────────────────────────────
// INTERVALS (milliseconds)
// ──────────────────────────────────────────────────────────
#define INTERVAL_FETCH        500    // Read config from DB (enable/mode/manual)
#define INTERVAL_COUNTDOWN   1000    // 1-second tick
#define INTERVAL_HEARTBEAT  10000    // Device heartbeat
/* INTERVAL_STATUS removed */
#define INTERVAL_WIFI_RETRY  4000    // WiFi reconnect attempt
#define HTTP_TIMEOUT          800    // HTTP request timeout ms (reduced from 2000)

// ──────────────────────────────────────────────────────────
// LIGHT ENUM
// ──────────────────────────────────────────────────────────
enum LightState { LIGHT_OFF, LIGHT_RED, LIGHT_YELLOW, LIGHT_GREEN };

LightState strToLight(const String& s) {
  if (s == "red")    return LIGHT_RED;
  if (s == "yellow") return LIGHT_YELLOW;
  if (s == "green")  return LIGHT_GREEN;
  return LIGHT_OFF;
}

String lightToStr(LightState l) {
  switch (l) {
    case LIGHT_RED:    return "red";
    case LIGHT_YELLOW: return "yellow";
    case LIGHT_GREEN:  return "green";
    default:           return "off";
  }
}

int durations[] = {0, DUR_RED, DUR_YELLOW, DUR_GREEN};

int durationFor(LightState l) {
  return durations[l];
}

// ──────────────────────────────────────────────────────────
// DEVICE STATE
// ──────────────────────────────────────────────────────────
LightState currentLight  = LIGHT_OFF;
int        remaining     = 0;      // authoritative countdown — ESP32 owns this
bool       enabled       = false;
String     mode          = "auto";
String     manualLight   = "red";

// Config change detection (previous values)
bool   prev_enabled     = false;
String prev_mode        = "auto";
String prev_manualLight = "red";

bool   wifiOK     = false;
bool   supabaseOK = false;

// Timers
unsigned long tFetch     = 0;
unsigned long tCountdown = 0;
unsigned long tHeartbeat = 0;
/* tStatus removed */
unsigned long tWifiRetry = 0;

// ──────────────────────────────────────────────────────────
// HTTP HELPERS
// ──────────────────────────────────────────────────────────
void addAuthHeaders(HTTPClient& http) {
  http.addHeader("apikey",        SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("Prefer",        "return=minimal");
  http.setTimeout(HTTP_TIMEOUT);
}

String httpGET(const String& url) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("apikey",        SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(HTTP_TIMEOUT);
  int code = http.GET();
  String body = (code > 0) ? http.getString() : "";
  supabaseOK = (code > 0);
  if (code < 0) Serial.printf("[HTTP GET] %s\n", http.errorToString(code).c_str());
  http.end();
  return body;
}

int httpPATCH(const String& url, const String& body) {
  HTTPClient http;
  http.begin(url);
  addAuthHeaders(http);
  int code = http.PATCH(body);
  if (code < 0) Serial.printf("[HTTP PATCH] %s\n", http.errorToString(code).c_str());
  http.end();
  return code;
}

int httpPOST(const String& url, const String& body) {
  HTTPClient http;
  http.begin(url);
  addAuthHeaders(http);
  int code = http.POST(body);
  if (code < 0) Serial.printf("[HTTP POST] %s\n", http.errorToString(code).c_str());
  http.end();
  return code;
}

// ──────────────────────────────────────────────────────────
// HARDWARE
// ──────────────────────────────────────────────────────────
void allLightsOff() {
  digitalWrite(PIN_RED,    LOW);
  digitalWrite(PIN_YELLOW, LOW);
  digitalWrite(PIN_GREEN,  LOW);
}

void applyHardware(LightState l) {
  allLightsOff();
  switch (l) {
    case LIGHT_RED:    digitalWrite(PIN_RED,    HIGH); break;
    case LIGHT_YELLOW: digitalWrite(PIN_YELLOW, HIGH); break;
    case LIGHT_GREEN:  digitalWrite(PIN_GREEN,  HIGH); break;
    default: break;
  }
}

// ──────────────────────────────────────────────────────────
// SET LIGHT — always executes (no guard), ESP32 is authority
// ──────────────────────────────────────────────────────────
void setLight(LightState l) {
  String prev = lightToStr(currentLight);
  currentLight = l;
  remaining    = durationFor(l);

  applyHardware(l);

  String cur = lightToStr(l);
  String curUpper = cur; curUpper.toUpperCase();
  Serial.printf("[LIGHT] %s → %s (%ds)\n", prev.c_str(), curUpper.c_str(), remaining);

  // Write new light state to DB immediately — no throttle
  pushStateToDB();
  logLightChange(cur);
}

// ──────────────────────────────────────────────────────────
// AUTO CYCLE: red → green → yellow → red
// ──────────────────────────────────────────────────────────
void autoStep() {
  switch (currentLight) {
    case LIGHT_RED:    setLight(LIGHT_GREEN);  break;
    case LIGHT_GREEN:  setLight(LIGHT_YELLOW); break;
    case LIGHT_YELLOW: setLight(LIGHT_RED);    break;
    default:           setLight(LIGHT_RED);    break;
  }
}

// ──────────────────────────────────────────────────────────
// PUSH STATE TO DB — ESP32 writes, dashboard reads
// This is ONLY function that writes remaining_time.
// remaining_time is never read back from DB by the ESP32.
// Rate limited to prevent congestion.
// ──────────────────────────────────────────────────────────
void pushStateToDB() {
  if (!wifiOK) return;
  
  static unsigned long lastWrite = 0;
  unsigned long now = millis();
  if (now - lastWrite < 1000) return; // Reduced to 1s for dashboard sync
  lastWrite = now;
  
  String url  = String(SUPABASE_URL) + "/rest/v1/device_config?device_id=eq." + DEVICE_ID;
  String body = "{\"current_light\":\"" + lightToStr(currentLight)
    + "\",\"remaining_time\":" + String(remaining)
    + ",\"enabled\":"          + (enabled ? "true" : "false")
    + ",\"mode\":\""           + mode + "\""
    + "}";
  httpPATCH(url, body);
}

// ──────────────────────────────────────────────────────────
// LOG LIGHT CHANGE
// ──────────────────────────────────────────────────────────
void logLightChange(const String& light) {
  if (!wifiOK) return;
  String url  = String(SUPABASE_URL) + "/rest/v1/traffic_log";
  String body = "{\"device_id\":\"" + String(DEVICE_ID)
    + "\",\"light\":\""  + light
    + "\",\"mode\":\""   + mode
    + "\",\"source\":\"device\"}";
  httpPOST(url, body);
}

// ──────────────────────────────────────────────────────────
// HEARTBEAT
// ──────────────────────────────────────────────────────────
void sendHeartbeat() {
  if (!wifiOK) return;
  String url  = String(SUPABASE_URL) + "/rest/v1/devices?device_id=eq." + DEVICE_ID;
  int code = httpPATCH(url, "{\"online\":true}");
  if (code == 200 || code == 204) Serial.println("[HEARTBEAT] OK");
}

// ──────────────────────────────────────────────────────────
// FETCH CONFIG — reads ONLY control fields, never remaining_time
// The ESP32 is the time authority. It writes remaining_time,
// it does NOT read it back to apply to its own countdown.
// ──────────────────────────────────────────────────────────
void fetchConfig() {
  if (!wifiOK) return;

  // Only fetch fields that the DASHBOARD can change
  String url = String(SUPABASE_URL)
    + "/rest/v1/device_config?device_id=eq." + DEVICE_ID
    + "&select=enabled,mode,manual_light";

  String resp = httpGET(url);
  if (resp.isEmpty()) return;

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, resp) || !doc.is<JsonArray>() || doc.size() == 0) return;

  JsonObject obj = doc[0];
  bool   new_enabled     = obj["enabled"]      | false;
  String new_mode        = obj["mode"]          | "auto";
  String new_manualLight = obj["manual_light"]  | "red";

  // ── Detect enabled/disabled change ──────────────────────
  if (new_enabled != prev_enabled) {
    prev_enabled = new_enabled;
    enabled = new_enabled;
    Serial.printf("[CONFIG] enabled changed → %s\n", enabled ? "true" : "false");

    if (!enabled) {
      // System turned OFF
      allLightsOff();
      currentLight = LIGHT_OFF;
      remaining    = 0;
      pushStateToDB();
    } else {
      // System turned ON — start from appropriate light
      if (new_mode == "manual") {
        setLight(strToLight(new_manualLight));
      } else {
        setLight(LIGHT_RED);
      }
    }
    prev_mode        = new_mode;
    prev_manualLight = new_manualLight;
    mode             = new_mode;
    manualLight      = new_manualLight;
    return;
  }

  // ── System is OFF — nothing else to do ──────────────────
  if (!enabled) return;

  // ── Detect mode change ───────────────────────────────────
  if (new_mode != prev_mode) {
    prev_mode = new_mode;
    mode      = new_mode;
    Serial.printf("[CONFIG] mode changed → %s\n", mode.c_str());

    if (mode == "manual") {
      manualLight      = new_manualLight;
      prev_manualLight = new_manualLight;
      setLight(strToLight(manualLight));
    } else {
      // Switched back to auto — restart from red
      setLight(LIGHT_RED);
    }
    return;
  }

  // ── Detect manual light change ───────────────────────────
  if (mode == "manual" && new_manualLight != prev_manualLight) {
    prev_manualLight = new_manualLight;
    manualLight      = new_manualLight;
    Serial.printf("[CONFIG] manual_light changed → %s\n", manualLight.c_str());
    setLight(strToLight(manualLight));
    return;
  }
}

// ──────────────────────────────────────────────────────────
// COUNTDOWN TICK — single 1-second clock
// Writes to DB via pushStateToDB() which has its own rate limit.
// Serial output provides real-time sync to dashboard.
// ──────────────────────────────────────────────────────────
void tickCountdown() {
  if (!enabled || currentLight == LIGHT_OFF) return;

  if (remaining > 0) {
    remaining--;
  }

  // Push to DB (rate limited inside pushStateToDB)
  pushStateToDB();

  // Serial output for dashboard serial sync (real-time)
  Serial.printf("[TICK] light=%s remaining=%d mode=%s\n",
    lightToStr(currentLight).c_str(), remaining, mode.c_str());

  // Auto-advance when time runs out
  if (mode == "auto" && remaining <= 0) {
    autoStep();
  }
}

// ──────────────────────────────────────────────────────────
// SERIAL COMMAND HANDLER
// ──────────────────────────────────────────────────────────
void handleSerial(const String& raw) {
  String cmd = raw; cmd.trim(); cmd.toUpperCase();
  Serial.printf("[CMD] %s\n", cmd.c_str());

  if (cmd == "ENABLE") {
    enabled = true; prev_enabled = true;
    setLight(LIGHT_RED);

  } else if (cmd == "DISABLE") {
    enabled = false; prev_enabled = false;
    allLightsOff();
    currentLight = LIGHT_OFF; remaining = 0;
    pushStateToDB();

  } else if (cmd == "AUTO") {
    mode = "auto"; prev_mode = "auto";
    setLight(LIGHT_RED);

  } else if (cmd == "RED") {
    mode = "manual"; manualLight = "red";
    prev_mode = "manual"; prev_manualLight = "red";
    setLight(LIGHT_RED);

  } else if (cmd == "YELLOW") {
    mode = "manual"; manualLight = "yellow";
    prev_mode = "manual"; prev_manualLight = "yellow";
    setLight(LIGHT_YELLOW);

  } else if (cmd == "GREEN") {
    mode = "manual"; manualLight = "green";
    prev_mode = "manual"; prev_manualLight = "green";
    setLight(LIGHT_GREEN);

  } else if (cmd == "STATUS") {
    Serial.printf("[STATUS] light=%s rem=%d mode=%s wifi=%s\n", lightToStr(currentLight).c_str(), remaining, mode.c_str(), wifiOK?"OK":"NO");

  } else {
    Serial.println("[CMD] Valid: ENABLE/DISABLE/AUTO/RED/YELLOW/GREEN/STATUS");
  }
}

// ──────────────────────────────────────────────────────────
// STATUS PRINT
// ──────────────────────────────────────────────────────────
void printStatus() {
  Serial.printf(
    "\n┌─────────────── STATUS ───────────────┐\n"
    "│ WiFi:    %-6s  Supabase: %-6s    │\n"
    "│ Enabled: %-5s   Mode:     %-8s │\n"
    "│ Light:   %-6s  Remaining: %-4ds   │\n"
    "│ Durations: R=%ds  Y=%ds  G=%ds      │\n"
    "└──────────────────────────────────────┘\n",
    wifiOK ? "OK" : "FAIL",
    supabaseOK ? "OK" : "FAIL",
    enabled ? "YES" : "NO",
    mode.c_str(),
    lightToStr(currentLight).c_str(),
    remaining,
    DUR_RED, DUR_YELLOW, DUR_GREEN
  );
}

// ──────────────────────────────────────────────────────────
// WIFI
// ──────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    delay(200); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true;
    digitalWrite(PIN_STATUS, HIGH);
    Serial.printf(" OK — IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    wifiOK = false;
    Serial.println(" FAILED");
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiOK) {
      wifiOK = true;
      digitalWrite(PIN_STATUS, HIGH);
      Serial.println("[WIFI] Reconnected");
      sendHeartbeat();
    }
    return;
  }
  if (wifiOK) {
    wifiOK = false;
    digitalWrite(PIN_STATUS, LOW);
    Serial.println("[WIFI] Lost connection");
  }
  unsigned long now = millis();
  if (now - tWifiRetry >= INTERVAL_WIFI_RETRY) {
    tWifiRetry = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("[WIFI] Retrying...");
  }
}

// ──────────────────────────────────────────────────────────
// SETUP
// ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  Serial.println();
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║   LUMIN Traffic Controller v4.0      ║");
  Serial.println("║   Timing-accurate, delay-free         ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.printf("[CONFIG] R=%ds  Y=%ds  G=%ds\n", DUR_RED, DUR_YELLOW, DUR_GREEN);

  pinMode(PIN_RED,    OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_GREEN,  OUTPUT);
  pinMode(PIN_STATUS, OUTPUT);
  allLightsOff();

  connectWiFi();

  if (wifiOK) {
    sendHeartbeat();
    // Read initial config from DB (enabled flag, mode, manual_light)
    fetchConfig();
  }

  Serial.println("[BOOT] Ready — loop starting");
}

// ──────────────────────────────────────────────────────────
// LOOP — entirely millis()-based, zero blocking delay()
// ──────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // Serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleSerial(cmd);
  }

  // WiFi watchdog
  maintainWiFi();

  // Fetch config (enable/mode/manual changes from dashboard)
  if (now - tFetch >= INTERVAL_FETCH) {
    tFetch = now;
    fetchConfig();
  }

  // 1-second countdown tick — this drives ALL timing
  if (now - tCountdown >= INTERVAL_COUNTDOWN) {
    tCountdown = now;
    tickCountdown();
  }

  // Heartbeat
  if (now - tHeartbeat >= INTERVAL_HEARTBEAT) {
    tHeartbeat = now;
    sendHeartbeat();
  }

  /* Status print removed - only tick status */
}
