"""System tests: the real simulator binaries talking over UDP on localhost.

Two kinds of clients are used against the C++ base station:
  * the C++ UE simulator, started as a separate process
  * a small Python UE built on the generated Python messages

The second one is the more interesting check, since it proves that two
independently generated code bases agree on the wire format.

CTest passes the binary paths in SIGFLOW_ENB and SIGFLOW_UE and sets
PYTHONPATH to the sigflow runtime plus the generated modules.
"""

from __future__ import annotations

import os
import queue
import re
import socket
import subprocess
import threading
import time
import unittest

from sigflow import wire

import mobility_messages as mobility
import session_messages as session

ENB = os.environ.get("SIGFLOW_ENB", "sigflow-enb")
UE = os.environ.get("SIGFLOW_UE", "sigflow-ue")
GERMAN_IMSI = 262011234567890
REGISTRY = {**session.MESSAGES, **mobility.MESSAGES}


class BaseStationProcess:
    """Runs sigflow-enb on a free port and collects its output."""

    def __init__(self, *extra_args: str) -> None:
        self.lines: queue.Queue[str] = queue.Queue()
        self.output: list[str] = []
        self.process = subprocess.Popen(
            [ENB, "--port", "0", *extra_args],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        # Reading in a thread means a hanging binary cannot block the test,
        # the queue gets a timeout instead.
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()
        ready = self.wait_for_line(r"ready on port (\d+)")
        self.port = int(ready.group(1))

    def _pump(self) -> None:
        assert self.process.stdout is not None
        with self.process.stdout:
            for line in self.process.stdout:
                self.lines.put(line.rstrip("\n"))

    def wait_for_line(self, pattern: str, timeout: float = 5.0) -> re.Match[str]:
        deadline = time.monotonic() + timeout
        regex = re.compile(pattern)
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                break
            self.output.append(line)
            if match := regex.search(line):
                return match
        raise AssertionError(f"no line matching {pattern!r}, output so far:\n" + "\n".join(self.output))

    def stop(self) -> int:
        if self.process.poll() is None:
            self.process.terminate()
        try:
            code = self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            code = self.process.wait()
        self._reader.join(timeout=5)
        return code


class PythonUe:
    """Minimal UE written against the generated Python messages."""

    def __init__(self, port: int) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(2.0)
        self.address = ("127.0.0.1", port)
        self.sequence = 0

    def close(self) -> None:
        self.sock.close()

    def send(self, message) -> None:
        self.sock.sendto(wire.encode_message(message, self.sequence), self.address)
        self.sequence += 1

    def send_raw(self, data: bytes) -> None:
        self.sock.sendto(data, self.address)

    def receive(self):
        data, _ = self.sock.recvfrom(70000)
        return wire.decode_message(wire.decode_frame(data), REGISTRY)

    def attach(self, imsi: int = GERMAN_IMSI) -> session.AttachResponse:
        self.send(session.AttachRequest(
            imsi=imsi,
            category=session.UeCategory.CAT_1,
            device_name="python-ue",
            supported_bands=[3, 20],
        ))
        response = self.receive()
        assert isinstance(response, session.AttachResponse), response
        return response


class AttachFlowTest(unittest.TestCase):
    def setUp(self) -> None:
        self.enb = BaseStationProcess()
        self.addCleanup(self.enb.stop)

    def python_ue(self) -> PythonUe:
        ue = PythonUe(self.enb.port)
        self.addCleanup(ue.close)
        return ue

    def test_cpp_ue_attaches_hands_over_and_detaches(self) -> None:
        result = subprocess.run(
            [UE, "--port", str(self.enb.port), "--interval-ms", "20"],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("handover to cell 102", result.stdout)
        self.enb.wait_for_line(r"detached ue_id=1 cause=USER_REQUEST after 1 handover")

    def test_python_ue_against_cpp_base_station(self) -> None:
        ue = self.python_ue()
        response = ue.attach()
        self.assertEqual(response.result, session.AttachResult.ACCEPTED)
        self.assertEqual(response.serving_cell_id, 101)
        ue_id = response.ue_id

        # Neighbour 8 dB stronger than serving, well above the 3 dB margin.
        ue.send(mobility.MeasurementReport(
            ue_id=ue_id,
            serving=mobility.CellMeasurement(101, -1000, -120),
            neighbours=[mobility.CellMeasurement(102, -920, -90)],
        ))
        command = ue.receive()
        self.assertIsInstance(command, mobility.HandoverCommand)
        self.assertEqual((command.ue_id, command.target_cell_id, command.margin_db_x10), (ue_id, 102, 80))

        ue.send(session.DetachRequest(ue_id, session.DetachCause.POWER_OFF))
        self.assertEqual(ue.receive(), session.DetachAccept(ue_id))

    def test_small_margin_does_not_trigger_handover(self) -> None:
        ue = self.python_ue()
        ue_id = ue.attach().ue_id
        ue.send(mobility.MeasurementReport(
            ue_id=ue_id,
            serving=mobility.CellMeasurement(101, -900, -100),
            neighbours=[mobility.CellMeasurement(102, -880, -100)],  # only 2 dB better
        ))
        with self.assertRaises(socket.timeout):
            ue.sock.settimeout(0.3)
            ue.receive()

    def test_repeated_attach_keeps_the_same_ue_id(self) -> None:
        ue = self.python_ue()
        first = ue.attach()
        second = ue.attach()
        self.assertEqual(first.ue_id, second.ue_id)

    def test_foreign_network_is_rejected(self) -> None:
        result = subprocess.run(
            [UE, "--port", str(self.enb.port), "--imsi", "310150123456789"],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 3, result.stdout)
        self.assertIn("REJECTED_UNKNOWN_NETWORK", result.stdout)

    def test_garbage_datagrams_are_survived(self) -> None:
        ue = self.python_ue()
        ue.send_raw(b"\x00" * 10)
        ue.send_raw(b"SF definitely not a frame")
        valid_but_wrong = bytearray(wire.encode_message(session.DetachAccept(1), 0))
        valid_but_wrong[-1] ^= 0xFF  # broken crc
        ue.send_raw(bytes(valid_but_wrong))

        self.assertEqual(ue.attach().result, session.AttachResult.ACCEPTED)
        self.enb.stop()
        self.enb.wait_for_line(r"rejected datagrams=3")


class CongestionTest(unittest.TestCase):
    def test_full_cell_rejects_with_retry_hint(self) -> None:
        enb = BaseStationProcess("--max-ues", "1")
        self.addCleanup(enb.stop)
        first, second = PythonUe(enb.port), PythonUe(enb.port)
        self.addCleanup(first.close)
        self.addCleanup(second.close)

        self.assertEqual(first.attach(GERMAN_IMSI).result, session.AttachResult.ACCEPTED)
        rejected = second.attach(GERMAN_IMSI + 1)
        self.assertEqual(rejected.result, session.AttachResult.REJECTED_CONGESTION)
        self.assertIsNone(rejected.ue_id)
        self.assertEqual(rejected.retry_after_ms, 500)


if __name__ == "__main__":
    unittest.main()
