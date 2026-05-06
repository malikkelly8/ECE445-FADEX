#include <Arduino.h>
#include "HX711.h"

// FadeX water-by-weight dosing test.
//
// For water, 1 mL is approximately 1 g. This sketch tares a cup on a selected
// load cell, runs a selected pump in short bursts, smooths the load-cell
// reading with a moving average, and stops when the measured weight reaches the
// requested volume.
//
// Important vibration strategy:
// The pump is always OFF when the load cell is measured. The sketch runs one
// burst, stops the pump, waits for the cup/scale to settle, then reads weight.
//
// Simple command:
//   dose 20
// Full command:
//   dose <pump 1-3> <load cell 1-4> <mL>
// Example:
//   dose 2 1 20

struct PumpChannel {
  int in1Gpio;
  int in2Gpio;
  const char *name;
  bool polarityFlipped;
};

struct LoadCellChannel {
  HX711 *scale;
  int doutGpio;
  float calibrationFactor;
  const char *name;
  const char *connector;
};

constexpr PumpChannel PUMPS[] = {
  // The assembled pump polarity is flipped, so software "forward" uses
  // IN1 low and IN2 high for every pump.
  {22, 21, "Pump 1 / Nicotine", true},
  {19, 18, "Pump 2 / Diluent or Water", true},
  {33, 4, "Pump 3 / Mixing", true}
};

constexpr int HX711_SCK_GPIO = 23;
constexpr float LOADCELL_CAL_FACTORS[] = {
  3053.60f,
  3109.60f,
  2951.40f,
  2925.40f
};

HX711 loadCell1;
HX711 loadCell2;
HX711 loadCell3;
HX711 loadCell4;

LoadCellChannel LOAD_CELLS[] = {
  {&loadCell1, 25, LOADCELL_CAL_FACTORS[0], "LCA1", "J2"},
  {&loadCell2, 26, LOADCELL_CAL_FACTORS[1], "LCA2", "J10"},
  {&loadCell3, 27, LOADCELL_CAL_FACTORS[2], "LCA3", "J13"},
  {&loadCell4, 13, LOADCELL_CAL_FACTORS[3], "LCA4", "J5"}
};

constexpr uint8_t DEFAULT_PUMP_NUMBER = 2;
constexpr uint8_t DEFAULT_LOAD_CELL_NUMBER = 1;
constexpr float WATER_GRAMS_PER_ML = 1.0f;

constexpr size_t MOVING_AVERAGE_SAMPLES = 10;

// Long tubing needs a stronger "coarse fill" phase. The code still pumps in
// bursts because the load cells are only trusted while every pump is stopped.
constexpr float FAST_ZONE_G = 10.0f;
constexpr float MEDIUM_ZONE_G = 4.0f;
constexpr float SLOW_ZONE_G = 1.0f;
constexpr unsigned long FAST_BURST_MS = 2400;
constexpr unsigned long MEDIUM_BURST_MS = 850;
constexpr unsigned long SLOW_BURST_MS = 220;
constexpr unsigned long TRIM_BURST_MS = 70;
constexpr unsigned long SCALE_SETTLE_MS = 650;
constexpr unsigned long MAX_DOSE_TIME_MS = 90000;
constexpr float NEGATIVE_WEIGHT_ABORT_G = -0.50f;
constexpr size_t SAFETY_AVERAGE_SAMPLES = 5;

float averageBuffer[MOVING_AVERAGE_SAMPLES] = {};
size_t averageIndex = 0;
size_t averageCount = 0;

PumpChannel *pumpFromNumber(uint8_t pumpNumber) {
  if (pumpNumber < 1 || pumpNumber > 3) {
    return nullptr;
  }
  return const_cast<PumpChannel *>(&PUMPS[pumpNumber - 1]);
}

LoadCellChannel *loadCellFromNumber(uint8_t loadCellNumber) {
  if (loadCellNumber < 1 || loadCellNumber > 4) {
    return nullptr;
  }
  return &LOAD_CELLS[loadCellNumber - 1];
}

void stopPump(const PumpChannel &pump) {
  digitalWrite(pump.in1Gpio, LOW);
  digitalWrite(pump.in2Gpio, LOW);
}

void stopAllPumps() {
  for (const PumpChannel &pump : PUMPS) {
    stopPump(pump);
  }
}

void runPumpForward(const PumpChannel &pump) {
  if (pump.polarityFlipped) {
    digitalWrite(pump.in1Gpio, LOW);
    digitalWrite(pump.in2Gpio, HIGH);
    return;
  }

  digitalWrite(pump.in1Gpio, HIGH);
  digitalWrite(pump.in2Gpio, LOW);
}

void setupPump(const PumpChannel &pump) {
  pinMode(pump.in1Gpio, OUTPUT);
  pinMode(pump.in2Gpio, OUTPUT);
  stopPump(pump);
}

bool waitForReady(HX711 &scale, unsigned long timeoutMs) {
  const unsigned long startMs = millis();
  while (!scale.is_ready() && (millis() - startMs < timeoutMs)) {
    delay(10);
  }
  return scale.is_ready();
}

bool tareLoadCell(LoadCellChannel &cell) {
  stopAllPumps();
  Serial.print("Taring ");
  Serial.print(cell.name);
  Serial.print(" on ");
  Serial.print(cell.connector);
  Serial.println(". Leave the empty cup in place and keep the pumps off.");
  delay(1500);

  if (!waitForReady(*cell.scale, 3000)) {
    Serial.println("Load cell not ready. Check 3.3 V, GND, DAT, and SCLK.");
    return false;
  }

  cell.scale->set_scale();
  cell.scale->tare(20);
  cell.scale->set_scale(cell.calibrationFactor);
  delay(SCALE_SETTLE_MS);
  Serial.println("Tare complete.");
  return true;
}

bool tareAllLoadCells() {
  Serial.println();
  Serial.println("Pre-dose safety tare: leave every cup/container in place.");
  stopAllPumps();

  for (LoadCellChannel &cell : LOAD_CELLS) {
    if (!tareLoadCell(cell)) {
      Serial.print("Could not tare ");
      Serial.print(cell.name);
      Serial.println(". Dose will not run.");
      return false;
    }
  }

  Serial.println("All load cells tared. Safety interlock is armed.");
  return true;
}

float readGrams(LoadCellChannel &cell) {
  if (!cell.scale->is_ready()) {
    return NAN;
  }
  return cell.scale->get_units(1);
}

float readAverageGrams(LoadCellChannel &cell, size_t sampleCount) {
  float sum = 0.0f;
  size_t validSamples = 0;

  for (size_t i = 0; i < sampleCount; ++i) {
    const float reading = readGrams(cell);
    if (!isnan(reading)) {
      sum += reading;
      validSamples++;
    }
    delay(40);
  }

  if (validSamples == 0) {
    return NAN;
  }
  return sum / validSamples;
}

float pushMovingAverage(float newValue) {
  averageBuffer[averageIndex] = newValue;
  averageIndex = (averageIndex + 1) % MOVING_AVERAGE_SAMPLES;
  if (averageCount < MOVING_AVERAGE_SAMPLES) {
    averageCount++;
  }

  float sum = 0.0f;
  for (size_t i = 0; i < averageCount; ++i) {
    sum += averageBuffer[i];
  }
  return sum / averageCount;
}

float seedMovingAverage(LoadCellChannel &cell) {
  Serial.print("Filling ");
  Serial.print(MOVING_AVERAGE_SAMPLES);
  Serial.println("-sample moving average...");

  averageIndex = 0;
  averageCount = 0;
  float lastReading = 0.0f;

  for (size_t i = 0; i < MOVING_AVERAGE_SAMPLES; ++i) {
    lastReading = readGrams(cell);
    if (isnan(lastReading)) {
      lastReading = 0.0f;
    }
    pushMovingAverage(lastReading);
    delay(80);
  }

  return pushMovingAverage(lastReading);
}

float readSettledMovingAverage(LoadCellChannel &cell) {
  // Only call this while all pumps are off. That keeps motor vibration out of
  // the load-cell measurement as much as possible.
  delay(SCALE_SETTLE_MS);
  return seedMovingAverage(cell);
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

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toLowerCase();

  return command == "stop" || command == "s";
}

bool emergencyStopIfAnyLoadCellNegative() {
  stopAllPumps();
  delay(SCALE_SETTLE_MS);

  for (LoadCellChannel &cell : LOAD_CELLS) {
    const float reading = readAverageGrams(cell, SAFETY_AVERAGE_SAMPLES);

    Serial.print("Safety ");
    Serial.print(cell.name);
    Serial.print(": ");
    if (isnan(reading)) {
      Serial.println("not ready");
      Serial.println("EMERGENCY STOP: load-cell safety reading failed.");
      stopAllPumps();
      return true;
    }

    Serial.print(reading, 2);
    Serial.println(" g");

    if (reading < NEGATIVE_WEIGHT_ABORT_G) {
      Serial.println();
      Serial.println("EMERGENCY STOP: negative load-cell weight detected.");
      Serial.print(cell.name);
      Serial.print(" measured ");
      Serial.print(reading, 2);
      Serial.print(" g, below threshold ");
      Serial.print(NEGATIVE_WEIGHT_ABORT_G, 2);
      Serial.println(" g.");
      stopAllPumps();
      return true;
    }
  }

  return false;
}

void doseByWeight(uint8_t pumpNumber, uint8_t loadCellNumber, float targetMl) {
  PumpChannel *pump = pumpFromNumber(pumpNumber);
  LoadCellChannel *cell = loadCellFromNumber(loadCellNumber);

  if (pump == nullptr || cell == nullptr || targetMl <= 0.0f) {
    Serial.println("Use: dose <pump 1-3> <load cell 1-4> <mL>");
    Serial.println("Example: dose 2 1 20");
    return;
  }

  const float targetGrams = targetMl * WATER_GRAMS_PER_ML;

  Serial.println();
  Serial.println("--- Weight-based water dose ---");
  Serial.print("Pump: ");
  Serial.println(pump->name);
  Serial.print("Scale: ");
  Serial.print(cell->name);
  Serial.print(" on ");
  Serial.println(cell->connector);
  Serial.print("Target: ");
  Serial.print(targetMl, 2);
  Serial.print(" mL water ~= ");
  Serial.print(targetGrams, 2);
  Serial.println(" g");

  Serial.println("Pre-dose tare will happen now before any pump starts.");
  if (!tareAllLoadCells()) {
    return;
  }

  if (emergencyStopIfAnyLoadCellNegative()) {
    return;
  }

  float averagedGrams = readSettledMovingAverage(*cell);
  const unsigned long startMs = millis();

  while (averagedGrams < targetGrams) {
    const unsigned long now = millis();
    const float remainingGrams = targetGrams - averagedGrams;

    if (now - startMs > MAX_DOSE_TIME_MS) {
      stopAllPumps();
      Serial.println("Dose aborted: safety timeout reached.");
      return;
    }

    if (stopRequestedFromSerial()) {
      stopAllPumps();
      Serial.println("Dose aborted by STOP command.");
      return;
    }

    const unsigned long burstMs = burstDurationForRemaining(remainingGrams);

    Serial.print("Pump burst: ");
    Serial.print(burstMs);
    Serial.println(" ms");

    runPumpForward(*pump);
    delay(burstMs);
    stopPump(*pump);

    if (emergencyStopIfAnyLoadCellNegative()) {
      return;
    }

    averagedGrams = readSettledMovingAverage(*cell);

    Serial.print("Current avg: ");
    Serial.print(averagedGrams, 2);
    Serial.print(" g | target: ");
    Serial.print(targetGrams, 2);
    Serial.print(" g | remaining: ");
    Serial.print(max(0.0f, targetGrams - averagedGrams), 2);
    Serial.println(" g");
  }

  stopAllPumps();

  const float finalGrams = readSettledMovingAverage(*cell);
  Serial.println("--- Dose complete ---");
  Serial.print("Requested: ");
  Serial.print(targetMl, 2);
  Serial.println(" mL");
  Serial.print("Final moving-average weight: ");
  Serial.print(finalGrams, 2);
  Serial.println(" g");
  Serial.print("Estimated delivered volume: ");
  Serial.print(finalGrams / WATER_GRAMS_PER_ML, 2);
  Serial.println(" mL");
}

void printHelp() {
  Serial.println();
  Serial.println("FadeX weight-based water dosing test");
  Serial.println("Commands:");
  Serial.println("  dose 20          -> Pump 2 into LCA1 until 20 g / 20 mL");
  Serial.println("  dose 2 1 20      -> Pump 2, LCA1, target 20 mL");
  Serial.println("                    Pump is off during every weight reading");
  Serial.println("                    Pump polarity is flipped in software");
  Serial.println("  tare 1           -> tare LCA1 with empty cup in place");
  Serial.println("  read 1           -> print one moving-average reading from LCA1");
  Serial.println("  stop             -> abort an active dose");
  Serial.println("  help             -> print this menu");
  Serial.println();
}

void handleTareCommand(const String &command) {
  int cellNumber = 0;
  if (sscanf(command.c_str(), "tare %d", &cellNumber) != 1) {
    Serial.println("Use: tare <load cell 1-4>");
    return;
  }

  LoadCellChannel *cell = loadCellFromNumber(static_cast<uint8_t>(cellNumber));
  if (cell == nullptr) {
    Serial.println("Load cell must be 1 through 4.");
    return;
  }

  tareLoadCell(*cell);
}

void handleReadCommand(const String &command) {
  int cellNumber = 0;
  if (sscanf(command.c_str(), "read %d", &cellNumber) != 1) {
    Serial.println("Use: read <load cell 1-4>");
    return;
  }

  LoadCellChannel *cell = loadCellFromNumber(static_cast<uint8_t>(cellNumber));
  if (cell == nullptr) {
    Serial.println("Load cell must be 1 through 4.");
    return;
  }

  const float averagedGrams = seedMovingAverage(*cell);
  Serial.print(cell->name);
  Serial.print(" moving-average weight: ");
  Serial.print(averagedGrams, 2);
  Serial.println(" g");
}

void handleDoseCommand(const String &command) {
  char action[12] = {};
  float first = 0.0f;
  float second = 0.0f;
  float third = 0.0f;
  const int count = sscanf(command.c_str(), "%11s %f %f %f", action, &first, &second, &third);

  if (count == 2) {
    doseByWeight(DEFAULT_PUMP_NUMBER, DEFAULT_LOAD_CELL_NUMBER, first);
    return;
  }

  if (count == 4) {
    doseByWeight(static_cast<uint8_t>(first), static_cast<uint8_t>(second), third);
    return;
  }

  Serial.println("Use: dose <mL> or dose <pump 1-3> <load cell 1-4> <mL>");
}

void handleCommand(String command) {
  command.trim();
  command.toLowerCase();

  if (command == "help") {
    printHelp();
  } else if (command.startsWith("dose ")) {
    handleDoseCommand(command);
  } else if (command.startsWith("tare ")) {
    handleTareCommand(command);
  } else if (command.startsWith("read ")) {
    handleReadCommand(command);
  } else if (command == "stop" || command == "s") {
    stopAllPumps();
    Serial.println("All pumps stopped.");
  } else if (command.length() > 0) {
    Serial.println("Unknown command. Type help.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  for (const PumpChannel &pump : PUMPS) {
    setupPump(pump);
  }

  for (LoadCellChannel &cell : LOAD_CELLS) {
    cell.scale->begin(cell.doutGpio, HX711_SCK_GPIO);
    cell.scale->set_scale(cell.calibrationFactor);

    Serial.print(cell.name);
    Serial.print(" ");
    Serial.print(cell.connector);
    Serial.print(" DOUT GPIO");
    Serial.print(cell.doutGpio);
    Serial.print(" | ");
    Serial.println(cell.scale->is_ready() ? "ready" : "not ready");
  }

  Serial.println();
  Serial.println("Pump PCB pins:");
  Serial.println("  Pump 1: IN1 GPIO22, IN2 GPIO21");
  Serial.println("  Pump 2: IN1 GPIO19, IN2 GPIO18");
  Serial.println("  Pump 3: IN1 GPIO33, IN2 GPIO4");
  Serial.println("Pump polarity: flipped in software for all pumps");
  Serial.println("Burst timing: faster coarse bursts for 8-12 inch tubing");
  Serial.println("HX711 shared SCLK: GPIO23");

  printHelp();
}

void loop() {
  if (Serial.available() > 0) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  delay(20);
}
