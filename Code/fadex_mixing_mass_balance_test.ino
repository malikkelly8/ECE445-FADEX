#include <Arduino.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>

// FadeX mixing-reservoir mass-balance test.
//
// Test setup:
// - Pump 1 moves "nicotine" reservoir liquid into the mixing reservoir.
// - Pump 2 moves diluent reservoir liquid into the mixing reservoir.
// - For this test, both liquids can be water.
//
// Load-cell assumption:
// - LCA1 holds the nicotine/source reservoir.
// - LCA2 holds the diluent/source reservoir.
// - LCA3 holds the mixing reservoir.
//
// The sketch tares with the full source reservoirs and empty mixing reservoir
// already in place. During pumping, source reservoirs read negative because
// mass is leaving them. That is expected. The mass-balance check compares:
//
//   total source mass lost ~= mixing reservoir mass gained

struct PumpChannel {
  int in1Gpio;
  int in2Gpio;
  const char *name;
};

struct LoadCellChannel {
  int doutGpio;
  float calibrationFactor;
  const char *name;
  const char *role;
  long tareOffset;
};

constexpr PumpChannel NIC_PUMP = {22, 21, "Pump 1 / Nic reservoir"};
constexpr PumpChannel DIL_PUMP = {19, 18, "Pump 2 / Diluent reservoir"};
constexpr PumpChannel MIX_PUMP = {33, 4, "Pump 3 / Mixing"};

constexpr int HX711_SCK_GPIO = 23;
constexpr float LOADCELL_CAL_FACTORS[] = {
  3053.60f,
  3109.60f,
  2951.40f,
  2925.40f
};

LoadCellChannel LOAD_CELLS[] = {
  {25, LOADCELL_CAL_FACTORS[0], "LCA1", "Nic source reservoir", 0},
  {26, LOADCELL_CAL_FACTORS[1], "LCA2", "Diluent source reservoir", 0},
  {27, LOADCELL_CAL_FACTORS[2], "LCA3", "Mixing reservoir", 0},
  {13, LOADCELL_CAL_FACTORS[3], "LCA4", "Spare / optional", 0}
};

constexpr uint8_t NIC_SOURCE_CELL_INDEX = 0;
constexpr uint8_t DIL_SOURCE_CELL_INDEX = 1;
constexpr uint8_t MIXING_CELL_INDEX = 2;
constexpr uint8_t MONITORED_CELL_COUNT = 3;

constexpr float WATER_GRAMS_PER_ML = 1.0f;
constexpr size_t MOVING_AVERAGE_SAMPLES = 4;
constexpr size_t TARE_SAMPLES = 20;
constexpr unsigned long SCALE_SETTLE_MS = 350;
constexpr unsigned long HX711_SAMPLE_DELAY_MS = 25;
constexpr unsigned long HX711_READY_TIMEOUT_MS = 3000;
constexpr unsigned long MAX_SINGLE_PUMP_TIME_MS = 90000;

// Burst timing uses a fast coarse-fill phase for the 8-12 inch tubing, then
// shorter bursts near the target so the scale can settle before final trim.
constexpr float FAST_ZONE_G = 10.0f;
constexpr float MEDIUM_ZONE_G = 4.0f;
constexpr float SLOW_ZONE_G = 1.0f;
constexpr unsigned long FAST_BURST_MS = 1800;
constexpr unsigned long MEDIUM_BURST_MS = 650;
constexpr unsigned long SLOW_BURST_MS = 180;
constexpr unsigned long TRIM_BURST_MS = 45;
constexpr float MIXING_TARGET_TOLERANCE_G = 0.25f;
constexpr float SOURCE_GUARD_ZONE_G = 2.0f;
constexpr float SOURCE_TRIM_ZONE_G = 1.0f;
constexpr float SOURCE_OVERRUN_TOLERANCE_G = 0.60f;
constexpr float SOURCE_OVERRUN_TOLERANCE_PERCENT = 8.0f;

// Mass-balance tolerance: require within 5% or 1.0 g, whichever is larger.
constexpr float MASS_BALANCE_TOLERANCE_PERCENT = 5.0f;
constexpr float MIN_MASS_BALANCE_TOLERANCE_G = 1.0f;
constexpr float MIXING_NEGATIVE_ABORT_G = -0.50f;

struct WeightSnapshot {
  bool valid;
  float nicReading;
  float dilReading;
  float mixReading;
  float nicLoss;
  float dilLoss;
  float totalSourceLoss;
  float mixingGain;
};

const char *resetReasonLabel(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software reset";
    case ESP_RST_PANIC: return "panic / exception";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}

void printResetReason() {
  const esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Last ESP32 reset reason: ");
  Serial.println(resetReasonLabel(reason));
  if (reason == ESP_RST_BROWNOUT) {
    Serial.println("Brownout usually means pump current/noise is pulling the ESP32 supply down.");
  }
}

void stopPump(const PumpChannel &pump) {
  digitalWrite(pump.in1Gpio, LOW);
  digitalWrite(pump.in2Gpio, LOW);
}

void stopAllPumps() {
  stopPump(NIC_PUMP);
  stopPump(DIL_PUMP);
  stopPump(MIX_PUMP);
}

void runPumpForward(const PumpChannel &pump) {
  // The pump wiring is flipped relative to the original assumption, so the
  // software "forward" direction drives IN1 low and IN2 high.
  digitalWrite(pump.in1Gpio, LOW);
  digitalWrite(pump.in2Gpio, HIGH);
}

void setupPump(const PumpChannel &pump) {
  pinMode(pump.in1Gpio, OUTPUT);
  pinMode(pump.in2Gpio, OUTPUT);
  stopPump(pump);
}

bool waitForAllLoadCellsReady(unsigned long timeoutMs) {
  const unsigned long startMs = millis();
  while (millis() - startMs < timeoutMs) {
    bool allReady = true;
    for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
      if (digitalRead(LOAD_CELLS[i].doutGpio) == HIGH) {
        allReady = false;
        break;
      }
    }

    if (allReady) {
      return true;
    }

    delay(2);
    yield();
  }

  return false;
}

long signExtendHx711(uint32_t raw24) {
  if (raw24 & 0x800000UL) {
    raw24 |= 0xFF000000UL;
  }
  return static_cast<int32_t>(raw24);
}

bool readSharedRawValues(long rawValues[], uint8_t count, unsigned long timeoutMs) {
  if (!waitForAllLoadCellsReady(timeoutMs)) {
    return false;
  }

  uint32_t unsignedValues[MONITORED_CELL_COUNT] = {};

  // All HX711 boards share SCLK on the PCB. This loop clocks every amplifier
  // together and samples every DOUT line on the same 24 pulses. Reading the
  // boards one-at-a-time would shift the other boards too and corrupt readings.
  noInterrupts();
  for (uint8_t bitIndex = 0; bitIndex < 24; ++bitIndex) {
    digitalWrite(HX711_SCK_GPIO, HIGH);
    delayMicroseconds(1);

    for (uint8_t i = 0; i < count; ++i) {
      unsignedValues[i] <<= 1;
      if (digitalRead(LOAD_CELLS[i].doutGpio) == HIGH) {
        unsignedValues[i] |= 1;
      }
    }

    digitalWrite(HX711_SCK_GPIO, LOW);
    delayMicroseconds(1);
  }

  // One extra clock pulse selects HX711 channel A with gain 128 for the next
  // conversion, matching the default SparkFun/bogde HX711 setup.
  digitalWrite(HX711_SCK_GPIO, HIGH);
  delayMicroseconds(1);
  digitalWrite(HX711_SCK_GPIO, LOW);
  interrupts();

  for (uint8_t i = 0; i < count; ++i) {
    rawValues[i] = signExtendHx711(unsignedValues[i]);
  }

  return true;
}

bool readAverageGramsAll(float grams[], size_t sampleCount) {
  double sums[MONITORED_CELL_COUNT] = {};
  size_t validSamples = 0;

  for (size_t sample = 0; sample < sampleCount; ++sample) {
    long rawValues[MONITORED_CELL_COUNT] = {};
    if (!readSharedRawValues(rawValues, MONITORED_CELL_COUNT, HX711_READY_TIMEOUT_MS)) {
      return false;
    }

    for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
      const LoadCellChannel &cell = LOAD_CELLS[i];
      sums[i] += (static_cast<float>(rawValues[i] - cell.tareOffset) /
                  cell.calibrationFactor);
    }

    delay(HX711_SAMPLE_DELAY_MS);
    yield();
    validSamples++;
  }

  if (validSamples == 0) {
    return false;
  }

  for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
    grams[i] = static_cast<float>(sums[i] / validSamples);
  }

  return true;
}

bool tareAllLoadCellsShared() {
  double offsetSums[MONITORED_CELL_COUNT] = {};
  size_t validSamples = 0;

  for (size_t sample = 0; sample < TARE_SAMPLES; ++sample) {
    long rawValues[MONITORED_CELL_COUNT] = {};
    if (!readSharedRawValues(rawValues, MONITORED_CELL_COUNT, HX711_READY_TIMEOUT_MS)) {
      Serial.println("Load cells not ready during tare. Check 3.3 V, GND, DAT, and shared SCLK.");
      return false;
    }

    for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
      offsetSums[i] += rawValues[i];
    }

    delay(HX711_SAMPLE_DELAY_MS);
    yield();
    validSamples++;
  }

  if (validSamples == 0) {
    return false;
  }

  for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
    const double averageOffset = offsetSums[i] / validSamples;
    LOAD_CELLS[i].tareOffset =
      static_cast<long>(averageOffset + (averageOffset >= 0.0 ? 0.5 : -0.5));
    Serial.print("  ");
    Serial.print(LOAD_CELLS[i].name);
    Serial.print(" tared at raw offset ");
    Serial.println(LOAD_CELLS[i].tareOffset);
  }

  return true;
}

bool tareAllCellsForMixingTest() {
  stopAllPumps();
  Serial.println();
  Serial.println("Place full source reservoirs and empty mixing reservoir on load cells.");
  Serial.println("Taring all load cells before delivery...");
  delay(1500);

  if (!tareAllLoadCellsShared()) {
    stopAllPumps();
    return false;
  }

  Serial.println("Tare complete. Source weight loss will show as negative grams.");
  return true;
}

bool stopRequestedFromSerial() {
  if (!Serial.available()) {
    return false;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toLowerCase();
  return command == "stop" || command == "s";
}

unsigned long burstDurationForRemaining(float remainingGrams) {
  if (remainingGrams > FAST_ZONE_G) {
    return FAST_BURST_MS;
  }
  if (remainingGrams > MEDIUM_ZONE_G) {
    return MEDIUM_BURST_MS;
  }
  if (remainingGrams > SLOW_ZONE_G) {
    return SLOW_BURST_MS;
  }
  return TRIM_BURST_MS;
}

float sourceLossLimitForTarget(float targetGrams) {
  const float percentLimit =
    targetGrams * (SOURCE_OVERRUN_TOLERANCE_PERCENT / 100.0f);
  return targetGrams + fmaxf(SOURCE_OVERRUN_TOLERANCE_G, percentLimit);
}

unsigned long capBurstNearSourceTarget(unsigned long requestedBurstMs,
                                       float sourceLossGrams,
                                       float targetGrams) {
  if (sourceLossGrams >= targetGrams - SOURCE_TRIM_ZONE_G) {
    return min(requestedBurstMs, TRIM_BURST_MS);
  }

  if (sourceLossGrams >= targetGrams - SOURCE_GUARD_ZONE_G) {
    return min(requestedBurstMs, SLOW_BURST_MS);
  }

  return requestedBurstMs;
}

WeightSnapshot readWeightSnapshot() {
  WeightSnapshot weights = {};

  delay(SCALE_SETTLE_MS);

  float grams[MONITORED_CELL_COUNT] = {};
  if (!readAverageGramsAll(grams, MOVING_AVERAGE_SAMPLES)) {
    weights.valid = false;
    weights.nicReading = NAN;
    weights.dilReading = NAN;
    weights.mixReading = NAN;
    return weights;
  }

  weights.valid = true;
  weights.nicReading = grams[NIC_SOURCE_CELL_INDEX];
  weights.dilReading = grams[DIL_SOURCE_CELL_INDEX];
  weights.mixReading = grams[MIXING_CELL_INDEX];

  weights.nicLoss = fmaxf(0.0f, -weights.nicReading);
  weights.dilLoss = fmaxf(0.0f, -weights.dilReading);
  weights.totalSourceLoss = weights.nicLoss + weights.dilLoss;
  weights.mixingGain = fmaxf(0.0f, weights.mixReading);

  return weights;
}

float sourceLossFromSnapshot(const WeightSnapshot &weights, uint8_t sourceCellIndex) {
  if (sourceCellIndex == NIC_SOURCE_CELL_INDEX) {
    return weights.nicLoss;
  }
  if (sourceCellIndex == DIL_SOURCE_CELL_INDEX) {
    return weights.dilLoss;
  }
  return 0.0f;
}

void printLiveWeights(const char *label, const WeightSnapshot &weights) {
  Serial.print("LIVE WEIGHTS");
  if (label != nullptr && strlen(label) > 0) {
    Serial.print(" [");
    Serial.print(label);
    Serial.print("]");
  }

  if (!weights.valid) {
    Serial.println(" | invalid shared-HX711 read");
    return;
  }

  Serial.print(" | LCA1 nic=");
  Serial.print(weights.nicReading, 2);
  Serial.print(" g (loss ");
  Serial.print(weights.nicLoss, 2);
  Serial.print(" g)");
  Serial.print(" | LCA2 dil=");
  Serial.print(weights.dilReading, 2);
  Serial.print(" g (loss ");
  Serial.print(weights.dilLoss, 2);
  Serial.print(" g)");
  Serial.print(" | LCA3 mix=");
  Serial.print(weights.mixReading, 2);
  Serial.print(" g (gain ");
  Serial.print(weights.mixingGain, 2);
  Serial.print(" g)");
  Serial.print(" | total source loss=");
  Serial.print(weights.totalSourceLoss, 2);
  Serial.println(" g");
}

bool abortIfMixingCellWentNegative(const WeightSnapshot &weights) {
  if (!weights.valid) {
    stopAllPumps();
    Serial.println();
    Serial.println("EMERGENCY STOP: shared HX711 read failed.");
    Serial.println("Check load-cell wiring and make sure DAT1/DAT2/DAT3 share SCLK GPIO23.");
    return true;
  }

  if (weights.mixReading < MIXING_NEGATIVE_ABORT_G) {
    stopAllPumps();
    Serial.println();
    Serial.println("EMERGENCY STOP: mixing reservoir weight went negative.");
    Serial.print("Mixing reservoir reading: ");
    Serial.print(weights.mixReading, 2);
    Serial.println(" g");
    return true;
  }
  return false;
}

bool pumpSourceUntilMixingGain(const PumpChannel &pump,
                               uint8_t sourceCellIndex,
                               float doseMl,
                               float targetMixingGainGrams,
                               const char *liquidName) {
  const unsigned long startMs = millis();
  WeightSnapshot weights = readWeightSnapshot();
  if (abortIfMixingCellWentNegative(weights)) {
    return false;
  }
  float lossGrams = sourceLossFromSnapshot(weights, sourceCellIndex);
  float mixingGainGrams = weights.mixingGain;
  const float sourceLossLimitGrams =
    sourceLossLimitForTarget(targetMixingGainGrams);

  Serial.println();
  Serial.print("Delivering ");
  Serial.print(doseMl, 2);
  Serial.print(" mL of ");
  Serial.print(liquidName);
  Serial.print(" using ");
  Serial.println(pump.name);
  Serial.print("Control target: mixing reservoir gain ");
  Serial.print(targetMixingGainGrams, 2);
  Serial.println(" g");
  Serial.print("Source safety limit: ");
  Serial.print(sourceLossLimitGrams, 2);
  Serial.println(" g lost from source");

  while (mixingGainGrams < targetMixingGainGrams - MIXING_TARGET_TOLERANCE_G) {
    if (millis() - startMs > MAX_SINGLE_PUMP_TIME_MS) {
      stopAllPumps();
      Serial.println("Abort: pump safety timeout reached.");
      return false;
    }

    if (stopRequestedFromSerial()) {
      stopAllPumps();
      Serial.println("Abort: STOP command received.");
      return false;
    }

    if (lossGrams >= sourceLossLimitGrams) {
      stopAllPumps();
      Serial.println();
      Serial.println("SOURCE SAFETY STOP: source reservoir already lost too much liquid.");
      Serial.print("Source loss: ");
      Serial.print(lossGrams, 2);
      Serial.print(" g | source limit: ");
      Serial.print(sourceLossLimitGrams, 2);
      Serial.println(" g");
      Serial.println("The mixing reservoir is under-reading or liquid is not reaching it.");
      Serial.println("Check tube priming, leaks, tube contact with the cup, and load-cell placement.");
      return false;
    }

    const float remainingGrams = targetMixingGainGrams - mixingGainGrams;
    const unsigned long requestedBurstMs = burstDurationForRemaining(remainingGrams);
    const unsigned long burstMs = capBurstNearSourceTarget(
      requestedBurstMs,
      lossGrams,
      targetMixingGainGrams);

    Serial.print(liquidName);
    Serial.print(" burst ");
    Serial.print(burstMs);
    Serial.print(" ms");
    if (burstMs < requestedBurstMs) {
      Serial.print(" (source guard capped from ");
      Serial.print(requestedBurstMs);
      Serial.print(" ms)");
    }
    Serial.println();

    runPumpForward(pump);
    delay(burstMs);
    stopPump(pump);
    Serial.println("Pump stopped. Reading settled weights...");

    weights = readWeightSnapshot();
    if (abortIfMixingCellWentNegative(weights)) {
      return false;
    }

    lossGrams = sourceLossFromSnapshot(weights, sourceCellIndex);
    mixingGainGrams = weights.mixingGain;
    printLiveWeights(liquidName, weights);

    Serial.print(liquidName);
    Serial.print(" source loss: ");
    Serial.print(lossGrams, 2);
    Serial.print(" g | mixing gain: ");
    Serial.print(mixingGainGrams, 2);
    Serial.print(" g / target ");
    Serial.print(targetMixingGainGrams, 2);
    Serial.println(" g");
  }

  stopPump(pump);
  Serial.print(liquidName);
  Serial.println(" delivery complete.");
  return true;
}

float massBalanceTolerance(float expectedTotalGrams) {
  const float percentTolerance =
    expectedTotalGrams * (MASS_BALANCE_TOLERANCE_PERCENT / 100.0f);
  return fmaxf(MIN_MASS_BALANCE_TOLERANCE_G, percentTolerance);
}

void printCurrentWeights() {
  Serial.println();
  Serial.println("--- Current settled load-cell weights ---");

  const WeightSnapshot weights = readWeightSnapshot();
  if (!weights.valid) {
    Serial.println("Could not read shared HX711 values.");
    return;
  }

  const float readings[] = {
    weights.nicReading,
    weights.dilReading,
    weights.mixReading
  };

  for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
    LoadCellChannel &cell = LOAD_CELLS[i];
    Serial.print(cell.name);
    Serial.print(" (");
    Serial.print(cell.role);
    Serial.print("): ");
    Serial.print(readings[i], 2);
    Serial.println(" g");
  }
}

void runMixingMassBalanceTest(float nicMl, float dilMl) {
  if (nicMl < 0.0f || dilMl < 0.0f || (nicMl + dilMl) <= 0.0f) {
    Serial.println("Use positive targets. Example: MIXTEST 10 20");
    return;
  }

  Serial.println();
  Serial.println("=== FADEX MIXING MASS-BALANCE TEST ===");
  Serial.print("Nic/source target: ");
  Serial.print(nicMl, 2);
  Serial.println(" mL water");
  Serial.print("Diluent target: ");
  Serial.print(dilMl, 2);
  Serial.println(" mL water");

  if (!tareAllCellsForMixingTest()) {
    Serial.println("Test aborted during tare.");
    return;
  }

  printCurrentWeights();
  printLiveWeights("after tare", readWeightSnapshot());

  if (nicMl > 0.0f) {
    if (!pumpSourceUntilMixingGain(
          NIC_PUMP,
          NIC_SOURCE_CELL_INDEX,
          nicMl,
          nicMl * WATER_GRAMS_PER_ML,
          "NIC/WATER")) {
      return;
    }
  }

  if (dilMl > 0.0f) {
    if (!pumpSourceUntilMixingGain(
          DIL_PUMP,
          DIL_SOURCE_CELL_INDEX,
          dilMl,
          (nicMl + dilMl) * WATER_GRAMS_PER_ML,
          "DIL/WATER")) {
      return;
    }
  }

  stopAllPumps();
  delay(1000);

  const WeightSnapshot finalWeights = readWeightSnapshot();
  if (!finalWeights.valid) {
    Serial.println("Final shared HX711 read failed. Test result is invalid.");
    return;
  }

  const float nicSourceReading = finalWeights.nicReading;
  const float dilSourceReading = finalWeights.dilReading;
  const float mixReading = finalWeights.mixReading;

  const float nicLoss = fmaxf(0.0f, -nicSourceReading);
  const float dilLoss = fmaxf(0.0f, -dilSourceReading);
  const float totalSourceLoss = nicLoss + dilLoss;
  const float mixingGain = fmaxf(0.0f, mixReading);
  const float expectedTotal = (nicMl + dilMl) * WATER_GRAMS_PER_ML;
  const float massBalanceError = fabsf(totalSourceLoss - mixingGain);
  const float targetError = fabsf(totalSourceLoss - expectedTotal);
  const float allowedError = massBalanceTolerance(expectedTotal);

  Serial.println();
  Serial.println("=== MIXING TEST RESULTS ===");
  Serial.print("Nic source final reading: ");
  Serial.print(nicSourceReading, 2);
  Serial.print(" g | loss: ");
  Serial.print(nicLoss, 2);
  Serial.println(" g");

  Serial.print("Diluent source final reading: ");
  Serial.print(dilSourceReading, 2);
  Serial.print(" g | loss: ");
  Serial.print(dilLoss, 2);
  Serial.println(" g");

  Serial.print("Mixing reservoir gain: ");
  Serial.print(mixingGain, 2);
  Serial.println(" g");

  Serial.print("Total source loss: ");
  Serial.print(totalSourceLoss, 2);
  Serial.println(" g");

  Serial.print("Expected delivered mass: ");
  Serial.print(expectedTotal, 2);
  Serial.println(" g");

  Serial.print("Source-vs-mix mass-balance error: ");
  Serial.print(massBalanceError, 2);
  Serial.print(" g | allowed: +/-");
  Serial.print(allowedError, 2);
  Serial.println(" g");

  Serial.print("Target delivery error from source loss: ");
  Serial.print(targetError, 2);
  Serial.println(" g");

  if (massBalanceError <= allowedError) {
    Serial.println("PASS: source loss matches mixing gain within tolerance.");
  } else {
    Serial.println("FAIL: source loss does not match mixing gain.");
    Serial.println("Check tubing leaks, splashing, load-cell placement, and vibration.");
  }
}

void runSinglePumpMassBalanceTest(uint8_t pumpNumber, float targetMl) {
  if (pumpNumber == 1) {
    runMixingMassBalanceTest(targetMl, 0.0f);
    return;
  }

  if (pumpNumber == 2) {
    runMixingMassBalanceTest(0.0f, targetMl);
    return;
  }

  Serial.println("For this stage, use only Pump 1 or Pump 2.");
  Serial.println("Example: MIXONE 1 20 or MIXONE 2 20");
}

void printHelp() {
  Serial.println();
  Serial.println("FadeX mixing mass-balance test commands:");
  Serial.println("  MIXONE 1 20   -> run only Pump 1 until 20 mL leaves source 1");
  Serial.println("  MIXONE 2 20   -> run only Pump 2 until 20 mL leaves source 2");
  Serial.println("  MIXTEST 10 20  -> deliver 10 mL from Pump 1 and 20 mL from Pump 2");
  Serial.println("  MIXTEST 20 10  -> deliver 20 mL from Pump 1 and 10 mL from Pump 2");
  Serial.println("  MIXTEST 15 15  -> equal-volume test");
  Serial.println("  READ           -> print settled load-cell readings");
  Serial.println("  TARE           -> tare all load cells with reservoirs in place");
  Serial.println("  STOP           -> stop all pumps");
  Serial.println("  HELP           -> print this menu");
  Serial.println();
  Serial.println("Safety:");
  Serial.println("  Only one pump is ever on at a time.");
  Serial.println("  MIXTEST runs Pump 1 first, stops it, then runs Pump 2.");
  Serial.println("  Pump stop is controlled by mixing reservoir gain, not source loss.");
  Serial.println("  Weight lines print after each pump burst while pumps are off.");
  Serial.println();
  Serial.println("Suggested test set:");
  Serial.println("  MIXONE 1 10");
  Serial.println("  MIXONE 2 10");
  Serial.println("  MIXTEST 5 5");
  Serial.println("  MIXTEST 10 20");
  Serial.println("  MIXTEST 20 10");
  Serial.println("  MIXTEST 15 15");
}

void handleCommand(String command) {
  command.trim();
  String upper = command;
  upper.toUpperCase();

  if (upper == "HELP" || upper == "?") {
    printHelp();
  } else if (upper == "STOP") {
    stopAllPumps();
    Serial.println("All pumps stopped.");
  } else if (upper == "READ") {
    printCurrentWeights();
  } else if (upper == "TARE") {
    tareAllCellsForMixingTest();
  } else if (upper.startsWith("MIXTEST ")) {
    float nicMl = 0.0f;
    float dilMl = 0.0f;
    if (sscanf(command.c_str(), "%*s %f %f", &nicMl, &dilMl) == 2) {
      runMixingMassBalanceTest(nicMl, dilMl);
    } else {
      Serial.println("Use: MIXTEST <nic mL> <diluent mL>");
    }
  } else if (upper.startsWith("MIXONE ")) {
    int pumpNumber = 0;
    float targetMl = 0.0f;
    if (sscanf(command.c_str(), "%*s %d %f", &pumpNumber, &targetMl) == 2) {
      runSinglePumpMassBalanceTest(static_cast<uint8_t>(pumpNumber), targetMl);
    } else {
      Serial.println("Use: MIXONE <pump 1-2> <mL>");
      Serial.println("Example: MIXONE 1 20");
    }
  } else if (command.length() > 0) {
    Serial.println("Unknown command. Type HELP.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  printResetReason();

  setupPump(NIC_PUMP);
  setupPump(DIL_PUMP);
  setupPump(MIX_PUMP);

  pinMode(HX711_SCK_GPIO, OUTPUT);
  digitalWrite(HX711_SCK_GPIO, LOW);

  for (LoadCellChannel &cell : LOAD_CELLS) {
    pinMode(cell.doutGpio, INPUT);

    Serial.print(cell.name);
    Serial.print(" ");
    Serial.print(cell.role);
    Serial.print(" on DOUT GPIO");
    Serial.print(cell.doutGpio);
    Serial.println();
  }

  Serial.println();
  Serial.println("Pump pins:");
  Serial.println("  Pump 1 / nic source: IN1 GPIO22, IN2 GPIO21");
  Serial.println("  Pump 2 / diluent source: IN1 GPIO19, IN2 GPIO18");
  Serial.println("  Pump 3 / mixing: IN1 GPIO33, IN2 GPIO4");
  Serial.println("Pump polarity: flipped in software, so forward = IN1 LOW / IN2 HIGH");
  Serial.println("HX711 shared SCLK: GPIO23");
  Serial.println("HX711 read mode: shared-clock simultaneous read for LCA1/LCA2/LCA3");

  printHelp();
}

void loop() {
  if (Serial.available() > 0) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  delay(20);
}