/*
  Sensor Hub - Smart Energy System
  Pins: HC-SR04 A (21/34), B (22/35), DHT11 A (27), B (33), Mode Button (14)
  NRF24L01+: CE=4, CSN=5
*/

#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// WiFi & API - UPDATE THESE
const char* WIFI_SSID = "Omer iphone";
const char* WIFI_PASSWORD = "omeromer1";
const char* PRICING_API_URL = "https://smart-energy-production-a08d.up.railway.app/";

// MQTT (HiveMQ Cloud)
const char* MQTT_SERVER = "1e1a4e5c581e4bc3a697f8937d7fb9e4.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "omeravi";
const char* MQTT_PASSWORD = "Omeromer1!";

// Pins
const int TRIG_A = 21, ECHO_A = 34;
const int TRIG_B = 22, ECHO_B = 35;
const int DHT_PIN_A = 26, DHT_PIN_B = 25;
const int MAIN_BUTTON = 14;
const int STATUS_LED = 2;
#define NRF_CE 4
#define NRF_CSN 5

// Objects
RF24 radio(NRF_CE, NRF_CSN);
DHT dhtA(DHT_PIN_A, DHT11);
DHT dhtB(DHT_PIN_B, DHT11);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
const byte PIPE_TX[6] = "SENS1";
unsigned long lastDHTread = 0;


struct Telemetry {
  uint32_t ms;
  int16_t distA_cm, distB_cm;
  int16_t tempA_x10, tempB_x10;
  uint8_t humidA, humidB;
  uint8_t ecoMode;
  uint8_t priceTier;
  int8_t tempOffset;
};

struct PricingData {
  float price;
  uint8_t tier;
  String action;
  int8_t tempOffset;
  bool valid;
  unsigned long lastUpdate;
};

bool ecoMode = true;
bool lastButtonState = HIGH;
unsigned long lastButtonPress = 0;
PricingData currentPricing = {0, 2, "normal", 0, false, 0};
unsigned long lastPriceFetch = 0, lastNrfSend = 0, lastMqttPublish = 0;
float tempA = 0, tempB = 0, humidA = 0, humidB = 0;
int16_t distA = -1, distB = -1;

const unsigned long DEBOUNCE_MS = 200;
const unsigned long PRICE_FETCH_INTERVAL = 300000;
const unsigned long NRF_SEND_INTERVAL = 250;
const unsigned long MQTT_PUBLISH_INTERVAL = 5000;
const unsigned long PULSE_TIMEOUT_US = 30000;
const int MIN_CM = 2, MAX_CM = 400;

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
  for (int i = 0; i < 2; i++)
    for (int j = i + 1; j < 3; j++)
      if (readings[i] > readings[j]) { long t = readings[i]; readings[i] = readings[j]; readings[j] = t; }
  return (readings[1] >= 10000) ? -1 : (int16_t)readings[1];
}

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


void checkMainButton() {
  bool current = digitalRead(MAIN_BUTTON);
  if (current == LOW && lastButtonState == HIGH && millis() - lastButtonPress > DEBOUNCE_MS) {
    ecoMode = !ecoMode;
    lastButtonPress = millis();
    Serial.printf("Mode: %s\n", ecoMode ? "ECO" : "MANUAL");
    for (int i = 0; i < 3; i++) { digitalWrite(STATUS_LED, HIGH); delay(100); digitalWrite(STATUS_LED, LOW); delay(100); }
  }
  lastButtonState = current;
}

uint8_t tierStringToNum(const String& tier) {
  if (tier == "very_low") return 0;
  if (tier == "low") return 1;
  if (tier == "normal") return 2;
  if (tier == "high") return 3;
  if (tier == "very_high") return 4;
  if (tier == "critical") return 5;
  return 2;
}

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
      Serial.printf("Price: %.2f¢, Tier: %d\n", currentPricing.price, currentPricing.tier);
    }
  }
  http.end();
}

void sendTelemetryNRF() {
  Telemetry t = {
    millis(), distA, distB,
    (int16_t)(tempA * 10), (int16_t)(tempB * 10),
    (uint8_t)constrain(humidA, 0, 100), (uint8_t)constrain(humidB, 0, 100),
    ecoMode ? (uint8_t)1 : (uint8_t)0, currentPricing.tier, currentPricing.tempOffset
  };
  bool ok = radio.write(&t, sizeof(t));
  Serial.printf("NRF: A=%d B=%d eco=%d tier=%d -> %s\n", distA, distB, ecoMode, currentPricing.tier, ok ? "OK" : "FAIL");
}

void reconnectMQTT() {
  if (mqttClient.connected()) return;
  String clientId = "SensorHub-" + String(random(0xffff), HEX);
  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD))
    Serial.println("MQTT connected");
}

void publishMQTT() {
  if (!mqttClient.connected()) reconnectMQTT();
  if (!mqttClient.connected()) return;
  
  bool occA = (distA > 0 && distA < 150), occB = (distB > 0 && distB < 150);
  float amps = 0.42 + (occA ? 0.5 : 0) + (occB ? 0.5 : 0);
  
  StaticJsonDocument<512> doc;
  doc["ts"] = millis();
  doc["tC_A"] = tempA; doc["tC_B"] = tempB;
  doc["rh_A"] = humidA; doc["rh_B"] = humidB;
  doc["dist_A"] = distA; doc["dist_B"] = distB;
  doc["occ_A"] = occA; doc["occ_B"] = occB;
  doc["voltage"] = 120; doc["amps"] = amps;
  doc["eco_mode"] = ecoMode;
  doc["price_cents"] = currentPricing.price;
  doc["price_tier"] = currentPricing.tier;
  doc["price_action"] = currentPricing.action;
  
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish("sensors/room1/telemetry", payload.c_str());
}

void connectWiFi() {
  Serial.print("WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts++ < 30) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " FAILED");
  if (WiFi.status() == WL_CONNECTED) Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_A, OUTPUT); pinMode(ECHO_A, INPUT);
  pinMode(TRIG_B, OUTPUT); pinMode(ECHO_B, INPUT);
  pinMode(MAIN_BUTTON, INPUT_PULLUP);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(TRIG_A, LOW); digitalWrite(TRIG_B, LOW);
  
  dhtA.begin(); dhtB.begin();
  
  if (radio.begin()) {
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_1MBPS);
    radio.setChannel(108);
    radio.openWritingPipe(PIPE_TX);
    radio.stopListening();
  }
  
  connectWiFi();
  espClient.setInsecure();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  reconnectMQTT();
  fetchComedPricing();
  
  Serial.println("Sensor Hub Ready - Button GPIO14 toggles ECO/MANUAL");
}

void loop() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (mqttClient.connected()) mqttClient.loop();
  
  checkMainButton();
  distA = readDistanceCM(TRIG_A, ECHO_A);
  distB = readDistanceCM(TRIG_B, ECHO_B);
  if (millis() - lastDHTread > 2500) {   // read every 2.5 seconds only
    readTemperatures();
    lastDHTread = millis();
    Serial.printf("TEMP A = %.2f C | HUM A = %.1f %% || TEMP B = %.2f C | HUM B = %.1f %%\n", tempA, humidA, tempB, humidB);
  }

  if (now - lastPriceFetch >= PRICE_FETCH_INTERVAL || lastPriceFetch == 0) { fetchComedPricing(); lastPriceFetch = now; }
  if (now - lastNrfSend >= NRF_SEND_INTERVAL) { sendTelemetryNRF(); lastNrfSend = now; }
  if (now - lastMqttPublish >= MQTT_PUBLISH_INTERVAL) { publishMQTT(); lastMqttPublish = now; }
  
  static unsigned long lastBlink = 0;
  if (now - lastBlink >= 1000) { digitalWrite(STATUS_LED, !digitalRead(STATUS_LED)); lastBlink = now; }
  delay(10);
}