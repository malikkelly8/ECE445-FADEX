#define BLYNK_TEMPLATE_ID "TMPL_REPLACE_ME"
#define BLYNK_TEMPLATE_NAME "FadeX Homebase"
#define BLYNK_AUTH_TOKEN "REPLACE_ME"

#define BLYNK_PRINT Serial

#include <Arduino.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

// FadeX Blynk starter sketch
//
// Purpose:
// - get the homebase online with Blynk
// - publish FadeX-style telemetry to Virtual Pins
// - receive a clinician override concentration and a prime command
// - make dashboard and app development possible before the full firmware is
//   integrated end-to-end
//
// This sketch uses demo values by default so you can bring up the app even
// before all hardware is connected.

char ssid[] = "REPLACE_WITH_WIFI_SSID";
char pass[] = "REPLACE_WITH_WIFI_PASSWORD";

constexpr bool DEMO_MODE = true;
constexpr unsigned long PUBLISH_INTERVAL_MS = 5000;
constexpr unsigned long DEMO_STEP_INTERVAL_MS = 3000;

struct FadeXState {
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
};

FadeXState state = {
  5.0f,   // currentConcentrationPct
  4.7f,   // nextConcentrationPct
  32,     // dailyPuffCount
  280.0f, // dailyDurationSec
  0.0f,   // surveyScore
  0,      // triggerCode
  6,      // scenarioCode
  false,  // relapseRiskHigh
  28.2f,  // nicotineVolumeMl
  1.8f,   // diluentVolumeMl
  14.50f, // moneySavedUsd
  25      // milestoneProgressPct
};

float clinicianOverridePct = -1.0f;
bool primeRequested = false;
bool relapseEventLatched = false;
bool milestoneEventLatched = false;
uint8_t demoStep = 0;

BlynkTimer timer;

void printLocalConfig() {
  Serial.println();
  Serial.println("FadeX homebase Blynk starter");
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
  Serial.println("  V13 Prime Command");
  Serial.println();
}

void publishTelemetry() {
  Blynk.virtualWrite(V0, state.currentConcentrationPct);
  Blynk.virtualWrite(V1, state.nextConcentrationPct);
  Blynk.virtualWrite(V2, static_cast<int>(state.dailyPuffCount));
  Blynk.virtualWrite(V3, state.dailyDurationSec);
  Blynk.virtualWrite(V4, state.surveyScore);
  Blynk.virtualWrite(V5, static_cast<int>(state.triggerCode));
  Blynk.virtualWrite(V6, static_cast<int>(state.scenarioCode));
  Blynk.virtualWrite(V7, state.relapseRiskHigh ? 1 : 0);
  Blynk.virtualWrite(V8, state.nicotineVolumeMl);
  Blynk.virtualWrite(V9, state.diluentVolumeMl);
  Blynk.virtualWrite(V10, state.moneySavedUsd);
  Blynk.virtualWrite(V11, static_cast<int>(state.milestoneProgressPct));

  Serial.println("Published FadeX telemetry to Blynk.");
}

void maybeLogEvents() {
  if (state.relapseRiskHigh && !relapseEventLatched) {
    Blynk.logEvent("relapse_risk_high",
                   String("Scenario ") + state.scenarioCode +
                   ", trigger " + state.triggerCode);
    relapseEventLatched = true;
  }

  if (!state.relapseRiskHigh) {
    relapseEventLatched = false;
  }

  if (state.milestoneProgressPct >= 100 && !milestoneEventLatched) {
    Blynk.logEvent("milestone_reached",
                   String("Milestone completed. Money saved: $") +
                   state.moneySavedUsd);
    milestoneEventLatched = true;
  }

  if (state.milestoneProgressPct < 100) {
    milestoneEventLatched = false;
  }
}

void advanceDemoValues() {
  if (!DEMO_MODE) {
    return;
  }

  demoStep = (demoStep + 1) % 5;

  switch (demoStep) {
    case 0:
      state.dailyPuffCount = 28;
      state.dailyDurationSec = 240.0f;
      state.surveyScore = 0.0f;
      state.triggerCode = 0;
      state.scenarioCode = 6;
      state.relapseRiskHigh = false;
      state.moneySavedUsd = 15.0f;
      state.milestoneProgressPct = 30;
      break;
    case 1:
      state.dailyPuffCount = 34;
      state.dailyDurationSec = 310.0f;
      state.surveyScore = -0.5f;
      state.triggerCode = 1;
      state.scenarioCode = 7;
      state.relapseRiskHigh = false;
      state.moneySavedUsd = 16.0f;
      state.milestoneProgressPct = 35;
      break;
    case 2:
      state.dailyPuffCount = 41;
      state.dailyDurationSec = 420.0f;
      state.surveyScore = -1.0f;
      state.triggerCode = 4;
      state.scenarioCode = 8;
      state.relapseRiskHigh = true;
      state.moneySavedUsd = 17.25f;
      state.milestoneProgressPct = 45;
      break;
    case 3:
      state.dailyPuffCount = 25;
      state.dailyDurationSec = 210.0f;
      state.surveyScore = 0.5f;
      state.triggerCode = 0;
      state.scenarioCode = 1;
      state.relapseRiskHigh = false;
      state.moneySavedUsd = 20.75f;
      state.milestoneProgressPct = 60;
      break;
    default:
      state.dailyPuffCount = 18;
      state.dailyDurationSec = 150.0f;
      state.surveyScore = 0.5f;
      state.triggerCode = 0;
      state.scenarioCode = 1;
      state.relapseRiskHigh = false;
      state.moneySavedUsd = 25.50f;
      state.milestoneProgressPct = 100;
      break;
  }

  if (clinicianOverridePct >= 0.5f) {
    state.nextConcentrationPct = clinicianOverridePct;
  }

  maybeLogEvents();
  publishTelemetry();
}

BLYNK_CONNECTED() {
  Serial.println("Connected to Blynk.");

  // Pull down any app-side command state when the device reconnects.
  Blynk.syncVirtual(V12, V13);
  publishTelemetry();
}

BLYNK_WRITE(V12) {
  clinicianOverridePct = param.asFloat();
  state.nextConcentrationPct = clinicianOverridePct;

  Serial.print("Clinician override received: ");
  Serial.print(clinicianOverridePct);
  Serial.println("%");

  publishTelemetry();
}

BLYNK_WRITE(V13) {
  const int command = param.asInt();
  primeRequested = (command == 1);

  Serial.print("Prime command received: ");
  Serial.println(primeRequested ? "START" : "STOP");

  // In the full firmware this is where you would trigger the priming routine.
}

void setup() {
  Serial.begin(115200);
  delay(500);

  printLocalConfig();

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  timer.setInterval(PUBLISH_INTERVAL_MS, publishTelemetry);
  timer.setInterval(DEMO_STEP_INTERVAL_MS, advanceDemoValues);
}

void loop() {
  Blynk.run();
  timer.run();
}
