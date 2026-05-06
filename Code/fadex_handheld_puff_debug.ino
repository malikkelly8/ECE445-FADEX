#include <Arduino.h>

// Quick handheld puff-input diagnostic.
// Use this on the breadboard to verify whether the ESP32 can see the puff signal.

constexpr int PIN_PUFF_SENSOR = 13;

// The current FadeX handheld code expects the puff signal to go LOW during a puff.
// If your sensor outputs HIGH during a puff, change this to false.
constexpr bool PUFF_ACTIVE_LOW = true;

bool lastActive = false;
uint32_t puffCount = 0;
unsigned long puffStartMs = 0;
unsigned long lastStatusMs = 0;

bool puffIsActive() {
  const int raw = digitalRead(PIN_PUFF_SENSOR);
  return PUFF_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_PUFF_SENSOR, INPUT_PULLUP);

  Serial.println();
  Serial.println("FadeX handheld puff input debug");
  Serial.println("Breadboard puff pin: GPIO13");
  Serial.println("Expected behavior: raw pin should change when you puff/press sensor.");
  Serial.println("Current polarity: active LOW");
  Serial.println();
}

void loop() {
  const int raw = digitalRead(PIN_PUFF_SENSOR);
  const bool active = puffIsActive();

  if (active && !lastActive) {
    puffStartMs = millis();
    Serial.println("PUFF START detected");
  }

  if (!active && lastActive) {
    const unsigned long durationMs = millis() - puffStartMs;
    if (durationMs >= 50) {
      puffCount++;
      Serial.print("PUFF END detected | duration ms: ");
      Serial.print(durationMs);
      Serial.print(" | puff count: ");
      Serial.println(puffCount);
    } else {
      Serial.print("Ignored short pulse ms: ");
      Serial.println(durationMs);
    }
  }

  lastActive = active;

  if (millis() - lastStatusMs >= 500) {
    lastStatusMs = millis();
    Serial.print("GPIO13 raw=");
    Serial.print(raw == HIGH ? "HIGH" : "LOW");
    Serial.print(" | interpreted=");
    Serial.println(active ? "PUFF ACTIVE" : "idle");
  }
}
