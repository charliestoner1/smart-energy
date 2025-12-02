/*
  Control Hub - Smart Energy System
  Pins: Room A (LED=25, Fan=27, Btn=14), Room B (LED=26, Fan=33, Btn=12)
  NRF24L01+: CE=4, CSN=5
*/

#include <SPI.h>
#include <RF24.h>

const int PIN_LED1 = 25, PIN_FAN1 = 27, PIN_BTN_A = 14;
const int PIN_LED2 = 26, PIN_FAN2 = 33, PIN_BTN_B = 12;
const int PIN_STATUS = 2;
#define NRF_CE 4
#define NRF_CSN 5

RF24 radio(NRF_CE, NRF_CSN);
const byte PIPE_RX[6] = "SENS1";

struct Telemetry {
  uint32_t ms;
  int16_t distA_cm, distB_cm;
  int16_t tempA_x10, tempB_x10;
  uint8_t humidA, humidB;
  uint8_t ecoMode;
  uint8_t priceTier;
  int8_t tempOffset;
};

const int OCCUPIED_CUTOFF_CM = 150;
const int MIN_VALID_CM = 2, MAX_VALID_CM = 400;
const unsigned long PACKET_TIMEOUT_MS = 10000;
const unsigned long DEBOUNCE_MS = 200;
const char* TIER_NAMES[] = {"VERY_LOW", "LOW", "NORMAL", "HIGH", "VERY_HIGH", "CRITICAL"};

Telemetry lastTelemetry;
unsigned long lastPacketMs = 0;
bool telemetryValid = false;
bool ecoMode = true;
bool roomA_occupied = false, roomB_occupied = false;
bool roomA_deviceOn = false, roomB_deviceOn = false;
bool lastBtnA = HIGH, lastBtnB = HIGH;
unsigned long lastBtnPressA = 0, lastBtnPressB = 0;
bool manualStateA = false, manualStateB = false;

void setRoomDevices(int fanPin, int ledPin, bool on) {
  digitalWrite(fanPin, on ? HIGH : LOW);
  digitalWrite(ledPin, on ? HIGH : LOW);
}

bool distanceMeansOccupied(int16_t cm) {
  return (cm >= MIN_VALID_CM && cm <= MAX_VALID_CM && cm <= OCCUPIED_CUTOFF_CM);
}

bool shouldDeviceBeOn(bool occupied, float tempC, uint8_t priceTier) {
  if (priceTier >= 5) return false;
  if (priceTier == 4) return occupied && (tempC > 28.0);
  if (priceTier == 3) return occupied && (tempC > 26.0);
  return occupied;
}

void applyEcoModeControl() {
  float tempA = lastTelemetry.tempA_x10 / 10.0;
  float tempB = lastTelemetry.tempB_x10 / 10.0;
  uint8_t tier = lastTelemetry.priceTier;
  
  roomA_occupied = distanceMeansOccupied(lastTelemetry.distA_cm);
  roomB_occupied = distanceMeansOccupied(lastTelemetry.distB_cm);
  roomA_deviceOn = shouldDeviceBeOn(roomA_occupied, tempA, tier);
  roomB_deviceOn = shouldDeviceBeOn(roomB_occupied, tempB, tier);
  
  setRoomDevices(PIN_FAN1, PIN_LED1, roomA_deviceOn);
  setRoomDevices(PIN_FAN2, PIN_LED2, roomB_deviceOn);
  
  Serial.printf("ECO [%s] A:%d->%s B:%d->%s\n", TIER_NAMES[tier],
    roomA_occupied, roomA_deviceOn?"ON":"OFF", roomB_occupied, roomB_deviceOn?"ON":"OFF");
}

void checkRoomButtons() {
  unsigned long now = millis();
  
  bool currentBtnA = digitalRead(PIN_BTN_A);
  if (currentBtnA == LOW && lastBtnA == HIGH && now - lastBtnPressA > DEBOUNCE_MS) {
    manualStateA = !manualStateA;
    lastBtnPressA = now;
    Serial.printf("Btn A -> Room A: %s\n", manualStateA ? "ON" : "OFF");
  }
  lastBtnA = currentBtnA;
  
  bool currentBtnB = digitalRead(PIN_BTN_B);
  if (currentBtnB == LOW && lastBtnB == HIGH && now - lastBtnPressB > DEBOUNCE_MS) {
    manualStateB = !manualStateB;
    lastBtnPressB = now;
    Serial.printf("Btn B -> Room B: %s\n", manualStateB ? "ON" : "OFF");
  }
  lastBtnB = currentBtnB;
}

void applyManualModeControl() {
  checkRoomButtons();
  setRoomDevices(PIN_FAN1, PIN_LED1, manualStateA);
  setRoomDevices(PIN_FAN2, PIN_LED2, manualStateB);
  roomA_deviceOn = manualStateA;
  roomB_deviceOn = manualStateB;
}

void processNRFPackets() {
  while (radio.available()) {
    radio.read(&lastTelemetry, sizeof(lastTelemetry));
    lastPacketMs = millis();
    telemetryValid = true;
    
    bool newEcoMode = (lastTelemetry.ecoMode == 1);
    if (newEcoMode != ecoMode) {
      Serial.printf("\n*** MODE: %s ***\n\n", newEcoMode ? "ECO" : "MANUAL");
      ecoMode = newEcoMode;
      if (ecoMode) { manualStateA = false; manualStateB = false; }
    }
    
    digitalWrite(PIN_STATUS, HIGH); delay(10); digitalWrite(PIN_STATUS, LOW);
    Serial.printf("RX: d[%d,%d] t[%.1f,%.1f] eco=%d tier=%d\n",
      lastTelemetry.distA_cm, lastTelemetry.distB_cm,
      lastTelemetry.tempA_x10/10.0, lastTelemetry.tempB_x10/10.0,
      lastTelemetry.ecoMode, lastTelemetry.priceTier);
  }
}

void applyFailsafe() {
  setRoomDevices(PIN_FAN1, PIN_LED1, false);
  setRoomDevices(PIN_FAN2, PIN_LED2, false);
  roomA_deviceOn = false; roomB_deviceOn = false;
  
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 500) { digitalWrite(PIN_STATUS, !digitalRead(PIN_STATUS)); lastBlink = millis(); }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED1, OUTPUT); pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_FAN1, OUTPUT); pinMode(PIN_FAN2, OUTPUT);
  pinMode(PIN_STATUS, OUTPUT);
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  
  setRoomDevices(PIN_FAN1, PIN_LED1, false);
  setRoomDevices(PIN_FAN2, PIN_LED2, false);
  
  if (radio.begin()) {
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_1MBPS);
    radio.setChannel(108);
    radio.openReadingPipe(1, PIPE_RX);
    radio.startListening();
  }
  
  Serial.println("Control Hub Ready");
  Serial.println("Room buttons active in MANUAL mode only");
}

void loop() {
  processNRFPackets();
  
  if (millis() - lastPacketMs < PACKET_TIMEOUT_MS && telemetryValid) {
    if (ecoMode) applyEcoModeControl();
    else applyManualModeControl();
  } else {
    applyFailsafe();
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn > 5000) { Serial.println("FAILSAFE - No packets"); lastWarn = millis(); }
  }
  delay(5);
}
