/*
 * ESP32 Energy Optimization with ComEd Real-Time Pricing
 * Sensor Hub - Collects data and responds to price signals
 */

#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <DHT.h>

// WiFi Configuration
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// API Configuration
const char* pricingApiUrl = PRICING_API_URL;

// MQTT Configuration
const char* mqtt_server = MQTT_SERVER;
const int mqtt_port = MQTT_PORT;
const char* mqtt_user = MQTT_USER;
const char* mqtt_password = MQTT_PASSWORD;

// Pin Definitions (Sensor Hub)
#define DHT_PIN 4
#define PIR_PIN 5
#define ACS712_PIN 34
#define STATUS_LED 2

// DHT Sensor
#define DHTTYPE DHT22
DHT dht(DHT_PIN, DHTTYPE);

// MQTT Client
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Global Variables
struct PriceData {
  float price_cents_per_kwh;
  String tier;
  String action;
  int temp_offset;
  unsigned long timestamp;
  String status;
  unsigned long last_update;
} currentPrice;

struct SensorData {
  float temperature;
  float humidity;
  bool motion_detected;
  float current_amps;
  float power_watts;
  unsigned long timestamp;
} sensors;

// Timing
unsigned long lastPriceUpdate = 0;
unsigned long lastSensorRead = 0;
unsigned long lastMqttPublish = 0;
const unsigned long PRICE_UPDATE_INTERVAL = 300000;  // 5 minutes
const unsigned long SENSOR_READ_INTERVAL = 5000;     // 5 seconds
const unsigned long MQTT_PUBLISH_INTERVAL = 60000;   // 1 minute

// Function Prototypes
void setup_wifi();
void reconnect_mqtt();
void fetch_comed_price();
void read_sensors();
void publish_telemetry();
void apply_price_based_logic();
float read_current_sensor();
void blink_status_led(int times);

void setup() {
  Serial.begin(115200);
  
  // Initialize pins
  pinMode(STATUS_LED, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  
  // Initialize sensors
  dht.begin();
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqttClient.setServer(mqtt_server, mqtt_port);
  
  // Initial price fetch
  fetch_comed_price();
  
  Serial.println("ESP32 Sensor Hub initialized with ComEd pricing");
  blink_status_led(3);
}

void loop() {
  // Maintain MQTT connection
  if (!mqttClient.connected()) {
    reconnect_mqtt();
  }
  mqttClient.loop();
  
  // Update price data every 5 minutes
  unsigned long currentMillis = millis();
  if (currentMillis - lastPriceUpdate >= PRICE_UPDATE_INTERVAL) {
    fetch_comed_price();
    lastPriceUpdate = currentMillis;
  }
  
  // Read sensors every 5 seconds
  if (currentMillis - lastSensorRead >= SENSOR_READ_INTERVAL) {
    read_sensors();
    lastSensorRead = currentMillis;
  }
  
  // Publish to MQTT every minute
  if (currentMillis - lastMqttPublish >= MQTT_PUBLISH_INTERVAL) {
    publish_telemetry();
    lastMqttPublish = currentMillis;
  }
  
  // Apply price-based control logic
  apply_price_based_logic();
  
  delay(100);
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed!");
  }
}

void reconnect_mqtt() {
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    String clientId = "ESP32Sensor-";
    clientId += String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("connected");
      
      // Subscribe to control topics
      mqttClient.subscribe("control/room1/cmd");
      mqttClient.subscribe("pricing/update");
      
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void fetch_comed_price() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping price update");
    return;
  }
  
  HTTPClient http;
  http.begin(pricingApiUrl);
  http.setTimeout(10000);
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    
    // Parse JSON
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      currentPrice.price_cents_per_kwh = doc["p"].as<float>();
      currentPrice.tier = doc["t"].as<String>();
      currentPrice.action = doc["a"].as<String>();
      currentPrice.temp_offset = doc["o"].as<int>();
      currentPrice.timestamp = doc["ts"].as<unsigned long>();
      currentPrice.status = doc["s"].as<String>();
      currentPrice.last_update = millis();
      
      Serial.printf("Price updated: %.2f¢/kWh (tier: %s, action: %s)\n", 
                    currentPrice.price_cents_per_kwh,
                    currentPrice.tier.c_str(),
                    currentPrice.action.c_str());
      
      // Blink LED to indicate successful update
      digitalWrite(STATUS_LED, HIGH);
      delay(100);
      digitalWrite(STATUS_LED, LOW);
      
    } else {
      Serial.printf("JSON parsing failed: %s\n", error.c_str());
    }
    
  } else {
    Serial.printf("HTTP request failed, code: %d\n", httpCode);
  }
  
  http.end();
}

void read_sensors() {
  // Read temperature and humidity
  sensors.temperature = dht.readTemperature();
  sensors.humidity = dht.readHumidity();
  
  // Read PIR sensor
  sensors.motion_detected = digitalRead(PIR_PIN);
  
  // Read current sensor
  sensors.current_amps = read_current_sensor();
  
  // Calculate power (assuming 120V)
  sensors.power_watts = sensors.current_amps * 120.0;
  
  sensors.timestamp = millis();
  
  // Validate readings
  if (isnan(sensors.temperature) || isnan(sensors.humidity)) {
    Serial.println("Failed to read from DHT sensor!");
  }
}

float read_current_sensor() {
  // Read ACS712 sensor
  // The ACS712-20A outputs 2.5V at 0A, with 100mV per Amp
  const int numSamples = 10;
  float sum = 0;
  
  for (int i = 0; i < numSamples; i++) {
    int rawValue = analogRead(ACS712_PIN);
    float voltage = (rawValue / 4095.0) * 3.3;
    sum += voltage;
    delay(10);
  }
  
  float avgVoltage = sum / numSamples;
  float current = (avgVoltage - 2.5) / 0.1;  // 100mV/A sensitivity
  
  return abs(current);
}

void publish_telemetry() {
  if (!mqttClient.connected()) {
    return;
  }
  
  // Create telemetry JSON
  StaticJsonDocument<512> doc;
  
  doc["ts"] = sensors.timestamp;
  doc["tC"] = sensors.temperature;
  doc["rh"] = sensors.humidity;
  doc["pir"] = sensors.motion_detected;
  doc["current_A"] = sensors.current_amps;
  doc["power_W"] = sensors.power_watts;
  
  // Add pricing data
  doc["price_cents"] = currentPrice.price_cents_per_kwh;
  doc["price_tier"] = currentPrice.tier;
  doc["price_action"] = currentPrice.action;
  doc["temp_offset"] = currentPrice.temp_offset;
  
  char buffer[512];
  serializeJson(doc, buffer);
  
  // Publish to MQTT
  if (mqttClient.publish("sensors/room1/telemetry", buffer, false)) {
    Serial.println("Telemetry published");
  } else {
    Serial.println("Failed to publish telemetry");
  }
}

void apply_price_based_logic() {
  // Price-based decision making
  
  // If price data is stale (>10 minutes), use conservative defaults
  if (millis() - currentPrice.last_update > 600000) {
    Serial.println("Price data stale, using conservative mode");
    return;
  }
  
  // Publish anomaly if price is in critical tier
  if (currentPrice.tier == "critical" || currentPrice.tier == "very_high") {
    StaticJsonDocument<256> alert;
    alert["type"] = "high_price";
    alert["tier"] = currentPrice.tier;
    alert["price"] = currentPrice.price_cents_per_kwh;
    alert["message"] = "Electricity price is very high - reduce consumption";
    
    char alertBuffer[256];
    serializeJson(alert, alertBuffer);
    mqttClient.publish("alerts/room1/anomaly", alertBuffer);
  }
  
  // If price is very low and motion detected, could enable additional comfort features
  if (currentPrice.tier == "very_low" && sensors.motion_detected) {
    Serial.println("Low price + occupancy: optimal time for energy use");
  }
  
  // If price is high and no motion, recommend turning things off
  if ((currentPrice.tier == "high" || currentPrice.tier == "very_high") && 
      !sensors.motion_detected) {
    StaticJsonDocument<256> recommendation;
    recommendation["type"] = "energy_saving";
    recommendation["reason"] = "high_price_no_occupancy";
    recommendation["action"] = "reduce_load";
    
    char recBuffer[256];
    serializeJson(recommendation, recBuffer);
    mqttClient.publish("control/room1/cmd", recBuffer);
  }
}

void blink_status_led(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(STATUS_LED, HIGH);
    delay(200);
    digitalWrite(STATUS_LED, LOW);
    delay(200);
  }
}
