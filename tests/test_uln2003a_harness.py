"""Host tests: python3 -m unittest discover -s tests -v."""
import contextlib
import importlib.util
import io
import pathlib
import unittest
from unittest.mock import patch

SOURCE = (pathlib.Path(__file__).resolve().parents[1] /
          "layers/meta-jamesc/recipes-support/uln2003a-test/files/uln2003a-test.py")
spec = importlib.util.spec_from_file_location("harness", SOURCE)
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


class Attribute:
    def __init__(self, device, name):
        self.device, self.name = device, name

    def write_text(self, value):
        if self.name == "move" and self.device.busy:
            raise OSError("Device or resource busy")
        self.device.writes.append((self.name, value))

    def read_text(self):
        return self.device.states.pop(0)


class Device:
    def __init__(self, states=(), busy=False):
        self.states, self.busy, self.writes = list(states), busy, []

    def __truediv__(self, name):
        return Attribute(self, name)


def state(running, completed, remaining):
    return (f"running={running} direction=clockwise speed=128 "
            f"completed={completed} remaining={remaining}\n")


class HarnessTests(unittest.TestCase):
    def run_move(self, device):
        with contextlib.redirect_stdout(io.StringIO()):
            harness.move(device, "cw", 128, 8, 4096)

    @patch.object(harness.time, "sleep")
    def test_completion_and_coil_release(self, sleep):
        device = Device([state(1, 4, 4), state(0, 8, 0)])
        self.run_move(device)
        self.assertEqual(device.writes, [("move", "cw 128 8\n"), ("stop", "1\n")])

    def test_busy_does_not_stop_other_move(self):
        device = Device(busy=True)
        with self.assertRaises(OSError):
            self.run_move(device)
        self.assertEqual(device.writes, [])

    @patch.object(harness.time, "sleep", side_effect=KeyboardInterrupt)
    def test_interrupt_releases_coils(self, sleep):
        device = Device([state(1, 4, 4)])
        with self.assertRaises(KeyboardInterrupt):
            self.run_move(device)
        self.assertEqual(device.writes[-1], ("stop", "1\n"))

    def test_incomplete_move_fails_and_releases_coils(self):
        device = Device([state(0, 4, 0)])
        with self.assertRaisesRegex(RuntimeError, "before all steps"):
            self.run_move(device)
        self.assertEqual(device.writes[-1], ("stop", "1\n"))

    @patch.object(harness.time, "monotonic", side_effect=[0, 10, 10])
    def test_timeout_releases_coils(self, monotonic):
        device = Device([state(1, 0, 8)])
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            self.run_move(device)
        self.assertEqual(device.writes[-1], ("stop", "1\n"))

    def test_bad_status_releases_coils(self):
        device = Device(["invalid"])
        with self.assertRaises(ValueError):
            self.run_move(device)
        self.assertEqual(device.writes[-1], ("stop", "1\n"))


if __name__ == "__main__":
    unittest.main()
