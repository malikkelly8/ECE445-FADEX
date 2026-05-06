#include <Arduino.h>
#include "HX711.h"

// Round-robin load-cell validation sketch for the FadeX PCB.
//
// Goal:
// - wait for a known weight on Load Cell 1
// - once it stays near the target weight for about 2 seconds,
//   advance to Load Cell 2
// - continue through Load Cell 3 and Load Cell 4
// - wrap back to Load Cell 1 and repeat forever
//
// This is meant for a quick final-demo hardware check after calibration.

constexpr int HX711_SCK_PIN = 23;
constexpr int LOADCELL_DATA_PINS[] = {25, 26, 27, 13};
constexpr const char *LOADCELL_NAMES[] = {"LCA1", "LCA2", "LCA3", "LCA4"};
constexpr const char *LOADCELL_CONNECTORS[] = {"J2", "J10", "J13", "J5"};
constexpr const char *LOADCELL_DATA_NETS[] = {"DAT1", "DAT2", "DAT3", "DAT4"};
constexpr int LOADCELL_ESP32_MODULE_PINS[] = {10, 11, 12, 16};
constexpr int HX711_SCK_ESP32_MODULE_PIN = 37;

// Replace these with the real per-channel calibration factors once known.
constexpr float LOADCELL_CAL_FACTORS[] = {
  3053.60f,
  3109.60f,
  2951.40f,
  2925.40f
};

constexpr float TARGET_WEIGHT_G = 33.4f;
constexpr float TARGET_TOLERANCE_G = 0.03f;
constexpr unsigned long HOLD_TIME_MS = 2000;
constexpr unsigned long PRINT_INTERVAL_MS = 150;
constexpr unsigned long TARE_READY_TIMEOUT_MS = 3000;

HX711 loadCell1;
HX711 loadCell2;
HX711 loadCell3;
HX711 loadCell4;

HX711 *const LOAD_CELLS[] = {
  &loadCell1,
  &loadCell2,
  &loadCell3,
  &loadCell4
};

uint8_t currentTargetIndex = 0;
unsigned long holdStartMs = 0;
unsigned long lastPrintMs = 0;
float lastKnownReadings[4] = {0.0f, 0.0f, 0.0f, 0.0f};
bool loadCellTared[4] = {false, false, false, false};

bool waitForReady(HX711 *scale, unsigned long timeoutMs) {
  const unsigned long startMs = millis();
  while (!scale->is_ready() && (millis() - startMs < timeoutMs)) {
    delay(10);
  }
  return scale->is_ready();
}

void tareLoadCell(uint8_t index) {
  HX711 *scale = LOAD_CELLS[index];

  Serial.print("  ");
  Serial.print(LOADCELL_NAMES[index]);
  Serial.print(": ");

  if (!waitForReady(scale, TARE_READY_TIMEOUT_MS)) {
    Serial.println("not ready for tare");
    loadCellTared[index] = false;
    return;
  }

  scale->set_scale();
  scale->tare(20);
  scale->set_scale(LOADCELL_CAL_FACTORS[index]);
  lastKnownReadings[index] = 0.0f;
  loadCellTared[index] = true;
  Serial.println("tared");
}

void tareAllLoadCells() {
  Serial.println();
  Serial.println("Leave every load cell untouched. Taring all channels...");
  delay(1500);

  for (uint8_t i = 0; i < 4; ++i) {
    tareLoadCell(i);
  }

  holdStartMs = 0;
  Serial.println("Tare complete.");
  Serial.println();
}

float readLoadCellUnits(uint8_t index) {
  HX711 *scale = LOAD_CELLS[index];
  if (!scale->is_ready()) {
    return NAN;
  }
  return scale->get_units(3);
}

void printPrompt() {
  Serial.println();
  Serial.print("Target channel: ");
  Serial.print(LOADCELL_NAMES[currentTargetIndex]);
  Serial.print(" | Place ");
  Serial.print(TARGET_WEIGHT_G, 1);
  Serial.print(" g on this cell and hold for ");
  Serial.print(HOLD_TIME_MS / 1000.0f, 1);
  Serial.println(" s");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("FadeX round-robin load-cell test");
  Serial.println("PCB load-cell connector map:");
  Serial.print("  Shared SCLK -> GPIO");
  Serial.print(HX711_SCK_PIN);
  Serial.print(" / ESP32 module pin ");
  Serial.println(HX711_SCK_ESP32_MODULE_PIN);
  Serial.println("Commands:");
  Serial.println("  t = tare all load cells");
  Serial.println("  r = restart sequence at LCA1");
  Serial.println();

  for (uint8_t i = 0; i < 4; ++i) {
    HX711 *scale = LOAD_CELLS[i];
    scale->begin(LOADCELL_DATA_PINS[i], HX711_SCK_PIN);

    Serial.print(LOADCELL_NAMES[i]);
    Serial.print(" / ");
    Serial.print(LOADCELL_CONNECTORS[i]);
    Serial.print(" / ");
    Serial.print(LOADCELL_DATA_NETS[i]);
    Serial.print(" -> DOUT GPIO");
    Serial.print(LOADCELL_DATA_PINS[i]);
    Serial.print(" / ESP32 module pin ");
    Serial.print(LOADCELL_ESP32_MODULE_PINS[i]);

    if (!scale->is_ready()) {
      Serial.println(" | not ready");
      loadCellTared[i] = false;
      continue;
    }

    scale->set_scale(LOADCELL_CAL_FACTORS[i]);
    loadCellTared[i] = false;
    Serial.println(" | ready");
  }

  tareAllLoadCells();
  printPrompt();
}

void loop() {
  if (Serial.available() > 0) {
    char input = static_cast<char>(Serial.read());
    if (input == 't' || input == 'T') {
      tareAllLoadCells();
      printPrompt();
    } else if (input == 'r' || input == 'R') {
      currentTargetIndex = 0;
      holdStartMs = 0;
      Serial.println();
      Serial.println("Sequence reset to LCA1.");
      printPrompt();
    }
  }

  const unsigned long now = millis();
  if (now - lastPrintMs < PRINT_INTERVAL_MS) {
    return;
  }
  lastPrintMs = now;

  float readings[4] = {};
  for (uint8_t i = 0; i < 4; ++i) {
    const float reading = readLoadCellUnits(i);
    if (!isnan(reading)) {
      lastKnownReadings[i] = reading;
    }
    readings[i] = lastKnownReadings[i];
  }

  const float targetReading = readings[currentTargetIndex];
  const bool targetInWindow =
    fabsf(targetReading - TARGET_WEIGHT_G) <= TARGET_TOLERANCE_G;

  if (targetInWindow) {
    if (holdStartMs == 0) {
      holdStartMs = now;
    }
  } else {
    holdStartMs = 0;
  }

  Serial.print("Target ");
  Serial.print(LOADCELL_NAMES[currentTargetIndex]);
  Serial.print(" | ");

  for (uint8_t i = 0; i < 4; ++i) {
    Serial.print(LOADCELL_NAMES[i]);
    Serial.print("=");
    Serial.print(readings[i], 2);
    if (!loadCellTared[i]) {
      Serial.print("(UNTARED)");
    }

    if (i < 3) {
      Serial.print(" | ");
    }
  }

  if (targetInWindow) {
    const unsigned long heldMs = now - holdStartMs;
    Serial.print(" | hold=");
    Serial.print(heldMs / 1000.0f, 2);
    Serial.print(" / ");
    Serial.print(HOLD_TIME_MS / 1000.0f, 1);
    Serial.print(" s");

    if (heldMs >= HOLD_TIME_MS) {
      Serial.println();
      Serial.print("Accepted ");
      Serial.print(LOADCELL_NAMES[currentTargetIndex]);
      Serial.println(".");

      currentTargetIndex = (currentTargetIndex + 1) % 4;
      holdStartMs = 0;
      printPrompt();
      return;
    }
  }

  Serial.println();
}
