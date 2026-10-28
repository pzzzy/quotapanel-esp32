"""Cross-language parity against the established quota estimator."""

import importlib.util, random, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("forecast", ROOT / "tools/forecast.py")
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


class NativeForecast(unittest.TestCase):
    def test_realistic_histories_and_discontinuities(self):
        rng = random.Random(20261026)
        now = 1793066000
        cases = []
        for i in range(350):
            samples = []
            t = now - rng.randrange(0, 90000)
            used = rng.randrange(0, 60)
            while t < now:
                samples.append((t, used))
                t += rng.choice([30, 60, 120, 300, 600, 900])
                used = min(100, used + rng.choice([0, 0, 0, 1, 2]))
                if rng.random() < 0.005:
                    used = max(0, used - 10)
            samples.append((now, used))
            cases.append(
                (samples, 100 - used, now + rng.choice([300, 3600, 86400, 604800]))
            )
        cases += [
            ([], 50, now + 100),
            (
                [
                    (now - 3600, 10),
                    (now - 3000, 10),
                    (now - 2400, 10),
                    (now - 1800, 10),
                    (now - 1200, 10),
                    (now - 600, 10),
                    (now, 10),
                ],
                90,
                now + 300,
            ),
            ([(now - 600, 50), (now - 600, 20), (now, 30)], 70, now - 1),
        ]
        # Explicit fallback, correction, expiry, quiet and resumed-rate cases.
        for gap in (601, 900, 3600, 21600, 21601):
            prior = [
                (now - gap - 10800 + i, 10 + i / 360) for i in range(0, 10801, 300)
            ]
            for used in (1, 40, 50):
                cases.append((prior + [(now, used)], 100 - used, now + 86400))
        prior = [(now - 8100 - 10800 + i, 10 + i / 360) for i in range(0, 10801, 300)]
        quiet = [(now - 4500 + i, 50) for i in range(0, 3601, 300)]
        cases.append((prior + quiet + [(now, 50)], 50, now + 86400))
        cases.append(
            (
                prior + [(now - 900 + i, 50 + i / 300) for i in range(0, 901, 30)],
                47,
                now + 86400,
            )
        )
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(Path(tmp) / "forecast")
            subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-fsanitize=address,undefined",
                    str(ROOT / "tests/forecast_native_harness.cpp"),
                    str(ROOT / "firmware/src/native_forecast.cpp"),
                    "-o",
                    exe,
                ],
                check=True,
            )
            lines = []
            for samples, remaining, reset in cases:
                lines.append(
                    f"{now} {reset} {remaining} {len(samples)} "
                    + " ".join(f"{t} {u}" for t, u in samples)
                )
            actual = subprocess.run(
                [exe],
                input="\n".join(lines) + "\n",
                text=True,
                capture_output=True,
                check=True,
            ).stdout.splitlines()
        self.assertEqual(len(actual), len(cases))
        for row, (samples, remaining, reset) in zip(actual, cases):
            e = reference.estimate(samples, remaining, reset, now)
            v = row.split()
            self.assertEqual(v[:2], [e["state"], e["confidence"]])
            for index, key in enumerate(
                ["seconds", "lowSeconds", "highSeconds", "rate"], 2
            ):
                expected = -1 if e[key] is None else e[key]
                self.assertAlmostEqual(
                    float(v[index]), expected, delta=0.001 if key == "rate" else 1
                )
            self.assertEqual(int(v[6]), e["coverageSeconds"])
            self.assertEqual(v[7] == "1", e["resetFirst"])
            self.assertEqual(v[8] == "1", e["resetPossible"])
            self.assertEqual(v[9] == "1", e["historical"])
            self.assertEqual(int(v[10]), e["historyAgeSeconds"])


if __name__ == "__main__":
    unittest.main()
