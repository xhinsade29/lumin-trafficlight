#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

//////////////////////////////////////////////////////////
// WIFI CREDENTIALS
//////////////////////////////////////////////////////////

const char* WIFI_SSID = "Chichi";
const char* WIFI_PASSWORD = "12345678";

//////////////////////////////////////////////////////////
// SUPABASE CONFIG
//////////////////////////////////////////////////////////

const char* SUPABASE_URL = "https://xjyvtiudpgpnmwbjxaux.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_Vx33MOF0kKgpHCI2h9IVPQ_cj_bpIlx";
const char* DEVICE_ID = "ESP32_TL_001";

//////////////////////////////////////////////////////////
// LED PINS
//////////////////////////////////////////////////////////

#define RED_LED 23
#define YELLOW_LED 22
#define GREEN_LED 21

//////////////////////////////////////////////////////////
// SYSTEM VARIABLES
//////////////////////////////////////////////////////////

struct TrafficState {
  bool enabled;
  String mode;
  int greenDuration;
  int yellowDuration;
  int redDuration;
};

TrafficState state;

String currentLight = "red";

unsigned long previousMillis = 0;
unsigned long fetchTimer = 0;
unsigned long heartbeatTimer = 0;
unsigned long statusTimer = 0;

int remainingTime = 0;

bool wifiConnected = false;
bool supabaseConnected = false;
bool dashboardControl = false;

//////////////////////////////////////////////////////////
// WIFI CONNECTION
//////////////////////////////////////////////////////////

void connectWiFi() {

  Serial.println("\nCONNECTING TO WIFI...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {

    wifiConnected = true;

    Serial.println("\nWIFI CONNECTED ✓");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

  } else {

    wifiConnected = false;
    Serial.println("\nWIFI CONNECTION FAILED ✗");

  }
}

//////////////////////////////////////////////////////////
// PARSE DASHBOARD CONFIG
//////////////////////////////////////////////////////////

void parseConfig(String payload) {

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, payload);

  JsonObject obj = doc[0];

  state.enabled = obj["enabled"];
  state.mode = obj["mode"].as<String>();
  state.greenDuration = obj["green_duration"];
  state.yellowDuration = obj["yellow_duration"];
  state.redDuration = obj["red_duration"];

  Serial.println("\nDASHBOARD CONFIG RECEIVED");

  Serial.print("Mode: ");
  Serial.println(state.mode);

  Serial.print("Enabled: ");
  Serial.println(state.enabled);

  Serial.print("Green Duration: ");
  Serial.println(state.greenDuration);

  Serial.print("Yellow Duration: ");
  Serial.println(state.yellowDuration);

  Serial.print("Red Duration: ");
  Serial.println(state.redDuration);

  dashboardControl = true;
}

//////////////////////////////////////////////////////////
// FETCH CONFIG FROM SUPABASE
//////////////////////////////////////////////////////////

void fetchConfig() {

  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  String url = String(SUPABASE_URL) + "/rest/v1/device_config?device_id=eq." + DEVICE_ID + "&select=*";

  http.begin(url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  int code = http.GET();

  if (code == 200) {

    supabaseConnected = true;

    String payload = http.getString();

    parseConfig(payload);

    Serial.println("SUPABASE CONNECTION ✓");

  } else {

    supabaseConnected = false;

    Serial.print("SUPABASE ERROR: ");
    Serial.println(code);
  }

  http.end();
}

//////////////////////////////////////////////////////////
// LOG LIGHT CHANGE TO TRAFFIC_LOG
//////////////////////////////////////////////////////////

void logLightChange(String light) {

  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  String url = String(SUPABASE_URL) + "/rest/v1/traffic_log";

  http.begin(url);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");

  String body = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"light\":\"" + light + "\",\"mode\":\"" + state.mode + "\",\"source\":\"device\"}";

  int code = http.POST(body);

  if (code == 201) {
    Serial.println("LOGGED TO TRAFFIC_LOG ✓");
  } else {
    Serial.print("LOG FAILED: ");
    Serial.println(code);
  }

  http.end();
}

//////////////////////////////////////////////////////////
// UPDATE CURRENT LIGHT
//////////////////////////////////////////////////////////

void updateCurrentLight(String light) {

  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  String url = String(SUPABASE_URL) + "/rest/v1/device_config?device_id=eq." + DEVICE_ID;

  http.begin(url);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");

  String body = "{\"current_light\":\"" + light + "\"}";

  int code = http.PATCH(body);

  if (code == 204 || code == 200) {

    Serial.println("DATABASE UPDATED ✓");
    Serial.print("Current Light Stored: ");
    Serial.println(light);

  } else {

    Serial.print("DATABASE UPDATE FAILED: ");
    Serial.println(code);
  }

  http.end();
}

//////////////////////////////////////////////////////////
// HEARTBEAT
//////////////////////////////////////////////////////////

void sendHeartbeat() {

  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  String url = String(SUPABASE_URL) + "/rest/v1/devices?device_id=eq." + DEVICE_ID;

  http.begin(url);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");

  String body = "{\"online\":true}";

  int code = http.PATCH(body);

  http.end();

  if (code == 204 || code == 200) {
    Serial.println("DEVICE HEARTBEAT SENT");
  } else {
    Serial.print("HEARTBEAT FAILED: ");
    Serial.println(code);
  }
}

//////////////////////////////////////////////////////////
// TRAFFIC LIGHT CONTROL
//////////////////////////////////////////////////////////

void setLight(String light) {

  digitalWrite(RED_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(GREEN_LED, LOW);

  if (light == "green") {

    digitalWrite(GREEN_LED, HIGH);
    remainingTime = state.greenDuration;

  } else if (light == "yellow") {

    digitalWrite(YELLOW_LED, HIGH);
    remainingTime = state.yellowDuration;

  } else {

    digitalWrite(RED_LED, HIGH);
    remainingTime = state.redDuration;

  }

  currentLight = light;

  updateCurrentLight(light);
  logLightChange(light);

  Serial.print("LIGHT: ");
  Serial.println(light);
}

//////////////////////////////////////////////////////////
// AUTO TRAFFIC SYSTEM
//////////////////////////////////////////////////////////

void autoTraffic() {

  if (remainingTime <= 0) {

    if (currentLight == "green") {

      setLight("yellow");

    } else if (currentLight == "yellow") {

      setLight("red");

    } else {

      setLight("green");

    }

  }
}

//////////////////////////////////////////////////////////
// SYSTEM STATUS
//////////////////////////////////////////////////////////

void printSystemStatus(){

  Serial.println("\n===== SYSTEM STATUS =====");

  Serial.print("WiFi: ");
  Serial.println(wifiConnected ? "CONNECTED" : "DISCONNECTED");

  Serial.print("Supabase: ");
  Serial.println(supabaseConnected ? "CONNECTED" : "DISCONNECTED");

  Serial.print("Dashboard Control: ");
  Serial.println(dashboardControl ? "ACTIVE" : "NOT DETECTED");

  Serial.println("=========================\n");
}

//////////////////////////////////////////////////////////
// SETUP
//////////////////////////////////////////////////////////

void setup() {

  Serial.begin(115200);

  pinMode(RED_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);

  connectWiFi();

  fetchConfig();

  setLight("red");

  Serial.println("\nSYSTEM STARTED\n");
}

//////////////////////////////////////////////////////////
// LOOP
//////////////////////////////////////////////////////////

void loop() {

  unsigned long currentMillis = millis();

  // Fetch dashboard config every 5 sec
  if(currentMillis - fetchTimer > 5000){

    fetchTimer = currentMillis;
    fetchConfig();

  }

  // Heartbeat every 15 sec
  if(currentMillis - heartbeatTimer > 15000){

    heartbeatTimer = currentMillis;
    sendHeartbeat();

  }

  // Status every 10 sec
  if(currentMillis - statusTimer > 10000){

    statusTimer = currentMillis;
    printSystemStatus();

  }

  // Traffic countdown
  if(currentMillis - previousMillis > 1000){

    previousMillis = currentMillis;

    remainingTime--;

    Serial.print("Remaining: ");
    Serial.println(remainingTime);

    if(state.mode == "auto" && state.enabled){

      autoTraffic();

    }

  }

}