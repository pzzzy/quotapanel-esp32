import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class HistoryJournal(unittest.TestCase):
    def test_reboot_torn_writes_wrap_corruption_and_forecast_restore(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(Path(tmp) / "history")
            subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-fsanitize=address,undefined",
                    str(ROOT / "tests/history_journal_harness.cpp"),
                    str(ROOT / "firmware/src/native_forecast.cpp"),
                    "-o",
                    exe,
                ],
                check=True,
            )
            subprocess.run([exe], check=True)
