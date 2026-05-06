#include <Arduino.h>

// FadeX homebase photodiode / ADC bring-up test
//
// What this verifies:
// 1. The homebase PCB net "PDO" is really connected to the expected ESP32 pin.
// 2. The ESP32 ADC can measure the photodiode amplifier output.
// 3. The serial monitor reports a stable raw ADC count and voltage estimate.
//
// Pin mapping from the homebase schematic:
//   ESP32 module pin 4 -> GPIO36 / ADC1_CH0 -> PDO
//
// Why GPIO36 is okay here:
//   GPIO36 is input-only, which is exactly what we want for an ADC input.

//  Test
//   - feed a known voltage into the ESP32 ADC input before the analog front-end
//     is connected, or
//   - validate the assembled board by shining light on the photodiode and
//     watching PDO move.
//


constexpr int PHOTO_ADC_PIN = 36;
constexpr uint8_t ADC_RESOLUTION_BITS = 12;
constexpr unsigned long PRINT_PERIOD_MS = 250;
constexpr size_t ADC_AVERAGE_COUNT = 32;

void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(ADC_RESOLUTION_BITS);
  analogSetPinAttenuation(PHOTO_ADC_PIN, ADC_11db);

  Serial.println();
  Serial.println("FadeX photodiode ADC test");
  Serial.println("Homebase schematic mapping:");
  Serial.println("  PDO -> GPIO36 / ADC1_CH0");
  Serial.println("ADC configuration:");
  Serial.println("  Resolution: 12 bits");
  Serial.println("  Attenuation: 11 dB");
  Serial.println("Expected use:");
  Serial.println("  Read the OPA380 photodiode amplifier output on PDO.");
  Serial.println("Reminder:");
  Serial.println("  Keep any external test voltage between 0 V and 3.3 V.");
  Serial.println("  Avoid back-driving the assembled op-amp output.");
  Serial.println();
}

void loop() {
  uint32_t rawSum = 0;
  uint32_t milliVoltSum = 0;

  // Average several readings so the serial output is easier to interpret.
  for (size_t i = 0; i < ADC_AVERAGE_COUNT; ++i) {
    rawSum += analogRead(PHOTO_ADC_PIN);
    milliVoltSum += analogReadMilliVolts(PHOTO_ADC_PIN);
    delayMicroseconds(200);
  }

  const float rawAverage = static_cast<float>(rawSum) / ADC_AVERAGE_COUNT;
  const float milliVoltAverage =
      static_cast<float>(milliVoltSum) / ADC_AVERAGE_COUNT;
  const float voltage = milliVoltAverage / 1000.0f;

  Serial.print("PDO raw avg: ");
  Serial.print(rawAverage, 1);
  Serial.print(" / 4095");
  Serial.print(" | PDO voltage avg: ");
  Serial.print(voltage, 3);
  Serial.println(" V");

  delay(PRINT_PERIOD_MS);
}
