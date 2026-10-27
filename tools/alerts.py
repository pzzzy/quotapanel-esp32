"""Persistent, conservative change detection; never redeems a reset."""

import json
from pathlib import Path
import threading
import time


class AlertTracker:
    def __init__(self, path):
        self.path = Path(path)
        self.lock = threading.Lock()
        try:
            self.state = json.loads(self.path.read_text())
        except (OSError, ValueError):
            self.state = {"previous": {}, "events": [], "sequence": 0}

    def update(self, index, account, identity, now):
        with self.lock:
            key = str(index)
            prior = self.state["previous"].get(key)
            old = (
                prior["account"]
                if prior and prior.get("identity") == identity
                else None
            )
            events = []
            current_windows = {
                (w["bucket"], w.get("slot", w["label"])): w for w in account["windows"]
            }
            previous_windows = (
                {(w["bucket"], w.get("slot", w["label"])): w for w in old["windows"]}
                if old
                else {}
            )
            latch = dict(prior.get("warnings", {})) if old else {}
            warning = None
            bank_drop = bool(
                old
                and old.get("banked") is not None
                and account.get("banked") is not None
                and account["banked"] < old["banked"]
                and not (
                    old.get("bankExpires") is not None and old["bankExpires"] <= now
                )
            )
            exhausted = False
            reset = False
            usage_fell = False
            for key_window, w in current_windows.items():
                before = previous_windows.get(key_window)
                latch_key = ":".join(key_window)
                notified = set(latch.get(latch_key, []))
                restored = (
                    before
                    and before["used"] is not None
                    and w["used"] is not None
                    and w["used"] < before["used"]
                )
                cycle_changed = (
                    before
                    and before.get("reset")
                    and w.get("reset")
                    and before["reset"] != w["reset"]
                )
                if (
                    restored
                    and (
                        cycle_changed
                        or bank_drop
                        or (before.get("reset") and now >= before["reset"])
                    )
                ) or (cycle_changed and now >= before["reset"]):
                    notified.clear()
                r = w["remaining"]
                if r is not None:
                    reached = [
                        threshold
                        for threshold in (10, 5)
                        if r <= threshold and threshold not in notified
                    ]
                    if reached and r > 0:
                        warning = (
                            min([warning] + reached)
                            if warning is not None
                            else min(reached)
                        )
                    notified.update(
                        threshold for threshold in (10, 5) if r <= threshold
                    )
                latch[latch_key] = sorted(notified)
                if w["remaining"] == 0 and (
                    old is None
                    or (
                        before is not None
                        and before["remaining"] is not None
                        and before["remaining"] > 0
                    )
                ):
                    exhausted = True
                if before and before["used"] is not None and w["used"] is not None:
                    fell = before["used"] > w["used"]
                    usage_fell |= fell
                    moved_reset = (
                        before.get("reset")
                        and w.get("reset")
                        and w["reset"] != before["reset"]
                    )
                    elapsed_reset = before.get("reset") and now >= before["reset"]
                    reset |= bool(fell and (moved_reset or elapsed_reset))
            # A decreased bank alone can mean expiration. Require a correlated
            # quota restoration, and suppress this inference near known expiry.
            bank_used = False
            if (
                old
                and old.get("banked") is not None
                and account.get("banked") is not None
            ):
                expired = (
                    old.get("bankExpires") is not None and old["bankExpires"] <= now
                )
                bank_used = (
                    account["banked"] < old["banked"] and usage_fell and not expired
                )
                if account["banked"] > old["banked"]:
                    events.append("banked_gained")
            if bank_used:
                events.append("banked_used")
            elif reset:
                events.append("reset")
            if exhausted:
                events.append("exhausted")
            elif warning is not None:
                events.append("warning_" + str(warning))
            for event in events:
                self.state["sequence"] = max(
                    self.state["sequence"] + 1, int(now * 1000)
                )
                self.state["events"].append(
                    {
                        "id": self.state["sequence"],
                        "account": index,
                        "type": event,
                        "at": int(now),
                    }
                )
            self.state["events"] = [
                e for e in self.state["events"] if now - e["at"] < 600
            ][-16:]
            self.state["previous"][key] = {
                "identity": identity,
                "account": account,
                "warnings": latch,
            }
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps(self.state, separators=(",", ":")))
            tmp.chmod(0o600)
            tmp.replace(self.path)
            return events

    def snapshot(self):
        with self.lock:
            return list(self.state["events"])
