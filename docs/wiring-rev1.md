# Rev.1 wiring

Target: ESP32-S3 N16R8, 24 V RO loads, 5 V ESP32 supply.

## GPIO map

| GPIO | Function | Notes |
| ---: | --- | --- |
| 4 | Feed flow | reserved PCNT input |
| 5 | Pure flow | reserved PCNT input |
| 6 | Low-pressure input | PC817 CH1 |
| 7 | High-pressure / tank-full input | PC817 CH2 |
| 8 | I²C SDA | SSD1306 + optional DS3231 |
| 9 | I²C SCL | SSD1306 + optional DS3231 |
| 10 | Up button | active low |
| 11 | Down button | active low |
| 12 | OK button | active low |
| 13 | Inlet valve command | FR120N #1 |
| 14 | Pump command | FR120N #2 |
| 15 | Flush valve command | FR120N #3 |
| 16 | Drain flow | reserved PCNT input |
| 17 | Recovery stepper | reserved |
| 18 | Recovery stepper | reserved |
| 21 | Recovery stepper | reserved |
| 38 | Leak input | PC817 CH3, optional |

GPIO35–37, USB pins and strapping-sensitive pins are intentionally not assigned to application I/O.

## Power distribution

```text
24 V PSU + ---- main fuse ----+---- inlet valve ---- FR120N #1
                              +---- RO pump -------- FR120N #2
                              +---- flush valve ---- FR120N #3
                              +---- 24->5 V buck --- ESP32 5 V

24 V PSU - ------------------- common load return / module wiring per board
```

Size the main fuse and conductor cross-section from the actual pump inrush/continuous current plus solenoids. Do not infer the fuse rating from firmware.

## FR120N outputs

- GPIO13 -> inlet module logic input.
- GPIO14 -> pump module logic input.
- GPIO15 -> flush module logic input.
- Configure Active HIGH/LOW to match the actual module before connecting hydraulic loads.
- First bench test with LEDs/multimeter/dummy loads.
- Install flyback suppression directly at every inductive 24 V load (pump/solenoid) with polarity appropriate for normal operation.
- Firmware outputs are logical/semantic; `Hardware::apply_outputs()` is the only hydraulic GPIO driver.

## PC817 pressure inputs

- CH1 output -> GPIO6 -> semantic `water_available`.
- CH2 output -> GPIO7 -> semantic `tank_full`.
- CH3 output -> GPIO38 -> optional semantic `leak_detected`.
- Configure each contact as NO/NC according to the real pressure switch and verify with the pump disconnected.

The ESP32 input side uses internal pull-ups and the current board adapter interprets an optocoupler-closed input as a low GPIO level before NO/NC normalization.

## Independent low-pressure interlock

The low-pressure switch must also interrupt the pump power/control path independently of ESP32 software, provided the switch/contact or interposing relay is correctly rated. This is the first safety layer: loss of feed water must be capable of preventing pump operation even if firmware or a MOSFET command is wrong.

Test the hardware interlock with the ESP32 deliberately commanding/attempting pump operation only after the low-voltage bench checks are complete.

## I²C

- SDA GPIO8, SCL GPIO9.
- SSD1306 default address: `0x3C`.
- DS3231 address: `0x68` when installed.
- Firmware treats missing RTC/OLED as optional/non-fatal where applicable.

## Optional flow inputs

GPIO4/5/16 are reserved for feed/pure/drain pulse sensors. Firmware instantiates PCNT measurement only when the channel is enabled and `pulses_per_liter > 0`. Electrical conditioning depends on the purchased sensor; do not connect an unknown-voltage sensor output directly to ESP32.

## First energization order

1. Power ESP32 from the 5 V buck with all 24 V hydraulic loads disconnected.
2. Verify OLED/buttons and all logical outputs OFF through reset.
3. Verify PC817 low/high semantics using switches only.
4. Connect inlet/flush valves and verify direction/flyback.
5. Verify the low-pressure hardware interlock.
6. Connect the pump **last**.
7. Follow `docs/bench/real-ro-acceptance.md`.
