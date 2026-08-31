# Real RO hardware-in-the-loop acceptance

Status: **NOT RUN on the real RO system**

This document is deliberately an execution record, not a claim that hardware was tested remotely. Fill every result with measured observations, date and operator initials. Connect the pump only after the preceding low-risk stages pass.

## Test record

- Firmware commit: ____________________
- Date/time: ____________________
- Operator: ____________________
- ESP32-S3 board/module: ____________________
- 24 V PSU rating: ____________________
- Pump model/current: ____________________
- Inlet valve: ____________________
- Flush valve: ____________________
- Low-pressure switch NO/NC: ____________________
- High-pressure switch NO/NC: ____________________
- Notes/photos/log location: ____________________

Result legend: `PASS`, `FAIL`, `NOT RUN`.

## Stage A — inputs only, pump disconnected

| # | Check | Expected | Result | Observation |
| ---: | --- | --- | --- | --- |
| A1 | Power/reset outputs | No unintended inlet/pump/flush activation | NOT RUN | |
| A2 | PC817 low-pressure polarity | UI/snapshot `water_available` matches real switch | NOT RUN | |
| A3 | PC817 high-pressure polarity | UI/snapshot `tank_full` matches real switch | NOT RUN | |
| A4 | Optional leak input | Absent -> unavailable/non-fault; present -> correct state | NOT RUN | |
| A5 | OLED/buttons | Status and local navigation usable without network | NOT RUN | |
| A6 | Admin reset gesture | Up+Down + local OK clears admin only | NOT RUN | |
| A7 | Factory reset gesture | Up+Down+OK + local confirmation resets device safely | NOT RUN | |

Do not continue if any output pulses during boot/reset.

## Stage B — valves through FR120N, pump still disconnected

| # | Check | Expected | Result | Observation |
| ---: | --- | --- | --- | --- |
| B1 | Inlet output polarity | Commanded ON/OFF matches physical valve | NOT RUN | |
| B2 | Flush output polarity | Commanded ON/OFF matches physical valve | NOT RUN | |
| B3 | Flyback suppression | Installed at each inductive load, correct polarity | NOT RUN | |
| B4 | Maintenance timeout | Valve test stops at configured timeout | NOT RUN | |
| B5 | Reboot during valve activity | Outputs return OFF before control restarts | NOT RUN | |

## Stage C — independent low-pressure hardware interlock

Pump remains disconnected from the MOSFET output until the interlock wiring is verified electrically.

| # | Check | Expected | Result | Observation |
| ---: | --- | --- | --- | --- |
| C1 | Interlock continuity with water available | Pump power/control path may conduct | NOT RUN | |
| C2 | Interlock with feed pressure removed | Pump path is physically interrupted independent of ESP32 | NOT RUN | |
| C3 | Contact/current suitability | Switch/interposing relay rating is adequate | NOT RUN | |

This stage is a release blocker.

## Stage D — connect pump last

### D1 Empty tank automatic startup

1. Ensure feed water available and tank requires water.
2. Power controller.
3. Measure boot stabilization and startup sequence.
4. Confirm startup flush (default ~20 s) then production.

Result: **NOT RUN**  
Measured stabilization: ______  
Measured startup flush: ______  
Unexpected cycling/noise: ______

### D2 Tank full -> final flush -> standby

1. Allow tank-full switch to become active.
2. Confirm ~2 s debounce.
3. Confirm `FINAL_FLUSH` remains latched even if opening flush releases high pressure.
4. Confirm final flush completes and all outputs turn OFF in `STANDBY`.

Result: **NOT RUN**  
Measured debounce: ______  
Measured final flush: ______  
Number of unintended restarts: ______

Acceptance: unintended restarts must be **0**.

### D3 Low-water stop and recovery

1. While producing, close feed water / trigger low-pressure switch.
2. Confirm independent hardware interlock stops/prevents pump immediately.
3. Confirm software enters `WAIT_WATER` and Pump command is OFF.
4. Restore feed water.
5. Measure 3 s stable period + additional 10 s delay before restart sequence.

Result: **NOT RUN**  
Hardware stop observation: ______  
Stable delay measured: ______  
Restart delay measured: ______

### D4 Power-loss recovery matrix

| Power removed during | Expected after reboot | Result | Observation |
| --- | --- | --- | --- |
| Producing | Safe OFF boot evaluation, then startup logic if water still required | NOT RUN | |
| Final flush | Safe OFF boot evaluation; never restore transient output state | NOT RUN | |
| Standby | Safe OFF boot then sensor-derived standby/production decision | NOT RUN | |
| Manual flush | Manual operation is not restored | NOT RUN | |
| Maintenance | Maintenance is not restored | NOT RUN | |

### D5 Persistent max-runtime error

Use a deliberately shortened **test configuration** rather than running the pump for three hours.

1. Set a safe short max-runtime value for the test.
2. Produce until `ERROR_MAX_RUNTIME`.
3. Verify all outputs OFF.
4. Power-cycle.
5. Verify error remains latched.
6. Reset intentionally and restore production configuration.

Result: **NOT RUN**  
Latch survived reboot: ______

### D6 Leak latch (only if sensor installed)

1. Trigger leak input safely.
2. Confirm all outputs OFF and persistent `ERROR_LEAK`.
3. Power-cycle and confirm latch remains.
4. Verify reset is denied while leak input remains active.
5. Clear physical leak signal and reset intentionally.

Result: **NOT RUN / N/A**

### D7 Quiet Hours accelerated test

Use a shortened standby interval while preserving a quiet window that crosses the current test time.

Verify:

- due before quiet hours -> runs at due time;
- due during quiet hours -> moved to quiet boundary / suppressed according to configured rule;
- boot overdue during quiet period -> no surprise immediate nighttime flush;
- normal production caused by water use is not blocked.

Result: **NOT RUN**

### D8 Network-loss autonomy

During stable production:

1. Disconnect Wi-Fi/router.
2. If MQTT is used, stop broker separately.
3. Observe for at least several minutes.
4. Verify hydraulic state/control cadence is unchanged except for real sensor events.
5. Restore network and confirm state/discovery republish.

Result: **NOT RUN**

### D9 OTA rollback

1. Start from a known-good image.
2. Initiate OTA and verify controller reaches `OTA_HOLD` with all outputs OFF before flash writing.
3. Install a deliberately failing/unvalidated test image using a safe bench procedure.
4. Confirm bootloader rolls back to known-good firmware.
5. Repeat with a good image and confirm it is marked valid only after stable runtime.

Result: **NOT RUN**

## Multi-day soak

Run normal settings on the real RO installation for several days.

Track:

- unexpected ESP32 resets;
- watchdog resets;
- unintended output pulses;
- high-pressure short cycling;
- unexplained manual/standby flushes;
- network outages and whether RO continued normally;
- event-log/statistics consistency.

Start: __________  End: __________  Result: **NOT RUN**

## Final sign-off

Release may be called real-system accepted only when all applicable safety-critical rows are PASS and failures have an attached issue/commit reference.

- Final result: **NOT RUN**
- Operator signature/initials: ____________________
- Firmware commit accepted: ____________________
