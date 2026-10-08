# Cutting a release

How a version of this project reaches somebody who is not going to build it. Three drop-in zips and
a `SHA256SUMS`, attached to a GitHub Release, built and gated by
[`.github/workflows/release.yml`](../.github/workflows/release.yml) from a pushed tag.

This is the maintainer's page. A *user* wants [INSTALL.md](../INSTALL.md) — "Download a release" for
the zips, section 2 onward for building from source.

- [0. The routine — from a green tree to a public release](#0-the-routine--from-a-green-tree-to-a-public-release)
- [1. The version convention](#1-the-version-convention)
- [2. Cutting one](#2-cutting-one)
- [3. How the stamp reaches the binaries](#3-how-the-stamp-reaches-the-binaries)
- [4. Packaging by hand](#4-packaging-by-hand)
- [5. What a green release run does not prove](#5-what-a-green-release-run-does-not-prove)
- [6. The launcher release](#6-the-launcher-release)
- [7. Cutting a release BY HAND, when Actions cannot run](#7-cutting-a-release-by-hand-when-actions-cannot-run)

## 0. The routine — from a green tree to a public release

One ordered checklist, complete enough to cut `v0.1.1` alone. Sections 1–6 below are the reference
detail each step points into; nothing here restates them. Two repositories are involved: the
**private** `mh_re_private` (`origin` on the maintainer's box — every commit and every rehearsal
goes there) and the **public** `mission_humanity_re` (reached only through
`tools/build_public_seed.py`; its `main` is squash commits, its Releases page and its Pages site
are what players see). `<owner>` below is the GitHub account holding both.

### 0.1 One-time prerequisites (check once; nothing later can create them)

Everything in this step is a repository *setting* — no workflow file can make it for itself.
`gh variable list -R <owner>/<repo>` and `gh secret list -R <owner>/<repo>` show what is there
(names only for secrets). Both repositories need:

| kind | name | value |
| --- | --- | --- |
| variable | `MH_UPDATE_BASE_URL` | the **public** Pages base, `https://<owner>.github.io/mission_humanity_re` — on the private repo too (a private repository's Pages needs a paid plan, so its launcher builds point at the public site) |
| variable | `MH_REPORT_URL`, `MH_REPORT_CA_PEM`, `MH_REPORT_SPKI_PIN` | the collector's base URL, its local root CA (PEM) and the SPKI pin — `launcher-release.yml`'s header says what each is |
| variable | `MH_RELAY_ADDR` | the relay as `host:port` (`<relay-host>:7100`); never written into the tree — `lint_machine_paths` refuses it |
| secret | `MH_RELAY_KEY` | the relay's deployment key, 64 hex: the first line of the VPS's `deploy/secrets/relay_key.txt` = `RELAY_KEY` in `tools/machine.local.json` ([docs/deploy.md](deploy.md) "The relay key") |
| secret | `MH_LAUNCHER_MINISIGN_KEY` | the minisign secret key whose public half is `PUBLIC_KEY` in `src/launcher/src/update.rs` ([The manifest](#the-manifest)) |
| secret | `MH_REPORT_TOKEN` | the collector's write-only report token |

Private repository only: the **`vps` Environment** (required reviewer, the `VPS_HOST` /
`VPS_USER` / `VPS_SSH_KEY` secrets, the `VPS_DEPLOY_DIR` variable) — the numbered "ONE-TIME USER
SETUP" in [`deploy.yml`](../.github/workflows/deploy.yml)'s header.

Public repository only:

- **Pages, BRANCH mode** (decided 2026-09-23, dist V021; section 7.3 has the detail): Settings →
  Pages → Source → the dedicated orphan **`gh-pages`** branch (NEVER `main`: the publish tool
  rewrites that tree wholesale and would delete the manifest on the next release), the repository
  variable **`MH_PAGES_BRANCH` = `gh-pages`**, and `MH_PAGES_PUBLISH` **unset** (it gated the retired
  `actions/deploy-pages` job, which fails against a branch source).
  ([Pages](#pages));
- **(Actions mode only — not needed in branch mode, whose job declares no Environment)** **the
  `github-pages` Environment must allow tags `v*`**: Settings → Environments →
  `github-pages` → *Deployment branches and tags* → add a **tag** rule `v*`. GitHub creates that
  Environment by itself on the first Pages deploy, allowing only `main`, and a tag-triggered
  `publish manifest.json to GitHub Pages` job is then rejected with *"Tag v… is not allowed to
  deploy to github-pages due to environment protection rules"* — the manifest is on the release
  but not on Pages, so no launcher sees the update. It happened on `v0.1.0`; the rule was added
  through the API and the job re-run. Check it before every public tag until it has been seen
  to hold once.

### 0.2 Gate the private tree

1. **If `src/mh_dll/mh/Debug` holds no `.obj`, build the Debug solution first** — the export-
   contract step of the gate reads the Debug objects and the gate itself builds only Release. The
   symptom is the gate aborting at `LIBMH EXPORT CONTRACT STALE` with `… holds no .obj files`:
   the [src/mh_dll/README.md](../src/mh_dll/README.md) "The gate" msbuild line with
   `/p:Configuration=Debug`, then again.
2. `python tools/run_gate.py` — every unit green, or red only on a row the private tracker already
   carries for it (a new red is a new row, not a release). A change to the lockstep **hash
   manifest** (a new hash region) makes `tools/lint_fixture_currency.py` report `STALE FIXTURE` /
   `STALE ORACLE` in the replay and ABC units: re-capture the fixtures and oracles under the new
   manifest before going on (`TL-GATE-D25FX` was the worked example).
3. The tree says what ships: the **Known limits** block in [section 1](#1-the-version-convention)
   and the public message template below it match the tracker; `INSTALL.md` and
   `docs/mp-internet.md` describe the launcher as built.
4. Commit, and `git push origin master`.

### 0.3 Let the VPS relay follow the push

The push you just made runs [`images.yml`](../.github/workflows/images.yml) when its watched
paths changed, and a green `images` run **creates** a [`deploy.yml`](../.github/workflows/deploy.yml)
run by `workflow_run`. That run **pauses on the `vps` Environment**: open it in the Actions tab
and approve it (*Review deployments*). Then read its `relay healthy + listening + counters` step:

- `relay healthy; started …`;
- the `"event":"listening"` line — its `protocol_level` is the relay level the client will be
  checked against;
- the `"event":"counters"` line, which must carry `peers_rekeyed` and `health_pings` (the job
  fails by itself when it does not: that is an old image).

**The VPS relay must never be older than the client it will serve** — an outdated relay answers
a newer client's ops with `bad_op` and the players see nothing but a match that never leaves the
relay (`mp:R4a` put the level into HELLO/WELCOME so the client at least says "Relay outdated").
If the push touched none of `images.yml`'s paths, the deployed image already matches; a
deliberate redeploy is `workflow_dispatch` on `deploy.yml`. Prefer a moment when the last
counters line says `peers=0`.

### 0.4 Rehearse on the private remote

1. Write the release message to a file — the **public** template in
   [section 1](#the-public-release-message) even for a rehearsal, so the rehearsal reads the same
   page a player will. Cut an **annotated** tag with a `-rcN` suffix (a `-` anywhere = prerelease)
   and push it to `origin`:

   ```powershell
   git tag -a v0.1.1-rc1 -F tmp\release_message.txt
   git push origin v0.1.1-rc1
   ```

2. Watch the workflow runs on the private repo. A game tag `v*` starts `release` only
   (`build-and-gate`, `selftests-asan`, `selftests-plain`, `publish`; **`publish-pages` is SKIPPED
   here by design** — grey, not red). The launcher is its own tag, `launcher-v*`, which starts
   `launcher-release` (`build-and-test`, `publish`, `sign`; its `publish-pages` is skipped the same
   way). A joint release is two tags, launcher first (section 6). A red gate publishes nothing:
   fix, then the next `-rcN`.
3. **Verify the artifacts, not the run** — in a scratch directory under the repo root (`tmp\rc`,
   say; `tmp/` is gitignored):

   ```powershell
   gh release download v0.1.1-rc1 -R <owner>/mh_re_private -D .
   Get-FileHash *.zip | Format-Table Hash, Path     # against SHA256SUMS, line by line
   Expand-Archive mission_humanity_re-0.1.1-rc1-net.zip -DestinationPath net
   (Get-Item net\mh.dll).VersionInfo.FileVersion            # 0.1.1-rc1+<short sha>: the TAG, as a string
   Select-String '^transport=' net\mh_net.ini              # transport=udp
   gh run download <release run id> -R <owner>/mh_re_private -n manifest-0.1.1-rc1 -D manifest
   python ..\..\tools\gen_update_manifest.py --verify manifest\game.json   # key = update.rs PUBLIC_KEY
   ```

   and in `game.json` the `relay` object holds `addr` and `key`, and the `net` entry's hash and
   size equal the zip and `SHA256SUMS`. In the `publish` job's log the manifest is printed with
   `"key": "<blanked in this log>"` — the key never appears in a log. `launcher-release`'s log
   ends with `exe says: mh_launcher 0.1.1-rc1 ok`.
4. A superseded rc's release and tag may be deleted (`gh release delete`, then
   `git push origin :refs/tags/v…`); the rc that shipped stays as the record.

### 0.5 The public follow-up commit

The public `main` is not a mirror: it is ONE squash commit per publish of the private tree's
publish set, made by `tools/build_public_seed.py`. `--clone-dir` must not exist yet.

```powershell
python tools\build_public_seed.py --follow-up https://github.com/<owner>/mission_humanity_re.git `
  --clone-dir tmp\pub --author-email <the LICENSE holder's email> `
  --push https://github.com/<owner>/mission_humanity_re.git
```

It clones the public repo, replaces its tree with the publish set, runs the secret sweep and
`check_publishable.py --public` **inside the clone**, commits `publish: private <sha> -- <subject>`
(the private HEAD it was cut from) and pushes. `python tools\build_public_seed.py --check` is the
drift gate to run before it when in doubt. Read the sha it pushed: `git -C tmp\pub rev-parse HEAD`.

### 0.6 The public tag

1. In that clone, tag **the public squash commit** — never a private sha, which does not exist in
   the public history — with the same message file, and push the tag:

   ```powershell
   git -C tmp\pub tag -a v0.1.1 <public main sha> -F ..\release_message.txt
   git -C tmp\pub push origin v0.1.1
   ```

2. Watch `release` (and `launcher-release` for a `launcher-v*` tag) on the public repo; this time
   each `publish-pages` job **must be green** (0.1's tag rule is what lets it run).
3. Then:

   ```powershell
   curl.exe -s https://<owner>.github.io/mission_humanity_re/channels/latest/game.json   # the new version, with "relay"
   curl.exe -sI https://<owner>.github.io/mission_humanity_re/channels/latest/game.json.minisig | Select-Object -First 1   # HTTP/1.1 200 OK
   ```

   and the same artifact checks as 0.4.3 against `gh release download v0.1.1 -R
   <owner>/mission_humanity_re` ([Verifying it worked](#verifying-it-worked) has the Pages-vs-
   release hash comparison).

### 0.7 After the tag

- **On a player's box, the launcher sees the update** — Status → *Check for updates* offers the
  new version; Play installs it. This is the one clause the private rehearsal cannot prove
  (release assets need authentication there and Pages is public-only), so it is checked on the
  public release every time.
- **A real two-connection match on the public build**, both peers on home connections through
  the relay, no port forwarding: `python tools/mp_analyze.py <host session dir> <joiner session
  dir>` must say ALL PAIRS IDENTICAL, and the relay's counters line must end the match with
  `peers_rekeyed=2 leg_bad_mac=0` ([docs/mp-internet.md](mp-internet.md) for the player's side,
  [src/relay/README.md](../src/relay/README.md) for the relay's). A desync here is the next
  patch release, not a reason to pull this one.
- The **Known limits** block in the templates below is kept current — retire a line when its
  tracker row closes, add one when a limit ships.
- Log the release in the private tree's session notes (the day's line names the tag and the
  public sha).

## 1. The version convention

**`vMAJOR.MINOR.PATCH`, optionally with a pre-release suffix: `v0.1.0`, `v0.1.0-rc1`, `v0.2.0-beta`.**
The tag keeps its leading `v`; everything derived from it drops it — `MhVersion`, the zip names, the
version in the binaries.

Three rules the workflow *depends on* rather than merely documents:

| | |
| --- | --- |
| **A `-` anywhere in the tag means prerelease** | `v0.1.0-rc1` publishes with `--prerelease`. That is the whole rule — there is no separate flag and no second list to keep in step. |
| **Tags must be annotated** | A lightweight tag is refused before anything is published. `git tag -l --format='%(contents)'` on a lightweight tag helpfully returns the *commit's* message, so the check is `git cat-file -t`, which answers `tag` for an annotated object and `commit` for a lightweight one. |
| **The tag message IS the release notes** | Ruled 2026-09-17. The public repository's history is squash seeds, so auto-generated notes would say nothing a reader can use. An annotated tag with an empty message fails the publish job rather than publishing an empty body. |

So the message is worth writing properly — it is the page every downloader reads:

```
git tag -a v0.1.0 -m "First public release.

Multiplayer with a lockstep turn engine and an authenticated, encrypted link, over UDP through the
project's relay (the launcher configures it; a match starts relayed and goes direct when it can)
or over TCP by address. Three configurations; see README.txt in the zip. Download
mh_launcher.exe, drop it next to mh.exe, run it.

Known limits:
- The TCP transport (`[net] transport=tcp`, the explicit choice -- UDP is the default) has no
  relay: the host's TCP 6501 must be forwarded. Direct dial by address needs a forwarded port with
  either transport.
- 8 players per game.
- One map download at a time per session."
```

### The public release message

The message **is the whole release page** — GitHub shows nothing else for a tag but the asset
list — so a public release carries more than the rc template above. `v0.1.0`'s (read it with
`gh release view v0.1.0 -R <owner>/mission_humanity_re`) has this shape, in plain text with
capitalised headings rather than markdown; keep the shape and rewrite the facts:

```
<Game name> -- multiplayer restored, v<X.Y.Z>          (one title line)

<what this is, two short paragraphs: the game shipped without working multiplayer; a DLL beside
 mh.exe restores it; the launcher keeps it installed and points it at the relay>
<you need your own copy of the game; nothing from the game is in this download>

HOW TO PLAY
1. Download mh_launcher.exe and put it in the game folder, next to mh.exe.
2. Run it. <it finds the game, asks for a configuration, installs it, writes the relay settings>
3. Press Play. In the game: NETWORK GAME -> your name -> <create a game, or pick one from the list>
4. <a match starts through the relay and goes direct when the network allows -- no router setup>

<the Report tab: what a bug report packs and that it is sent only on consent>

WHAT IS IN THE ZIPS (the launcher picks one; see README.txt inside)
- net             <one line>
- net-debug       <one line>
- brokered-debug  <one line>

KNOWN LIMITS
- <the same block as the rc template, one line each, current at this tag>
- Windows only, 32-bit game process. The launcher is a 64-bit Windows program.

<closing pointer: README.md, INSTALL.md and docs/ in the repository>
```

Keep the **Known limits** block current: it is the one place a downloader reads before filing a bug
that is a limit. Three at `v0.2.0` — TCP needs a forwarded port (only UDP has the relay; UDP is the
shipped default since 2026-09-20, so a hand-unzipped install with an untouched `mh_net.ini` is
already on the relay-capable transport), the 8-player cap, and one channel-C map transfer at a time
(`mp:T2a`). Each is retired from the template when its tracker row closes, which is what happened to
the fourth: **the one-row browser (`mp:R2b`) shipped fixed in v0.2.0** — the list now shows every
relay-hosted game — so the line is gone from the block above rather than kept as history. The
example message is the CURRENT template, not a transcript of `v0.1.0`.

## 2. Cutting one

**Rehearse on the private repository first.** Ruled 2026-09-17, and the reason is that the release
job is the only thing in this repository that can publish: a workflow bug on a public tag is
visible to everyone, and a tag is cheap only before it is pushed.

1. **Check the tree is releasable.** The offline gate, exactly as
   [INSTALL.md](../INSTALL.md) section 3 lists it — `run_selftests.py`, `replay_libref.py`,
   `lint_repo.py`. The release workflow runs the same set, but finding out locally costs minutes
   rather than a tag.
2. **Cut an annotated `-rc` tag and push it to the PRIVATE remote.** Watch the run end to end. What
   a good one looks like: `build-and-gate`, `selftests-asan` and `selftests-plain` green (three parallel jobs), then `publish` producing a **prerelease** whose
   body is the tag message, with three zips and a `SHA256SUMS` attached.
3. **Check the artifacts, not the run.** Download a zip, confirm it holds what it should, and read
   the `FileVersion` of a DLL inside it (Explorer's Details tab, or `(Get-Item x.dll).VersionInfo`)
   — it must be the tag. A green pipeline that stamped the wrong thing is exactly the failure this
   flow exists to make impossible, so verify it once per release rather than trusting it.
4. **Then push the tag to the public remote.** Same workflow file, same gate; the release appears on
   the public Releases page.

A **dry run without a tag** is `workflow_dispatch` from the Actions tab: it builds, gates and
packages exactly as a tag run does, uploads `dist/` as a workflow artifact, and **publishes
nothing**. It stamps `0.0.0-dev` — the property sheet's own default — so the strict stamp check
still runs and the zips it produces cannot be mistaken for a release.

**A red gate publishes nothing.** `publish` needs `build-and-gate` and both selftest jobs, so a failing selftest, replay
or lint stops the flow with the tag pushed and no release created. Fix, then move the tag or cut
the next one.

## 3. How the stamp reaches the binaries

[`src/mh_dll/mh_version.props`](../src/mh_dll/mh_version.props) is imported by the five modules a
release ships — `mh`, `mh_net`, `mh_harness`, `libmh_dll` (the **hosted** `libmh.dll`) and
`msvfw32_shim` — and by nothing else. It carries two properties:

| property | default | set by |
| --- | --- | --- |
| `MhVersion` | `0.0.0-dev` | `/p:MhVersion=<tag without the v>` |
| `MhGitSha` | `git rev-parse --short=8 HEAD`, or `unknown` when git cannot answer | `/p:MhGitSha=<short sha>` |

They become three things:

- a **VERSIONINFO resource** in each module, from the shared
  [`src/mh_dll/mh_version.rc`](../src/mh_dll/mh_version.rc). `FileVersion` and `ProductVersion` are
  the full string (`0.1.0-rc1+abc12345`); the numeric `FILEVERSION` field is four integers and
  cannot hold a suffix, so it is the semver triple plus a zero (`0,1,0,0`). **Compare the string,
  not the numbers** — `0.1.0` and `0.1.0-rc1` are the same four integers.
- **preprocessor defines** reaching C++ through `mh_common/include/mh_version.h`.
- **the first line of `mh_net.log`**, written by `mh.dll`:

      ; [build] mh 0.1.0-rc1+abc12345

  which is what makes a bug report's log name its build. It is the reason the stamp exists at all.

A build with no properties stamps `0.0.0-dev+<sha>`, so a development DLL can never read as a
release. The packager refuses to build a zip from binaries whose stamp is not the version it was
asked for, which is what makes the zip's *name* evidence.

## 4. Packaging by hand

The workflow calls one command, and so can you:

```powershell
python tools\release_package.py --version 0.1.0
```

It reads `src\mh_dll\Release` (override with `--release-dir`) and writes `dist\` (override with
`--out`): three zips plus `SHA256SUMS`.

| zip | contents | configuration |
| --- | --- | --- |
| `mission_humanity_re-<V>-net.zip` | `msvfw32.dll` `mh.dll` `mh_net.dll` `mh_net_udp.dll` `mh_net.ini` `LICENSE` `THIRD_PARTY.md` `README.txt` — `[net] transport=udp` is the default, `mh_net.dll` is the explicit `transport=tcp` | (1) all-original + restored multiplayer |
| `mission_humanity_re-<V>-net-debug.zip` | the above **+ `mh_harness.dll`**, and an `mh_net.ini` with the diagnostic logging keys on AND the harness armed — it hashes spine-free without `libmh.dll` since mp:D29 (user decision O6, 2026-09-24) | (1), verbose, instrumented |
| `mission_humanity_re-<V>-brokered-debug.zip` | the above **+ `libmh.dll`** (the hosted build) at the zip root, and an `mh_net.ini` that ALSO arms the harness: per-step hashes + the order record, clock not pinned | (2) brokered, instrumented |

No selftest executable, no standalone binary, no byte of the game.

**The ship `mh_net.ini` is [`src/mh_dll/mh_net.example.ini`](../src/mh_dll/mh_net.example.ini)
verbatim**, with a provenance header. That file already documents every key with its real default,
so a copy of it *is* the shipping configuration and cannot drift from it; a hand-authored minimal
ini would be a second statement of the same defaults with nothing comparing the two. The debug
variant is the same file with four observer keys flipped — `[net] sp_clock_log`,
`[trace] temporal_sp`, `[desync] verbose`, `[input] mouse_trace` — plus, in every zip that carries
`mh_harness.dll`, three `[harness]` keys: `enable=1` (every sim step hashed into `mh_harness.log`),
`fixed_step=0` (the game clock is NOT pinned — the harness's compiled default would make the sim run
one step per present, a different game; `0` is what `test_ui.py --determinism` runs) and
`order_mode=1` (every dispatched order recorded to `mh_orders.bin`). User ruling 2026-09-20: the
debug ini records orders and per-step hashes, because a bug report without them is one the desync
tooling cannot read. The harness keys follow `mh_harness.dll` (mp:D29 + user decision O6,
2026-09-24): until D29 they followed `libmh.dll`, because ruling Q4 refused the instrument in
configuration (1); D29 made it arm spine-free there (the per-step hash reads only the region
registry and owner table, which `mh.dll` answers itself), so `net-debug` now records hashes too. `[debug] overlay` left the list the
same day: the debug ini keeps `overlay=0` — installed but hidden, Ctrl+Alt+D shows it. The tool's
selftest asserts each debug ini differs from the ship ini in exactly its listed lines.

Three refusals, each with a named reason and a non-zero exit:

- **a missing artifact** — all named at once, so one rebuild fixes them all.
- **a version the binaries do not carry** — `--allow-dev` bypasses this for a local dry run against
  an unstamped tree, and nothing else does.
- **the wrong `libmh.dll`** — the hosted and standalone builds share a file name and cannot do each
  other's job ([docs/dll-split.md](dll-split.md)). The candidate must export every row of the
  generated contract `mh.dll` itself binds against; the hosted build resolves all of them and the
  standalone one a small overlapping subset. This refusal has no bypass flag.

`python tools\release_package.py --selftest` runs the rules against fake artifacts in a temp
directory — no toolchain, no game — and is a `lint_repo.py` row.

## 5. What a green release run does not prove

The same thing a green CI run does not prove, because it is the same gate: **the rig half is
bring-your-own-game.** The UI capture suite, the two-peer determinism run and the in-memory patch
parity oracle need a retail copy and a second machine, and a hosted runner has neither. They are
not run and are not pretended to; `src/mh_dll/README.md` "The gate" is the full article and
[INSTALL.md](../INSTALL.md) section 8 is the short one.

So a published release has passed the offline subset, on binaries stamped with its tag, and nothing
more is claimed. Deciding that a build is worth publishing stays a human act — which is exactly what
pushing a tag is.

## 6. The launcher release

**Two tags, two workflows, two signed files per channel (RL8, v0.2.0).** The game and the launcher
release independently:

| tag | workflow | publishes |
| --- | --- | --- |
| `vX.Y.Z[-rcN]` | [`release.yml`](../.github/workflows/release.yml) | the three game zips + `SHA256SUMS` on the tag's Release, and the signed `channels/latest/game.json` on Pages |
| `launcher-vX.Y.Z[-rcN]` | [`launcher-release.yml`](../.github/workflows/launcher-release.yml) | `mh_launcher.exe` on the tag's own Release (unversioned file name; the version is inside, `--verify-binary`), and the signed `channels/latest/launcher.json` on Pages |
| manual | [`promote.yml`](../.github/workflows/promote.yml) | `channels/stable/{game,launcher}.json`: the same artifact, re-signed (section 7.4) |
| weekly + manual | [`bridge-resign.yml`](../.github/workflows/bridge-resign.yml) | the frozen schema-1 `manifest.json` at the old URL (section 7.5) |

A `launcher-v*` tag does not match `release.yml`'s `v*` filter, so the two never fire on one push.
Nothing polls anything any more. **A joint release is two tags, launcher first**: `release.yml`
fetches the SERVED `channels/latest/launcher.json` and `gen_update_manifest.py --channel-launcher`
refuses a game whose `min_launcher` (default: the `MIN_LAUNCHER` constant in the tool — bump it in
the commit that makes the game need a newer launcher) is newer than that launcher. Both tags are
annotated and the tag message is the release notes.

**Layout on Pages** (`gh-pages`, the base URL unchanged): `channels/{latest,stable}/{launcher,game}.json`
each with a `.minisig`, plus the root `manifest.json` bridge. `kind` and `channel` are INSIDE the
signed body, so a stable file served at the latest URL is refused by the launcher; the
launcher's own `channel` setting (`launcher.toml`) picks which pair it follows (`latest` is the
rolling tag build; `stable` is what `promote.yml` has blessed). Schemas: `launcher.json` =
`{schema:2, kind, channel, version, issued_at, url, sha256, size, notes_url?}`; `game.json` =
`{schema:2, kind, channel, version, issued_at, min_launcher, game:{net:{url, sha256, size}},
relay?, notes_url?}`. There is no 30-day STALE check on schema 2; `issued_at` only has to move
forward (REPLAY floor per channel and kind), which is why every re-sign sets it to now.

**The launcher build.** `RUSTFLAGS=-C target-feature=+crt-static` statically links the MSVC CRT, so
the shipped exe imports no `VCRUNTIME140.dll`/`api-ms-win-crt-*.dll` — a per-user install with no
admin rights cannot rely on the VC++ Redistributable being present, and this project ships none
(dist LA1's build-time finding). `MH_UPDATE_BASE_URL` is baked in via `option_env!` from the
`MH_UPDATE_BASE_URL` **repository variable** (Settings → Secrets and variables → Actions →
Variables) — the Pages base URL `manifest.json` is published to. A tag build with that variable
unset fails outright (a launcher with no compiled-in update source is a release nobody can update);
a `workflow_dispatch` dry run is exempt, the same way a dry-run `release.yml` run stamps
`0.0.0-dev` rather than requiring a real tag.

### The manifest

`tools/gen_update_manifest.py` builds and minisign-signs the schema-2 files. `--kind game` reads
`dist/`'s `SHA256SUMS` (already there from `tools/release_package.py`) and names the `net` zip;
`--kind launcher` hashes the exe. Each writes `<kind>.json` + `.minisig` into `--out`; the signature's
trusted comment names kind and channel. `--verify FILE` with no `--public-key` checks against
`update.rs`'s `PUBLIC_KEY` — the key players have, not the one that happened to sign. The tool also
does `--promote` / `--bridge` / `--resign` / `--sign-file` / `--age-days` (7.4, 7.5). It is pure Python standard library — no
`rsign2`/libsodium dependency in CI — for the reasons its own header gives (the part an attacker
attacks is the *verifier*, `minisign-verify` inside the launcher, not this signer; a wrong signer
only fails to publish). **The job fails outright, before touching the network, if the
`MH_LAUNCHER_MINISIGN_KEY` repository secret is unset** — an unsigned manifest is never published.

**The relay (`dist:LA6`).** The manifest may also carry `relay: {addr, key}` — the relay the
launcher writes into every player's `mh_net.ini` (`[net] transport=udp`, `[net] relay=<addr>`) and
`mh_key.txt`. Both values are injected at signing time and only there: the **`MH_RELAY_ADDR`
repository variable** (`host:port`) and the **`MH_RELAY_KEY` repository secret** (the relay's
64-hex deployment key — `machine_config.RELAY_KEY` on the maintainer's machine, never in the tree).
With **neither** set the manifest simply has no `relay` field and the launcher leaves the player's
ini alone — a valid manifest, and what the private rehearsal repo produces. With **one** of the two
set the tool refuses and the job fails: an address without its key is a relay nobody can
authenticate to. The key is a secret only so GitHub masks it in the job log; the published manifest
carries it in clear, by decision (it keeps scanners off the relay, not players — the signature is
what stops a stranger re-pointing launchers). The workflow prints the manifest with the key line
blanked. Rotating the deployment key = change the secret on the VPS (`deploy/secrets/relay_key.txt`)
and the repository secret together, then cut a release: every launcher rewrites `mh_key.txt` on its
next update, and only when the key differs.

**The signing key.** `src/launcher/src/update.rs`'s `PUBLIC_KEY` is compiled into every launcher
build; only the matching secret half — held in the `MH_LAUNCHER_MINISIGN_KEY` repository secret and
nowhere in this tree — can produce a manifest that key accepts. Generate a pair with:

```powershell
rsign generate --unencrypted -p mh_launcher_minisign.pub -s mh_launcher_minisign.key
```

(`cargo install rsign2` first if `rsign` is not on PATH). **Use `--unencrypted`, not
`-W`/`--passwordless`** — rsign2's `-W` still writes a `scrypt`-tagged secret key with an empty
password, which `tools/gen_update_manifest.py`'s reader refuses by name as an encrypted key (see
that file's header). Paste the printed public-key line into `update.rs`'s `PUBLIC_KEY`, set the
secret key file's content as `MH_LAUNCHER_MINISIGN_KEY`, and ship a launcher release carrying the
new `PUBLIC_KEY` **before** the old secret is retired — an already-installed launcher only ever
trusts the key it was built with.

### Pages

The Pages site serves `channels/<channel>/<kind>.json` + `.minisig` and the root bridge
`manifest.json` (the `gh-pages` branch also holds `.nojekyll` and a one-page `index.html`). Each
workflow's `publish-pages` job, after its `publish` job has uploaded every asset the file names,
clones `gh-pages`, commits ITS OWN files, pushes (rebase + retry: four workflows share the branch,
but never a file), requests a Pages build (`POST …/pages/builds`), waits for the build of ITS commit,
and then `curl`s the files back and requires them byte-identical to the signed ones — a stale or
404 manifest reds the run instead of breaking only the players in the field. It needs
`contents: write` + `pages: write` and no third-party action. **One-time repo setup:** Pages source
= the `gh-pages` branch and the variable `MH_PAGES_BRANCH` = `gh-pages`. Unset, the job is skipped
— Pages does not exist on a private free-plan repo, so the private rehearsal leaves it unset (and
`release.yml` then runs the `min_launcher` guard-less, with a warning) and only the public repo sets
it. The earlier Actions-mode job (`actions/deploy-pages`, `MH_PAGES_PUBLISH`) was retired by dist
V021: the hand-cut v0.2.0-rc2 moved Pages to branch mode, where `deploy-pages` cannot deploy, and
the branch is the better home (G288).

### Verifying it worked

Same order as [section 2](#2-cutting-one)'s rehearsal, plus:

- `launcher-release.yml`'s `build-and-test` job is green (fmt, clippy, `cargo test`, and the
  static-CRT import-table check), its `publish` job created the `launcher-v…` Release with
  `mh_launcher.exe`, and `sign` verified `launcher.json` against `update.rs`'s `PUBLIC_KEY`.
- `release.yml`'s `publish` job signed `game.json` (log: `guard ok -- served latest launcher is …`
  unless the repo has no Pages) and verified it the same way.
- The Pages URL (`https://<owner>.github.io/<repo>/channels/latest/game.json`) serves the file the
  run artifact (`manifest-<version>`) holds — `Get-FileHash` should agree; the job's read-back
  already required it.
- A launcher built against the OLD `PUBLIC_KEY` refuses the new file (`refuse SIGNATURE` in its
  log); a launcher built against the current one applies it, per dist LA2's own acceptance clauses.

## 7. Cutting a release BY HAND, when Actions cannot run

Written 2026-09-23, from doing it: `v0.2.0-rc2` was cut and published with GitHub Actions refusing
every job (*"recent account payments have failed or your spending limit needs to be increased"* —
all three jobs dead in ~5 s, both publish jobs skipped). Nothing in the pipeline is magic; every
step below is a command you can run, and the only two things Actions holds that a workstation does
not are the **secrets**.

**What this path gives up, stated rather than hidden:** section 0.4's rehearsal on the private
remote. A hand cut has no green workflow behind it, so the verification in step 6 is not optional
paperwork — it IS the gate, and it is the only evidence the release is good.

### 7.1 What you need that is not in the tree

| | where it lives | note |
| --- | --- | --- |
| minisign secret key | a file on the maintainer's box (`MH_LAUNCHER_MINISIGN_KEY` is a copy of it) | **verify it is the right key before signing** — derive its public half and compare to `PUBLIC_KEY` in `src/launcher/src/update.rs`; a manifest signed by the wrong key is refused by every launcher in the field |
| `MH_REPORT_TOKEN` | the VPS, `<VPS_DEPLOY_DIR>/secrets/report_token.txt` (`~/mh-deploy` by default) — the GitHub secret was read off that file, so the VPS is the source of truth | do not mint a new one to avoid the lookup: rotating strands every launcher already shipped with the old one |
| `MH_RELAY_KEY` | `tools/machine.local.json`'s `RELAY_KEY` | reaches `gen_update_manifest.py` through the ENVIRONMENT, never a command line |
| everything else | repository **variables**, readable with `gh variable get` | `MH_UPDATE_BASE_URL`, `MH_REPORT_URL`, `MH_REPORT_CA_PEM`, `MH_REPORT_SPKI_PIN`, `MH_RELAY_ADDR` |

### 7.2 The steps

1. **Gate the tree** exactly as 0.2 says — `run_gate.py` green, and **reproduce any CI-only lint in
   a `git clone` of the repo** rather than reasoning about it (dead-ends G287: a fingerprint over
   raw bytes hashes the CHECKOUT, so a row can be green on the maintainer's box and red on CI).
2. **Build stamped.** The stamp does not come from the tag by itself:

   ```powershell
   msbuild src\mh_dll\mh.sln /t:Build /p:Configuration=Release /p:Platform=x86 `
     /p:MhVersion=<X.Y.Z[-rcN]> /p:MhGitSha=$(git rev-parse --short=8 HEAD)
   ```

   Read `FileVersion` off every DLL afterwards; `release_package.py` refuses a mismatch, which is
   what makes the zip's name evidence.
3. **Package**: `python tools\release_package.py --version <X.Y.Z[-rcN]>` → three zips +
   `SHA256SUMS` in `dist\`.
4. **Build the launcher** with all five `option_env!` values exported — `MH_LAUNCHER_VERSION`
   (separate from the tag; unset ships `0.0.0-dev` under a release file name), `MH_UPDATE_BASE_URL`,
   `MH_REPORT_URL`, `MH_REPORT_TOKEN`, `MH_REPORT_CA_PEM`, `MH_REPORT_SPKI_PIN`. Two traps, both
   dead-ends G289: `$env:X = (gh variable get ...)` FLATTENS the multi-line CA PEM into one
   space-joined line that is not a certificate, so re-join it with newlines; and confirm the bake
   landed by finding the strings in the built exe, not by trusting that cargo rebuilt.
5. **Manifests** (schema 2, one per kind; the launcher tag and the game tag are separate, launcher first):

   ```powershell
   python tools\gen_update_manifest.py --kind launcher --channel latest --version <X.Y.Z> `
     --launcher-exe mh_launcher.exe --launcher-url https://github.com/<owner>/<repo>/releases/download/launcher-v<X.Y.Z>/mh_launcher.exe `
     --notes-url … --secret-key <key> --out pages\channels\latest
   python tools\gen_update_manifest.py --kind game --channel latest --version <X.Y.Z> --dist dist `
     --asset-base-url https://github.com/<owner>/<repo>/releases/download/v<X.Y.Z> `
     --channel-launcher pages\served\launcher.json --notes-url … --secret-key <key> --out pages\channels\latest
   ```

   with `MH_RELAY_ADDR`/`MH_RELAY_KEY` in the environment for the game file. `--asset-base-url` is
   the RELEASE download URL, never the Pages URL. `--channel-launcher` is the launcher.json
   (+ `.minisig`) currently SERVED for that channel (`curl` it); the tool verifies it and refuses a
   `--min-launcher` newer than it. Then `--verify` each output.
6. **Publish, assets FIRST and the manifest LAST** (G288 — a live manifest whose assets do not exist
   yet points every launcher at 404s):
   a. `build_public_seed.py --follow-up … --push` — the public squash commit.
   b. `git tag -a <tag> <public main sha> -F <message file>` in that clone, and push it.
      **The push starts `release` (a `v*` tag) or `launcher-release` (a `launcher-v*` tag) if Actions
      can run.** On `v0.2.0-rc3` (2026-09-25) they did. Cancel the run (`gh run cancel <id>`)
      before its publish jobs: they would replace the hand-built assets with CI builds whose hashes
      the manifest you signed does not carry.
   c. `gh release create <tag> -R <owner>/<repo> [--prerelease] --notes-file <message file>` with
      the three zips and `SHA256SUMS` (game tag) or `mh_launcher.exe` (launcher tag). This is the
      REST API — it works with Actions dead.
   d. Only now put the signed `<kind>.json` + `.minisig` on the Pages branch under
      `channels/latest/` (7.3).
7. **Verify as a player, not as a maintainer.** This is the step that replaces the rehearsal:
   - `curl` the SERVED manifest and check it is byte-identical to the one you signed;
   - verify its signature against the key compiled into the launcher, extracted from
     `update.rs` — not against the key you happen to have signed with;
   - download EVERY url the manifest names and compare to the signed hashes;
   - unzip the published `net` zip and read `mh.dll`'s `FileVersion`.

   All four passed for `v0.2.0-rc2`, and the third is what proves a launcher gets what was signed.

### 7.3 Pages: serve the manifest from a branch nothing rewrites

Pages is in **branch** (`legacy`) mode, decided 2026-09-23 (dist V021). The manifest is a
committed file on a **dedicated orphan branch (`gh-pages`), never `main`**: `build_public_seed.py`
REPLACES `main`'s tree wholesale on every publish, so a manifest committed there is deleted by the
NEXT release — a break that arrives one release after the change that caused it. The branch holds
only `channels/**`, the bridge `manifest.json` (+ `.minisig`), `.nojekyll` and a one-page `index.html`.

- **With Actions**, each workflow's `publish-pages` job does the commit + build + read-back (the
  [Pages](#pages) section), gated on `MH_PAGES_BRANCH`.
- **By hand**, commit the signed files to `gh-pages` yourself. A push may not trigger a build by
  itself; force one with `gh api -X POST repos/<owner>/<repo>/pages/builds` and confirm with
  `…/pages/builds/latest` that the built commit is the one you pushed.

**`MH_PAGES_PUBLISH` stays unset.** It gated the retired `actions/deploy-pages` job, which fails
against a `legacy` source; switching back to Actions mode would mean restoring that job, setting
Source → GitHub Actions, re-checking the `github-pages` Environment's `v*` tag rule (0.1), and
deleting `gh-pages` so nothing serves a stale manifest.

**Whichever mode, read the site back** (`curl` the manifest and its `.minisig`): nothing else in the
release flow fetches Pages, so a 404 there is invisible to a green pipeline and breaks only the
players already in the field.

### 7.4 Promote latest to stable

`latest` is what a tag publishes; `stable` is only ever a promotion. Promotion re-signs the SAME
artifact — `sha256`, `size`, `url`, `version`, `min_launcher` and `relay` are carried over byte for
byte; only `channel` and a fresh `issued_at` change (the tool re-parses its output and refuses
otherwise). No rebuild, no new bytes.

**With Actions:** run `promote.yml` (workflow_dispatch, input `kind` = `game` | `launcher` | `both`).
It fetches the SERVED `channels/latest/<kind>.json`, verifies it against `update.rs`'s `PUBLIC_KEY`,
promotes, and publishes `channels/stable/` with the usual read-back. `both` promotes the launcher
first; the game is then guarded against the stable launcher, so a stable player is never offered a
game their launcher cannot run (`game` alone needs a stable launcher already served). **After the
FIRST launcher promotion, also dispatch `bridge-resign.yml`** so rc7.1 arrivals get the new
launcher now rather than on the next weekly run (7.5).

**By hand:**

```powershell
curl.exe -fsSL <base>/channels/latest/game.json -o in\game.json          # + .minisig
python tools\gen_update_manifest.py --promote in\game.json --to stable --secret-key <key> `
  --channel-launcher <stable launcher.json> --out out\channels\stable
python tools\gen_update_manifest.py --verify out\channels\stable\game.json
```

Then publish `out\channels\stable\*` to `gh-pages` (7.3) and read the SERVED copy back. Order for a
joint stable release: launcher first.

### 7.5 The bridge (RL10): keep the schema-1 `manifest.json` alive for rc7.1 launchers

An installed rc7.1 launcher reads `<base>/manifest.json` (schema 1) and refuses it STALE once
`issued_at` is more than 30 days old. From v0.2.0 that file is the BRIDGE: schema 1, `version` and
the three game entries exactly as rc7.1 shipped them (the rc7.1 game update is a no-op), a `launcher`
entry copied from the verified `channels/stable/launcher.json` (version, url, sha256), the relay
and notes as before. An rc7.1 launcher offers the launcher update on one click, installs it and
restarts with `--update`; the new launcher then follows its channel's schema-2 files and never
reads the bridge again. The new launcher must keep answering `--verify-binary` (`mh_launcher <ver>
ok`) and accept `--update`, `--view` and `--exit-after-update` for this to work. Bridge arrivals
default to the `stable` channel (user decision 2026-10-08).

**Until the v0.2.0 launcher is promoted to stable** the bridge's launcher entry stays as is and
only `issued_at` moves. The bridge ends at **v0.2.0 stable + 6 months**, recorded in the repository
variable `MH_BRIDGE_UNTIL` (`YYYY-MM-DD`, set by the maintainer; unset = no end date and a
warning).

**With Actions:** `bridge-resign.yml` runs weekly (Mondays 05:17 UTC) and on dispatch. It verifies
the served bridge against `PUBLIC_KEY`, copies the launcher entry from the served stable launcher
when it exists, re-signs when the served bridge is ≥ 14 days old (or the stable launcher moved),
publishes, reads the SERVED file back byte for byte, and — every run — goes red if the served bridge
is more than 21 days old. After `MH_BRIDGE_UNTIL` every run is a green no-op; disable the workflow
then. Two risks the schedule cannot cover: GitHub disables scheduled workflows after 60 days
without repository activity, and Actions billing can refuse every job (the reason this section
exists). Look at the Actions tab at least monthly while the bridge lives.

**By hand** (the fallback, and what ran for the first time on 2026-10-08 with `--resign`):

1. Fetch what is served: `curl -fsSL <base>/manifest.json -o in\manifest.json` and `.minisig`;
   optionally `<base>/channels/stable/launcher.json` + `.minisig` into `in\`.
2. Re-issue (add `--launcher-json in\launcher.json` only if the stable launcher exists):

   ```powershell
   python tools\gen_update_manifest.py --bridge --from-manifest in\manifest.json `
     [--launcher-json in\launcher.json] --secret-key <key> --out out
   python tools\gen_update_manifest.py --verify out\manifest.json
   ```

   The tool refuses unless the input bridge (and the launcher file) verify against `update.rs`'s
   `PUBLIC_KEY`, the secret key is that key's pair, `issued_at` does not go backwards and the
   launcher version does not roll back; it asserts version, game entries, relay and notes are
   unchanged. `diff in\manifest.json out\manifest.json` shows `issued_at` (and the launcher entry).
3. **Publish (needs the maintainer's explicit go):** commit `out\manifest.json` + `.minisig` to
   `gh-pages` at the ROOT (7.3), force a Pages build, read the SERVED copy back and re-verify
   (`--verify`, `--age-days`).

### 7.6 Re-sign any schema-1 manifest (`--resign`)

`--resign in\manifest.json --secret-key <key> --out out` sets **only** `issued_at` to now and signs
again, with the same checks as the bridge (`--bridge` without `--launcher-json` does the same plus
the bridge shape check). `--sign-file <file> [--trusted-comment ...] [--out sig]` signs any other
file with the same key path (default output `<file>.minisig`). Both read only unencrypted keys (see
the tool header).
