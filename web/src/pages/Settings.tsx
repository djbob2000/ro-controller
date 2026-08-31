import { useEffect, useState } from "preact/hooks";
import { putSettings } from "../api/client";

export function Settings({ data }: { data: Record<string, unknown> }) {
  const [text, setText] = useState("{}");
  const [message, setMessage] = useState("");
  useEffect(() => setText(JSON.stringify(data, null, 2)), [data]);

  async function save() {
    try {
      const parsed = JSON.parse(text) as Record<string, unknown>;
      await putSettings(parsed);
      setMessage("Saved. Controller may restart to apply hardware/network changes.");
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "Invalid JSON");
    }
  }

  return <section class="card"><h2>Settings</h2><textarea value={text} onInput={(event) => setText(event.currentTarget.value)} rows={22}/><p><button onClick={save}>Validate & save</button></p><p>{message}</p></section>;
}
