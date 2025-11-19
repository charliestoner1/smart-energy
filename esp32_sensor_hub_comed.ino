/*
  Sensor Hub (ESP32) — Sends distances for Room A/B via NRF24L01+
  Pins:
    HC-SR04 A: TRIG=21, ECHO=34 (ECHO via 10k/15k divider!)
    HC-SR04 B: TRIG=22, ECHO=35 (ECHO via 10k/15k divider!)
    NRF24L01+: CE=4, CSN=5  (3.3V only; add 10–47uF cap at module)
               SCK=18, MISO=19, MOSI=23 (VSPI defaults)
*/

#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <PubSubClient.h>

// -------- WiFi & MQTT --------
const char* ssid = "your_wifi_ssid";         // need to set this
const char* password = "your_wifi_password"; // need to set this
const char* mqtt_server = "1e1a4e5c581e4bc3a697f8937d7fb9e4.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "omeravi";
const char* mqtt_password = "Omeromer1!";

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// ---------------- PIN MAP ----------------
const int TRIG_A = 21;
const int ECHO_A = 34;   // input-only, via divider
const int TRIG_B = 22;
const int ECHO_B = 35;   // input-only, via divider

#define NRF_CE  4
#define NRF_CSN 5
RF24 radio(NRF_CE, NRF_CSN);

// Address must match Control Hub
const byte PIPE_TX[6] = "SENS1";

// --------- PAYLOAD (must match Control Hub) ---------
struct Telemetry {
  uint32_t ms;       // sender millis
  int16_t  distA_cm; // -1 if timeout
  int16_t  distB_cm; // -1 if timeout
};

// --------- SETTINGS ---------
const unsigned long SEND_PERIOD_MS = 250;
const unsigned long MQTT_PERIOD_MS = 5000;
const unsigned long PULSE_TIMEOUT_US = 30000UL; // 30ms ~5m
const int MIN_CM = 2, MAX_CM = 400;

// simple smoothing (median of 3 samples)
int16_t readDistanceCM(int trig, int echo) {
  auto one = [&](void)->long {
    // trigger 10us pulse
    digitalWrite(trig, LOW); delayMicroseconds(2);
    digitalWrite(trig, HIGH); delayMicroseconds(10);
    digitalWrite(trig, LOW);
    // measure echo high pulse
    unsigned long us = pulseIn(echo, HIGH, PULSE_TIMEOUT_US);
    if (us == 0) return -1;
    long cm = us / 58; // µs -> cm
    if (cm < MIN_CM || cm > MAX_CM) return -1;
    return cm;
  };
  long a = one(); delay(5);
  long b = one(); delay(5);
  long c = one();

  // median of (a,b,c), but treat -1 as "max" so valid readings win
  long x[3] = { (a<0?10000:a), (b<0?10000:b), (c<0?10000:c) };
  // sort 3
  if (x[0] > x[1]) swap(x[0], x[1]);
  if (x[1] > x[2]) swap(x[1], x[2]);
  if (x[0] > x[1]) swap(x[0], x[1]);

  long med = (x[1] >= 10000) ? -1 : x[1];
  return (int16_t)med;
}

// MQTT RECONNECT FUNCTION
void reconnectMQTT() {
  if (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    String clientId = "ESP32Sensor-" + String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println(" OK");
    } else {
      Serial.print(" FAIL (rc=");
      Serial.print(mqttClient.state());
      Serial.println(")");
    }
  }
}

// PUBLISH TO MQTT FOR DASHBOARD
void publishToMQTT() {
  // Read current distances
  int16_t distA = readDistanceCM(TRIG_A, ECHO_A);
  int16_t distB = readDistanceCM(TRIG_B, ECHO_B);
  
  // Determine if room is occupied (distance < 100cm = someone present)
  bool occupied = (distA > 0 && distA < 100) || (distB > 0 && distB < 100);
  
  // Simulate missing sensors (temporary until you add real ones)
  float tempC = 24.0;
  float humidity = 60.0;
  int pir = occupied ? 1 : 0;
  
  // Estimate power: occupied = ~150W (fan running), empty = ~50W (baseline)
  float amps = occupied ? 1.25 : 0.42;
  
  // Build JSON payload
  String payload = "{";
  payload += "\"ts\":" + String(millis()) + ",";
  payload += "\"tC\":" + String(tempC, 1) + ",";
  payload += "\"rh\":" + String(humidity, 1) + ",";
  payload += "\"pir\":" + String(pir) + ",";
  payload += "\"voltage\":120,";
  payload += "\"amps\":" + String(amps, 2);
  payload += "}";
  
  // Publish
  bool ok = mqttClient.publish("sensors/room1/telemetry", payload.c_str());
  
  Serial.print("MQTT  ");
  Serial.print(occupied ? "👤" : "🚫");
  Serial.print("  ");
  Serial.print((int)(amps * 120));
  Serial.print("W  pub=");
  Serial.println(ok ? "OK" : "FAIL");
}


unsigned long lastSend = 0;

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_A, OUTPUT); digitalWrite(TRIG_A, LOW);
  pinMode(ECHO_A, INPUT);   // GPIO34 is input-only (perfect)
  pinMode(TRIG_B, OUTPUT); digitalWrite(TRIG_B, LOW);
  pinMode(ECHO_B, INPUT);   // GPIO35 is input-only

  if (!radio.begin()) {
    Serial.println("NRF init failed (power/CE/CSN?).");
  }
  radio.setPALevel(RF24_PA_LOW);     // raise to RF24_PA_HIGH if needed
  radio.setDataRate(RF24_1MBPS);
  radio.setChannel(108);
  radio.openWritingPipe(PIPE_TX);
  radio.stopListening();             // TX mode

  Serial.println("Sensor Hub ready: sending to 'SENS1'");

  // connect to WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" OK");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // setup MQTT
  espClient.setInsecure(); // For testing
  mqttClient.setServer(mqtt_server, mqtt_port);

  reconnectMQTT();
}

void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  if (millis() - lastSend >= SEND_PERIOD_MS) {
    lastSend = millis();

    Telemetry t;
    t.ms = millis();
    t.distA_cm = readDistanceCM(TRIG_A, ECHO_A);
    t.distB_cm = readDistanceCM(TRIG_B, ECHO_B);

    bool ok = radio.write(&t, sizeof(t));

    // Debug
    Serial.print("TX  A:"); Serial.print(t.distA_cm);
    Serial.print("cm  B:"); Serial.print(t.distB_cm);
    Serial.print("  send="); Serial.println(ok ? "OK" : "FAIL");
  }

  if (millis() - lastMqtt >= MQTT_PERIOD_MS) {
    lastMqtt = millis();
    publishToMQTT();
  }
}
