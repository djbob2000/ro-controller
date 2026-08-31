# ESP32-S3 bench tests

These tests exercise the real firmware through its serial console. Test builds enable `CONFIG_RO_TEST_HOOKS=y`; production builds keep it disabled.

## Safety model

The diagnostic protocol can:

- read the coherent controller snapshot (`snapshot`);
- substitute semantic `water/tank/leak` inputs;
- stop/start the network stack;
- enqueue the normal high-level manual-flush/reset commands.

It **cannot** directly set Pump/Inlet/Flush GPIO levels. Every output still passes through `Controller` and `Hardware::apply_outputs()`.

## Build and flash

```bash
idf.py fullclean
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test.defaults" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test.defaults" build flash
```

Install host tooling in an isolated environment:

```bash
python -m venv .venv
. .venv/bin/activate
pip install pytest pytest-embedded pytest-embedded-serial-esp pytest-embedded-idf
```

Run:

```bash
pytest tests/target --target esp32s3 -m bench
```

The pressure-cycle tests use production timings, so startup/recovery paths can take tens of seconds. Do not connect the real RO pump for the first bench run; use LEDs/meter/dummy loads on the FR120N outputs.

## Physical reset fixture

Password reset and factory reset are deliberately **not** simulated by the firmware test hook. They prove physical-presence authorization and therefore need external actuation of GPIO10/11/12 while resetting the board. Set `RO_RESET_FIXTURE=1` only when that fixture is present.

## Diagnostic commands

```text
snapshot
simulate:water=1,tank=0,leak=0
simulate:off
network:down
network:up
command:flush
command:reset_error
```

Every protocol response starts with `RO_TEST`, making it easy to distinguish from normal ESP-IDF logs.
