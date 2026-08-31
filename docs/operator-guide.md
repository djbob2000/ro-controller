# Operator guide

## First run

With no saved Wi-Fi configuration the controller starts an open AP named `RO-Controller-XXXX` and serves the provisioning page over HTTP. Connect locally and enter:

- Wi-Fi SSID/password;
- a new admin password (minimum 10 characters in the provisioning UI);
- browser-detected timezone.

After validation the controller saves configuration and restarts into station mode. Normal administration is HTTPS only. The per-device certificate is self-signed, so the browser will initially warn until you explicitly trust/accept it for the LAN device.

The usual hostname is `https://ro-controller.local/`; the current station IP can also be used.

## Normal operation

The automatic state machine handles:

- startup flush after cold boot when water is required;
- production;
- debounced tank-full -> final flush -> standby;
- low-water stop and delayed recovery;
- preventive standby flush;
- Quiet Hours for preventive standby flush only;
- persistent leak/max-runtime errors.

Network loss does not stop the automatic controller. Normal production requested by water use is not blocked by Quiet Hours.

## Local buttons and Maintenance

- Up / Down: navigate pages/menu.
- OK: select.
- Long OK: local menu.
- Maintenance entry/exit is local-only.

Maintenance output tests time out automatically (default 30 s). Pump/manual-flush tests still obey low-water protection.

### Admin password reset

1. Power off.
2. Hold Up + Down.
3. Power on and keep holding for about 5 s.
4. Release Up/Down when prompted.
5. Hold OK for the local confirmation period.

This clears only the administrator verifier/salt. Wi-Fi, MQTT, counters, calibration/settings and persistent safety latches are preserved. The controller restarts.

### Factory reset

1. Power off.
2. Hold Up + Down + OK.
3. Power on and keep holding for about 10 s.
4. Follow the local OK confirmation.

Factory reset erases configuration/credentials, filter state, persisted controller facts and stored operational data, then returns to provisioning. Hydraulic outputs are forced OFF before reset/restart.

## Web administration security

Normal Web UI/API uses:

- HTTPS;
- random in-memory session token in a `Secure; HttpOnly; SameSite=Strict` cookie;
- separate CSRF token for state-changing API calls;
- same-origin validation when the browser supplies an `Origin` header.

The web API has no route for entering/exiting Maintenance and no unrestricted pump/inlet/flush switch.

## Filters

Filter status is based on configured calendar days and/or accumulated water volume when a suitable volume source is available. Calendar warnings are unavailable while wall-clock time is unknown; warnings never block RO production.

Reset a filter only when the element has actually been replaced. Reset requires valid wall-clock time so the installation timestamp is meaningful.

## Configuration backup / restore

Backup uses a user-supplied passphrase and authenticated encryption. Store the backup and its passphrase separately.

Restore validates/decrypts the configuration, persists it, and schedules a clean restart. Operational history is not intended to be transplanted as controller configuration.

## OTA firmware update

OTA is rejected if the firmware cannot enter its safe OTA hold. The control task must first report:

- state `OTA_HOLD`;
- inlet OFF;
- pump OFF;
- flush OFF.

Only then is the inactive OTA partition written. Bootloader rollback is enabled; a newly booted image is marked valid only after stable runtime reaches the firmware validation deadline.

Do not remove power while the inactive partition is being written.

## MQTT

Enable MQTT in settings and configure host, port, credentials and TLS mode. TLS mode uses certificate validation; firmware never silently falls back from `mqtts://` to plaintext.

Runtime root is:

```text
<base-topic>/<device-id>
```

Default base topic is `ro-controller`.

Important topics:

```text
.../availability             retained online/offline (LWT)
.../state                    controller snapshot
.../command/start_flush      payload PRESS only
.../command/reset_error      payload PRESS only
```

No MQTT topic can directly force Pump/Inlet/Flush ON/OFF or enter Maintenance.

## Home Assistant Discovery

Discovery is published below the configured discovery prefix (default `homeassistant`). Core entities include Mode, State, Water Available, Tank Full, Inlet/Pump/Flush read-only state, production runtime, five filter status entities and the two safe buttons.

Leak/Flow/TDS/Recovery entities are published only when that capability is configured. Disabled optional entities publish an empty retained discovery config to remove stale Home Assistant entities.

## Time and Quiet Hours

Time priority:

1. NTP when available;
2. optional DS3231 RTC;
3. calendar time unknown.

Control/safety durations always use monotonic time. If calendar time is unknown, normal RO operation continues but preventive standby flush is not launched because Quiet Hours cannot be evaluated safely.

Default Quiet Hours are 22:00–08:00. They apply only to preventive standby flushing, not production caused by actual water use.

## Errors

### Low water

Not latched. Pump stops immediately. After water returns it must remain valid for 3 s, then the controller waits an additional 10 s before restart.

### Leak

Latched when leak sensing is enabled. All outputs OFF. The latch survives reboot and can be reset only after the leak input is no longer active.

### Maximum runtime

Default continuous production limit is 3 h. Exceeding it latches an error, turns all hydraulic outputs OFF and requires explicit reset. Power cycling does not clear the latch.
