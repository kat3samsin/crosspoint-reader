# CrossPoint performance benchmark

The `katre_benchmark` image has the same feature set as `katre_fast`, plus
opt-in serial timing records. The normal fast image compiles the benchmark
calls out completely.

The benchmark image deliberately waits 250 ms for USB serial enumeration.
Compare benchmark builds with benchmark builds; do not compare its absolute
boot time with a production `katre_fast` image. `boot_to_home` also starts at
chip reset, so a power-button boot includes the time until the button is
released. Use the same short press each time, or use an automated reset for the
cleanest firmware comparison.

## Build and collect

```bash
pio run -e katre_benchmark
pio run -e katre_benchmark -t upload
python3 scripts/perf_collect.py \
  --port /dev/cu.usbmodem2101 \
  --timeout 180 \
  --label katre-fast \
  --output benchmark-katre-fast.json
```

Replace the serial path with the CrossPoint path shown by:

```bash
pio device list
```

The collector prints count, minimum, median, p95, and maximum durations. It
also writes every accepted record and the grouped summaries to the requested
JSON file. A malformed `PERF` record or an incomplete `--samples` run exits
nonzero instead of silently producing partial evidence.

Raw serial logs can be parsed later without a connected device:

```bash
python3 scripts/perf_collect.py crosspoint-serial.log \
  --label katre-fast \
  --output benchmark-katre-fast.json
```

## Repeatable X4 run

Keep the device, SD card, EPUB, font, font size, margins, anti-aliasing, and
refresh settings identical between builds.

1. Boot to Home five times. `boot_to_home` ends after the panel's blocking
   display call returns. For production boot feel, also record external video;
   the benchmark image includes the USB serial delay.
2. Open the same cached, text-only EPUB five times. Repeat with its cache
   removed if first-open performance matters. `book_open` covers the software
   transition through the first completed page paint; `epub_load` isolates
   EPUB metadata/index loading.
3. Repeat the cached open with one Readest-managed EPUB and one unmanaged EPUB.
   `readest_probe` isolates manifest ownership, hashing, sidecar, and progress
   mapping overhead.
4. Make 20 forward page turns inside a fully indexed, text-only chapter. Wait
   for each panel update to finish before pressing again. Overlapping inputs are
   discarded because the firmware coalesces them into one ambiguous render.
   Ignore chapter boundaries and keep automatic page turn off.
5. Save `/api/status` from the same idle state for each build so free heap is
   comparable. Every timing record also includes free heap immediately after
   that scenario.

Use medians for the main comparison and p95 to catch stalls. Do not optimize
the Readest manifest path unless `readest_probe` or managed-versus-unmanaged
`book_open` results show a repeatable regression.
