export type OptionalReading = {
  available: boolean;
  value: number | null;
};

export type ControllerState = {
  mode: "auto" | "maintenance";
  state: string;
  error: string;
  water_available: boolean;
  tank_full: boolean;
  leak_available: boolean;
  leak_detected: boolean;
  inlet: boolean;
  pump: boolean;
  flush: boolean;
  production_runtime_s: number;
  wifi_connected: boolean;
  mqtt_connected: boolean;
  ip: string;
  time_source: string;
  time_valid: boolean;
  rtc_available: boolean;
  feed_flow: OptionalReading;
  pure_flow: OptionalReading;
  drain_flow: OptionalReading;
  feed_tds: OptionalReading;
  pure_tds: OptionalReading;
  recovery: OptionalReading;
  filters: unknown[];
};

export type LoginResult = { csrf: string };
