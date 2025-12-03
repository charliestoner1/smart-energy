/*
  Control Hub - Smart Energy System 
  
  This hub controls devices for both Room A and Room B.
  It receives data via NRF24L01+ from Sensor Hub AND via MQTT from Dashboard.
  
  Pins:
    Room A: LED=25, Fan=27, Button=14
    Room B: LED=26, Fan=33, Button=12
    NRF24L01+: CE=4, CSN=5
    Status LED: 2
  
  MQTT Topics Subscribed:
    control/room1/cmd   - Commands for Room A
    control/room2/cmd   - Commands for Room B
    control/mode        - Mode changes (eco/manual)
*/

#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ============ CONFIGURATION - UPDATE THESE ============
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// MQTT (HiveMQ Cloud)
const char* MQTT_SERVER = "1e1a4e5c581e4bc3a697f8937d7fb9e4.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "omeravi";
const char* MQTT_PASSWORD = "Omeromer1!";

// ============ MQTT TOPICS ============
const char* TOPIC_CONTROL_A = "control/room1/cmd";
const char* TOPIC_CONTROL_B = "control/room2/cmd";
const char* TOPIC_MODE = "control/mode";
const char* TOPIC_STATE_A = "control/room1/state";
const char* TOPIC_STATE_B = "control/room2/state";

// ============ PIN DEFINITIONS ============
const int PIN_LED1 = 25, PIN_FAN1 = 27, PIN_BTN_A = 14;  // Room A
const int PIN_LED2 = 26, PIN_FAN2 = 33, PIN_BTN_B = 12;  // Room B
const int PIN_STATUS = 2;
#define NRF_CE 4
#define NRF_CSN 5

// ============ OBJECTS ============
RF24 radio(NRF_CE, NRF_CSN);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
const byte PIPE_RX[6] = "SENS1";

// ============ TELEMETRY STRUCT (from Sensor Hub via NRF) ============
struct Telemetry {
  uint32_t ms;
  int16_t distA_cm, distB_cm;
  int16_t tempA_x10, tempB_x10;
  uint8_t humidA, humidB;
  uint8_t ecoMode;
  uint8_t priceTier;
  int8_t tempOffset;
};

// ============ CONSTANTS ============
const int OCCUPIED_CUTOFF_CM = 150;
const int MIN_VALID_CM = 2, MAX_VALID_CM = 400;
const unsigned long PACKET_TIMEOUT_MS = 10000;
const unsigned long DEBOUNCE_MS = 200;
const unsigned long STATE_PUBLISH_INTERVAL = 5000;
const char* TIER_NAMES[] = {"VERY_LOW", "LOW", "NORMAL", "HIGH", "VERY_HIGH", "CRITICAL"};

// ============ STATE VARIABLES ============
Telemetry lastTelemetry;
unsigned long lastPacketMs = 0;
bool telemetryValid = false;
bool ecoMode = true;
bool overrideActive = false;

// Room states
bool roomA_occupied = false, roomB_occupied = false;
bool fanA = false, lampA = false;
bool fanB = false, lampB = false;

// Button states
bool lastBtnA = HIGH, lastBtnB = HIGH;
unsigned long lastBtnPressA = 0, lastBtnPressB = 0;

// Timing
unsigned long lastStatePublish = 0;

// ============ DEVICE CONTROL ============
void setFan(char room, bool on) {
  if (room == 'A') {
    fanA = on;
    digitalWrite(PIN_FAN1, on ? HIGH : LOW);
  } else {
    fanB = on;
    digitalWrite(PIN_FAN2, on ? HIGH : LOW);
  }
  Serial.printf("Room %c Fan: %s\n", room, on ? "ON" : "OFF");
}

void setLamp(char room, bool on) {
  if (room == 'A') {
    lampA = on;
    digitalWrite(PIN_LED1, on ? HIGH : LOW);
  } else {
    lampB = on;
    digitalWrite(PIN_LED2, on ? HIGH : LOW);
  }
  Serial.printf("Room %c Lamp: %s\n", room, on ? "ON" : "OFF");
}

void setAllDevices(bool on) {
  setFan('A', on);
  setLamp('A', on);
  setFan('B', on);
  setLamp('B', on);
}

// ============ MQTT CALLBACK ============
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("JSON parse error: %s\n", error.c_str());
    return;
  }
  
  String topicStr = String(topic);
  Serial.printf("MQTT RX [%s]: ", topic);
  serializeJson(doc, Serial);
  Serial.println();
  
  // Handle mode changes
  if (topicStr == TOPIC_MODE) {
    String newMode = doc["mode"].as<String>();
    bool newEcoMode = (newMode == "eco");
    
    if (newEcoMode != ecoMode) {
      ecoMode = newEcoMode;
      Serial.printf("\n*** MODE FROM MQTT: %s ***\n\n", ecoMode ? "ECO" : "MANUAL");
      
      // Visual feedback
      for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_STATUS, HIGH); delay(100);
        digitalWrite(PIN_STATUS, LOW); delay(100);
      }
    }
    return;
  }
  
  // Handle control commands
  if (topicStr == TOPIC_CONTROL_A || topicStr == TOPIC_CONTROL_B) {
    char room = (topicStr == TOPIC_CONTROL_A) ? 'A' : 'B';
    
    String device = doc["device"].as<String>();
    String action = doc["action"].as<String>();
    String reason = doc["reason"] | "unknown";
    
    // Check for emergency override
    if (reason == "emergency_override") {
      overrideActive = true;
      setAllDevices(false);
      Serial.println("!!! EMERGENCY OVERRIDE ACTIVATED !!!");
      return;
    }
    
    // In Eco mode, only allow commands with reason "eco_control" or during override deactivation
    if (ecoMode && reason != "eco_control" && reason != "emergency_override") {
      Serial.printf("Ignoring manual command in ECO mode (reason: %s)\n", reason.c_str());
      return;
    }
    
    // Apply command
    bool on = (action == "on");
    
    if (device == "fan") {
      setFan(room, on);
    } else if (device == "lamp") {
      setLamp(room, on);
    } else if (device == "all") {
      if (room == 'A') {
        setFan('A', on);
        setLamp('A', on);
      } else {
        setFan('B', on);
        setLamp('B', on);
      }
    }
    
    // Publish state update
    publishDeviceState(room);
  }
}

// ============ PUBLISH DEVICE STATE ============
void publishDeviceState(char room) {
  if (!mqttClient.connected()) return;
  
  StaticJsonDocument<128> doc;
  doc["timestamp"] = millis();
  
  if (room == 'A') {
    doc["fan"] = fanA;
    doc["lamp"] = lampA;
    doc["occupied"] = roomA_occupied;
    
    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(TOPIC_STATE_A, payload.c_str());
  } else {
    doc["fan"] = fanB;
    doc["lamp"] = lampB;
    doc["occupied"] = roomB_occupied;
    
    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(TOPIC_STATE_B, payload.c_str());
  }
}

// ============ ECO MODE CONTROL LOGIC ============
bool distanceMeansOccupied(int16_t cm) {
  return (cm >= MIN_VALID_CM && cm <= MAX_VALID_CM && cm <= OCCUPIED_CUTOFF_CM);
}

bool shouldDeviceBeOn(bool occupied, float tempC, uint8_t priceTier) {
  // Critical pricing - everything off
  if (priceTier >= 5) return false;
  
  // Very high pricing - only if occupied AND very hot
  if (priceTier == 4) return occupied && (tempC > 28.0);
  
  // High pricing - only if occupied AND hot
  if (priceTier == 3) return occupied && (tempC > 26.0);
  
  // Normal/Low pricing - on if occupied
  return occupied;
}

void applyEcoModeControl() {
  if (!telemetryValid) return;
  
  float tempA = lastTelemetry.tempA_x10 / 10.0;
  float tempB = lastTelemetry.tempB_x10 / 10.0;
  uint8_t tier = lastTelemetry.priceTier;
  
  // Update occupancy
  roomA_occupied = distanceMeansOccupied(lastTelemetry.distA_cm);
  roomB_occupied = distanceMeansOccupied(lastTelemetry.distB_cm);
  
  // Determine what should be on
  bool shouldFanA = shouldDeviceBeOn(roomA_occupied, tempA, tier);
  bool shouldFanB = shouldDeviceBeOn(roomB_occupied, tempB, tier);
  
  // Apply if different from current state
  if (shouldFanA != fanA) {
    setFan('A', shouldFanA);
    setLamp('A', shouldFanA);  // Lamp follows fan in eco mode
  }
  if (shouldFanB != fanB) {
    setFan('B', shouldFanB);
    setLamp('B', shouldFanB);
  }
  
  Serial.printf("ECO [%s] A:occ=%d->%s B:occ=%d->%s\n", 
    TIER_NAMES[tier],
    roomA_occupied, fanA ? "ON" : "OFF",
    roomB_occupied, fanB ? "ON" : "OFF");
}

// ============ MANUAL MODE BUTTON HANDLING ============
void checkRoomButtons() {
  if (ecoMode) return;  // Buttons only work in manual mode
  
  unsigned long now = millis();
  
  // Room A button
  bool currentBtnA = digitalRead(PIN_BTN_A);
  if (currentBtnA == LOW && lastBtnA == HIGH && now - lastBtnPressA > DEBOUNCE_MS) {
    // Toggle both fan and lamp for Room A
    bool newState = !fanA;
    setFan('A', newState);
    setLamp('A', newState);
    lastBtnPressA = now;
    publishDeviceState('A');
  }
  lastBtnA = currentBtnA;
  
  // Room B button
  bool currentBtnB = digitalRead(PIN_BTN_B);
  if (currentBtnB == LOW && lastBtnB == HIGH && now - lastBtnPressB > DEBOUNCE_MS) {
    // Toggle both fan and lamp for Room B
    bool newState = !fanB;
    setFan('B', newState);
    setLamp('B', newState);
    lastBtnPressB = now;
    publishDeviceState('B');
  }
  lastBtnB = currentBtnB;
}

// ============ NRF24L01+ PACKET PROCESSING ============
void processNRFPackets() {
  while (radio.available()) {
    radio.read(&lastTelemetry, sizeof(lastTelemetry));
    lastPacketMs = millis();
    telemetryValid = true;
    
    // Check if mode changed from Sensor Hub button
    bool newEcoMode = (lastTelemetry.ecoMode == 1);
    if (newEcoMode != ecoMode) {
      ecoMode = newEcoMode;
      Serial.printf("\n*** MODE FROM SENSOR HUB: %s ***\n\n", ecoMode ? "ECO" : "MANUAL");
    }
    
    // Update occupancy from NRF data
    roomA_occupied = distanceMeansOccupied(lastTelemetry.distA_cm);
    roomB_occupied = distanceMeansOccupied(lastTelemetry.distB_cm);
    
    // Blink status LED
    digitalWrite(PIN_STATUS, HIGH);
    delay(10);
    digitalWrite(PIN_STATUS, LOW);
    
    Serial.printf("NRF RX: d[%d,%d] t[%.1f,%.1f] eco=%d tier=%d\n",
      lastTelemetry.distA_cm, lastTelemetry.distB_cm,
      lastTelemetry.tempA_x10 / 10.0, lastTelemetry.tempB_x10 / 10.0,
      lastTelemetry.ecoMode, lastTelemetry.priceTier);
  }
}

// ============ FAILSAFE ============
void applyFailsafe() {
  setAllDevices(false);
  
  // Fast blink to indicate failsafe
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 250) {
    digitalWrite(PIN_STATUS, !digitalRead(PIN_STATUS));
    lastBlink = millis();
  }
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

// ============ MQTT CONNECTION ============
void connectMQTT() {
  if (mqttClient.connected()) return;
  
  String clientId = "ControlHub-" + String(random(0xffff), HEX);
  Serial.print("Connecting to MQTT...");
  
  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println(" OK");
    
    // Subscribe to control topics
    mqttClient.subscribe(TOPIC_CONTROL_A);
    mqttClient.subscribe(TOPIC_CONTROL_B);
    mqttClient.subscribe(TOPIC_MODE);
    
    Serial.println("Subscribed to control topics");
    
    // Publish initial state
    publishDeviceState('A');
    publishDeviceState('B');
  } else {
    Serial.printf(" FAILED (rc=%d)\n", mqttClient.state());
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Control Hub Starting ===");
  
  // Pin setup
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_FAN1, OUTPUT);
  pinMode(PIN_FAN2, OUTPUT);
  pinMode(PIN_STATUS, OUTPUT);
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  
  // Start with everything off
  setAllDevices(false);
  
  // NRF24L01+ setup
  if (radio.begin()) {
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_1MBPS);
    radio.setChannel(108);
    radio.openReadingPipe(1, PIPE_RX);
    radio.startListening();
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
  mqttClient.setBufferSize(512);
  connectMQTT();
  
  Serial.println("\nControl Hub Ready!");
  Serial.println("- NRF: Receiving from Sensor Hub");
  Serial.println("- MQTT: Receiving from Dashboard");
  Serial.println("- Buttons: Active in MANUAL mode only");
  Serial.printf("Current mode: %s\n", ecoMode ? "ECO" : "MANUAL");
}

// ============ MAIN LOOP ============
void loop() {
  unsigned long now = millis();
  
  // Maintain WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  
  // Maintain MQTT connection
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();
  
  // Process NRF packets from Sensor Hub
  processNRFPackets();
  
  // Check physical buttons (only works in manual mode)
  checkRoomButtons();
  
  // Apply control logic based on mode
  if (overrideActive) {
    // Override keeps everything off
    static unsigned long lastWarn = 0;
    if (now - lastWarn > 5000) {
      Serial.println("OVERRIDE ACTIVE - All devices disabled");
      lastWarn = now;
    }
  } else if (millis() - lastPacketMs < PACKET_TIMEOUT_MS && telemetryValid) {
    // Normal operation
    if (ecoMode) {
      applyEcoModeControl();
    }
    // In manual mode, devices are controlled by MQTT commands or buttons
  } else {
    // Failsafe - no packets received
    applyFailsafe();
    static unsigned long lastWarn = 0;
    if (now - lastWarn > 5000) {
      Serial.println("FAILSAFE - No NRF packets received");
      lastWarn = now;
    }
  }
  
  // Periodically publish device state
  if (now - lastStatePublish > STATE_PUBLISH_INTERVAL) {
    publishDeviceState('A');
    publishDeviceState('B');
    lastStatePublish = now;
  }
  
  // Heartbeat LED (slow blink = normal, fast blink = failsafe)
  static unsigned long lastBlink = 0;
  unsigned long blinkInterval = telemetryValid ? 1000 : 250;
  if (now - lastBlink > blinkInterval) {
    digitalWrite(PIN_STATUS, !digitalRead(PIN_STATUS));
    lastBlink = now;
  }
  
  delay(5);
}
