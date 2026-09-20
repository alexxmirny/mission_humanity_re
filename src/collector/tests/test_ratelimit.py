from __future__ import annotations

from collector.ratelimit import TokenBucket


def test_allows_up_to_the_configured_rate_then_blocks():
    bucket = TokenBucket(rate_per_min=3)
    assert bucket.allow("k") is True
    assert bucket.allow("k") is True
    assert bucket.allow("k") is True
    assert bucket.allow("k") is False


def test_keys_are_independent():
    bucket = TokenBucket(rate_per_min=1)
    assert bucket.allow("a") is True
    assert bucket.allow("b") is True
    assert bucket.allow("a") is False
    assert bucket.allow("b") is False
