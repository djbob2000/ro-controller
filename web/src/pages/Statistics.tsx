export function Statistics({ data }: { data: unknown }) {
  return <section class="card"><h2>Statistics</h2><pre>{JSON.stringify(data, null, 2)}</pre></section>;
}
