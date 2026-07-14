#!/usr/bin/env python3
"""Collect and summarize opt-in CrossPoint firmware performance records."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, TextIO


MAX_RECORD_BYTES = 512
RECONNECT_DELAY_SECONDS = 0.25
PERF_RECORD_VERSION = 2
REPORT_SCHEMA = 3
PROVENANCE_FIELDS = ("device_id", "device_model", "protocol_id")
SCENARIO_RE = re.compile(r"^[a-z][a-z0-9_]*$")
DIMENSION_KEYS = {
    "boot_to_home": (),
    "book_open": ("epub_index_cache", "managed"),
    "epub_load": ("epub_index_cache",),
    "page_turn_in_section": (),
    "readest_probe": ("managed",),
}
CACHE_STATES = frozenset({"hit", "miss", "unknown"})
PAGE_DIRECTIONS = frozenset({"forward", "backward"})
PAGE_REFRESH_MODES = frozenset({"fast", "half"})
PAGE_FONT_SIZES = frozenset({0, 1, 2, 3})
AUTOMATED_PAGE_TURN_COUNT = 20
AUTOMATED_PAGE_TURN_SETTLE_MS = 3000
PAGE_TURN_START_RE = re.compile(
    r"^PERF_CONTROL page_turn_auto started count=(?P<count>\d+) "
    r"settle_ms=(?P<settle_ms>\d+) spine=(?P<spine>\d+) "
    r"page_a=(?P<page_a>\d+) page_b=(?P<page_b>\d+)$"
)
PAGE_TURN_DONE_RE = re.compile(
    r"^PERF_CONTROL page_turn_auto done count=(?P<count>\d+) "
    r"spine=(?P<spine>\d+) page=(?P<page>\d+)$"
)


class PerfRecordError(ValueError):
    """Raised when a PERF line violates the versioned record contract."""


@dataclass(frozen=True)
class PerfRecord:
    version: int
    scenario: str
    iteration: int
    duration_us: int
    fields: dict[str, str | int | bool]

    def to_dict(self) -> dict[str, str | int | bool]:
        return {
            "v": self.version,
            "scenario": self.scenario,
            "iteration": self.iteration,
            "duration_us": self.duration_us,
            **self.fields,
        }


class LineFramer:
    """Turn arbitrary serial byte chunks into complete UTF-8 lines."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, chunk: bytes) -> list[str]:
        self._buffer.extend(chunk)
        lines: list[str] = []
        while True:
            newline = self._buffer.find(b"\n")
            if newline < 0:
                break
            raw = bytes(self._buffer[:newline])
            del self._buffer[: newline + 1]
            lines.append(raw.rstrip(b"\r").decode("utf-8", errors="replace"))
        return lines

    def take_partial(self) -> bytes:
        partial = bytes(self._buffer)
        self._buffer.clear()
        return partial


def _object_without_duplicate_keys(
    pairs: list[tuple[str, object]]
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise PerfRecordError(f"duplicate field: {key}")
        result[key] = value
    return result


def _non_negative_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise PerfRecordError(f"{field} must be a non-negative integer")
    if value > 0xFFFFFFFF:
        raise PerfRecordError(f"{field} exceeds uint32 range")
    return value


def _required_field(
    fields: dict[str, str | int | bool], field: str
) -> str | int | bool:
    if field not in fields:
        raise PerfRecordError(f"missing field(s): {field}")
    return fields[field]


def _require_uint32(fields: dict[str, str | int | bool], field: str) -> int:
    return _non_negative_int(_required_field(fields, field), field)


def _require_bool(fields: dict[str, str | int | bool], field: str) -> None:
    if not isinstance(_required_field(fields, field), bool):
        raise PerfRecordError(f"{field} must be a boolean")


def _require_enum(
    fields: dict[str, str | int | bool], field: str, allowed: frozenset[str]
) -> None:
    value = _required_field(fields, field)
    if not isinstance(value, str) or value not in allowed:
        choices = ", ".join(sorted(allowed))
        raise PerfRecordError(f"{field} must be one of: {choices}")


def _validate_scenario_fields(
    scenario: str, fields: dict[str, str | int | bool]
) -> None:
    _require_uint32(fields, "heap_free_bytes")
    if scenario == "book_open":
        _require_enum(fields, "epub_index_cache", CACHE_STATES)
        _require_bool(fields, "managed")
    elif scenario == "epub_load":
        _require_enum(fields, "epub_index_cache", CACHE_STATES)
    elif scenario == "readest_probe":
        _require_bool(fields, "managed")
    elif scenario == "page_turn_in_section":
        _require_enum(fields, "direction", PAGE_DIRECTIONS)
        _require_uint32(fields, "spine_index")
        from_page = _require_uint32(fields, "from_page")
        to_page = _require_uint32(fields, "to_page")
        font_size = _require_uint32(fields, "font_size")
        if font_size not in PAGE_FONT_SIZES:
            raise PerfRecordError("font_size must be one of: 0, 1, 2, 3")
        _require_bool(fields, "text_antialiasing")
        _require_enum(fields, "refresh_mode", PAGE_REFRESH_MODES)
        direction = fields["direction"]
        if direction == "forward" and to_page != from_page + 1:
            raise PerfRecordError("forward page turn must advance exactly one page")
        if direction == "backward" and from_page != to_page + 1:
            raise PerfRecordError("backward page turn must retreat exactly one page")


def parse_perf_line(line: str) -> PerfRecord | None:
    """Parse one raw serial line; ordinary firmware output returns None."""

    if not line.startswith("PERF "):
        return None
    if len(line.encode("utf-8")) > MAX_RECORD_BYTES:
        raise PerfRecordError("record exceeds 512 bytes")

    try:
        payload = json.loads(
            line[5:],
            object_pairs_hook=_object_without_duplicate_keys,
            parse_constant=lambda value: (_ for _ in ()).throw(
                PerfRecordError(f"non-finite number: {value}")
            ),
        )
    except PerfRecordError:
        raise
    except (json.JSONDecodeError, TypeError, ValueError) as exc:
        raise PerfRecordError(f"invalid JSON: {exc}") from exc

    if not isinstance(payload, dict):
        raise PerfRecordError("payload must be a JSON object")

    required = {"v", "scenario", "iteration", "duration_us"}
    missing = required - payload.keys()
    if missing:
        raise PerfRecordError(f"missing field(s): {', '.join(sorted(missing))}")

    version = _non_negative_int(payload.pop("v"), "v")
    if version != PERF_RECORD_VERSION:
        raise PerfRecordError(f"unsupported version: {version}")
    scenario = payload.pop("scenario")
    if not isinstance(scenario, str) or not SCENARIO_RE.fullmatch(scenario):
        raise PerfRecordError("scenario must match [a-z][a-z0-9_]*")
    if scenario not in DIMENSION_KEYS:
        raise PerfRecordError(f"unsupported scenario: {scenario}")
    iteration = _non_negative_int(payload.pop("iteration"), "iteration")
    if iteration < 1:
        raise PerfRecordError("iteration must be at least 1")
    duration_us = _non_negative_int(payload.pop("duration_us"), "duration_us")

    fields: dict[str, str | int | bool] = {}
    for key, value in payload.items():
        if not isinstance(key, str) or not SCENARIO_RE.fullmatch(key):
            raise PerfRecordError(f"invalid field name: {key}")
        if not isinstance(value, (str, int, bool)) or value is None:
            raise PerfRecordError(f"optional field {key} must be a flat scalar")
        fields[key] = value

    _validate_scenario_fields(scenario, fields)

    return PerfRecord(version, scenario, iteration, duration_us, fields)


def read_text_lines(streams: Iterable[TextIO]) -> Iterable[str]:
    for stream in streams:
        yield from stream


def paths_alias(first: Path, second: Path) -> bool:
    try:
        if first.resolve() == second.resolve():
            return True
    except (OSError, RuntimeError):
        pass
    try:
        return first.samefile(second)
    except OSError:
        return False


def collect_records(
    lines: Iterable[str], scenario: str | None = None, samples: int | None = None
) -> list[PerfRecord]:
    records: list[PerfRecord] = []
    matching = 0
    for line_number, line in enumerate(lines, 1):
        try:
            record = parse_perf_line(line.rstrip("\r\n"))
        except PerfRecordError as exc:
            raise PerfRecordError(f"line {line_number}: {exc}") from exc
        if record is None:
            continue
        if scenario is not None and record.scenario != scenario:
            continue
        records.append(record)
        matching += 1
        if samples is not None and matching >= samples:
            break
    return records


def build_benchmark_context(
    records: Iterable[PerfRecord],
) -> dict[str, object]:
    """Build non-grouping page trace provenance and reject mixed settings."""

    page_records = [
        record for record in records if record.scenario == "page_turn_in_section"
    ]
    if not page_records:
        return {}

    first = page_records[0]
    constants = {
        "font_size": first.fields["font_size"],
        "text_antialiasing": first.fields["text_antialiasing"],
    }
    if len(page_records) < 2 or len(page_records) % 2 != 0:
        raise PerfRecordError(
            "page_turn_in_section report requires an even alternating two-page trace"
        )
    first_from = (
        first.fields["spine_index"],
        first.fields["from_page"],
    )
    first_to = (first.fields["spine_index"], first.fields["to_page"])
    trace: list[dict[str, str | int | bool]] = []
    previous_iteration: int | None = None
    for index, record in enumerate(page_records):
        for field, expected in constants.items():
            if record.fields[field] != expected:
                raise PerfRecordError(
                    f"page_turn_in_section {field} changed within the report"
                )
        if (
            previous_iteration is not None
            and record.iteration != previous_iteration + 1
        ):
            raise PerfRecordError("page_turn_in_section iterations must be consecutive")
        previous_iteration = record.iteration
        expected_from, expected_to = (
            (first_from, first_to) if index % 2 == 0 else (first_to, first_from)
        )
        actual_from = (record.fields["spine_index"], record.fields["from_page"])
        actual_to = (record.fields["spine_index"], record.fields["to_page"])
        if (actual_from, actual_to) != (expected_from, expected_to):
            raise PerfRecordError(
                "page_turn_in_section trace must alternate between exactly two pages"
            )
        trace.append(
            {
                "direction": record.fields["direction"],
                "spine_index": record.fields["spine_index"],
                "from_page": record.fields["from_page"],
                "to_page": record.fields["to_page"],
                "refresh_mode": record.fields["refresh_mode"],
            }
        )

    return {
        "page_turn_in_section": {
            **constants,
            "trace": trace,
        }
    }


def _nearest_rank(values: list[float], percentile: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(percentile * len(ordered)) - 1)]


def summarize(records: Iterable[PerfRecord]) -> list[dict[str, object]]:
    groups: dict[tuple[str, tuple[tuple[str, str | int | bool], ...]], list[float]] = {}
    for record in records:
        dimensions = tuple(
            (key, record.fields[key])
            for key in DIMENSION_KEYS.get(record.scenario, ())
            if key in record.fields
        )
        groups.setdefault((record.scenario, dimensions), []).append(
            record.duration_us / 1000.0
        )

    summaries: list[dict[str, object]] = []
    for (scenario, dimensions), values in sorted(groups.items()):
        summaries.append(
            {
                "scenario": scenario,
                "dimensions": dict(dimensions),
                "count": len(values),
                "min_ms": min(values),
                "median_ms": statistics.median(values),
                "p95_ms": _nearest_rank(values, 0.95),
                "max_ms": max(values),
            }
        )
    return summaries


def print_summary(
    summaries: Iterable[dict[str, object]], stream: TextIO | None = None
) -> None:
    if stream is None:
        stream = sys.stderr
    rows = list(summaries)
    if not rows:
        print("No PERF records found.", file=stream)
        return
    print(
        "scenario                         n     min   median      p95      max",
        file=stream,
    )
    for row in rows:
        dimensions = ",".join(
            f"{key}={value}" for key, value in row["dimensions"].items()
        )
        label = row["scenario"] + (f" [{dimensions}]" if dimensions else "")
        print(
            f"{label:<32} {row['count']:>3} "
            f"{row['min_ms']:>7.1f} {row['median_ms']:>8.1f} "
            f"{row['p95_ms']:>8.1f} {row['max_ms']:>8.1f}",
            file=stream,
        )


def collect_serial(
    port: str, baud: int, timeout: float, echo: bool, drive_page_turns: bool = False
) -> Iterable[str]:
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required for --port; install scripts/requirements.txt"
        ) from exc

    deadline = time.monotonic() + timeout
    command_issued = False
    driven_queued = False
    driven_start: tuple[int, int] | None = None
    driven_records: list[tuple[str, PerfRecord]] = []
    while time.monotonic() < deadline:
        framer = LineFramer()
        try:
            with serial.Serial(port, baud, timeout=0.25) as connection:
                if drive_page_turns:
                    connection.reset_input_buffer()
                    command = b"CMD:PERF_PAGE_TURNS_20\n"
                    command_issued = True
                    if connection.write(command) != len(command):
                        raise PerfRecordError(
                            "short serial write for page-turn command"
                        )
                    connection.flush()
                while time.monotonic() < deadline:
                    chunk = connection.read(connection.in_waiting or 1)
                    if not chunk:
                        continue
                    for line in framer.feed(chunk):
                        if echo:
                            print(line, flush=True)
                        if not drive_page_turns:
                            yield line
                            continue

                        if line.startswith("PERF_CONTROL page_turn_auto error="):
                            raise PerfRecordError(line.removeprefix("PERF_CONTROL "))

                        if line.startswith("PERF_CONTROL page_turn_auto started"):
                            match = PAGE_TURN_START_RE.fullmatch(line)
                            if match is None:
                                raise PerfRecordError(
                                    "malformed page_turn_auto started marker"
                                )
                            if not driven_queued:
                                raise PerfRecordError(
                                    "page_turn_auto started before queued"
                                )
                            if driven_start is not None:
                                raise PerfRecordError(
                                    "duplicate page_turn_auto started marker"
                                )
                            count = int(match["count"])
                            settle_ms = int(match["settle_ms"])
                            spine = int(match["spine"])
                            page_a = int(match["page_a"])
                            page_b = int(match["page_b"])
                            if count != AUTOMATED_PAGE_TURN_COUNT:
                                raise PerfRecordError(
                                    "unexpected automated page-turn count"
                                )
                            if settle_ms != AUTOMATED_PAGE_TURN_SETTLE_MS:
                                raise PerfRecordError(
                                    "unexpected automated page-turn settling interval"
                                )
                            if page_b != page_a + 1:
                                raise PerfRecordError(
                                    "automated pages A and B are not adjacent"
                                )
                            driven_start = (spine, page_a)
                            continue

                        if line.startswith("PERF_CONTROL page_turn_auto done"):
                            match = PAGE_TURN_DONE_RE.fullmatch(line)
                            if match is None:
                                raise PerfRecordError(
                                    "malformed page_turn_auto done marker"
                                )
                            if driven_start is None:
                                raise PerfRecordError(
                                    "page_turn_auto done before started"
                                )
                            count = int(match["count"])
                            spine = int(match["spine"])
                            page = int(match["page"])
                            if count != AUTOMATED_PAGE_TURN_COUNT:
                                raise PerfRecordError(
                                    "unexpected completed page-turn count"
                                )
                            if (spine, page) != driven_start:
                                raise PerfRecordError(
                                    "automated page-turn run did not finish on page A"
                                )
                            if len(driven_records) != AUTOMATED_PAGE_TURN_COUNT:
                                raise PerfRecordError(
                                    "page_turn_auto done without exactly 20 PERF records"
                                )
                            first = driven_records[0][1]
                            last = driven_records[-1][1]
                            start_spine, page_a = driven_start
                            if (
                                first.fields["direction"] != "forward"
                                or first.fields["spine_index"] != start_spine
                                or first.fields["from_page"] != page_a
                                or first.fields["to_page"] != page_a + 1
                                or last.fields["spine_index"] != start_spine
                                or last.fields["to_page"] != page_a
                            ):
                                raise PerfRecordError(
                                    "PERF records do not match the automated A/B run"
                                )
                            for record_line, _record in driven_records:
                                yield record_line
                            return

                        if line.startswith("PERF_CONTROL page_turn_auto"):
                            if line != "PERF_CONTROL page_turn_auto queued":
                                raise PerfRecordError(
                                    "unknown page_turn_auto control marker"
                                )
                            if driven_queued or driven_start is not None:
                                raise PerfRecordError(
                                    "duplicate page_turn_auto queued marker"
                                )
                            driven_queued = True
                            continue

                        if line.startswith("PERF "):
                            if driven_start is None:
                                continue
                            record = parse_perf_line(line)
                            if (
                                record is None
                                or record.scenario != "page_turn_in_section"
                            ):
                                raise PerfRecordError(
                                    "unexpected PERF scenario during automated page turns"
                                )
                            if len(driven_records) >= AUTOMATED_PAGE_TURN_COUNT:
                                raise PerfRecordError(
                                    "more than 20 PERF records before page_turn_auto done"
                                )
                            driven_records.append((line, record))

            partial = framer.take_partial()
            if partial.startswith(b"PERF "):
                raise PerfRecordError("collection ended with a partial PERF record")
            if drive_page_turns:
                raise PerfRecordError("page_turn_auto did not finish before timeout")
            return
        except (serial.SerialException, OSError) as exc:
            partial = framer.take_partial()
            if partial.startswith(b"PERF "):
                raise PerfRecordError(
                    "serial disconnect left a partial PERF record"
                ) from exc
            if drive_page_turns and command_issued:
                raise PerfRecordError(
                    "serial disconnected after page-turn command; rerun the benchmark"
                ) from exc
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            time.sleep(min(RECONNECT_DELAY_SECONDS, remaining))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "inputs", nargs="*", type=Path, help="raw serial logs (default: stdin)"
    )
    parser.add_argument("--port", help="read live serial data from this device")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--timeout",
        type=float,
        default=120.0,
        help="live collection timeout in seconds",
    )
    parser.add_argument("--scenario", choices=sorted(DIMENSION_KEYS))
    parser.add_argument(
        "--samples", type=int, help="stop after this many matching records"
    )
    parser.add_argument("--label", default="benchmark")
    parser.add_argument("--device-id", help="stable identifier for this device")
    parser.add_argument("--device-model", help="device model, for example X4")
    parser.add_argument(
        "--protocol-id", help="shared identifier for comparable benchmark runs"
    )
    parser.add_argument(
        "--output", type=Path, help="write records and summaries as JSON"
    )
    parser.add_argument(
        "--echo", action="store_true", help="echo live serial lines to stdout"
    )
    parser.add_argument(
        "--drive-page-turns",
        action="store_true",
        help="trigger one automated 20-turn A/B run after opening the serial port",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.port and args.inputs:
        raise SystemExit("--port cannot be combined with input files")
    if args.samples is not None and args.samples <= 0:
        raise SystemExit("--samples must be positive")
    if args.drive_page_turns and (
        not args.port or args.scenario != "page_turn_in_section" or args.samples != 20
    ):
        print(
            "error: --drive-page-turns requires --port, "
            "--scenario page_turn_in_section, and --samples 20",
            file=sys.stderr,
        )
        return 2
    if args.output:
        if not args.label.strip():
            print("error: --label must be non-empty", file=sys.stderr)
            return 2
        if any(paths_alias(args.output, path) for path in args.inputs):
            print("error: --output must not overwrite an input log", file=sys.stderr)
            return 2
        provenance = {name: getattr(args, name) for name in PROVENANCE_FIELDS}
        missing = [
            name
            for name, value in provenance.items()
            if not isinstance(value, str) or not value.strip()
        ]
        if missing:
            print(
                "error: --output requires "
                + ", ".join(f"--{name.replace('_', '-')}" for name in missing),
                file=sys.stderr,
            )
            return 2

    opened: list[TextIO] = []
    try:
        if args.port:
            lines = collect_serial(
                args.port,
                args.baud,
                args.timeout,
                args.echo,
                args.drive_page_turns,
            )
        elif args.inputs:
            opened = [
                path.open(encoding="utf-8", errors="replace") for path in args.inputs
            ]
            lines = read_text_lines(opened)
        else:
            lines = sys.stdin

        sample_limit = None if args.drive_page_turns else args.samples
        records = collect_records(lines, args.scenario, sample_limit)
    except (OSError, RuntimeError, PerfRecordError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("Collection stopped.", file=sys.stderr)
        return 130
    finally:
        for stream in opened:
            stream.close()

    if args.samples is not None and len(records) < args.samples:
        print(
            f"error: collected {len(records)} of {args.samples} requested samples",
            file=sys.stderr,
        )
        return 2
    if args.scenario is not None and not records:
        print(f"error: no {args.scenario} records found", file=sys.stderr)
        return 2

    try:
        benchmark_context = build_benchmark_context(records)
    except PerfRecordError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    summaries = summarize(records)
    print_summary(summaries)
    if args.output:
        report = {
            "schema": REPORT_SCHEMA,
            "label": args.label,
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "provenance": provenance,
            "benchmark_context": benchmark_context,
            "records": [record.to_dict() for record in records],
            "summary": summaries,
        }
        try:
            args.output.write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
        except OSError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    return 0 if records else 1


if __name__ == "__main__":
    raise SystemExit(main())
