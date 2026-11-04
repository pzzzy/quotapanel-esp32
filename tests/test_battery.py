import subprocess
import tempfile
import unittest
import array
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from generate_voices import mulaw


class Battery(unittest.TestCase):
    def test_compact_battery_voice_matches_public_pcm_asset(self):
        self.assertEqual([mulaw(x) for x in (0, -1, 32767, -32768)], [255, 127, 128, 0])
        with wave.open(str(ROOT / "assets/voice/battery_low.wav")) as wav:
            samples = array.array("h", wav.readframes(wav.getnframes()))
        if sys.byteorder != "little":
            samples.byteswap()
        encoded = (
            (ROOT / "firmware/src/voices.generated.h")
            .read_text()
            .split("voice_battery_low[] PROGMEM = {", 1)[1]
            .split("};", 1)[0]
        )
        actual = [int(x) for x in encoded.replace("\n", "").split(",") if x]
        self.assertEqual(actual, [mulaw(x) for x in samples])

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
