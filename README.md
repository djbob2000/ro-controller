# ESP32-S3 Reverse Osmosis Controller

Firmware for a home reverse-osmosis controller based on ESP32-S3 N16R8 (16 MB Flash / 8 MB PSRAM).

## Safety architecture

- Hydraulic outputs boot/reset OFF, including active-low configurations (OFF output latches are preloaded before GPIO output mode is enabled).
- The 20 ms control task owns the deterministic RO state machine and is watched by ESP-IDF Task Watchdog.
- Flash/NVS/filesystem writes run in a separate low-priority storage task; the control task only enqueues bounded work.
- Low-water stops the pump immediately in software; the installation must also keep the independent low-pressure hardware interlock in the pump power path.
- Leak and maximum-production errors are persistent latches.
- High-pressure/tank-full is latched through the final-flush sequence to prevent short cycling.
- Maintenance entry/exit is physical-button-only.
- Web and MQTT expose only high-level requests; they never drive hydraulic GPIOs directly.
- Wi-Fi/MQTT loss is non-fatal and does not participate in control timing.

## Phase-1 features

- inlet valve, RO pump and flush valve control;
- low/high pressure inputs through PC817;
- SSD1306 OLED + Up/Down/OK local UI;
- startup/final/standby/manual flushing and Quiet Hours;
- persistent configuration and error facts in NVS;
- optional DS3231 + NTP time service;
- LittleFS bounded event history, durable daily statistics checkpoints and filter tracking;
- first-run Wi-Fi AP provisioning with atomic settings/admin-verifier commit;
- authenticated HTTPS administration with per-device self-signed certificate;
- Secure/HttpOnly/SameSite session cookie + CSRF/origin checks for write APIs;
- authenticated SSE event endpoint;
- encrypted configuration backup/restore;
- dual-slot OTA with rollback validation and hydraulic OTA hold;
- MQTT + Home Assistant discovery with safe command allowlist;
- optional PCNT flow channels on GPIO4/5/16 and explicit unavailable semantics for absent Flow/TDS/Leak/Recovery hardware.

## Default timing

| Function | Default |
| --- | ---: |
| Boot sensor stabilization | 2.5 s |
| Low-pressure stable time | 3 s |
| Additional restart delay | 10 s |
| Tank-full debounce | 2 s |
| Prepare | 1 s |
| Flush | 20 s |
| Long-idle startup-flush threshold | 6 h |
| Preventive standby flush | every 24 h |
| Quiet Hours | 22:00–08:00 |
| Max continuous production | 3 h |
| Maintenance output test | 30 s |

## Build from source

ESP-IDF is pinned to v6.0.2.

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Flash a CI firmware bundle

The `esp-idf-build` CI job publishes an artifact named `ro-controller-esp32s3`. It contains the production application, bootloader, partition table, OTA-data image, ESP-IDF-generated `flash_args` / `flasher_args.json`, SHA-256 checksums and `FLASHING.txt`.

After extracting the artifact:

```bash
python -m pip install --upgrade esptool
python -m esptool --chip esp32s3 -p PORT -b 460800 --before default_reset --after hard_reset write_flash @flash_args
```

Replace `PORT` with e.g. `/dev/cu.usbmodemXXXX` on macOS or `/dev/ttyACM0` on Linux. Use the **production artifact**, not the test-hooks build, on an installed controller.

For the first powered test, leave the pump power circuit disconnected and verify input semantics plus valve/output logic first. Connect the pump last and retain the independent low-pressure hardware interlock described in `docs/wiring-rev1.md`.

## Tests

```bash
cmake -S tests/host -B build-host
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
```

Bench test build and pytest instructions are in `tests/target/README.md`.

## Documentation

- `docs/wiring-rev1.md` — Rev.1 pinout and power/load wiring.
- `docs/operator-guide.md` — provisioning, local controls, web, MQTT/HA, backup and OTA.
- `docs/development.md` — reproducible development and verification commands.
- `docs/bench/real-ro-acceptance.md` — staged real-system acceptance checklist.

## Important deployment note

The ESP32 HTTPS server is intended for the trusted LAN only. Do not port-forward it to the public Internet. For remote access, use a separate secure network layer such as Home Assistant remote access or a VPN/Tailscale-class solution.
