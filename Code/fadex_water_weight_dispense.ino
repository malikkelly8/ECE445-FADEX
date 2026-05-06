#include <Arduino.h>
#include <math.h>

// FadeX water weight dispense test.
//
// Goal:
// - Dispense water by measuring how much mass appears in the mixing cup.
// - For water, 1 mL is approximately 1 g.
// - This sketch tares LCA3 with the receiving/mixing cup in place, then runs
//   Pump 1 or Pump 2 in short bursts until LCA3 reaches the requested target.
//
// Hardware assumptions:
// - Pump 1 feeds the mixing reservoir/cup.
// - Pump 2 feeds the same mixing reservoir/cup.
// - LCA3 is under the mixing reservoir/cup.
// - Pump wiring is flipped, so software "forward" is IN1 LOW and IN2 HIGH.

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

constexpr PumpChannel PUMP_1 = {22, 21, "Pump 1 / Nic-water"};
constexpr PumpChannel PUMP_2 = {19, 18, "Pump 2 / Diluent-water"};
constexpr PumpChannel PUMP_3 = {33, 4, "Pump 3 / disabled in this test"};

constexpr int HX711_SCK_GPIO = 23;
constexpr LoadCellChannel MIXING_LOAD_CELL_TEMPLATE = {
  27,
  2951.40f,
  "LCA3",
  "Mixing reservoir",
  0
};

LoadCellChannel mixingLoadCell = MIXING_LOAD_CELL_TEMPLATE;

constexpr float WATER_GRAMS_PER_ML = 1.0f;

// Accuracy-first settings. Increase burst times if the test is too slow after
// tubing is fully primed; decrease them if you overshoot the target.
constexpr unsigned long FAST_BURST_MS = 1000;
constexpr unsigned long MEDIUM_BURST_MS = 350;
constexpr unsigned long SLOW_BURST_MS = 100;
constexpr unsigned long TRIM_BURST_MS = 30;

constexpr float FAST_ZONE_G = 12.0f;
constexpr float MEDIUM_ZONE_G = 6.0f;
constexpr float SLOW_ZONE_G = 2.0f;
constexpr float TARGET_TOLERANCE_G = 0.20f;

constexpr size_t TARE_SAMPLES = 30;
constexpr size_t READ_AVERAGE_SAMPLES = 8;
constexpr unsigned long SCALE_SETTLE_MS = 450;
constexpr unsigned long HX711_READY_TIMEOUT_MS = 3000;
constexpr unsigned long HX711_SAMPLE_DELAY_MS = 15;
constexpr unsigned long MAX_PUMP_ON_TIME_MS = 180000;

bool stopRequested = false;
bool hasLastDispenseResult = false;
float lastDisplayedGainMl = 0.0f;

void stopPump(const PumpChannel &pump) {
  digitalWrite(pump.in1Gpio, LOW);
  digitalWrite(pump.in2Gpio, LOW);
}

void stopAllPumps() {
  stopPump(PUMP_1);
  stopPump(PUMP_2);
  stopPump(PUMP_3);
}

void runPumpForward(const PumpChannel &pump) {
  digitalWrite(pump.in1Gpio, LOW);
  digitalWrite(pump.in2Gpio, HIGH);
}

void setupPump(const PumpChannel &pump) {
  pinMode(pump.in1Gpio, OUTPUT);
  pinMode(pump.in2Gpio, OUTPUT);
  stopPump(pump);
}

bool waitForHx711Ready(int doutGpio, unsigned long timeoutMs) {
  const unsigned long startMs = millis();
  while (millis() - startMs < timeoutMs) {
    if (digitalRead(doutGpio) == LOW) {
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

bool readHx711Raw(const LoadCellChannel &cell, long &rawValue) {
  if (!waitForHx711Ready(cell.doutGpio, HX711_READY_TIMEOUT_MS)) {
    return false;
  }

  uint32_t raw24 = 0;

  noInterrupts();
  for (uint8_t bitIndex = 0; bitIndex < 24; ++bitIndex) {
    digitalWrite(HX711_SCK_GPIO, HIGH);
    delayMicroseconds(1);
    raw24 <<= 1;
    if (digitalRead(cell.doutGpio) == HIGH) {
      raw24 |= 1;
    }
    digitalWrite(HX711_SCK_GPIO, LOW);
    delayMicroseconds(1);
  }

  // One extra pulse keeps the HX711 on channel A, gain 128.
  digitalWrite(HX711_SCK_GPIO, HIGH);
  delayMicroseconds(1);
  digitalWrite(HX711_SCK_GPIO, LOW);
  interrupts();

  rawValue = signExtendHx711(raw24);
  return true;
}

bool readAverageGrams(const LoadCellChannel &cell,
                      size_t sampleCount,
                      float &grams) {
  double sum = 0.0;

  for (size_t sample = 0; sample < sampleCount; ++sample) {
    long rawValue = 0;
    if (!readHx711Raw(cell, rawValue)) {
      return false;
    }

    sum += static_cast<float>(rawValue - cell.tareOffset) /
           cell.calibrationFactor;
    delay(HX711_SAMPLE_DELAY_MS);
    yield();
  }

  grams = static_cast<float>(sum / sampleCount);
  return true;
}

bool readAverageRaw(const LoadCellChannel &cell,
                    size_t sampleCount,
                    long &averageRaw) {
  double sum = 0.0;

  for (size_t sample = 0; sample < sampleCount; ++sample) {
    long rawValue = 0;
    if (!readHx711Raw(cell, rawValue)) {
      return false;
    }

    sum += rawValue;
    delay(HX711_SAMPLE_DELAY_MS);
    yield();
  }

  const double average = sum / sampleCount;
  averageRaw = static_cast<long>(average + (average >= 0.0 ? 0.5 : -0.5));
  return true;
}

bool tareMixingCell() {
  stopAllPumps();
  Serial.println();
  Serial.println("Taring LCA3. Leave the empty receiving cup on the scale.");
  delay(1000);

  double rawSum = 0.0;
  for (size_t sample = 0; sample < TARE_SAMPLES; ++sample) {
    long rawValue = 0;
    if (!readHx711Raw(mixingLoadCell, rawValue)) {
      Serial.println("Tare failed: LCA3/HX711 not ready. Check 3.3V, GND, DAT3 GPIO27, SCLK GPIO23.");
      return false;
    }
    rawSum += rawValue;
    delay(HX711_SAMPLE_DELAY_MS);
    yield();
  }

  const double averageRaw = rawSum / TARE_SAMPLES;
  mixingLoadCell.tareOffset =
    static_cast<long>(averageRaw + (averageRaw >= 0.0 ? 0.5 : -0.5));

  Serial.print("LCA3 tare offset: ");
  Serial.println(mixingLoadCell.tareOffset);
  return true;
}

void printCalibrationFactor() {
  Serial.print("Active LCA3 calibration factor: ");
  Serial.println(mixingLoadCell.calibrationFactor, 2);
}

void setCalibrationFactor(float newFactor) {
  if (fabsf(newFactor) < 1.0f) {
    Serial.println("Calibration factor rejected because it is too close to zero.");
    return;
  }

  mixingLoadCell.calibrationFactor = newFactor;
  Serial.print("New active LCA3 calibration factor: ");
  Serial.println(mixingLoadCell.calibrationFactor, 2);
  Serial.println("If this works, copy this value into MIXING_LOAD_CELL_TEMPLATE.");
}

void calibrateFromLastActualVolume(float actualMl) {
  if (!hasLastDispenseResult || lastDisplayedGainMl <= 0.0f) {
    Serial.println("No previous dispense result to scale from. Run DISPENSE first.");
    return;
  }

  if (actualMl <= 0.0f) {
    Serial.println("Use a positive measured volume. Example: CALFROM 19.5");
    return;
  }

  const float oldFactor = mixingLoadCell.calibrationFactor;
  const float newFactor = oldFactor * (lastDisplayedGainMl / actualMl);

  Serial.println();
  Serial.println("=== CALIBRATION FROM MEASURED WATER ===");
  Serial.print("Last displayed LCA3 gain: ");
  Serial.print(lastDisplayedGainMl, 2);
  Serial.println(" g ~= mL");
  Serial.print("Measured physical volume: ");
  Serial.print(actualMl, 2);
  Serial.println(" mL");
  Serial.print("Old factor: ");
  Serial.println(oldFactor, 2);
  setCalibrationFactor(newFactor);
}

void calibrateWithKnownWeight(float knownGrams) {
  if (knownGrams <= 0.0f) {
    Serial.println("Use a positive known weight. Example: CAL 33.4");
    return;
  }

  Serial.println();
  Serial.println("=== LCA3 KNOWN-WEIGHT CALIBRATION ===");
  Serial.println("Step 1: leave the empty receiving cup on LCA3.");
  if (!tareMixingCell()) {
    Serial.println("Calibration aborted during tare.");
    return;
  }

  Serial.print("Step 2: place ");
  Serial.print(knownGrams, 2);
  Serial.println(" g into the cup now.");
  Serial.println("Reading will start in 7 seconds...");
  delay(7000);

  long loadedRaw = 0;
  if (!readAverageRaw(mixingLoadCell, TARE_SAMPLES, loadedRaw)) {
    Serial.println("Calibration failed: LCA3/HX711 not ready after weight was placed.");
    return;
  }

  const long rawDelta = loadedRaw - mixingLoadCell.tareOffset;
  if (rawDelta == 0) {
    Serial.println("Calibration failed: raw reading did not change.");
    return;
  }

  const float oldFactor = mixingLoadCell.calibrationFactor;
  const float newFactor = static_cast<float>(rawDelta) / knownGrams;

  Serial.print("Loaded raw average: ");
  Serial.println(loadedRaw);
  Serial.print("Raw delta: ");
  Serial.println(rawDelta);
  Serial.print("Old factor: ");
  Serial.println(oldFactor, 2);
  setCalibrationFactor(newFactor);
}

bool readSettledMixingGrams(float &grams) {
  delay(SCALE_SETTLE_MS);
  if (!readAverageGrams(mixingLoadCell, READ_AVERAGE_SAMPLES, grams)) {
    Serial.println("Read failed: LCA3/HX711 not ready.");
    return false;
  }
  return true;
}

float mixingGainFromReading(float grams) {
  return fmaxf(0.0f, grams);
}

bool printPostTareZeroCheck() {
  float grams = 0.0f;
  if (!readSettledMixingGrams(grams)) {
    return false;
  }

  Serial.print("After tare LCA3 settled reading: ");
  Serial.print(grams, 2);
  Serial.println(" g");
  Serial.print("After tare control gain: ");
  Serial.print(mixingGainFromReading(grams), 2);
  Serial.println(" g");
  return true;
}

unsigned long burstForRemaining(float remainingGrams) {
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

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();

  if (command == "STOP" || command == "S") {
    stopRequested = true;
    return true;
  }

  Serial.print("Command ignored while dispensing: ");
  Serial.println(command);
  Serial.println("Type STOP to abort the active dispense.");
  return false;
}

bool dispensePumpToMixingTarget(const PumpChannel &pump,
                                float targetMixingGrams,
                                const char *label) {
  unsigned long totalPumpOnMs = 0;
  float currentGrams = 0.0f;

  if (!readSettledMixingGrams(currentGrams)) {
    stopAllPumps();
    return false;
  }

  float currentGain = mixingGainFromReading(currentGrams);

  Serial.println();
  Serial.print("Dispensing with ");
  Serial.println(pump.name);
  Serial.print("Target LCA3 gain: ");
  Serial.print(targetMixingGrams, 2);
  Serial.println(" g");
  Serial.print("Pump-on timeout: ");
  Serial.print(MAX_PUMP_ON_TIME_MS);
  Serial.println(" ms");

  while (currentGain < targetMixingGrams - TARGET_TOLERANCE_G) {
    if (totalPumpOnMs > MAX_PUMP_ON_TIME_MS) {
      stopAllPumps();
      Serial.println("Abort: pump-on timeout reached before target mass.");
      Serial.println("This usually means the load cell is under-reading, tubing is not dripping into the cup, or the pump/tube is blocked.");
      return false;
    }

    if (stopRequestedFromSerial()) {
      stopAllPumps();
      Serial.println("Abort: STOP command received.");
      return false;
    }

    const float remainingGrams = targetMixingGrams - currentGain;
    const unsigned long burstMs = burstForRemaining(remainingGrams);

    Serial.print(label);
    Serial.print(" burst ");
    Serial.print(burstMs);
    Serial.print(" ms | remaining approx ");
    Serial.print(remainingGrams, 2);
    Serial.println(" g");

    runPumpForward(pump);
    delay(burstMs);
    stopPump(pump);
    totalPumpOnMs += burstMs;

    Serial.println("Pump stopped. Waiting for drips/scale to settle...");
    if (!readSettledMixingGrams(currentGrams)) {
      stopAllPumps();
      return false;
    }

    currentGain = mixingGainFromReading(currentGrams);

    Serial.print("LCA3 reading: ");
    Serial.print(currentGrams, 2);
    Serial.print(" g | gain used for control: ");
    Serial.print(currentGain, 2);
    Serial.print(" g / target ");
    Serial.print(targetMixingGrams, 2);
    Serial.println(" g");
  }

  stopPump(pump);
  Serial.print(label);
  Serial.println(" target reached.");
  return true;
}

void runSingleDispense(uint8_t pumpNumber, float targetMl) {
  if (targetMl <= 0.0f) {
    Serial.println("Use a positive volume. Example: DISPENSE 1 10");
    return;
  }

  const PumpChannel *pump = nullptr;
  const char *label = nullptr;
  if (pumpNumber == 1) {
    pump = &PUMP_1;
    label = "PUMP1";
  } else if (pumpNumber == 2) {
    pump = &PUMP_2;
    label = "PUMP2";
  } else {
    Serial.println("Use Pump 1 or Pump 2 only. Example: DISPENSE 1 10");
    return;
  }

  stopRequested = false;

  Serial.println();
  Serial.println("=== FADEX WATER WEIGHT DISPENSE ===");
  Serial.print("Requested volume: ");
  Serial.print(targetMl, 2);
  Serial.println(" mL water");
  Serial.println("Control rule: 1.00 mL water ~= 1.00 g on LCA3.");

  if (!tareMixingCell()) {
    Serial.println("Dispense aborted during tare.");
    return;
  }
  if (!printPostTareZeroCheck()) {
    Serial.println("Dispense aborted: could not verify LCA3 after tare.");
    return;
  }

  const float targetGrams = targetMl * WATER_GRAMS_PER_ML;
  const bool ok = dispensePumpToMixingTarget(*pump, targetGrams, label);

  stopAllPumps();

  float finalGrams = 0.0f;
  if (readSettledMixingGrams(finalGrams)) {
    const float finalGain = mixingGainFromReading(finalGrams);
    hasLastDispenseResult = true;
    lastDisplayedGainMl = finalGain;
    const float errorMl = finalGain - targetMl;
    Serial.println();
    Serial.println("=== RESULT ===");
    Serial.print("Target: ");
    Serial.print(targetMl, 2);
    Serial.println(" mL");
    Serial.print("Final LCA3 gain: ");
    Serial.print(finalGain, 2);
    Serial.println(" g ~= mL");
    Serial.print("Error: ");
    Serial.print(errorMl, 2);
    Serial.println(" mL");
  }

  Serial.println(ok ? "Dispense finished." : "Dispense stopped early.");
}

void runMixDispense(float pump1Ml, float pump2Ml) {
  if (pump1Ml < 0.0f || pump2Ml < 0.0f || pump1Ml + pump2Ml <= 0.0f) {
    Serial.println("Use non-negative volumes with a positive total. Example: MIX 10 20");
    return;
  }

  stopRequested = false;

  Serial.println();
  Serial.println("=== FADEX TWO-PUMP WATER DISPENSE ===");
  Serial.print("Pump 1 requested: ");
  Serial.print(pump1Ml, 2);
  Serial.println(" mL");
  Serial.print("Pump 2 requested: ");
  Serial.print(pump2Ml, 2);
  Serial.println(" mL");
  Serial.println("Pump 1 runs first, then Pump 2. Only one pump runs at a time.");

  if (!tareMixingCell()) {
    Serial.println("Dispense aborted during tare.");
    return;
  }
  if (!printPostTareZeroCheck()) {
    Serial.println("Dispense aborted: could not verify LCA3 after tare.");
    return;
  }

  bool ok = true;
  if (pump1Ml > 0.0f) {
    ok = dispensePumpToMixingTarget(
      PUMP_1,
      pump1Ml * WATER_GRAMS_PER_ML,
      "PUMP1"
    );
  }

  if (ok && pump2Ml > 0.0f) {
    ok = dispensePumpToMixingTarget(
      PUMP_2,
      (pump1Ml + pump2Ml) * WATER_GRAMS_PER_ML,
      "PUMP2"
    );
  }

  stopAllPumps();

  float finalGrams = 0.0f;
  if (readSettledMixingGrams(finalGrams)) {
    const float finalGain = mixingGainFromReading(finalGrams);
    hasLastDispenseResult = true;
    lastDisplayedGainMl = finalGain;
    const float targetMl = pump1Ml + pump2Ml;
    Serial.println();
    Serial.println("=== RESULT ===");
    Serial.print("Total target: ");
    Serial.print(targetMl, 2);
    Serial.println(" mL");
    Serial.print("Final LCA3 gain: ");
    Serial.print(finalGain, 2);
    Serial.println(" g ~= mL");
    Serial.print("Total error: ");
    Serial.print(finalGain - targetMl, 2);
    Serial.println(" mL");
  }

  Serial.println(ok ? "Two-pump dispense finished." : "Two-pump dispense stopped early.");
}

void runPrime(uint8_t pumpNumber, unsigned long runMs) {
  const PumpChannel *pump = nullptr;
  if (pumpNumber == 1) {
    pump = &PUMP_1;
  } else if (pumpNumber == 2) {
    pump = &PUMP_2;
  } else {
    Serial.println("Use Pump 1 or Pump 2 only. Example: PRIME 1 3000");
    return;
  }

  Serial.print("Priming ");
  Serial.print(pump->name);
  Serial.print(" for ");
  Serial.print(runMs);
  Serial.println(" ms.");

  runPumpForward(*pump);
  delay(runMs);
  stopPump(*pump);
  Serial.println("Prime complete.");
}

void printHelp() {
  Serial.println();
  Serial.println("FadeX water-by-weight dispense commands:");
  Serial.println("  DISPENSE 1 10   -> tare, then Pump 1 dispenses 10 mL water into LCA3 cup");
  Serial.println("  DISPENSE 2 10   -> tare, then Pump 2 dispenses 10 mL water into LCA3 cup");
  Serial.println("  MIX 10 20       -> tare, Pump 1 adds 10 mL, Pump 2 adds 20 mL");
  Serial.println("  PRIME 1 3000    -> run Pump 1 for 3000 ms without using load cell");
  Serial.println("  PRIME 2 3000    -> run Pump 2 for 3000 ms without using load cell");
  Serial.println("  READ            -> print settled LCA3 reading");
  Serial.println("  TARE            -> tare LCA3 with cup in place");
  Serial.println("  CAL 33.4        -> calibrate LCA3 with a 33.4 g known weight");
  Serial.println("  CALFROM 19.5    -> after a run, rescale LCA3 using actual measured mL");
  Serial.println("  SETCAL 1439     -> manually set active LCA3 calibration factor");
  Serial.println("  CALFACTOR       -> print active LCA3 calibration factor");
  Serial.println("  STOP            -> abort active dispense / stop pumps");
  Serial.println("  HELP            -> print this menu");
  Serial.println();
  Serial.println("Physical setup reminder:");
  Serial.println("  Put the empty receiving cup on LCA3 before DISPENSE/MIX.");
  Serial.println("  Make sure tubing drips into the cup without touching the cup or scale.");
  Serial.println("  Prime tubes first if they are empty.");
}

void handleCommand(String command) {
  command.trim();
  if (command.isEmpty()) {
    return;
  }

  String upper = command;
  upper.toUpperCase();

  if (upper == "HELP" || upper == "?") {
    printHelp();
  } else if (upper == "STOP") {
    stopRequested = true;
    stopAllPumps();
    Serial.println("All pumps stopped.");
  } else if (upper == "TARE") {
    tareMixingCell();
  } else if (upper == "CALFACTOR") {
    printCalibrationFactor();
  } else if (upper == "READ") {
    float grams = 0.0f;
    if (readSettledMixingGrams(grams)) {
      Serial.print("LCA3 settled reading: ");
      Serial.print(grams, 2);
      Serial.println(" g");
    }
  } else if (upper.startsWith("DISPENSE ")) {
    int pumpNumber = 0;
    float targetMl = 0.0f;
    if (sscanf(command.c_str(), "%*s %d %f", &pumpNumber, &targetMl) == 2) {
      runSingleDispense(static_cast<uint8_t>(pumpNumber), targetMl);
    } else {
      Serial.println("Use: DISPENSE <pump 1-2> <mL>");
    }
  } else if (upper.startsWith("MIX ")) {
    float pump1Ml = 0.0f;
    float pump2Ml = 0.0f;
    if (sscanf(command.c_str(), "%*s %f %f", &pump1Ml, &pump2Ml) == 2) {
      runMixDispense(pump1Ml, pump2Ml);
    } else {
      Serial.println("Use: MIX <pump1 mL> <pump2 mL>");
    }
  } else if (upper.startsWith("CALFROM ")) {
    calibrateFromLastActualVolume(command.substring(8).toFloat());
  } else if (upper.startsWith("SETCAL ")) {
    setCalibrationFactor(command.substring(7).toFloat());
  } else if (upper.startsWith("CAL ")) {
    calibrateWithKnownWeight(command.substring(4).toFloat());
  } else if (upper.startsWith("PRIME ")) {
    int pumpNumber = 0;
    unsigned long runMs = 0;
    if (sscanf(command.c_str(), "%*s %d %lu", &pumpNumber, &runMs) == 2) {
      runPrime(static_cast<uint8_t>(pumpNumber), runMs);
    } else {
      Serial.println("Use: PRIME <pump 1-2> <milliseconds>");
    }
  } else {
    Serial.println("Unknown command. Type HELP.");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(500);

  setupPump(PUMP_1);
  setupPump(PUMP_2);
  setupPump(PUMP_3);

  pinMode(HX711_SCK_GPIO, OUTPUT);
  digitalWrite(HX711_SCK_GPIO, LOW);
  pinMode(mixingLoadCell.doutGpio, INPUT);

  Serial.println();
  Serial.println("FadeX water-by-weight dispense test ready.");
  Serial.println("Pump pins:");
  Serial.println("  Pump 1: IN1 GPIO22, IN2 GPIO21");
  Serial.println("  Pump 2: IN1 GPIO19, IN2 GPIO18");
  Serial.println("Load cell:");
  Serial.println("  LCA3 mixing reservoir: DAT3 GPIO27, shared SCLK GPIO23");
  Serial.println("  Calibration factor: 2951.40");
  Serial.println("Pump direction: forward = IN1 LOW, IN2 HIGH");
  printHelp();
}

void loop() {
  if (Serial.available() > 0) {
    handleCommand(Serial.readStringUntil('\n'));
  }
  delay(10);
}
