import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from perf_collect import (
    build_benchmark_context,
    parse_perf_line,
    summarize,
)  # noqa: E402
from perf_compare import (  # noqa: E402
    PerfReport,
    PerfReportError,
    compare_reports,
    load_report,
    main,
    print_comparison,
)


PROVENANCE = {
    "device_id": "katre-x4",
    "device_model": "X4",
    "protocol_id": "katre-x4-benchmark-v1",
}


def grouped_report(label, groups, provenance=None, benchmark_context=None):
    return PerfReport(
        label,
        dict(provenance or PROVENANCE),
        groups,
        dict(benchmark_context or {}),
    )


def record(
    iteration,
    duration_us,
    *,
    direction="forward",
    spine_index=2,
    from_page=None,
    to_page=None,
    font_size=3,
    text_antialiasing=True,
    refresh_mode="fast",
):
    if from_page is None:
        from_page = 9 + iteration
    if to_page is None:
        to_page = from_page + (1 if direction == "forward" else -1)
    return {
        "v": 2,
        "scenario": "page_turn_in_section",
        "iteration": iteration,
        "duration_us": duration_us,
        "direction": direction,
        "spine_index": spine_index,
        "from_page": from_page,
        "to_page": to_page,
        "font_size": font_size,
        "text_antialiasing": text_antialiasing,
        "refresh_mode": refresh_mode,
        "heap_free_bytes": 76000,
    }


def report(label, durations):
    records = [
        record(
            index,
            duration,
            direction="forward" if index % 2 else "backward",
            from_page=10 if index % 2 else 11,
            to_page=11 if index % 2 else 10,
        )
        for index, duration in enumerate(durations, 1)
    ]
    parsed = [
        parse_perf_line("PERF " + json.dumps(item, separators=(",", ":")))
        for item in records
    ]
    return {
        "schema": 3,
        "label": label,
        "generated_at": "2026-07-14T00:00:00+00:00",
        "provenance": dict(PROVENANCE),
        "benchmark_context": build_benchmark_context(
            item for item in parsed if item is not None
        ),
        "records": records,
        "summary": summarize(item for item in parsed if item is not None),
    }


class PerfCompareTest(unittest.TestCase):
    def test_compares_matching_reports_with_positive_duration_reduction(self):
        baseline_group = {
            "scenario": "boot_to_home",
            "dimensions": {},
            "count": 20,
            "median_ms": 1000.0,
            "p95_ms": 1200.0,
        }
        candidate_group = {
            **baseline_group,
            "median_ms": 800.0,
            "p95_ms": 900.0,
        }
        key = ("boot_to_home", ())

        comparison = compare_reports(
            grouped_report("baseline", {key: baseline_group}),
            grouped_report("fast", {key: candidate_group}),
        )

        group = comparison["groups"][0]
        self.assertEqual(group["median_delta_ms"], -200.0)
        self.assertEqual(group["median_duration_reduction_pct"], 20.0)
        self.assertEqual(group["p95_duration_reduction_pct"], 25.0)

    def test_rejects_different_dimensions(self):
        group = {
            "scenario": "page_turn_in_section",
            "dimensions": {},
            "count": 20,
            "median_ms": 100.0,
            "p95_ms": 120.0,
        }

        with self.assertRaisesRegex(PerfReportError, "report groups differ"):
            compare_reports(
                grouped_report(
                    "baseline",
                    {("page_turn_in_section", ()): group},
                ),
                grouped_report(
                    "fast",
                    {
                        (
                            "page_turn_in_section",
                            (("refresh_mode", "half"),),
                        ): {**group, "dimensions": {"refresh_mode": "half"}}
                    },
                ),
            )

    def test_rejects_unequal_or_insufficient_sample_counts(self):
        key = ("boot_to_home", ())
        group = {
            "scenario": "boot_to_home",
            "dimensions": {},
            "count": 20,
            "median_ms": 100.0,
            "p95_ms": 120.0,
        }

        with self.assertRaisesRegex(PerfReportError, "sample counts differ"):
            compare_reports(
                grouped_report("baseline", {key: group}),
                grouped_report("fast", {key: {**group, "count": 21}}),
            )
        with self.assertRaisesRegex(PerfReportError, "minimum is 20"):
            compare_reports(
                grouped_report("baseline", {key: {**group, "count": 19}}),
                grouped_report("fast", {key: {**group, "count": 19}}),
            )

    def test_load_report_recomputes_and_verifies_summary(self):
        payload = report("baseline", [100_000] * 20)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            path.write_text(json.dumps(payload), encoding="utf-8")

            loaded = load_report(path)
            self.assertEqual(loaded.label, "baseline")

            payload["summary"][0]["median_ms"] = 1.0
            path.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(PerfReportError, "does not match"):
                load_report(path)

    def test_load_report_verifies_context_and_rejects_iteration_gaps(self):
        payload = report("baseline", [100_000] * 20)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            payload["benchmark_context"]["page_turn_in_section"]["font_size"] = 2
            path.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(PerfReportError, "context does not match"):
                load_report(path)

            payload = report("baseline", [100_000] * 20)
            payload["records"][1]["iteration"] = 3
            path.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(PerfReportError, "consecutive"):
                load_report(path)

    def test_rejects_matching_groups_with_different_page_trace(self):
        group = {
            "scenario": "page_turn_in_section",
            "dimensions": {},
            "count": 20,
            "median_ms": 100.0,
            "p95_ms": 120.0,
        }
        key = ("page_turn_in_section", ())
        baseline_context = {
            "page_turn_in_section": {
                "font_size": 3,
                "text_antialiasing": True,
                "trace": [
                    {
                        "direction": "forward",
                        "spine_index": 2,
                        "from_page": 10,
                        "to_page": 11,
                        "refresh_mode": "fast",
                    },
                    {
                        "direction": "backward",
                        "spine_index": 2,
                        "from_page": 11,
                        "to_page": 10,
                        "refresh_mode": "half",
                    },
                ],
            }
        }
        candidate_context = json.loads(json.dumps(baseline_context))
        candidate_context["page_turn_in_section"]["trace"][0]["from_page"] = 11

        with self.assertRaisesRegex(PerfReportError, "context must match"):
            compare_reports(
                grouped_report(
                    "baseline", {key: group}, benchmark_context=baseline_context
                ),
                grouped_report(
                    "fast", {key: group}, benchmark_context=candidate_context
                ),
            )

        candidate_context = json.loads(json.dumps(baseline_context))
        candidate_context["page_turn_in_section"]["font_size"] = 2
        with self.assertRaisesRegex(PerfReportError, "context must match"):
            compare_reports(
                grouped_report(
                    "baseline", {key: group}, benchmark_context=baseline_context
                ),
                grouped_report(
                    "fast", {key: group}, benchmark_context=candidate_context
                ),
            )

    def test_normalizes_consecutive_iteration_numbers_before_comparison(self):
        baseline_payload = report("baseline", [100_000] * 20)
        candidate_payload = report("fast", [80_000] * 20)
        for item in candidate_payload["records"]:
            item["iteration"] += 40

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            baseline_path = root / "baseline.json"
            candidate_path = root / "candidate.json"
            baseline_path.write_text(json.dumps(baseline_payload), encoding="utf-8")
            candidate_path.write_text(json.dumps(candidate_payload), encoding="utf-8")

            comparison = compare_reports(
                load_report(baseline_path), load_report(candidate_path)
            )

        self.assertEqual(
            comparison["benchmark_context"], baseline_payload["benchmark_context"]
        )

    def test_load_report_rejects_duplicate_fields_and_naive_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            path.write_text('{"schema":2,"schema":2}', encoding="utf-8")
            with self.assertRaisesRegex(PerfReportError, "duplicate field"):
                load_report(path)

            payload = report("baseline", [100_000] * 20)
            payload["generated_at"] = "2026-07-14T00:00:00"
            path.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(PerfReportError, "include a timezone"):
                load_report(path)

    def test_load_report_requires_structured_provenance(self):
        payload = report("baseline", [100_000] * 20)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            del payload["provenance"]["device_id"]
            path.write_text(json.dumps(payload), encoding="utf-8")

            with self.assertRaisesRegex(PerfReportError, "invalid provenance"):
                load_report(path)

    def test_load_report_rejects_malformed_summary_rows_cleanly(self):
        payload = report("baseline", [100_000] * 20)
        payload["summary"] = [{"count": 20}]
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            path.write_text(json.dumps(payload), encoding="utf-8")

            with self.assertRaisesRegex(PerfReportError, "invalid recomputed summary"):
                load_report(path)

    def test_cli_writes_machine_readable_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            baseline = root / "baseline.json"
            candidate = root / "candidate.json"
            output = root / "comparison.json"
            baseline.write_text(
                json.dumps(report("baseline-sha-x4", [100_000] * 20)),
                encoding="utf-8",
            )
            candidate.write_text(
                json.dumps(report("fast-sha-x4", [80_000] * 20)),
                encoding="utf-8",
            )

            with redirect_stdout(io.StringIO()):
                self.assertEqual(
                    main([str(baseline), str(candidate), "--output", str(output)]),
                    0,
                )
            comparison = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(comparison["baseline_label"], "baseline-sha-x4")
        self.assertEqual(comparison["candidate_label"], "fast-sha-x4")
        self.assertEqual(comparison["groups"][0]["median_duration_reduction_pct"], 20.0)
        self.assertEqual(comparison["provenance"], PROVENANCE)

    def test_cli_rejects_comparing_a_report_to_itself(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            path.write_text(
                json.dumps(report("baseline", [100_000] * 20)), encoding="utf-8"
            )
            with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
                self.assertEqual(main([str(path), str(path)]), 2)

    def test_cli_does_not_overwrite_an_input_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            baseline = root / "baseline.json"
            candidate = root / "candidate.json"
            baseline.write_text(
                json.dumps(report("baseline", [100_000] * 20)), encoding="utf-8"
            )
            candidate.write_text(
                json.dumps(report("fast", [90_000] * 20)), encoding="utf-8"
            )
            original = baseline.read_text(encoding="utf-8")

            with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
                self.assertEqual(
                    main(
                        [
                            str(baseline),
                            str(candidate),
                            "--output",
                            str(baseline),
                        ]
                    ),
                    2,
                )

            self.assertEqual(baseline.read_text(encoding="utf-8"), original)

            output_alias = root / "comparison.json"
            os.link(baseline, output_alias)
            with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
                self.assertEqual(
                    main(
                        [
                            str(baseline),
                            str(candidate),
                            "--output",
                            str(output_alias),
                        ]
                    ),
                    2,
                )

            self.assertEqual(baseline.read_text(encoding="utf-8"), original)

    def test_rejects_identical_labels(self):
        key = ("boot_to_home", ())
        group = {
            "scenario": "boot_to_home",
            "dimensions": {},
            "count": 20,
            "median_ms": 100.0,
            "p95_ms": 120.0,
        }

        with self.assertRaisesRegex(PerfReportError, "labels must differ"):
            compare_reports(
                grouped_report("same", {key: group}),
                grouped_report("same", {key: group}),
            )

    def test_rejects_mismatched_device_or_protocol_provenance(self):
        key = ("boot_to_home", ())
        group = {
            "scenario": "boot_to_home",
            "dimensions": {},
            "count": 20,
            "median_ms": 100.0,
            "p95_ms": 120.0,
        }
        other_device = {**PROVENANCE, "device_id": "another-x4"}

        with self.assertRaisesRegex(PerfReportError, "provenance must match"):
            compare_reports(
                grouped_report("baseline", {key: group}),
                grouped_report("fast", {key: group}, other_device),
            )

    def test_prints_provenance_and_signed_duration_reduction(self):
        stream = io.StringIO()
        comparison = {
            "baseline_label": "upstream",
            "candidate_label": "fast",
            "provenance": PROVENANCE,
            "groups": [
                {
                    "scenario": "boot_to_home",
                    "dimensions": {},
                    "count": 20,
                    "baseline_median_ms": 100.0,
                    "candidate_median_ms": 90.0,
                    "median_duration_reduction_pct": 10.0,
                    "baseline_p95_ms": 120.0,
                    "candidate_p95_ms": 110.0,
                    "p95_duration_reduction_pct": 8.333,
                }
            ],
        }

        print_comparison(comparison, stream)

        self.assertIn("baseline:  upstream", stream.getvalue())
        self.assertIn("device:    katre-x4 (X4)", stream.getvalue())
        self.assertIn("positive means candidate took less time", stream.getvalue())
        self.assertIn("+10.0%", stream.getvalue())


if __name__ == "__main__":
    unittest.main()
