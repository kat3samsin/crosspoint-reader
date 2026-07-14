#!/usr/bin/env python3
"""Compare two CrossPoint performance reports."""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field as dataclass_field
from datetime import datetime, timezone
from pathlib import Path
from typing import TextIO

from perf_collect import (
    PROVENANCE_FIELDS,
    REPORT_SCHEMA,
    PerfRecordError,
    build_benchmark_context,
    parse_perf_line,
    paths_alias,
    summarize,
)


DEFAULT_MINIMUM_SAMPLES = 20
Scalar = str | int | bool
GroupKey = tuple[str, tuple[tuple[str, Scalar], ...]]


class PerfReportError(ValueError):
    """Raised when reports cannot be compared safely."""


@dataclass(frozen=True)
class PerfReport:
    label: str
    provenance: dict[str, str]
    groups: dict[GroupKey, dict[str, object]]
    benchmark_context: dict[str, object] = dataclass_field(default_factory=dict)


def _object_without_duplicate_keys(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise PerfReportError(f"duplicate field: {key}")
        result[key] = value
    return result


def _group_key(summary: dict[str, object]) -> GroupKey:
    scenario = summary.get("scenario")
    dimensions = summary.get("dimensions")
    if not isinstance(scenario, str) or not isinstance(dimensions, dict):
        raise PerfReportError("invalid recomputed summary")
    for key, value in dimensions.items():
        if not isinstance(key, str) or not isinstance(value, (str, int, bool)):
            raise PerfReportError("invalid summary dimensions")
    return scenario, tuple(sorted(dimensions.items()))


def load_report(path: Path) -> PerfReport:
    try:
        payload = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_object_without_duplicate_keys,
            parse_constant=lambda value: (_ for _ in ()).throw(
                PerfReportError(f"non-finite number: {value}")
            ),
        )
    except (OSError, json.JSONDecodeError, UnicodeError, PerfReportError) as exc:
        raise PerfReportError(f"{path}: {exc}") from exc

    if not isinstance(payload, dict):
        raise PerfReportError(f"{path}: report must be a JSON object")
    if payload.get("schema") != REPORT_SCHEMA or isinstance(
        payload.get("schema"), bool
    ):
        raise PerfReportError(f"{path}: unsupported report schema")

    label = payload.get("label")
    if not isinstance(label, str) or not label.strip():
        raise PerfReportError(f"{path}: label must be a non-empty string")

    generated_at = payload.get("generated_at")
    if not isinstance(generated_at, str):
        raise PerfReportError(f"{path}: generated_at must be a timestamp")
    try:
        generated = datetime.fromisoformat(generated_at)
    except ValueError as exc:
        raise PerfReportError(f"{path}: generated_at must be ISO 8601") from exc
    if generated.tzinfo is None or generated.utcoffset() is None:
        raise PerfReportError(f"{path}: generated_at must include a timezone")

    raw_provenance = payload.get("provenance")
    if not isinstance(raw_provenance, dict) or set(raw_provenance) != set(
        PROVENANCE_FIELDS
    ):
        raise PerfReportError(f"{path}: invalid provenance fields")
    provenance: dict[str, str] = {}
    for field in sorted(PROVENANCE_FIELDS):
        value = raw_provenance[field]
        if not isinstance(value, str) or not value.strip():
            raise PerfReportError(f"{path}: provenance {field} must be non-empty")
        provenance[field] = value

    raw_records = payload.get("records")
    if not isinstance(raw_records, list) or not raw_records:
        raise PerfReportError(f"{path}: records must be a non-empty list")

    records = []
    for index, raw_record in enumerate(raw_records, 1):
        if not isinstance(raw_record, dict):
            raise PerfReportError(f"{path}: record {index} must be an object")
        try:
            record = parse_perf_line(
                "PERF "
                + json.dumps(raw_record, separators=(",", ":"), ensure_ascii=False)
            )
        except PerfRecordError as exc:
            raise PerfReportError(f"{path}: record {index}: {exc}") from exc
        if record is None:  # pragma: no cover - the prefix above is constant
            raise PerfReportError(f"{path}: record {index} is not a PERF record")
        records.append(record)

    summaries = summarize(records)
    raw_summaries = payload.get("summary")
    if not isinstance(raw_summaries, list):
        raise PerfReportError(f"{path}: summary must be a list")
    supplied_groups: dict[GroupKey, dict[str, object]] = {}
    for raw_summary in raw_summaries:
        if not isinstance(raw_summary, dict):
            raise PerfReportError(f"{path}: summary rows must be objects")
        key = _group_key(raw_summary)
        if key in supplied_groups:
            raise PerfReportError(f"{path}: duplicate summary group")
        supplied_groups[key] = raw_summary

    recomputed_groups = {_group_key(summary): summary for summary in summaries}
    if supplied_groups != recomputed_groups:
        raise PerfReportError(f"{path}: summary does not match raw records")

    try:
        recomputed_context = build_benchmark_context(records)
    except PerfRecordError as exc:
        raise PerfReportError(f"{path}: invalid benchmark context: {exc}") from exc
    raw_context = payload.get("benchmark_context")
    if not isinstance(raw_context, dict) or raw_context != recomputed_context:
        raise PerfReportError(f"{path}: benchmark context does not match raw records")

    return PerfReport(
        label=label,
        provenance=provenance,
        groups=recomputed_groups,
        benchmark_context=recomputed_context,
    )


def _duration_reduction_percent(baseline_ms: float, candidate_ms: float) -> float:
    if not math.isfinite(baseline_ms) or baseline_ms <= 0:
        raise PerfReportError("baseline duration must be finite and positive")
    if not math.isfinite(candidate_ms) or candidate_ms < 0:
        raise PerfReportError("candidate duration must be finite and non-negative")
    return (baseline_ms - candidate_ms) / baseline_ms * 100.0


def compare_reports(
    baseline: PerfReport,
    candidate: PerfReport,
    minimum_samples: int = DEFAULT_MINIMUM_SAMPLES,
) -> dict[str, object]:
    if minimum_samples < 1:
        raise PerfReportError("minimum samples must be positive")
    if baseline.label == candidate.label:
        raise PerfReportError("baseline and candidate labels must differ")
    if baseline.provenance != candidate.provenance:
        raise PerfReportError("baseline and candidate provenance must match")
    if baseline.benchmark_context != candidate.benchmark_context:
        raise PerfReportError("baseline and candidate benchmark context must match")
    if baseline.groups.keys() != candidate.groups.keys():
        missing = sorted(baseline.groups.keys() - candidate.groups.keys())
        extra = sorted(candidate.groups.keys() - baseline.groups.keys())
        details = []
        if missing:
            details.append(f"missing candidate groups: {missing}")
        if extra:
            details.append(f"extra candidate groups: {extra}")
        raise PerfReportError("report groups differ; " + "; ".join(details))

    comparisons: list[dict[str, object]] = []
    for key in sorted(baseline.groups):
        baseline_group = baseline.groups[key]
        candidate_group = candidate.groups[key]
        baseline_count = int(baseline_group["count"])
        candidate_count = int(candidate_group["count"])
        if baseline_count != candidate_count:
            raise PerfReportError(f"sample counts differ for {key}")
        if baseline_count < minimum_samples:
            raise PerfReportError(
                f"{key} has {baseline_count} samples; minimum is {minimum_samples}"
            )

        baseline_median = float(baseline_group["median_ms"])
        candidate_median = float(candidate_group["median_ms"])
        baseline_p95 = float(baseline_group["p95_ms"])
        candidate_p95 = float(candidate_group["p95_ms"])
        comparisons.append(
            {
                "scenario": key[0],
                "dimensions": dict(key[1]),
                "count": baseline_count,
                "baseline_median_ms": baseline_median,
                "candidate_median_ms": candidate_median,
                "median_delta_ms": candidate_median - baseline_median,
                "median_duration_reduction_pct": _duration_reduction_percent(
                    baseline_median, candidate_median
                ),
                "baseline_p95_ms": baseline_p95,
                "candidate_p95_ms": candidate_p95,
                "p95_delta_ms": candidate_p95 - baseline_p95,
                "p95_duration_reduction_pct": _duration_reduction_percent(
                    baseline_p95, candidate_p95
                ),
            }
        )

    return {
        "schema": 1,
        "baseline_label": baseline.label,
        "candidate_label": candidate.label,
        "provenance": baseline.provenance,
        "benchmark_context": baseline.benchmark_context,
        "groups": comparisons,
    }


def print_comparison(
    comparison: dict[str, object], stream: TextIO | None = None
) -> None:
    if stream is None:
        stream = sys.stdout
    print(
        f"baseline:  {comparison['baseline_label']}\n"
        f"candidate: {comparison['candidate_label']}",
        file=stream,
    )
    provenance = comparison["provenance"]
    print(
        f"device:    {provenance['device_id']} ({provenance['device_model']})\n"
        f"protocol:  {provenance['protocol_id']}",
        file=stream,
    )
    print("less time: positive means candidate took less time", file=stream)
    print(
        "scenario                         n  base med  cand med  less time  "
        "base p95  cand p95  less time",
        file=stream,
    )
    for group in comparison["groups"]:
        dimensions = ",".join(
            f"{key}={value}" for key, value in group["dimensions"].items()
        )
        label = group["scenario"] + (f" [{dimensions}]" if dimensions else "")
        print(
            f"{label:<32} {group['count']:>3} "
            f"{group['baseline_median_ms']:>9.1f} "
            f"{group['candidate_median_ms']:>9.1f} "
            f"{group['median_duration_reduction_pct']:>+9.1f}% "
            f"{group['baseline_p95_ms']:>9.1f} "
            f"{group['candidate_p95_ms']:>9.1f} "
            f"{group['p95_duration_reduction_pct']:>+9.1f}%",
            file=stream,
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--minimum-samples", type=int, default=DEFAULT_MINIMUM_SAMPLES)
    parser.add_argument("--output", type=Path, help="write comparison as JSON")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if paths_alias(args.baseline, args.candidate):
            raise PerfReportError("baseline and candidate must be different files")
        if args.output and (
            paths_alias(args.output, args.baseline)
            or paths_alias(args.output, args.candidate)
        ):
            raise PerfReportError("output must not overwrite an input report")
        baseline = load_report(args.baseline)
        candidate = load_report(args.candidate)
        comparison = compare_reports(baseline, candidate, args.minimum_samples)
        if args.output:
            payload = {
                **comparison,
                "generated_at": datetime.now(timezone.utc).isoformat(),
            }
            args.output.write_text(
                json.dumps(payload, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    except (OSError, PerfReportError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print_comparison(comparison)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
