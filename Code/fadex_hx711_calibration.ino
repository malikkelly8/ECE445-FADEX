#include <Arduino.h>
#include "HX711.h"

// HX711 calibration sketch tailored to the FadeX PCB.
//
// This keeps the same basic SparkFun workflow, but it is adapted for:
// - ESP32 serial speed
// - the shared HX711 clock on the PCB
// - one of four PCB-connected load-cell amplifier data lines
//
// Use this sketch one channel at a time:
// 1. Set ACTIVE_LOAD_CELL to 1, 2, 3, or 4
// 2. Upload the sketch
// 3. Remove all weight and let the sketch tare the cell
// 4. Place a known weight on that one load cell
// 5. Press '+' / 'a' or '-' / 'z' until the reading matches the known weight
// 6. Record the printed calibration factor for that channel
// 7. Repeat for the next load cell
//
// PCB connector map:
//   J2  Load Cell 1 connector -> DAT1 / GPIO25
//   J10 Load Cell 2 connector -> DAT2 / GPIO26
//   J13 Load Cell 3 connector -> DAT3 / GPIO27
//   J5  Load Cell 4 connector -> DAT4 / GPIO13
//   All four connectors share SCLK / GPIO23

constexpr int HX711_SCK = 23;
constexpr int LOADCELL_DATA_PINS[] = {25, 26, 27, 13};
constexpr const char *LOADCELL_NAMES[] = {"LCA1", "LCA2", "LCA3", "LCA4"};
constexpr const char *LOADCELL_CONNECTORS[] = {"J2", "J10", "J13", "J5"};
constexpr const char *LOADCELL_DATA_NETS[] = {"DAT1", "DAT2", "DAT3", "DAT4"};
constexpr int LOADCELL_ESP32_MODULE_PINS[] = {10, 11, 12, 16};
constexpr int HX711_SCK_ESP32_MODULE_PIN = 37;

// Change this from 1 to 4 depending on which load cell you are calibrating.
constexpr int ACTIVE_LOAD_CELL = 1;
// constexpr int ACTIVE_LOAD_CELL = 2;
// constexpr int ACTIVE_LOAD_CELL = 3;
// constexpr int ACTIVE_LOAD_CELL = 4;
constexpr int HX711_DOUT = LOADCELL_DATA_PINS[ACTIVE_LOAD_CELL - 1];
constexpr const char *ACTIVE_LOAD_CELL_NAME = LOADCELL_NAMES[ACTIVE_LOAD_CELL - 1];
constexpr const char *ACTIVE_LOAD_CELL_CONNECTOR = LOADCELL_CONNECTORS[ACTIVE_LOAD_CELL - 1];
constexpr const char *ACTIVE_LOAD_CELL_DATA_NET = LOADCELL_DATA_NETS[ACTIVE_LOAD_CELL - 1];
constexpr int ACTIVE_LOAD_CELL_ESP32_MODULE_PIN = LOADCELL_ESP32_MODULE_PINS[ACTIVE_LOAD_CELL - 1];

HX711 scale;

// This is only a starting guess. The correct value may be positive or negative.
float calibration_factor = 3053.6f;
// float calibration_factor = 3109.6f;
// float calibration_factor = 2951.4f;
// float calibration_factor = 2925.4f;
float calibration_step = 10.0f;

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("HX711 calibration sketch for FadeX PCB");
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
  Serial.println("Remove all weight from the load cell.");
  Serial.println("After readings begin, place a known weight on the load cell.");
  Serial.println("Press '+' or 'a' to increase the calibration factor.");
  Serial.println("Press '-' or 'z' to decrease the calibration factor.");
  Serial.println("Press ']' to increase adjustment step.");
  Serial.println("Press '[' to decrease adjustment step.");
  Serial.println("Press 't' to tare again.");
  Serial.println("Change the printed units label to grams if you prefer.");

  scale.begin(HX711_DOUT, HX711_SCK);

  if (!scale.is_ready()) {
    Serial.println("HX711 is not ready. Check power, ground, DOUT, and SCK.");
    return;
  }

  scale.set_scale();
  scale.tare();

  long zero_factor = scale.read_average();
  Serial.print("Zero factor: ");
  Serial.println(zero_factor);
}

void loop() {
  if (!scale.is_ready()) {
    Serial.println("HX711 not ready.");
    delay(500);
    return;
  }

  scale.set_scale(calibration_factor);

  const float reading = scale.get_units(5);

  Serial.print("Reading: ");
  Serial.print(reading, 2);
  Serial.print(" units");
  Serial.print(" | calibration_factor: ");
  Serial.println(calibration_factor, 2);

  if (Serial.available() > 0) {
    char input = static_cast<char>(Serial.read());
    if (input == '+' || input == 'a') {
      calibration_factor += calibration_step;
    } else if (input == '-' || input == 'z') {
      calibration_factor -= calibration_step;
    } else if (input == ']') {
      calibration_step *= 10.0f;
      Serial.print("Calibration step is now ");
      Serial.println(calibration_step, 2);
    } else if (input == '[') {
      calibration_step /= 10.0f;
      if (calibration_step < 0.1f) {
        calibration_step = 0.1f;
      }
      Serial.print("Calibration step is now ");
      Serial.println(calibration_step, 2);
    } else if (input == 't' || input == 'T') {
      scale.tare();
      Serial.println("Scale tared again.");
    }
  }

  delay(250);
}
