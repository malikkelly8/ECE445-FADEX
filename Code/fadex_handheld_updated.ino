#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <string.h>

// FadeX simple handheld.
//
// This version is intentionally based on the GPIO input test that worked:
// puff input -> GPIO22
// other side of the puff/button switch -> GND
// INPUT_PULLUP keeps the pin HIGH normally and LOW when pressed.
//
// Every valid puff input hold/release adds one puff to the current session. Type
// SYNC or SEND in Serial Monitor when you want to send the whole session.

constexpr int PUFF_INPUT_PIN = 22;
constexpr int PWM_OUTPUT_PIN = 23;
constexpr unsigned long DEBOUNCE_MS = 50;
constexpr unsigned long MIN_PUFF_MS = 50;
constexpr unsigned long SEND_COMPLETE_TIMEOUT_MS = 1500;
constexpr unsigned long ACK_TIMEOUT_MS = 2500;

// Homebase Wi-Fi STA / ESP-NOW MAC: 88:57:21:7A:08:2C
uint8_t HOMEBASE_MAC[] = {0x88, 0x57, 0x21, 0x7A, 0x08, 0x2C};

// This handheld Wi-Fi STA / PCB ESP-NOW MAC: 88:13:BF:55:35:F0
constexpr uint8_t EXPECTED_THIS_HANDHELD_MAC[] = {
  0x88, 0x13, 0xBF, 0x55, 0x35, 0xF0
};

// Must match the Wi-Fi channel used by the Blynk-connected homebase.
// IllinoisNet was observed on channel 11.
constexpr uint8_t ESPNOW_WIFI_CHANNEL = 11;

struct SessionData {
  uint32_t totalPuffs;
  float totalDuration;
  float reservedScore;
  uint8_t reservedCode;
  uint32_t sessionId;
};

struct SyncAck {
  bool accepted;
  uint32_t sessionId;
  float adaptiveMultiplier;
  float appliedConcentration;
  char scenarioId[4];
};

bool lastRawPuffInputState = HIGH;
bool stablePuffInputState = HIGH;
bool puffTimingActive = false;
unsigned long lastRawChangeMs = 0;
unsigned long puffStartMs = 0;

uint32_t sessionPuffs = 0;
float sessionDurationSeconds = 0.0f;
uint32_t nextSessionId = 1;

volatile bool sendComplete = false;
volatile bool sendSucceeded = false;
volatile bool ackReceived = false;
SyncAck lastAck = {};

void printMacAddress(const uint8_t *mac) {
  for (uint8_t i = 0; i < 6; ++i) {
    if (i > 0) {
      Serial.print(":");
    }
    if (mac[i] < 0x10) {
      Serial.print("0");
    }
    Serial.print(mac[i], HEX);
  }
}

bool localMacMatchesExpected() {
  uint8_t actualMac[6] = {};
  WiFi.macAddress(actualMac);
  return memcmp(actualMac, EXPECTED_THIS_HANDHELD_MAC, sizeof(actualMac)) == 0;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  (void)info;
  sendSucceeded = (status == ESP_NOW_SEND_SUCCESS);
  sendComplete = true;
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  (void)info;
  if (len != sizeof(SyncAck)) {
    return;
  }

  memcpy(&lastAck, data, sizeof(lastAck));
  ackReceived = true;
}
#else
void onDataSent(const uint8_t *macAddr, esp_now_send_status_t status) {
  (void)macAddr;
  sendSucceeded = (status == ESP_NOW_SEND_SUCCESS);
  sendComplete = true;
}

void onDataRecv(const uint8_t *macAddr, const uint8_t *data, int len) {
  (void)macAddr;
  if (len != sizeof(SyncAck)) {
    return;
  }

  memcpy(&lastAck, data, sizeof(lastAck));
  ackReceived = true;
}
#endif

bool ensurePeerRegistered() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, HOMEBASE_MAC, 6);
  peerInfo.channel = ESPNOW_WIFI_CHANNEL;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (esp_now_is_peer_exist(HOMEBASE_MAC)) {
    return true;
  }

  return esp_now_add_peer(&peerInfo) == ESP_OK;
}

void printStatus() {
  Serial.println();
  Serial.println("=== FadeX Handheld Status ===");
  Serial.print("This handheld STA MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Expected handheld STA MAC: ");
  printMacAddress(EXPECTED_THIS_HANDHELD_MAC);
  Serial.println();
  Serial.print("Homebase peer STA MAC: ");
  printMacAddress(HOMEBASE_MAC);
  Serial.println();
  Serial.print("ESP-NOW channel: ");
  Serial.println(ESPNOW_WIFI_CHANNEL);
  Serial.print("Puff input GPIO: ");
  Serial.println(PUFF_INPUT_PIN);
  Serial.print("PWM output GPIO: ");
  Serial.println(PWM_OUTPUT_PIN);
  Serial.println("Puff input behavior: active LOW starts timing, release adds one puff");
  Serial.print("Current session puffs: ");
  Serial.println(sessionPuffs);
  Serial.print("Current session duration: ");
  Serial.print(sessionDurationSeconds, 2);
  Serial.println(" s");
  Serial.print("Next session ID: ");
  Serial.println(nextSessionId);

  if (!localMacMatchesExpected()) {
    Serial.println("WARNING: This ESP32 MAC does not match the expected handheld MAC.");
  }
}

void printHelp() {
  Serial.println();
  Serial.println("FadeX simple handheld commands:");
  Serial.println("  Hold/release GPIO22 puff input -> add one puff to this session");
  Serial.println("  STATUS                      -> show current session totals");
  Serial.println("  PUFF <seconds>              -> manually add one puff for testing");
  Serial.println("  SYNC or SEND                -> send current session to homebase");
  Serial.println("  RESET                       -> clear current session totals");
  Serial.println("  HELP                        -> print this menu");
  Serial.println();
}

void addPuffToSession(float puffSeconds) {
  if (puffSeconds <= 0.0f) {
    Serial.println("Puff duration must be positive.");
    return;
  }

  sessionPuffs++;
  sessionDurationSeconds += puffSeconds;

  Serial.print("Puff added | session puffs=");
  Serial.print(sessionPuffs);
  Serial.print(" | session duration=");
  Serial.print(sessionDurationSeconds, 2);
  Serial.println(" s");
}

void clearCurrentSession() {
  sessionPuffs = 0;
  sessionDurationSeconds = 0.0f;
  Serial.println("Current handheld session cleared.");
}

void setPwmOutput(bool active) {
  digitalWrite(PWM_OUTPUT_PIN, active ? HIGH : LOW);
}

void syncCurrentSession() {
  if (sessionPuffs == 0 || sessionDurationSeconds <= 0.0f) {
    Serial.println("No puffs in the current session yet. Activate GPIO22 first.");
    return;
  }

  SessionData session = {};
  session.totalPuffs = sessionPuffs;
  session.totalDuration = sessionDurationSeconds;
  // Reserved compatibility fields: homebase interprets these as Neutral/None.
  session.reservedScore = 0.0f;
  session.reservedCode = 0;
  session.sessionId = nextSessionId;

  Serial.println();
  Serial.println("--- HANDHELD SESSION SYNC ---");
  Serial.print("Session ID: ");
  Serial.println(session.sessionId);
  Serial.print("Puffs: ");
  Serial.println(session.totalPuffs);
  Serial.print("Duration (s): ");
  Serial.println(session.totalDuration, 2);

  sendComplete = false;
  sendSucceeded = false;
  ackReceived = false;

  const esp_err_t result =
    esp_now_send(HOMEBASE_MAC, reinterpret_cast<uint8_t *>(&session), sizeof(session));

  if (result != ESP_OK) {
    Serial.println("ESP-NOW send could not start. Check peer MAC/channel.");
    return;
  }

  const unsigned long sendStartMs = millis();
  while (!sendComplete && millis() - sendStartMs < SEND_COMPLETE_TIMEOUT_MS) {
    delay(10);
  }

  if (!sendComplete || !sendSucceeded) {
    Serial.println("ESP-NOW send failed. Check homebase power/channel/distance.");
    return;
  }

  Serial.println("ESP-NOW send succeeded. Waiting for homebase ACK...");

  const unsigned long ackStartMs = millis();
  while (!ackReceived && millis() - ackStartMs < ACK_TIMEOUT_MS) {
    delay(10);
  }

  if (ackReceived && lastAck.sessionId == session.sessionId) {
    Serial.print("ACK received | accepted=");
    Serial.print(lastAck.accepted ? "yes" : "no");
    Serial.print(" | scenario=");
    Serial.print(lastAck.scenarioId);
    Serial.print(" | A=");
    Serial.print(lastAck.adaptiveMultiplier, 2);
    Serial.print(" | next concentration=");
    Serial.print(lastAck.appliedConcentration, 2);
    Serial.println("%");
  } else if (ackReceived) {
    Serial.println("ACK received, but session ID did not match.");
  } else {
    Serial.println("No ACK received. Packet may still have reached the homebase.");
  }

  nextSessionId++;
  clearCurrentSession();
}

void processPuffInput() {
  const bool rawPuffInputState = digitalRead(PUFF_INPUT_PIN);
  const unsigned long now = millis();

  if (rawPuffInputState != lastRawPuffInputState) {
    lastRawPuffInputState = rawPuffInputState;
    lastRawChangeMs = now;
  }

  if (now - lastRawChangeMs < DEBOUNCE_MS) {
    return;
  }

  if (rawPuffInputState == stablePuffInputState) {
    return;
  }

  stablePuffInputState = rawPuffInputState;

  if (stablePuffInputState == LOW) {
    puffTimingActive = true;
    puffStartMs = now;
    setPwmOutput(true);
    Serial.println("Puff input active. Timing puff...");
    return;
  }

  if (stablePuffInputState == HIGH && puffTimingActive) {
    puffTimingActive = false;
    setPwmOutput(false);

    const unsigned long puffMs = now - puffStartMs;
    if (puffMs < MIN_PUFF_MS) {
      Serial.println("Puff input tap ignored as bounce/noise.");
      return;
    }

    const float puffSeconds = puffMs / 1000.0f;
    Serial.print("Puff input released. Measured puff duration=");
    Serial.print(puffSeconds, 2);
    Serial.println(" s");
    addPuffToSession(puffSeconds);
  }
}

void handleSerialCommand() {
  if (Serial.available() <= 0) {
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.isEmpty()) {
    return;
  }

  String upper = command;
  upper.toUpperCase();

  if (upper == "HELP" || upper == "?") {
    printHelp();
  } else if (upper == "STATUS") {
    printStatus();
  } else if (upper == "SYNC" || upper == "SEND") {
    syncCurrentSession();
  } else if (upper.startsWith("PUFF ")) {
    addPuffToSession(command.substring(5).toFloat());
  } else if (upper == "RESET" || upper == "CLEAR") {
    clearCurrentSession();
  } else {
    Serial.println("Unknown command. Type HELP.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PUFF_INPUT_PIN, INPUT_PULLUP);
  pinMode(PWM_OUTPUT_PIN, OUTPUT);
  setPwmOutput(false);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  if (!ensurePeerRegistered()) {
    Serial.println("Failed to register homebase peer. Check HOMEBASE_MAC.");
  }

  Serial.println();
  Serial.println("Puff input test ready.");
  Serial.println("Press/activate the puff input on GPIO22.");
  Serial.println("GPIO23 goes HIGH while the puff input is active.");
  Serial.println("FadeX ESP-NOW sender is also ready.");
  printStatus();
  printHelp();
}

void loop() {
  processPuffInput();
  handleSerialCommand();
}
