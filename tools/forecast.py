"""Quota runway, based on measured percentage points, never token counts.

Point forecasts are conditional on continued recent pace. Ranges are sensitivity
bounds (quantization + disagreement), NOT calibrated probability intervals.
"""

import math
from pathlib import Path
import sqlite3
import threading

HORIZONS = ((900, 0.55), (3600, 0.30), (21600, 0.10), (86400, 0.05))
MAX_GAP = 600


def estimate(samples, remaining, reset, now):
    result = {
        "state": "learning",
        "seconds": None,
        "lowSeconds": None,
        "highSeconds": None,
        "rate": None,
        "confidence": "LOW",
        "asOf": int(now),
        "coverageSeconds": 0,
        "pointsConsumed": 0,
        "resetFirst": False,
        "resetPossible": False,
    }
    if remaining is None or not math.isfinite(remaining):
        result["state"] = "unavailable"
        return result
    if remaining <= 0:
        result.update(state="exhausted", seconds=0)
        return result
    if reset is not None and reset <= now:
        result["state"] = "reset_pending"
        return result
    # Last observation per timestamp, ordered; clip to the last 24h.
    ordered = sorted(
        {
            float(t): float(u)
            for t, u in samples
            if now - 86400 <= t <= now
            and math.isfinite(t)
            and math.isfinite(u)
            and 0 <= u <= 100
        }.items()
    )
    if not ordered or now - ordered[-1][0] > 90:
        result["state"] = "stale"
        return result
    # Never attribute a reset, correction, or unobserved gap to current burn.
    segment = []
    for sample in ordered:
        if segment and (
            sample[0] - segment[-1][0] > MAX_GAP or sample[1] < segment[-1][1]
        ):
            segment = []
        segment.append(sample)
    if len(segment) < 2:
        return result
    end, used = segment[-1]
    span = end - segment[0][0]
    consumed = used - segment[0][1]
    result.update(coverageSeconds=int(span), pointsConsumed=round(consumed, 2))
    if span < 600:
        return result

    def baseline(cutoff):
        if cutoff <= segment[0][0]:
            return segment[0]
        for left, right in zip(segment, segment[1:]):
            if left[0] <= cutoff <= right[0]:
                fraction = (cutoff - left[0]) / (right[0] - left[0])
                return cutoff, left[1] + fraction * (right[1] - left[1])
        return segment[-1]

    recent_start, recent_used = baseline(end - 3600)
    recent_span = end - recent_start
    if recent_span >= 3600 and used - recent_used < 1:
        result["state"] = "no_burn"
        return result
    if consumed < 2:
        return result  # At least two quota quanta before projecting.
    candidates = []
    seen = set()
    for horizon, weight in HORIZONS:
        start, before = baseline(end - horizon)
        elapsed = end - start
        if elapsed < 600 or round(start, 3) in seen:
            continue
        seen.add(round(start, 3))
        delta = max(0, used - before)
        if delta < 2:
            continue
        # Endpoint error up to one percentage point for whole-% reporting.
        rate = delta * 3600 / elapsed
        error = 3600 / elapsed
        quality = min(1, elapsed / horizon) * min(1, max(1, delta) / 3)
        candidates.append((rate, max(0, rate - error), rate + error, weight * quality))
    if not candidates:
        return result
    weight_sum = sum(c[3] for c in candidates)
    rate = sum(c[0] * c[3] for c in candidates) / weight_sum
    if rate <= 0:
        result["state"] = "no_burn"
        return result
    # Include the disagreement across horizons instead of claiming a narrow CI.
    slow = min(c[1] for c in candidates)
    fast = max(c[2] for c in candidates)
    seconds = remaining / rate * 3600
    low = max(0, remaining - 1) / fast * 3600
    high = (remaining + 1) / slow * 3600 if slow > 0 else None
    # Precision falls as the forecast extends beyond observed history.
    spread = fast / max(slow, 0.001)
    confidence = "LOW"
    if span >= 3600 and consumed >= 4 and spread <= 3 and seconds <= span * 8:
        confidence = "MED"
    if span >= 7200 and consumed >= 8 and spread < 1.6 and seconds <= span * 4:
        confidence = "HIGH"
    until_reset = reset - now if reset else None
    result.update(
        state="estimate",
        seconds=round(seconds),
        lowSeconds=round(low),
        highSeconds=round(high) if high is not None else None,
        rate=round(rate, 3),
        confidence=confidence,
        resetFirst=until_reset is not None and low >= until_reset,
        resetPossible=until_reset is not None and seconds >= until_reset,
    )
    return result


def window_key(w):
    return f"{w['bucket']}:{w.get('slot', 'primary')}:{w.get('minutes')}"


class History:
    def __init__(self, path):
        self.path = Path(path)
        self.lock = threading.Lock()
        self.db = sqlite3.connect(str(path), check_same_thread=False)
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS samples (identity TEXT, window TEXT, reset INTEGER, at REAL, used REAL, source TEXT, PRIMARY KEY(identity,window,reset,at))"
        )
        self.db.execute("CREATE INDEX IF NOT EXISTS sample_time ON samples(at)")
        self.db.commit()
        self.path.chmod(0o600)

    def seed(self, identity, w, samples):
        with self.lock:
            self.db.executemany(
                "INSERT OR IGNORE INTO samples VALUES (?,?,?,?,?,?)",
                (
                    (
                        identity,
                        window_key(w),
                        w.get("reset") or 0,
                        t,
                        u,
                        "local-quota-log",
                    )
                    for t, u in samples
                ),
            )
            self.db.commit()

    def update(self, identity, account, now):
        with self.lock:
            for w in account["windows"]:
                used = w.get("used")
                if used is not None and 0 <= used <= 100:
                    self.db.execute(
                        "INSERT OR REPLACE INTO samples VALUES (?,?,?,?,?,?)",
                        (
                            identity,
                            window_key(w),
                            w.get("reset") or 0,
                            now,
                            used,
                            "live",
                        ),
                    )
                samples = self.db.execute(
                    "SELECT at,used FROM samples WHERE identity=? AND window=? AND reset=? AND at>=? ORDER BY at",
                    (identity, window_key(w), w.get("reset") or 0, now - 86400),
                ).fetchall()
                w["forecast"] = estimate(
                    samples, w.get("remaining"), w.get("reset"), now
                )
            self.db.execute("DELETE FROM samples WHERE at<?", (now - 28 * 86400,))
            self.db.commit()
        normal = [w for w in account["windows"] if w["bucket"] == "codex"] or account[
            "windows"
        ]
        forecasts = [(w, w["forecast"]) for w in normal]
        finite = [pair for pair in forecasts if pair[1]["seconds"] is not None]
        choice = (
            min(finite, key=lambda p: p[1]["seconds"])
            if finite
            else (forecasts[0] if forecasts else None)
        )
        account["forecast"] = (
            dict(choice[1]) if choice else {"state": "unavailable", "seconds": None}
        )
        account["forecast"]["partial"] = any(
            f["state"] not in ("estimate", "exhausted") for _, f in forecasts
        )
        if choice:
            account["forecast"]["window"] = choice[0]["label"]
        return account
