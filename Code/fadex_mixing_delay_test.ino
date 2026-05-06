#include <Arduino.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>

// FadeX mixing-reservoir mass-balance test.
// This version adds more wait time after each pump burst so liquid can finish
// dripping and the mixing-reservoir load cell can settle before the next burst.

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

// --- PIN DEFINITIONS ---
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

// --- LOAD-CELL STABILITY CONSTANTS ---
constexpr size_t MOVING_AVERAGE_SAMPLES = 12;
constexpr size_t TARE_SAMPLES = 40;
constexpr unsigned long SCALE_SETTLE_MS = 2000;
constexpr unsigned long BETWEEN_PUMP_BURSTS_MS = 1500;
constexpr unsigned long HX711_SAMPLE_DELAY_MS = 15;
constexpr unsigned long HX711_READY_TIMEOUT_MS = 3000;
constexpr unsigned long MAX_SINGLE_PUMP_TIME_MS = 90000;

// --- BURST TIMING ---
// These are pump ON-times. Larger values pump more liquid.
constexpr float FAST_ZONE_G = 10.0f;
constexpr float MEDIUM_ZONE_G = 4.0f;
constexpr float SLOW_ZONE_G = 1.0f;
constexpr unsigned long FAST_BURST_MS = 1800;
constexpr unsigned long MEDIUM_BURST_MS = 650;
constexpr unsigned long SLOW_BURST_MS = 180;
constexpr unsigned long TRIM_BURST_MS = 45;

constexpr uint8_t NIC_SOURCE_CELL_INDEX = 0;
constexpr uint8_t DIL_SOURCE_CELL_INDEX = 1;
constexpr uint8_t MIXING_CELL_INDEX = 2;
constexpr uint8_t MONITORED_CELL_COUNT = 3;

constexpr float MIXING_TARGET_TOLERANCE_G = 0.25f;
constexpr float WATER_GRAMS_PER_ML = 1.0f;

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
  // Your pump wiring is flipped, so forward is IN1 LOW and IN2 HIGH.
  digitalWrite(pump.in1Gpio, LOW);
  digitalWrite(pump.in2Gpio, HIGH);
}

void setupPump(const PumpChannel &pump) {
  pinMode(pump.in1Gpio, OUTPUT);
  pinMode(pump.in2Gpio, OUTPUT);
  stopPump(pump);
}

long signExtendHx711(uint32_t raw24) {
  if (raw24 & 0x800000UL) {
    raw24 |= 0xFF000000UL;
  }
  return static_cast<int32_t>(raw24);
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

    delay(1);
    yield();
  }

  return false;
}

bool readSharedRawValues(long rawValues[], uint8_t count, unsigned long timeoutMs) {
  if (!waitForAllLoadCellsReady(timeoutMs)) {
    return false;
  }

  uint32_t unsignedValues[MONITORED_CELL_COUNT] = {};

  // All HX711 boards share SCLK, so every amplifier must be clocked together.
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

  // One extra pulse selects HX711 channel A, gain 128, for the next read.
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

  for (size_t sample = 0; sample < sampleCount; ++sample) {
    long rawValues[MONITORED_CELL_COUNT] = {};
    if (!readSharedRawValues(rawValues, MONITORED_CELL_COUNT, HX711_READY_TIMEOUT_MS)) {
      return false;
    }

    for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
      sums[i] += (static_cast<float>(rawValues[i] - LOAD_CELLS[i].tareOffset) /
                  LOAD_CELLS[i].calibrationFactor);
    }

    delay(HX711_SAMPLE_DELAY_MS);
    yield();
  }

  for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
    grams[i] = static_cast<float>(sums[i] / sampleCount);
  }

  return true;
}

WeightSnapshot readWeightSnapshot() {
  WeightSnapshot weights = {};

  // Wait after the pump stops so liquid can drip out and the cup can stop moving.
  delay(SCALE_SETTLE_MS);

  float grams[MONITORED_CELL_COUNT] = {};
  if (!readAverageGramsAll(grams, MOVING_AVERAGE_SAMPLES)) {
    weights.valid = false;
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

bool tareAllLoadCells() {
  Serial.println("Taring all load cells...");

  double offsetSums[MONITORED_CELL_COUNT] = {};

  for (size_t sample = 0; sample < TARE_SAMPLES; ++sample) {
    long rawValues[MONITORED_CELL_COUNT] = {};
    if (!readSharedRawValues(rawValues, MONITORED_CELL_COUNT, HX711_READY_TIMEOUT_MS)) {
      Serial.println("Tare failed: load cells not ready.");
      return false;
    }

    for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
      offsetSums[i] += rawValues[i];
    }

    delay(10);
    yield();
  }

  for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
    LOAD_CELLS[i].tareOffset =
      static_cast<long>(offsetSums[i] / static_cast<double>(TARE_SAMPLES));
    Serial.print("  ");
    Serial.print(LOAD_CELLS[i].name);
    Serial.print(" tare offset: ");
    Serial.println(LOAD_CELLS[i].tareOffset);
  }

  return true;
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

bool stopRequestedFromSerial() {
  if (!Serial.available()) {
    return false;
  }

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toUpperCase();
  return cmd == "STOP" || cmd == "S";
}

bool pumpUntilMixingTarget(const PumpChannel &pump,
                           float targetMixingGrams,
                           const char *label) {
  const unsigned long startMs = millis();

  WeightSnapshot snapshot = readWeightSnapshot();
  if (!snapshot.valid) {
    Serial.println("Could not read LCA3 before pumping.");
    return false;
  }

  float current = snapshot.mixingGain;

  while (current < targetMixingGrams - MIXING_TARGET_TOLERANCE_G) {
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

    const float remaining = targetMixingGrams - current;
    const unsigned long burstMs = burstDurationForRemaining(remaining);

    Serial.print(label);
    Serial.print(" burst ");
    Serial.print(burstMs);
    Serial.print(" ms | remaining ");
    Serial.print(remaining, 2);
    Serial.println(" g");

    runPumpForward(pump);
    delay(burstMs);
    stopPump(pump);

    // Extra wait before even starting the averaged scale read.
    Serial.print("Pump stopped. Waiting ");
    Serial.print(BETWEEN_PUMP_BURSTS_MS);
    Serial.println(" ms for tubing drip and cup motion...");
    delay(BETWEEN_PUMP_BURSTS_MS);

    snapshot = readWeightSnapshot();
    if (!snapshot.valid) {
      stopAllPumps();
      Serial.println("Abort: load-cell read failed.");
      return false;
    }

    current = snapshot.mixingGain;
    Serial.print(label);
    Serial.print(" -> LCA3 mixing gain: ");
    Serial.print(current, 2);
    Serial.print(" g / target ");
    Serial.print(targetMixingGrams, 2);
    Serial.println(" g");
  }

  stopPump(pump);
  Serial.print(label);
  Serial.println(" delivery complete.");
  return true;
}

void runMixingMassBalanceTest(float nicMl, float dilMl) {
  if (nicMl < 0.0f || dilMl < 0.0f || (nicMl + dilMl) <= 0.0f) {
    Serial.println("Use positive targets. Example: MIXTEST 10 0");
    return;
  }

  Serial.println();
  Serial.println("=== STARTING DELAYED MIXING TEST ===");
  Serial.print("Pump 1 target: ");
  Serial.print(nicMl, 2);
  Serial.println(" mL");
  Serial.print("Pump 2 target: ");
  Serial.print(dilMl, 2);
  Serial.println(" mL");

  stopAllPumps();
  delay(500);

  if (!tareAllLoadCells()) {
    Serial.println("Test aborted during tare.");
    return;
  }

  WeightSnapshot afterTare = readWeightSnapshot();
  Serial.print("After tare LCA3 mixing: ");
  Serial.print(afterTare.mixReading, 2);
  Serial.println(" g");

  if (nicMl > 0.0f) {
    const float nicTarget = nicMl * WATER_GRAMS_PER_ML;
    if (!pumpUntilMixingTarget(NIC_PUMP, nicTarget, "Nic Pump")) {
      return;
    }
  }

  if (dilMl > 0.0f) {
    const float totalTarget = (nicMl + dilMl) * WATER_GRAMS_PER_ML;
    if (!pumpUntilMixingTarget(DIL_PUMP, totalTarget, "Dil Pump")) {
      return;
    }
  }

  delay(2000);
  WeightSnapshot finalSnapshot = readWeightSnapshot();
  if (!finalSnapshot.valid) {
    Serial.println("Final load-cell read failed.");
    return;
  }

  const float expected = (nicMl + dilMl) * WATER_GRAMS_PER_ML;
  const float error = fabsf(finalSnapshot.mixingGain - expected);

  Serial.println();
  Serial.println("=== TEST COMPLETE ===");
  Serial.print("Final LCA3 mixing gain: ");
  Serial.print(finalSnapshot.mixingGain, 2);
  Serial.println(" g");
  Serial.print("Expected: ");
  Serial.print(expected, 2);
  Serial.println(" g");
  Serial.print("Error: ");
  Serial.print(error, 2);
  Serial.println(" g");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  setupPump(NIC_PUMP);
  setupPump(DIL_PUMP);
  setupPump(MIX_PUMP);

  pinMode(HX711_SCK_GPIO, OUTPUT);
  digitalWrite(HX711_SCK_GPIO, LOW);

  for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
    pinMode(LOAD_CELLS[i].doutGpio, INPUT);
  }

  Serial.println("System Ready.");
  Serial.println("Use MIXTEST <pump1 mL> <pump2 mL> to begin.");
  Serial.println("Example: MIXTEST 10 0");
  Serial.println("Type STOP during pumping to abort.");
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    String upper = cmd;
    upper.toUpperCase();

    if (upper.startsWith("MIXTEST")) {
      float n = 0.0f;
      float d = 0.0f;
      if (sscanf(cmd.c_str(), "%*s %f %f", &n, &d) == 2) {
        runMixingMassBalanceTest(n, d);
      } else {
        Serial.println("Use: MIXTEST <pump1 mL> <pump2 mL>");
        Serial.println("Example: MIXTEST 10 0");
      }
    } else if (upper == "STOP") {
      stopAllPumps();
      Serial.println("All pumps stopped.");
    } else {
      Serial.println("Unknown command. Use MIXTEST 10 0 or STOP.");
    }
  }
}
