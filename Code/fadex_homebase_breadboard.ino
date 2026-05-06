#include <esp_now.h>
#include <WiFi.h>

// Breadboard homebase using a three-pump ESP32 wiring setup:
// - receives puff count + duration from the handheld
// - asks the operator for survey score and trigger using the Serial Monitor
// - computes the taper algorithm
// - drives nicotine, diluent, and final-transfer pumps
// - supports direct volume-based calibration commands for Pumps 1 and 2
// - supports priming feed lines before a run
// - sends a short result packet back to the handheld

struct SessionData {
  uint32_t totalPuffs;
  float totalDuration;
  uint32_t sessionId;
};

struct SyncAck {
  bool accepted;
  uint32_t sessionId;
  float adaptiveMultiplier;
  float appliedConcentration;
  char scenarioId[4];
};

struct PumpChannel {
  int ena;
  int in1;
  int in2;
  const char *name;
};

// Three-pump breadboard layout on the ESP32.
constexpr PumpChannel PUMP1 = {25, 26, 27, "Pump 1 / Nicotine"};
constexpr PumpChannel PUMP2 = {14, 19, 18, "Pump 2 / Diluent"};
constexpr PumpChannel PUMP3 = {33, 32, 23, "Pump 3 / Final Transfer"};

// Multi-point calibration from recent water tests.
// The lag-based model fit the measurements better than a simple volume offset:
// runtime_seconds = startup_lag_seconds + (target_mL / steady_mL_per_sec)
constexpr float PUMP1_STEADY_ML_PER_SEC = 1.04f;
constexpr float PUMP2_STEADY_ML_PER_SEC = 1.04f;
constexpr float PUMP1_STARTUP_LAG_SEC = 1.26f;
constexpr float PUMP2_STARTUP_LAG_SEC = 1.26f;

constexpr float MAX_CONCENTRATION_PERCENT = 5.0f;
constexpr float MIN_CONCENTRATION_PERCENT = 0.5f;
constexpr float TOTAL_VOLUME_ML = 30.0f;
constexpr float TAPER_STEP_PERCENT = 0.28f;
constexpr unsigned long PRIME_PUMPS_12_MS = 10000;
constexpr unsigned long PUMP3_FINAL_TRANSFER_MS = 60000;

const char *const TRIGGER_LABELS[] = {
  "None", "Stress", "Social", "Habit",
  "Withdrawal", "Meals", "Driving", "Other"
};

const float SURVEY_VALUES[] = {0.5f, 0.0f, -0.5f, -1.0f};

// 4 usage tiers x 4 survey tiers = 16 scenarios.
const char *const SCENARIO_IDS[4][4] = {
  {"G1", "G2", "S1", "S2"},
  {"G3", "G4", "S3", "S4"},
  {"G5", "G6", "S5", "S6"},
  {"G7", "G8", "S7", "S8"}
};

float currentConcentration = 5.0f;

volatile bool pendingSessionReady = false;
SessionData pendingSession = {};
uint8_t pendingSenderMac[6] = {};
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// Keep the last response so a duplicate radio packet does not dose twice.
bool haveLastAck = false;
uint8_t lastSenderMac[6] = {};
SyncAck lastAck = {};

void printMacAddress() {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  Serial.print("Homebase MAC: ");
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

void stopPump(const PumpChannel &pump) {
  analogWrite(pump.ena, 0);
  digitalWrite(pump.in1, LOW);
  digitalWrite(pump.in2, LOW);
}

void stopAllPumps() {
  stopPump(PUMP1);
  stopPump(PUMP2);
  stopPump(PUMP3);
}

void setupPumpDriver(const PumpChannel &pump) {
  pinMode(pump.ena, OUTPUT);
  pinMode(pump.in1, OUTPUT);
  pinMode(pump.in2, OUTPUT);
  stopPump(pump);
}

void startPump(const PumpChannel &pump) {
  digitalWrite(pump.in1, HIGH);
  digitalWrite(pump.in2, LOW);
  analogWrite(pump.ena, 255);
}

void runPump(const PumpChannel &pump, unsigned long runMs) {
  if (runMs == 0) {
    return;
  }

  Serial.print("Running ");
  Serial.print(pump.name);
  Serial.print(" for ");
  Serial.print(runMs);
  Serial.println(" ms");

  startPump(pump);
  delay(runMs);
  stopPump(pump);
}

void runPumpsSimultaneous(unsigned long pump1Ms, unsigned long pump2Ms) {
  if (pump1Ms == 0 && pump2Ms == 0) {
    Serial.println("Nothing to run. Both computed runtimes were zero.");
    return;
  }

  Serial.println("Starting simultaneous pump test.");
  Serial.print("Pump 1 runtime (ms): ");
  Serial.println(pump1Ms);
  Serial.print("Pump 2 runtime (ms): ");
  Serial.println(pump2Ms);

  bool pump1Active = pump1Ms > 0;
  bool pump2Active = pump2Ms > 0;

  if (pump1Active) {
    startPump(PUMP1);
  }
  if (pump2Active) {
    startPump(PUMP2);
  }

  const unsigned long startMs = millis();
  while (pump1Active || pump2Active) {
    const unsigned long elapsedMs = millis() - startMs;

    if (pump1Active && elapsedMs >= pump1Ms) {
      stopPump(PUMP1);
      pump1Active = false;
      Serial.println("Pump 1 stopped.");
    }

    if (pump2Active && elapsedMs >= pump2Ms) {
      stopPump(PUMP2);
      pump2Active = false;
      Serial.println("Pump 2 stopped.");
    }

    delay(1);
  }

  Serial.println("Simultaneous pump test finished.");
}

void primeFeedLines(unsigned long runMs) {
  if (runMs == 0) {
    Serial.println("Prime time must be greater than zero.");
    return;
  }

  Serial.println();
  Serial.println("Priming Pumps 1 and 2 together to fill the feed tubes.");
  runPumpsSimultaneous(runMs, runMs);
}

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

bool sameMac(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

float clampDose(float value) {
  if (value < MIN_CONCENTRATION_PERCENT) {
    return MIN_CONCENTRATION_PERCENT;
  }
  if (value > MAX_CONCENTRATION_PERCENT) {
    return MAX_CONCENTRATION_PERCENT;
  }
  return value;
}

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

float usageMultiplierForTier(uint8_t tier) {
  switch (tier) {
    case 0: return 0.9f;
    case 1: return 1.0f;
    case 2: return 1.1f;
    default: return 1.3f;
  }
}

const char *triggerLabel(uint8_t triggerCode) {
  const size_t triggerCount = sizeof(TRIGGER_LABELS) / sizeof(TRIGGER_LABELS[0]);
  if (triggerCode >= triggerCount) {
    return "Unknown";
  }
  return TRIGGER_LABELS[triggerCode];
}

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

void sendAck(const uint8_t *destinationMac, const SyncAck &ack) {
  if (!ensurePeerRegistered(destinationMac)) {
    Serial.println("Failed to register handheld peer for ACK.");
    return;
  }

  esp_now_send(destinationMac, reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
}

String readSerialLineBlocking() {
  while (true) {
    while (Serial.available() <= 0) {
      delay(10);
    }

    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      return line;
    }
  }
}

float promptForSurveyScore() {
  while (true) {
    Serial.println();
    Serial.println("Enter survey score:");
    Serial.println("1 = Better (+0.5)");
    Serial.println("2 = Neutral (0.0)");
    Serial.println("3 = Struggling (-0.5)");
    Serial.println("4 = Worse (-1.0)");
    Serial.print("> ");

    const String input = readSerialLineBlocking();

    if (input == "1") {
      return SURVEY_VALUES[0];
    }
    if (input == "2") {
      return SURVEY_VALUES[1];
    }
    if (input == "3") {
      return SURVEY_VALUES[2];
    }
    if (input == "4") {
      return SURVEY_VALUES[3];
    }

    Serial.println("Invalid survey choice. Please enter 1, 2, 3, or 4.");
  }
}

uint8_t promptForTriggerCode() {
  while (true) {
    Serial.println();
    Serial.println("Enter trigger code:");
    for (uint8_t i = 0; i < sizeof(TRIGGER_LABELS) / sizeof(TRIGGER_LABELS[0]); ++i) {
      Serial.print(i);
      Serial.print(" = ");
      Serial.println(TRIGGER_LABELS[i]);
    }
    Serial.print("> ");

    const String input = readSerialLineBlocking();
    const long value = input.toInt();

    if (input.length() == 1 && value >= 0 && value <= 7) {
      return static_cast<uint8_t>(value);
    }

    Serial.println("Invalid trigger choice. Please enter a number from 0 to 7.");
  }
}

void printSessionSummary(
  const SessionData &data,
  float surveyScore,
  uint8_t triggerCode,
  const char *scenarioId,
  float mu,
  float adaptiveMultiplier,
  float preliminaryConcentration,
  float clampedConcentration
) {
  Serial.println();
  Serial.println("--- FADEX SESSION ---");
  Serial.print("Session ID: ");
  Serial.println(data.sessionId);
  Serial.print("Puffs: ");
  Serial.println(data.totalPuffs);
  Serial.print("Duration (s): ");
  Serial.println(data.totalDuration, 1);
  Serial.print("Survey: ");
  Serial.println(surveyScore, 2);
  Serial.print("Trigger: ");
  Serial.println(triggerLabel(triggerCode));
  Serial.print("Mu: ");
  Serial.println(mu, 2);
  Serial.print("Scenario: ");
  Serial.println(scenarioId);
  Serial.print("A: ");
  Serial.println(adaptiveMultiplier, 2);
  Serial.print("Preliminary C_next (%): ");
  Serial.println(preliminaryConcentration, 2);
  Serial.print("Final C_next after clamp (%): ");
  Serial.println(clampedConcentration, 2);
}

void runCalibrationCommand(const PumpChannel &pump, unsigned long runMs) {
  Serial.print("Calibrating ");
  Serial.print(pump.name);
  Serial.print(" for ");
  Serial.print(runMs);
  Serial.println(" ms...");
  runPump(pump, runMs);
  Serial.println("Measure delivered volume and update that pump's steady mL/sec and startup lag if needed.");
}

unsigned long runtimeForTargetMl(float targetMl, float steadyMlPerSec, float startupLagSec) {
  if (targetMl <= 0.0f) {
    return 0;
  }

  if (steadyMlPerSec <= 0.0f) {
    return 0;
  }

  const float runtimeSeconds = startupLagSec + (targetMl / steadyMlPerSec);
  return static_cast<unsigned long>(runtimeSeconds * 1000.0f);
}

void runVolumeCommand(const PumpChannel &pump, float steadyMlPerSec, float startupLagSec, float targetMl) {
  if (targetMl <= 0.0f) {
    Serial.println("Usage: RUN1ML <milliliters> or RUN2ML <milliliters>");
    return;
  }

  const unsigned long runMs = runtimeForTargetMl(targetMl, steadyMlPerSec, startupLagSec);

  Serial.print("Target volume for ");
  Serial.print(pump.name);
  Serial.print(" (mL): ");
  Serial.println(targetMl, 2);
  Serial.print("Using steady mL/sec = ");
  Serial.println(steadyMlPerSec, 4);
  Serial.print("Using startup lag (s) = ");
  Serial.println(startupLagSec, 2);
  Serial.print("Calculated runtime (ms): ");
  Serial.println(runMs);

  runPump(pump, runMs);
  Serial.println("Measure the delivered volume and compare it to the target.");
}

void runDualVolumeCommand(float pump1Ml, float pump2Ml) {
  if (pump1Ml < 0.0f || pump2Ml < 0.0f) {
    Serial.println("Usage: RUNBOTH <pump1_mL> <pump2_mL>");
    return;
  }

  Serial.println();
  Serial.println("Starting two-pump volume test.");
  runVolumeCommand(PUMP1, PUMP1_STEADY_ML_PER_SEC, PUMP1_STARTUP_LAG_SEC, pump1Ml);
  runVolumeCommand(PUMP2, PUMP2_STEADY_ML_PER_SEC, PUMP2_STARTUP_LAG_SEC, pump2Ml);
  Serial.println("Two-pump volume test finished.");
}

void runDualVolumeSimultaneousCommand(float pump1Ml, float pump2Ml) {
  if (pump1Ml < 0.0f || pump2Ml < 0.0f) {
    Serial.println("Usage: RUNSIM <pump1_mL> <pump2_mL>");
    return;
  }

  const unsigned long pump1Ms =
    runtimeForTargetMl(pump1Ml, PUMP1_STEADY_ML_PER_SEC, PUMP1_STARTUP_LAG_SEC);
  const unsigned long pump2Ms =
    runtimeForTargetMl(pump2Ml, PUMP2_STEADY_ML_PER_SEC, PUMP2_STARTUP_LAG_SEC);

  Serial.println();
  Serial.println("Starting simultaneous two-pump volume test.");
  Serial.print("Pump 1 target (mL): ");
  Serial.println(pump1Ml, 2);
  Serial.print("Pump 2 target (mL): ");
  Serial.println(pump2Ml, 2);
  Serial.print("Pump 1 steady mL/sec: ");
  Serial.println(PUMP1_STEADY_ML_PER_SEC, 4);
  Serial.print("Pump 1 startup lag (s): ");
  Serial.println(PUMP1_STARTUP_LAG_SEC, 2);
  Serial.print("Pump 2 steady mL/sec: ");
  Serial.println(PUMP2_STEADY_ML_PER_SEC, 4);
  Serial.print("Pump 2 startup lag (s): ");
  Serial.println(PUMP2_STARTUP_LAG_SEC, 2);

  runPumpsSimultaneous(pump1Ms, pump2Ms);
  Serial.println("Measure both delivered volumes and compare them to the targets.");
}

void handleSerialCommands() {
  if (Serial.available() <= 0) {
    return;
  }

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  String upper = cmd;
  upper.toUpperCase();

  if (upper == "CAL1") {
    runCalibrationCommand(PUMP1, 30000);
  } else if (upper == "CAL2") {
    runCalibrationCommand(PUMP2, 30000);
  } else if (upper == "CAL3") {
    runCalibrationCommand(PUMP3, 30000);
  } else if (upper.startsWith("CALSEC ")) {
    const float seconds = cmd.substring(7).toFloat();
    if (seconds <= 0.0f) {
      Serial.println("Usage: CALSEC <seconds>");
    } else {
      runCalibrationCommand(PUMP1, static_cast<unsigned long>(seconds * 1000.0f));
    }
  } else if (upper.startsWith("CAL1SEC ")) {
    const float seconds = cmd.substring(8).toFloat();
    if (seconds <= 0.0f) {
      Serial.println("Usage: CAL1SEC <seconds>");
    } else {
      runCalibrationCommand(PUMP1, static_cast<unsigned long>(seconds * 1000.0f));
    }
  } else if (upper.startsWith("CAL2SEC ")) {
    const float seconds = cmd.substring(8).toFloat();
    if (seconds <= 0.0f) {
      Serial.println("Usage: CAL2SEC <seconds>");
    } else {
      runCalibrationCommand(PUMP2, static_cast<unsigned long>(seconds * 1000.0f));
    }
  } else if (upper.startsWith("CAL3SEC ")) {
    const float seconds = cmd.substring(8).toFloat();
    if (seconds <= 0.0f) {
      Serial.println("Usage: CAL3SEC <seconds>");
    } else {
      runCalibrationCommand(PUMP3, static_cast<unsigned long>(seconds * 1000.0f));
    }
  } else if (upper.startsWith("RUN1MS ")) {
    const unsigned long runMs = static_cast<unsigned long>(cmd.substring(7).toInt());
    if (runMs == 0) {
      Serial.println("Usage: RUN1MS <milliseconds>");
    } else {
      runPump(PUMP1, runMs);
    }
  } else if (upper.startsWith("RUN2MS ")) {
    const unsigned long runMs = static_cast<unsigned long>(cmd.substring(7).toInt());
    if (runMs == 0) {
      Serial.println("Usage: RUN2MS <milliseconds>");
    } else {
      runPump(PUMP2, runMs);
    }
  } else if (upper.startsWith("RUN3MS ")) {
    const unsigned long runMs = static_cast<unsigned long>(cmd.substring(7).toInt());
    if (runMs == 0) {
      Serial.println("Usage: RUN3MS <milliseconds>");
    } else {
      runPump(PUMP3, runMs);
    }
  } else if (upper.startsWith("RUN1ML ")) {
    const float targetMl = cmd.substring(7).toFloat();
    runVolumeCommand(PUMP1, PUMP1_STEADY_ML_PER_SEC, PUMP1_STARTUP_LAG_SEC, targetMl);
  } else if (upper.startsWith("RUN2ML ")) {
    const float targetMl = cmd.substring(7).toFloat();
    runVolumeCommand(PUMP2, PUMP2_STEADY_ML_PER_SEC, PUMP2_STARTUP_LAG_SEC, targetMl);
  } else if (upper == "PRIME") {
    primeFeedLines(PRIME_PUMPS_12_MS);
  } else if (upper.startsWith("PRIMESEC ")) {
    const float seconds = cmd.substring(9).toFloat();
    if (seconds <= 0.0f) {
      Serial.println("Usage: PRIMESEC <seconds>");
    } else {
      primeFeedLines(static_cast<unsigned long>(seconds * 1000.0f));
    }
  } else if (upper.startsWith("RUNBOTH ")) {
    const int firstSpace = cmd.indexOf(' ');
    const int secondSpace = cmd.indexOf(' ', firstSpace + 1);
    if (firstSpace < 0 || secondSpace < 0) {
      Serial.println("Usage: RUNBOTH <pump1_mL> <pump2_mL>");
    } else {
      const float pump1Ml = cmd.substring(firstSpace + 1, secondSpace).toFloat();
      const float pump2Ml = cmd.substring(secondSpace + 1).toFloat();
      runDualVolumeCommand(pump1Ml, pump2Ml);
    }
  } else if (upper.startsWith("RUNSIM ")) {
    const int firstSpace = cmd.indexOf(' ');
    const int secondSpace = cmd.indexOf(' ', firstSpace + 1);
    if (firstSpace < 0 || secondSpace < 0) {
      Serial.println("Usage: RUNSIM <pump1_mL> <pump2_mL>");
    } else {
      const float pump1Ml = cmd.substring(firstSpace + 1, secondSpace).toFloat();
      const float pump2Ml = cmd.substring(secondSpace + 1).toFloat();
      runDualVolumeSimultaneousCommand(pump1Ml, pump2Ml);
    }
  } else if (upper == "STATUS") {
    Serial.print("Current concentration (%): ");
    Serial.println(currentConcentration, 2);
    Serial.print("Pump 1 steady mL/sec: ");
    Serial.println(PUMP1_STEADY_ML_PER_SEC, 4);
    Serial.print("Pump 1 startup lag (s): ");
    Serial.println(PUMP1_STARTUP_LAG_SEC, 2);
    Serial.print("Pump 2 steady mL/sec: ");
    Serial.println(PUMP2_STEADY_ML_PER_SEC, 4);
    Serial.print("Pump 2 startup lag (s): ");
    Serial.println(PUMP2_STARTUP_LAG_SEC, 2);
    Serial.print("Prime time for Pumps 1+2 (ms): ");
    Serial.println(PRIME_PUMPS_12_MS);
    Serial.print("Pump 3 final transfer time (ms): ");
    Serial.println(PUMP3_FINAL_TRANSFER_MS);
  } else if (upper == "STOP") {
    stopAllPumps();
    Serial.println("All pumps stopped.");
  } else if (upper.startsWith("SETC ")) {
    const float requested = cmd.substring(5).toFloat();
    currentConcentration = clampDose(requested);
    Serial.print("Current concentration manually set to (%): ");
    Serial.println(currentConcentration, 2);
  } else if (upper == "HELP") {
    Serial.println("Commands: PRIME, PRIMESEC <sec>, CAL1, CAL2, CAL3, CAL1SEC <sec>, CAL2SEC <sec>, CAL3SEC <sec>, RUN1MS <ms>, RUN2MS <ms>, RUN3MS <ms>, RUN1ML <mL>, RUN2ML <mL>, RUNBOTH <mL1> <mL2>, RUNSIM <mL1> <mL2>, STATUS, SETC <value>, STOP, HELP");
  } else {
    Serial.println("Commands: PRIME, PRIMESEC <sec>, CAL1, CAL2, CAL3, CAL1SEC <sec>, CAL2SEC <sec>, CAL3SEC <sec>, RUN1MS <ms>, RUN2MS <ms>, RUN3MS <ms>, RUN1ML <mL>, RUN2ML <mL>, RUNBOTH <mL1> <mL2>, RUNSIM <mL1> <mL2>, STATUS, SETC <value>, STOP, HELP");
  }
}

void processSession(const SessionData &data, const uint8_t senderMac[6]) {
  if (haveLastAck && sameMac(senderMac, lastSenderMac) && data.sessionId == lastAck.sessionId) {
    Serial.println("Duplicate session detected. Re-sending previous ACK without re-running pumps.");
    sendAck(senderMac, lastAck);
    return;
  }

  Serial.println();
  Serial.println("New handheld session received.");
  Serial.print("Session ID: ");
  Serial.println(data.sessionId);
  Serial.print("Puffs: ");
  Serial.println(data.totalPuffs);
  Serial.print("Duration (s): ");
  Serial.println(data.totalDuration, 1);

  const float surveyScore = promptForSurveyScore();
  const uint8_t triggerCode = promptForTriggerCode();

  const uint8_t usageTier = classifyUsageTier(data.totalDuration);
  const float mu = usageMultiplierForTier(usageTier);

  uint8_t surveyTier = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    if (surveyScore == SURVEY_VALUES[i]) {
      surveyTier = i;
      break;
    }
  }

  const char *scenarioId = SCENARIO_IDS[usageTier][surveyTier];
  const float adaptiveMultiplier = 1.0f + surveyScore + (1.0f - mu);
  const float preliminaryConcentration = currentConcentration - (TAPER_STEP_PERCENT * adaptiveMultiplier);
  const float clampedConcentration = clampDose(preliminaryConcentration);

  const float nicotineVolumeMl =
    (clampedConcentration / MAX_CONCENTRATION_PERCENT) * TOTAL_VOLUME_ML;
  const float diluentVolumeMl = TOTAL_VOLUME_ML - nicotineVolumeMl;

  const unsigned long nicotineRunMs =
    runtimeForTargetMl(nicotineVolumeMl, PUMP1_STEADY_ML_PER_SEC, PUMP1_STARTUP_LAG_SEC);
  const unsigned long diluentRunMs =
    runtimeForTargetMl(diluentVolumeMl, PUMP2_STEADY_ML_PER_SEC, PUMP2_STARTUP_LAG_SEC);

  printSessionSummary(
    data,
    surveyScore,
    triggerCode,
    scenarioId,
    mu,
    adaptiveMultiplier,
    preliminaryConcentration,
    clampedConcentration
  );

  Serial.println("Applying safety clamp range 0.5% to 5.0%");
  Serial.print("Nicotine volume (mL): ");
  Serial.println(nicotineVolumeMl, 2);
  Serial.print("Diluent volume (mL): ");
  Serial.println(diluentVolumeMl, 2);

  Serial.println();
  Serial.println("STEP 1: Pump nicotine reservoir with Pump 1.");
  runPump(PUMP1, nicotineRunMs);

  Serial.println("STEP 2: Pump diluent reservoir with Pump 2.");
  runPump(PUMP2, diluentRunMs);

  Serial.println("STEP 3: Run Pump 3 to pull the final concentration through the long tubing.");
  runPump(PUMP3, PUMP3_FINAL_TRANSFER_MS);

  currentConcentration = clampedConcentration;

  SyncAck ack = {};
  ack.accepted = true;
  ack.sessionId = data.sessionId;
  ack.adaptiveMultiplier = adaptiveMultiplier;
  ack.appliedConcentration = currentConcentration;
  snprintf(ack.scenarioId, sizeof(ack.scenarioId), "%s", scenarioId);

  memcpy(lastSenderMac, senderMac, 6);
  lastAck = ack;
  haveLastAck = true;

  sendAck(senderMac, ack);

  Serial.println("ACK sent back to handheld.");
  Serial.print("New current concentration (%): ");
  Serial.println(currentConcentration, 2);
}

void printStartupHelp() {
  Serial.println();
  Serial.println("Homebase breadboard build ready.");
  Serial.println("Three-pump wiring:");
  Serial.println("- Pump 1 ENA = GPIO25");
  Serial.println("- Pump 1 IN1 = GPIO26");
  Serial.println("- Pump 1 IN2 = GPIO27");
  Serial.println("- Pump 2 ENA = GPIO14");
  Serial.println("- Pump 2 IN1 = GPIO19");
  Serial.println("- Pump 2 IN2 = GPIO18");
  Serial.println("- Pump 3 ENA = GPIO33");
  Serial.println("- Pump 3 IN1 = GPIO32");
  Serial.println("- Pump 3 IN2 = GPIO23");
  Serial.println("When a handheld session arrives, enter:");
  Serial.println("- survey choice 1..4");
  Serial.println("- trigger code 0..7");
  Serial.println("Prime command example: PRIME or PRIMESEC 15");
  Serial.println("Test command examples: RUNBOTH 20 10, RUNSIM 20 10, RUN3MS 60000");
  Serial.println("Pump 3 automatically runs for 60 seconds after Pumps 1 and 2 during a dosing cycle.");
  Serial.println("Manual commands: PRIME, PRIMESEC <sec>, CAL1, CAL2, CAL3, CAL1SEC <sec>, CAL2SEC <sec>, CAL3SEC <sec>, RUN1MS <ms>, RUN2MS <ms>, RUN3MS <ms>, RUN1ML <mL>, RUN2ML <mL>, RUNBOTH <mL1> <mL2>, RUNSIM <mL1> <mL2>, STATUS, SETC <value>, STOP, HELP");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  setupPumpDriver(PUMP1);
  setupPumpDriver(PUMP2);
  setupPumpDriver(PUMP3);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed on homebase.");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  printStartupHelp();
  printMacAddress();
}

void loop() {
  handleSerialCommands();

  SessionData session = {};
  uint8_t senderMac[6] = {};

  if (fetchPendingSession(session, senderMac)) {
    processSession(session, senderMac);
  }
}
