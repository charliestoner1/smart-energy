/*
  Control Hub (ESP32) — Receives ultrasonic distances from Sensor Hub via NRF24L01+
  - Room A controls: FAN1 (GPIO 27), LED1 (GPIO 25)
  - Room B controls: FAN2 (GPIO 33), LED2 (GPIO 26)
  - NRF: CE=4, CSN=5 (3.3V only; add 10–47uF cap at VCC-GND on the module)
*/

#include <SPI.h>
#include <RF24.h>

// ----------------- PIN MAP (match your working wiring) -----------------
const int PIN_LED1 = 25;  // pairs with Fan1 / Room A
const int PIN_LED2 = 26;  // pairs with Fan2 / Room B
const int PIN_FAN1 = 27;  // Room A fan (via NPN transistor base -> 1k)
const int PIN_FAN2 = 33;  // Room B fan (via NPN transistor base -> 1k)
const int PIN_STATUS = 2; // onboard LED

// NRF24L01+ (VSPI pins are hardware: SCK=18, MISO=19, MOSI=23)
#define NRF_CE   4
#define NRF_CSN  5
RF24 radio(NRF_CE, NRF_CSN);

// Use two addressing pipes: sensor writes to "SENS1", control listens there.
// (Make sure your friend’s sensor hub uses the same addresses.)
const byte PIPE_RX[6] = "SENS1";

// ----------------- PAYLOAD DEFINITION -----------------
struct Telemetry {
  uint32_t ms;       // millis() on sender
  int16_t distA_cm;  // Room A distance in cm (-1 if timeout)
  int16_t distB_cm;  // Room B distance in cm (-1 if timeout)
};

// ----------------- STATE -----------------
unsigned long lastPacketMs = 0;
bool roomA_occupied = false;
bool roomB_occupied = false;

// Tunables
const int   OCCUPIED_CUTOFF_CM = 150; // <= this => "occupied"
const int   MIN_VALID_CM       = 2;   // filter nonsense
const int   MAX_VALID_CM       = 400; // ~4m cap
const unsigned long PACKET_TIMEOUT_MS = 10'000; // lose packets -> fail safe

// ----------------- HELPERS -----------------
void setFanLed(int fanPin, int ledPin, bool on) {
  // With NPN driver for fans, HIGH on the GPIO = fan ON.
  digitalWrite(fanPin, on ? HIGH : LOW);
  digitalWrite(ledPin, on ? HIGH : LOW);
}

bool distanceMeansOccupied(int16_t cm) {
  if (cm < 0) return false; // timeout/out-of-range
  if (cm < MIN_VALID_CM || cm > MAX_VALID_CM) return false;
  return (cm <= OCCUPIED_CUTOFF_CM);
}

// ----------------- SETUP -----------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_FAN1, OUTPUT);
  pinMode(PIN_FAN2, OUTPUT);
  pinMode(PIN_STATUS, OUTPUT);

  // Start with everything OFF
  setFanLed(PIN_FAN1, PIN_LED1, false);
  setFanLed(PIN_FAN2, PIN_LED2, false);

  // NRF init
  if (!radio.begin()) {
    Serial.println("NRF init failed! Check wiring/3.3V/CE/CSN.");
  }
  radio.setPALevel(RF24_PA_LOW);    // start conservative (can try RF24_PA_HIGH if needed)
  radio.setDataRate(RF24_1MBPS);    // robust default
  radio.setChannel(108);            // avoid WiFi band center (optional)
  radio.openReadingPipe(1, PIPE_RX);
  radio.startListening();

  Serial.println("Control Hub ready: listening for telemetry on pipe 'SENS1'");
}

// ----------------- LOOP -----------------
void loop() {
  // Read any NRF packets
  while (radio.available()) {
    Telemetry t;
    radio.read(&t, sizeof(t));
    lastPacketMs = millis();

    // Decide occupancy per room
    roomA_occupied = distanceMeansOccupied(t.distA_cm);
    roomB_occupied = distanceMeansOccupied(t.distB_cm);

    // Actuate
    setFanLed(PIN_FAN1, PIN_LED1, roomA_occupied);
    setFanLed(PIN_FAN2, PIN_LED2, roomB_occupied);

    // Blink status briefly on packet
    digitalWrite(PIN_STATUS, HIGH);
    delay(20);
    digitalWrite(PIN_STATUS, LOW);

    // Debug
    Serial.print("RX  A:"); Serial.print(t.distA_cm);
    Serial.print("cm  B:"); Serial.print(t.distB_cm);
    Serial.print("cm  ->  occA:"); Serial.print(roomA_occupied);
    Serial.print(" occB:"); Serial.println(roomB_occupied);
  }

  // Fail-safe: if no packets for a while, turn everything off
  if (millis() - lastPacketMs > PACKET_TIMEOUT_MS) {
    setFanLed(PIN_FAN1, PIN_LED1, false);
    setFanLed(PIN_FAN2, PIN_LED2, false);
  }

  // Small idle delay
  delay(5);
}
