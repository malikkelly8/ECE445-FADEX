#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

// FadeX ESP32 identity test.
//
// Flash this same sketch to either the new homebase ESP32 or the new handheld
// ESP32. Open Serial Monitor at 115200 baud and copy the "Wi-Fi STA MAC" line.
// That station MAC is the address used by ESP-NOW.

constexpr int STATUS_LED_PIN = 2;
constexpr unsigned long PRINT_INTERVAL_MS = 5000;
constexpr unsigned long BLINK_INTERVAL_MS = 500;

unsigned long lastPrintMs = 0;
unsigned long lastBlinkMs = 0;
bool ledState = false;

String formatMac(const uint8_t mac[6]) {
  char text[18];
  snprintf(
    text,
    sizeof(text),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0],
    mac[1],
    mac[2],
    mac[3],
    mac[4],
    mac[5]
  );
  return String(text);
}

void printIdentity() {
  uint8_t staMac[6] = {};
  uint8_t softApMac[6] = {};

  esp_wifi_get_mac(WIFI_IF_STA, staMac);
  esp_wifi_get_mac(WIFI_IF_AP, softApMac);

  Serial.println();
  Serial.println("FadeX ESP32 MAC / identity test");
  Serial.print("Wi-Fi STA MAC / ESP-NOW MAC: ");
  Serial.println(formatMac(staMac));
  Serial.print("Wi-Fi SoftAP MAC:             ");
  Serial.println(formatMac(softApMac));
  Serial.print("Chip model:                   ");
  Serial.println(ESP.getChipModel());
  Serial.print("Chip revision:                ");
  Serial.println(ESP.getChipRevision());
  Serial.print("CPU frequency:                ");
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println(" MHz");
  Serial.print("Flash size:                   ");
  Serial.print(ESP.getFlashChipSize() / 1024);
  Serial.println(" KB");
  Serial.println();
  Serial.println("Copy the Wi-Fi STA MAC for ESP-NOW pairing.");
  Serial.println("Type PRINT in Serial Monitor to print this again.");
}

void handleSerial() {
  if (!Serial.available()) {
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();

  if (command.equalsIgnoreCase("print")) {
    printIdentity();
  } else if (command.length() > 0) {
    Serial.println("Unknown command. Type PRINT to show the MAC addresses again.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);

  printIdentity();
}

void loop() {
  handleSerial();

  const unsigned long now = millis();
  if (now - lastBlinkMs >= BLINK_INTERVAL_MS) {
    lastBlinkMs = now;
    ledState = !ledState;
    digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
  }

  if (now - lastPrintMs >= PRINT_INTERVAL_MS) {
    lastPrintMs = now;
    printIdentity();
  }
}
