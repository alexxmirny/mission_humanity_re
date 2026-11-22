#!/usr/bin/env python3
"""Generate the website-owned Pages documents and credited image assets.

Source: src/website/ templates/styles/images; output: an explicit directory.
The existing Pages checkout is input only, including signed channel manifests.
API failure or a bad existing signature is fatal, preserving the deployed snapshot.
An absent channel or launcher asset instead renders an explicit unavailable state.
"""

import argparse
from datetime import datetime, timezone
from html import escape
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import quote

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import gen_update_manifest as manifests

SOURCE = Path(__file__).resolve().parent
ASSETS = ("battle.webp", "orders.webp", "lobby.webp", "map.webp", "wordmark.webp")
TAG = re.compile(r"^(launcher-)?v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")
TOKEN = re.compile(r"@@([A-Z_]+)@@")


def utc(value):
    if not isinstance(value, str) or not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value
    ):
        raise ValueError("expected an explicit UTC timestamp")
    return datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def esc(value):
    return escape(str(value), quote=True)


def link(url, label, css=""):
    return '<a%s href="%s">%s</a>' % (
        (' class="%s"' % esc(css)) if css else "", esc(url), esc(label)
    )


def release_url(repo_url, tag):
    return repo_url + "/releases/tag/" + quote(tag, safe="")


def badge(release):
    if release["prerelease"]:
        return '<span class="badge prerelease">Prerelease</span>'
    return ""


def load_releases(repo, fixture):
    if fixture:
        rows = json.loads(fixture.read_text(encoding="utf-8"))
    else:
        result = subprocess.run(
            ["gh", "api", "--hostname", "github.com", "--paginate", "--slurp",
             "-H", "Accept: application/vnd.github+json",
             "-H", "X-GitHub-Api-Version: 2022-11-28",
             "repos/" + repo + "/releases?per_page=100"],
            check=True, capture_output=True, text=True, encoding="utf-8", timeout=180,
        )
        pages = json.loads(result.stdout)
        if not isinstance(pages, list) or any(not isinstance(p, list) for p in pages):
            raise ValueError("release API did not return complete array pages")
        rows = [row for page in pages for row in page]
    if not isinstance(rows, list):
        raise ValueError("releases must be an array")
    kept = []
    ids = set()
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("release is not an object")
        if row.get("draft") is not False:
            continue
        tag = row.get("tag_name")
        if not isinstance(tag, str) or not TAG.fullmatch(tag):
            continue
        utc(row.get("published_at"))
        if type(row.get("id")) is not int or row["id"] in ids:
            raise ValueError("missing or duplicate release id")
        if type(row.get("prerelease")) is not bool:
            raise ValueError("missing release prerelease classification")
        if row.get("body") is not None and not isinstance(row["body"], str):
            raise ValueError("release body is not text")
        if not isinstance(row.get("assets"), list):
            raise ValueError("release assets are not an array")
        ids.add(row["id"])
        kept.append(row)
    return sorted(kept, key=lambda r: (utc(r["published_at"]), r["id"]), reverse=True)


def latest_launcher(rows, repo_url):
    row = next((r for r in rows if r["tag_name"].startswith("launcher-v")), None)
    if row is None:
        return '<h3>No launcher published</h3><p>No published launcher release was found. ' + link(
            repo_url + "/releases", "Browse GitHub releases"
        ) + '</p>'
    tag = row["tag_name"]
    version = tag.removeprefix("launcher-v")
    url = repo_url + "/releases/download/" + quote(tag, safe="") + "/mh_launcher.exe"
    assets = [a for a in row["assets"] if isinstance(a, dict) and a.get("name") == "mh_launcher.exe"]
    if len(assets) > 1:
        raise ValueError("launcher release has duplicate named assets")
    asset = assets[0] if assets else None
    header = '<p class="download-version">Launcher %s %s</p>' % (esc(version), badge(row))
    notes = link(release_url(repo_url, tag), "Launcher release notes")
    if asset is None or asset.get("state") != "uploaded" or type(asset.get("size")) is not int or asset["size"] <= 0:
        return header + '<p>The latest launcher release has no ready download. An older file is not being presented as latest.</p>' + notes
    if asset.get("browser_download_url") != url:
        raise ValueError("launcher asset does not use this repository's release URL")
    result = header + '<div class="actions">' + link(url, "Download Launcher", "button") + '</div>'
    result += '<p class="small">' + notes + '</p>'
    return result


def channel_card(pages, channel, kind, repo_url, now):
    title = channel.capitalize() + " · " + kind.capitalize()
    path = pages / "channels" / channel / (kind + ".json")
    start = '<article class="card channel"><h3>%s</h3>' % esc(title)
    if not path.exists():
        if Path(str(path) + ".minisig").exists():
            raise ValueError("channel signature exists without manifest")
        return start + '<p>Not published for this channel.</p></article>'
    _, manifest = manifests.load_verified(str(path), manifests.launcher_public_key())
    manifests.validate_v2(manifest, kind, channel)
    issued = utc(manifest["issued_at"])
    if (issued - now).total_seconds() > 86400:
        raise ValueError("channel manifest is over a day in the future")
    version = manifest["version"]
    tag = ("launcher-v" if kind == "launcher" else "v") + version
    if not TAG.fullmatch(tag):
        raise ValueError("channel version is not a supported release tag")
    record = manifest if kind == "launcher" else manifest["game"]["net"]
    filename = "mh_launcher.exe" if kind == "launcher" else "mission_humanity_re-" + version + "-net.zip"
    expected = repo_url + "/releases/download/" + quote(tag, safe="") + "/" + quote(filename, safe="")
    if record["url"] != expected:
        raise ValueError("channel download is not the expected repository asset")
    result = start + '<p class="version">%s</p>' % esc(version)
    result += '<p class="small">Manifest issued %s UTC</p>' % esc(manifest["issued_at"].replace("T", " ").removesuffix("Z"))
    result += '<p>' + link(record["url"], "Download " + filename) + '</p>'
    result += '<p>' + link(release_url(repo_url, tag), "Release notes on GitHub") + '</p>'
    if kind == "game":
        result += '<p class="small">Requires launcher %s or newer.</p>' % esc(manifest["min_launcher"])
    result += '<p class="hash">SHA-256: %s</p></article>' % esc(record["sha256"])
    return result


def release_notes(rows, repo_url):
    if not rows:
        return '<p>No published game or launcher releases were found. ' + link(repo_url + "/releases", "Check GitHub releases") + '.</p>'
    result = []
    for i, row in enumerate(rows):
        tag = row["tag_name"]
        kind = "Launcher" if tag.startswith("launcher-v") else "Game"
        version = tag.removeprefix("launcher-v") if kind == "Launcher" else tag.removeprefix("v")
        timestamp = row["published_at"]
        body = row.get("body") or "No release notes were supplied. Open the original release for its assets."
        result.append(
            '<details class="release" id="release-%s"%s><summary><span class="release-title">%s %s</span> %s'
            '<time class="release-date" datetime="%s">%s UTC</time></summary>'
            '<div class="release-body"><p>%s</p><pre>%s</pre></div></details>' % (
                esc(row["id"]), " open" if i == 0 else "", esc(kind), esc(version), badge(row),
                esc(timestamp), esc(timestamp.replace("T", " ").removesuffix("Z")),
                link(release_url(repo_url, tag), "Original release and assets"), esc(body),
            )
        )
    return "\n".join(result)


def render(source, values):
    text = source.read_text(encoding="utf-8")
    keys = set(TOKEN.findall(text))
    if not keys <= values.keys():
        raise ValueError("unhandled template token")
    return TOKEN.sub(lambda m: values[m.group(1)], text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, help="Public owner/repository, not a private source repository")
    parser.add_argument("--pages", type=Path, required=True, help="Read-only existing gh-pages checkout")
    parser.add_argument("--out", type=Path, required=True, help="Owned output directory")
    parser.add_argument("--releases-json", type=Path, help="Offline flattened releases fixture; no network when supplied")
    parser.add_argument("--snapshot-at", help="Explicit UTC timestamp for reproducible fixture generation")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repo):
        parser.error("--repo must be owner/repository")
    if args.out.resolve() == args.pages.resolve():
        parser.error("--out must not overwrite the input Pages checkout")
    now = utc(args.snapshot_at) if args.snapshot_at else datetime.now(timezone.utc).replace(microsecond=0)
    repo_url = "https://github.com/" + args.repo
    rows = load_releases(args.repo, args.releases_json)
    values = {
        "REPO": esc(repo_url),
        "LATEST_LAUNCHER": latest_launcher(rows, repo_url),
        "CHANNELS": "\n".join(
            channel_card(args.pages, channel, kind, repo_url, now)
            for channel in ("stable", "latest") for kind in ("launcher", "game")
        ),
        "RELEASES": release_notes(rows, repo_url),
        "SNAPSHOT": "Updated " + esc(now.strftime("%d %B %Y")),
    }
    # Finish all parsing/validation/rendering before creating any output file.
    files = {
        "index.html": render(SOURCE / "index.html", values).encode("utf-8"),
        "downloads.html": render(SOURCE / "downloads.html", values).encode("utf-8"),
        "site.css": (SOURCE / "site.css").read_text(encoding="utf-8").encode("utf-8"),
    }
    for asset in ASSETS:
        files["website-assets/" + asset] = (SOURCE / "website-assets" / asset).read_bytes()
    args.out.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        target = args.out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    print("Generated website documents and %d images; %d published releases" % (len(ASSETS), len(rows)))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError, manifests.Refusal) as error:
        print("Website generation refused: " + str(error), file=sys.stderr)
        sys.exit(1)
