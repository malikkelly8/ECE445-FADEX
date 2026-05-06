#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// FadeX ESP-NOW homebase receiver test.
//
// Flash this sketch to the breadboard/homebase ESP32. It listens for puff
// packets from the vape ESP32 and prints each received puff to Serial Monitor.
//
// Homebase STA / ESP-NOW MAC used for this test:
//   88:57:21:7A:08:2C
//
// Vape STA / ESP-NOW MAC used for this test:
//   88:13:BF:55:33:D8

constexpr uint8_t ESPNOW_CHANNEL = 1;

constexpr uint8_t VAPE_MAC[6] = {
  0x88, 0x13, 0xBF, 0x55, 0x33, 0xD8
};

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
    Serial.print("Failed to add ESP-NOW peer. Error code: ");
    Serial.println(result);
    return false;
  }

  return true;
}

void sendAck(const uint8_t *destinationMac, const PuffPacket &packet) {
  AckPacket ack = {};
  ack.accepted = true;
  ack.packetId = packet.packetId;
  ack.puffCountSeen = packet.puffCount;

  const esp_err_t result =
    esp_now_send(destinationMac, reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));

  if (result != ESP_OK) {
    Serial.print("ACK send failed. Error code: ");
    Serial.println(result);
  }
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  const uint8_t *sourceMac = info->src_addr;

  Serial.println();
  Serial.print("Packet received from ");
  printMac(sourceMac);
  Serial.println();

  if (len != static_cast<int>(sizeof(PuffPacket))) {
    Serial.print("Ignored packet with unexpected size: ");
    Serial.print(len);
    Serial.print(" bytes. Expected ");
    Serial.print(sizeof(PuffPacket));
    Serial.println(" bytes.");
    return;
  }

  PuffPacket packet = {};
  memcpy(&packet, data, sizeof(packet));

  Serial.println("PUFF RECEIVED BY HOMEBASE");
  Serial.print("  packetId: ");
  Serial.println(packet.packetId);
  Serial.print("  puffCount: ");
  Serial.println(packet.puffCount);
  Serial.print("  lastPuffSeconds: ");
  Serial.println(packet.lastPuffSeconds, 2);
  Serial.print("  totalPuffSeconds: ");
  Serial.println(packet.totalPuffSeconds, 2);
  Serial.print("  vapeUptimeMs: ");
  Serial.println(packet.uptimeMs);

  registerPeer(sourceMac);
  sendAck(sourceMac, packet);
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

  esp_now_register_recv_cb(onDataRecv);
  registerPeer(VAPE_MAC);
}

void printStartup() {
  Serial.println();
  Serial.println("FadeX homebase ESP-NOW puff receiver test");
  Serial.print("Homebase local STA MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Expected vape peer MAC: ");
  printMac(VAPE_MAC);
  Serial.println();
  Serial.print("ESP-NOW channel: ");
  Serial.println(ESPNOW_CHANNEL);
  Serial.println();
  Serial.println("Waiting for vape puff packets...");
  Serial.println("On the vape Serial Monitor, type PUFF to send a test puff.");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  setupEspNowRadio();
  printStartup();
}

void loop() {
  delay(100);
}
