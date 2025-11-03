/*
 * ESP32 Energy Optimization with ComEd Real-Time Pricing
 * Control Hub - Actuates relays based on price tiers and sensor data
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// WiFi Configuration
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// MQTT Configuration
const char* mqtt_server = "YOUR_MQTT_BROKER";
const int mqtt_port = 1883;
const char* mqtt_user = "YOUR_MQTT_USER";
const char* mqtt_password = "YOUR_MQTT_PASSWORD";

// Pin Definitions (Control Hub)
#define RELAY1_PIN 25  // Fan control
#define RELAY2_PIN 26  // Lamp/Appliance control
#define OVERRIDE_BTN 0 // Manual override button
#define STATUS_LED 2

// MQTT Client
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Control State
struct DeviceState {
  bool fan_enabled;
  bool lamp_enabled;
  bool auto_mode;
  bool override_active;
  unsigned long override_time;
} deviceState = {false, false, true, false, 0};

struct PricingState {
  float price_cents;
  String tier;
  String action;
  int temp_offset;
  unsigned long last_update;
} pricingState;

struct RoomConditions {
  float temperature;
  float humidity;
  bool motion_detected;
  float power_watts;
  unsigned long last_update;
} roomConditions;

// Timing
const unsigned long OVERRIDE_TIMEOUT = 1800000;  // 30 minutes
const unsigned long STATE_PUBLISH_INTERVAL = 30000;  // 30 seconds
unsigned long lastStatePublish = 0;

// Function Prototypes
void setup_wifi();
void reconnect_mqtt();
void mqtt_callback(char* topic, byte* payload, unsigned int length);
void handle_telemetry(JsonDocument& doc);
void handle_control_command(JsonDocument& doc);
void apply_pricing_control();
void set_fan(bool state);
void set_lamp(bool state);
void publish_state();
void check_override_button();
void emergency_shutdown();

void setup() {
  Serial.begin(115200);
  
  // Initialize pins
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(OVERRIDE_BTN, INPUT_PULLUP);
  
  // Set relays to OFF initially (active LOW for most relay modules)
  digitalWrite(RELAY1_PIN, HIGH);
  digitalWrite(RELAY2_PIN, HIGH);
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqtt_callback);
  
  Serial.println("ESP32 Control Hub initialized with ComEd pricing integration");
  
  // Blink LED to indicate ready
  for (int i = 0; i < 3; i++) {
    digitalWrite(STATUS_LED, HIGH);
    delay(200);
    digitalWrite(STATUS_LED, LOW);
    delay(200);
  }
}

void loop() {
  // Maintain MQTT connection
  if (!mqttClient.connected()) {
    reconnect_mqtt();
  }
  mqttClient.loop();
  
  // Check for manual override button press
  check_override_button();
  
  // Apply pricing-based control logic
  if (deviceState.auto_mode && !deviceState.override_active) {
    apply_pricing_control();
  }
  
  // Publish device state periodically
  unsigned long currentMillis = millis();
  if (currentMillis - lastStatePublish >= STATE_PUBLISH_INTERVAL) {
    publish_state();
    lastStatePublish = currentMillis;
  }
  
  // Check if override has timed out
  if (deviceState.override_active && 
      (currentMillis - deviceState.override_time > OVERRIDE_TIMEOUT)) {
    Serial.println("Override timeout, returning to auto mode");
    deviceState.override_active = false;
    deviceState.auto_mode = true;
  }
  
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
    
    String clientId = "ESP32Control-";
    clientId += String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("connected");
      
      // Subscribe to topics
      mqttClient.subscribe("sensors/room1/telemetry");
      mqttClient.subscribe("control/room1/cmd");
      mqttClient.subscribe("pricing/update");
      mqttClient.subscribe("control/room1/override");
      
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  
  // Convert payload to string
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  
  Serial.println(message);
  
  // Parse JSON
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return;
  }
  
  // Route to appropriate handler
  String topicStr = String(topic);
  
  if (topicStr == "sensors/room1/telemetry") {
    handle_telemetry(doc);
  } else if (topicStr == "control/room1/cmd") {
    handle_control_command(doc);
  } else if (topicStr == "pricing/update") {
    // Update pricing state from MQTT (alternative to HTTP polling)
    pricingState.price_cents = doc["price"].as<float>();
    pricingState.tier = doc["tier"].as<String>();
    pricingState.action = doc["action"].as<String>();
    pricingState.temp_offset = doc["offset"].as<int>();
    pricingState.last_update = millis();
  } else if (topicStr == "control/room1/override") {
    if (doc["emergency"] == true) {
      emergency_shutdown();
    }
  }
}

void handle_telemetry(JsonDocument& doc) {
  // Update room conditions from sensor hub
  roomConditions.temperature = doc["tC"].as<float>();
  roomConditions.humidity = doc["rh"].as<float>();
  roomConditions.motion_detected = doc["pir"].as<bool>();
  roomConditions.power_watts = doc["power_W"].as<float>();
  roomConditions.last_update = millis();
  
  // Update pricing info if included in telemetry
  if (doc.containsKey("price_cents")) {
    pricingState.price_cents = doc["price_cents"].as<float>();
    pricingState.tier = doc["price_tier"].as<String>();
    pricingState.action = doc["price_action"].as<String>();
    pricingState.temp_offset = doc["temp_offset"].as<int>();
    pricingState.last_update = millis();
  }
  
  Serial.printf("Room: %.1f°C, %d%% RH, Motion: %d, Power: %.1fW, Price: %.2f¢ (%s)\n",
                roomConditions.temperature,
                (int)roomConditions.humidity,
                roomConditions.motion_detected,
                roomConditions.power_watts,
                pricingState.price_cents,
                pricingState.tier.c_str());
}

void handle_control_command(JsonDocument& doc) {
  // Handle manual or automated control commands
  
  String cmdType = doc["type"].as<String>();
  
  if (cmdType == "manual") {
    // Manual control from dashboard
    if (doc.containsKey("fan")) {
      set_fan(doc["fan"].as<bool>());
    }
    if (doc.containsKey("lamp")) {
      set_lamp(doc["lamp"].as<bool>());
    }
    
    // Manual commands activate override mode temporarily
    deviceState.override_active = true;
    deviceState.override_time = millis();
    deviceState.auto_mode = false;
    
  } else if (cmdType == "auto_mode") {
    deviceState.auto_mode = doc["enabled"].as<bool>();
    deviceState.override_active = false;
    Serial.printf("Auto mode: %s\n", deviceState.auto_mode ? "enabled" : "disabled");
    
  } else if (cmdType == "energy_saving") {
    // Recommendation from sensor hub based on pricing
    String reason = doc["reason"].as<String>();
    String action = doc["action"].as<String>();
    
    if (action == "reduce_load" && deviceState.auto_mode) {
      Serial.println("Energy saving triggered by high price + no occupancy");
      set_fan(false);
      // Keep essential devices on, but disable comfort devices
    }
  }
  
  // Send acknowledgment
  StaticJsonDocument<128> ack;
  ack["cmd_received"] = cmdType;
  ack["status"] = "executed";
  ack["timestamp"] = millis();
  
  char ackBuffer[128];
  serializeJson(ack, ackBuffer);
  mqttClient.publish("control/room1/ack", ackBuffer);
}

void apply_pricing_control() {
  // Check if pricing data is fresh (within last 10 minutes)
  if (millis() - pricingState.last_update > 600000) {
    Serial.println("Price data stale, using conservative defaults");
    return;
  }
  
  String tier = pricingState.tier;
  String action = pricingState.action;
  
  // Price-based control logic
  
  if (tier == "very_low" || tier == "low") {
    // Low prices: Normal or enhanced comfort
    if (roomConditions.motion_detected) {
      // Room occupied, maintain comfort
      // Fan can run if temperature is warm
      if (roomConditions.temperature > 24.0) {
        set_fan(true);
      }
    }
    
  } else if (tier == "normal") {
    // Normal pricing: Standard operation
    if (roomConditions.motion_detected && roomConditions.temperature > 25.0) {
      set_fan(true);
    } else if (roomConditions.temperature < 23.0) {
      set_fan(false);
    }
    
  } else if (tier == "high") {
    // High prices: Reduce non-essential loads
    if (roomConditions.motion_detected) {
      // Room occupied, but be conservative
      if (roomConditions.temperature > 26.0) {
        set_fan(true);  // Only if really warm
      } else {
        set_fan(false);
      }
    } else {
      // No occupancy: turn off non-essential
      set_fan(false);
      set_lamp(false);
    }
    
  } else if (tier == "very_high" || tier == "critical") {
    // Very high prices: Minimize consumption
    Serial.println("CRITICAL PRICING: Minimizing loads");
    
    if (!roomConditions.motion_detected) {
      // No occupancy: turn everything off
      set_fan(false);
      set_lamp(false);
    } else {
      // Occupied: keep only essentials, disable comfort loads
      set_fan(false);  // Disable fan even if occupied
      
      // Could keep lamp on for safety if motion detected
      if (roomConditions.motion_detected) {
        set_lamp(true);
      }
    }
    
    // Publish alert
    StaticJsonDocument<256> alert;
    alert["type"] = "critical_pricing";
    alert["price"] = pricingState.price_cents;
    alert["action_taken"] = "minimized_loads";
    alert["savings_mode"] = "aggressive";
    
    char alertBuffer[256];
    serializeJson(alert, alertBuffer);
    mqttClient.publish("alerts/room1/anomaly", alertBuffer);
  }
  
  // Additional logic: if power consumption is high AND prices are high
  if (roomConditions.power_watts > 500 && 
      (tier == "high" || tier == "very_high" || tier == "critical")) {
    Serial.printf("High power (%dW) + high prices - reducing loads\n", 
                  (int)roomConditions.power_watts);
    set_fan(false);
  }
}

void set_fan(bool state) {
  if (deviceState.fan_enabled != state) {
    deviceState.fan_enabled = state;
    digitalWrite(RELAY1_PIN, state ? LOW : HIGH);  // Active LOW relay
    Serial.printf("Fan: %s\n", state ? "ON" : "OFF");
    
    // Blink LED to indicate state change
    digitalWrite(STATUS_LED, HIGH);
    delay(100);
    digitalWrite(STATUS_LED, LOW);
  }
}

void set_lamp(bool state) {
  if (deviceState.lamp_enabled != state) {
    deviceState.lamp_enabled = state;
    digitalWrite(RELAY2_PIN, state ? LOW : HIGH);  // Active LOW relay
    Serial.printf("Lamp: %s\n", state ? "ON" : "OFF");
  }
}

void publish_state() {
  StaticJsonDocument<512> doc;
  
  // Device states
  doc["fan"] = deviceState.fan_enabled;
  doc["lamp"] = deviceState.lamp_enabled;
  doc["auto_mode"] = deviceState.auto_mode;
  doc["override_active"] = deviceState.override_active;
  
  // Room conditions
  doc["temperature"] = roomConditions.temperature;
  doc["humidity"] = roomConditions.humidity;
  doc["motion"] = roomConditions.motion_detected;
  doc["power_watts"] = roomConditions.power_watts;
  
  // Pricing state
  doc["price_cents"] = pricingState.price_cents;
  doc["price_tier"] = pricingState.tier;
  doc["price_action"] = pricingState.action;
  
  doc["timestamp"] = millis();
  
  char buffer[512];
  serializeJson(doc, buffer);
  
  mqttClient.publish("control/room1/state", buffer);
}

void check_override_button() {
  static unsigned long lastPress = 0;
  static bool lastState = HIGH;
  
  bool currentState = digitalRead(OVERRIDE_BTN);
  
  // Button pressed (active LOW with pull-up)
  if (currentState == LOW && lastState == HIGH && (millis() - lastPress > 1000)) {
    Serial.println("Manual override button pressed");
    
    // Toggle override mode
    deviceState.override_active = !deviceState.override_active;
    
    if (deviceState.override_active) {
      deviceState.override_time = millis();
      deviceState.auto_mode = false;
      // Turn everything off for safety
      set_fan(false);
      set_lamp(false);
      Serial.println("Override activated - manual control only");
    } else {
      deviceState.auto_mode = true;
      Serial.println("Override deactivated - returning to auto mode");
    }
    
    lastPress = millis();
  }
  
  lastState = currentState;
}

void emergency_shutdown() {
  Serial.println("EMERGENCY SHUTDOWN ACTIVATED");
  
  // Turn off all loads
  set_fan(false);
  set_lamp(false);
  
  // Lock in override mode
  deviceState.override_active = true;
  deviceState.override_time = millis();
  deviceState.auto_mode = false;
  
  // Publish emergency status
  StaticJsonDocument<128> doc;
  doc["status"] = "emergency_shutdown";
  doc["timestamp"] = millis();
  doc["reason"] = "manual_override_or_security";
  
  char buffer[128];
  serializeJson(doc, buffer);
  mqttClient.publish("alerts/room1/emergency", buffer);
  
  // Rapid LED blink
  for (int i = 0; i < 10; i++) {
    digitalWrite(STATUS_LED, HIGH);
    delay(100);
    digitalWrite(STATUS_LED, LOW);
    delay(100);
  }
}
