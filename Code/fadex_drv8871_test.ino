#include <Arduino.h>

// DRV8871 three-pump bring-up sketch for the FadeX PCB.
//
// This sketch verifies that each driver can:
// 1. run a motor/pump in one direction,
// 2. reverse polarity,
// 3. stop cleanly.
//
// PCB schematic map:
//   Pump 1 connector J7 -> 3190.1.IN1 / GPIO22, 3190.1.IN2 / GPIO21
//   Pump 2 connector J4 -> 3190.2.IN1 / GPIO19, 3190.2.IN2 / GPIO18
//   Pump 3 connector J9 -> 3190.3.IN1 / GPIO33, 3190.3.IN2 / GPIO4

struct MotorChannel {
  int in1;
  int in2;
  const char *name;
};

constexpr MotorChannel MOTOR1 = {22, 21, "Pump 1 / Nicotine"};
constexpr MotorChannel MOTOR2 = {19, 18, "Pump 2 / Diluent"};
constexpr MotorChannel MOTOR3 = {33, 4, "Pump 3 / Mixing"};

constexpr unsigned long RUN_MS = 2000;
constexpr unsigned long PAUSE_MS = 1000;
constexpr bool AUTO_DEMO_ON_BOOT = false;

void stopMotor(const MotorChannel &motor) {
  digitalWrite(motor.in1, LOW);
  digitalWrite(motor.in2, LOW);
}

void stopAllMotors() {
  stopMotor(MOTOR1);
  stopMotor(MOTOR2);
  stopMotor(MOTOR3);
}

void driveForward(const MotorChannel &motor) {
  // The assembled pump polarity is flipped, so the useful "forward" direction
  // is IN1 low and IN2 high.
  digitalWrite(motor.in1, LOW);
  digitalWrite(motor.in2, HIGH);
}

void driveReverse(const MotorChannel &motor) {
  digitalWrite(motor.in1, HIGH);
  digitalWrite(motor.in2, LOW);
}

void runDirection(const MotorChannel &motor, bool forward, unsigned long runMs) {
  Serial.print(motor.name);
  Serial.print(forward ? " forward for " : " reverse for ");
  Serial.print(runMs);
  Serial.println(" ms");

  if (forward) {
    driveForward(motor);
  } else {
    driveReverse(motor);
  }

  delay(runMs);
  stopMotor(motor);
}

void demoMotor(const MotorChannel &motor) {
  runDirection(motor, true, RUN_MS);
  delay(PAUSE_MS);
  runDirection(motor, false, RUN_MS);
  delay(PAUSE_MS);
}

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  demo    -> run all three pumps forward then reverse");
  Serial.println("  1f      -> Pump 1 forward");
  Serial.println("  1r      -> Pump 1 reverse");
  Serial.println("  1s      -> Pump 1 stop");
  Serial.println("  2f      -> Pump 2 forward");
  Serial.println("  2r      -> Pump 2 reverse");
  Serial.println("  2s      -> Pump 2 stop");
  Serial.println("  3f      -> Pump 3 forward");
  Serial.println("  3r      -> Pump 3 reverse");
  Serial.println("  3s      -> Pump 3 stop");
  Serial.println("  stop    -> stop all pumps");
  Serial.println("  help    -> print commands again");
}

void runAutoDemo() {
  Serial.println();
  Serial.println("Running automatic DRV8871 demo...");
  demoMotor(MOTOR1);
  demoMotor(MOTOR2);
  demoMotor(MOTOR3);
  Serial.println("Automatic demo complete.");
}

void setupMotor(const MotorChannel &motor) {
  pinMode(motor.in1, OUTPUT);
  pinMode(motor.in2, OUTPUT);
  stopMotor(motor);
}

void handleCommand(const String &cmd) {
  if (cmd == "demo") {
    runAutoDemo();
  } else if (cmd == "1f") {
    driveForward(MOTOR1);
  } else if (cmd == "1r") {
    driveReverse(MOTOR1);
  } else if (cmd == "1s") {
    stopMotor(MOTOR1);
  } else if (cmd == "2f") {
    driveForward(MOTOR2);
  } else if (cmd == "2r") {
    driveReverse(MOTOR2);
  } else if (cmd == "2s") {
    stopMotor(MOTOR2);
  } else if (cmd == "3f") {
    driveForward(MOTOR3);
  } else if (cmd == "3r") {
    driveReverse(MOTOR3);
  } else if (cmd == "3s") {
    stopMotor(MOTOR3);
  } else if (cmd == "stop") {
    stopAllMotors();
  } else if (cmd == "help") {
    printHelp();
  } else if (cmd.length() > 0) {
    Serial.println("Unknown command. Type 'help' for the list.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  setupMotor(MOTOR1);
  setupMotor(MOTOR2);
  setupMotor(MOTOR3);

  Serial.println();
  Serial.println("FadeX DRV8871 / pump hardware test");
  Serial.println("PCB pin map:");
  Serial.println("  Pump 1 J7: 3190.1.IN1 GPIO22, 3190.1.IN2 GPIO21");
  Serial.println("  Pump 2 J4: 3190.2.IN1 GPIO19, 3190.2.IN2 GPIO18");
  Serial.println("  Pump 3 J9: 3190.3.IN1 GPIO33, 3190.3.IN2 GPIO4");
  Serial.println("Verify each pump runs in one direction, then reverses.");
  Serial.println("Pump polarity is flipped in software: forward = IN1 LOW / IN2 HIGH.");
  Serial.println("Auto-demo is OFF for PCB safety. Type demo to run the full sequence.");
  printHelp();

  if (AUTO_DEMO_ON_BOOT) {
    delay(1500);
    runAutoDemo();
  }
}

void loop() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    handleCommand(cmd);
  }
}
