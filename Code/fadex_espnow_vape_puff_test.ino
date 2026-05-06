#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// FadeX ESP-NOW vape puff transmitter test.
//
// Flash this sketch to the vape/handheld ESP32. It sends a puff packet to the
// homebase whenever the puff sensor activates. You can also type PUFF in Serial
// Monitor to send a fake 2-second puff before the sensor wiring is ready.
//
// Vape STA / ESP-NOW MAC used for this test:
//   88:13:BF:55:33:D8
//
// Homebase STA / ESP-NOW MAC used for this test:
//   88:57:21:7A:08:2C

constexpr uint8_t ESPNOW_CHANNEL = 1;

constexpr uint8_t HOMEBASE_MAC[6] = {
  0x88, 0x57, 0x21, 0x7A, 0x08, 0x2C
};

// PCB vape schematic uses PSENS on GPIO22. If you are testing an older
// breadboard puff wire, change this to 13.
constexpr int PUFF_SENSOR_PIN = 22;
constexpr bool PUFF_ACTIVE_LOW = true;

constexpr unsigned long DEBOUNCE_MS = 60;
constexpr unsigned long MIN_PUFF_MS = 150;

struct PuffPacket {
  uint32_t packetId;
  uint32_t puffCount;
  float lastPuffSeconds;
  float totalPuffSeconds;
  uint32_t uptimeMs;
};

struct AckPacket {
  bool accepted;
  uint32_t packetId;
  uint32_t puffCountSeen;
};

uint32_t nextPacketId = 1;
uint32_t puffCount = 0;
float totalPuffSeconds = 0.0f;

bool puffInProgress = false;
unsigned long puffStartMs = 0;
bool stableSensorActive = false;
bool lastRawSensorActive = false;
unsigned long lastRawChangeMs = 0;

void printMac(const uint8_t *mac) {
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

bool registerPeer(const uint8_t *peerMac) {
  if (esp_now_is_peer_exist(peerMac)) {
    return true;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  const esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    Serial.print("Failed to add homebase ESP-NOW peer. Error code: ");
    Serial.println(result);
    return false;
  }

  return true;
}

void sendPuffPacket(float puffSeconds) {
  puffCount++;
  totalPuffSeconds += puffSeconds;

  PuffPacket packet = {};
  packet.packetId = nextPacketId++;
  packet.puffCount = puffCount;
  packet.lastPuffSeconds = puffSeconds;
  packet.totalPuffSeconds = totalPuffSeconds;
  packet.uptimeMs = millis();

  Serial.println();
  Serial.println("Sending puff packet to homebase");
  Serial.print("  packetId: ");
  Serial.println(packet.packetId);
  Serial.print("  puffCount: ");
  Serial.println(packet.puffCount);
  Serial.print("  lastPuffSeconds: ");
  Serial.println(packet.lastPuffSeconds, 2);
  Serial.print("  totalPuffSeconds: ");
  Serial.println(packet.totalPuffSeconds, 2);

  const esp_err_t result =
    esp_now_send(HOMEBASE_MAC, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));

  if (result != ESP_OK) {
    Serial.print("ESP-NOW send could not start. Error code: ");
    Serial.println(result);
  }
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  (void)info;
  Serial.print("ESP-NOW send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILED");
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  (void)info;

  if (len != static_cast<int>(sizeof(AckPacket))) {
    Serial.print("Received non-ACK packet with size ");
    Serial.println(len);
    return;
  }

  AckPacket ack = {};
  memcpy(&ack, data, sizeof(ack));

  Serial.print("Homebase ACK received for packet ");
  Serial.print(ack.packetId);
  Serial.print(" | puffCountSeen=");
  Serial.println(ack.puffCountSeen);
}

bool readPuffSensorActive() {
  const int raw = digitalRead(PUFF_SENSOR_PIN);
  return PUFF_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
}

void updatePuffSensor() {
  const bool rawActive = readPuffSensorActive();
  const unsigned long nowMs = millis();

  if (rawActive != lastRawSensorActive) {
    lastRawSensorActive = rawActive;
    lastRawChangeMs = nowMs;
  }

  if ((nowMs - lastRawChangeMs) < DEBOUNCE_MS || rawActive == stableSensorActive) {
    return;
  }

  stableSensorActive = rawActive;

  if (stableSensorActive) {
    puffInProgress = true;
    puffStartMs = nowMs;
    Serial.println("Puff START detected on vape.");
    return;
  }

  if (!puffInProgress) {
    return;
  }

  puffInProgress = false;
  const unsigned long puffMs = nowMs - puffStartMs;

  if (puffMs < MIN_PUFF_MS) {
    Serial.print("Ignored very short puff/noise: ");
    Serial.print(puffMs);
    Serial.println(" ms");
    return;
  }

  const float puffSeconds = puffMs / 1000.0f;
  Serial.print("Puff END. Duration: ");
  Serial.print(puffSeconds, 2);
  Serial.println(" s");
  sendPuffPacket(puffSeconds);
}

void handleSerialCommand(String command) {
  command.trim();
  command.toUpperCase();

  if (command == "PUFF") {
    sendPuffPacket(2.0f);
  } else if (command == "STATUS") {
    Serial.println();
    Serial.println("FadeX vape test status");
    Serial.print("  puffCount: ");
    Serial.println(puffCount);
    Serial.print("  totalPuffSeconds: ");
    Serial.println(totalPuffSeconds, 2);
    Serial.print("  puff pin GPIO: ");
    Serial.println(PUFF_SENSOR_PIN);
    Serial.print("  sensor currently active: ");
    Serial.println(readPuffSensorActive() ? "YES" : "NO");
  } else if (command == "HELP" || command == "?") {
    Serial.println();
    Serial.println("Commands:");
    Serial.println("  PUFF    -> send a fake 2-second puff to homebase");
    Serial.println("  STATUS  -> print puff counter and sensor state");
    Serial.println("  HELP    -> print this menu");
  } else if (command.length() > 0) {
    Serial.println("Unknown command. Type HELP.");
  }
}

void setupEspNowRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Both test sketches force the same Wi-Fi channel so ESP-NOW can talk
  // without needing Blynk, a router, or any Wi-Fi credentials.
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed. Restart the board and try again.");
    while (true) {
      delay(1000);
    }
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  registerPeer(HOMEBASE_MAC);
}

void printStartup() {
  Serial.println();
  Serial.println("FadeX vape ESP-NOW puff transmitter test");
  Serial.print("Vape local STA MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Target homebase MAC: ");
  printMac(HOMEBASE_MAC);
  Serial.println();
  Serial.print("ESP-NOW channel: ");
  Serial.println(ESPNOW_CHANNEL);
  Serial.print("Puff sensor GPIO: ");
  Serial.println(PUFF_SENSOR_PIN);
  Serial.println();
  Serial.println("Type PUFF to send a test puff manually.");
  Serial.println("Then try the real puff sensor and watch the homebase Serial Monitor.");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PUFF_SENSOR_PIN, PUFF_ACTIVE_LOW ? INPUT_PULLUP : INPUT);

  setupEspNowRadio();
  printStartup();
}

void loop() {
  updatePuffSensor();

  if (Serial.available() > 0) {
    handleSerialCommand(Serial.readStringUntil('\n'));
  }

  delay(10);
}
