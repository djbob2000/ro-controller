# Development and verification

## Pinned firmware environment

- ESP-IDF: **v6.0.2**
- C++: C++20
- Target: `esp32s3`
- Flash: 16 MB
- PSRAM: Octal 8 MB target configuration

Environment used while completing this branch:

- Python 3.13.5
- CMake 3.31.6
- Node.js 22.16.0
- npm 10.9.2

Node is only required for the optional/source web frontend work; the current firmware embeds its administration page in `ro_network` and therefore the ESP-IDF build itself has no Node dependency.

## Clean host verification

```bash
rm -rf build-host
cmake -S tests/host -B build-host
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
```

## Clean firmware verification

```bash
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```

GitHub CI uses the `espressif/idf:v6.0.2` container and uploads the application, bootloader, partition table and initial OTA data binaries after a successful build.

## Bench/test-hook build

The test protocol is compile-time gated and is disabled in production defaults.

```bash
idf.py fullclean
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test.defaults" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test.defaults" build
```

Then follow `tests/target/README.md`.

Do not ship a firmware image with `CONFIG_RO_TEST_HOOKS=y`.

## Partitions

The project uses two OTA application slots, NVS, LittleFS/storage and coredump partitions. Any partition-table edit must preserve a rollback-capable dual-slot layout and fit the configured 16 MB flash.

## Coding boundary

The safety invariant is:

```text
local UI / Web / MQTT / scheduler
            |
       Command queue
            |
        Controller
            |
   DesiredOutputs snapshot
            |
 Hardware::apply_outputs()
            |
       GPIO13/14/15
```

Network/storage/UI code must not call hydraulic `gpio_set_level()` directly.

## Mbed TLS 4 / ESP-IDF 6 note

ESP-IDF 6 uses Mbed TLS 4/PSA Crypto. `ro_services` contains narrow compatibility bridges for the legacy call shape used by the original certificate/backup implementation. New crypto code should use current PSA/Mbed TLS APIs directly instead of expanding those shims.

## Verification gates

Before calling a firmware revision releasable:

1. host tests pass;
2. normal ESP-IDF build passes from a clean build directory;
3. test-hook ESP-IDF build passes;
4. target bench tests pass on an ESP32-S3 with dummy loads;
5. wiring checks are completed with the pump disconnected;
6. real-RO acceptance checklist is executed with the pump connected last;
7. OTA rollback is actually exercised on hardware;
8. run the system for several days and record resets/output anomalies/short cycling.

Steps 4–8 require physical hardware and must never be inferred from CI.

## Useful CI inspection

The workflow is `.github/workflows/ci.yml`. It runs on pushes to `main` and `feature/complete-controller` and on pull requests.
