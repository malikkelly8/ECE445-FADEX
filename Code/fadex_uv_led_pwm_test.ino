#include <Arduino.h>

// FadeX homebase UV LED PWM bring-up test
//
// 
// The current PCB routes UVLEDPWM to ESP32 GPIO39, but GPIO39 is input-only and
// cannot generate the PWM signal needed by the AL8860 CTRL pin.
//
// Rework assumed by this sketch:
// We should add a jumper wire from ESP32 GPIO32 to the UVLEDPWM net or directly to the
//   AL8860 CTRL pin.
// leave the original GPIO39 connection alone; it will not drive the net.
//
// What this verifies:
// 1. The ESP32 can generate PWM on the jumper wire.
// 2. The PWM duty cycle changes cleanly over time.
// 3. The UV LED driver responds to the control signal.
//
// 
// - Probe GPIO32 to confirm the ESP32 is generating a clean PWM waveform.
// - Probe the CTRL net as well if you want to see what the driver pin sees.
//

constexpr int UV_LED_PWM_PIN = 32;
constexpr uint8_t PWM_CHANNEL = 0;
constexpr uint8_t PWM_RESOLUTION_BITS = 10;
constexpr uint32_t PWM_FREQUENCY_HZ = 200;
constexpr unsigned long STEP_HOLD_MS = 2000;

// These steps make it easy to verify on the scope that the duty cycle really
// changes over time.
constexpr uint8_t DUTY_STEP_COUNT = 6;
const uint8_t DUTY_STEPS_PERCENT[DUTY_STEP_COUNT] = {0, 10, 25, 50, 75, 100};

unsigned long lastStepMs = 0;
uint8_t dutyIndex = 0;
bool sweepEnabled = true;

uint32_t percentToDutyCount(uint8_t percent) {
  const uint32_t maxDuty = (1u << PWM_RESOLUTION_BITS) - 1u;
  return (static_cast<uint32_t>(percent) * maxDuty) / 100u;
}

void applyDutyPercent(uint8_t percent) {
  const uint32_t dutyCount = percentToDutyCount(percent);
  ledcWrite(PWM_CHANNEL, dutyCount);

  Serial.print("PWM duty set to ");
  Serial.print(percent);
  Serial.print("% (count ");
  Serial.print(dutyCount);
  Serial.println(")");
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  sweep      - resume automatic duty-cycle sweep");
  Serial.println("  off        - force 0% duty");
  Serial.println("  on         - force 100% duty");
  Serial.println("  duty <n>   - set duty cycle to n percent (0 to 100)");
  Serial.println("  help       - print this help text");
}

void handleSerial() {
  if (!Serial.available()) {
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();

  if (command.equalsIgnoreCase("sweep")) {
    sweepEnabled = true;
    Serial.println("Automatic PWM sweep enabled.");
    return;
  }

  if (command.equalsIgnoreCase("off")) {
    sweepEnabled = false;
    applyDutyPercent(0);
    return;
  }

  if (command.equalsIgnoreCase("on")) {
    sweepEnabled = false;
    applyDutyPercent(100);
    return;
  }

  if (command.startsWith("duty ")) {
    const int percent = command.substring(5).toInt();
    if (percent < 0 || percent > 100) {
      Serial.println("Duty must be between 0 and 100.");
      return;
    }

    sweepEnabled = false;
    applyDutyPercent(static_cast<uint8_t>(percent));
    return;
  }

  if (command.equalsIgnoreCase("help")) {
    printHelp();
    return;
  }

  Serial.println("Unknown command.");
  printHelp();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  ledcSetup(PWM_CHANNEL, PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS);
  ledcAttachPin(UV_LED_PWM_PIN, PWM_CHANNEL);

  Serial.println();
  Serial.println("FadeX UV LED PWM test");
  Serial.println("Rework assumption:");
  Serial.println("  GPIO32 is jumpered to UVLEDPWM / AL8860 CTRL.");
  Serial.println("PWM configuration:");
  Serial.print("  GPIO: ");
  Serial.println(UV_LED_PWM_PIN);
  Serial.print("  Frequency (Hz): ");
  Serial.println(PWM_FREQUENCY_HZ);
  Serial.print("  Resolution (bits): ");
  Serial.println(PWM_RESOLUTION_BITS);
  Serial.println("Behavior:");
  Serial.println("  The sketch sweeps through several duty cycles every 2 seconds.");
  Serial.println("  Use the oscilloscope to verify a clean PWM waveform on GPIO32.");
  Serial.println("  If the CTRL node looks rounded, the input capacitor is filtering it.");
  Serial.println();
  printHelp();

  applyDutyPercent(DUTY_STEPS_PERCENT[dutyIndex]);
  lastStepMs = millis();
}

void loop() {
  handleSerial();

  if (!sweepEnabled) {
    delay(20);
    return;
  }

  const unsigned long now = millis();
  if (now - lastStepMs < STEP_HOLD_MS) {
    delay(20);
    return;
  }

  dutyIndex = (dutyIndex + 1) % DUTY_STEP_COUNT;
  applyDutyPercent(DUTY_STEPS_PERCENT[dutyIndex]);
  lastStepMs = now;
}
