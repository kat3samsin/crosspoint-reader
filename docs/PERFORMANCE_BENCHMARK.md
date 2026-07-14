# CrossPoint performance benchmark

The `katre_benchmark` image has the same feature set as `katre_fast`, plus
opt-in serial timing records. The normal fast image compiles the benchmark
calls out completely.

The benchmark image deliberately waits 250 ms for USB serial enumeration.
Compare benchmark builds with benchmark builds; do not compare its absolute
boot time with a production `katre_fast` image. `boot_to_home` uses firmware
uptime through the first completed Home paint, so it includes the serial delay,
firmware setup, and power-button verification after the firmware timer starts.
Use the same short press each time, or use an automated reset for the cleanest
firmware comparison. Use external video for reset-to-visible wall-clock time.

## Build and collect

```bash
pio run -e katre_benchmark
pio run -e katre_benchmark -t upload
python3 scripts/perf_collect.py \
  --port /dev/cu.usbmodem2101 \
  --timeout 900 \
  --scenario boot_to_home \
  --samples 20 \
  --label "katre-fast-$(git rev-parse --short HEAD)-x4" \
  --output benchmark-katre-fast-x4-boot.json
```

Replace the serial path with the CrossPoint path shown by:

```bash
pio device list
```

The live collector retries while the port is absent and reconnects to the same
path after each USB reset. Use a separate report for each scenario, changing
`--scenario`, `--samples`, and the output filename. Every label must identify
the firmware SHA and device model.

The collector prints count, minimum, median, p95, and maximum durations. It
also writes every accepted record and the grouped summaries to the requested
JSON file. A malformed `PERF` record or an incomplete `--samples` run exits
nonzero instead of silently producing partial evidence.

Raw serial logs can be parsed later without a connected device:

```bash
python3 scripts/perf_collect.py crosspoint-serial.log \
  --label "katre-fast-$(git rev-parse --short HEAD)-x4" \
  --output benchmark-katre-fast.json
```

## Repeatable X4 run

Keep the device, SD card, EPUB, font, font size, margins, anti-aliasing, and
refresh settings identical between builds.

1. Run the boot collector command above, then boot to Home 20 times.
   `boot_to_home` ends after the panel's blocking display call returns. The
   collector retains accepted samples while USB serial disconnects and
   re-enumerates.
2. Open the same fully cached, text-only EPUB at least 20 times. `book_open`
   covers the software transition through the first completed page paint;
   `epub_load` isolates EPUB metadata/index loading. `epub_index_cache` reports
   only whether `book.bin` existed. Keep the entire per-book cache directory in
   the same state for every sample. For cold opens, identify and remove that
   book's complete `/.crosspoint/epub_*` cache directory, not just `book.bin`,
   and collect cold opens separately.
3. Repeat at least 20 cached opens with one Readest-managed EPUB and 20 with one
   unmanaged EPUB.
   `readest_probe` isolates manifest ownership, hashing, sidecar, and progress
   mapping overhead.
4. Make 20 forward page turns inside a fully indexed, text-only chapter. Wait
   for each panel update to finish before pressing again. Overlapping inputs are
   discarded because the firmware coalesces them into one ambiguous render.
   `page_turn_in_section` deliberately excludes chapter boundaries; keep
   automatic page turn off.
5. Save `/api/status` from the same idle state for each build so free heap is
   comparable. Every timing record also includes free heap immediately after
   that scenario.

Use medians for the main comparison and p95 to catch stalls only for groups with
at least 20 samples. Do not optimize the Readest manifest path unless
`readest_probe` or managed-versus-unmanaged `book_open` results show a
repeatable regression.
