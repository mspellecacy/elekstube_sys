"""Exercise the firmware's real clock/button state code on the host."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest


class FirmwareStateTests(unittest.TestCase):
    def test_timing_tools_start_stop_reset_background_and_rollover(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "timing-tools-test"
            subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(root / "src"),
                            str(root / "tests/timing_tools_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    def test_independent_usb_tcp_framing_and_overflow_recovery(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "json-lines-test"
            subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(root / "src"),
                            str(root / "tests/json_lines_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    def test_nixie_animation_bounds_and_display_byte_order(self):
        self._check_renderer("nixie")

    def test_flip_animation_bounds_idle_cards_and_display_byte_order(self):
        self._check_renderer("flip")

    def test_orbit_animation_bounds_transitions_cache_and_display_byte_order(self):
        self._check_renderer("orbit")

    def _check_renderer(self, name):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / f"{name}-renderer-test"
            subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(root / "src"), str(root / f"tests/{name}_renderer_test.cpp"),
                            "-o", str(binary)], check=True)
            # LeakSanitizer cannot inspect threads in traced development sandboxes.
            # Address/undefined checks still exercise the allocation-free renderer.
            subprocess.run([str(binary)], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    def test_ntp_startup_persistence_dst_and_offline_time(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "ntp-clock-test"
            subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "tests/ntp_stubs"), "-I", str(root / "src"),
                            str(root / "tests/ntp_clock_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_time_rollovers_and_button_bounce(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "clock-state-test"
            subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "src"), str(root / "tests/clock_state_test.cpp"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
