#include <Arduino.h>

// FadeX PCB pump-driver bring-up test.
//
// This matches the new homebase PCB schematic:
//   Pump 1 connector J7 -> 3190.1.IN1 / GPIO22, 3190.1.IN2 / GPIO21
//   Pump 2 connector J4 -> 3190.2.IN1 / GPIO19, 3190.2.IN2 / GPIO18
//   Pump 3 connector J9 -> 3190.3.IN1 / GPIO33, 3190.3.IN2 / GPIO4
//
// Safety choice:
// Pumps do NOT auto-run on boot. Use Serial Monitor commands only.

struct PumpChannel {
  int in1Gpio;
  int in2Gpio;
  const char *name;
  const char *connector;
  const char *in1Net;
  const char *in2Net;
  int in1Esp32ModulePin;
  int in2Esp32ModulePin;
};

constexpr PumpChannel PUMP1 = {
  22, 21, "Pump 1 / Nicotine", "J7", "3190.1.IN1", "3190.1.IN2", 36, 33
};
constexpr PumpChannel PUMP2 = {
  19, 18, "Pump 2 / Diluent", "J4", "3190.2.IN1", "3190.2.IN2", 31, 30
};
constexpr PumpChannel PUMP3 = {
  33, 4, "Pump 3 / Mixing", "J9", "3190.3.IN1", "3190.3.IN2", 9, 26
};

constexpr unsigned long DEFAULT_RUN_MS = 2000;

void stopPump(const PumpChannel &pump) {
  digitalWrite(pump.in1Gpio, LOW);
  digitalWrite(pump.in2Gpio, LOW);
}

void stopAllPumps() {
  stopPump(PUMP1);
  stopPump(PUMP2);
  stopPump(PUMP3);
}

void driveForward(const PumpChannel &pump) {
  // The assembled pump polarity is flipped, so the useful "forward" direction
  // is IN1 low and IN2 high.
  digitalWrite(pump.in1Gpio, LOW);
  digitalWrite(pump.in2Gpio, HIGH);
}

void driveReverse(const PumpChannel &pump) {
  digitalWrite(pump.in1Gpio, HIGH);
  digitalWrite(pump.in2Gpio, LOW);
}

void runForMs(const PumpChannel &pump, bool forward, unsigned long runMs) {
  Serial.print(pump.name);
  Serial.print(forward ? " forward " : " reverse ");
  Serial.print(runMs);
  Serial.println(" ms");

  if (forward) {
    driveForward(pump);
  } else {
    driveReverse(pump);
  }

  delay(runMs);
  stopPump(pump);
  Serial.print(pump.name);
  Serial.println(" stopped");
}

void setupPump(const PumpChannel &pump) {
  pinMode(pump.in1Gpio, OUTPUT);
  pinMode(pump.in2Gpio, OUTPUT);
  stopPump(pump);
}

const PumpChannel *pumpFromNumber(char number) {
  switch (number) {
    case '1': return &PUMP1;
    case '2': return &PUMP2;
    case '3': return &PUMP3;
    default: return nullptr;
  }
}

void printHelp() {
  Serial.println();
  Serial.println("FadeX PCB pump test commands:");
  Serial.println("  1f, 2f, 3f       -> run pump forward until stopped");
  Serial.println("  1r, 2r, 3r       -> run pump reverse until stopped");
  Serial.println("  1s, 2s, 3s       -> stop one pump");
  Serial.println("  run 1 f 2.0      -> run pump 1 forward for 2.0 seconds");
  Serial.println("  run 2 r 1.5      -> run pump 2 reverse for 1.5 seconds");
  Serial.println("  stop             -> stop all pumps");
  Serial.println("  help             -> print this menu");
  Serial.println();
}

void printPumpMap(const PumpChannel &pump) {
  Serial.print("  ");
  Serial.print(pump.name);
  Serial.print(" on ");
  Serial.print(pump.connector);
  Serial.print(": ");
  Serial.print(pump.in1Net);
  Serial.print(" -> GPIO");
  Serial.print(pump.in1Gpio);
  Serial.print(" / ESP32 pin ");
  Serial.print(pump.in1Esp32ModulePin);
  Serial.print(", ");
  Serial.print(pump.in2Net);
  Serial.print(" -> GPIO");
  Serial.print(pump.in2Gpio);
  Serial.print(" / ESP32 pin ");
  Serial.println(pump.in2Esp32ModulePin);
}

void handleRunCommand(const String &command) {
  int firstSpace = command.indexOf(' ');
  int secondSpace = command.indexOf(' ', firstSpace + 1);
  int thirdSpace = command.indexOf(' ', secondSpace + 1);

  if (firstSpace < 0 || secondSpace < 0 || thirdSpace < 0) {
    Serial.println("Use: run <pump 1-3> <f/r> <seconds>");
    return;
  }

  const char pumpNumber = command.charAt(firstSpace + 1);
  const char direction = static_cast<char>(tolower(command.charAt(secondSpace + 1)));
  const float seconds = command.substring(thirdSpace + 1).toFloat();
  const PumpChannel *pump = pumpFromNumber(pumpNumber);

  if (pump == nullptr || (direction != 'f' && direction != 'r') || seconds <= 0.0f) {
    Serial.println("Use: run <pump 1-3> <f/r> <seconds>");
    return;
  }

  runForMs(*pump, direction == 'f', static_cast<unsigned long>(seconds * 1000.0f));
}

void handleCommand(String command) {
  command.trim();
  command.toLowerCase();

  if (command == "help") {
    printHelp();
    return;
  }

  if (command == "stop") {
    stopAllPumps();
    Serial.println("All pumps stopped.");
    return;
  }

  if (command.startsWith("run ")) {
    handleRunCommand(command);
    return;
  }

  if (command.length() == 2) {
    const PumpChannel *pump = pumpFromNumber(command.charAt(0));
    const char action = command.charAt(1);

    if (pump != nullptr && action == 'f') {
      driveForward(*pump);
      Serial.print(pump->name);
      Serial.println(" forward. Type stop or 1s/2s/3s.");
      return;
    }

    if (pump != nullptr && action == 'r') {
      driveReverse(*pump);
      Serial.print(pump->name);
      Serial.println(" reverse. Type stop or 1s/2s/3s.");
      return;
    }

    if (pump != nullptr && action == 's') {
      stopPump(*pump);
      Serial.print(pump->name);
      Serial.println(" stopped.");
      return;
    }
  }

  if (command.length() > 0) {
    Serial.println("Unknown command. Type help.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  setupPump(PUMP1);
  setupPump(PUMP2);
  setupPump(PUMP3);

  Serial.println();
  Serial.println("FadeX PCB pump-driver test");
  Serial.println("PCB schematic pin map:");
  printPumpMap(PUMP1);
  printPumpMap(PUMP2);
  printPumpMap(PUMP3);
  Serial.println("Pump polarity: forward is flipped in software to IN1 LOW / IN2 HIGH.");
  Serial.println("No pump will run until you send a command.");
  printHelp();
}

void loop() {
  if (Serial.available() > 0) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  delay(10);
}
