#include <Arduino.h>

// This sketch outputs a continuous 50% duty-cycle square wave on one GPIO.
// Use it to verify that:
// 1. the ESP32 can be programmed successfully, and
// 2. the chosen output pin is really connected where you think it is.
//
// Probe the selected TEST_GPIO with an oscilloscope after upload.
// The scope should show a square wave at SQUARE_WAVE_HZ.
//
// Important note:
// The "27, 28, 31, 30, 33, 36" numbers from the ESP32-WROOM schematic
// are module pin numbers, not Arduino GPIO numbers.
// Those map to:
//   module pin 27 -> GPIO16
//   module pin 28 -> GPIO17
//   module pin 31 -> GPIO19
//   module pin 30 -> GPIO18
//   module pin 33 -> GPIO21
//   module pin 36 -> GPIO22

constexpr int TEST_GPIO = 16;
constexpr uint32_t SQUARE_WAVE_HZ = 1000;
constexpr uint8_t LEDC_CHANNEL = 0;
constexpr uint8_t LEDC_RESOLUTION_BITS = 8;

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("FadeX square-wave GPIO test");
  Serial.print("Output GPIO: ");
  Serial.println(TEST_GPIO);
  Serial.print("Frequency (Hz): ");
  Serial.println(SQUARE_WAVE_HZ);
  Serial.println("Probe this pin with the oscilloscope.");

  ledcSetup(LEDC_CHANNEL, SQUARE_WAVE_HZ, LEDC_RESOLUTION_BITS);
  ledcAttachPin(TEST_GPIO, LEDC_CHANNEL);

  // 50% duty cycle for a clean square wave.
  ledcWrite(LEDC_CHANNEL, 128);
}

void loop() {
  // Nothing else to do. The hardware PWM peripheral keeps the square wave running.
  delay(1000);
}
