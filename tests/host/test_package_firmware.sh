#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

build_dir="$work_dir/build"
out_dir="$work_dir/out"
mkdir -p "$build_dir/bootloader" "$build_dir/partition_table"

printf 'firmware\n' > "$build_dir/ro_controller.bin"
printf 'bootloader\n' > "$build_dir/bootloader/bootloader.bin"
printf 'partition\n' > "$build_dir/partition_table/partition-table.bin"
printf 'ota\n' > "$build_dir/ota_data_initial.bin"
cat > "$build_dir/flash_args" <<'EOF'
--flash-mode dio --flash-freq 80m --flash-size 16MB
0x0 bootloader/bootloader.bin
0x8000 partition_table/partition-table.bin
0x13000 ota_data_initial.bin
0x20000 ro_controller.bin
EOF
printf '{"flash_files":{}}\n' > "$build_dir/flasher_args.json"

"$repo_root/scripts/package-firmware.sh" "$build_dir" "$out_dir"

for file in \
  ro_controller.bin \
  bootloader/bootloader.bin \
  partition_table/partition-table.bin \
  ota_data_initial.bin \
  flash_args \
  flasher_args.json \
  FLASHING.txt \
  SHA256SUMS.txt; do
  test -f "$out_dir/$file"
done

(
  cd "$out_dir"
  sha256sum -c SHA256SUMS.txt
)

grep -Fq 'write_flash @flash_args' "$out_dir/FLASHING.txt"
grep -Fq '0x20000 ro_controller.bin' "$out_dir/flash_args"

echo 'Firmware packaging test passed.'
