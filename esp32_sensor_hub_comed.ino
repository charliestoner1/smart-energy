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
}

void loop() {
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
}
