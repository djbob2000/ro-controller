import type { ControllerState } from "../api/types";

export function Diagnostics({ state }: { state: ControllerState }) {
  return <section class="card"><h2>Diagnostics</h2><dl><dt>Wi-Fi</dt><dd>{state.wifi_connected ? "Connected" : "Disconnected"}</dd><dt>MQTT</dt><dd>{state.mqtt_connected ? "Connected" : "Disconnected"}</dd><dt>IP</dt><dd>{state.ip}</dd><dt>Clock</dt><dd>{state.time_valid ? state.time_source : "Unavailable"}</dd><dt>RTC</dt><dd>{state.rtc_available ? "Installed" : "Not installed"}</dd><dt>Production runtime</dt><dd>{state.production_runtime_s}s</dd></dl></section>;
}
