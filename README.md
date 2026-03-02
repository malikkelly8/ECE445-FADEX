# FadeX - Automated Nicotine Tapering System (ECE 445)
**Team Members:** 
* Malik Kelly (Algorithm Development & App Integration)
* Justin Leith (Project Management & Mechanical Design)
* Ian Zentner (MCU Logic & Hardware Integration)

## Project Overview
FadeX is an automated fluidic delivery system designed to obfuscate the nicotine tapering process. 
To bypass the high failure rates of standard nicotine replacement therapies, FadeX maintains the user's 
sensorimotor behavioral routine while delivering a computationally linear, micro-dosed nicotine reduction. 

The system utilizes a two-part architecture:
1. **Batch-Mixing Base Station:** An ESP32-driven fluidic mixing station utilizing stepper peristaltic pumps,
   closed-loop gravimetric load cells, and optical UVC verification to create customized nicotine/diluent ratios
   
2. **Handheld Vaporizer:** A passive, BLE-enabled vaporizer equipped with thermal interlocks and pressure sensors
   to track user behavior and safely deliver the custom daily dose.

## Project Tracking & Engineering Notebooks
* **[Link to GitHub Project Kanban Board]** - Real-time timeline mapped to our Week 7-15 schedule.
* **`/Notebook_Entries/`** - Chronological engineering logs for Malik, Justin, and Ian.

## Repository Structure
* `/Notebook_Entries/` - Daily design process logs, math models, and R&V test results.
* `/Hardware/` - Base Station and Handheld PCB schematics and layout files.
* `/Software/` - ESP32 firmware (pump PWM control, safety watchdogs) and BLE Mobile App code.
* `/CAD/` - 3D models for the Home Base acrylic housing and Handheld tank.
* `/Datasheets/` - Pertinent component manuals (e.g., IRLB3034 MOSFET, HX711, A4988).
* `/Images/` - Verification graphs, scope traces, and subsystem block diagrams.
