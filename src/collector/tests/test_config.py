"""Config loading: TOKEN_FILE vs. the REPORT_TOKEN test/local override."""

from __future__ import annotations

import pytest
from collector.config import ConfigError, get_settings


def test_missing_token_file_and_no_inline_token_raises(tmp_path, monkeypatch):
    monkeypatch.delenv("REPORT_TOKEN", raising=False)
    monkeypatch.setenv("TOKEN_FILE", str(tmp_path / "does-not-exist"))
    monkeypatch.setenv("REPORTS_DIR", str(tmp_path / "reports"))

    with pytest.raises(ConfigError):
        get_settings()


def test_inline_report_token_overrides_token_file(tmp_path, monkeypatch):
    monkeypatch.setenv("REPORT_TOKEN", "  inline-secret  ")
    monkeypatch.setenv("TOKEN_FILE", str(tmp_path / "does-not-exist"))
    monkeypatch.setenv("REPORTS_DIR", str(tmp_path / "reports"))

    settings = get_settings()
    assert settings.token == "inline-secret"  # stripped


def test_token_file_is_read_and_stripped(tmp_path, monkeypatch):
    token_file = tmp_path / "report_token"
    token_file.write_text("file-secret\n", encoding="utf-8")
    monkeypatch.delenv("REPORT_TOKEN", raising=False)
    monkeypatch.setenv("TOKEN_FILE", str(token_file))
    monkeypatch.setenv("REPORTS_DIR", str(tmp_path / "reports"))

    settings = get_settings()
    assert settings.token == "file-secret"


def test_defaults(tmp_path, monkeypatch):
    monkeypatch.setenv("REPORT_TOKEN", "t")
    monkeypatch.setenv("REPORTS_DIR", str(tmp_path / "reports"))
    monkeypatch.delenv("MAX_BODY_MB", raising=False)
    monkeypatch.delenv("RATE_PER_MIN", raising=False)
    monkeypatch.delenv("TIMESTAMP_SKEW_SECONDS", raising=False)

    settings = get_settings()
    assert settings.max_body_mb == 64
    assert settings.rate_per_min == 10
    assert settings.timestamp_skew_seconds == 600
    assert settings.max_body_bytes == 64 * 1024 * 1024
