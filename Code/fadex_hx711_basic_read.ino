#include <Arduino.h>
#include "HX711.h"

// HX711 basic read sketch adapted for the FadeX homebase ESP32.
//
// We would use this after the calibration sketch once we found a good
// calibration factor for our specific TAL221 + HX711 setup.
//
// PCB connector map:
//   J2  Load Cell 1 connector -> DAT1 / GPIO25
//   J10 Load Cell 2 connector -> DAT2 / GPIO26
//   J13 Load Cell 3 connector -> DAT3 / GPIO27
//   J5  Load Cell 4 connector -> DAT4 / GPIO13
//   All four connectors share SCLK / GPIO23
//
// Set ACTIVE_LOAD_CELL to whichever channel you want to verify.

constexpr int HX711_SCK = 23;
constexpr int LOADCELL_DATA_PINS[] = {25, 26, 27, 13};
constexpr const char *LOADCELL_NAMES[] = {"LCA1", "LCA2", "LCA3", "LCA4"};
constexpr const char *LOADCELL_CONNECTORS[] = {"J2", "J10", "J13", "J5"};
constexpr const char *LOADCELL_DATA_NETS[] = {"DAT1", "DAT2", "DAT3", "DAT4"};
constexpr int LOADCELL_ESP32_MODULE_PINS[] = {10, 11, 12, 16};
constexpr int HX711_SCK_ESP32_MODULE_PIN = 37;

constexpr int ACTIVE_LOAD_CELL = 4;
constexpr int HX711_DOUT = LOADCELL_DATA_PINS[ACTIVE_LOAD_CELL - 1];
constexpr const char *ACTIVE_LOAD_CELL_NAME = LOADCELL_NAMES[ACTIVE_LOAD_CELL - 1];
constexpr const char *ACTIVE_LOAD_CELL_CONNECTOR = LOADCELL_CONNECTORS[ACTIVE_LOAD_CELL - 1];
constexpr const char *ACTIVE_LOAD_CELL_DATA_NET = LOADCELL_DATA_NETS[ACTIVE_LOAD_CELL - 1];
constexpr int ACTIVE_LOAD_CELL_ESP32_MODULE_PIN = LOADCELL_ESP32_MODULE_PINS[ACTIVE_LOAD_CELL - 1];

// Replace this with the value you found for the selected load cell.
constexpr float CALIBRATION_FACTOR = 2925.40f;

HX711 scale;

void tareScale() {
  Serial.println("Leave the load cell completely untouched. Taring...");
  delay(1500);
  scale.set_scale();
  scale.tare(20);
  scale.set_scale(CALIBRATION_FACTOR);
  delay(250);
  Serial.println("Tare complete.");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("HX711 basic read sketch for FadeX PCB");
  Serial.print("Active load cell: ");
  Serial.println(ACTIVE_LOAD_CELL_NAME);
  Serial.print("PCB connector: ");
  Serial.println(ACTIVE_LOAD_CELL_CONNECTOR);
  Serial.print("HX711 data net: ");
  Serial.println(ACTIVE_LOAD_CELL_DATA_NET);
  Serial.print("DOUT Arduino pin: GPIO");
  Serial.println(HX711_DOUT);
  Serial.print("DOUT ESP32 module pin: ");
  Serial.println(ACTIVE_LOAD_CELL_ESP32_MODULE_PIN);
  Serial.print("Shared SCK Arduino pin: GPIO");
  Serial.println(HX711_SCK);
  Serial.print("Shared SCK ESP32 module pin: ");
  Serial.println(HX711_SCK_ESP32_MODULE_PIN);

  scale.begin(HX711_DOUT, HX711_SCK);

  if (!scale.is_ready()) {
    Serial.println("HX711 is not ready. Check power, ground, DOUT, and SCK.");
    return;
  }

  tareScale();

  Serial.println("Readings:");
  Serial.println("Press 't' to tare again.");
}

void loop() {
  if (!scale.is_ready()) {
    Serial.println("HX711 not ready.");
    delay(500);
    return;
  }

  if (Serial.available() > 0) {
    char input = static_cast<char>(Serial.read());
    if (input == 't' || input == 'T') {
      tareScale();
    }
  }

  const long raw = scale.read_average(3);
  const float reading = scale.get_units(5);

  Serial.print("Raw: ");
  Serial.print(raw);
  Serial.print(" | Reading: ");
  Serial.print(reading, 2);
  Serial.print(" | calibration_factor: ");
  Serial.println(CALIBRATION_FACTOR, 2);

  delay(250);
}
