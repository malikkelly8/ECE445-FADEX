#include <esp_now.h>
#include <WiFi.h>

// Breadboard handheld:
// - watches the puff sensor
// - accumulates puff count and total puff duration
// - sends one session packet to the homebase when the Sync button is pressed
// - waits for the homebase to finish processing and send back a short result

// Handheld hardware pins from the earlier breadboard prototype.
// The PCB/schematic version moved the puff sensor to GPIO22,
// but the original breadboard code used GPIO13.
constexpr int PIN_PUFF_SENSOR = 13;
constexpr int PIN_SYNC = 27;

// Update this MAC address to match the homebase MAC printed in its Serial Monitor.
uint8_t HOMEBASE_MAC[] = {0x88, 0x57, 0x21, 0x79, 0x8C, 0x80};

// This is the only packet the handheld sends in the breadboard version.
// We keep it small so bring-up is easy:
// the homebase will ask the operator for survey + trigger data later.
struct SessionData {
  uint32_t totalPuffs;
  float totalDuration;
  uint32_t sessionId;
};

// The homebase sends this after it has collected operator input,
// run the algorithm, and finished the dosing calculation.
struct SyncAck {
  bool accepted;
  uint32_t sessionId;
  float adaptiveMultiplier;
  float appliedConcentration;
  char scenarioId[4];
};

SessionData currentSession = {};
SyncAck lastAck = {};

volatile bool sendComplete = false;
volatile bool sendSucceeded = false;
volatile bool ackReceived = false;

// While we are waiting for the homebase operator to finish entering data,
// we intentionally pause new puff collection so one treatment cycle maps to one packet.
bool waitingForAck = false;
uint32_t nextSessionId = 1;

void printMacAddress() {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  Serial.print("Handheld MAC: ");
  for (int i = 0; i < 6; ++i) {
    if (i > 0) {
      Serial.print(":");
    }
    if (mac[i] < 16) {
      Serial.print("0");
    }
    Serial.print(mac[i], HEX);
  }
  Serial.println();
}

void clearSession() {
  currentSession.totalPuffs = 0;
  currentSession.totalDuration = 0.0f;
  currentSession.sessionId = 0;
}

bool buttonPressed(int pin) {
  if (digitalRead(pin) != LOW) {
    return false;
  }

  delay(25);
  if (digitalRead(pin) != LOW) {
    return false;
  }

  while (digitalRead(pin) == LOW) {
    delay(5);
  }

  return true;
}

void printSessionTotals() {
  Serial.print("Puffs: ");
  Serial.print(currentSession.totalPuffs);
  Serial.print(" | Duration (s): ");
  Serial.println(currentSession.totalDuration, 1);
}

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

void ensurePeerRegistered() {
  if (esp_now_is_peer_exist(HOMEBASE_MAC)) {
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, HOMEBASE_MAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

void processPuffSensor() {
  if (digitalRead(PIN_PUFF_SENSOR) != LOW) {
    return;
  }

  const unsigned long startMs = millis();
  while (digitalRead(PIN_PUFF_SENSOR) == LOW) {
    delay(1);
  }

  const float durationSeconds = (millis() - startMs) / 1000.0f;
  if (durationSeconds < 0.05f) {
    return;
  }

  currentSession.totalPuffs++;
  currentSession.totalDuration += durationSeconds;

  Serial.print("Puff recorded. ");
  printSessionTotals();
}

void handleAckIfReady() {
  if (!ackReceived) {
    return;
  }

  ackReceived = false;

  if (!waitingForAck) {
    Serial.println("Received ACK, but no session was waiting.");
    return;
  }

  if (lastAck.sessionId != nextSessionId) {
    Serial.println("Received ACK for an unexpected session ID. Ignoring.");
    return;
  }

  waitingForAck = false;

  Serial.println();
  Serial.println("--- HOMEBASE RESULT ---");
  Serial.print("Scenario: ");
  Serial.println(lastAck.scenarioId);
  Serial.print("Adaptive multiplier A: ");
  Serial.println(lastAck.adaptiveMultiplier, 2);
  Serial.print("Applied concentration (%): ");
  Serial.println(lastAck.appliedConcentration, 2);

  clearSession();
  nextSessionId++;

  Serial.println("Session cleared. Ready to collect the next session.");
  printSessionTotals();
}

void sendCurrentSession() {
  if (waitingForAck) {
    Serial.println("Still waiting for the last ACK. Finish the prompts on the homebase first.");
    return;
  }

  if (currentSession.totalPuffs == 0 && currentSession.totalDuration < 0.05f) {
    Serial.println("Nothing to sync yet. Take at least one puff first.");
    return;
  }

  currentSession.sessionId = nextSessionId;

  sendComplete = false;
  sendSucceeded = false;
  ackReceived = false;

  Serial.println();
  Serial.println("Sending session to homebase...");
  printSessionTotals();
  Serial.print("Session ID: ");
  Serial.println(currentSession.sessionId);

  const esp_err_t result =
    esp_now_send(HOMEBASE_MAC, reinterpret_cast<uint8_t *>(&currentSession), sizeof(currentSession));

  if (result != ESP_OK) {
    Serial.print("Failed to start ESP-NOW send. Error code: ");
    Serial.println(result);
    return;
  }

  const unsigned long waitStart = millis();
  while (!sendComplete && (millis() - waitStart) < 1500) {
    delay(10);
  }

  if (!sendComplete || !sendSucceeded) {
    Serial.println("Radio send failed. Session data is still stored locally.");
    return;
  }

  waitingForAck = true;
  Serial.println("Session delivered to homebase.");
  Serial.println("Complete the survey + trigger prompts in the homebase Serial Monitor.");
}

void printStartupHelp() {
  Serial.println();
  Serial.println("Handheld breadboard build ready.");
  Serial.println("Behavior:");
  Serial.println("- Puff sensor on GPIO13 records puff duration and puff count.");
  Serial.println("- Sync button on GPIO27 sends the current session to the homebase.");
  Serial.println("- New puff collection pauses until the homebase sends back an ACK.");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  pinMode(PIN_PUFF_SENSOR, INPUT_PULLUP);
  pinMode(PIN_SYNC, INPUT_PULLUP);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed on handheld.");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  ensurePeerRegistered();

  printStartupHelp();
  printMacAddress();
  printSessionTotals();
}

void loop() {
  handleAckIfReady();

  if (!waitingForAck) {
    processPuffSensor();
  }

  if (buttonPressed(PIN_SYNC)) {
    sendCurrentSession();
  }
}
