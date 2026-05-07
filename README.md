# FadeX - Automated Nicotine Tapering System

FadeX is an adaptive nicotine tapering prototype developed for ECE 445. The project explores a safer, feedback-based way to reduce nicotine concentration over time while preserving the user's familiar puff routine. Instead of forcing large manual concentration jumps, FadeX uses puff behavior, user check-ins, and embedded control logic to calculate a gradual taper and prepare a target mixture.

## Team Members

- **Malik Kelly** - Tapering algorithm, app/chatbot interface, ESP32 integration support
- **Justin Leith** - Project management, mechanical design, optical sensing support
- **Ian Zentner** - MCU logic, PCB design, hardware integration

## Final Prototype Overview

The final FadeX prototype centers on a **Homebase ESP32** that coordinates communication, dosing logic, pump control, load-cell feedback, and concentration sensing. A simplified **Handheld ESP32** is used to record puff activity and send puff-session data to the homebase. The system was demonstrated using a nicotine surrogate rather than real nicotine for safety.

The final architecture includes:

- **Homebase Controller:** ESP32-based controller that runs the tapering algorithm, receives puff data, controls pumps, reads load cells, and manages safety logic.
- **Handheld Puff Input:** Simplified ESP32 handheld with a puff button that records puff count/session duration and sends data to the homebase through ESP-NOW.
- **Fluid Delivery System:** Peristaltic pumps used to deliver concentrate/surrogate and diluent into a mixing reservoir.
- **Load-Cell Feedback:** HX711 load-cell amplifiers used to tare containers and verify delivered liquid mass.
- **UNCS Concentration Sensing:** UV LED and photodiode circuitry used to support concentration feedback for the vitamin C/ascorbic acid surrogate.
- **User Interface:** Blynk dashboard and/or serial-based FadeX Chatbot used to display system state, collect check-in responses, and guide the demo flow.

## Tapering Algorithm

FadeX calculates the next concentration using puff behavior, treatment week, user survey feedback, and safety limits. The algorithm combines objective usage data with simple self-report inputs so that the taper can slow down or stabilize when relapse risk appears higher.

Core algorithm ideas:

- Puff count and puff duration are collected from the handheld or demo input.
- The user check-in classifies the session as `Better`, `Neutral`, `Struggling`, or `Worse`.
- Trigger questions are only asked when the user reports `Struggling` or `Worse`.
- Scenario codes classify the current user state and guide tapering behavior.
- The next concentration is safety-clamped before pump volumes are calculated.
- Pump delivery is verified using load-cell feedback instead of relying only on timed pumping.

Example equations:

```text
A = 1.0 + S + (1.0 - Mu)
C_next = clamp(C_current - 0.28A, 0.5%, 5.0%)
V_concentrate = (C_next / 5.0) * V_total
V_diluent = V_total - V_concentrate
```

## Repository Structure

```text
/Code/              Firmware sketches, tests, and integration code
/Images/            Diagrams, presentation graphics, screenshots, and verification visuals
/Notebook_Entries/  Chronological lab notebook entries and engineering logs
/Hardware/          PCB schematics, pinout references, and hardware design files
/Software/          ESP32 firmware, app/Blynk code, and communication tests
/CAD/               Mechanical design files and housing concepts
/Datasheets/        Component datasheets and reference documents
```

Some folders may contain uploaded final report materials, figures, or archived versions of earlier designs.

## Key Subsystem Tests

The project was developed through modular tests before final integration:

- ESP32 programming and MAC address identification
- ESP-NOW handheld-to-homebase puff communication
- Pump direction, flow-rate, and volume delivery tests
- Load-cell calibration and tare verification
- Water-by-weight dispense testing
- UV LED PWM output testing
- Photodiode ADC readout testing
- Blynk dashboard and FadeX Chatbot workflow testing
- Final integrated dosing and demo flow testing

## Safety Notes

The course prototype used water and vitamin C/ascorbic acid surrogate solutions instead of nicotine for safety. The firmware and demo flow include safety-oriented behavior such as load-cell taring, staged pump pulses, maximum mixing-volume checks, and emergency stop commands. The homebase controller is responsible for safety-critical pump control rather than relying only on the app interface.

## Project Tracking and Engineering Notebook

- `/Notebook_Entries/` contains chronological lab notebook entries documenting objectives, design decisions, testing, debugging, and final outcomes.
- `/Images/` contains visuals used for reports, slides, flowcharts, and verification documentation.
- The GitHub Project/Kanban board was used during development to track subsystem progress and remaining demo tasks.

## Final Deliverables

Final project materials include the final paper, video assignment, lab notebook entries, subsystem test code, algorithm visuals, and final demo documentation.
