#define BLYNK_PRINT Serial
#include "secrets.h"

// =====================================================
// BLYNK CONFIG
// =====================================================
#define BLYNK_TEMPLATE_ID   "TMPL6lPALoZqD"
#define BLYNK_TEMPLATE_NAME "Smart Home"
#define BLYNK_AUTH_TOKEN    SECRET_BLYNK_TOKEN

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <BlynkSimpleEsp8266.h>
#include <DHTesp.h>

// =====================================================
// WIFI CONFIG
// =====================================================
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;

// =====================================================
// HIVEMQ CONFIG
// =====================================================
const char* MQTT_BROKER   = "ecb95cb280fb409fa5b900f28b8bb752.s1.eu.hivemq.cloud";
const int   MQTT_PORT     = 8883;
const char* MQTT_USERNAME = SECRET_MQTT_USERNAME;
const char* MQTT_PASSWORD = SECRET_MQTT_PASSWORD;
const char* MQTT_TOPIC_AI = "esp32-cam";

// =====================================================
// GPIO CONFIG - NodeMCU ESP8266
// =====================================================
#define IR_RX_PIN       5    // D1 = GPIO5  - OUT của IR receiver 38kHz
#define IR_TX_PIN       4    // D2 = GPIO4  - IR transmitter
#define LED_RELAY_PIN   14   // D5 = GPIO14 - Relay đèn LED
#define FAN_RELAY_PIN   13   // D7 = GPIO13 - Relay quạt

#define DHTPIN          0   // D3 = GPIO0  - Chân DHT11 đặt tại D3
#define DHTTYPE         DHTesp::DHT11
#define MQ2_PIN         A0   // A0 - MQ2 analog output

// =====================================================
// RELAY LOGIC
// =====================================================
#define RELAY_ON    HIGH
#define RELAY_OFF   LOW

#define IR_DETECT_LEVEL LOW
#define IR_IDLE_LEVEL   HIGH

// =====================================================
// IR VERIFY CONFIG
// =====================================================
const int IR_BURST_COUNT            = 100;
const unsigned int IR_BURST_ON_US   = 560;
const unsigned int IR_BURST_OFF_US  = 560;
const unsigned int IR_SAMPLE_DELAY_US = 50;
const int IR_MIN_HIT_COUNT          = 5;
const int IR_IDLE_CHECK_SAMPLES     = 20;
const int IR_IDLE_MAX_DETECT        = 2;

// =====================================================
// TIME CONFIG
// =====================================================
const unsigned long AUTO_OFF_DELAY_MS   = 10000;
const unsigned long AUTO_IR_RETRY_MS    = 5000;

// =====================================================
// SENSOR CONFIG
// =====================================================
const int GAS_THRESHOLD = 80;

// =====================================================
// GLOBAL STATES
// =====================================================
bool humanDetected  = false;
bool ledState       = false;
bool fanState       = false;
bool autoMode       = false;
bool irBusy         = false;
bool gasAlertSent   = false;

unsigned long lastHumanDetectedTime     = 0;
unsigned long lastHumanLostTime         = 0;
unsigned long lastAutoOnAttempt         = 0;
unsigned long lastAutoOffAttempt        = 0;
unsigned long lastMqttReconnectAttempt  = 0;
unsigned long lastBlynkReconnectAttempt = 0;

int gasBaseline = 0;

// =====================================================
// OBJECTS
// =====================================================
DHTesp dht;
WiFiClient blynkClient;               
WiFiClientSecure secureMqttClient;    
PubSubClient mqttClient(secureMqttClient);
BlynkTimer timer;

// =====================================================
// ===             RELAY FUNCTIONS                   ===
// =====================================================

void forceRelayOutputs() {
  digitalWrite(LED_RELAY_PIN, ledState ? RELAY_ON : RELAY_OFF);
  digitalWrite(FAN_RELAY_PIN, fanState ? RELAY_ON : RELAY_OFF);
}

void updateAllStatus() {
  forceRelayOutputs();
  if (!Blynk.connected()) return;

  Blynk.virtualWrite(V0, humanDetected ? 1 : 0);
  Blynk.virtualWrite(V1, ledState ? 1 : 0);
  Blynk.virtualWrite(V2, fanState ? 1 : 0);
  Blynk.virtualWrite(V3, autoMode ? 1 : 0);
}

void setLedRaw(bool on) {
  ledState = on;
  digitalWrite(LED_RELAY_PIN, ledState ? RELAY_ON : RELAY_OFF);
  Serial.print("[LED RELAY] ");
  Serial.println(ledState ? "ON" : "OFF");
  if (Blynk.connected()) Blynk.virtualWrite(V1, ledState ? 1 : 0);
}

void setFanRaw(bool on) {
  fanState = on;
  digitalWrite(FAN_RELAY_PIN, fanState ? RELAY_ON : RELAY_OFF);
  Serial.print("[FAN RELAY] ");
  Serial.println(fanState ? "ON" : "OFF");
  if (Blynk.connected()) Blynk.virtualWrite(V2, fanState ? 1 : 0);
}

// =====================================================
// ===              IR FUNCTIONS                     ===
// =====================================================

void irStopCarrier() {
  digitalWrite(IR_TX_PIN, LOW);
}

int readIR() {
  return digitalRead(IR_RX_PIN);
}

bool irReceiverIdleOK() {
  int detectCount = 0;
  for (int i = 0; i < IR_IDLE_CHECK_SAMPLES; i++) {
    if (readIR() == IR_DETECT_LEVEL) detectCount++;
    delayMicroseconds(200);
    yield();
  }

  Serial.print("[IR] Idle check detectCount=");
  Serial.println(detectCount);

  if (detectCount > IR_IDLE_MAX_DETECT) {
    Serial.println("[IR] Receiver not idle. Possible noise.");
    return false;
  }
  return true;
}

void irCarrierOnAndSample(unsigned int durationUs, int &hitCount, int &sampleCount) {
  unsigned long start = micros();
  while ((unsigned long)(micros() - start) < durationUs) {
    digitalWrite(IR_TX_PIN, HIGH);
    delayMicroseconds(13);
    if (readIR() == IR_DETECT_LEVEL) hitCount++;
    sampleCount++;

    digitalWrite(IR_TX_PIN, LOW);
    delayMicroseconds(13);
    if (readIR() == IR_DETECT_LEVEL) hitCount++;
    sampleCount++;
  }
}

void irCarrierOffAndSample(unsigned int durationUs, int &hitCount, int &sampleCount) {
  irStopCarrier();
  unsigned long start = micros();
  while ((unsigned long)(micros() - start) < durationUs) {
    if (readIR() == IR_DETECT_LEVEL) hitCount++;
    sampleCount++;
    delayMicroseconds(IR_SAMPLE_DELAY_US);
  }
}

bool sendIRAndDetect() {
  if (irBusy) {
    Serial.println("[IR] Busy -> skip");
    return false;
  }

  irBusy = true;
  Serial.println("=================================");
  Serial.println("[IR] Start verification");

  irStopCarrier();
  delay(5);

  if (!irReceiverIdleOK()) {
    irStopCarrier();
    irBusy = false;
    Serial.println("[IR] VERIFY FAIL: receiver not idle");
    Serial.println("=================================");
    return false;
  }

  int hitCount   = 0;
  int sampleCount = 0;

  Serial.println("[IR] Sending 38kHz burst...");

  for (int burst = 0; burst < IR_BURST_COUNT; burst++) {
    irCarrierOnAndSample(IR_BURST_ON_US, hitCount, sampleCount);
    irCarrierOffAndSample(IR_BURST_OFF_US, hitCount, sampleCount);
    yield();

    if (hitCount >= IR_MIN_HIT_COUNT) {
      irStopCarrier();
      irBusy = false;
      Serial.print("[IR] DETECTED | hitCount=");
      Serial.print(hitCount);
      Serial.println(" | VERIFY OK");
      Serial.println("=================================");
      return true;
    }
  }

  irStopCarrier();
  irBusy = false;
  Serial.println("[IR] VERIFY FAIL: No response");
  Serial.println("=================================");
  return false;
}

// =====================================================
// ===          IR VERIFIED RELAY CONTROL            ===
// =====================================================

void applyLedWithIR(bool targetState, const char* source) {
  Serial.print("["); Serial.print(source); Serial.print("] LED target = ");
  Serial.println(targetState ? "ON" : "OFF");

  bool irOK = sendIRAndDetect();
  if (irOK) {
    setLedRaw(targetState);
  } else {
    Serial.println("[LED] IR FAIL -> relay locked");
    updateAllStatus();
  }
}

void applyFanWithIR(bool targetState, const char* source) {
  Serial.print("["); Serial.print(source); Serial.print("] FAN target = ");
  Serial.println(targetState ? "ON" : "OFF");

  bool irOK = sendIRAndDetect();
  if (irOK) {
    setFanRaw(targetState);
  } else {
    Serial.println("[FAN] IR FAIL -> relay locked");
    updateAllStatus();
  }
}

void applyBothWithIR(bool ledTarget, bool fanTarget, const char* source) {
  bool irOK = sendIRAndDetect();
  if (irOK) {
    setLedRaw(ledTarget);
    setFanRaw(fanTarget);
  } else {
    Serial.println("[AUTO] IR FAIL -> Relays locked");
    updateAllStatus();
  }
}

// =====================================================
// ===              AUTO CONTROL                     ===
// =====================================================

void autoTurnOnIfNeeded() {
  if (!autoMode || !humanDetected) return;
  if (ledState && fanState) return;

  unsigned long now = millis();
  if (now - lastAutoOnAttempt < AUTO_IR_RETRY_MS) return;
  lastAutoOnAttempt = now;

  applyBothWithIR(true, true, "AUTO_ON");
}

void autoOffTask() {
  if (!autoMode || humanDetected) return;
  if (!ledState && !fanState) return;

  unsigned long now = millis();
  if (now - lastHumanLostTime < AUTO_OFF_DELAY_MS) return;
  if (now - lastAutoOffAttempt < AUTO_IR_RETRY_MS) return;

  lastAutoOffAttempt = now;
  applyBothWithIR(false, false, "AUTO_OFF");
}

void autoOnTask() {
  autoTurnOnIfNeeded();
}

// =====================================================
// ===             SENSOR FUNCTIONS                  ===
// =====================================================

int readGasAverage() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(MQ2_PIN);
    delay(5);
  }
  return sum / 10;
}

void sendSensorToBlynk(float temp, float humi, int gasValue, int delta, bool gasDetected) {
  if (!Blynk.connected()) return;

  Blynk.virtualWrite(V4, temp);
  Blynk.virtualWrite(V5, humi);
  
  delay(20); 

  Blynk.virtualWrite(V6, gasValue);
  Blynk.virtualWrite(V7, gasDetected ? 1 : 0);
  Blynk.virtualWrite(V8, gasBaseline);
  Blynk.virtualWrite(V9, delta);
  
  Serial.println("[BLYNK] Đã gửi thông số cảm biến lên Cloud.");
}

void sensorTask() {
  int gasValue     = readGasAverage();
  int delta        = gasValue - gasBaseline;
  bool gasDetected = (delta >= GAS_THRESHOLD);
  
  if (gasDetected) {
    if (!gasAlertSent && Blynk.connected()) {
      Blynk.logEvent("gas_alert", "Cảnh báo nguy hiểm: Phát hiện rò rỉ khí Gas ở mức cao!");
      gasAlertSent = true; 
    }
  } else {
    gasAlertSent = false; 
  }
  
  TempAndHumidity data = dht.getTempAndHumidity();

  Serial.print("[SENSOR] GasRaw=");  Serial.print(gasValue);
  Serial.print(" | Baseline=");     Serial.print(gasBaseline);
  Serial.print(" | Delta=");        Serial.print(delta);

  if (dht.getStatus() != DHTesp::ERROR_NONE) {
    Serial.print(" | DHT11 Error: ");
    Serial.println(dht.getStatusString());
    sendSensorToBlynk(0.0, 0.0, gasValue, delta, gasDetected);
  } else {
    Serial.print(" | Temp=");       Serial.print(data.temperature);
    Serial.print("C | Humi=");      Serial.print(data.humidity);
    Serial.println("%");

    sendSensorToBlynk(data.temperature, data.humidity, gasValue, delta, gasDetected);
  }
}

void calibrateMQ2() {
  Serial.println("[MQ2] Warming up 30 seconds...");
  for (int i = 30; i > 0; i--) {
    Serial.print(i); Serial.print(".. ");
    delay(1000);
    ESP.wdtFeed(); 
  }
  Serial.println();

  long sum = 0;
  for (int i = 0; i < 50; i++) {
    sum += readGasAverage();
    delay(50);
  }
  gasBaseline = sum / 50;
  Serial.print("[MQ2] Calibrated Baseline: "); Serial.println(gasBaseline);
}

// =====================================================
// ===              MQTT CALLBACK                    ===
// =====================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (msg == "HUMAN_DETECTED" || msg == "1") {
    humanDetected = true;
    lastHumanDetectedTime = millis();
    if (Blynk.connected()) Blynk.virtualWrite(V0, 1);
    autoTurnOnIfNeeded();
  }
  else if (msg == "HUMAN_NO_DETECTED" || msg == "0") {
    if (humanDetected) { // Chỉ xử lý khi trạng thái chuyển từ "Có người" -> "Không người"
      lastHumanLostTime = millis();
      humanDetected = false;
      
      if (Blynk.connected()) {
        Blynk.virtualWrite(V0, 0);
        
        if (ledState == true || fanState == true) {
          Blynk.logEvent("device_left_on", "Cảnh báo: Không có người trong phòng nhưng thiết bị vẫn đang bật!");
        }
      }
    }
  }
}

// =====================================================
// ===             BLYNK HANDLERS                    ===
// =====================================================

BLYNK_CONNECTED() {
  Serial.println("[BLYNK] Connected successfully!");
  updateAllStatus();
}

BLYNK_WRITE(V1) {
  int value = param.asInt();
  if (autoMode) {
    autoMode = false;
    if (Blynk.connected()) Blynk.virtualWrite(V3, 0);
  }
  applyLedWithIR(value == 1, "MANUAL_LED");
}

BLYNK_WRITE(V2) {
  int value = param.asInt();
  if (autoMode) {
    autoMode = false;
    if (Blynk.connected()) Blynk.virtualWrite(V3, 0);
  }
  applyFanWithIR(value == 1, "MANUAL_FAN");
}

BLYNK_WRITE(V3) {
  autoMode = (param.asInt() == 1);
  if (autoMode) {
    autoTurnOnIfNeeded();
    autoOffTask();
  }
  updateAllStatus();
}

// =====================================================
// ===              NETWORK FUNCTIONS                ===
// =====================================================

void connectWiFi() {
  Serial.print("[WiFi] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected!");
}

bool connectMQTT() {
  if (mqttClient.connected()) return true;
  String clientId = "ESP8266-" + String(ESP.getChipId());
  if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("[MQTT] Connected to HiveMQ Cloud");
    mqttClient.subscribe(MQTT_TOPIC_AI);
  }
  return mqttClient.connected();
}

// =====================================================
// ===                 TIMER TASKS                   ===
// =====================================================

void heartbeatTask() {
  forceRelayOutputs();
  Serial.print("[HEARTBEAT] WiFi="); Serial.print(WiFi.status() == WL_CONNECTED ? "OK" : "LOST");
  Serial.print(" | MQTT="); Serial.print(mqttClient.connected() ? "OK" : "LOST");
  Serial.print(" | Blynk="); Serial.println(Blynk.connected() ? "OK" : "LOST");
}

void reconnectTask() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return;
  }
  unsigned long now = millis();
  if (!mqttClient.connected() && (now - lastMqttReconnectAttempt > 5000)) {
    lastMqttReconnectAttempt = now;
    connectMQTT();
  }
  if (!Blynk.connected() && (now - lastBlynkReconnectAttempt > 8000)) {
    lastBlynkReconnectAttempt = now;
    Blynk.connect();
  }
}

// =====================================================
// ===                     SETUP                     ===
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(IR_TX_PIN, OUTPUT);
  pinMode(IR_RX_PIN, INPUT_PULLUP);
  irStopCarrier();

  pinMode(LED_RELAY_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  forceRelayOutputs();

  dht.setup(DHTPIN, DHTesp::DHT11);
  Serial.println("[DHT11] Configured on D3 (GPIO0)");

  calibrateMQ2();
  connectWiFi();

  secureMqttClient.setInsecure();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  connectMQTT();

  Blynk.config(BLYNK_AUTH_TOKEN, "blynk.cloud", 80);
  Blynk.connect();

  timer.setInterval(4000L, heartbeatTask);
  timer.setInterval(5000L, reconnectTask);
  timer.setInterval(1000L, autoOnTask);
  timer.setInterval(1000L, autoOffTask);
  timer.setInterval(3000L, sensorTask); 
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (mqttClient.connected()) mqttClient.loop();
    if (Blynk.connected()) Blynk.run();
  }
  timer.run();
}