# ESP32-S3 Reverse Osmosis Controller

Firmware for a home reverse-osmosis controller based on ESP32-S3 N16R8 (16 MB Flash / 8 MB PSRAM).

## Safety architecture

- Hydraulic outputs boot/reset OFF.
- The 20 ms control task owns the deterministic RO state machine and is watched by ESP-IDF Task Watchdog.
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
- LittleFS event/statistics storage and filter service tracking;
- first-run Wi-Fi AP provisioning;
- authenticated HTTPS administration with per-device self-signed certificate;
- Secure/HttpOnly/SameSite session cookie + CSRF/origin checks for write APIs;
- encrypted configuration backup/restore;
- dual-slot OTA with rollback validation and hydraulic OTA hold;
- MQTT + Home Assistant discovery with safe command allowlist;
- reserved optional interfaces for Flow/TDS/Leak/Recovery.

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

## Build

ESP-IDF is pinned to v6.0.2.

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Host tests:

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
