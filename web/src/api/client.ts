import type { ControllerState, LoginResult } from "./types";

let csrf = localStorage.getItem("roCsrf") ?? "";

async function json<T>(url: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(url, { credentials: "same-origin", ...init });
  if (!response.ok) {
    let message = `${response.status} ${response.statusText}`;
    try {
      const body = await response.json() as { error?: string };
      if (body.error) message = body.error;
    } catch {
      // Keep HTTP status when the response is not JSON.
    }
    throw new Error(message);
  }
  return response.json() as Promise<T>;
}

function writeHeaders(contentType = false): HeadersInit {
  const headers: Record<string, string> = {};
  if (csrf) headers["X-CSRF-Token"] = csrf;
  if (contentType) headers["Content-Type"] = "application/json";
  return headers;
}

export async function login(password: string): Promise<void> {
  const result = await json<LoginResult>("/api/login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ password }),
  });
  csrf = result.csrf;
  localStorage.setItem("roCsrf", csrf);
}

export async function setup(payload: {
  password: string;
  ssid: string;
  wifi_password: string;
  timezone_name: string;
  utc_offset_minutes: number;
}): Promise<void> {
  await json("/api/setup", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
}

export const getState = () => json<ControllerState>("/api/state");
export const getSettings = () => json<Record<string, unknown>>("/api/settings");
export const getStats = () => json<Record<string, unknown>>("/api/stats");
export const getFilters = () => json<unknown[]>("/api/filters");

export async function getEvents(): Promise<string> {
  const response = await fetch("/api/events", { credentials: "same-origin" });
  if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
  return response.text();
}

export async function postAction(path: "/api/actions/flush" | "/api/actions/reset-error"): Promise<void> {
  await json(path, { method: "POST", headers: writeHeaders() });
}

export async function putSettings(settings: Record<string, unknown>): Promise<void> {
  await json("/api/settings", {
    method: "PUT",
    headers: writeHeaders(true),
    body: JSON.stringify(settings),
  });
}

export function formatReading(reading: { available: boolean; value: number | null }, unit: string): string {
  if (!reading.available || reading.value == null) return "Not installed";
  return `${reading.value} ${unit}`;
}
