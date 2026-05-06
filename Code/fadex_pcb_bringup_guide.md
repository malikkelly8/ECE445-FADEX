# FadeX PCB Bring-Up Guide

Use this order so each subsystem is tested before the full algorithm runs.

## 1. ESP32 MAC / Identity Test

Sketch:
`fadex_esp32_mac_address/fadex_esp32_mac_address.ino`

Steps:

1. Plug in only USB first.
2. In Arduino IDE, select the ESP32 board and the visible `/dev/cu.usbserial-*` port.
3. Set upload speed to `115200` if upload is flaky.
4. Upload the sketch.
5. Open Serial Monitor at `115200` baud.
6. Copy the line labeled `Wi-Fi STA MAC / ESP-NOW MAC`.

Do this once for the homebase ESP32 and once for the handheld ESP32.

Why this matters:
The handheld sends ESP-NOW packets to the homebase station MAC, so the new
homebase MAC must replace the old `HOMEBASE_MAC[]` value in the handheld code.

Current recorded MAC addresses:

| Device | Wi-Fi STA / ESP-NOW MAC |
| --- | --- |
| Homebase PCB ESP32 | `88:13:BF:55:34:80` |
| Handheld vape ESP32 | `88:13:BF:55:35:F0` |

## 2. PCB Pump Driver Test

Sketch:
`fadex_pcb_pump_test/fadex_pcb_pump_test.ino`

PCB connector map:

| Pump | Connector | IN1 net / Arduino pin | IN2 net / Arduino pin |
| --- | --- | --- |
| Pump 1 | J7 | `3190.1.IN1` / GPIO22 | `3190.1.IN2` / GPIO21 |
| Pump 2 | J4 | `3190.2.IN1` / GPIO19 | `3190.2.IN2` / GPIO18 |
| Pump 3 | J9 | `3190.3.IN1` / GPIO33 | `3190.3.IN2` / GPIO4 |

Safe test steps:

1. Upload the sketch with pumps disconnected from fluid first if possible.
2. Connect the 12 V pump supply after upload.
3. Open Serial Monitor at `115200` baud.
4. Type `run 1 f 1.0` to run pump 1 forward for 1 second.
5. Type `run 2 f 1.0` to run pump 2 forward for 1 second.
6. Type `run 3 f 1.0` to run pump 3 forward for 1 second.
7. Type `stop` any time something looks wrong.

If a pump moves fluid the wrong way, that is wiring/polarity direction, not a
code failure. Reverse direction can be checked with `run 1 r 1.0`.

Note: pump 3 uses `GPIO4`, which is an ESP32 boot strapping pin. If upload
fails while the pump-3 driver is connected, disconnect that input during
programming and reconnect it after flashing.

## 3. Load Cell / HX711 Test

Sketch:
`fadex_hx711_round_robin_test/fadex_hx711_round_robin_test.ino`

PCB connector map:

| Channel | Connector | HX711 DOUT net / Arduino pin |
| --- | --- | --- |
| LCA1 | J2 | `DAT1` / GPIO25 |
| LCA2 | J10 | `DAT2` / GPIO26 |
| LCA3 | J13 | `DAT3` / GPIO27 |
| LCA4 | J5 | `DAT4` / GPIO13 |
| Shared SCLK | all four | `SCLK` / GPIO23 |

This sketch tares all four load cells, then asks for the `33.4 g` reference
weight on LCA1, LCA2, LCA3, and LCA4 in order.

## 4. Photodiode ADC Test

Sketch:
`fadex_photodiode_adc_test/fadex_photodiode_adc_test.ino`

PCB pin map:

| Signal | ESP32 Pin |
| --- | --- |
| PDO | GPIO36 / ADC1_CH0 |

Expected behavior:
The serial monitor prints raw ADC counts and estimated voltage. Shining light
or changing the photodiode input should move the reported voltage. Keep any
external test voltage between `0 V` and `3.3 V`.

## 4.5. Water Dose By Weight Test

Sketch:
`fadex_weight_dose_test/fadex_weight_dose_test.ino`

Purpose:
This is the first bridge between the pumps and the load cells. The sketch tares
an empty cup on a selected load cell, runs a selected pump, and stops when the
moving-average weight reaches the requested water volume.

Water assumption:
`1 mL water ~= 1 g`

Vibration handling:
The sketch does not trust load-cell readings while a pump is running. It tares
with the pumps off, runs one short pump burst, stops the pump, waits for the
scale to settle, then takes the moving-average reading.

Emergency stop:
Before every dose, all four load cells are tared with their cups/containers in
place. After every pump burst, the sketch checks all four load cells. If any
settled average is below `-0.50 g`, every pump is stopped and the dose is
aborted. This small negative threshold avoids false trips from normal load-cell
noise around zero.

Useful commands:

| Command | Meaning |
| --- | --- |
| `dose 20` | Pump 2 into LCA1 until about `20 g`, or `20 mL`, is measured |
| `dose 2 1 20` | Pump 2, load cell 1, target `20 mL` |
| `read 1` | Read LCA1 with the moving-average filter |
| `tare 1` | Tare LCA1 with the empty cup in place |
| `stop` | Abort an active dose |

## 4.6. Mixing Reservoir Mass-Balance Test

Sketch:
`fadex_mixing_mass_balance_test/fadex_mixing_mass_balance_test.ino`

Purpose:
This verifies that liquid leaving the nicotine/source and diluent/source
reservoirs appears in the mixing reservoir. For this test, both source liquids
can be water.

Default load-cell map:

| Load Cell | Role |
| --- | --- |
| LCA1 | Nicotine/source reservoir |
| LCA2 | Diluent/source reservoir |
| LCA3 | Mixing reservoir |
| LCA4 | Spare / optional |

The sketch tares all load cells before delivery with full source reservoirs and
an empty mixing reservoir already in place. Source reservoirs then read negative
as liquid leaves them. The code compares:

```text
total source mass lost ~= mixing reservoir mass gained
```

Useful commands:

| Command | Meaning |
| --- | --- |
| `MIXONE 1 20` | Run only Pump 1 until about `20 mL` leaves source 1 |
| `MIXONE 2 20` | Run only Pump 2 until about `20 mL` leaves source 2 |
| `MIXTEST 10 20` | Pump 10 mL from source 1 and 20 mL from source 2 |
| `MIXTEST 20 10` | Pump 20 mL from source 1 and 10 mL from source 2 |
| `MIXTEST 15 15` | Equal-volume source test |
| `READ` | Print settled weights from all load cells |
| `TARE` | Tare all load cells with reservoirs in place |
| `STOP` | Stop all pumps immediately |

During the test, the terminal prints `LIVE WEIGHTS` after every pump burst.
Those readings are taken only while all pumps are off, so the numbers are
slower but less distorted by motor vibration. `MIXTEST` still runs only one
pump at a time: Pump 1 finishes first, then Pump 2 starts.

If the ESP32 reboots during the test, check the first line printed after
startup. The sketch reports the last reset reason. If it says `brownout`, the
pump supply/noise is likely pulling the ESP32 power rail down.

Passing condition:
The default tolerance is `5%` of the requested total mass or `1.0 g`, whichever
is larger.

## 5. UV LED PWM Test

Sketch:
`fadex_uv_led_pwm_test/fadex_uv_led_pwm_test.ino`

Important PCB note:
The schematic routes `UVLEDPWM` to `GPIO39`, but `GPIO39` is input-only on the
ESP32. It can read a voltage, but it cannot generate PWM.

Current workaround:
Use the existing jumper plan from an output-capable pin such as `GPIO32` to the
`UVLEDPWM` / LED-driver control net, then run the PWM test sketch.

## 5.5. UNCS Vitamin C Concentration Test

Sketch:
`fadex_homebase_final_blynk/fadex_homebase_final_blynk.ino`

The final homebase firmware now includes the UNCS concentration math and reads
the photodiode/TIA output on `PDO / GPIO36`.

Demo commands:

| Command | Meaning |
| --- | --- |
| `UNCS DARK` | LED off, capture detector dark voltage |
| `UNCS BLANK` | LED on with `0 mM` water blank, capture blank voltage |
| `UNCS SAMPLE` | LED on with Vitamin C sample, calculate concentration |
| `UNCS PWM 50` | Set UV LED duty cycle to `50%` |
| `UNCS STATUS` | Print stored voltages and latest concentration |

Calculation:

```cpp
T = (v_sample - v_dark) / (v_blank - v_dark)
A = -log10(T)
C_mM = (A / 0.241) ^ 2.0964
```

TIA / optical gain guidance:

If `V_blank - V_dark` is too small, the detector is not seeing enough usable
light, so increase LED duty, improve alignment, or increase TIA gain. If
`V_blank` is near `3.0 V`, the ADC may be close to saturation, so lower LED
duty or reduce TIA gain.

## 6. Full Homebase Firmware

Sketch:
`fadex_homebase_final_blynk/fadex_homebase_final_blynk.ino`

The full firmware now uses the PCB pump pin map:

| Pump | Connector | IN1 net / Arduino pin | IN2 net / Arduino pin |
| --- | --- | --- | --- |
| Nicotine / Pump 1 | J7 | `3190.1.IN1` / GPIO22 | `3190.1.IN2` / GPIO21 |
| Diluent / Pump 2 | J4 | `3190.2.IN1` / GPIO19 | `3190.2.IN2` / GPIO18 |
| Mixing / Pump 3 | J9 | `3190.3.IN1` / GPIO33 | `3190.3.IN2` / GPIO4 |

On startup, the serial monitor prints the homebase Wi-Fi STA MAC so the
handheld can be paired later.
