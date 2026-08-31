import { render } from "preact";
import { useEffect, useState } from "preact/hooks";
import { getEvents, getFilters, getSettings, getState, getStats, login, setup } from "./api/client";
import type { ControllerState } from "./api/types";
import { Dashboard } from "./pages/Dashboard";
import { Statistics } from "./pages/Statistics";
import { EventLog } from "./pages/EventLog";
import { Filters } from "./pages/Filters";
import { Settings } from "./pages/Settings";
import { Firmware } from "./pages/Firmware";
import { Diagnostics } from "./pages/Diagnostics";
import "./style.css";

type Page = "dashboard" | "statistics" | "events" | "filters" | "settings" | "firmware" | "diagnostics";

function Login({ onDone }: { onDone: () => void }) {
  const [password, setPassword] = useState("");
  const [ssid, setSsid] = useState("");
  const [wifiPassword, setWifiPassword] = useState("");
  const [message, setMessage] = useState("");

  async function signIn() {
    try { await login(password); setMessage(""); onDone(); }
    catch (error) { setMessage(error instanceof Error ? error.message : "Login failed"); }
  }

  async function firstSetup() {
    try {
      await setup({
        password,
        ssid,
        wifi_password: wifiPassword,
        timezone_name: Intl.DateTimeFormat().resolvedOptions().timeZone || "Etc/UTC",
        utc_offset_minutes: -new Date().getTimezoneOffset(),
      });
      setMessage("Saved. Controller is restarting.");
    } catch (error) { setMessage(error instanceof Error ? error.message : "Setup failed"); }
  }

  return <main class="login"><section class="card"><h1>RO Controller</h1><label>Admin password<input type="password" value={password} onInput={(e) => setPassword(e.currentTarget.value)}/></label><label>Wi-Fi SSID<input value={ssid} onInput={(e) => setSsid(e.currentTarget.value)}/></label><label>Wi-Fi password<input type="password" value={wifiPassword} onInput={(e) => setWifiPassword(e.currentTarget.value)}/></label><div class="actions"><button onClick={signIn}>Login</button><button onClick={firstSetup}>First setup</button></div><p>{message}</p></section></main>;
}

function App() {
  const [page, setPage] = useState<Page>("dashboard");
  const [state, setState] = useState<ControllerState | null>(null);
  const [settings, setSettings] = useState<Record<string, unknown>>({});
  const [stats, setStats] = useState<unknown>({});
  const [filters, setFilters] = useState<unknown>([]);
  const [events, setEvents] = useState("");
  const [authenticated, setAuthenticated] = useState(false);

  async function refresh() {
    try {
      const next = await getState();
      setState(next);
      setAuthenticated(true);
      if (page === "settings") setSettings(await getSettings());
      if (page === "statistics") setStats(await getStats());
      if (page === "filters") setFilters(await getFilters());
      if (page === "events") setEvents(await getEvents());
    } catch {
      setAuthenticated(false);
      setState(null);
    }
  }

  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 2000);
    return () => window.clearInterval(timer);
  }, [page]);

  if (!authenticated || !state) return <Login onDone={() => void refresh()} />;

  const content = page === "dashboard" ? <Dashboard state={state}/> :
    page === "statistics" ? <Statistics data={stats}/> :
    page === "events" ? <EventLog text={events}/> :
    page === "filters" ? <Filters data={filters}/> :
    page === "settings" ? <Settings data={settings}/> :
    page === "firmware" ? <Firmware/> : <Diagnostics state={state}/>;

  const pages: Page[] = ["dashboard", "statistics", "events", "filters", "settings", "firmware", "diagnostics"];
  return <><header><strong>RO Controller</strong><span>{state.state} · {state.ip}</span></header><nav>{pages.map((item) => <button class={page === item ? "active" : ""} onClick={() => setPage(item)}>{item}</button>)}</nav><main>{content}</main></>;
}

render(<App/>, document.getElementById("app")!);
