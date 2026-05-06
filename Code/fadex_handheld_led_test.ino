#include <Arduino.h>

// Simple LED blink test for the handheld ESP32 development board.
#ifndef LED_BUILTIN
constexpr int STATUS_LED_PIN = 2; // LED PIN for esp32
#else
constexpr int STATUS_LED_PIN = LED_BUILTIN;
#endif

constexpr unsigned long BLINK_ON_MS = 500;
constexpr unsigned long BLINK_OFF_MS = 500;

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  Serial.println();
  Serial.println("FadeX handheld ESP32 LED test");
  Serial.print("Blinking LED on GPIO ");
  Serial.println(STATUS_LED_PIN);
}

void loop() {
  digitalWrite(STATUS_LED_PIN, HIGH);
  delay(BLINK_ON_MS);
  digitalWrite(STATUS_LED_PIN, LOW);
  delay(BLINK_OFF_MS);
}
