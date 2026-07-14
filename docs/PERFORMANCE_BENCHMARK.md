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
  --device-id "katre-x4" \
  --device-model "X4" \
  --protocol-id "katre-x4-benchmark-v2" \
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
Use the same device ID, model, and protocol ID for the matching baseline and
fast runs. Change the protocol ID whenever the SD card, EPUB, font, layout, or
refresh setup changes.

The collector prints count, minimum, median, p95, and maximum durations. It
also writes every accepted record and the grouped summaries to the requested
JSON file. A malformed `PERF` record or an incomplete `--samples` run exits
nonzero instead of silently producing partial evidence.

Raw serial logs can be parsed later without a connected device:

```bash
python3 scripts/perf_collect.py crosspoint-serial.log \
  --label "katre-fast-$(git rev-parse --short HEAD)-x4" \
  --device-id "katre-x4" \
  --device-model "X4" \
  --protocol-id "katre-x4-benchmark-v2" \
  --output benchmark-katre-fast.json
```

Compare matching 20-sample baseline and fast reports after both runs:

```bash
python3 scripts/perf_compare.py \
  benchmark-baseline-x4-page-turn.json \
  benchmark-katre-fast-x4-page-turn.json \
  --output benchmark-x4-page-turn-comparison.json
```

The comparator recomputes summaries from the raw records and rejects different
devices, protocol IDs, scenarios, cache states, managed states, sample counts,
or page traces. Page-turn records use wire version 2 and reports use schema 3.
Their benchmark context contains the exact spine/page/direction/refresh trace
plus the constant font-size and text-antialiasing settings; raw iteration
numbers are normalized after consecutive-order validation. Positive
percentages mean the fast candidate took less time. Treat the percentages as
descriptive results from this X4 run, not statistical significance.

For `book_open`, compare upstream only with the fast branch's unmanaged
(`managed=false`) report. Managed Readest books and `readest_probe` have no
upstream-equivalent group and are measured separately on the fast branch.

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
4. Choose two adjacent pages inside a fully indexed, text-only chapter and make
   20 turns alternating between them: A to B, B to A, and repeat. Wait for each
   panel update to finish before pressing again. Overlapping inputs are
   discarded because the firmware coalesces them into one ambiguous render.
   The collector rejects iteration gaps, any third page, a mid-run font-size or
   text-antialiasing change, an unidentified refresh branch, and image pages.
   Only text-page `fast` and `half` refresh records are comparable because image
   pages can execute several different display sequences.
   `page_turn_in_section` deliberately excludes chapter boundaries; keep
   automatic page turn off.
5. Save `/api/status` from the same idle state for each build so free heap is
   comparable. Every timing record also includes free heap immediately after
   that scenario.

Use medians for the main comparison and p95 to catch stalls only for groups with
at least 20 samples. Do not optimize the Readest manifest path unless
`readest_probe` or managed-versus-unmanaged `book_open` results show a
repeatable regression.
