/*
  ESP32 DevKit — SmartHome Dashboard + SinricPro Alexa + Telegram Control + LDR Auto Light + Plant Watering
  + Auto Exhaust Fan on Gas + Outside Weather + AI Energy Suggestion
  Sensors: DHT11, PIR, MQ-6, LDR, Soil Moisture
  Libraries:
    - DHT sensor library (Adafruit)
    - SinricPro
    - UniversalTelegramBot
    - WiFiClientSecure
    - HTTPClient (built-in)
*/


#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <HTTPClient.h>


// ======================== CONFIG ============================
// WiFi
const char* WIFI_SSID = "Grms";
const char* WIFI_PASS = "12345678";


// Telegram bot credentials
#define BOT_TOKEN  "8308112077:AAFjA3OXmZhZ2pdiPWBhu0oc3JBQQyftg7I"
#define CHAT_ID    "6089430487"


const char* HOME_LAT = "10.8279";
const char* HOME_LON = "77.0605";


// Sinric Pro credentials
const char* APP_KEY    = "23ad1405-a7d0-4b87-a30a-13276b9c22eb";
const char* APP_SECRET = "42abe68f-110b-492b-ab75-e88c2403b7de-1f9c212f-4a0e-44f7-9213-4fbab7935eb5";


// SinricPro Device IDs
const char* LIGHT_ID = "6896ed5cddd2551252befd39";
const char* FAN_ID   = "6896edcc678c5bc9ab27dca3";
const char* ALARM_ID = "YOUR_ALARM_DEVICE_ID";  // <-- set this!


// Weather (OpenWeatherMap)
const char* OPENWEATHER_API_KEY = "21d106c87261beac8a14156fe63db0fc"; // <-- set this!
const char* HOME_CITY           = "Coimbatore";               // <-- set your city
const char* HOME_COUNTRY        = "IN";                       // optional
// If you prefer lat/lon (more accurate), set these and use the lat/lon endpoint instead
// const char* HOME_LAT = "11.0168";
// const char* HOME_LON = "76.9558";


// Hardware pins
const uint8_t DHTPIN         = 4;
const uint8_t DHTTYPE        = DHT11;
const uint8_t PIR_PIN        = 14;
const uint8_t MQ6_PIN        = 34;
const uint8_t LED_LIGHT      = 16;
const uint8_t LED_FAN        = 17;  // Room fan
const uint8_t LED_ALARM      = 27;
const uint8_t LDR_PIN        = 35;
const uint8_t SOIL_MOISTURE_PIN = 32; // Analog
const uint8_t WATER_PUMP_PIN = 25;     // Relay control
const uint8_t EXHAUST_FAN_PIN = 26;    // NEW: Exhaust fan relay (for gas)


// Settings
const bool PIR_ACTIVE_LOW = false;
int MQ6_ALERT_THRESHOLD = 1500;
int LDR_THRESHOLD = 3000;
int SOIL_MOISTURE_THRESHOLD = 500; // Adjust per sensor calibration


// Weather fetch interval
const unsigned long WEATHER_FETCH_INTERVAL = 10UL * 60UL * 1000UL; // 10 minutes


// AI Suggestion: fan unused for 2 hours (no motion)
const unsigned long FAN_UNUSED_LIMIT_MS = 2UL * 60UL * 60UL * 1000UL; // 2 hours


// ======================== OBJECTS ===========================
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);


// Sinric devices
SinricProSwitch& lightSwitch = SinricPro[LIGHT_ID];
SinricProSwitch& fanSwitch   = SinricPro[FAN_ID];
SinricProSwitch& alarmSwitch = SinricPro[ALARM_ID];


// States
bool autoLightControl = true;
bool autoWaterPumpControl = true;


unsigned long lastTelegramCheck = 0;
const unsigned long TELEGRAM_CHECK_INTERVAL = 2000; // 2 sec


// Weather cache
float outsideTemp = NAN;
float outsideHum  = NAN;
String outsideCond = "--";
unsigned long lastWeatherFetch = 0;


// AI suggestion tracking
unsigned long fanOnStart = 0;
bool fanWasOn = false;
bool suggestionSent = false; // avoid spamming


// Gas alert tracking (to avoid repeated Telegram spam)
bool gasAlertActive = false;



// ======================== HTML Dashboard ============================
String htmlPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>SmartHome Dashboard</title>
  <style>
    body { margin: 0; font-family: 'Segoe UI', Arial, sans-serif; background: #f4f6f9; color: #333; }
    header { background: #0077cc; color: white; padding: 15px; text-align: center; font-size: 1.6rem; font-weight: bold; box-shadow: 0 2px 6px rgba(0,0,0,0.1); }
    .status-bar { text-align: center; font-size: 0.9rem; background: #e8f4ff; padding: 5px; color: #0077cc; font-weight: 500; }
    main { display: grid; gap: 15px; padding: 15px; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); max-width: 1200px; margin: auto; }
    .card { background: white; border-radius: 10px; padding: 15px; text-align: center; box-shadow: 0 4px 8px rgba(0,0,0,0.08); transition: transform 0.2s; }
    .card:hover { transform: translateY(-4px); }
    .card h2 { font-size: 1rem; color: #555; margin-bottom: 8px; }
    .value { font-size: 2rem; font-weight: bold; color: #0077cc; margin: 6px 0; }
    .note { display: inline-block; padding: 4px 8px; border-radius: 12px; font-size: 0.85rem; font-weight: bold; }
    .normal { background: #e4f9e4; color: #2e7d32; }
    .alert { background: #ffd1d1; color: #b30000; }
    .motion-on { background: #fff1c1; color: #b36b00; }
    .motion-off { background: #e6ebf4; color: #555; }
    .controls { display: flex; justify-content: center; gap: 10px; flex-wrap: wrap; margin-top: 8px; }
    .toggle-btn { background: #ddd; border: none; border-radius: 20px; padding: 8px 16px; cursor: pointer; font-weight: bold; transition: background 0.3s; }
    .toggle-btn.active { background: #4caf50; color: white; }
    footer { text-align: center; padding: 10px; font-size: 0.8rem; color: #999; }
  </style>
</head>
<body>
<header>🏠 Smart Home Dashboard</header>
<div class="status-bar" id="connStatus">Connecting to ESP...</div>
<main>
  <div class="card">
    <h2>🌡 Room Temperature</h2>
    <div id="temp" class="value">-- °C</div>
  </div>
  <div class="card">
    <h2>💧 Room Humidity</h2>
    <div id="hum" class="value">-- %</div>
  </div>
  <div class="card">
    <h2>🔥 Air Quality</h2>
    <div id="gas" class="value">--</div>
    <div id="gasNote" class="note normal">Normal</div>
  </div>
  <div class="card">
    <h2>🚶 Presence</h2>
    <div id="motion" class="value">--</div>
    <div id="motionNote" class="note motion-off">No Presence</div>
  </div>
  <div class="card">
    <h2>☀ Brightness</h2>
    <div id="ldr" class="value">--</div>
    <div id="ldrNote" class="note normal">Auto Mode</div>
  </div>

  <div class="card">
    <h2>🌱 Soil Health</h2>
    <div id="soilMoisture" class="value">--</div>
    <div id="pumpStatus" class="note normal">Irrigation OFF</div>
    <div class="controls">
      <button id="btnPump" class="toggle-btn" onclick="toggle('pump')">💧 Irrigation</button>
      <button id="btnAutoPump" class="toggle-btn" onclick="toggle('autoPump')">☀ Auto Irrigation</button>
    </div>
  </div>

  <div class="card">
    <h2>🌬 Exhaust Fan</h2>
    <div id="exhaustState" class="value">--</div>
    <div id="exhaustNote" class="note normal">Safe</div>
    <div class="controls">
      <button id="btnExhaust" class="toggle-btn" onclick="toggle('exhaust')">🌬 Manual Exhaust</button>
    </div>
  </div>

  <div class="card">
    <h2>🌍 Outside Weather</h2>
    <div id="outTemp" class="value">-- °C</div>
    <div id="outHum" class="value">-- %</div>
    <div id="outCond" class="note normal">--</div>
  </div>

  <div class="card">
    <h2>🔌 Room Controls</h2>
    <div class="controls">
      <button id="btnLight" class="toggle-btn" onclick="toggle('light')">💡 Light</button>
      <button id="btnFan" class="toggle-btn" onclick="toggle('fan')">🌀 Fan</button>
      <button id="btnAlarm" class="toggle-btn" onclick="toggle('alarm')">🚨 Alarm</button>
      <button id="btnAuto" class="toggle-btn" onclick="toggle('auto')">☀ Auto Light</button>
    </div>
  </div>

  <div class="card">
    <h2>🤖 Smart Suggestion</h2>
    <div id="aiSuggest" class="note normal">All good 👍</div>
  </div>
</main>
<footer>© 2025 SmartHome • Alexa + Telegram + Weather + AI</footer>
<script>
let states = { light:false, fan:false, alarm:false, auto:true, pump:false, autoPump:true, exhaust:false };

function updateButtons(){
  ['light','fan','alarm','auto','pump','autoPump','exhaust'].forEach(dev=>{
    const btn = document.getElementById('btn' + dev.charAt(0).toUpperCase()+dev.slice(1));
    if(btn){ if(states[dev]) btn.classList.add('active'); else btn.classList.remove('active'); }
  });
}

function fetchData(){
  fetch('/data').then(r=>r.json()).then(data=>{
    document.getElementById('connStatus').innerText = 'Connected ✅';
    document.getElementById('temp').innerText = data.temp.toFixed(1) + ' °C';
    document.getElementById('hum').innerText  = data.hum.toFixed(1) + ' %';
    document.getElementById('gas').innerText  = data.gas;
    document.getElementById('ldr').innerText  = data.ldr;
    document.getElementById('ldrNote').innerText = data.autoLight ? 'Auto Mode' : 'Manual Mode';

    let gasNote = document.getElementById('gasNote');
    gasNote.className = 'note ' + (data.gas_alert ? 'alert' : 'normal');
    gasNote.innerText = data.gas_alert ? '⚠ Poor Air Quality' : 'Normal';

    let motionNote = document.getElementById('motionNote');
    motionNote.className = 'note ' + (data.motion ? 'motion-on' : 'motion-off');
    motionNote.innerText = data.motion ? 'Presence Detected' : 'No Presence';

    // Soil moisture and pump display
    document.getElementById('soilMoisture').innerText = data.soilMoisture;
    const pumpNote = document.getElementById('pumpStatus');
    pumpNote.className = 'note ' + (data.pumpOn ? 'alert' : 'normal');
    pumpNote.innerText = data.pumpOn ? 'Irrigation ON' : 'Irrigation OFF';

    // Exhaust
    document.getElementById('exhaustState').innerText = data.exhaustOn ? 'ON' : 'OFF';
    const exhaustNote = document.getElementById('exhaustNote');
    exhaustNote.className = 'note ' + (data.gas_alert ? 'alert' : 'normal');
    exhaustNote.innerText = data.gas_alert ? 'Exhaust Active (Gas)' : 'Safe';

    // Outside weather
    document.getElementById('outTemp').innerText = isNaN(data.outTemp) ? '-- °C' : (data.outTemp.toFixed(1) + ' °C');
    document.getElementById('outHum').innerText  = isNaN(data.outHum) ? '-- %' : (data.outHum.toFixed(0) + ' %');
    document.getElementById('outCond').innerText = data.outCond || '--';

    // AI Suggestion
    const ai = document.getElementById('aiSuggest');
    if (data.aiSuggest && data.aiSuggest.length > 0) {
      ai.className = 'note alert';
      ai.innerText = data.aiSuggest;
    } else {
      ai.className = 'note normal';
      ai.innerText = 'All good 👍';
    }

    states.light = !!data.ledLight;
    states.fan   = !!data.ledFan;
    states.alarm = !!data.ledAlarm;
    states.auto  = !!data.autoLight;
    states.pump  = !!data.pumpOn;
    states.autoPump = !!data.autoPump;
    states.exhaust = !!data.exhaustOn;
    updateButtons();
  }).catch(()=>{
    document.getElementById('connStatus').innerText = 'Disconnected ❌';
  });
}

function toggle(dev){
  fetch('/toggle?dev='+dev).then(()=>{
    states[dev] = !states[dev];
    updateButtons();
  });
}

setInterval(fetchData, 1000);
window.onload = fetchData;
</script>
</body>
</html>
)rawliteral";


// ======================== HELPERS ===========================
int getSmoothADC(uint8_t pin, int samples = 15, int delayMs = 3) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(delayMs);
  }
  return sum / samples;
}


void sendTelegramStatus() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int gas = getSmoothADC(MQ6_PIN);
  int ldr = getSmoothADC(LDR_PIN);
  bool pir = PIR_ACTIVE_LOW ? !digitalRead(PIR_PIN) : digitalRead(PIR_PIN);
  int soil = getSmoothADC(SOIL_MOISTURE_PIN);
  bool pump = digitalRead(WATER_PUMP_PIN);
  bool exhaust = digitalRead(EXHAUST_FAN_PIN);

  String msg = "🏠 SmartHome Status:\n";
  msg += "🌡 Temp: " + String(t, 1) + "°C\n";
  msg += "💧 Hum: " + String(h, 1) + "%\n";
  msg += "🔥 Gas: " + String(gas) + (gas > MQ6_ALERT_THRESHOLD ? " ⚠ ALERT!\n" : " Normal\n");
  msg += "🚶 Presence: " + String(pir ? "Detected" : "None") + "\n";
  msg += "☀ Brightness: " + String(ldr) + "\n";
  msg += "🌱 Soil: " + String(soil) + "\n";
  msg += "💧 Irrigation: " + String(pump ? "ON" : "OFF") + "\n";
  msg += "🌬 Exhaust: " + String(exhaust ? "ON" : "OFF") + "\n";
  msg += "💡 Light: " + String(digitalRead(LED_LIGHT) ? "ON" : "OFF") + "\n";
  msg += "🌀 Fan: " + String(digitalRead(LED_FAN) ? "ON" : "OFF") + "\n";
  msg += "🚨 Alarm: " + String(digitalRead(LED_ALARM) ? "ON" : "OFF") + "\n";
  msg += "☀ Auto Light: " + String(autoLightControl ? "ON" : "OFF") + "\n";
  msg += "☀ Auto Pump: " + String(autoWaterPumpControl ? "ON" : "OFF") + "\n";
  msg += "🌍 Outside: " + (isnan(outsideTemp) ? String("--") : String(outsideTemp,1) + "°C") + ", " + (outsideCond.length()?outsideCond:"--");
  bot.sendMessage(CHAT_ID, msg, "");
}


void handleTelegramMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String text = bot.messages[i].text;
    text.toLowerCase();

    if (text == "/light_on")  { digitalWrite(LED_LIGHT, HIGH); autoLightControl = false; bot.sendMessage(CHAT_ID, "💡 Light ON", ""); }
    if (text == "/light_off") { digitalWrite(LED_LIGHT, LOW);  autoLightControl = false; bot.sendMessage(CHAT_ID, "💡 Light OFF", ""); }
    if (text == "/fan_on")    { digitalWrite(LED_FAN, HIGH); bot.sendMessage(CHAT_ID, "🌀 Fan ON", ""); }
    if (text == "/fan_off")   { digitalWrite(LED_FAN, LOW);  bot.sendMessage(CHAT_ID, "🌀 Fan OFF", ""); }
    if (text == "/alarm_on")  { digitalWrite(LED_ALARM, HIGH); bot.sendMessage(CHAT_ID, "🚨 Alarm ON", ""); }
    if (text == "/alarm_off") { digitalWrite(LED_ALARM, LOW);  bot.sendMessage(CHAT_ID, "🚨 Alarm OFF", ""); }
    if (text == "/auto_on")   { autoLightControl = true; bot.sendMessage(CHAT_ID, "☀ Auto Light Mode ON", ""); }
    if (text == "/auto_off")  { autoLightControl = false; bot.sendMessage(CHAT_ID, "☀ Auto Light Mode OFF", ""); }

    // Water pump manual and auto control
    if (text == "/pump_on")  { digitalWrite(WATER_PUMP_PIN, HIGH); autoWaterPumpControl = false; bot.sendMessage(CHAT_ID, "💧 Irrigation ON", ""); }
    if (text == "/pump_off") { digitalWrite(WATER_PUMP_PIN, LOW);  autoWaterPumpControl = false; bot.sendMessage(CHAT_ID, "💧 Irrigation OFF", ""); }
    if (text == "/auto_pump_on")  { autoWaterPumpControl = true;  bot.sendMessage(CHAT_ID, "☀ Auto Irrigation ON", ""); }
    if (text == "/auto_pump_off") { autoWaterPumpControl = false; bot.sendMessage(CHAT_ID, "☀ Auto Irrigation OFF", ""); }
    if (text == "/pump_status") {
      int soil = getSmoothADC(SOIL_MOISTURE_PIN);
      int pump = digitalRead(WATER_PUMP_PIN);
      bot.sendMessage(CHAT_ID, "💧 Pump Status:\nSoil: " + String(soil) + "\nIrrigation is " + (pump ? "ON" : "OFF"), "");
    }

    // Exhaust manual control
    if (text == "/exhaust_on")  { digitalWrite(EXHAUST_FAN_PIN, HIGH); bot.sendMessage(CHAT_ID, "🌬 Exhaust ON", ""); }
    if (text == "/exhaust_off") { digitalWrite(EXHAUST_FAN_PIN, LOW);  bot.sendMessage(CHAT_ID, "🌬 Exhaust OFF", ""); }

    if (text == "/status") { sendTelegramStatus(); }
  }
}


// ======================== SinricPro Callbacks ================
bool onPowerStateLight(const String &deviceId, bool &state) {
  autoLightControl = false;
  digitalWrite(LED_LIGHT, state ? HIGH : LOW);
  return true;
}
bool onPowerStateFan(const String &deviceId, bool &state) {
  digitalWrite(LED_FAN, state ? HIGH : LOW);
  return true;
}
bool onPowerStateAlarm(const String &deviceId, bool &state) {
  digitalWrite(LED_ALARM, state ? HIGH : LOW);
  return true;
}


// ======================== Weather Fetch ======================
void fetchWeather() {
  if (strlen(OPENWEATHER_API_KEY) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String("https://api.openweathermap.org/data/2.5/weather?lat=") + HOME_LAT + "&lon=" + HOME_LON + "&appid=" + OPENWEATHER_API_KEY + "&units=metric";

  // Use insecure TLS to avoid managing CA certs on ESP32 for hackathon speed
  WiFiClientSecure client;
  client.setInsecure();

  if (!http.begin(client, url)) {
    Serial.println("[Weather] HTTP begin failed");
    return;
  }

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    int it = payload.indexOf("\"temp\":");
    int ih = payload.indexOf("\"humidity\":");
    int iw = payload.indexOf("\"weather\"");
    if (it != -1) {
      int comma = payload.indexOf(",", it+7);
      outsideTemp = payload.substring(it+7, comma).toFloat();
    }
    if (ih != -1) {
      int comma = payload.indexOf(",", ih+11);
      outsideHum = payload.substring(ih+11, comma).toFloat();
    }
    if (iw != -1) {
      int mainIdx = payload.indexOf("\"main\":", iw);
      if (mainIdx != -1) {
        int q1 = payload.indexOf("\"", mainIdx+7);
        int q2 = payload.indexOf("\"", q1+1);
        outsideCond = payload.substring(q1+1, q2);
      }
    }
    Serial.printf("[Weather] %.1f C, %.0f%%, %s\n", outsideTemp, outsideHum, outsideCond.c_str());
  } else {
    Serial.printf("[Weather] GET failed, code %d\n", code);
  }
  http.end();
}


// ======================== HTTP endpoints ====================
String buildAISuggestion(bool fanState, bool occupied) {
  if (fanState && !occupied) {
    if (!fanWasOn) {
      fanOnStart = millis();
      fanWasOn = true;
    }
    if ((millis() - fanOnStart) > FAN_UNUSED_LIMIT_MS) {
      if (!suggestionSent) {
        bot.sendMessage(CHAT_ID, "💡 Suggestion: Switch off Fan — ON for 2 hrs without occupancy.", "");
        suggestionSent = true;
      }
      return String("💡 Switch off Fan — ON for 2 hrs without occupancy.");
    }
  } else {
    fanWasOn = fanState;
    fanOnStart = fanState ? fanOnStart : 0;
    suggestionSent = false;
  }
  return String("");
}


void handleData() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int gas = getSmoothADC(MQ6_PIN);
  bool gas_alert = (gas > MQ6_ALERT_THRESHOLD);
  bool motionDetected = PIR_ACTIVE_LOW ? !digitalRead(PIR_PIN) : digitalRead(PIR_PIN);
  int ldrValue = getSmoothADC(LDR_PIN);
  int soilMoistureValue = getSmoothADC(SOIL_MOISTURE_PIN);

  // Auto light control
  if(autoLightControl){
    if(ldrValue > LDR_THRESHOLD || motionDetected) digitalWrite(LED_LIGHT, HIGH);
    else digitalWrite(LED_LIGHT, LOW);
  }

  // Auto water pump control
  if(autoWaterPumpControl){
    if(soilMoistureValue < SOIL_MOISTURE_THRESHOLD) digitalWrite(WATER_PUMP_PIN, HIGH);
    else digitalWrite(WATER_PUMP_PIN, LOW);
  }

  // Auto exhaust on gas alert
  if (gas_alert) {
    digitalWrite(EXHAUST_FAN_PIN, HIGH);
    if (!gasAlertActive) {
      gasAlertActive = true;
      bot.sendMessage(CHAT_ID, "⚠ Gas Alert! Exhaust Fan turned ON automatically.", "");
    }
  } else {
    digitalWrite(EXHAUST_FAN_PIN, LOW);
    gasAlertActive = false;
  }

  // AI Suggestion
  bool fanState = digitalRead(LED_FAN);
  String aiSugg = buildAISuggestion(fanState, motionDetected);

  if (millis() - lastWeatherFetch > WEATHER_FETCH_INTERVAL) {
    lastWeatherFetch = millis();
    fetchWeather();
  }

  String json = "{";
  json += "\"temp\":" + String(t,1) + ",";
  json += "\"hum\":"  + String(h,1) + ",";
  json += "\"gas\":"  + String(gas) + ",";
  json += "\"gas_alert\":" + String(gas_alert ? 1 : 0) + ",";
  json += "\"motion\":" + String(motionDetected ? 1 : 0) + ",";
  json += "\"ldr\":" + String(ldrValue) + ",";
  json += "\"autoLight\":" + String(autoLightControl ? 1 : 0) + ",";
  json += "\"ledLight\":" + String(digitalRead(LED_LIGHT) ? 1 : 0) + ",";
  json += "\"ledFan\":"   + String(digitalRead(LED_FAN) ? 1 : 0) + ",";
  json += "\"ledAlarm\":" + String(digitalRead(LED_ALARM) ? 1 : 0) + ",";
  json += "\"soilMoisture\":" + String(soilMoistureValue) + ",";
  json += "\"pumpOn\":" + String(digitalRead(WATER_PUMP_PIN) ? 1 : 0) + ",";
  json += "\"autoPump\":" + String(autoWaterPumpControl ? 1 : 0) + ",";
  json += "\"exhaustOn\":" + String(digitalRead(EXHAUST_FAN_PIN) ? 1 : 0) + ",";
  if (isnan(outsideTemp)) json += "\"outTemp\":null,"; else json += "\"outTemp\":" + String(outsideTemp,1) + ",";
  if (isnan(outsideHum))  json += "\"outHum\":null,";  else json += "\"outHum\":"  + String(outsideHum,0) + ",";
  json += "\"outCond\":\"" + outsideCond + "\",";
  json += "\"aiSuggest\":\"" + aiSugg + "\"";
  json += "}";
  server.send(200, "application/json", json);
}


void handleToggle() {
  if (!server.hasArg("dev")) { server.send(400, "text/plain", "missing dev"); return; }
  String dev = server.arg("dev");
  if (dev == "light") { autoLightControl = false; digitalWrite(LED_LIGHT, !digitalRead(LED_LIGHT)); }
  else if (dev == "fan") digitalWrite(LED_FAN, !digitalRead(LED_FAN));
  else if (dev == "alarm") digitalWrite(LED_ALARM, !digitalRead(LED_ALARM));
  else if (dev == "auto") autoLightControl = !autoLightControl;
  else if (dev == "pump") { 
    autoWaterPumpControl = false; 
    digitalWrite(WATER_PUMP_PIN, !digitalRead(WATER_PUMP_PIN)); 
  }
  else if (dev == "autoPump") autoWaterPumpControl = !autoWaterPumpControl;
  else if (dev == "exhaust") {
    digitalWrite(EXHAUST_FAN_PIN, !digitalRead(EXHAUST_FAN_PIN));
  }
  server.send(200, "text/plain", "OK");
}


// ======================== Setup ============================
void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(PIR_PIN, INPUT);
  pinMode(LED_LIGHT, OUTPUT);
  pinMode(LED_FAN, OUTPUT);
  pinMode(LED_ALARM, OUTPUT);
  pinMode(SOIL_MOISTURE_PIN, INPUT);
  pinMode(WATER_PUMP_PIN, OUTPUT);
  pinMode(EXHAUST_FAN_PIN, OUTPUT);
  digitalWrite(WATER_PUMP_PIN, LOW);
  digitalWrite(EXHAUST_FAN_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  secured_client.setInsecure();

  server.on("/", [](){ server.send(200, "text/html", htmlPage); });
  server.on("/data", handleData);
  server.on("/toggle", handleToggle);
  server.begin();

  SinricPro.onConnected([](){ Serial.println("Connected to SinricPro!"); });
  SinricPro.onDisconnected([](){ Serial.println("Disconnected from SinricPro"); });
  lightSwitch.onPowerState(onPowerStateLight);
  fanSwitch.onPowerState(onPowerStateFan);
  alarmSwitch.onPowerState(onPowerStateAlarm);
  SinricPro.begin(APP_KEY, APP_SECRET);

  fetchWeather();
  lastWeatherFetch = millis();
}


// ======================== Loop =============================
void loop() {
  server.handleClient();
  SinricPro.handle();

  if (millis() - lastTelegramCheck > TELEGRAM_CHECK_INTERVAL) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while(numNewMessages) {
      handleTelegramMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTelegramCheck = millis();
  }
}
