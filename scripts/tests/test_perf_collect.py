import io
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from perf_collect import (  # noqa: E402
    LineFramer,
    PerfRecordError,
    collect_records,
    parse_perf_line,
    summarize,
)


class PerfCollectTest(unittest.TestCase):
    def test_parses_noise_and_preserves_flat_optional_fields(self):
        lines = io.StringIO(
            "boot noise\r\n"
            'PERF {"v":1,"scenario":"book_open","iteration":1,"duration_us":1500000,'
            '"cache":"hit","managed":true,"heap_free_bytes":76000}\r\n'
        )

        records = collect_records(lines)

        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].duration_us, 1_500_000)
        self.assertEqual(records[0].fields["cache"], "hit")
        self.assertIs(records[0].fields["managed"], True)

    def test_frames_fragmented_and_multiple_lines(self):
        framer = LineFramer()

        self.assertEqual(framer.feed(b"PER"), [])
        self.assertEqual(
            framer.feed(b"F one\r\nPERF two\npartial"), ["PERF one", "PERF two"]
        )
        self.assertEqual(framer.feed(b" line\n"), ["partial line"])

    def test_rejects_invalid_contract_values(self):
        invalid = [
            'PERF {"v":2,"scenario":"page_turn","iteration":1,"duration_us":1}',
            'PERF {"v":1,"scenario":"page_turn","iteration":true,"duration_us":1}',
            'PERF {"v":1,"scenario":"page_turn","iteration":1,"duration_us":-1}',
            'PERF {"v":1,"scenario":"page_turn","iteration":1,"duration_us":1,"nested":{}}',
            'PERF {"v":1,"v":1,"scenario":"page_turn","iteration":1,"duration_us":1}',
        ]

        for line in invalid:
            with self.subTest(line=line), self.assertRaises(PerfRecordError):
                parse_perf_line(line)

    def test_summarizes_median_and_nearest_rank_p95_by_dimensions(self):
        lines = [
            f'PERF {{"v":1,"scenario":"page_turn","iteration":{index},'
            f'"duration_us":{duration},"direction":"forward"}}\n'
            for index, duration in enumerate((100_000, 200_000, 300_000, 400_000), 1)
        ]

        summary = summarize(collect_records(lines))[0]

        self.assertEqual(summary["count"], 4)
        self.assertEqual(summary["median_ms"], 250.0)
        self.assertEqual(summary["p95_ms"], 400.0)
        self.assertEqual(summary["dimensions"], {"direction": "forward"})

    def test_scenario_filter_excludes_other_records(self):
        lines = [
            'PERF {"v":1,"scenario":"book_open","iteration":1,"duration_us":1}\n',
            'PERF {"v":1,"scenario":"page_turn","iteration":1,"duration_us":2}\n',
        ]

        records = collect_records(lines, scenario="page_turn")

        self.assertEqual([record.scenario for record in records], ["page_turn"])


if __name__ == "__main__":
    unittest.main()
