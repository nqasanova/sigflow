"""End to end tests: compile the real schemas and use the generated code."""

import importlib
import io
import sys
import tempfile
import unittest
import unittest.mock
from contextlib import redirect_stderr
from pathlib import Path

from msgc import parse_file, validate
from msgc.__main__ import main
from msgc.backends import cpp, python

REPO = Path(__file__).resolve().parents[3]
SCHEMAS = sorted((REPO / "schemas").glob("*.msg"))


class PythonBackendTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        out = Path(cls.tmp.name)
        schemas = [parse_file(p) for p in SCHEMAS]
        validate(schemas)
        for schema in schemas:
            for name, content in python.generate(schema).items():
                (out / name).write_text(content)

        sys.path[:0] = [str(out), str(REPO / "python")]
        cls.session = importlib.import_module("session_messages")
        cls.mobility = importlib.import_module("mobility_messages")
        cls.wire = importlib.import_module("sigflow.wire")

    @classmethod
    def tearDownClass(cls):
        sys.path.remove(cls.tmp.name)
        sys.path.remove(str(REPO / "python"))
        for name in ("session_messages", "mobility_messages"):
            sys.modules.pop(name, None)
        cls.tmp.cleanup()

    def test_round_trip_through_a_frame(self):
        s = self.session
        request = s.AttachRequest(
            imsi=262011234567890,
            category=s.UeCategory.CAT_4,
            device_name="pixel",
            supported_bands=[1, 3, 20],
            requested_bandwidth_mhz=20,
        )
        frame = self.wire.decode_frame(self.wire.encode_message(request, 7))
        self.assertEqual(frame.sequence, 7)
        self.assertEqual(self.wire.decode_message(frame, s.MESSAGES), request)

    def test_same_bytes_as_cpp_golden_frame(self):
        # Same expectation as tests/unit/generated_messages_test.cpp.
        s = self.session
        request = s.AttachRequest(262011234567890, s.UeCategory.CAT_4, "pixel", [1, 3, 20], 20)
        self.assertEqual(
            self.wire.encode_message(request, 7).hex(),
            "53460100010100070000002b0100080000ee4c3e9046d202000104030005706978656c"
            "040002000104000200030400020014050002001457ea",
        )

    def test_nested_and_negative_values(self):
        m = self.mobility
        report = m.MeasurementReport(
            ue_id=3,
            serving=m.CellMeasurement(101, -955, -105),
            neighbours=[m.CellMeasurement(102, -900, -80)],
        )
        self.assertEqual(m.MeasurementReport.decode(report.encode()), report)

    def test_missing_required_field(self):
        with self.assertRaisesRegex(self.wire.DecodeError, "DetachRequest.cause"):
            writer = self.wire.TlvWriter()
            writer.put_int(1, 5, 4, signed=False)
            self.session.DetachRequest.decode(writer.getvalue())

    def test_invalid_enum_value_names_the_field(self):
        writer = self.wire.TlvWriter()
        writer.put_int(1, 9, 1, signed=False)
        with self.assertRaisesRegex(self.wire.DecodeError, r"AttachResponse.result: unknown AttachResult value 9"):
            self.session.AttachResponse.decode(writer.getvalue())

    def test_unknown_tags_are_ignored(self):
        encoded = self.session.DetachAccept(ue_id=4).encode()
        writer = self.wire.TlvWriter()
        writer.put_bytes(200, b"newer peer")
        decoded = self.session.DetachAccept.decode(encoded + writer.getvalue())
        self.assertEqual(decoded.ue_id, 4)

    def test_out_of_range_value_fails_on_encode(self):
        with self.assertRaises(OverflowError):
            self.session.DetachAccept(ue_id=2**32).encode()

    def test_frame_corruption_is_detected(self):
        frame = bytearray(self.wire.encode_message(self.session.DetachAccept(1), 0))
        frame[-3] ^= 0x01
        with self.assertRaisesRegex(self.wire.DecodeError, "crc"):
            self.wire.decode_frame(bytes(frame))


class CppBackendTest(unittest.TestCase):
    def setUp(self):
        self.schema = parse_file(REPO / "schemas" / "session.msg")
        self.files = cpp.generate(self.schema)

    def test_file_names(self):
        self.assertEqual(sorted(self.files), ["session_messages.cpp", "session_messages.hpp"])

    def test_header_contents(self):
        header = self.files["session_messages.hpp"]
        self.assertIn("namespace ran::session {", header)
        self.assertIn("enum class AttachResult : std::uint8_t {", header)
        self.assertIn("kRejectedCongestion = 2,", header)
        self.assertIn("static constexpr std::uint16_t kMessageId = 0x0101;", header)
        self.assertIn("std::optional<std::uint16_t> requested_bandwidth_mhz;", header)
        self.assertIn("std::vector<std::uint16_t> supported_bands;", header)
        # Required enum fields start at the first declared value.
        self.assertIn("UeCategory category{UeCategory::kCatM1};", header)

    def test_output_is_deterministic(self):
        self.assertEqual(self.files, cpp.generate(parse_file(REPO / "schemas" / "session.msg")))


class CliTest(unittest.TestCase):
    def test_check_mode_reports_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.msg"
            bad.write_text("namespace x;\nmessage M = 1 { 1: nope a; }\n")
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                self.assertEqual(main(["--check", str(bad)]), 1)
            self.assertIn("bad.msg:2:17: error: unknown type 'nope'", stderr.getvalue())

    def test_unchanged_files_are_not_rewritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = ["--lang", "cpp", "--out-dir", tmp, *map(str, SCHEMAS)]
            with redirect_stderr(io.StringIO()), unittest.mock.patch("sys.stdout", new=io.StringIO()):
                main(args)
                header = Path(tmp) / "session_messages.hpp"
                first_mtime = header.stat().st_mtime_ns
                main(args)
            self.assertEqual(header.stat().st_mtime_ns, first_mtime)


if __name__ == "__main__":
    unittest.main()
