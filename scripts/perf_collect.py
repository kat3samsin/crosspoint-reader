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
REPORT_SCHEMA = 2
PROVENANCE_FIELDS = ("device_id", "device_model", "protocol_id")
SCENARIO_RE = re.compile(r"^[a-z][a-z0-9_]*$")
DIMENSION_KEYS = {
    "boot_to_home": (),
    "book_open": ("epub_index_cache", "managed"),
    "epub_load": ("epub_index_cache",),
    "page_turn_in_section": ("direction",),
    "readest_probe": ("managed",),
}
CACHE_STATES = frozenset({"hit", "miss", "unknown"})
PAGE_DIRECTIONS = frozenset({"forward", "backward"})


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


def _require_uint32(fields: dict[str, str | int | bool], field: str) -> None:
    _non_negative_int(_required_field(fields, field), field)


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
    if version != 1:
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


def collect_serial(port: str, baud: int, timeout: float, echo: bool) -> Iterable[str]:
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required for --port; install scripts/requirements.txt"
        ) from exc

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        framer = LineFramer()
        try:
            with serial.Serial(port, baud, timeout=0.25) as connection:
                while time.monotonic() < deadline:
                    chunk = connection.read(connection.in_waiting or 1)
                    if not chunk:
                        continue
                    for line in framer.feed(chunk):
                        if echo:
                            print(line)
                        yield line

            partial = framer.take_partial()
            if partial.startswith(b"PERF "):
                raise PerfRecordError("collection ended with a partial PERF record")
            return
        except (serial.SerialException, OSError) as exc:
            partial = framer.take_partial()
            if partial.startswith(b"PERF "):
                raise PerfRecordError(
                    "serial disconnect left a partial PERF record"
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
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.port and args.inputs:
        raise SystemExit("--port cannot be combined with input files")
    if args.samples is not None and args.samples <= 0:
        raise SystemExit("--samples must be positive")
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
            lines = collect_serial(args.port, args.baud, args.timeout, args.echo)
        elif args.inputs:
            opened = [
                path.open(encoding="utf-8", errors="replace") for path in args.inputs
            ]
            lines = read_text_lines(opened)
        else:
            lines = sys.stdin

        records = collect_records(lines, args.scenario, args.samples)
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

    summaries = summarize(records)
    print_summary(summaries)
    if args.output:
        report = {
            "schema": REPORT_SCHEMA,
            "label": args.label,
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "provenance": provenance,
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
