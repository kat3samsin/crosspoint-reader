import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from perf_collect import (  # noqa: E402
    LineFramer,
    PerfRecordError,
    build_benchmark_context,
    collect_serial,
    collect_records,
    main,
    parse_perf_line,
    summarize,
)


class FakeSerialException(OSError):
    pass


class FakeSerialConnection:
    def __init__(self, events):
        self.events = list(events)
        self.writes = []
        self.reset_count = 0
        self.flush_count = 0

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    @property
    def in_waiting(self):
        if not self.events:
            return 1
        event = self.events[0]
        return len(event) if isinstance(event, bytes) else 1

    def read(self, _size):
        if not self.events:
            raise FakeSerialException("device disconnected")
        event = self.events.pop(0)
        if isinstance(event, BaseException):
            raise event
        return event

    def reset_input_buffer(self):
        self.reset_count += 1

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def flush(self):
        self.flush_count += 1


class FakeSerialModule:
    SerialException = FakeSerialException

    def __init__(self, sessions):
        self.sessions = list(sessions)
        self.open_count = 0
        self.connections = []

    def Serial(self, *_args, **_kwargs):  # noqa: N802
        self.open_count += 1
        if not self.sessions:
            raise AssertionError("unexpected serial open")
        session = self.sessions.pop(0)
        if isinstance(session, BaseException):
            raise session
        connection = FakeSerialConnection(session)
        self.connections.append(connection)
        return connection


def boot_record() -> bytes:
    return (
        'PERF {"v":2,"scenario":"boot_to_home","iteration":1,'
        '"duration_us":1000,"heap_free_bytes":76000}\n'
    ).encode()


def page_record(
    iteration=1,
    duration_us=1000,
    *,
    direction="forward",
    spine_index=2,
    from_page=10,
    to_page=11,
    font_size=3,
    text_antialiasing=True,
    refresh_mode="fast",
):
    return "PERF " + json.dumps(
        {
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
        },
        separators=(",", ":"),
    )


def automated_page_turn_events(*, done_page=10):
    events = [
        b"PERF_CONTROL page_turn_auto queued\n",
        b"PERF_CONTROL page_turn_auto started count=20 settle_ms=3000 "
        b"spine=2 page_a=10 page_b=11\n",
    ]
    for iteration in range(1, 21):
        forward = iteration % 2 == 1
        events.append(
            (
                page_record(
                    iteration,
                    direction="forward" if forward else "backward",
                    from_page=10 if forward else 11,
                    to_page=11 if forward else 10,
                    refresh_mode="half" if iteration in (1, 11) else "fast",
                )
                + "\n"
            ).encode()
        )
    events.append(
        f"PERF_CONTROL page_turn_auto done count=20 spine=2 page={done_page}\n".encode()
    )
    return events


def provenance_args():
    return [
        "--device-id",
        "katre-x4",
        "--device-model",
        "X4",
        "--protocol-id",
        "katre-x4-benchmark-v1",
    ]


class PerfCollectTest(unittest.TestCase):
    def test_parses_noise_and_preserves_flat_optional_fields(self):
        lines = io.StringIO(
            "boot noise\r\n"
            'PERF {"v":2,"scenario":"book_open","iteration":1,"duration_us":1500000,'
            '"epub_index_cache":"hit","managed":true,"heap_free_bytes":76000,'
            '"firmware_sha":"abc1234"}\r\n'
        )

        records = collect_records(lines)

        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].duration_us, 1_500_000)
        self.assertEqual(records[0].fields["epub_index_cache"], "hit")
        self.assertIs(records[0].fields["managed"], True)
        self.assertEqual(records[0].fields["firmware_sha"], "abc1234")

    def test_frames_fragmented_and_multiple_lines(self):
        framer = LineFramer()

        self.assertEqual(framer.feed(b"PER"), [])
        self.assertEqual(
            framer.feed(b"F one\r\nPERF two\npartial"), ["PERF one", "PERF two"]
        )
        self.assertEqual(framer.feed(b" line\n"), ["partial line"])

    def test_rejects_invalid_contract_values(self):
        invalid = [
            'PERF {"v":1,"scenario":"boot_to_home","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":2,"scenario":"boot_to_home","iteration":true,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":2,"scenario":"boot_to_home","iteration":1,"duration_us":-1,'
            '"heap_free_bytes":1}',
            'PERF {"v":2,"scenario":"boot_to_home","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"nested":{}}',
            'PERF {"v":2,"v":2,"scenario":"boot_to_home","iteration":1,'
            '"duration_us":1,"heap_free_bytes":1}',
        ]

        for line in invalid:
            with self.subTest(line=line), self.assertRaises(PerfRecordError):
                parse_perf_line(line)

    def test_rejects_unknown_scenarios_and_invalid_required_dimensions(self):
        invalid = [
            'PERF {"v":2,"scenario":"unknown","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":2,"scenario":"boot_to_home","iteration":0,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":2,"scenario":"boot_to_home","iteration":1,"duration_us":1}',
            'PERF {"v":2,"scenario":"boot_to_home","iteration":1,"duration_us":1,'
            '"heap_free_bytes":true}',
            'PERF {"v":2,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"managed":false}',
            'PERF {"v":2,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"epub_index_cache":"warm","managed":false}',
            'PERF {"v":2,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"epub_index_cache":"hit","managed":0}',
            'PERF {"v":2,"scenario":"epub_load","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":2,"scenario":"readest_probe","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":2,"scenario":"page_turn_in_section","iteration":1,'
            '"duration_us":1,"heap_free_bytes":1,"direction":"next"}',
            'PERF {"v":2,"scenario":"page_turn","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"direction":"forward"}',
        ]

        for line in invalid:
            with self.subTest(line=line), self.assertRaises(PerfRecordError):
                parse_perf_line(line)

    def test_accepts_every_known_v2_scenario(self):
        lines = [
            boot_record().decode(),
            'PERF {"v":2,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"epub_index_cache":"hit","managed":false}',
            'PERF {"v":2,"scenario":"epub_load","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"epub_index_cache":"miss"}',
            'PERF {"v":2,"scenario":"readest_probe","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"managed":true}',
            page_record(direction="backward", from_page=11, to_page=10),
        ]

        records = collect_records(lines)

        self.assertEqual(
            [record.scenario for record in records],
            [
                "boot_to_home",
                "book_open",
                "epub_load",
                "readest_probe",
                "page_turn_in_section",
            ],
        )

    def test_page_turn_requires_and_strictly_validates_context_fields(self):
        payload = json.loads(page_record()[5:])
        required = (
            "spine_index",
            "from_page",
            "to_page",
            "font_size",
            "text_antialiasing",
            "refresh_mode",
        )
        invalid_values = {
            "spine_index": -1,
            "from_page": True,
            "to_page": "11",
            "font_size": 4,
            "text_antialiasing": 1,
            "refresh_mode": "unknown",
        }

        for field in required:
            missing = dict(payload)
            del missing[field]
            line = "PERF " + json.dumps(missing, separators=(",", ":"))
            with self.subTest(missing=field), self.assertRaises(PerfRecordError):
                parse_perf_line(line)

        for field, value in invalid_values.items():
            invalid = {**payload, field: value}
            line = "PERF " + json.dumps(invalid, separators=(",", ":"))
            with self.subTest(invalid=field), self.assertRaises(PerfRecordError):
                parse_perf_line(line)

        with self.assertRaisesRegex(PerfRecordError, "refresh_mode must be one of"):
            parse_perf_line(page_record(refresh_mode="image"))

        with self.assertRaisesRegex(PerfRecordError, "advance exactly one"):
            parse_perf_line(page_record(to_page=12))
        with self.assertRaisesRegex(PerfRecordError, "retreat exactly one"):
            parse_perf_line(page_record(direction="backward", from_page=11, to_page=11))

    def test_page_context_rejects_iteration_gaps_and_mid_run_setting_changes(self):
        valid = collect_records(
            [
                page_record(4, from_page=10, to_page=11),
                page_record(5, direction="backward", from_page=11, to_page=10),
            ]
        )
        context = build_benchmark_context(valid)
        self.assertEqual(context["page_turn_in_section"]["font_size"], 3)
        self.assertEqual(context["page_turn_in_section"]["trace"][1]["to_page"], 10)
        self.assertNotIn("iteration", context["page_turn_in_section"]["trace"][0])

        gaps = collect_records(
            [
                page_record(4, from_page=10, to_page=11),
                page_record(6, direction="backward", from_page=11, to_page=10),
            ]
        )
        with self.assertRaisesRegex(PerfRecordError, "consecutive"):
            build_benchmark_context(gaps)

        for changed_field, changed_value in (
            ("font_size", 2),
            ("text_antialiasing", False),
        ):
            kwargs = {changed_field: changed_value}
            mixed = collect_records(
                [
                    page_record(4, from_page=10, to_page=11),
                    page_record(
                        5,
                        direction="backward",
                        from_page=11,
                        to_page=10,
                        **kwargs,
                    ),
                ]
            )
            with self.subTest(field=changed_field), self.assertRaisesRegex(
                PerfRecordError, "changed within the report"
            ):
                build_benchmark_context(mixed)

        non_alternating = collect_records(
            [
                page_record(4, from_page=10, to_page=11),
                page_record(5, from_page=11, to_page=12),
            ]
        )
        with self.assertRaisesRegex(PerfRecordError, "exactly two pages"):
            build_benchmark_context(non_alternating)

        odd_trace = collect_records(
            [
                page_record(4, from_page=10, to_page=11),
                page_record(5, direction="backward", from_page=11, to_page=10),
                page_record(6, from_page=10, to_page=11),
            ]
        )
        with self.assertRaisesRegex(PerfRecordError, "even alternating"):
            build_benchmark_context(odd_trace)

    def test_summarizes_median_and_nearest_rank_p95_by_dimensions(self):
        lines = [
            page_record(
                index,
                duration,
                from_page=9 + index,
                to_page=10 + index,
                refresh_mode="half",
            )
            for index, duration in enumerate((100_000, 200_000, 300_000, 400_000), 1)
        ]

        summary = summarize(collect_records(lines))[0]

        self.assertEqual(summary["count"], 4)
        self.assertEqual(summary["median_ms"], 250.0)
        self.assertEqual(summary["p95_ms"], 400.0)
        self.assertEqual(summary["dimensions"], {})

    def test_summarizes_alternating_modes_as_one_page_turn_sample_set(self):
        lines = [
            page_record(
                index,
                direction="forward" if index % 2 else "backward",
                from_page=10 if index % 2 else 11,
                to_page=11 if index % 2 else 10,
                refresh_mode="half" if index in (5, 15) else "fast",
            )
            for index in range(1, 21)
        ]

        summaries = summarize(collect_records(lines))

        self.assertEqual(len(summaries), 1)
        self.assertEqual(summaries[0]["count"], 20)
        self.assertEqual(summaries[0]["dimensions"], {})

    def test_scenario_filter_excludes_other_records(self):
        lines = [
            'PERF {"v":2,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"epub_index_cache":"hit","managed":false,"heap_free_bytes":76000}\n',
            page_record(),
        ]

        records = collect_records(lines, scenario="page_turn_in_section")

        self.assertEqual(
            [record.scenario for record in records], ["page_turn_in_section"]
        )

    def test_serial_retries_when_port_is_initially_absent(self):
        fake_serial = FakeSerialModule(
            [FakeSerialException("port not found"), [boot_record()]]
        )

        with mock.patch.dict(sys.modules, {"serial": fake_serial}), mock.patch(
            "perf_collect.time.sleep"
        ) as sleep:
            records = collect_records(
                collect_serial("fake-port", 115200, 1, False), samples=1
            )

        self.assertEqual(len(records), 1)
        self.assertEqual(fake_serial.open_count, 2)
        sleep.assert_called_once()

    def test_serial_reconnects_across_five_sessions_and_resets_framing(self):
        sessions = []
        for index in range(5):
            events = [boot_record()]
            if index < 4:
                events.extend([b"ordinary partial", OSError("device reset")])
            sessions.append(events)
        fake_serial = FakeSerialModule(sessions)

        with mock.patch.dict(sys.modules, {"serial": fake_serial}), mock.patch(
            "perf_collect.time.sleep"
        ) as sleep:
            records = collect_records(
                collect_serial("fake-port", 115200, 5, False), samples=5
            )

        self.assertEqual(len(records), 5)
        self.assertEqual(fake_serial.open_count, 5)
        self.assertEqual(sleep.call_count, 4)

    def test_serial_rejects_partial_perf_record_on_disconnect(self):
        fake_serial = FakeSerialModule(
            [[b'PERF {"v":2', FakeSerialException("device reset")]]
        )

        with mock.patch.dict(sys.modules, {"serial": fake_serial}), mock.patch(
            "perf_collect.time.sleep"
        ), self.assertRaisesRegex(PerfRecordError, "partial PERF"):
            list(collect_serial("fake-port", 115200, 1, False))

    def test_serial_page_turn_driver_retries_only_before_command_is_sent(self):
        fake_serial = FakeSerialModule(
            [
                FakeSerialException("port not found"),
                [(page_record(99) + "\n").encode(), *automated_page_turn_events()],
            ]
        )

        with mock.patch.dict(sys.modules, {"serial": fake_serial}), mock.patch(
            "perf_collect.time.sleep"
        ):
            records = collect_records(
                collect_serial("fake-port", 115200, 5, False, True), samples=20
            )

        self.assertEqual(len(records), 20)
        self.assertEqual(records[0].iteration, 1)
        self.assertEqual(fake_serial.open_count, 2)
        self.assertEqual(fake_serial.connections[0].reset_count, 1)
        self.assertEqual(
            fake_serial.connections[0].writes, [b"CMD:PERF_PAGE_TURNS_20\n"]
        )
        self.assertEqual(fake_serial.connections[0].flush_count, 1)

    def test_serial_page_turn_driver_rejects_disconnect_after_command(self):
        fake_serial = FakeSerialModule(
            [[FakeSerialException("device reset")], automated_page_turn_events()]
        )

        with mock.patch.dict(sys.modules, {"serial": fake_serial}), mock.patch(
            "perf_collect.time.sleep"
        ), self.assertRaisesRegex(PerfRecordError, "disconnected after"):
            list(collect_serial("fake-port", 115200, 5, False, True))

        self.assertEqual(fake_serial.open_count, 1)

    def test_serial_page_turn_driver_requires_valid_done_marker(self):
        fake_serial = FakeSerialModule([automated_page_turn_events(done_page=11)])

        with mock.patch.dict(
            sys.modules, {"serial": fake_serial}
        ), self.assertRaisesRegex(PerfRecordError, "finish on page A"):
            list(collect_serial("fake-port", 115200, 5, False, True))

    def test_serial_page_turn_driver_requires_queued_before_started(self):
        events = automated_page_turn_events()
        events.pop(0)
        fake_serial = FakeSerialModule([events])

        with mock.patch.dict(
            sys.modules, {"serial": fake_serial}
        ), self.assertRaisesRegex(PerfRecordError, "started before queued"):
            list(collect_serial("fake-port", 115200, 5, False, True))

    def test_page_turn_driver_requires_exact_live_scenario(self):
        with redirect_stderr(io.StringIO()):
            self.assertEqual(main(["--drive-page-turns"]), 2)
            self.assertEqual(
                main(
                    [
                        "--port",
                        "fake-port",
                        "--scenario",
                        "page_turn_in_section",
                        "--samples",
                        "19",
                        "--drive-page-turns",
                    ]
                ),
                2,
            )

    def test_output_report_requires_and_records_structured_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            serial_log = root / "serial.log"
            output = root / "report.json"
            serial_log.write_bytes(boot_record())

            with redirect_stderr(io.StringIO()):
                self.assertEqual(main([str(serial_log), "--output", str(output)]), 2)
                self.assertEqual(
                    main(
                        [
                            str(serial_log),
                            "--label",
                            "",
                            *provenance_args(),
                            "--output",
                            str(output),
                        ]
                    ),
                    2,
                )
                self.assertEqual(
                    main(
                        [
                            str(serial_log),
                            "--label",
                            "baseline-sha-x4",
                            *provenance_args(),
                            "--output",
                            str(output),
                        ]
                    ),
                    0,
                )

            report = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(report["schema"], 3)
        self.assertEqual(report["benchmark_context"], {})
        self.assertEqual(
            report["provenance"],
            {
                "device_id": "katre-x4",
                "device_model": "X4",
                "protocol_id": "katre-x4-benchmark-v1",
            },
        )

    def test_output_report_does_not_overwrite_an_input_log_hardlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            serial_log = root / "serial.log"
            output_alias = root / "report.json"
            serial_log.write_bytes(boot_record())
            os.link(serial_log, output_alias)
            original = serial_log.read_bytes()

            with redirect_stderr(io.StringIO()):
                self.assertEqual(
                    main(
                        [
                            str(serial_log),
                            "--label",
                            "baseline-sha-x4",
                            *provenance_args(),
                            "--output",
                            str(output_alias),
                        ]
                    ),
                    2,
                )

            self.assertEqual(serial_log.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
