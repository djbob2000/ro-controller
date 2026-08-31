export function Filters({ data }: { data: unknown }) {
  return <section class="card"><h2>Filters</h2><pre>{JSON.stringify(data, null, 2)}</pre></section>;
}
