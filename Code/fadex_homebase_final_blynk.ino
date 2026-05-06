#define BLYNK_TEMPLATE_ID "TMPL2y04fShpL"
#define BLYNK_TEMPLATE_NAME "FadeX Homebase"
#define BLYNK_AUTH_TOKEN "-UXdaY64X3PrJmzBSJ-fNi3Phx6ySBni"

#define BLYNK_PRINT Serial

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include "esp_wpa2.h"
#include <math.h>
#include <string.h>

// Set to 1 for the final Wi-Fi + Blynk demo. Set to 0 only if the Blynk
// library is not installed and you need a Serial-Monitor-only fallback build.
#define FADEX_ENABLE_BLYNK_LIBRARY 1

#if FADEX_ENABLE_BLYNK_LIBRARY
#include <BlynkSimpleEsp32.h>
#else
#define V0 0
#define V1 1
#define V2 2
#define V3 3
#define V4 4
#define V5 5
#define V6 6
#define V7 7
#define V8 8
#define V9 9
#define V10 10
#define V11 11
#define V12 12
#define V13 13
#define V14 14
#define V15 15
#define V16 16
#define V17 17
#define V18 18
#define V19 19
#define V20 20
#define V21 21
#define V22 22
#define V23 23
#define V24 24
#define V25 25
#define V26 26
#define V27 27
#define V28 28

class FadeXDummyBlynk {
public:
  void run() {}
  bool connected() { return false; }
  void begin(const char *, const char *, const char *) {}
  void config(const char *) {}
  bool connect(unsigned long = 0) { return false; }

  template <typename... Args>
  void virtualWrite(Args...) {}

  template <typename... Args>
  void syncVirtual(Args...) {}

  template <typename... Args>
  void logEvent(Args...) {}
};

class BlynkTimer {
public:
  template <typename Callback>
  void setInterval(unsigned long, Callback) {}

  void run() {}
};

class WidgetTerminal {
public:
  explicit WidgetTerminal(int) {}
  void clear() {}
  void flush() {}
  void println() {}

  template <typename T>
  void println(const T &) {}
};

FadeXDummyBlynk Blynk;
#endif

// Set this to true for eduroam / university WPA2-Enterprise Wi-Fi.
// Set it to false if you go back to a normal hotspot or home router.
constexpr bool USE_ENTERPRISE_WIFI = false;

char ssid[] = "#212";
char pass[] = "graduate";

// WPA2-Enterprise credentials. Do not share your real password in chat.
// Some schools want "NetID@school.edu"; others want just "NetID".
char enterpriseIdentity[] = "mkelly61@illinois.edu";
char enterpriseUsername[] = "mkelly61@illinois.edu";
char enterprisePassword[] = "Pandabear2004!";

// Final demo mode: the homebase connects directly to Blynk through Wi-Fi,
// while the handheld sends puff sessions to the homebase over ESP-NOW.
constexpr bool USE_BLYNK_WIFI = true;
constexpr bool USE_BLYNK_USB_BRIDGE = false;
constexpr bool SERIAL_CHATBOT_ONLY = !USE_BLYNK_WIFI && !USE_BLYNK_USB_BRIDGE;

// IllinoisNet was observed on channel 11. When Blynk Wi-Fi is enabled, the
// homebase uses the AP channel automatically, so the handheld must match.
constexpr uint8_t ESPNOW_WIFI_CHANNEL = 11;

// PCB handheld/vape ESP32 STA MAC:
//   88:13:BF:55:35:F0
constexpr uint8_t EXPECTED_HANDHELD_MAC[6] = {
  0x88, 0x13, 0xBF, 0x55, 0x35, 0xF0
};

// the homebase receives the complete session packet that was collected
// on the handheld device
struct SessionData {
  uint32_t totalPuffs;     // number of puffs in the synced session
  float totalDuration;     // total puff duration in seconds
  float surveyScore;       // user self-report score
  uint8_t triggerCode;     // what the user said triggered the craving/use
  uint32_t sessionId;      // unique session ID
};

// short response packet sent back to the handheld for user feedback
struct SyncAck {
  bool accepted;
  uint32_t sessionId;
  float adaptiveMultiplier;
  float appliedConcentration;
  char scenarioId[4];
};

// one pump driver channel is represented by the two logic inputs that control
// the DRV8701 driver
struct PumpChannel {
  int in1;
  int in2;
  const char *name;
};

// each load-cell amplifier uses its own data line but shares one clock line
// with the other HX711 amplifiers. Because SCLK is shared, the final firmware
// reads the HX711 boards together instead of using one HX711 object at a time.
struct LoadCellChannel {
  int dataPin;
  float calibrationFactor;
  const char *name;
  const char *role;
  long tareOffset;
};

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

struct UncsMeasurement {
  bool valid;
  float sampleVoltage;
  float transmittance;
  float absorbance;
  float concentrationMm;
};

// values mirrored to the Blynk dashboard
struct DashboardState {
  float currentConcentrationPct;
  float nextConcentrationPct;
  uint32_t dailyPuffCount;
  float dailyDurationSec;
  float surveyScore;
  uint8_t triggerCode;
  uint8_t scenarioCode;
  bool relapseRiskHigh;
  float nicotineVolumeMl;
  float diluentVolumeMl;
  float moneySavedUsd;
  uint8_t milestoneProgressPct;
  uint8_t quitPlanWeeks;
  uint8_t currentTreatmentWeek;
  float plannedWeeklyStepPct;
};

// A calculated dose can wait here until the Blynk app tells the homebase to
// physically run the pumps. This makes demos safer because calculation and
// dispensing are separate actions.
struct PendingDose {
  bool ready;
  uint32_t sessionId;
  float appliedConcentrationPct;
  float nicotineVolumeMl;
  float diluentVolumeMl;
  unsigned long nicotineRunMs;
  unsigned long diluentRunMs;
};

enum GuidedTerminalState : uint8_t {
  TERMINAL_IDLE = 0,
  TERMINAL_WAITING_PLAN_DECISION,
  TERMINAL_WAITING_SURVEY,
  TERMINAL_WAITING_TRIGGER,
  TERMINAL_WAITING_PUFFS,
  TERMINAL_WAITING_RUN
};

// Final PCB pump-driver wiring:
// Pump 1 (nicotine) -> IN1 GPIO22, IN2 GPIO21
// Pump 2 (diluent)  -> IN1 GPIO19, IN2 GPIO18
// Pump 3 (mixing)   -> IN1 GPIO33, IN2 GPIO4
constexpr PumpChannel NICOTINE_PUMP = {22, 21, "Nicotine"};
constexpr PumpChannel DILUENT_PUMP = {19, 18, "Diluent"};
constexpr PumpChannel MIXING_PUMP = {33, 4, "Mixing"};

// Final PCB load-cell wiring:
// Shared HX711 clock -> GPIO23
// Data lines -> GPIO25, GPIO26, GPIO27, GPIO13
constexpr int HX711_SCK_PIN = 23;
constexpr int HX711_DATA1_PIN = 25;
constexpr int HX711_DATA2_PIN = 26;
constexpr int HX711_DATA3_PIN = 27;
constexpr int HX711_DATA4_PIN = 13;

// UNCS optical sensor wiring:
// PDO is the photodiode/TIA output into ESP32 ADC1_CH0.
// UVLEDPWM needs an output-capable pin. The PCB net was originally tied to
// GPIO39, which is input-only, so the demo assumes the jumper to GPIO32.
constexpr int UNCS_PHOTODIODE_ADC_PIN = 36;
constexpr int UNCS_UV_LED_PWM_PIN = 32;
constexpr uint8_t UNCS_PWM_CHANNEL = 0;
constexpr uint8_t UNCS_PWM_RESOLUTION_BITS = 10;
constexpr uint32_t UNCS_PWM_FREQUENCY_HZ = 200;
constexpr uint8_t UNCS_DEFAULT_LED_DUTY_PERCENT = 100;
constexpr size_t UNCS_ADC_SAMPLE_COUNT = 32;
constexpr unsigned long UNCS_LED_SETTLE_MS = 800;
constexpr unsigned long UNCS_ADC_PRINT_PERIOD_MS = 250;
constexpr unsigned long UNCS_AUTO_SHUTOFF_MS = 10000;
constexpr unsigned long UNCS_CIRC_DEFAULT_MS = 60000;
constexpr unsigned long UNCS_CIRC_MAX_MS = 120000;
constexpr float UNCS_DEFAULT_DARK_VOLTAGE = 0.142f;
constexpr float UNCS_MIN_LIGHT_WINDOW_V = 0.050f;
constexpr float UNCS_ADC_SATURATION_WARNING_V = 3.000f;
constexpr float UNCS_SURROGATE_MAX_MM = 10.0f;
constexpr float UNCS_TARGET_TOLERANCE_MM = 0.25f;
constexpr float UNCS_LARGE_ERROR_MM = 1.0f;
constexpr unsigned long UNCS_LARGE_PULSE_MS = 500;
constexpr unsigned long UNCS_SMALL_PULSE_MS = 100;
constexpr unsigned long UNCS_FLUSH_DELAY_MS = 3000;
constexpr uint8_t UNCS_MAX_ADJUSTMENT_STEPS = 30;
constexpr float MIXING_CHAMBER_MAX_SAFE_G = 100.0f;
constexpr float CALIB_A = 19.7487f;
constexpr float CALIB_B = 2.0964f;

// calibration and algorithm constants
constexpr float STARTING_CONCENTRATION_PERCENT = 5.0f;
constexpr float NIC_ML_PER_SEC = 0.98f;
constexpr float DIL_ML_PER_SEC = 0.98f;
constexpr float MAX_CONCENTRATION_PERCENT = 5.0f;
constexpr float MIN_CONCENTRATION_PERCENT = 0.5f;
constexpr float TOTAL_VOLUME_ML = 30.0f;
constexpr float TAPER_STEP_PERCENT = 0.28f;
constexpr float BASELINE_DAILY_COST_USD = 12.0f;
constexpr float BASELINE_DAILY_USAGE_SECONDS = 450.0f;
constexpr float MAX_USAGE_FACTOR_FOR_SAVINGS = 3.0f;
constexpr uint8_t DEFAULT_QUIT_PLAN_WEEKS = 12;
constexpr uint8_t MIN_QUIT_PLAN_WEEKS = 2;
constexpr uint8_t MAX_QUIT_PLAN_WEEKS = 24;

// stage 4 hardware timing helpers
// Set this false if you want to test only the Blynk/app flow. For the integrated
// homebase demo, it is true so the DRV8701 pump pins actually drive.
constexpr bool PUMPS_ENABLED = true;
// Pump 3 stays disabled for this demo. Pump 1 and Pump 2 feed the mixing
// reservoir, and LCA3 decides when each liquid target has been reached.
constexpr unsigned long MIXING_RUN_MS = 0;
constexpr unsigned long PRIME_RUN_MS = 3000;

// Calibrated with the 33.4 g reference mass on the PCB load-cell channels.
constexpr float LOADCELL1_CAL_FACTOR = 3053.60f;
constexpr float LOADCELL2_CAL_FACTOR = 3109.60f;
constexpr float LOADCELL3_CAL_FACTOR = 2951.40f;
constexpr float LOADCELL4_CAL_FACTOR = 2925.40f;

// LCA1/LCA2/LCA3 are used for the final demo:
// LCA1 = nicotine reservoir, LCA2 = diluent reservoir, LCA3 = mixing reservoir.
// LCA4 stays wired in the pin map, but it is not required for dose control.
constexpr uint8_t NIC_SOURCE_CELL_INDEX = 0;
constexpr uint8_t DIL_SOURCE_CELL_INDEX = 1;
constexpr uint8_t MIXING_CELL_INDEX = 2;
constexpr uint8_t MONITORED_CELL_COUNT = 3;
constexpr float WATER_GRAMS_PER_ML = 1.0f;
constexpr size_t MOVING_AVERAGE_SAMPLES = 4;
constexpr size_t TARE_SAMPLES = 20;
constexpr size_t TARE_VERIFY_SAMPLES = 8;
constexpr float TARE_ZERO_TOLERANCE_G = 0.05f;
constexpr unsigned long SCALE_SETTLE_MS = 350;
constexpr unsigned long HX711_SAMPLE_DELAY_MS = 25;
constexpr unsigned long HX711_READY_TIMEOUT_MS = 3000;
constexpr unsigned long MAX_SINGLE_PUMP_TIME_MS = 90000;

// Burst timing fills quickly while far from the target, then trims in shorter
// pulses so the scale has time to settle before the final dose mass.
constexpr float FAST_ZONE_G = 10.0f;
constexpr float MEDIUM_ZONE_G = 4.0f;
constexpr float SLOW_ZONE_G = 1.0f;
constexpr unsigned long FAST_BURST_MS = 1800;
constexpr unsigned long MEDIUM_BURST_MS = 650;
constexpr unsigned long SLOW_BURST_MS = 180;
constexpr unsigned long TRIM_BURST_MS = 45;
constexpr float MIXING_TARGET_TOLERANCE_G = 0.25f;

// Final delivery check: use the mixing reservoir because source loss was noisy
// in hardware testing. Final demo target must be within +/-0.5 g.
constexpr float DELIVERY_TOLERANCE_G = 0.5f;

// cloud / app timing helpers
constexpr unsigned long BLYNK_PUBLISH_INTERVAL_MS = 5000;

const char *const TRIGGER_LABELS[] = {
  "None", "Stress", "Social", "Habit",
  "Withdrawal", "Meals", "Driving", "Other"
};

const float SURVEY_VALUES[] = {0.5f, 0.0f, -0.5f, -1.0f};
const char *const SURVEY_LABELS[] = {
  "Better",
  "Neutral",
  "Struggling",
  "Worse"
};

// 4 usage tiers x 4 survey tiers = 16 named scenarios
const char *const SCENARIO_IDS[4][4] = {
  {"G1", "G2", "G3", "G4"},
  {"G5", "G6", "G7", "G8"},
  {"S1", "S2", "S3", "S4"},
  {"S5", "S6", "S7", "S8"}
};

// the homebase keeps track of the most recently applied concentration
float currentConcentration = STARTING_CONCENTRATION_PERCENT;
float cumulativeMoneySavedUsd = 0.0f;
uint8_t quitPlanWeeks = DEFAULT_QUIT_PLAN_WEEKS;
uint8_t currentTreatmentWeek = 1;

// one clinician override concentration can be pushed from the app
float clinicianOverridePct = -1.0f;
bool primeRequested = false;
bool doseStartRequested = false;
bool pumpStopRequested = false;
PendingDose pendingDose = {};

// App-only demo session. This lets Blynk act like the handheld by collecting
// puff count, puff duration, survey score, and trigger code directly.
SessionData appSession = {};
uint32_t nextAppSessionId = 100000;
bool appPuffActive = false;
unsigned long appPuffStartMs = 0;
GuidedTerminalState guidedTerminalState = TERMINAL_IDLE;

// UNCS stores the most recent dark, blank, and sample voltages so the demo can
// run one step at a time from the Serial Monitor or FadeX Chatbot.
float uncsDarkVoltage = UNCS_DEFAULT_DARK_VOLTAGE;
float uncsBlankVoltage = NAN;
float uncsSampleVoltage = NAN;
float uncsLastConcentrationMm = NAN;
uint8_t uncsLedDutyPercent = UNCS_DEFAULT_LED_DUTY_PERCENT;
uint8_t uncsActiveDutyPercent = 0;
bool uncsAdcStreaming = false;
unsigned long uncsLedTurnOnMs = 0;
unsigned long lastUncsAdcPrintMs = 0;

// used for event logging so the app does not get spammed
bool relapseEventLatched = false;
uint8_t lastMilestoneBucket = 0;

DashboardState dashboard = {
  STARTING_CONCENTRATION_PERCENT,
  STARTING_CONCENTRATION_PERCENT,
  0,
  0.0f,
  0.0f,
  0,
  1,
  false,
  TOTAL_VOLUME_ML,
  0.0f,
  0.0f,
  0,
  DEFAULT_QUIT_PLAN_WEEKS,
  1,
  (STARTING_CONCENTRATION_PERCENT - MIN_CONCENTRATION_PERCENT) /
    (DEFAULT_QUIT_PLAN_WEEKS - 1)
};

// the radio callback should stay short, so new work is dropped into this buffer
// and processed later in the main loop
volatile bool pendingSessionReady = false;
SessionData pendingSession = {};
uint8_t pendingSenderMac[6] = {};
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

BlynkTimer timer;
WidgetTerminal fadeXChatbot(V22);

LoadCellChannel loadCells[] = {
  {HX711_DATA1_PIN, LOADCELL1_CAL_FACTOR, "LCA1", "Nic source reservoir", 0},
  {HX711_DATA2_PIN, LOADCELL2_CAL_FACTOR, "LCA2", "Diluent source reservoir", 0},
  {HX711_DATA3_PIN, LOADCELL3_CAL_FACTOR, "LCA3", "Mixing reservoir", 0},
  {HX711_DATA4_PIN, LOADCELL4_CAL_FACTOR, "LCA4", "Spare / optional", 0}
};

void publishTelemetryToBlynk();
void publishTelemetryToBridge();
void handleSerialCommands();
void processPendingPrimeRequest();
void processPendingDoseStartRequest();
void printLoadCellReadings();
bool tareLoadCells(bool requireZeroVerification = false);
void resetAppSession();
void clearAppSessionScratchpad();
void processAppSessionAndRunDose();
void processSession(const SessionData &data, const uint8_t *senderMac);
void stageHandheldSessionForSurvey(const SessionData &data, const uint8_t *senderMac);
void setAppSurveyScore(float surveyScore);
void setAppTriggerCode(int triggerCode);
float snapSurveyScore(float surveyScore);
const char *surveyLabel(float surveyScore);
const char *triggerLabel(uint8_t triggerCode);
float computePlannedWeeklyStep(float currentConcentrationPct);
void resetQuitPlan(uint8_t requestedWeeks);
void logToBlynkTerminal(const String &line);
void clearBlynkTerminal();
void executeConsoleCommand(String command, bool fromBlynkTerminal);
bool isStopCommand(const String &commandUpper);
bool stopRequestedFromSerial();
String appSessionStatusLine();
void promptGuidedTerminalState();
void beginGuidedTerminalSession();
void beginPreDoseSurveyFlow();
bool parseSurveyText(const String &commandUpper, float &surveyScore);
bool parseTriggerText(const String &commandUpper, int &triggerCode);
const char *guidedStateLabel(GuidedTerminalState state);
void showChatbotWelcome();
void setupUncsSensor();
void executeUncsCommand(String command, bool fromBlynkTerminal);
void setUncsLed(bool enabled);
void publishUncsTelemetry();
void runPump(const PumpChannel &pump, unsigned long runMs);
void stopPump(const PumpChannel &pump);
void stopAllPumps();
void runPumpForward(const PumpChannel &pump);
WeightSnapshot readWeightSnapshot();
float scaleNicotinePercentToSurrogateMm(float nicotinePercent);
float calculateConcentration(float absorbance);
float calculateUncsAbsorbance(float darkVoltage,
                              float blankVoltage,
                              float sampleVoltage);
float calculateUncsConcentrationMm(float darkVoltage,
                                   float blankVoltage,
                                   float sampleVoltage);
UncsMeasurement readUncsMeasurement();
bool runUncsSurrogateControl(float targetMm,
                             const String &targetLabel,
                             bool fromBlynkTerminal);
float readUncsPhotodiodeVoltage();
bool runWeightControlledDose(float nicotineMl, float diluentMl);
void runMixingMassBalanceTest(float nicMl, float dilMl);
void runSinglePumpMassBalanceTest(uint8_t pumpNumber, float targetMl);

// keep Blynk responsive while long pump runs are happening
void serviceCloud() {
  if (USE_BLYNK_WIFI) {
    Blynk.run();
  }
  timer.run();
}

void printMacAddress(const uint8_t *mac) {
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

String bridgeEscaped(String text) {
  text.replace("\\", "\\\\");
  text.replace("\r", "");
  text.replace("\n", "\\n");
  return text;
}

void bridgeSetPin(const char *pin, const String &value) {
  if (!USE_BLYNK_USB_BRIDGE) {
    return;
  }

  Serial.print("BRIDGE_SET ");
  Serial.print(pin);
  Serial.print(" ");
  Serial.println(bridgeEscaped(value));
}

void clearBlynkTerminal() {
  if (SERIAL_CHATBOT_ONLY) {
    Serial.println();
    Serial.println("--------------------------------------------------");
    return;
  }

  bridgeSetPin("v23", "");

  if (!Blynk.connected()) {
    return;
  }

  fadeXChatbot.clear();
  fadeXChatbot.flush();
  Blynk.virtualWrite(V22, "clr");
}

void renderBlynkTerminalText(const String &text) {
  if (SERIAL_CHATBOT_ONLY) {
    Serial.println();
    Serial.println("FadeX Chatbot:");
    Serial.println(text);
    Serial.println("--------------------------------------------------");
    return;
  }

  bridgeSetPin("v23", text);

  if (!Blynk.connected()) {
    return;
  }

  Blynk.virtualWrite(V23, text);
  fadeXChatbot.println();

  int start = 0;
  while (start <= text.length()) {
    const int newlineIndex = text.indexOf('\n', start);
    if (newlineIndex < 0) {
      fadeXChatbot.println(text.substring(start));
      break;
    }

    fadeXChatbot.println(text.substring(start, newlineIndex));
    start = newlineIndex + 1;
  }

  fadeXChatbot.flush();
}

void logToBlynkTerminal(const String &line) {
  renderBlynkTerminalText(line);
}

void showChatbotWelcome() {
  renderBlynkTerminalText(
    "Welcome to FadeX Chatbot.\n"
    "I can help check in before your next dose.\n"
    "Type START to begin, or STATUS to see the current session.\n" +
    appSessionStatusLine()
  );
}

String appSessionStatusLine() {
  String line =
    String("Current status | conc=") + String(currentConcentration, 2) +
    "% | week " + String(static_cast<int>(currentTreatmentWeek)) +
    "/" + String(static_cast<int>(quitPlanWeeks)) +
    " | puffs=" + String(appSession.totalPuffs) +
    " | duration=" + String(appSession.totalDuration, 1) + " s" +
    " | survey=" + String(surveyLabel(appSession.surveyScore)) +
    " | trigger=" + String(triggerLabel(appSession.triggerCode));

  if (pendingDose.ready) {
    line += " | dose ready";
  }

  return line;
}

bool appSessionHasPuffMetrics() {
  return appSession.totalPuffs > 0 || appSession.totalDuration >= 0.05f;
}

bool isWeekOneBaselineFlow() {
  return currentTreatmentWeek <= 1;
}

void promptForPuffMetricsBeforeDose() {
  guidedTerminalState = TERMINAL_WAITING_PUFFS;
  const String prompt =
    isWeekOneBaselineFlow()
      ? "Week 1 baseline: record puff metrics first.\n"
        "Use the Puff button or type PUFF <seconds>.\n"
        "Then I will ask check-in questions to calculate the week 2 dose.\n"
      : "Record puff metrics first.\n"
        "Use the Puff button or type PUFF <seconds>.\n"
        "After usage is recorded, I will ask the check-in questions.\n";

  renderBlynkTerminalText(prompt + appSessionStatusLine());
}

void calculateAndRunAppDoseFromGuidedFlow() {
  if (!appSessionHasPuffMetrics()) {
    promptForPuffMetricsBeforeDose();
    return;
  }

  guidedTerminalState = TERMINAL_IDLE;
  renderBlynkTerminalText(
    "Check-in complete. Calculating and dispensing the dose now.\n" +
    appSessionStatusLine()
  );
  processAppSessionAndRunDose();
}

bool parseSurveyText(const String &commandUpper, float &surveyScore) {
  if (commandUpper.indexOf("BETTER") >= 0) {
    surveyScore = 0.5f;
    return true;
  }
  if (commandUpper.indexOf("NEUTRAL") >= 0) {
    surveyScore = 0.0f;
    return true;
  }
  if (commandUpper.indexOf("STRUGGL") >= 0) {
    surveyScore = -0.5f;
    return true;
  }
  if (commandUpper.indexOf("WORSE") >= 0) {
    surveyScore = -1.0f;
    return true;
  }
  return false;
}

bool parseTriggerText(const String &commandUpper, int &triggerCode) {
  if (commandUpper.indexOf("NONE") >= 0) {
    triggerCode = 0;
    return true;
  }
  if (commandUpper.indexOf("STRESS") >= 0) {
    triggerCode = 1;
    return true;
  }
  if (commandUpper.indexOf("SOCIAL") >= 0) {
    triggerCode = 2;
    return true;
  }
  if (commandUpper.indexOf("HABIT") >= 0) {
    triggerCode = 3;
    return true;
  }
  if (commandUpper.indexOf("WITHDRAW") >= 0) {
    triggerCode = 4;
    return true;
  }
  if (commandUpper.indexOf("MEAL") >= 0) {
    triggerCode = 5;
    return true;
  }
  if (commandUpper.indexOf("DRIV") >= 0) {
    triggerCode = 6;
    return true;
  }
  if (commandUpper.indexOf("OTHER") >= 0) {
    triggerCode = 7;
    return true;
  }
  return false;
}

const char *guidedStateLabel(GuidedTerminalState state) {
  switch (state) {
    case TERMINAL_WAITING_PLAN_DECISION: return "waiting_plan";
    case TERMINAL_WAITING_SURVEY: return "waiting_survey";
    case TERMINAL_WAITING_TRIGGER: return "waiting_trigger";
    case TERMINAL_WAITING_PUFFS: return "waiting_puffs";
    case TERMINAL_WAITING_RUN: return "waiting_run";
    default: return "idle";
  }
}

void promptGuidedTerminalState() {
  switch (guidedTerminalState) {
    case TERMINAL_WAITING_PLAN_DECISION:
      renderBlynkTerminalText(
        String("Your current quit plan is ") + String(static_cast<int>(quitPlanWeeks)) +
        " weeks.\nType PLAN <weeks> to change it, or CONTINUE to keep this plan.\n" +
        appSessionStatusLine()
      );
      break;
    case TERMINAL_WAITING_SURVEY:
      if (isWeekOneBaselineFlow()) {
        renderBlynkTerminalText(
          "Week 1 puff metrics are recorded.\n"
          "How are you feeling as we calculate the week 2 dose?\n"
          "Type BETTER, NEUTRAL, STRUGGLING, or WORSE.\n"
          "I will only ask about triggers if you are struggling.\n" +
          appSessionStatusLine()
        );
      } else {
        renderBlynkTerminalText(
          "Before I calculate your next dose, how are you feeling today?\n"
          "Type BETTER, NEUTRAL, STRUGGLING, or WORSE.\n"
          "I will only ask about triggers if you are struggling.\n" +
          appSessionStatusLine()
        );
      }
      break;
    case TERMINAL_WAITING_TRIGGER:
      renderBlynkTerminalText(
        "Thanks for being honest. What made today harder?\n"
        "Type STRESS, SOCIAL, HABIT, WITHDRAWAL, MEALS, DRIVING, OTHER, or NONE.\n" +
        appSessionStatusLine()
      );
      break;
    case TERMINAL_WAITING_PUFFS:
      promptForPuffMetricsBeforeDose();
      break;
    case TERMINAL_WAITING_RUN:
      renderBlynkTerminalText(
        "I have what I need for this check-in.\n"
        "Calculating and dispensing automatically.\n" +
        appSessionStatusLine()
      );
      break;
    default:
      break;
  }
}

void beginGuidedTerminalSession() {
  resetAppSession();
  clearAppSessionScratchpad();
  logToBlynkTerminal("FadeX Chatbot session started.");
  logToBlynkTerminal(appSessionStatusLine());
  promptForPuffMetricsBeforeDose();
}

void beginPreDoseSurveyFlow() {
  pendingDose = {};
  clinicianOverridePct = -1.0f;
  appSession.surveyScore = 0.0f;
  appSession.triggerCode = 0;
  dashboard.surveyScore = 0.0f;
  dashboard.triggerCode = 0;
  publishTelemetryToBlynk();

  if (!appSessionHasPuffMetrics()) {
    promptForPuffMetricsBeforeDose();
    return;
  }

  guidedTerminalState = TERMINAL_WAITING_SURVEY;
  promptGuidedTerminalState();
}

uint32_t percentToUncsDutyCount(uint8_t percent) {
  const uint32_t maxDuty = (1u << UNCS_PWM_RESOLUTION_BITS) - 1u;
  return (static_cast<uint32_t>(percent) * maxDuty) / 100u;
}

void writeUncsPwmDuty(uint8_t percent) {
  percent = static_cast<uint8_t>(constrain(percent, 0, 100));
  const uint32_t dutyCount = percentToUncsDutyCount(percent);
  uncsActiveDutyPercent = percent;

  if (percent > 0) {
    uncsLedTurnOnMs = millis();
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(UNCS_UV_LED_PWM_PIN, dutyCount);
#else
  ledcWrite(UNCS_PWM_CHANNEL, dutyCount);
#endif
}

void applyUncsDutyPercent(uint8_t percent) {
  percent = static_cast<uint8_t>(constrain(percent, 0, 100));

  // Keep the last non-zero duty as the measurement setting so UV ON can bring
  // the LED back to the same brightness after UV OFF or UV DUTY 0.
  if (percent > 0) {
    uncsLedDutyPercent = percent;
  }

  writeUncsPwmDuty(percent);
  Serial.print(">>> UV LED PWM set to ");
  Serial.print(static_cast<int>(percent));
  Serial.println("% <<<");
}

void setUncsLed(bool enabled) {
  writeUncsPwmDuty(enabled ? uncsLedDutyPercent : 0);
  Serial.print("UNCS UV LED ");
  Serial.println(enabled ? "ON" : "OFF");
}

void setupUncsSensor() {
  analogReadResolution(12);
  analogSetPinAttenuation(UNCS_PHOTODIODE_ADC_PIN, ADC_11db);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(UNCS_UV_LED_PWM_PIN, UNCS_PWM_FREQUENCY_HZ, UNCS_PWM_RESOLUTION_BITS);
#else
  ledcSetup(UNCS_PWM_CHANNEL, UNCS_PWM_FREQUENCY_HZ, UNCS_PWM_RESOLUTION_BITS);
  ledcAttachPin(UNCS_UV_LED_PWM_PIN, UNCS_PWM_CHANNEL);
#endif

  setUncsLed(false);

  Serial.println("UNCS optical sensor initialized.");
  Serial.print("  Photodiode PDO ADC: GPIO");
  Serial.println(UNCS_PHOTODIODE_ADC_PIN);
  Serial.print("  UV LED PWM output: GPIO");
  Serial.println(UNCS_UV_LED_PWM_PIN);
  Serial.println("  Note: UVLEDPWM needs the GPIO32 jumper because GPIO39 is input-only.");
}

float readUncsPhotodiodeVoltage() {
  uint32_t millivoltSum = 0;

  for (size_t i = 0; i < UNCS_ADC_SAMPLE_COUNT; ++i) {
    millivoltSum += analogReadMilliVolts(UNCS_PHOTODIODE_ADC_PIN);
    serviceCloud();
    delayMicroseconds(100);
  }

  const float averageMillivolts =
    static_cast<float>(millivoltSum) / static_cast<float>(UNCS_ADC_SAMPLE_COUNT);
  return averageMillivolts / 1000.0f;
}

String uncsDetectorReadingLine() {
  const float voltage = readUncsPhotodiodeVoltage();
  String line =
    String("[UV LED: ") + String(static_cast<int>(uncsActiveDutyPercent)) +
    "%] -> Detector Output: " + String(voltage, 3) + " V";

  if (!isnan(uncsBlankVoltage)) {
    const float absorbance =
      calculateUncsAbsorbance(uncsDarkVoltage, uncsBlankVoltage, voltage);
    const float concentrationMm = calculateConcentration(absorbance);

    line +=
      " | A=" + String(absorbance, 3) +
      " | C=" + String(concentrationMm, 3) + " mM";
  }

  return line;
}

void printUncsDetectorReading() {
  Serial.println(uncsDetectorReadingLine());
}

void processUncsSafetyAndStreaming() {
  if (uncsActiveDutyPercent > 0 &&
      millis() - uncsLedTurnOnMs >= UNCS_AUTO_SHUTOFF_MS) {
    Serial.println();
    Serial.println("[!] UV auto-shutoff triggered. Turning LED OFF.");
    applyUncsDutyPercent(0);
  }

  if (!uncsAdcStreaming) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastUncsAdcPrintMs >= UNCS_ADC_PRINT_PERIOD_MS) {
    printUncsDetectorReading();
    lastUncsAdcPrintMs = now;
  }
}

float scaleNicotinePercentToSurrogateMm(float nicotinePercent) {
  const float boundedPercent =
    constrain(nicotinePercent, 0.0f, MAX_CONCENTRATION_PERCENT);
  return (boundedPercent / MAX_CONCENTRATION_PERCENT) * UNCS_SURROGATE_MAX_MM;
}

float calculateConcentration(float absorbance) {
  if (isnan(absorbance) || absorbance <= 0.0f) {
    return 0.0f;
  }

  return CALIB_A * powf(absorbance, CALIB_B);
}

float calculateUncsAbsorbance(float darkVoltage,
                              float blankVoltage,
                              float sampleVoltage) {
  // The dark voltage is the detector offset with the LED off. We subtract it
  // from both LED-on measurements so the concentration math only sees light.
  if (blankVoltage <= darkVoltage) {
    return 0.0f;
  }

  if (sampleVoltage <= darkVoltage) {
    sampleVoltage = darkVoltage + 0.0001f;
  }

  float transmittance =
    (sampleVoltage - darkVoltage) / (blankVoltage - darkVoltage);

  // Clamp the ratio into the physical range so log10() cannot explode from
  // noise or a sample reading that is slightly brighter than the blank.
  transmittance = constrain(transmittance, 0.0001f, 1.0f);

  return -1.0f * log10f(transmittance);
}

float calculateUncsConcentrationMm(float darkVoltage,
                                   float blankVoltage,
                                   float sampleVoltage) {
  const float absorbance =
    calculateUncsAbsorbance(darkVoltage, blankVoltage, sampleVoltage);
  return calculateConcentration(absorbance);
}

bool waitForUncsResponsiveDelay(unsigned long waitMs) {
  const unsigned long startMs = millis();

  while (millis() - startMs < waitMs) {
    serviceCloud();
    processUncsSafetyAndStreaming();

    if (pumpStopRequested || stopRequestedFromSerial()) {
      stopAllPumps();
      writeUncsPwmDuty(0);
      return false;
    }

    delay(5);
  }

  return true;
}

UncsMeasurement readUncsMeasurement() {
  UncsMeasurement measurement = {};
  measurement.valid = false;
  measurement.sampleVoltage = NAN;
  measurement.transmittance = NAN;
  measurement.absorbance = NAN;
  measurement.concentrationMm = NAN;

  if (isnan(uncsBlankVoltage) || uncsBlankVoltage <= uncsDarkVoltage) {
    return measurement;
  }

  writeUncsPwmDuty(uncsLedDutyPercent);
  if (!waitForUncsResponsiveDelay(UNCS_LED_SETTLE_MS)) {
    return measurement;
  }

  measurement.sampleVoltage = readUncsPhotodiodeVoltage();
  writeUncsPwmDuty(0);

  float sampleForMath = measurement.sampleVoltage;
  if (sampleForMath <= uncsDarkVoltage) {
    sampleForMath = uncsDarkVoltage + 0.0001f;
  }

  measurement.transmittance =
    (sampleForMath - uncsDarkVoltage) / (uncsBlankVoltage - uncsDarkVoltage);
  measurement.transmittance =
    constrain(measurement.transmittance, 0.0001f, 1.0f);
  measurement.absorbance = -1.0f * log10f(measurement.transmittance);
  measurement.concentrationMm = calculateConcentration(measurement.absorbance);
  measurement.valid = true;

  uncsSampleVoltage = measurement.sampleVoltage;
  uncsLastConcentrationMm = measurement.concentrationMm;
  publishUncsTelemetry();

  return measurement;
}

bool pulseAdjustmentPump(const PumpChannel &pump, unsigned long pulseMs) {
  runPumpForward(pump);
  const bool completed = waitForUncsResponsiveDelay(pulseMs);
  stopPump(pump);
  return completed;
}

bool runUncsSurrogateControl(float targetMm,
                             const String &targetLabel,
                             bool fromBlynkTerminal) {
  if (targetMm < 0.0f || targetMm > UNCS_SURROGATE_MAX_MM) {
    const String message =
      String("UNCS target must be 0 to ") +
      String(UNCS_SURROGATE_MAX_MM, 1) + " mM.";
    Serial.println(message);
    if (fromBlynkTerminal) {
      logToBlynkTerminal(message);
    }
    return false;
  }

  if (isnan(uncsBlankVoltage)) {
    const String message = "Run UNCS BLANK before UNCS TARGET.";
    Serial.println(message);
    if (fromBlynkTerminal) {
      logToBlynkTerminal(message);
    }
    return false;
  }

  pumpStopRequested = false;
  stopAllPumps();

  const String startMessage =
    String("UNCS surrogate trim started | target=") +
    String(targetMm, 2) + " mM (" + targetLabel + ").";
  Serial.println(startMessage);
  if (fromBlynkTerminal) {
    logToBlynkTerminal(startMessage);
  }

  runPumpForward(MIXING_PUMP);
  if (!waitForUncsResponsiveDelay(UNCS_FLUSH_DELAY_MS)) {
    Serial.println("UNCS trim aborted during initial circulation.");
    return false;
  }

  for (uint8_t step = 1; step <= UNCS_MAX_ADJUSTMENT_STEPS; ++step) {
    const WeightSnapshot weights = readWeightSnapshot();
    if (!weights.valid) {
      stopAllPumps();
      Serial.println("UNCS trim aborted: could not read mixing load cell.");
      return false;
    }

    if (weights.mixReading >= MIXING_CHAMBER_MAX_SAFE_G) {
      stopAllPumps();
      const String message =
        String("UNCS trim stopped: mixing chamber is ") +
        String(weights.mixReading, 1) + " g, at/above safe limit.";
      Serial.println(message);
      if (fromBlynkTerminal) {
        logToBlynkTerminal(message);
      }
      return false;
    }

    const UncsMeasurement measurement = readUncsMeasurement();
    if (!measurement.valid) {
      stopAllPumps();
      Serial.println("UNCS trim aborted: invalid optical reading.");
      return false;
    }

    const float errorMm = targetMm - measurement.concentrationMm;
    const float absErrorMm = fabsf(errorMm);

    Serial.print("UNCS step ");
    Serial.print(static_cast<int>(step));
    Serial.print(" | V=");
    Serial.print(measurement.sampleVoltage, 3);
    Serial.print(" V | A=");
    Serial.print(measurement.absorbance, 3);
    Serial.print(" | C=");
    Serial.print(measurement.concentrationMm, 3);
    Serial.print(" mM | target=");
    Serial.print(targetMm, 3);
    Serial.print(" mM | error=");
    Serial.print(errorMm, 3);
    Serial.println(" mM");

    if (absErrorMm <= UNCS_TARGET_TOLERANCE_MM) {
      stopAllPumps();
      const String doneMessage =
        String("UNCS target reached: C=") +
        String(measurement.concentrationMm, 2) + " mM.";
      Serial.println(doneMessage);
      if (fromBlynkTerminal) {
        logToBlynkTerminal(doneMessage);
      }
      return true;
    }

    const unsigned long pulseMs =
      absErrorMm > UNCS_LARGE_ERROR_MM ? UNCS_LARGE_PULSE_MS : UNCS_SMALL_PULSE_MS;
    const PumpChannel &adjustPump = errorMm > 0.0f ? NICOTINE_PUMP : DILUENT_PUMP;
    const char *adjustLiquid = errorMm > 0.0f ? "concentrate" : "diluent";

    Serial.print("Adding ");
    Serial.print(adjustLiquid);
    Serial.print(" with ");
    Serial.print(adjustPump.name);
    Serial.print(" pump for ");
    Serial.print(pulseMs);
    Serial.println(" ms.");

    if (!pulseAdjustmentPump(adjustPump, pulseMs)) {
      stopAllPumps();
      Serial.println("UNCS trim stopped by emergency stop command.");
      return false;
    }

    runPumpForward(MIXING_PUMP);
    if (!waitForUncsResponsiveDelay(UNCS_FLUSH_DELAY_MS)) {
      Serial.println("UNCS trim stopped during circulation delay.");
      return false;
    }
  }

  stopAllPumps();
  const String message = "UNCS trim stopped: max adjustment steps reached.";
  Serial.println(message);
  if (fromBlynkTerminal) {
    logToBlynkTerminal(message);
  }
  return false;
}

void printUncsSignalGuidance(float darkVoltage,
                             float blankVoltage,
                             float sampleVoltage) {
  if (isnan(blankVoltage)) {
    Serial.println("UNCS guidance: run UNCS BLANK after placing the water blank.");
    return;
  }

  const float lightWindow = blankVoltage - darkVoltage;
  Serial.print("UNCS light window V_blank - V_dark: ");
  Serial.print(lightWindow, 3);
  Serial.println(" V");

  if (lightWindow < UNCS_MIN_LIGHT_WINDOW_V) {
    Serial.println("UNCS guidance: blank is too close to dark. Increase LED duty, improve alignment, or increase TIA gain.");
  }

  if (blankVoltage > UNCS_ADC_SATURATION_WARNING_V) {
    Serial.println("UNCS guidance: blank is near ADC saturation. Reduce LED duty or lower TIA gain.");
  }

  if (!isnan(sampleVoltage)) {
    if (sampleVoltage > blankVoltage) {
      Serial.println("UNCS guidance: sample is brighter than blank. Re-check blank/sample order and cuvette placement.");
    } else if (sampleVoltage <= darkVoltage) {
      Serial.println("UNCS guidance: sample is at/below dark. Dilute the sample or increase optical signal.");
    }
  }
}

String uncsStatusLine() {
  return
    String("UNCS | dark=") + String(uncsDarkVoltage, 3) +
    " V | blank=" + (isnan(uncsBlankVoltage) ? String("not set") : String(uncsBlankVoltage, 3) + " V") +
    " | sample=" + (isnan(uncsSampleVoltage) ? String("not set") : String(uncsSampleVoltage, 3) + " V") +
    " | concentration=" + (isnan(uncsLastConcentrationMm) ? String("not set") : String(uncsLastConcentrationMm, 3) + " mM") +
    " | LED setting=" + String(static_cast<int>(uncsLedDutyPercent)) + "%" +
    " | LED output=" + String(static_cast<int>(uncsActiveDutyPercent)) + "%";
}

void publishUncsTelemetry() {
  if (USE_BLYNK_USB_BRIDGE) {
    Serial.print("BRIDGE_BATCH v25=");
    Serial.print(uncsDarkVoltage, 3);
    if (!isnan(uncsBlankVoltage)) {
      Serial.print(" v26=");
      Serial.print(uncsBlankVoltage, 3);
    }
    if (!isnan(uncsSampleVoltage)) {
      Serial.print(" v27=");
      Serial.print(uncsSampleVoltage, 3);
    }
    if (!isnan(uncsLastConcentrationMm)) {
      Serial.print(" v28=");
      Serial.print(uncsLastConcentrationMm, 3);
    }
    Serial.println();
  }

  if (!Blynk.connected()) {
    return;
  }

  // Optional app datastreams. If these are not created in Blynk, the serial and
  // chatbot output still work.
  Blynk.virtualWrite(V25, uncsDarkVoltage);
  if (!isnan(uncsBlankVoltage)) {
    Blynk.virtualWrite(V26, uncsBlankVoltage);
  }
  if (!isnan(uncsSampleVoltage)) {
    Blynk.virtualWrite(V27, uncsSampleVoltage);
  }
  if (!isnan(uncsLastConcentrationMm)) {
    Blynk.virtualWrite(V28, uncsLastConcentrationMm);
  }
}

void executeUncsCommand(String command, bool fromBlynkTerminal) {
  command.trim();
  String originalUpper = command;
  originalUpper.toUpperCase();

  if (originalUpper == "UV" || originalUpper == "UV HELP") {
    command = "UNCS HELP";
  } else if (originalUpper == "ON" ||
             originalUpper == "UV ON" ||
             originalUpper == "LED ON") {
    command = "UNCS LED ON";
  } else if (originalUpper == "OFF" ||
             originalUpper == "UV OFF" ||
             originalUpper == "LED OFF") {
    command = "UNCS LED OFF";
  } else if (originalUpper.startsWith("DUTY ")) {
    command = String("UNCS PWM ") + command.substring(5);
  } else if (originalUpper.startsWith("PWM ")) {
    command = String("UNCS PWM ") + command.substring(4);
  } else if (originalUpper.startsWith("UV DUTY ")) {
    command = String("UNCS PWM ") + command.substring(8);
  } else if (originalUpper.startsWith("UV PWM ")) {
    command = String("UNCS PWM ") + command.substring(7);
  } else if (originalUpper == "UV READ") {
    command = "UNCS ADC";
  } else if (originalUpper == "UV STREAM ON") {
    command = "UNCS STREAM ON";
  } else if (originalUpper == "UV STREAM OFF") {
    command = "UNCS STREAM OFF";
  } else if (originalUpper == "PRINT" || originalUpper == "PRINT TOGGLE") {
    command = "UNCS PRINT TOGGLE";
  } else if (originalUpper == "UNCS PRINT" ||
             originalUpper == "UNCS PRINT TOGGLE") {
    command = "UNCS PRINT TOGGLE";
  } else if (originalUpper == "CIRC" || originalUpper == "PUMP3") {
    command = "UNCS CIRC";
  } else if (originalUpper.startsWith("CIRC ")) {
    command = String("UNCS CIRC ") + command.substring(5);
  } else if (originalUpper.startsWith("PUMP3 ")) {
    command = String("UNCS CIRC ") + command.substring(6);
  } else if (originalUpper.startsWith("TARGET ")) {
    command = String("UNCS TARGET ") + command.substring(7);
  } else if (originalUpper.startsWith("TRIM ")) {
    command = String("UNCS TARGET ") + command.substring(5);
  } else if (originalUpper.startsWith("TARGETMM ")) {
    command = String("UNCS TARGETMM ") + command.substring(9);
  }

  String upper = command;
  upper.toUpperCase();

  if (upper == "UNCS" || upper == "UNCS HELP") {
    const String help =
      "UV commands: UV ON, UV OFF, UV DUTY <0-100>, UV READ, UV STREAM ON, UV STREAM OFF.\n"
      "UNCS commands: UNCS DARK, UNCS BLANK, UNCS SAMPLE, UNCS STATUS, "
      "UNCS LED ON, UNCS LED OFF, UNCS PWM <0-100>, UNCS VDARK <volts>, "
      "UNCS CIRC <ms>, UNCS TARGET <0-5%>, UNCS TARGETMM <mM>, PRINT TOGGLE.";
    Serial.println(help);
    if (fromBlynkTerminal) {
      renderBlynkTerminalText(help);
    }
    return;
  }

  if (upper == "UNCS STATUS") {
    const String status = uncsStatusLine();
    Serial.println(status);
    printUncsSignalGuidance(uncsDarkVoltage, uncsBlankVoltage, uncsSampleVoltage);
    if (fromBlynkTerminal) {
      renderBlynkTerminalText(status);
    }
    return;
  }

  if (upper == "UNCS ADC" || upper == "UNCS DETECTOR" || upper == "UNCS VOLTAGE") {
    const String reading = uncsDetectorReadingLine();
    Serial.println(reading);
    if (fromBlynkTerminal) {
      renderBlynkTerminalText(reading);
    }
    return;
  }

  if (upper == "UNCS STREAM ON") {
    uncsAdcStreaming = true;
    lastUncsAdcPrintMs = 0;
    const String message = "UV detector streaming ON. Type UV STREAM OFF to stop.";
    Serial.println(message);
    if (fromBlynkTerminal) {
      logToBlynkTerminal(message);
    }
    return;
  }

  if (upper == "UNCS STREAM OFF") {
    uncsAdcStreaming = false;
    const String message = "UV detector streaming OFF.";
    Serial.println(message);
    if (fromBlynkTerminal) {
      logToBlynkTerminal(message);
    }
    return;
  }

  if (upper == "UNCS PRINT TOGGLE") {
    uncsAdcStreaming = !uncsAdcStreaming;
    lastUncsAdcPrintMs = 0;
    const String message =
      String("UNCS print stream ") + (uncsAdcStreaming ? "ON." : "OFF.");
    Serial.println(message);
    if (fromBlynkTerminal) {
      logToBlynkTerminal(message);
    }
    return;
  }

  if (upper == "UNCS LED ON") {
    setUncsLed(true);
    if (fromBlynkTerminal) {
      logToBlynkTerminal("UNCS UV LED ON.");
    }
    return;
  }

  if (upper == "UNCS LED OFF") {
    setUncsLed(false);
    if (fromBlynkTerminal) {
      logToBlynkTerminal("UNCS UV LED OFF.");
    }
    return;
  }

  if (upper.startsWith("UNCS PWM ")) {
    const int requestedDuty = command.substring(9).toInt();
    applyUncsDutyPercent(static_cast<uint8_t>(constrain(requestedDuty, 0, 100)));
    const String message =
      String("UNCS LED PWM output set to ") + String(static_cast<int>(uncsActiveDutyPercent)) + "%.";
    Serial.println(message);
    if (fromBlynkTerminal) {
      logToBlynkTerminal(message);
    }
    return;
  }

  if (upper.startsWith("UNCS VDARK ")) {
    const float requestedDarkVoltage = command.substring(11).toFloat();
    if (requestedDarkVoltage < 0.0f || requestedDarkVoltage > 3.3f) {
      Serial.println("UNCS VDARK must be between 0.0 and 3.3 V.");
      if (fromBlynkTerminal) {
        logToBlynkTerminal("UNCS VDARK must be between 0.0 and 3.3 V.");
      }
      return;
    }

    uncsDarkVoltage = requestedDarkVoltage;
    const String message =
      String("UNCS dark voltage manually set to ") + String(uncsDarkVoltage, 3) + " V.";
    Serial.println(message);
    publishUncsTelemetry();
    if (fromBlynkTerminal) {
      logToBlynkTerminal(message);
    }
    return;
  }

  if (upper == "UNCS CIRC" || upper == "UNCS PUMP" ||
      upper.startsWith("UNCS CIRC ") || upper.startsWith("UNCS PUMP ")) {
    unsigned long requestedMs = UNCS_CIRC_DEFAULT_MS;

    if (upper.startsWith("UNCS CIRC ") || upper.startsWith("UNCS PUMP ")) {
      requestedMs = static_cast<unsigned long>(command.substring(10).toInt());
    }

    if (requestedMs == 0 || requestedMs > UNCS_CIRC_MAX_MS) {
      const String message =
        String("UNCS CIRC time must be 1 to ") +
        String(UNCS_CIRC_MAX_MS) + " ms.";
      Serial.println(message);
      if (fromBlynkTerminal) {
        logToBlynkTerminal(message);
      }
      return;
    }

    const String startMessage =
      String("Running Pump 3 / UNCS circulation for ") +
      String(requestedMs) + " ms.";
    Serial.println(startMessage);
    if (fromBlynkTerminal) {
      logToBlynkTerminal(startMessage);
    }

    stopAllPumps();
    pumpStopRequested = false;
    runPump(MIXING_PUMP, requestedMs);

    const String doneMessage = "UNCS circulation pump run complete.";
    Serial.println(doneMessage);
    if (fromBlynkTerminal) {
      logToBlynkTerminal(doneMessage);
    }
    return;
  }

  if (upper.startsWith("UNCS TARGETMM ")) {
    const float targetMm = command.substring(14).toFloat();
    runUncsSurrogateControl(
      targetMm,
      String(targetMm, 2) + " mM direct target",
      fromBlynkTerminal
    );
    return;
  }

  if (upper.startsWith("UNCS TARGET ")) {
    const float nicotinePercent = command.substring(12).toFloat();
    if (nicotinePercent < 0.0f || nicotinePercent > MAX_CONCENTRATION_PERCENT) {
      const String message =
        String("UNCS TARGET must be 0 to ") +
        String(MAX_CONCENTRATION_PERCENT, 1) + "%.";
      Serial.println(message);
      if (fromBlynkTerminal) {
        logToBlynkTerminal(message);
      }
      return;
    }

    const float targetMm = scaleNicotinePercentToSurrogateMm(nicotinePercent);
    runUncsSurrogateControl(
      targetMm,
      String(nicotinePercent, 2) + "% nicotine-equivalent",
      fromBlynkTerminal
    );
    return;
  }

  if (upper == "UNCS DARK") {
    setUncsLed(false);
    delay(UNCS_LED_SETTLE_MS);
    uncsDarkVoltage = readUncsPhotodiodeVoltage();
    const String message =
      String("UNCS dark voltage captured: ") + String(uncsDarkVoltage, 3) + " V.";
    Serial.println(message);
    publishUncsTelemetry();
    if (fromBlynkTerminal) {
      logToBlynkTerminal(message);
    }
    return;
  }

  if (upper == "UNCS BLANK") {
    setUncsLed(true);
    delay(UNCS_LED_SETTLE_MS);
    uncsBlankVoltage = readUncsPhotodiodeVoltage();
    const String message =
      String("UNCS blank voltage captured: ") + String(uncsBlankVoltage, 3) + " V.";
    Serial.println(message);
    printUncsSignalGuidance(uncsDarkVoltage, uncsBlankVoltage, uncsSampleVoltage);
    publishUncsTelemetry();
    if (fromBlynkTerminal) {
      renderBlynkTerminalText(message + "\n" + uncsStatusLine());
    }
    return;
  }

  if (upper == "UNCS SAMPLE" || upper == "UNCS READ") {
    if (isnan(uncsBlankVoltage)) {
      const String message = "Run UNCS BLANK before UNCS SAMPLE.";
      Serial.println(message);
      if (fromBlynkTerminal) {
        logToBlynkTerminal(message);
      }
      return;
    }

    const UncsMeasurement measurement = readUncsMeasurement();
    if (!measurement.valid) {
      const String message = "UNCS SAMPLE failed: invalid optical reading.";
      Serial.println(message);
      if (fromBlynkTerminal) {
        logToBlynkTerminal(message);
      }
      return;
    }

    const String result =
      String("UNCS sample voltage: ") + String(measurement.sampleVoltage, 3) + " V\n" +
      "Absorbance: " + String(measurement.absorbance, 3) + "\n" +
      "Vitamin C concentration: " + String(measurement.concentrationMm, 3) + " mM";

    Serial.println(result);
    printUncsSignalGuidance(uncsDarkVoltage, uncsBlankVoltage, uncsSampleVoltage);
    if (fromBlynkTerminal) {
      renderBlynkTerminalText(result + "\n" + uncsStatusLine());
    }
    return;
  }

  const String message =
    "Unknown UNCS command. Try UNCS HELP.";
  Serial.println(message);
  if (fromBlynkTerminal) {
    logToBlynkTerminal(message);
  }
}

// turn one pump fully off
void stopPump(const PumpChannel &pump) {
  digitalWrite(pump.in1, LOW);
  digitalWrite(pump.in2, LOW);
}

// emergency helper func to stop every pump at once
void stopAllPumps() {
  stopPump(NICOTINE_PUMP);
  stopPump(DILUENT_PUMP);
  stopPump(MIXING_PUMP);
}

// configure the output pins for a pump channel
void setupPump(const PumpChannel &pump) {
  pinMode(pump.in1, OUTPUT);
  pinMode(pump.in2, OUTPUT);
  stopPump(pump);
}

void runPumpForward(const PumpChannel &pump) {
  // The assembled pump polarity is flipped relative to the first schematic
  // assumption, so software "forward" is IN1 low and IN2 high.
  digitalWrite(pump.in1, LOW);
  digitalWrite(pump.in2, HIGH);
}

// run a pump for a fixed amount of time.
// this implementation uses a short loop instead of one long delay so Blynk
// stays connected while the pumps are running.
void runPump(const PumpChannel &pump, unsigned long runMs) {
  if (runMs == 0) {
    return;
  }

  Serial.print("Running ");
  Serial.print(pump.name);
  Serial.print(" pump for ");
  Serial.print(runMs);
  Serial.println(" ms");

  if (!PUMPS_ENABLED) {
    Serial.print("APP-ONLY DRY RUN: ");
    Serial.print(pump.name);
    Serial.println(" pump skipped. No GPIO motor drive was sent.");
    logToBlynkTerminal(
      String("Dry run: ") + pump.name + " pump skipped (" +
      String(runMs) + " ms)."
    );
    return;
  }

  runPumpForward(pump);

  const unsigned long startMs = millis();
  while (millis() - startMs < runMs) {
    serviceCloud();
    if (pumpStopRequested || stopRequestedFromSerial()) {
      stopAllPumps();
      Serial.println("Fixed-time pump run stopped by emergency stop command.");
      logToBlynkTerminal("Pump run stopped by emergency stop command.");
      break;
    }
    delay(1);
  }

  stopPump(pump);
}

bool waitForAllLoadCellsReady(unsigned long timeoutMs) {
  const unsigned long startMs = millis();

  while (millis() - startMs < timeoutMs) {
    bool allReady = true;
    for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
      if (digitalRead(loadCells[i].dataPin) == HIGH) {
        allReady = false;
        break;
      }
    }

    if (allReady) {
      return true;
    }

    Blynk.run();
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

  // All HX711 boards share SCLK on the PCB. This clocks every amplifier
  // together and samples every DOUT line on the same 24 pulses.
  noInterrupts();
  for (uint8_t bitIndex = 0; bitIndex < 24; ++bitIndex) {
    digitalWrite(HX711_SCK_PIN, HIGH);
    delayMicroseconds(1);

    for (uint8_t i = 0; i < count; ++i) {
      unsignedValues[i] <<= 1;
      if (digitalRead(loadCells[i].dataPin) == HIGH) {
        unsignedValues[i] |= 1;
      }
    }

    digitalWrite(HX711_SCK_PIN, LOW);
    delayMicroseconds(1);
  }

  // One extra pulse selects HX711 channel A gain 128 for the next conversion.
  digitalWrite(HX711_SCK_PIN, HIGH);
  delayMicroseconds(1);
  digitalWrite(HX711_SCK_PIN, LOW);
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
      const LoadCellChannel &cell = loadCells[i];
      sums[i] += (static_cast<float>(rawValues[i] - cell.tareOffset) /
                  cell.calibrationFactor);
    }

    Blynk.run();
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

bool tareLoadCells(bool requireZeroVerification) {
  Serial.println("Taring shared-SCLK load cells...");

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

    Blynk.run();
    delay(HX711_SAMPLE_DELAY_MS);
    yield();
    validSamples++;
  }

  if (validSamples == 0) {
    return false;
  }

  for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
    const double averageOffset = offsetSums[i] / validSamples;
    loadCells[i].tareOffset =
      static_cast<long>(averageOffset + (averageOffset >= 0.0 ? 0.5 : -0.5));
    Serial.print("  ");
    Serial.print(loadCells[i].name);
    Serial.print(" tared at raw offset ");
    Serial.println(loadCells[i].tareOffset);
  }

  if (requireZeroVerification) {
    delay(SCALE_SETTLE_MS);

    float verifiedGrams[MONITORED_CELL_COUNT] = {};
    if (!readAverageGramsAll(verifiedGrams, TARE_VERIFY_SAMPLES)) {
      Serial.println("Tare verification failed: could not read load cells.");
      return false;
    }

    bool tareIsCentered = true;
    Serial.print("Tare verification:");
    for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
      Serial.print(" ");
      Serial.print(loadCells[i].name);
      Serial.print("=");
      Serial.print(verifiedGrams[i], 3);
      Serial.print("g");

      if (fabsf(verifiedGrams[i]) > TARE_ZERO_TOLERANCE_G) {
        tareIsCentered = false;
      }
    }
    Serial.println();

    if (!tareIsCentered) {
      Serial.print("Tare verification failed. All load cells must be within +/-");
      Serial.print(TARE_ZERO_TOLERANCE_G, 2);
      Serial.println(" g before dose delivery.");
      logToBlynkTerminal(
        String("Dose aborted: tare not centered within +/-") +
        String(TARE_ZERO_TOLERANCE_G, 2) + " g."
      );
      return false;
    }
  }

  Serial.println("Tare complete. Dose control will use LCA3 mixing weight.");
  return true;
}

void setupLoadCells() {
  Serial.println("Initializing shared-SCLK load cells...");
  pinMode(HX711_SCK_PIN, OUTPUT);
  digitalWrite(HX711_SCK_PIN, LOW);

  for (uint8_t i = 0; i < MONITORED_CELL_COUNT; ++i) {
    pinMode(loadCells[i].dataPin, INPUT);
    Serial.print(loadCells[i].name);
    Serial.print(" ");
    Serial.print(loadCells[i].role);
    Serial.print(" on GPIO");
    Serial.println(loadCells[i].dataPin);
  }

  if (!tareLoadCells()) {
    Serial.println("Startup tare failed. You can retry with the TARE command.");
  }
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

void printLoadCellReadings() {
  Serial.println("--- LOAD CELL READINGS ---");

  const WeightSnapshot weights = readWeightSnapshot();
  if (!weights.valid) {
    Serial.println("Could not read shared HX711 values.");
    return;
  }

  Serial.print("LCA1 (Nic source reservoir): ");
  Serial.print(weights.nicReading, 2);
  Serial.println(" g");
  Serial.print("LCA2 (Diluent source reservoir): ");
  Serial.print(weights.dilReading, 2);
  Serial.println(" g");
  Serial.print("LCA3 (Mixing reservoir): ");
  Serial.print(weights.mixReading, 2);
  Serial.println(" g");
}

bool isStopCommand(const String &commandUpper) {
  return commandUpper == "STOP" ||
         commandUpper == "S" ||
         commandUpper == "ESTOP" ||
         commandUpper == "E-STOP" ||
         commandUpper == "ABORT" ||
         commandUpper == "HALT" ||
         commandUpper == "KILL" ||
         commandUpper == "ALLSTOP" ||
         commandUpper == "ALL STOP" ||
         commandUpper == "EMERGENCY" ||
         commandUpper == "EMERGENCY STOP";
}

bool stopRequestedFromSerial() {
  if (pumpStopRequested) {
    return true;
  }

  if (!Serial.available()) {
    return false;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();

  if (isStopCommand(command)) {
    pumpStopRequested = true;
    return true;
  }

  Serial.println("Pump run in progress. Type STOP, ESTOP, ABORT, or S to abort.");
  return false;
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

float deliveryTolerance(float expectedTotalGrams) {
  (void)expectedTotalGrams;
  return DELIVERY_TOLERANCE_G;
}

bool pumpUntilMixingGain(const PumpChannel &pump,
                         float doseMl,
                         float targetMixingGainGrams,
                         const char *liquidName) {
  const unsigned long startMs = millis();
  WeightSnapshot weights = readWeightSnapshot();

  if (!weights.valid) {
    stopAllPumps();
    Serial.println("Abort: shared HX711 read failed before pumping.");
    logToBlynkTerminal("Dose aborted: load-cell read failed before pumping.");
    return false;
  }

  float mixingGainGrams = weights.mixingGain;

  Serial.println();
  Serial.print("Delivering ");
  Serial.print(doseMl, 2);
  Serial.print(" mL of ");
  Serial.print(liquidName);
  Serial.print(" using ");
  Serial.println(pump.name);
  Serial.print("Control target: LCA3 mixing reservoir gain ");
  Serial.print(targetMixingGainGrams, 2);
  Serial.println(" g");
  logToBlynkTerminal(
    String("Delivering ") + String(doseMl, 2) + " mL " + liquidName +
    " to LCA3 target " + String(targetMixingGainGrams, 2) + " g."
  );

  while (mixingGainGrams < targetMixingGainGrams - MIXING_TARGET_TOLERANCE_G) {
    serviceCloud();

    if (millis() - startMs > MAX_SINGLE_PUMP_TIME_MS) {
      stopAllPumps();
      Serial.println("Abort: pump safety timeout reached.");
      logToBlynkTerminal("Dose aborted: pump safety timeout reached.");
      return false;
    }

    if (stopRequestedFromSerial()) {
      stopAllPumps();
      Serial.println("Abort: STOP command received.");
      logToBlynkTerminal("Dose aborted: STOP command received.");
      return false;
    }

    const float remainingGrams = targetMixingGainGrams - mixingGainGrams;
    const unsigned long burstMs = burstDurationForRemaining(remainingGrams);

    Serial.print(liquidName);
    Serial.print(" burst ");
    Serial.print(burstMs);
    Serial.println(" ms");

    if (PUMPS_ENABLED) {
      runPumpForward(pump);
      delay(burstMs);
      stopPump(pump);
    } else {
      Serial.println("APP-ONLY DRY RUN: pump burst skipped.");
      delay(burstMs);
    }

    Serial.println("Pump stopped. Reading settled weights...");
    weights = readWeightSnapshot();
    if (!weights.valid) {
      stopAllPumps();
      Serial.println("Abort: shared HX711 read failed after pump burst.");
      logToBlynkTerminal("Dose aborted: load-cell read failed after pump burst.");
      return false;
    }

    mixingGainGrams = weights.mixingGain;

    Serial.print(liquidName);
    Serial.print(" mixing gain: ");
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

bool runWeightControlledDose(float nicotineMl, float diluentMl) {
  if (nicotineMl < 0.0f || diluentMl < 0.0f || (nicotineMl + diluentMl) <= 0.0f) {
    Serial.println("Dose targets must be positive.");
    return false;
  }

  pumpStopRequested = false;
  stopAllPumps();

  Serial.println();
  Serial.println("=== FADEX WEIGHT-CONTROLLED DOSE ===");
  Serial.print("Nicotine target: ");
  Serial.print(nicotineMl, 2);
  Serial.println(" mL");
  Serial.print("Diluent target: ");
  Serial.print(diluentMl, 2);
  Serial.println(" mL");
  logToBlynkTerminal(
    String("Weight-controlled dose started: nic=") + String(nicotineMl, 2) +
    " mL, dil=" + String(diluentMl, 2) + " mL."
  );

  Serial.println("Taring load cells with reservoirs/cups in their starting positions...");
  logToBlynkTerminal("Taring load cells before delivery.");
  delay(1500);
  if (!tareLoadCells(true)) {
    stopAllPumps();
    logToBlynkTerminal("Dose aborted: tare failed.");
    return false;
  }

  if (nicotineMl > 0.0f) {
    const float nicotineTargetGrams = nicotineMl * WATER_GRAMS_PER_ML;
    if (!pumpUntilMixingGain(
          NICOTINE_PUMP,
          nicotineMl,
          nicotineTargetGrams,
          "NIC/WATER")) {
      return false;
    }
  }

  if (diluentMl > 0.0f) {
    const float totalTargetGrams = (nicotineMl + diluentMl) * WATER_GRAMS_PER_ML;
    if (!pumpUntilMixingGain(
          DILUENT_PUMP,
          diluentMl,
          totalTargetGrams,
          "DIL/WATER")) {
      return false;
    }
  }

  stopAllPumps();
  delay(1000);

  const WeightSnapshot finalWeights = readWeightSnapshot();
  if (!finalWeights.valid) {
    Serial.println("Final shared HX711 read failed. Dose result is invalid.");
    logToBlynkTerminal("Dose finished, but final load-cell read failed.");
    return false;
  }

  const float expectedTotal = (nicotineMl + diluentMl) * WATER_GRAMS_PER_ML;
  const float mixingGain = finalWeights.mixingGain;
  const float targetError = fabsf(mixingGain - expectedTotal);
  const float allowedError = deliveryTolerance(expectedTotal);

  Serial.println();
  Serial.println("=== DOSE DELIVERY RESULTS ===");
  Serial.print("Mixing reservoir gain: ");
  Serial.print(mixingGain, 2);
  Serial.println(" g");
  Serial.print("Expected delivered mass: ");
  Serial.print(expectedTotal, 2);
  Serial.println(" g");
  Serial.print("Target delivery error from LCA3: ");
  Serial.print(targetError, 2);
  Serial.print(" g | allowed: +/-");
  Serial.print(allowedError, 2);
  Serial.println(" g");

  if (targetError <= allowedError) {
    Serial.println("PASS: mixing reservoir received target mass within tolerance.");
    logToBlynkTerminal(
      String("Dose delivered: LCA3=") + String(mixingGain, 2) +
      " g, target=" + String(expectedTotal, 2) + " g."
    );
    return true;
  }

  Serial.println("WARNING: mixing reservoir mass is outside tolerance.");
  logToBlynkTerminal(
    String("Dose warning: LCA3=") + String(mixingGain, 2) +
    " g, target=" + String(expectedTotal, 2) + " g."
  );
  return false;
}

void runMixingMassBalanceTest(float nicMl, float dilMl) {
  if (!runWeightControlledDose(nicMl, dilMl)) {
    Serial.println("MIXTEST finished with warning/abort.");
    return;
  }

  Serial.println("MIXTEST finished successfully.");
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

  Serial.println("For this integrated test, use Pump 1 or Pump 2.");
  Serial.println("Example: MIXONE 1 20 or MIXONE 2 20");
}

// app-triggered priming is a simple short run of the feed pumps so the tubing
// is full before a real dose is prepared
void runPrimeSequence() {
  pumpStopRequested = false;
  Serial.println("Priming nicotine and diluent feed lines...");
  logToBlynkTerminal("Prime started: nicotine and diluent lines.");
  runPump(NICOTINE_PUMP, PRIME_RUN_MS);
  runPump(DILUENT_PUMP, PRIME_RUN_MS);
  Serial.println("Prime sequence finished.");
  logToBlynkTerminal("Prime finished.");
}

// Run the most recently calculated dose. A handheld sync calculates and stores
// the dose first; Blynk V14 starts this physical dispensing step.
void executePendingDose() {
  if (!pendingDose.ready) {
    Serial.println("No calculated dose is waiting. Sync handheld data first.");
    return;
  }

  Serial.println();
  Serial.println("--- RUNNING CALCULATED DOSE ---");
  Serial.print("Session ID: ");
  Serial.println(pendingDose.sessionId);
  Serial.print("Applied concentration (%): ");
  Serial.println(pendingDose.appliedConcentrationPct, 2);
  logToBlynkTerminal(
    String("Running dose for session ") + String(pendingDose.sessionId) +
    " at " + String(pendingDose.appliedConcentrationPct, 2) + "%."
  );

  const bool delivered = runWeightControlledDose(
    pendingDose.nicotineVolumeMl,
    pendingDose.diluentVolumeMl
  );

  if (!delivered) {
    Serial.println("Dose execution stopped before concentration state was updated.");
    logToBlynkTerminal(
      "Dose did not pass delivery check. Concentration was not advanced; retry Run Dose."
    );
    return;
  }

  if (MIXING_RUN_MS > 0) {
    runPump(MIXING_PUMP, MIXING_RUN_MS);
  }

  currentConcentration = pendingDose.appliedConcentrationPct;
  if (currentTreatmentWeek < quitPlanWeeks) {
    currentTreatmentWeek++;
  }
  dashboard.currentConcentrationPct = currentConcentration;
  dashboard.nextConcentrationPct = currentConcentration;
  dashboard.currentTreatmentWeek = currentTreatmentWeek;
  dashboard.plannedWeeklyStepPct = computePlannedWeeklyStep(currentConcentration);
  pendingDose.ready = false;

  publishTelemetryToBlynk();
  Serial.println("Dose execution finished.");
  logToBlynkTerminal("Dose execution finished.");
}

void resetAppSession() {
  appSession.totalPuffs = 0;
  appSession.totalDuration = 0.0f;
  appSession.surveyScore = 0.0f;
  appSession.triggerCode = 0;
  appSession.sessionId = 0;
  appPuffActive = false;

  dashboard.dailyPuffCount = 0;
  dashboard.dailyDurationSec = 0.0f;
  dashboard.surveyScore = 0.0f;
  dashboard.triggerCode = 0;
  publishTelemetryToBlynk();
}

void clearAppSessionScratchpad() {
  appSession.totalPuffs = 0;
  appSession.totalDuration = 0.0f;
  appSession.surveyScore = 0.0f;
  appSession.triggerCode = 0;
  appSession.sessionId = 0;
  appPuffActive = false;
}

void recordAppPuff(float durationSeconds) {
  if (durationSeconds < 0.05f) {
    return;
  }

  appSession.totalPuffs++;
  appSession.totalDuration += durationSeconds;

  dashboard.dailyPuffCount = appSession.totalPuffs;
  dashboard.dailyDurationSec = appSession.totalDuration;
  publishTelemetryToBlynk();

  Serial.print("Blynk puff recorded | count=");
  Serial.print(appSession.totalPuffs);
  Serial.print(" duration=");
  Serial.print(appSession.totalDuration, 1);
  Serial.println("s");
  logToBlynkTerminal(
    String("App puff recorded. Count=") + String(appSession.totalPuffs) +
    ", duration=" + String(appSession.totalDuration, 1) + " s"
  );

  if (guidedTerminalState == TERMINAL_WAITING_PUFFS) {
    guidedTerminalState = TERMINAL_WAITING_SURVEY;
    promptGuidedTerminalState();
  }
}

void setAppSurveyScore(float surveyScore) {
  appSession.surveyScore = snapSurveyScore(surveyScore);
  dashboard.surveyScore = appSession.surveyScore;
  publishTelemetryToBlynk();

  Serial.print("Blynk survey score set to ");
  Serial.println(appSession.surveyScore, 2);
  Serial.print("Survey choice: ");
  Serial.println(surveyLabel(appSession.surveyScore));
}

void setAppTriggerCode(int triggerCode) {
  appSession.triggerCode = static_cast<uint8_t>(constrain(triggerCode, 0, 7));
  dashboard.triggerCode = appSession.triggerCode;
  publishTelemetryToBlynk();

  Serial.print("Blynk trigger code set to ");
  Serial.println(appSession.triggerCode);
}

void processAppSessionAndRunDose() {
  if (pendingDose.ready) {
    executePendingDose();
    return;
  }

  if (appSession.totalPuffs == 0 && appSession.totalDuration < 0.05f) {
    Serial.println("No Blynk puff data yet. Press the Puff button before Run Dose.");
    logToBlynkTerminal(
      "No puff data yet. Use the Puff button or type PUFF <seconds> first."
    );
    return;
  }

  if (appSession.sessionId == 0) {
    appSession.sessionId = nextAppSessionId++;
  }
  Serial.println();
  Serial.println("--- GUIDED DOSE SESSION ---");
  processSession(appSession, nullptr);

  if (pendingDose.ready) {
    executePendingDose();
    resetAppSession();
  }
}

// register the handheld as a peer before sending a response packet
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

// enforcing the safety bounds from the algorithm
// never go below 0.5% and never exceed the 5.0% baseline
float clampDose(float value) {
  if (value < MIN_CONCENTRATION_PERCENT) {
    return MIN_CONCENTRATION_PERCENT;
  }
  if (value > MAX_CONCENTRATION_PERCENT) {
    return MAX_CONCENTRATION_PERCENT;
  }
  return value;
}

// translate total usage duration into one of the four usage tiers
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

// convert the usage tier into the multiplier used by the formula
float usageMultiplierForTier(uint8_t tier) {
  switch (tier) {
    case 0: return 0.9f;
    case 1: return 1.0f;
    case 2: return 1.1f;
    default: return 1.3f;
  }
}

float snapSurveyScore(float surveyScore) {
  uint8_t bestIndex = 0;
  float bestDiff = fabsf(surveyScore - SURVEY_VALUES[0]);

  for (uint8_t i = 1; i < 4; ++i) {
    const float diff = fabsf(surveyScore - SURVEY_VALUES[i]);
    if (diff < bestDiff) {
      bestDiff = diff;
      bestIndex = i;
    }
  }

  return SURVEY_VALUES[bestIndex];
}

// snap the incoming survey score to the nearest valid bucket
// so it maps cleanly into the 4 x 4 scenario matrix
uint8_t classifySurveyTier(float surveyScore) {
  const float snappedScore = snapSurveyScore(surveyScore);
  for (uint8_t i = 0; i < 4; ++i) {
    if (fabsf(snappedScore - SURVEY_VALUES[i]) < 0.001f) {
      return i;
    }
  }
  return 0;
}

const char *surveyLabel(float surveyScore) {
  const uint8_t tier = classifySurveyTier(surveyScore);
  return SURVEY_LABELS[tier];
}

// convert a numeric trigger code back into readable text for logs/debugging
const char *triggerLabel(uint8_t triggerCode) {
  const size_t triggerCount = sizeof(TRIGGER_LABELS) / sizeof(TRIGGER_LABELS[0]);
  if (triggerCode >= triggerCount) {
    return "Unknown";
  }
  return TRIGGER_LABELS[triggerCode];
}

// scenario code is exported to the app as a simple integer 1 to 16
uint8_t scenarioCodeForTiers(uint8_t usageTier, uint8_t surveyTier) {
  return static_cast<uint8_t>((usageTier * 4) + surveyTier + 1);
}

// Estimate savings by comparing what this session would have cost at the
// original 5.0% baseline against what it costs at the new tapered
// concentration, scaled by how much of a "baseline day" of usage the session
// represents.
float estimateSessionMoneySaved(float appliedConcentrationPct,
                                float sessionDurationSeconds) {
  if (sessionDurationSeconds <= 0.05f) {
    return 0.0f;
  }

  const float usageFactor = constrain(
    sessionDurationSeconds / BASELINE_DAILY_USAGE_SECONDS,
    0.0f,
    MAX_USAGE_FACTOR_FOR_SAVINGS
  );

  const float baselineSessionCost = BASELINE_DAILY_COST_USD * usageFactor;
  const float taperedSessionCost =
    baselineSessionCost * (appliedConcentrationPct / STARTING_CONCENTRATION_PERCENT);

  return fmaxf(0.0f, baselineSessionCost - taperedSessionCost);
}

float computePlannedWeeklyStep(float currentConcentrationPct) {
  const int weeksRemaining =
    static_cast<int>(quitPlanWeeks) - static_cast<int>(currentTreatmentWeek);

  if (weeksRemaining <= 0) {
    return fmaxf(0.0f, currentConcentrationPct - MIN_CONCENTRATION_PERCENT);
  }

  return fmaxf(
    0.0f,
    (currentConcentrationPct - MIN_CONCENTRATION_PERCENT) /
      static_cast<float>(weeksRemaining)
  );
}

void resetQuitPlan(uint8_t requestedWeeks) {
  quitPlanWeeks = static_cast<uint8_t>(
    constrain(requestedWeeks, MIN_QUIT_PLAN_WEEKS, MAX_QUIT_PLAN_WEEKS)
  );

  currentTreatmentWeek = 1;
  currentConcentration = STARTING_CONCENTRATION_PERCENT;
  clinicianOverridePct = -1.0f;
  cumulativeMoneySavedUsd = 0.0f;
  relapseEventLatched = false;
  lastMilestoneBucket = 0;
  pendingDose = {};

  dashboard.currentConcentrationPct = STARTING_CONCENTRATION_PERCENT;
  dashboard.nextConcentrationPct = STARTING_CONCENTRATION_PERCENT;
  dashboard.moneySavedUsd = 0.0f;
  dashboard.milestoneProgressPct = 0;
  dashboard.quitPlanWeeks = quitPlanWeeks;
  dashboard.currentTreatmentWeek = currentTreatmentWeek;
  dashboard.plannedWeeklyStepPct =
    computePlannedWeeklyStep(STARTING_CONCENTRATION_PERCENT);

  clearAppSessionScratchpad();
  resetAppSession();

  Serial.print("Quit plan reset to ");
  Serial.print(quitPlanWeeks);
  Serial.println(" weeks. Restarting at week 1 and 5.0% nicotine.");
  logToBlynkTerminal(
    String("Quit plan reset to ") + String(static_cast<int>(quitPlanWeeks)) +
    " weeks. Week 1 now starts at 5.0%."
  );
}

// milestone progress is also represented as a simple percent for the app
uint8_t computeMilestoneProgress(float appliedConcentrationPct) {
  const float taperSpan =
    STARTING_CONCENTRATION_PERCENT - MIN_CONCENTRATION_PERCENT;
  const float taperedAmount =
    STARTING_CONCENTRATION_PERCENT - appliedConcentrationPct;
  const float progressFraction = constrain(taperedAmount / taperSpan, 0.0f, 1.0f);
  return static_cast<uint8_t>(roundf(progressFraction * 100.0f));
}

void maybeLogBlynkEvents(
  bool highRelapseRisk,
  bool safetyClampApplied,
  uint8_t scenarioCode,
  uint8_t triggerCode,
  uint8_t milestoneProgressPct
) {
  if (!Blynk.connected()) {
    return;
  }

  if (highRelapseRisk && !relapseEventLatched) {
    Blynk.logEvent("relapse_risk_high",
                   String("Scenario ") + String(static_cast<int>(scenarioCode)) +
                   ", trigger " + String(static_cast<int>(triggerCode)));
    Blynk.logEvent("friend_alert_requested",
                   String("Support alert requested for trigger ") +
                   String(static_cast<int>(triggerCode)));
    relapseEventLatched = true;
  }

  if (!highRelapseRisk) {
    relapseEventLatched = false;
  }

  if (safetyClampApplied) {
    Blynk.logEvent("dose_clamped", "Safety clamp applied to keep dose in range.");
  }

  const uint8_t milestoneBucket = milestoneProgressPct / 25;
  if (milestoneBucket > lastMilestoneBucket && milestoneBucket > 0) {
    Blynk.logEvent("milestone_reached",
                   String("Taper milestone reached: ") +
                   (milestoneBucket * 25) + "%");
    lastMilestoneBucket = milestoneBucket;
  }
}

// radio receive callback:
// copy the packet into a shared buf and return quickly
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

// pull one waiting session packet out of the shared buf
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

// send a short summary back to the handheld for user-facing feedback
void sendAck(const uint8_t *destinationMac,
             uint32_t sessionId,
             float adaptiveMultiplier,
             float appliedConcentration,
             const char *scenarioId) {
  if (!ensurePeerRegistered(destinationMac)) {
    Serial.println("Failed to register handheld peer for ACK.");
    return;
  }

  SyncAck ack = {};
  ack.accepted = true;
  ack.sessionId = sessionId;
  ack.adaptiveMultiplier = adaptiveMultiplier;
  ack.appliedConcentration = appliedConcentration;
  snprintf(ack.scenarioId, sizeof(ack.scenarioId), "%s", scenarioId);

  esp_now_send(destinationMac, reinterpret_cast<uint8_t *>(&ack), sizeof(ack));
}

void stageHandheldSessionForSurvey(const SessionData &data, const uint8_t *senderMac) {
  if (pendingDose.ready) {
    Serial.println(
      "Handheld session received, but a dose is already waiting. Finish or reset the current dose first."
    );
    sendAck(senderMac, data.sessionId, 0.0f, dashboard.nextConcentrationPct, "BUS");
    return;
  }

  if (data.totalPuffs == 0 && data.totalDuration < 0.05f) {
    Serial.println("Handheld session ignored because it had no puff metrics.");
    sendAck(senderMac, data.sessionId, 0.0f, currentConcentration, "BAD");
    return;
  }

  clearAppSessionScratchpad();
  pendingDose = {};
  appSession.totalPuffs = data.totalPuffs;
  appSession.totalDuration = data.totalDuration;
  appSession.surveyScore = 0.0f;
  appSession.triggerCode = 0;
  appSession.sessionId = data.sessionId;

  dashboard.dailyPuffCount = appSession.totalPuffs;
  dashboard.dailyDurationSec = appSession.totalDuration;
  dashboard.surveyScore = appSession.surveyScore;
  dashboard.triggerCode = appSession.triggerCode;
  publishTelemetryToBlynk();

  Serial.println();
  Serial.println("--- HANDHELD PUFF METRICS RECEIVED ---");
  Serial.print("Handheld MAC: ");
  printMacAddress(senderMac);
  Serial.println();
  Serial.print("Session ID: ");
  Serial.println(data.sessionId);
  Serial.print("Puffs: ");
  Serial.println(data.totalPuffs);
  Serial.print("Duration (s): ");
  Serial.println(data.totalDuration, 1);
  Serial.println("Survey will be collected in FadeX Chatbot before dose calculation.");

  sendAck(senderMac, data.sessionId, 0.0f, currentConcentration, "ASK");
  logToBlynkTerminal(
    String("Handheld puff metrics received: puffs=") +
    String(data.totalPuffs) + ", duration=" +
    String(data.totalDuration, 1) + " s."
  );
  beginPreDoseSurveyFlow();
}

// print the whole decision process to serial monitor for demos and debugging
void printSessionSummary(
  const SessionData &data,
  const char *scenarioId,
  float mu,
  float adaptiveMultiplier,
  float preliminaryConcentration,
  float recommendedConcentration,
  float appliedConcentration,
  bool overrideApplied,
  bool highRelapseRisk
) {
  Serial.println("\n--- FADEX SESSION ---");
  Serial.print("Session ID: ");
  Serial.println(data.sessionId);
  Serial.print("Puffs: ");
  Serial.println(data.totalPuffs);
  Serial.print("Duration (s): ");
  Serial.println(data.totalDuration, 1);
  Serial.print("Treatment week: ");
  Serial.print(currentTreatmentWeek);
  Serial.print(" / ");
  Serial.println(quitPlanWeeks);
  Serial.print("Survey score: ");
  Serial.println(data.surveyScore, 2);
  Serial.print("Survey label: ");
  Serial.println(surveyLabel(data.surveyScore));
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
  Serial.print("Recommended C_next (%): ");
  Serial.println(recommendedConcentration, 2);
  Serial.print("Applied C_next (%): ");
  Serial.println(appliedConcentration, 2);
  Serial.print("Clinician override applied: ");
  Serial.println(overrideApplied ? "YES" : "NO");
  Serial.print("High relapse risk: ");
  Serial.println(highRelapseRisk ? "YES" : "NO");
}

void publishTelemetryToBridge() {
  if (!USE_BLYNK_USB_BRIDGE) {
    return;
  }

  Serial.print("BRIDGE_BATCH ");
  Serial.print("v0=");
  Serial.print(dashboard.currentConcentrationPct, 2);
  Serial.print(" v1=");
  Serial.print(dashboard.nextConcentrationPct, 2);
  Serial.print(" v2=");
  Serial.print(static_cast<int>(dashboard.dailyPuffCount));
  Serial.print(" v3=");
  Serial.print(dashboard.dailyDurationSec, 2);
  Serial.print(" v4=");
  Serial.print(dashboard.surveyScore, 2);
  Serial.print(" v5=");
  Serial.print(static_cast<int>(dashboard.triggerCode));
  Serial.print(" v6=");
  Serial.print(static_cast<int>(dashboard.scenarioCode));
  Serial.print(" v7=");
  Serial.print(dashboard.relapseRiskHigh ? 1 : 0);
  Serial.print(" v8=");
  Serial.print(dashboard.nicotineVolumeMl, 2);
  Serial.print(" v9=");
  Serial.print(dashboard.diluentVolumeMl, 2);
  Serial.print(" v10=");
  Serial.print(dashboard.moneySavedUsd, 2);
  Serial.print(" v11=");
  Serial.print(static_cast<int>(dashboard.milestoneProgressPct));
  Serial.print(" v16=");
  Serial.print(static_cast<int>(dashboard.quitPlanWeeks));
  Serial.print(" v19=");
  Serial.print(static_cast<int>(dashboard.quitPlanWeeks));
  Serial.print(" v20=");
  Serial.print(static_cast<int>(dashboard.currentTreatmentWeek));
  Serial.print(" v21=");
  Serial.println(dashboard.plannedWeeklyStepPct, 3);
}

void publishTelemetryToBlynk() {
  publishTelemetryToBridge();

  if (!Blynk.connected()) {
    return;
  }

  Blynk.virtualWrite(V0, dashboard.currentConcentrationPct);
  Blynk.virtualWrite(V1, dashboard.nextConcentrationPct);
  Blynk.virtualWrite(V2, static_cast<int>(dashboard.dailyPuffCount));
  Blynk.virtualWrite(V3, dashboard.dailyDurationSec);
  Blynk.virtualWrite(V4, dashboard.surveyScore);
  Blynk.virtualWrite(V5, static_cast<int>(dashboard.triggerCode));
  Blynk.virtualWrite(V6, static_cast<int>(dashboard.scenarioCode));
  Blynk.virtualWrite(V7, dashboard.relapseRiskHigh ? 1 : 0);
  Blynk.virtualWrite(V8, dashboard.nicotineVolumeMl);
  Blynk.virtualWrite(V9, dashboard.diluentVolumeMl);
  Blynk.virtualWrite(V10, dashboard.moneySavedUsd);
  Blynk.virtualWrite(V11, static_cast<int>(dashboard.milestoneProgressPct));
  Blynk.virtualWrite(V16, static_cast<int>(dashboard.quitPlanWeeks));
  Blynk.virtualWrite(V19, static_cast<int>(dashboard.quitPlanWeeks));
  Blynk.virtualWrite(V20, static_cast<int>(dashboard.currentTreatmentWeek));
  Blynk.virtualWrite(V21, dashboard.plannedWeeklyStepPct);
  publishUncsTelemetry();
}

// Core algorithm:
// 1. Determine usage tier from duration
// 2. Determine survey tier from self-report
// 3. Pick one of 16 scenario IDs
// 4. Compute the adaptive multiplier A
// 5. Compute a preliminary next concentration
// 6. Clamp the concentration to the safe range
// 7. Optionally apply a clinician override from Blynk
// 8. Convert the result into pump run times and execute the pumps
// 9. Publish the result to Blynk
void processSession(const SessionData &data, const uint8_t *senderMac) {
  if (pendingDose.ready) {
    Serial.println("A calculated dose is already waiting. Press Run Dose in Blynk before syncing again.");
    return;
  }

  const float normalizedSurveyScore = snapSurveyScore(data.surveyScore);
  const float concentrationBeforeSession = currentConcentration;
  const uint8_t usageTier = classifyUsageTier(data.totalDuration);
  const float mu = usageMultiplierForTier(usageTier);
  const uint8_t surveyTier = classifySurveyTier(normalizedSurveyScore);
  const char *scenarioId = SCENARIO_IDS[usageTier][surveyTier];
  const uint8_t scenarioCode = scenarioCodeForTiers(usageTier, surveyTier);
  const float plannedWeeklyStepPct = computePlannedWeeklyStep(currentConcentration);
  const int finalTransitionWeek =
    static_cast<int>(quitPlanWeeks) - 1;

  const float adaptiveMultiplier = 1.0f + normalizedSurveyScore + (1.0f - mu);
  float preliminaryConcentration =
    currentConcentration - (plannedWeeklyStepPct * adaptiveMultiplier);

  // The selected quit plan defines the final calendar week for reaching the
  // minimum dose. Adaptive logic can speed up or slow down earlier weeks, but
  // the last scheduled transition still lands on 0.5%.
  if (static_cast<int>(currentTreatmentWeek) >= finalTransitionWeek) {
    preliminaryConcentration = MIN_CONCENTRATION_PERCENT;
  }

  const float recommendedConcentration = clampDose(preliminaryConcentration);

  float appliedConcentration = recommendedConcentration;
  bool overrideApplied = false;

  if (clinicianOverridePct >= MIN_CONCENTRATION_PERCENT &&
      clinicianOverridePct <= MAX_CONCENTRATION_PERCENT) {
    appliedConcentration = clampDose(clinicianOverridePct);
    overrideApplied = true;
  }

  // convert the final concentration target into actual liquid volumes
  const float nicotineVolumeMl =
    (appliedConcentration / MAX_CONCENTRATION_PERCENT) * TOTAL_VOLUME_ML;
  const float diluentVolumeMl = TOTAL_VOLUME_ML - nicotineVolumeMl;

  // convert volume targets into pump run times using the calibrated flow rates
  const unsigned long nicotineRunMs =
    static_cast<unsigned long>((nicotineVolumeMl / NIC_ML_PER_SEC) * 1000.0f);
  const unsigned long diluentRunMs =
    static_cast<unsigned long>((diluentVolumeMl / DIL_ML_PER_SEC) * 1000.0f);

  // simple first-pass flag for the app/support layer
  const bool highRelapseRisk =
    (surveyTier >= 2 && usageTier >= 2) ||
    (appliedConcentration > concentrationBeforeSession);

  const bool safetyClampApplied =
    (recommendedConcentration != preliminaryConcentration);

  const float sessionMoneySavedUsd =
    estimateSessionMoneySaved(appliedConcentration, data.totalDuration);
  cumulativeMoneySavedUsd += sessionMoneySavedUsd;

  SessionData normalizedSession = data;
  normalizedSession.surveyScore = normalizedSurveyScore;

  printSessionSummary(
    normalizedSession,
    scenarioId,
    mu,
    adaptiveMultiplier,
    preliminaryConcentration,
    recommendedConcentration,
    appliedConcentration,
    overrideApplied,
    highRelapseRisk
  );
  logToBlynkTerminal(
    String(senderMac != nullptr ? "Handheld" : "App") +
    " session received: puffs=" + String(normalizedSession.totalPuffs) +
    ", duration=" + String(normalizedSession.totalDuration, 1) +
    " s, survey=" + surveyLabel(normalizedSession.surveyScore) +
    ", trigger=" + triggerLabel(normalizedSession.triggerCode)
  );

  Serial.println("Applying safety clamp range 0.5% to 5.0%");

  // Send handheld feedback as soon as the algorithm has accepted the session.
  // App-only Blynk sessions pass nullptr because there is no handheld to ACK.
  if (senderMac != nullptr) {
    sendAck(senderMac, data.sessionId, adaptiveMultiplier, appliedConcentration, scenarioId);
  }

  pendingDose.ready = true;
  pendingDose.sessionId = data.sessionId;
  pendingDose.appliedConcentrationPct = appliedConcentration;
  pendingDose.nicotineVolumeMl = nicotineVolumeMl;
  pendingDose.diluentVolumeMl = diluentVolumeMl;
  pendingDose.nicotineRunMs = nicotineRunMs;
  pendingDose.diluentRunMs = diluentRunMs;

  // refresh the app-facing state after calculation. The physical pump run will
  // start only after the Blynk Run Dose button is pressed.
  dashboard.currentConcentrationPct = concentrationBeforeSession;
  dashboard.nextConcentrationPct = appliedConcentration;
  dashboard.dailyPuffCount = data.totalPuffs;
  dashboard.dailyDurationSec = data.totalDuration;
  dashboard.surveyScore = normalizedSurveyScore;
  dashboard.triggerCode = data.triggerCode;
  dashboard.scenarioCode = scenarioCode;
  dashboard.relapseRiskHigh = highRelapseRisk;
  dashboard.nicotineVolumeMl = nicotineVolumeMl;
  dashboard.diluentVolumeMl = diluentVolumeMl;
  dashboard.moneySavedUsd = cumulativeMoneySavedUsd;
  dashboard.milestoneProgressPct = computeMilestoneProgress(appliedConcentration);
  dashboard.quitPlanWeeks = quitPlanWeeks;
  dashboard.currentTreatmentWeek = currentTreatmentWeek;
  dashboard.plannedWeeklyStepPct = plannedWeeklyStepPct;

  if (senderMac != nullptr) {
    clearAppSessionScratchpad();
  }

  maybeLogBlynkEvents(
    highRelapseRisk,
    safetyClampApplied,
    scenarioCode,
    data.triggerCode,
    dashboard.milestoneProgressPct
  );
  publishTelemetryToBlynk();
  Serial.print("Session money saved estimate ($): ");
  Serial.println(sessionMoneySavedUsd, 2);
  Serial.print("Cumulative money saved ($): ");
  Serial.println(cumulativeMoneySavedUsd, 2);
  Serial.print("Quit plan (weeks): ");
  Serial.println(quitPlanWeeks);
  Serial.print("Planned weekly step (%): ");
  Serial.println(plannedWeeklyStepPct, 3);
  if (senderMac != nullptr) {
    Serial.println("Dose calculated and waiting. Press Run Dose in Blynk to start pumps.");
  } else {
    Serial.println("Dose calculated from app session.");
  }
  logToBlynkTerminal(
    String("Dose ready: scenario ") + scenarioId +
    ", next conc=" + String(appliedConcentration, 2) +
    "%, nic=" + String(nicotineVolumeMl, 2) +
    " mL, dil=" + String(diluentVolumeMl, 2) + " mL"
  );
}

// calibration command:
// run one pump for 30 seconds so you can measure actual output and update
// the mL/sec constants above
void runCalibrationCommand(const PumpChannel &pump) {
  pumpStopRequested = false;
  Serial.print("Calibrating ");
  Serial.print(pump.name);
  Serial.println(" pump for 30 seconds...");
  runPump(pump, 30000);
  Serial.println("Measure output volume and update *_ML_PER_SEC.");
}

void printLocalConfig() {
  Serial.println();
  Serial.println("FadeX final homebase with Blynk");
  Serial.println("PCB pump pins:");
  Serial.println("  Pump 1 (Nicotine): IN1 GPIO22, IN2 GPIO21");
  Serial.println("  Pump 2 (Diluent):  IN1 GPIO19, IN2 GPIO18");
  Serial.println("  Pump 3 (Mixing):   IN1 GPIO33, IN2 GPIO4");
  Serial.println("PCB load-cell pins:");
  Serial.println("  HX711 SCLK: GPIO23");
  Serial.println("  LCA1 DATA: GPIO25");
  Serial.println("  LCA2 DATA: GPIO26");
  Serial.println("  LCA3 DATA: GPIO27");
  Serial.println("  LCA4 DATA: GPIO13");
  Serial.println();
  Serial.println("Virtual Pin map:");
  Serial.println("  V0  Current Concentration");
  Serial.println("  V1  Next Concentration");
  Serial.println("  V2  Daily Puff Count");
  Serial.println("  V3  Daily Duration");
  Serial.println("  V4  Survey Score");
  Serial.println("  V5  Trigger Code");
  Serial.println("  V6  Scenario Code");
  Serial.println("  V7  Relapse Risk Flag");
  Serial.println("  V8  Nicotine Volume");
  Serial.println("  V9  Diluent Volume");
  Serial.println("  V10 Money Saved");
  Serial.println("  V11 Milestone Progress");
  Serial.println("  V12 Clinician Override");
  Serial.println("  V15 Blynk Puff Button");
  Serial.println("  V16 Quit Plan Weeks");
  Serial.println("  V17 Blynk Trigger Code (optional alias)");
  Serial.println("  V18 Reset Blynk Session");
  Serial.println("  V19 Quit Plan Weeks (optional alternate)");
  Serial.println("  V20 Current Treatment Week");
  Serial.println("  V21 Planned Weekly Step");
  Serial.println("  V22 Debug Terminal");
  Serial.println("  V23 Current Chatbot Prompt");
  Serial.println("  V24 Clear Chatbot Terminal");
  Serial.println("  V25 UNCS Dark Voltage (optional)");
  Serial.println("  V26 UNCS Blank Voltage (optional)");
  Serial.println("  V27 UNCS Sample Voltage (optional)");
  Serial.println("  V28 UNCS Vitamin C Concentration mM (optional)");
  Serial.println();
  Serial.println("UNCS optical sensor:");
  Serial.println("  PDO photodiode ADC: GPIO36");
  Serial.println("  UV LED PWM: GPIO32 jumper to UVLEDPWM");
  Serial.println("  Emergency stop commands: STOP, ESTOP, ABORT, or S");
  Serial.println("  Commands: UV ON, UV OFF, UV DUTY <0-100>, UV READ, UV STREAM ON/OFF");
  Serial.println("            UNCS DARK, UNCS BLANK, UNCS SAMPLE, UNCS STATUS");
  Serial.println("            UNCS CIRC <ms> runs Pump 3 toward the UNCS sensor path");
  Serial.println("            UNCS TARGET <0-5%> trims surrogate to nicotine-equivalent mM");
  Serial.println("            UNCS TARGETMM <mM> trims surrogate using a direct mM target");
  Serial.print("  Surrogate scale: 5.0% nicotine-equivalent = ");
  Serial.print(UNCS_SURROGATE_MAX_MM, 1);
  Serial.println(" mM");
  Serial.println();
  Serial.println("Demo mode:");
  Serial.println("  Vape PCB is optional right now; Blynk Puff Button / Chatbot can supply usage data.");
  Serial.println("  Final dose delivery uses LCA3 mixing-reservoir weight, not noisy source-loss control.");
  Serial.print("  Expected handheld ESP-NOW MAC: ");
  printMacAddress(EXPECTED_HANDHELD_MAC);
  Serial.println();
  Serial.print("  Serial-only ESP-NOW channel: ");
  Serial.println(static_cast<int>(ESPNOW_WIFI_CHANNEL));
  Serial.println();
  Serial.print("Survey choices: ");
  Serial.println("0.5 Better | 0 Neutral | -0.5 Struggling | -1.0 Worse");
  Serial.print("Money saved model: baseline $");
  Serial.print(BASELINE_DAILY_COST_USD, 2);
  Serial.print("/day scaled by session duration relative to ");
  Serial.print(BASELINE_DAILY_USAGE_SECONDS, 0);
  Serial.println(" s baseline usage");
  Serial.println();
}

void executeConsoleCommand(String command, bool fromBlynkTerminal) {
  command.trim();
  if (command.isEmpty()) {
    return;
  }

  String upper = command;
  upper.toUpperCase();

  if (isStopCommand(upper)) {
    pumpStopRequested = true;
    stopAllPumps();
    Serial.println("EMERGENCY STOP: all pumps stopped.");
    logToBlynkTerminal("EMERGENCY STOP: all pumps stopped.");
    return;
  }

  if (upper.startsWith("UNCS") ||
      upper.startsWith("UV") ||
      upper == "ON" ||
      upper == "OFF" ||
      upper == "LED ON" ||
      upper == "LED OFF" ||
      upper == "PRINT" ||
      upper == "PRINT TOGGLE" ||
      upper == "CIRC" ||
      upper == "PUMP3" ||
      upper.startsWith("CIRC ") ||
      upper.startsWith("PUMP3 ") ||
      upper.startsWith("TARGET ") ||
      upper.startsWith("TRIM ") ||
      upper.startsWith("TARGETMM ") ||
      upper.startsWith("DUTY ") ||
      upper.startsWith("PWM ")) {
    executeUncsCommand(command, fromBlynkTerminal);
    return;
  }

  if (upper == "HELP" || upper == "?") {
    const String helpText =
      "FadeX flow: record puffs, then press Run Dose. "
      "Week 1 asks check-in questions after puff metrics to calculate week 2. "
      "Later weeks also ask only after puff metrics. "
      "Helpful commands: STATUS, PLAN <weeks>, PUFF <sec>, "
      "MIXONE <pump> <mL>, MIXTEST <nic> <dil>, UNCS TARGET <0-5%>, "
      "UNCS CIRC <ms>, "
      "CLEARCHAT, STOP/ESTOP/ABORT, UV HELP, UNCS HELP.";
    Serial.println(helpText);
    if (fromBlynkTerminal || SERIAL_CHATBOT_ONLY) {
      logToBlynkTerminal(helpText);
    }
    return;
  }

  if (upper == "STATUS" || upper == "WHAT'S GOING ON" || upper == "WHATS GOING ON") {
    const String statusLine = appSessionStatusLine();
    Serial.println(statusLine);
    Serial.print("Pending clinician override (%): ");
    Serial.println(clinicianOverridePct, 2);
    Serial.println(uncsStatusLine());
    if (fromBlynkTerminal || SERIAL_CHATBOT_ONLY) {
      String terminalStatus =
        statusLine + "\n" +
        String("Override=") + String(clinicianOverridePct, 2) +
        "% | guided state=" + guidedStateLabel(guidedTerminalState) +
        "\n" + uncsStatusLine();
      renderBlynkTerminalText(terminalStatus);
    }
    printLoadCellReadings();
    return;
  }

  if (upper == "CLEAR" || upper == "CLEARCHAT" || upper == "CLEAR CHAT") {
    clearBlynkTerminal();
    showChatbotWelcome();
    return;
  }

  if (upper == "START" || upper == "BEGIN" || upper == "NEW SESSION") {
    beginGuidedTerminalSession();
    return;
  }

  if ((upper == "CONTINUE" || upper == "KEEP" || upper == "KEEPPLAN") &&
      guidedTerminalState == TERMINAL_WAITING_PLAN_DECISION) {
    logToBlynkTerminal(
      String("Keeping the current ") +
      String(static_cast<int>(quitPlanWeeks)) + "-week quit plan."
    );
    if (!appSessionHasPuffMetrics()) {
      promptForPuffMetricsBeforeDose();
      return;
    }
    guidedTerminalState = TERMINAL_WAITING_SURVEY;
    promptGuidedTerminalState();
    return;
  }

  if (guidedTerminalState == TERMINAL_WAITING_SURVEY) {
    float surveyScore = 0.0f;
    if (parseSurveyText(upper, surveyScore)) {
      setAppSurveyScore(surveyScore);
      if (surveyScore <= -0.5f) {
        guidedTerminalState = TERMINAL_WAITING_TRIGGER;
        promptGuidedTerminalState();
      } else {
        setAppTriggerCode(0);
        calculateAndRunAppDoseFromGuidedFlow();
      }
      return;
    }
  }

  if (guidedTerminalState == TERMINAL_WAITING_TRIGGER) {
    int triggerCode = 0;
    if (parseTriggerText(upper, triggerCode)) {
      setAppTriggerCode(triggerCode);
      calculateAndRunAppDoseFromGuidedFlow();
      return;
    }
  }

  if (upper.startsWith("PUFF ")) {
    const float puffSeconds = command.substring(5).toFloat();
    if (puffSeconds > 0.0f) {
      recordAppPuff(puffSeconds);
    } else if (fromBlynkTerminal || SERIAL_CHATBOT_ONLY) {
      logToBlynkTerminal("Use PUFF <seconds>, for example: PUFF 2.4");
    }
    return;
  }

  if (upper == "CAL1") {
    runCalibrationCommand(NICOTINE_PUMP);
  } else if (upper == "CAL2") {
    runCalibrationCommand(DILUENT_PUMP);
  } else if (upper == "CAL3") {
    Serial.println("CAL3 is disabled for this demo. Only Pump 1 and Pump 2 should run.");
    logToBlynkTerminal("CAL3 disabled: Pump 3 is not used in this demo.");
  } else if (upper == "PRIME") {
    runPrimeSequence();
  } else if (upper.startsWith("MIXTEST ")) {
    float nicMl = 0.0f;
    float dilMl = 0.0f;
    if (sscanf(command.c_str(), "%*s %f %f", &nicMl, &dilMl) == 2) {
      runMixingMassBalanceTest(nicMl, dilMl);
    } else {
      Serial.println("Use: MIXTEST <nic mL> <diluent mL>");
      logToBlynkTerminal("Use: MIXTEST <nic mL> <diluent mL>");
    }
  } else if (upper.startsWith("MIXONE ")) {
    int pumpNumber = 0;
    float targetMl = 0.0f;
    if (sscanf(command.c_str(), "%*s %d %f", &pumpNumber, &targetMl) == 2) {
      runSinglePumpMassBalanceTest(static_cast<uint8_t>(pumpNumber), targetMl);
    } else {
      Serial.println("Use: MIXONE <pump 1-2> <mL>");
      logToBlynkTerminal("Use: MIXONE <pump 1-2> <mL>");
    }
  } else if (upper == "RUNDOSE" || upper == "RUN") {
    const bool runCommandAllowed =
      guidedTerminalState == TERMINAL_IDLE ||
      guidedTerminalState == TERMINAL_WAITING_RUN ||
      (guidedTerminalState == TERMINAL_WAITING_PUFFS && appSessionHasPuffMetrics());

    if (!runCommandAllowed) {
      if (fromBlynkTerminal || SERIAL_CHATBOT_ONLY) {
        renderBlynkTerminalText(
          "We are not ready to run yet.\n"
          "Please finish the current check-in step first.\n"
          "Current step: " + String(guidedStateLabel(guidedTerminalState)) +
          "\n" + appSessionStatusLine()
        );
      }
      return;
    }

    if (pendingDose.ready) {
      executePendingDose();
    } else {
      beginPreDoseSurveyFlow();
    }
  } else if (upper == "LOADS") {
    printLoadCellReadings();
  } else if (upper == "TARE") {
    tareLoadCells(true);
  } else if (upper.startsWith("SURVEY ")) {
    setAppSurveyScore(command.substring(7).toFloat());
    logToBlynkTerminal(
      String("Survey set to ") + surveyLabel(appSession.surveyScore) +
      " (" + String(appSession.surveyScore, 1) + ")"
    );
  } else if (upper.startsWith("TRIGGER ")) {
    setAppTriggerCode(command.substring(8).toInt());
    logToBlynkTerminal(
      String("Trigger set to ") + triggerLabel(appSession.triggerCode)
    );
  } else if (upper == "RESETSESSION" || upper == "RESET SESSION") {
    resetAppSession();
    guidedTerminalState = TERMINAL_IDLE;
    logToBlynkTerminal("App session reset.");
  } else if (upper == "CLEAROVERRIDE") {
    clinicianOverridePct = -1.0f;
    Serial.println("Clinician override cleared.");
    logToBlynkTerminal("Clinician override cleared.");
  } else if (upper.startsWith("PLAN ")) {
    int requestedWeeks = command.substring(5).toInt();
    resetQuitPlan(static_cast<uint8_t>(requestedWeeks));
    publishTelemetryToBlynk();
    if (guidedTerminalState == TERMINAL_WAITING_PLAN_DECISION) {
      guidedTerminalState = TERMINAL_WAITING_SURVEY;
      promptGuidedTerminalState();
    }
  } else if (upper.startsWith("OVERRIDE ")) {
    float requestedOverridePct = command.substring(9).toFloat();
    if (requestedOverridePct <= 0.0f) {
      clinicianOverridePct = -1.0f;
      Serial.println("Clinician override cleared.");
      logToBlynkTerminal("Clinician override cleared.");
    } else if (requestedOverridePct < MIN_CONCENTRATION_PERCENT ||
               requestedOverridePct > MAX_CONCENTRATION_PERCENT) {
      Serial.println("Override outside safe range.");
      logToBlynkTerminal("Override outside safe range.");
    } else {
      clinicianOverridePct = requestedOverridePct;
      Serial.print("Clinician override set to ");
      Serial.println(clinicianOverridePct, 2);
      logToBlynkTerminal(
        String("Clinician override set to ") + String(clinicianOverridePct, 2) + "%"
      );
    }
    if (USE_BLYNK_WIFI && Blynk.connected()) {
      Blynk.virtualWrite(V12, clinicianOverridePct > 0.0f ? clinicianOverridePct : 0);
    }
    publishTelemetryToBlynk();
  } else {
    const String helpText =
      "I didn’t understand that. Type HELP for the guided flow or STATUS for the current state.";
    Serial.println(helpText);
    if (fromBlynkTerminal || SERIAL_CHATBOT_ONLY) {
      logToBlynkTerminal(helpText);
      if (guidedTerminalState != TERMINAL_IDLE) {
        promptGuidedTerminalState();
      }
    }
  }
}

// small set of manual commands for debugging and calibration
void handleSerialCommands() {
  if (Serial.available() <= 0) {
    return;
  }

  String cmd = Serial.readStringUntil('\n');
  executeConsoleCommand(cmd, false);
}

bool waitForWifiConnection(unsigned long timeoutMs) {
  const unsigned long startMs = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startMs < timeoutMs) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

bool connectEnterpriseWifi() {
  Serial.println("Wi-Fi mode: WPA2-Enterprise / eduroam");
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  esp_wifi_sta_wpa2_ent_set_identity(
    reinterpret_cast<uint8_t *>(enterpriseIdentity),
    strlen(enterpriseIdentity)
  );

  esp_wifi_sta_wpa2_ent_set_username(
    reinterpret_cast<uint8_t *>(enterpriseUsername),
    strlen(enterpriseUsername)
  );

  esp_wifi_sta_wpa2_ent_set_password(
    reinterpret_cast<uint8_t *>(enterprisePassword),
    strlen(enterprisePassword)
  );

  esp_wifi_sta_wpa2_ent_enable();
  WiFi.begin(ssid);

  if (!waitForWifiConnection(30000)) {
    Serial.println("Enterprise Wi-Fi failed.");
    Serial.print("WiFi status code: ");
    Serial.println(WiFi.status());
    return false;
  }

  Serial.println("Enterprise Wi-Fi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Connected channel: ");
  Serial.println(WiFi.channel());
  return true;
}

bool connectPersonalWifi() {
  Serial.println("Wi-Fi mode: normal password / hotspot");
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, pass);

  if (!waitForWifiConnection(30000)) {
    Serial.println("Normal Wi-Fi failed.");
    Serial.print("WiFi status code: ");
    Serial.println(WiFi.status());
    return false;
  }

  Serial.println("Normal Wi-Fi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Connected channel: ");
  Serial.println(WiFi.channel());
  return true;
}

bool connectWifiForBlynk() {
  if (USE_ENTERPRISE_WIFI) {
    return connectEnterpriseWifi();
  }

  return connectPersonalWifi();
}

void startBlynkAfterWifi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping Blynk connect because Wi-Fi is not connected.");
    return;
  }

  Serial.println("Connecting to Blynk...");
  Blynk.config(BLYNK_AUTH_TOKEN);

  if (Blynk.connect(10000)) {
    Serial.println("Blynk connection started.");
  } else {
    Serial.println("Blynk did not connect yet. It will retry in Blynk.run().");
  }
}

#if FADEX_ENABLE_BLYNK_LIBRARY
BLYNK_CONNECTED() {
  Serial.println("Connected to Blynk.");
  clearBlynkTerminal();
  showChatbotWelcome();
  clinicianOverridePct = -1.0f;
  Blynk.virtualWrite(V12, 0);
  Blynk.virtualWrite(V14, 0);
  Blynk.virtualWrite(V15, 0);
  Blynk.virtualWrite(V18, 0);
  Blynk.syncVirtual(V4, V5, V16, V19);
  publishTelemetryToBlynk();
  publishUncsTelemetry();
}

BLYNK_WRITE(V4) {
  setAppSurveyScore(param.asFloat());
}

BLYNK_WRITE(V5) {
  setAppTriggerCode(param.asInt());
}

BLYNK_WRITE(V12) {
  const float requestedOverridePct = param.asFloat();

  if (requestedOverridePct <= 0.0f) {
    clinicianOverridePct = -1.0f;
    Serial.println("Clinician override cleared from Blynk.");
    logToBlynkTerminal("Clinician override cleared.");
  } else if (requestedOverridePct < MIN_CONCENTRATION_PERCENT ||
             requestedOverridePct > MAX_CONCENTRATION_PERCENT) {
    clinicianOverridePct = -1.0f;
    Serial.println("Clinician override outside safe range; cleared.");
    Blynk.virtualWrite(V12, 0);
    logToBlynkTerminal("Clinician override outside safe range; cleared.");
  } else {
    clinicianOverridePct = requestedOverridePct;
    Serial.print("Clinician override received: ");
    Serial.print(clinicianOverridePct);
    Serial.println("%");
    logToBlynkTerminal(
      String("Clinician override received: ") + String(clinicianOverridePct, 2) + "%"
    );
  }

  publishTelemetryToBlynk();
}

BLYNK_WRITE(V13) {
  const int command = param.asInt();
  if (command != 1) {
    primeRequested = false;
    return;
  }

  primeRequested = true;
  Serial.println("Prime command received: START");
  logToBlynkTerminal("Prime command received: START");
}

BLYNK_WRITE(V14) {
  const int command = param.asInt();
  if (command != 1) {
    doseStartRequested = false;
    return;
  }

  doseStartRequested = true;
  Serial.println("Run dose command received: START");
  logToBlynkTerminal("Run dose command received: START");
}

BLYNK_WRITE(V15) {
  const int command = param.asInt();

  if (command == 1 && !appPuffActive) {
    appPuffActive = true;
    appPuffStartMs = millis();
    Serial.println("Blynk puff START");
    logToBlynkTerminal("App puff START");
  } else if (command == 0 && appPuffActive) {
    appPuffActive = false;
    const float durationSeconds = (millis() - appPuffStartMs) / 1000.0f;
    recordAppPuff(durationSeconds);
  }
}

BLYNK_WRITE(V16) {
  int requestedWeeks = param.asInt();
  requestedWeeks = constrain(requestedWeeks,
                             MIN_QUIT_PLAN_WEEKS,
                             MAX_QUIT_PLAN_WEEKS);

  if (requestedWeeks != quitPlanWeeks) {
    resetQuitPlan(static_cast<uint8_t>(requestedWeeks));
  } else {
    dashboard.quitPlanWeeks = quitPlanWeeks;
    dashboard.currentTreatmentWeek = currentTreatmentWeek;
    dashboard.plannedWeeklyStepPct = computePlannedWeeklyStep(currentConcentration);
    Serial.println("Quit plan unchanged.");
  }

  publishTelemetryToBlynk();
}

BLYNK_WRITE(V17) {
  setAppTriggerCode(param.asInt());
}

BLYNK_WRITE(V18) {
  const int command = param.asInt();
  if (command == 1) {
    Serial.println("Resetting Blynk app session.");
    resetAppSession();
    Blynk.virtualWrite(V18, 0);
  }
}

BLYNK_WRITE(V19) {
  int requestedWeeks = param.asInt();

  if (requestedWeeks < MIN_QUIT_PLAN_WEEKS) {
    requestedWeeks = quitPlanWeeks;
    Blynk.virtualWrite(V19, static_cast<int>(quitPlanWeeks));
  }

  requestedWeeks = constrain(requestedWeeks,
                             MIN_QUIT_PLAN_WEEKS,
                             MAX_QUIT_PLAN_WEEKS);

  if (requestedWeeks != quitPlanWeeks) {
    resetQuitPlan(static_cast<uint8_t>(requestedWeeks));
  } else {
    dashboard.quitPlanWeeks = quitPlanWeeks;
    dashboard.currentTreatmentWeek = currentTreatmentWeek;
    dashboard.plannedWeeklyStepPct = computePlannedWeeklyStep(currentConcentration);
    Serial.println("Quit plan unchanged.");
  }

  publishTelemetryToBlynk();
}

BLYNK_WRITE(V22) {
  String command = param.asStr();
  command.trim();
  if (command.isEmpty()) {
    return;
  }

  logToBlynkTerminal(String("> ") + command);
  executeConsoleCommand(command, true);
}

BLYNK_WRITE(V24) {
  const int command = param.asInt();
  if (command == 1) {
    clearBlynkTerminal();
    showChatbotWelcome();
    Blynk.virtualWrite(V24, 0);
  }
}
#endif

void processPendingPrimeRequest() {
  if (!primeRequested) {
    return;
  }

  primeRequested = false;
  runPrimeSequence();

  if (Blynk.connected()) {
    // reset the app-side button so it behaves like a one-shot action
    Blynk.virtualWrite(V13, 0);
  }
}

void processPendingDoseStartRequest() {
  if (!doseStartRequested) {
    return;
  }

  doseStartRequested = false;

  // If a dose is already calculated, Run Dose starts the pumps. Otherwise,
  // collect puff metrics and survey answers before calculating.
  if (pendingDose.ready) {
    executePendingDose();
  } else {
    beginPreDoseSurveyFlow();
  }

  if (Blynk.connected()) {
    Blynk.virtualWrite(V14, 0);
  }
}

// standard arduino setup:
// init the pumps, the Wi-Fi radio, Blynk, and ESP-NOW.
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(500);

  printLocalConfig();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  Serial.print("Homebase Wi-Fi STA MAC / ESP-NOW MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Pump mode: ");
  Serial.println(PUMPS_ENABLED ? "ENABLED" : "APP-ONLY DRY RUN (pumps disabled)");

  if (USE_BLYNK_WIFI) {
    if (connectWifiForBlynk()) {
      startBlynkAfterWifi();
    } else {
      esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
      Serial.println("Wi-Fi failed; Blynk will stay offline for now.");
      Serial.println("ESP-NOW fallback channel set from ESPNOW_WIFI_CHANNEL.");
    }
  } else if (USE_BLYNK_USB_BRIDGE) {
    esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.println("Blynk mode: USB SERIAL BRIDGE (ESP32 Wi-Fi disabled)");
    Serial.println("Run fadex_blynk_usb_bridge.py on the laptop to connect this board to Blynk.");
  } else {
    esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.println("Blynk mode: OFF");
    Serial.println("FadeX Chatbot will run in the Serial Monitor.");
  }

  setupPump(NICOTINE_PUMP);
  setupPump(DILUENT_PUMP);
  setupPump(MIXING_PUMP);
  setupLoadCells();
  setupUncsSensor();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);
  if (USE_BLYNK_WIFI || USE_BLYNK_USB_BRIDGE) {
    timer.setInterval(BLYNK_PUBLISH_INTERVAL_MS, publishTelemetryToBlynk);
  }
  if (USE_BLYNK_WIFI) {
    Serial.print("Homebase Wi-Fi channel: ");
    Serial.println(WiFi.channel());
    Serial.println("Handheld ESP-NOW channel must match this Wi-Fi channel.");
  } else {
    Serial.print("Homebase ESP-NOW channel: ");
    Serial.println(static_cast<int>(ESPNOW_WIFI_CHANNEL));
  }
  Serial.println("Homebase listening.");
  Serial.println(
    "Commands: CAL1, CAL2, MIXONE, MIXTEST, STATUS, LOADS, TARE, "
    "UV HELP, UNCS HELP, CLEAROVERRIDE, STOP"
  );

  if (SERIAL_CHATBOT_ONLY) {
    showChatbotWelcome();
  }
}

// main loop:
// keep Blynk alive, handle app commands, handle manual commands,
// then process any newly received session packet
void loop() {
  serviceCloud();
  processUncsSafetyAndStreaming();
  processPendingPrimeRequest();
  processPendingDoseStartRequest();
  handleSerialCommands();

  SessionData session = {};
  uint8_t senderMac[6] = {};
  if (fetchPendingSession(session, senderMac)) {
    stageHandheldSessionForSurvey(session, senderMac);
  }
}
