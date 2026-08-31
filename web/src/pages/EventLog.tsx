export function EventLog({ text }: { text: string }) {
  return <section class="card"><h2>Event log</h2><pre>{text || "No events"}</pre></section>;
}
