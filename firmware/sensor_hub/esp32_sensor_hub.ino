/*
  Sensor Hub - Smart Energy System (Updated for Dashboard v2)
  
  This hub reads sensors for BOTH Room A and Room B, then publishes
  telemetry to separate MQTT topics for each room.
  
  Pins: 
    HC-SR04 A (21/34), B (22/35)
    DHT11 A (26), B (25)
    Mode Button (14)
    NRF24L01+: CE=4, CSN=5
  
  MQTT Topics Published:
    sensors/room1/telemetry  - Room A data
    sensors/room2/telemetry  - Room B data
  
  MQTT Topics Subscribed:
    control/mode             - Mode changes from dashboard
*/

#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ============ CONFIGURATION - UPDATE THESE ============
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* PRICING_API_URL = "https://smart-energy-production-a08d.up.railway.app/api/price/esp32";

// MQTT (HiveMQ Cloud)
const char* MQTT_SERVER = "1e1a4e5c581e4bc3a697f8937d7fb9e4.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "omeravi";
const char* MQTT_PASSWORD = "Omeromer1!";

// ============ MQTT TOPICS ============
const char* TOPIC_TELEMETRY_A = "sensors/room1/telemetry";
const char* TOPIC_TELEMETRY_B = "sensors/room2/telemetry";
const char* TOPIC_MODE = "control/mode";
const char* TOPIC_ALERTS_A = "alerts/room1/anomaly";
const char* TOPIC_ALERTS_B = "alerts/room2/anomaly";

// ============ PIN DEFINITIONS ============
const int TRIG_A = 21, ECHO_A = 34;
const int TRIG_B = 22, ECHO_B = 35;
const int DHT_PIN_A = 26, DHT_PIN_B = 25;
const int MODE_BUTTON = 14;
const int STATUS_LED = 2;
#define NRF_CE 4
#define NRF_CSN 5

// ============ OBJECTS ============
RF24 radio(NRF_CE, NRF_CSN);
DHT dhtA(DHT_PIN_A, DHT11);
DHT dhtB(DHT_PIN_B, DHT11);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
const byte PIPE_TX[6] = "SENS1";

// ============ TELEMETRY STRUCT (for NRF to Control Hub) ============
struct Telemetry {
  uint32_t ms;
  int16_t distA_cm, distB_cm;
  int16_t tempA_x10, tempB_x10;
  uint8_t humidA, humidB;
  uint8_t ecoMode;
  uint8_t priceTier;
  int8_t tempOffset;
};

// ============ PRICING DATA ============
struct PricingData {
  float price;
  uint8_t tier;
  String action;
  int8_t tempOffset;
  bool valid;
  unsigned long lastUpdate;
};

// ============ STATE VARIABLES ============
bool ecoMode = true;
bool lastButtonState = HIGH;
unsigned long lastButtonPress = 0;
PricingData currentPricing = {0, 2, "normal", 0, false, 0};

// Timing
unsigned long lastPriceFetch = 0;
unsigned long lastNrfSend = 0;
unsigned long lastMqttPublish = 0;
unsigned long lastDHTread = 0;

// Sensor data
float tempA = 0, tempB = 0, humidA = 0, humidB = 0;
int16_t distA = -1, distB = -1;

// Device states (received from Control Hub or inferred)
bool fanA = false, lampA = false;
bool fanB = false, lampB = false;

// ============ TIMING CONSTANTS ============
const unsigned long DEBOUNCE_MS = 200;
const unsigned long PRICE_FETCH_INTERVAL = 300000;  // 5 minutes
const unsigned long NRF_SEND_INTERVAL = 250;        // 250ms
const unsigned long MQTT_PUBLISH_INTERVAL = 5000;   // 5 seconds
const unsigned long PULSE_TIMEOUT_US = 30000;
const int MIN_CM = 2, MAX_CM = 400;
const int OCCUPANCY_THRESHOLD = 150;  // cm - closer than this = occupied

// ============ DISTANCE READING ============
int16_t readSingleDistance(int trig, int echo) {
  digitalWrite(trig, LOW); delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long us = pulseIn(echo, HIGH, PULSE_TIMEOUT_US);
  if (us == 0) return -1;
  long cm = us / 58;
  return (cm < MIN_CM || cm > MAX_CM) ? -1 : (int16_t)cm;
}

int16_t readDistanceCM(int trig, int echo) {
  long readings[3];
  for (int i = 0; i < 3; i++) {
    long cm = readSingleDistance(trig, echo);
    readings[i] = (cm < 0) ? 10000 : cm;
    delay(5);
  }
  // Sort for median
  for (int i = 0; i < 2; i++)
    for (int j = i + 1; j < 3; j++)
      if (readings[i] > readings[j]) { long t = readings[i]; readings[i] = readings[j]; readings[j] = t; }
  return (readings[1] >= 10000) ? -1 : (int16_t)readings[1];
}

// ============ TEMPERATURE READING ============
void readTemperatures() {
  float tA = dhtA.readTemperature();
  float hA = dhtA.readHumidity();
  float tB = dhtB.readTemperature();
  float hB = dhtB.readHumidity();
  
  if (!isnan(tA) && tA > -20 && tA < 80) tempA = tA;
  if (!isnan(hA) && hA >= 0 && hA <= 100) humidA = hA;
  if (!isnan(tB) && tB > -20 && tB < 80) tempB = tB;
  if (!isnan(hB) && hB >= 0 && hB <= 100) humidB = hB;
}

// ============ MODE BUTTON ============
void checkModeButton() {
  bool current = digitalRead(MODE_BUTTON);
  if (current == LOW && lastButtonState == HIGH && millis() - lastButtonPress > DEBOUNCE_MS) {
    ecoMode = !ecoMode;
    lastButtonPress = millis();
    Serial.printf("\n*** MODE CHANGED: %s ***\n\n", ecoMode ? "ECO" : "MANUAL");
    
    // Visual feedback - 3 blinks
    for (int i = 0; i < 3; i++) {
      digitalWrite(STATUS_LED, HIGH); delay(100);
      digitalWrite(STATUS_LED, LOW); delay(100);
    }
    
    // Publish mode change to MQTT
    publishModeChange();
  }
  lastButtonState = current;
}

// ============ MQTT MODE PUBLISH ============
void publishModeChange() {
  if (!mqttClient.connected()) return;
  
  StaticJsonDocument<64> doc;
  doc["mode"] = ecoMode ? "eco" : "manual";
  doc["ecoMode"] = ecoMode ? 1 : 0;
  doc["timestamp"] = millis();
  
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(TOPIC_MODE, payload.c_str(), true);  // retained
  
  Serial.printf("Published mode: %s\n", ecoMode ? "eco" : "manual");
}

// ============ MQTT CALLBACK ============
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) return;
  
  String topicStr = String(topic);
  
  // Handle mode changes from dashboard
  if (topicStr == TOPIC_MODE) {
    String newMode = doc["mode"].as<String>();
    bool newEcoMode = (newMode == "eco");
    
    if (newEcoMode != ecoMode) {
      ecoMode = newEcoMode;
      Serial.printf("\n*** MODE FROM DASHBOARD: %s ***\n\n", ecoMode ? "ECO" : "MANUAL");
      
      // Blink to indicate
      digitalWrite(STATUS_LED, HIGH); delay(200);
      digitalWrite(STATUS_LED, LOW);
    }
  }
}

// ============ PRICING TIER CONVERSION ============
uint8_t tierStringToNum(const String& tier) {
  if (tier == "very_low") return 0;
  if (tier == "low") return 1;
  if (tier == "normal") return 2;
  if (tier == "high") return 3;
  if (tier == "very_high") return 4;
  if (tier == "critical") return 5;
  return 2;  // default to normal
}

// ============ FETCH PRICING DATA ============
void fetchComedPricing() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  http.begin(PRICING_API_URL);
  http.setTimeout(10000);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, http.getString())) {
      currentPricing.price = doc["p"].as<float>();
      currentPricing.tier = tierStringToNum(doc["t"].as<String>());
      currentPricing.action = doc["a"].as<String>();
      currentPricing.tempOffset = doc["o"].as<int8_t>();
      currentPricing.valid = (doc["s"].as<String>() == "active");
      currentPricing.lastUpdate = millis();
      
      Serial.printf("Price: %.2f¢/kWh, Tier: %d (%s)\n", 
        currentPricing.price, currentPricing.tier, currentPricing.action.c_str());
    }
  } else {
    Serial.printf("Pricing fetch failed: %d\n", httpCode);
  }
  http.end();
}

// ============ SEND TELEMETRY VIA NRF24L01+ ============
void sendTelemetryNRF() {
  Telemetry t = {
    millis(),
    distA, distB,
    (int16_t)(tempA * 10), (int16_t)(tempB * 10),
    (uint8_t)constrain(humidA, 0, 100), (uint8_t)constrain(humidB, 0, 100),
    ecoMode ? (uint8_t)1 : (uint8_t)0,
    currentPricing.tier,
    currentPricing.tempOffset
  };
  
  bool ok = radio.write(&t, sizeof(t));
  Serial.printf("NRF TX: A=%dcm B=%dcm eco=%d tier=%d -> %s\n", 
    distA, distB, ecoMode, currentPricing.tier, ok ? "OK" : "FAIL");
}

// ============ MQTT CONNECTION ============
void connectMQTT() {
  if (mqttClient.connected()) return;
  
  String clientId = "SensorHub-" + String(random(0xffff), HEX);
  Serial.print("Connecting to MQTT...");
  
  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println(" OK");
    
    // Subscribe to mode topic
    mqttClient.subscribe(TOPIC_MODE);
    Serial.println("Subscribed to mode topic");
    
    // Publish current mode
    publishModeChange();
  } else {
    Serial.printf(" FAILED (rc=%d)\n", mqttClient.state());
  }
}

// ============ PUBLISH ROOM TELEMETRY ============
void publishMQTT() {
  if (!mqttClient.connected()) {
    connectMQTT();
    if (!mqttClient.connected()) return;
  }
  
  bool occA = (distA > 0 && distA < OCCUPANCY_THRESHOLD);
  bool occB = (distB > 0 && distB < OCCUPANCY_THRESHOLD);
  
  // Calculate power based on occupancy (estimate)
  float ampsA = 0.42 + (occA ? 0.5 : 0);
  float ampsB = 0.42 + (occB ? 0.5 : 0);
  
  // === ROOM A TELEMETRY ===
  StaticJsonDocument<256> docA;
  docA["ts"] = millis();
  docA["tC"] = tempA;
  docA["rh"] = humidA;
  docA["dist"] = distA;
  docA["pir"] = occA ? 1 : 0;
  docA["voltage"] = 120;
  docA["amps"] = ampsA;
  docA["fan"] = fanA;
  docA["lamp"] = lampA;
  docA["ecoMode"] = ecoMode ? 1 : 0;
  docA["priceTier"] = currentPricing.tier;
  docA["price"] = currentPricing.price;
  
  String payloadA;
  serializeJson(docA, payloadA);
  mqttClient.publish(TOPIC_TELEMETRY_A, payloadA.c_str());
  
  // === ROOM B TELEMETRY ===
  StaticJsonDocument<256> docB;
  docB["ts"] = millis();
  docB["tC"] = tempB;
  docB["rh"] = humidB;
  docB["dist"] = distB;
  docB["pir"] = occB ? 1 : 0;
  docB["voltage"] = 120;
  docB["amps"] = ampsB;
  docB["fan"] = fanB;
  docB["lamp"] = lampB;
  docB["ecoMode"] = ecoMode ? 1 : 0;
  docB["priceTier"] = currentPricing.tier;
  docB["price"] = currentPricing.price;
  
  String payloadB;
  serializeJson(docB, payloadB);
  mqttClient.publish(TOPIC_TELEMETRY_B, payloadB.c_str());
  
  Serial.printf("MQTT: RoomA[%.1fC %d%%] RoomB[%.1fC %d%%] eco=%d\n",
    tempA, (int)humidA, tempB, (int)humidB, ecoMode);
}

// ============ WIFI CONNECTION ============
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts++ < 30) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" FAILED");
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Sensor Hub Starting ===");
  
  // Pin setup
  pinMode(TRIG_A, OUTPUT); pinMode(ECHO_A, INPUT);
  pinMode(TRIG_B, OUTPUT); pinMode(ECHO_B, INPUT);
  pinMode(MODE_BUTTON, INPUT_PULLUP);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(TRIG_A, LOW);
  digitalWrite(TRIG_B, LOW);
  
  // DHT sensors
  dhtA.begin();
  dhtB.begin();
  
  // NRF24L01+
  if (radio.begin()) {
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_1MBPS);
    radio.setChannel(108);
    radio.openWritingPipe(PIPE_TX);
    radio.stopListening();
    Serial.println("NRF24L01+ initialized");
  } else {
    Serial.println("NRF24L01+ FAILED");
  }
  
  // WiFi
  connectWiFi();
  
  // MQTT
  espClient.setInsecure();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  connectMQTT();
  
  // Initial pricing fetch
  fetchComedPricing();
  
  Serial.println("\nSensor Hub Ready!");
  Serial.println("Press GPIO14 button to toggle ECO/MANUAL mode");
  Serial.printf("Current mode: %s\n", ecoMode ? "ECO" : "MANUAL");
}

// ============ MAIN LOOP ============
void loop() {
  unsigned long now = millis();
  
  // Maintain connections
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();
  
  // Check mode button
  checkModeButton();
  
  // Read distance sensors
  distA = readDistanceCM(TRIG_A, ECHO_A);
  distB = readDistanceCM(TRIG_B, ECHO_B);
  
  // Read temperature/humidity (every 2.5 seconds)
  if (now - lastDHTread > 2500) {
    readTemperatures();
    lastDHTread = now;
  }
  
  // Fetch pricing (every 5 minutes)
  if (now - lastPriceFetch >= PRICE_FETCH_INTERVAL || lastPriceFetch == 0) {
    fetchComedPricing();
    lastPriceFetch = now;
  }
  
  // Send NRF telemetry to Control Hub (every 250ms)
  if (now - lastNrfSend >= NRF_SEND_INTERVAL) {
    sendTelemetryNRF();
    lastNrfSend = now;
  }
  
  // Publish MQTT telemetry (every 5 seconds)
  if (now - lastMqttPublish >= MQTT_PUBLISH_INTERVAL) {
    publishMQTT();
    lastMqttPublish = now;
  }
  
  // Status LED blink (heartbeat)
  static unsigned long lastBlink = 0;
  if (now - lastBlink >= 1000) {
    digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
    lastBlink = now;
  }
  
  delay(10);
}
