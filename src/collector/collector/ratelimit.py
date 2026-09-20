"""In-process per-key token bucket for the app-code rate limits (plan D14:
"per-IP and per-token limits in app code").

Deliberately simple and in-memory: `RATE_PER_MIN` (default 10) is a soft
abuse guard, not a security boundary, and this service runs a small, bounded
number of uvicorn workers. Known limitation, documented rather than hidden:
each worker process has its own bucket, so the *effective* per-key rate with
N workers is up to N x RATE_PER_MIN. Acceptable at this service's scale; a
shared store (Redis, or a single worker) would be the fix if it ever isn't.
"""

from __future__ import annotations

import time
from threading import Lock


class TokenBucket:
    def __init__(self, rate_per_min: int, burst: int | None = None) -> None:
        self.rate_per_min = max(rate_per_min, 1)
        self.capacity = float(burst or self.rate_per_min)
        self._buckets: dict[str, tuple[float, float]] = {}
        self._lock = Lock()

    def allow(self, key: str) -> bool:
        now = time.monotonic()
        refill_rate = self.rate_per_min / 60.0
        with self._lock:
            tokens, last = self._buckets.get(key, (self.capacity, now))
            tokens = min(self.capacity, tokens + (now - last) * refill_rate)
            if tokens < 1.0:
                self._buckets[key] = (tokens, now)
                return False
            tokens -= 1.0
            self._buckets[key] = (tokens, now)
            return True
