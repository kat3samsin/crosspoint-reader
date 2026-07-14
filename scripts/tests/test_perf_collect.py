import io
import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from perf_collect import (  # noqa: E402
    LineFramer,
    PerfRecordError,
    collect_serial,
    collect_records,
    parse_perf_line,
    summarize,
)


class FakeSerialException(OSError):
    pass


class FakeSerialConnection:
    def __init__(self, events):
        self.events = list(events)

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


class FakeSerialModule:
    SerialException = FakeSerialException

    def __init__(self, sessions):
        self.sessions = list(sessions)
        self.open_count = 0

    def Serial(self, *_args, **_kwargs):  # noqa: N802
        self.open_count += 1
        if not self.sessions:
            raise AssertionError("unexpected serial open")
        session = self.sessions.pop(0)
        if isinstance(session, BaseException):
            raise session
        return FakeSerialConnection(session)


def boot_record() -> bytes:
    return (
        'PERF {"v":1,"scenario":"boot_to_home","iteration":1,'
        '"duration_us":1000,"heap_free_bytes":76000}\n'
    ).encode()


class PerfCollectTest(unittest.TestCase):
    def test_parses_noise_and_preserves_flat_optional_fields(self):
        lines = io.StringIO(
            "boot noise\r\n"
            'PERF {"v":1,"scenario":"book_open","iteration":1,"duration_us":1500000,'
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
            'PERF {"v":2,"scenario":"boot_to_home","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":1,"scenario":"boot_to_home","iteration":true,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":1,"scenario":"boot_to_home","iteration":1,"duration_us":-1,'
            '"heap_free_bytes":1}',
            'PERF {"v":1,"scenario":"boot_to_home","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"nested":{}}',
            'PERF {"v":1,"v":1,"scenario":"boot_to_home","iteration":1,'
            '"duration_us":1,"heap_free_bytes":1}',
        ]

        for line in invalid:
            with self.subTest(line=line), self.assertRaises(PerfRecordError):
                parse_perf_line(line)

    def test_rejects_unknown_scenarios_and_invalid_required_dimensions(self):
        invalid = [
            'PERF {"v":1,"scenario":"unknown","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":1,"scenario":"boot_to_home","iteration":0,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":1,"scenario":"boot_to_home","iteration":1,"duration_us":1}',
            'PERF {"v":1,"scenario":"boot_to_home","iteration":1,"duration_us":1,'
            '"heap_free_bytes":true}',
            'PERF {"v":1,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"managed":false}',
            'PERF {"v":1,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"epub_index_cache":"warm","managed":false}',
            'PERF {"v":1,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"epub_index_cache":"hit","managed":0}',
            'PERF {"v":1,"scenario":"epub_load","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":1,"scenario":"readest_probe","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1}',
            'PERF {"v":1,"scenario":"page_turn_in_section","iteration":1,'
            '"duration_us":1,"heap_free_bytes":1,"direction":"next"}',
            'PERF {"v":1,"scenario":"page_turn","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"direction":"forward"}',
        ]

        for line in invalid:
            with self.subTest(line=line), self.assertRaises(PerfRecordError):
                parse_perf_line(line)

    def test_accepts_every_known_v1_scenario(self):
        lines = [
            boot_record().decode(),
            'PERF {"v":1,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"epub_index_cache":"hit","managed":false}',
            'PERF {"v":1,"scenario":"epub_load","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"epub_index_cache":"miss"}',
            'PERF {"v":1,"scenario":"readest_probe","iteration":1,"duration_us":1,'
            '"heap_free_bytes":1,"managed":true}',
            'PERF {"v":1,"scenario":"page_turn_in_section","iteration":1,'
            '"duration_us":1,"heap_free_bytes":1,"direction":"backward"}',
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

    def test_summarizes_median_and_nearest_rank_p95_by_dimensions(self):
        lines = [
            f'PERF {{"v":1,"scenario":"page_turn_in_section","iteration":{index},'
            f'"duration_us":{duration},"direction":"forward","heap_free_bytes":76000}}\n'
            for index, duration in enumerate((100_000, 200_000, 300_000, 400_000), 1)
        ]

        summary = summarize(collect_records(lines))[0]

        self.assertEqual(summary["count"], 4)
        self.assertEqual(summary["median_ms"], 250.0)
        self.assertEqual(summary["p95_ms"], 400.0)
        self.assertEqual(summary["dimensions"], {"direction": "forward"})

    def test_scenario_filter_excludes_other_records(self):
        lines = [
            'PERF {"v":1,"scenario":"book_open","iteration":1,"duration_us":1,'
            '"epub_index_cache":"hit","managed":false,"heap_free_bytes":76000}\n',
            'PERF {"v":1,"scenario":"page_turn_in_section","iteration":1,'
            '"duration_us":2,"direction":"forward","heap_free_bytes":76000}\n',
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
            [[b'PERF {"v":1', FakeSerialException("device reset")]]
        )

        with mock.patch.dict(sys.modules, {"serial": fake_serial}), mock.patch(
            "perf_collect.time.sleep"
        ), self.assertRaisesRegex(PerfRecordError, "partial PERF"):
            list(collect_serial("fake-port", 115200, 1, False))


if __name__ == "__main__":
    unittest.main()
