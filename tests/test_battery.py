import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Battery(unittest.TestCase):
    def test_voltage_estimate_and_trend_fail_safely(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(Path(tmp) / "battery")
            subprocess.run(
                [
                    "c++",
                    "-std=c++11",
                    "-fsanitize=address,undefined",
                    str(ROOT / "tests/battery_harness.cpp"),
                    "-o",
                    exe,
                ],
                check=True,
            )
            subprocess.run([exe], check=True)
