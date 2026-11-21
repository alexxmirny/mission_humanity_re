"""WEB1 focused, network-free generator checks. Run after integration, not on release binaries."""

from datetime import datetime, timezone
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch
import json

from src.website import build

URL = "https://github.com/example/project"


def release(tag, date, ident, prerelease=False, body="Notes"):
    row = {
        "id": ident,
        "tag_name": tag,
        "published_at": date,
        "draft": False,
        "prerelease": prerelease,
        "body": body,
        "assets": [],
    }
    if tag.startswith("launcher-v"):
        row["assets"].append({
            "name": "mh_launcher.exe", "state": "uploaded", "size": 123,
            "browser_download_url": URL + "/releases/download/" + tag + "/mh_launcher.exe",
        })

    return row


class WebsiteChecks(unittest.TestCase):
    def test_sort_uses_publication_date_not_version_or_input_order(self):
        older_high_version = release("v9.0.0", "2026-11-19T12:00:00Z", 8)
        newer_low_version = release("v1.0.0", "2026-11-21T12:00:00Z", 3)
        draft = release("launcher-v10.0.0", "2026-11-22T12:00:00Z", 99)
        draft["draft"] = True
        fixture = Mock()
        fixture.read_text.return_value = json.dumps([older_high_version, draft, newer_low_version])
        rows = build.load_releases("example/project", fixture)
        self.assertEqual([row["id"] for row in rows], [3, 8])

    def test_api_failure_propagates_instead_of_publishing_empty_data(self):
        with patch.object(build.subprocess, "run", side_effect=OSError("offline")):
            with self.assertRaises(OSError):
                build.load_releases("example/project", None)
    def test_game_only_release_does_not_select_launcher(self):
        rows = [release("v9.0.0", "2026-11-21T12:00:00Z", 3),
                release("launcher-v1.0.0", "2026-11-20T12:00:00Z", 2)]
        html = build.latest_launcher(rows, URL)
        self.assertIn("Launcher 1.0.0", html)
        self.assertIn("launcher-v1.0.0/mh_launcher.exe", html)
        self.assertNotIn("Launcher 9.0.0", html)

    def test_prerelease_is_not_mislabeled_stable(self):
        html = build.latest_launcher([release("launcher-v1.0.1-rc1", "2026-11-21T12:00:00Z", 1, True)], URL)
        self.assertIn("Prerelease", html)
        self.assertNotIn("Final release", html)

    def test_missing_launcher_has_no_invented_version_or_exe(self):
        html = build.latest_launcher([], URL)
        self.assertIn("No launcher published", html)
        self.assertNotIn("mh_launcher.exe", html)

    def test_latest_missing_asset_does_not_fall_back(self):
        newest = release("launcher-v2.0.0", "2026-11-21T12:00:00Z", 2)
        newest["assets"] = []
        older = release("launcher-v1.0.0", "2026-11-20T12:00:00Z", 1)
        html = build.latest_launcher([newest, older], URL)
        self.assertIn("no ready download", html)
        self.assertNotIn("Download mh_launcher.exe", html)
        self.assertNotIn("launcher-v1.0.0", html)

    def test_duplicate_asset_is_fatal(self):
        row = release("launcher-v1.0.0", "2026-11-20T12:00:00Z", 1)
        row["assets"] *= 2
        with self.assertRaises(ValueError):
            build.latest_launcher([row], URL)

    def test_foreign_asset_origin_is_fatal(self):
        row = release("launcher-v1.0.0", "2026-11-20T12:00:00Z", 1)
        row["assets"][0]["browser_download_url"] = "https://foreign.invalid/mh_launcher.exe"
        with self.assertRaises(ValueError):
            build.latest_launcher([row], URL)

    def test_notes_never_become_html_or_script(self):
        payload = '</pre><script>alert(1)</script><img src=x onerror=alert(2)>'
        html = build.release_notes([release("v1.0.0", "2026-11-20T12:00:00Z", 1, body=payload)], URL)
        self.assertIn("&lt;script&gt;", html)
        self.assertNotIn("<script>", html)
        self.assertNotIn("<img", html)
        self.assertIn("&lt;/pre&gt;", html)

    def test_empty_notes_are_explicit(self):
        html = build.release_notes([release("v1.0.0", "2026-11-20T12:00:00Z", 1, body=None)], URL)
        self.assertIn("No release notes were supplied", html)

    def test_utc_timestamp_is_explicit_and_valid(self):
        self.assertEqual(build.utc("2026-11-21T12:00:00Z"), datetime(2026, 11, 21, 12, tzinfo=timezone.utc))
        for bad in (None, "2026-11-21", "2026-11-21T12:00:00+03:00", "2026-02-31T12:00:00Z"):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                build.utc(bad)

    def test_empty_changelog_is_explicit(self):
        self.assertIn("No published game or launcher releases", build.release_notes([], URL))

    def test_old_signed_schema2_manifest_still_offers_download(self):
        manifest = {
            "schema": 2, "kind": "launcher", "channel": "stable",
            "version": "1.0.0", "issued_at": "2020-01-01T00:00:00Z",
            "url": URL + "/releases/download/launcher-v1.0.0/mh_launcher.exe",
            "sha256": "a" * 64, "size": 123,
        }
        key_id, seed = b"12345678", bytes(range(32))
        public = build.manifests.public_key_line(key_id, build.manifests.ed25519_public_key(seed))
        data = build.manifests.manifest_bytes(manifest)
        signature = build.manifests.sign_bytes(key_id, seed, data, "fixture", "fixture")
        with TemporaryDirectory() as directory:
            pages = Path(directory)
            path = pages / "channels" / "stable" / "launcher.json"
            path.parent.mkdir(parents=True)
            path.write_bytes(data)
            Path(str(path) + ".minisig").write_text(signature, encoding="utf-8")
            with patch.object(build.manifests, "launcher_public_key", return_value=public):
                html = build.channel_card(pages, "stable", "launcher", URL,
                                          datetime(2026, 11, 21, tzinfo=timezone.utc))
            self.assertIn('href="' + manifest["url"] + '"', html)
            self.assertIn("Manifest issued 2020-01-01", html)


if __name__ == "__main__":
    unittest.main()
