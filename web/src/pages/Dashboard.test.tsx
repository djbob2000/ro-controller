import render from "preact-render-to-string";
import { describe, expect, it } from "vitest";
import { Dashboard } from "./Dashboard";
import type { ControllerState } from "../api/types";

const state: ControllerState = {
  mode: "auto", state: "STANDBY", error: "NONE",
  water_available: true, tank_full: true, leak_available: false, leak_detected: false,
  inlet: false, pump: false, flush: false, production_runtime_s: 0,
  wifi_connected: true, mqtt_connected: false, ip: "192.0.2.1",
  time_source: "NTP", time_valid: true, rtc_available: false,
  feed_flow: { available: false, value: null }, pure_flow: { available: false, value: null },
  drain_flow: { available: false, value: null }, feed_tds: { available: false, value: null },
  pure_tds: { available: false, value: null }, recovery: { available: false, value: null }, filters: [],
};

describe("Dashboard", () => {
  it("shows unavailable optional hardware explicitly", () => {
    const html = render(<Dashboard state={state}/>);
    expect(html.match(/Not installed/g)?.length).toBeGreaterThanOrEqual(6);
  });

  it("exposes only high-level action buttons", () => {
    const html = render(<Dashboard state={state}/>);
    expect(html).toContain("Start flush");
    expect(html).toContain("Reset error");
    expect(html).not.toContain("Pump ON");
    expect(html).not.toContain("Inlet ON");
  });
});
