#include <esp_now.h>
#include <WiFi.h>
#include <math.h>

//the homebase receives the complete session packet that was collected
//on the handheld device
struct SessionData {
  uint32_t totalPuffs;     //# of puffs in the synced session
  float totalDuration;     //total puff duration in seconds
  float surveyScore;       //user self-report score
  uint8_t triggerCode;     //what the user said triggered the craving/use
  uint32_t sessionId;      //UID
};

//short response packet sent back to the handheld for user feedback.
struct SyncAck {
  bool accepted;
  float adaptiveMultiplier;
  float appliedConcentration;
  char scenarioId[4];
};

//one pump driver channel is represented by two control pins and a name
struct PumpChannel {
  int in1;
  int in2;
  const char *name;
};

//pump driver mapping inferred from the homebase schematic
constexpr PumpChannel NICOTINE_PUMP = {22, 21, "Nicotine"};
constexpr PumpChannel DILUENT_PUMP = {19, 18, "Diluent"};
constexpr PumpChannel MIXING_PUMP = {17, 16, "Mixing"};

//calibration and algorithm constants
constexpr float NIC_ML_PER_SEC = 0.98f;
constexpr float DIL_ML_PER_SEC = 0.98f;
constexpr float MAX_CONCENTRATION_PERCENT = 5.0f;
constexpr float MIN_CONCENTRATION_PERCENT = 0.5f;
constexpr float TOTAL_VOLUME_ML = 30.0f;
constexpr float TAPER_STEP_PERCENT = 0.28f;

//setting to 0 if the third pump is not populated yet
constexpr unsigned long MIXING_RUN_MS = 1500;

const char *const TRIGGER_LABELS[] = {
  "None", "Stress", "Social", "Habit",
  "Withdrawal", "Meals", "Driving", "Other"
};

// 4 usage tiers x 4 survey tiers = 16 named scenarios
const char *const SCENARIO_IDS[4][4] = {
  {"G1", "G2", "S1", "S2"},
  {"G3", "G4", "S3", "S4"},
  {"G5", "G6", "S5", "S6"},
  {"G7", "G8", "S7", "S8"}
};

//the homebase keeps track of the most recently applied concentration
float currentConcentration = 5.0f;

//the radio callback should stay short, so new work is dropped into this buffer
//and processed later in the main loop
volatile bool pendingSessionReady = false;
SessionData pendingSession = {};
uint8_t pendingSenderMac[6] = {};
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

//turn one pump fully off
void stopPump(const PumpChannel &pump) {
  digitalWrite(pump.in1, LOW);
  digitalWrite(pump.in2, LOW);
}

//emergency helper func to stop every pump at once
void stopAllPumps() {
  stopPump(NICOTINE_PUMP);
  stopPump(DILUENT_PUMP);
  stopPump(MIXING_PUMP);
}

//configure the output pins for a pump channel
void setupPump(const PumpChannel &pump) {
  pinMode(pump.in1, OUTPUT);
  pinMode(pump.in2, OUTPUT);
  stopPump(pump);
}

//run a pump for a fixed amount of time
//in our prototype, time is used as a stand-in for delivered vol
void runPump(const PumpChannel &pump, unsigned long runMs) {
  if (runMs == 0) {
    return;
  }

  Serial.print("Running ");
  Serial.print(pump.name);
  Serial.print(" pump for ");
  Serial.print(runMs);
  Serial.println(" ms");

  digitalWrite(pump.in1, HIGH);
  digitalWrite(pump.in2, LOW);
  delay(runMs);
  stopPump(pump);
}

//register the handheld as a peer before sending a response packet
bool ensurePeerRegistered(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) {
    return true;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  return esp_now_add_peer(&peerInfo) == ESP_OK;
}

//enforcing the safety bounds from the algorithm
//never go below 0.5% and never exceed the 5.0% baseline
float clampDose(float value) {
  if (value < MIN_CONCENTRATION_PERCENT) {
    return MIN_CONCENTRATION_PERCENT;
  }
  if (value > MAX_CONCENTRATION_PERCENT) {
    return MAX_CONCENTRATION_PERCENT;
  }
  return value;
}

//translate total usage duration into one of the four usage tiers
uint8_t classifyUsageTier(float totalDurationSeconds) {
  if (totalDurationSeconds < 240.0f) {
    return 0;
  }
  if (totalDurationSeconds <= 450.0f) {
    return 1;
  }
  if (totalDurationSeconds <= 700.0f) {
    return 2;
  }
  return 3;
}

//convert the usage tier into the multiplier used by the formula
float usageMultiplierForTier(uint8_t tier) {
  switch (tier) {
    case 0: return 0.9f;
    case 1: return 1.0f;
    case 2: return 1.1f;
    default: return 1.3f;
  }
}

//snap the incoming survey score to the nearest valid bucket
//so it maps cleanly into the 4 x 4 scenario matrix
uint8_t classifySurveyTier(float surveyScore) {
  const float surveyValues[4] = {0.5f, 0.0f, -0.5f, -1.0f};
  uint8_t bestIndex = 0;
  float bestDiff = fabsf(surveyScore - surveyValues[0]);

  for (uint8_t i = 1; i < 4; ++i) {
    float diff = fabsf(surveyScore - surveyValues[i]);
    if (diff < bestDiff) {
      bestDiff = diff;
      bestIndex = i;
    }
  }

  return bestIndex;
}

//convert a numeric trigger code back into readable text for logs/debugging
const char *triggerLabel(uint8_t triggerCode) {
  const size_t triggerCount = sizeof(TRIGGER_LABELS) / sizeof(TRIGGER_LABELS[0]);
  if (triggerCode >= triggerCount) {
    return "Unknown";
  }
  return TRIGGER_LABELS[triggerCode];
}

//radio receive callback:
//copy the packet into a shared buf and return quickly
void onDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  if (len != sizeof(SessionData)) {
    return;
  }

  portENTER_CRITICAL_ISR(&pendingMux);
  if (!pendingSessionReady) {
    memcpy(&pendingSession, incomingData, sizeof(pendingSession));
    memcpy(pendingSenderMac, info->src_addr, 6);
    pendingSessionReady = true;
  }
  portEXIT_CRITICAL_ISR(&pendingMux);
}

//pull one waiting session packet out of the shared buf
bool fetchPendingSession(SessionData &session, uint8_t senderMac[6]) {
  bool hasSession = false;

  portENTER_CRITICAL(&pendingMux);
  if (pendingSessionReady) {
    session = pendingSession;
    memcpy(senderMac, pendingSenderMac, 6);
    pendingSessionReady = false;
    hasSession = true;
  }
  portEXIT_CRITICAL(&pendingMux);

  return hasSession;
}

//send a short summary back to the handheld for user-facing feedback
void sendAck(const uint8_t *destinationMac, float adaptiveMultiplier, float appliedConcentration, const char *scenarioId) {
  if (!ensurePeerRegistered(destinationMac)) {
    Serial.println("Failed to register handheld peer for ACK.");
    return;
  }

  SyncAck ack = {};
  ack.accepted = true;
  ack.adaptiveMultiplier = adaptiveMultiplier;
  ack.appliedConcentration = appliedConcentration;
  snprintf(ack.scenarioId, sizeof(ack.scenarioId), "%s", scenarioId);

  esp_now_send(destinationMac, reinterpret_cast<uint8_t *>(&ack), sizeof(ack));
}

//print the whole decision process to serial monitor for demos and debugging
void printSessionSummary(
  const SessionData &data,
  const char *scenarioId,
  float mu,
  float adaptiveMultiplier,
  float preliminaryConcentration,
  float clampedConcentration,
  bool highRelapseRisk
) {
  Serial.println("\n--- FADEX SESSION ---");
  Serial.print("Session ID: ");
  Serial.println(data.sessionId);
  Serial.print("Puffs: ");
  Serial.println(data.totalPuffs);
  Serial.print("Duration (s): ");
  Serial.println(data.totalDuration, 1);
  Serial.print("Survey score: ");
  Serial.println(data.surveyScore, 2);
  Serial.print("Trigger: ");
  Serial.println(triggerLabel(data.triggerCode));
  Serial.print("Mu: ");
  Serial.println(mu, 2);
  Serial.print("Scenario: ");
  Serial.println(scenarioId);
  Serial.print("A: ");
  Serial.println(adaptiveMultiplier, 2);
  Serial.print("Preliminary C_next (%): ");
  Serial.println(preliminaryConcentration, 2);
  Serial.print("Clamped C_next (%): ");
  Serial.println(clampedConcentration, 2);
  Serial.print("High relapse risk: ");
  Serial.println(highRelapseRisk ? "YES" : "NO");
}

// Core algorithm:
// 1. Determine usage tier from duration
// 2. Determine survey tier from self-report
// 3. Pick one of 16 scenario IDs
// 4. Compute the adaptive multiplier A
// 5. Compute a preliminary next concentration
// 6. Clamp the concentration to the safe range
// 7. Convert the result into pump run times and execute the pumps
void processSession(const SessionData &data, const uint8_t senderMac[6]) {
  const uint8_t usageTier = classifyUsageTier(data.totalDuration);
  const float mu = usageMultiplierForTier(usageTier);
  const uint8_t surveyTier = classifySurveyTier(data.surveyScore);
  const char *scenarioId = SCENARIO_IDS[usageTier][surveyTier];

  const float adaptiveMultiplier = 1.0f + data.surveyScore + (1.0f - mu);
  const float preliminaryConcentration = currentConcentration - (TAPER_STEP_PERCENT * adaptiveMultiplier);
  const float clampedConcentration = clampDose(preliminaryConcentration);

  //convert the final concentration target into actual liquid volumes
  const float nicotineVolumeMl = (clampedConcentration / MAX_CONCENTRATION_PERCENT) * TOTAL_VOLUME_ML;
  const float diluentVolumeMl = TOTAL_VOLUME_ML - nicotineVolumeMl;

  //convert volume targets into pump run times using the calibrated flow rates
  const unsigned long nicotineRunMs = static_cast<unsigned long>((nicotineVolumeMl / NIC_ML_PER_SEC) * 1000.0f);
  const unsigned long diluentRunMs = static_cast<unsigned long>((diluentVolumeMl / DIL_ML_PER_SEC) * 1000.0f);

  //simple first-pass flag for the app/support layer
  const bool highRelapseRisk =
    (surveyTier >= 2 && usageTier >= 2) ||
    (clampedConcentration > currentConcentration);

  printSessionSummary(
    data,
    scenarioId,
    mu,
    adaptiveMultiplier,
    preliminaryConcentration,
    clampedConcentration,
    highRelapseRisk
  );

  Serial.println("Applying safety clamp range 0.5% to 5.0%");

  //stage 4 hardware execution:
  //physically mix the nicotine and diluent needed for the next treatment cycle
  runPump(NICOTINE_PUMP, nicotineRunMs);
  runPump(DILUENT_PUMP, diluentRunMs);

  if (MIXING_RUN_MS > 0) {
    runPump(MIXING_PUMP, MIXING_RUN_MS);
  }

  //once mixing is done, this becomes the new current baseline
  currentConcentration = clampedConcentration;
  sendAck(senderMac, adaptiveMultiplier, currentConcentration, scenarioId);
}

//calibration command:
//run one pump for 30 seconds so you can measure actual output and update
//the mL/sec constants above
void runCalibrationCommand(const PumpChannel &pump) {
  Serial.print("Calibrating ");
  Serial.print(pump.name);
  Serial.println(" pump for 30 seconds...");
  runPump(pump, 30000);
  Serial.println("Measure output volume and update *_ML_PER_SEC.");
}

//small set of manual commands for debugging and calibration
void handleSerialCommands() {
  if (Serial.available() <= 0) {
    return;
  }

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "CAL1") {
    runCalibrationCommand(NICOTINE_PUMP);
  } else if (cmd == "CAL2") {
    runCalibrationCommand(DILUENT_PUMP);
  } else if (cmd == "CAL3") {
    runCalibrationCommand(MIXING_PUMP);
  } else if (cmd == "STATUS") {
    Serial.print("Current concentration (%): ");
    Serial.println(currentConcentration, 2);
  } else if (cmd == "STOP") {
    stopAllPumps();
    Serial.println("All pumps stopped.");
  } else {
    Serial.println("Commands: CAL1, CAL2, CAL3, STATUS, STOP");
  }
}

//standard arduino setup:
//init the pumps, the Wi-Fi radio, and ESP-NOW.
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  setupPump(NICOTINE_PUMP);
  setupPump(DILUENT_PUMP);
  setupPump(MIXING_PUMP);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);
  Serial.println("Homebase listening. Commands: CAL1, CAL2, CAL3, STATUS, STOP");
}

// main loop:
//handle manual commands, then process any newly received session packet
void loop() {
  handleSerialCommands();

  SessionData session = {};
  uint8_t senderMac[6] = {};
  if (fetchPendingSession(session, senderMac)) {
    processSession(session, senderMac);
  }
}
