import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from alerts import AlertTracker


def account(used=50, bank=1, reset=2000, expires=3000):
    return {
        "windows": [
            {
                "bucket": "codex",
                "slot": "primary",
                "label": "WEEK",
                "remaining": 100 - used,
                "used": used,
                "reset": reset,
            }
        ],
        "banked": bank,
        "bankExpires": expires,
    }


class TestAlerts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "state.json"
        self.t = AlertTracker(self.path)

    def tearDown(self):
        self.tmp.cleanup()

    def update(self, a, now=1000, identity="account-one"):
        return self.t.update(0, a, identity, now)

    def test_baseline_and_exhausted_once_across_restart(self):
        self.assertEqual(self.update(account()), [])
        self.assertEqual(self.update(account(100)), ["exhausted"])
        self.t = AlertTracker(self.path)
        self.assertEqual(self.update(account(100)), [])

    def test_scheduled_reset(self):
        self.update(account(100))
        self.assertEqual(self.update(account(0, reset=9000), 2001), ["reset"])

    def test_banked_use_suppresses_duplicate_reset(self):
        self.update(account(90))
        self.assertEqual(self.update(account(0, bank=0, reset=9000)), ["banked_used"])

    def test_expiry_is_not_spending(self):
        self.update(account(90, expires=1100))
        self.assertEqual(self.update(account(90, bank=0), 1200), [])

    def test_expiry_with_scheduled_reset_is_generic(self):
        self.update(account(90, expires=1900))
        self.assertEqual(self.update(account(0, bank=0, reset=9000), 2100), ["reset"])

    def test_gain_unknown_and_identity_change(self):
        self.update(account(bank=0))
        self.assertEqual(self.update(account(bank=1)), ["banked_gained"])
        self.assertEqual(
            self.update(account(0, bank=0), identity="different-account"), []
        )

    def test_small_correction_is_not_reset(self):
        self.update(account(50))
        self.assertEqual(self.update(account(49)), [])

    def test_first_observation_exhausted(self):
        self.assertEqual(self.update(account(100)), ["exhausted"])

    def test_unknown_is_not_a_fresh_exhaustion_transition(self):
        self.update(account(100))
        unknown = account()
        unknown["windows"][0]["used"] = None
        unknown["windows"][0]["remaining"] = None
        self.update(unknown)
        self.assertEqual(self.update(account(100)), [])

    def test_thresholds_once_with_jitter_and_restart(self):
        self.update(account(89))
        self.assertEqual(self.update(account(90)), ["warning_10"])
        self.update(account(89))
        self.assertEqual(self.update(account(90)), [])
        self.t = AlertTracker(self.path)
        self.assertEqual(self.update(account(95)), ["warning_5"])
        self.assertEqual(self.update(account(96)), [])

    def test_skip_to_five_only_plays_most_urgent_warning(self):
        self.update(account(88))
        self.assertEqual(self.update(account(97)), ["warning_5"])
        self.assertEqual(self.update(account(100)), ["exhausted"])

    def test_reset_rearms_both_warnings(self):
        self.update(account(95))
        self.update(account(0, reset=9000))
        self.assertEqual(self.update(account(90, reset=9000)), ["warning_10"])
        self.assertEqual(self.update(account(95, reset=9000)), ["warning_5"])

    def test_upgrade_under_threshold_warns_once(self):
        self.assertEqual(self.update(account(93)), ["warning_10"])
        self.assertEqual(self.update(account(93)), [])

    def test_multiple_account_and_unique_event_ids(self):
        self.t.update(0, account(100), "one", 1000)
        self.t.update(1, account(100), "two", 1000)
        ids = [e["id"] for e in self.t.snapshot()]
        self.assertEqual(len(set(ids)), 2)


if __name__ == "__main__":
    unittest.main()
