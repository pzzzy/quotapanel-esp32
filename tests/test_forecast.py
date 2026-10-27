import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from forecast import estimate, History

T = 2000000


def ramp(hours=3, rate=10, start=10):
    return [
        (T - hours * 3600 + i, start + rate * i / 3600)
        for i in range(0, int(hours * 3600) + 1, 60)
    ]


class TestForecast(unittest.TestCase):
    def test_constant_rate_and_range_contains_truth(self):
        f = estimate(ramp(), 60, T + 7 * 86400, T)
        self.assertAlmostEqual(f["rate"], 10, places=2)
        self.assertAlmostEqual(f["seconds"], 6 * 3600, delta=1)
        self.assertLess(f["lowSeconds"], f["seconds"])
        self.assertGreater(f["highSeconds"], f["seconds"])

    def test_integer_quota_steps_do_not_create_instant_burn(self):
        f = estimate([(t, int(u)) for t, u in ramp()], 60, T + 7 * 86400, T)
        self.assertAlmostEqual(f["rate"], 10, delta=2)
        self.assertGreater(f["seconds"], 4 * 3600)

    def test_repeated_samples_and_single_quantum_need_history(self):
        self.assertEqual(
            estimate([(T - 900, 10), (T, 11)], 89, T + 86400, T)["state"], "learning"
        )

    def test_quiet_is_not_infinite_runway(self):
        points = ramp(hours=1) + [(T + i, 20) for i in range(60, 3660, 60)]
        f = estimate(points, 80, T + 86400, T + 3600)
        self.assertEqual(f["state"], "no_burn")
        self.assertIsNone(f["seconds"])

    def test_resets_and_corrections_discard_old_slope(self):
        f = estimate(ramp() + [(T + 30, 0)], 100, T + 7 * 86400, T + 30)
        self.assertEqual(f["state"], "learning")

    def test_offline_jump_is_not_an_instantaneous_rate(self):
        f = estimate(ramp() + [(T + 3600, 70)], 30, T + 86400, T + 3600)
        self.assertEqual(f["state"], "learning")

    def test_stale_and_unknown(self):
        self.assertEqual(estimate(ramp(), 60, T + 86400, T + 200)["state"], "stale")
        self.assertEqual(estimate(ramp(), None, T + 86400, T)["state"], "unavailable")

    def test_fast_recent_regime_adapts_and_widens_range(self):
        points = [(T - 7200 + i, 10 + i / 3600) for i in range(0, 6301, 60)]
        points += [(T - 900 + i, 11.75 + 20 * i / 3600) for i in range(60, 901, 60)]
        f = estimate(points, 80, T + 86400, T)
        self.assertGreater(f["rate"], 10)
        self.assertLess(f["rate"], 20)
        self.assertEqual(f["confidence"], "LOW")
        self.assertGreater(f["highSeconds"], f["lowSeconds"] * 2)

    def test_reset_first_uses_conservative_bound(self):
        f = estimate(ramp(), 60, T + 3600, T)
        self.assertTrue(f["resetFirst"])
        f = estimate(ramp(), 60, T + 6 * 3600, T)
        self.assertFalse(f["resetFirst"])
        self.assertTrue(f["resetPossible"])

    def test_exhausted_reset_pending_and_future_samples(self):
        self.assertEqual(estimate(ramp(), 0, T + 100, T)["state"], "exhausted")
        self.assertEqual(estimate(ramp(), 60, T - 1, T)["state"], "reset_pending")
        f = estimate(ramp() + [(T + 10000, 100)], 60, T + 86400, T)
        self.assertAlmostEqual(f["rate"], 10, places=2)

    def test_history_persists_and_isolates_account_and_cycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "history.db"
            h = History(path)
            w = {
                "bucket": "codex",
                "slot": "primary",
                "minutes": 10080,
                "label": "WEEK",
                "reset": T + 86400,
                "used": 40,
                "remaining": 60,
            }
            h.seed("one", w, ramp())
            a = {"windows": [dict(w)]}
            h.update("one", a, T)
            self.assertEqual(a["forecast"]["state"], "estimate")
            h.db.close()
            h = History(path)
            a = {"windows": [dict(w)]}
            h.update("one", a, T + 30)
            self.assertEqual(a["forecast"]["state"], "estimate")
            a = {"windows": [dict(w)]}
            h.update("two", a, T + 30)
            self.assertEqual(a["forecast"]["state"], "learning")
            w["reset"] += 86400
            a = {"windows": [dict(w)]}
            h.update("one", a, T + 60)
            self.assertEqual(a["forecast"]["state"], "learning")
            h.db.close()

    def test_limiting_window_and_partial_coverage(self):
        with tempfile.TemporaryDirectory() as tmp:
            h = History(Path(tmp) / "history.db")
            w = {
                "bucket": "codex",
                "slot": "primary",
                "minutes": 10080,
                "label": "WEEK",
                "reset": T + 86400,
                "used": 40,
                "remaining": 60,
            }
            other = {
                **w,
                "slot": "secondary",
                "label": "5 HOUR",
                "used": None,
                "remaining": None,
            }
            h.seed("one", w, ramp())
            a = {"windows": [w, other]}
            h.update("one", a, T)
            self.assertTrue(a["forecast"]["partial"])
            self.assertEqual(a["forecast"]["window"], "WEEK")
            h.db.close()


if __name__ == "__main__":
    unittest.main()
