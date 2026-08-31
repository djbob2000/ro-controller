import type { ControllerState } from "../api/types";
import { formatReading, postAction } from "../api/client";

export function Dashboard({ state }: { state: ControllerState }) {
  return <section class="grid">
    <article class="card"><h2>{state.state}</h2><p>Mode: {state.mode}</p><p>Error: {state.error}</p></article>
    <article class="card"><h3>Inputs</h3><p>Water: {state.water_available ? "Available" : "Low"}</p><p>Tank: {state.tank_full ? "Full" : "Needs water"}</p><p>Leak: {state.leak_available ? (state.leak_detected ? "DETECTED" : "Dry") : "Not installed"}</p></article>
    <article class="card"><h3>Outputs</h3><p>Inlet: {state.inlet ? "ON" : "OFF"}</p><p>Pump: {state.pump ? "ON" : "OFF"}</p><p>Flush: {state.flush ? "ON" : "OFF"}</p></article>
    <article class="card"><h3>Measurements</h3><p>Feed: {formatReading(state.feed_flow, "L/min")}</p><p>Pure: {formatReading(state.pure_flow, "L/min")}</p><p>Drain: {formatReading(state.drain_flow, "L/min")}</p><p>Feed TDS: {formatReading(state.feed_tds, "ppm")}</p><p>Pure TDS: {formatReading(state.pure_tds, "ppm")}</p><p>Recovery: {formatReading(state.recovery, "%")}</p></article>
    <article class="card actions"><button onClick={() => postAction("/api/actions/flush")}>Start flush</button><button onClick={() => postAction("/api/actions/reset-error")}>Reset error</button></article>
  </section>;
}
