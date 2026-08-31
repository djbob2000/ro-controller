export function Firmware() {
  return <section class="card"><h2>Firmware</h2><p>OTA is accepted only after the controller enters its internal OTA hold and confirms Pump/Inlet/Flush are OFF.</p><p>Use the authenticated OTA endpoint from a trusted LAN client. The bootloader rollback path remains enabled.</p></section>;
}
