"""Regenerate the schema-2 update fixtures in this directory (dist RL8 unit A1).

    python src/launcher/tests/data/gen_schema2_fixtures.py

Every `s2_*.json` here is a schema-2 channel manifest (`launcher.json` / `game.json`) signed with the
TEST-ONLY key `schema2_test.key` by `tools/gen_update_manifest.py --sign-file`. That key is committed
on purpose and is NOT the release key: `update::PUBLIC_KEY` never changes for it, a real launcher
never trusts it, and the tests reach it only through the explicit key parameter of
`update::accept_v2`. Anybody holding this file can mint fixtures; nobody can mint a manifest a
shipped launcher will accept.

The zips the game manifests point at are the existing `mission_humanity_re-0.3.0-*.zip` fixtures
(their real sizes and digests are read here, so the manifests cannot drift from them). `.gitattributes`
in this directory switches line-ending normalisation off: these files travel byte for byte.
"""

import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
TOOL = os.path.join(REPO, "tools", "gen_update_manifest.py")
KEY = os.path.join(HERE, "schema2_test.key")
BASE = "http://127.0.0.1:8099/"
RELAY = {
    "addr": "192.0.2.10:7100",
    "key": "4d487465737474656b65794d487465737474656b65794d487465737474656b65",
}
LAUNCHER_SHA = hashlib.sha256(b"fixture launcher executable").hexdigest()


def zip_entry(tag, url_version=None):
    name = f"mission_humanity_re-0.3.0-{tag}.zip"
    with open(os.path.join(HERE, name), "rb") as fh:
        data = fh.read()
    shown = f"mission_humanity_re-{url_version or '0.3.0'}-{tag}.zip"
    return {"url": BASE + shown, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}


def launcher(channel, version, issued_at):
    return {
        "schema": 2,
        "kind": "launcher",
        "channel": channel,
        "version": version,
        "issued_at": issued_at,
        "url": f"{BASE}mh_launcher-{version}.exe",
        "sha256": LAUNCHER_SHA,
        "size": 1234,
    }


def game(channel, version, issued_at, min_launcher, tags, url_version=None, relay=True, notes=""):
    doc = {
        "schema": 2,
        "kind": "game",
        "channel": channel,
        "version": version,
        "issued_at": issued_at,
        "min_launcher": min_launcher,
        "game": {t: zip_entry(t, url_version) for t in tags},
    }
    if relay:
        doc["relay"] = RELAY
    if notes:
        doc["notes_url"] = notes
    return doc


ALL_TAGS = ["net", "net-debug", "brokered-debug"]
FIXTURES = {
    # the newest latest-channel pair
    "s2_launcher_latest.json": launcher("latest", "0.2.0", "2026-10-01T00:00:00Z"),
    "s2_game_latest.json": game(
        "latest",
        "0.3.0",
        "2026-10-02T00:00:00Z",
        "0.2.0",
        ALL_TAGS,
        notes="https://example.invalid/notes/0.3.0",
    ),
    # an OLDER genuine latest game manifest: the replay fixture
    "s2_game_latest_old.json": game(
        "latest", "0.2.5", "2026-09-20T00:00:00Z", "0.1.0", ["net"], url_version="0.2.5"
    ),
    # the stable channel: older game, older launcher, `net` only (decision D1)
    "s2_launcher_stable.json": launcher("stable", "0.1.5", "2026-09-25T00:00:00Z"),
    "s2_game_stable.json": game(
        "stable", "0.2.9", "2026-09-25T00:00:00Z", "0.1.0", ["net"], url_version="0.2.9"
    ),
}
# a genuine signature over a manifest that points at a plain-http host that is not the origin
HTTP = game("latest", "0.3.1", "2026-10-03T00:00:00Z", "0.2.0", ["net"], relay=False)
HTTP["game"]["net"]["url"] = "http://evil.example/mission_humanity_re-0.3.1-net.zip"
FIXTURES["s2_game_latest_http.json"] = HTTP


def main():
    for name, doc in FIXTURES.items():
        path = os.path.join(HERE, name)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(json.dumps(doc, indent=2) + "\n")
        comment = f"mission humanity {doc['kind']} {doc['channel']} {doc['version']} test fixture"
        subprocess.run(
            [sys.executable, TOOL, "--sign-file", path, "--secret-key", KEY]
            + ["--trusted-comment", comment],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        print("wrote", name, "+ .minisig")


if __name__ == "__main__":
    main()
