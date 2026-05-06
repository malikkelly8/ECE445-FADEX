# FadeX Hardware Bring-Up Notes

## ESP32 Pin Translation

The numbers `27, 28, 31, 30, 33, 36` from the ESP32-WROOM schematic are **module pin numbers**, not Arduino GPIO numbers.

For Arduino code, use:

- Module pin `27` -> `GPIO16`
- Module pin `28` -> `GPIO17`
- Module pin `31` -> `GPIO19`
- Module pin `30` -> `GPIO18`
- Module pin `33` -> `GPIO21`
- Module pin `36` -> `GPIO22`

That means the three DRV8871 channels in the hardware test sketch are:

- Pump 1: `GPIO16`, `GPIO17`
- Pump 2: `GPIO19`, `GPIO18`
- Pump 3: `GPIO21`, `GPIO22`

## Additional Pins Visible In The New Schematic Crops

From the homebase ESP32 crop:

- `DAT1 -> GPIO25`
- `DAT2 -> GPIO26`
- `DAT3 -> GPIO27`
- `DAT4 -> GPIO13`
- `SCLK -> GPIO23`
- `PDO -> GPIO36`
- `UVLEDPWM -> GPIO39` in the current schematic crop

That means the current homebase load-cell plan shown in the schematic is:

- Load Cell 1: `DAT1/GPIO25`, `SCLK/GPIO23`
- Load Cell 2: `DAT2/GPIO26`, `SCLK/GPIO23`
- Load Cell 3: `DAT3/GPIO27`, `SCLK/GPIO23`
- Load Cell 4: `DAT4/GPIO13`, `SCLK/GPIO23`

For the optical path:

- `PDO/GPIO36` is a good ADC input choice.
- `GPIO36` is input-only, which is fine for the photodiode voltage readout.
- `UVLEDPWM/GPIO39` is a problem if we want the ESP32 to generate PWM, because
  `GPIO39` is input-only and cannot drive the LED driver control pin.
- If the PCB is already made, a practical rework is to jumper `GPIO32` to the
  `UVLEDPWM` net or directly to the AL8860 `CTRL` pin.

From the handheld ESP32 crop:

- `PSENS -> GPIO22`
- `PWM -> GPIO23`

## ESP LED Note


On many ESP32 dev boards this LED is on `GPIO2`, but we should change that if we're using another LED

## What Each Sketch Verifies

- HOMEBASE LED TEST
  Blinks the onboard LED on the homebase ESP32 dev board so we can confirm the board is powered, programmed, and running code.

- HANDHELD LED TEST
  Blinks the onboard LED on the handheld ESP32 dev board so we can confirm the board is powered, programmed, and running code.

- SQUAREWAVE TEST
  Confirms the ESP32 was programmed successfully and that a chosen GPIO is really toggling. You should see a square wave on the oscilloscope.

- Motor Driver Test
  Confirms each DRV8871 channel can drive a pump in one direction, reverse polarity, and stop.

- Load Cell Test
  Confirms one HX711 wiring path works and lets you tune the calibration factor with a known weight.

- Load Cell AMP Basic Read

- Photodiode Test
  Confirms the `PDO` analog net reaches `GPIO36` and prints the measured voltage
  in the serial monitor.

- UV LED Test
  Generates PWM on `GPIO32` for a jumper-wire rework into the AL8860 `CTRL`
  input and sweeps the duty cycle so we can verify the waveform on a scope.

## CH340 / Auto-Programming Note

The CH340-to-ESP32 upload circuit is a **hardware test**, not a firmware test.

When you click **Upload** in Arduino IDE, the CH340 and transistor network should toggle the ESP32 `EN` and `IO0` lines to place the chip into bootloader/program mode. The square-wave sketch cannot verify that path directly because it only runs **after** the ESP32 has already been programmed and started.

The right way to scope that upload circuit is:

1. Leave the ESP32 and CH340 connected as in the board design.
2. Probe `EN` and `IO0`.
3. Click **Upload** in Arduino IDE.
4. Look for the expected boot-mode toggling around the start of the upload.

