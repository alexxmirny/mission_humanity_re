# Cutting a release

How a version of this project reaches somebody who is not going to build it. Three drop-in zips and
a `SHA256SUMS`, attached to a GitHub Release, built and gated by
[`.github/workflows/release.yml`](../.github/workflows/release.yml) from a pushed tag.

This is the maintainer's page. A *user* wants [INSTALL.md](../INSTALL.md) — "Download a release" for
the zips, section 2 onward for building from source.

- [1. The version convention](#1-the-version-convention)
- [2. Cutting one](#2-cutting-one)
- [3. How the stamp reaches the binaries](#3-how-the-stamp-reaches-the-binaries)
- [4. Packaging by hand](#4-packaging-by-hand)
- [5. What a green release run does not prove](#5-what-a-green-release-run-does-not-prove)
- [6. The launcher release](#6-the-launcher-release)

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
mh_launcher-<ver>.exe, drop it next to mh.exe, run it.

Known limits:
- The TCP transport (`[net] transport=tcp`, the explicit choice -- UDP is the default) has no
  relay: the host's TCP 6501 must be forwarded. Direct dial by address needs a forwarded port with
  either transport.
- 8 players per game.
- One map download at a time per session.
- The in-game list shows one relay-hosted lobby at a time (mp:R2b)."
```

Keep the **Known limits** block current: it is the one place a downloader reads before filing a bug
that is a limit. The four above are the state at v0.1.0 — TCP needs a forwarded port (only UDP has
the relay; UDP is the shipped default since 2026-09-20, so a hand-unzipped install with an untouched
`mh_net.ini` is already on the relay-capable transport), the 8-player cap, one channel-C map
transfer at a time (`mp:T2a`), and the one-row browser (`mp:R2b`) — and each is retired from the
template when its tracker row closes.

## 2. Cutting one

**Rehearse on the private repository first.** Ruled 2026-09-17, and the reason is that the release
job is the only thing in this repository that can publish: a workflow bug on a public tag is
visible to everyone, and a tag is cheap only before it is pushed.

1. **Check the tree is releasable.** The offline gate, exactly as
   [INSTALL.md](../INSTALL.md) section 3 lists it — `run_selftests.py`, `replay_libref.py`,
   `lint_repo.py`. The release workflow runs the same set, but finding out locally costs minutes
   rather than a tag.
2. **Cut an annotated `-rc` tag and push it to the PRIVATE remote.** Watch the run end to end. What
   a good one looks like: `build-and-gate` and `selftests` green (two parallel jobs), then `publish` producing a **prerelease** whose
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

**A red gate publishes nothing.** `publish` needs both `build-and-gate` and `selftests`, so a failing selftest, replay
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
| `mission_humanity_re-<V>-net-debug.zip` | the above **+ `mh_harness.dll`**, and an `mh_net.ini` with the diagnostic logging keys on (the harness itself cannot arm without `libmh.dll` — ruling Q4 — so it is carried, not armed) | (1), verbose |
| `mission_humanity_re-<V>-brokered-debug.zip` | the above **+ `libmh.dll`** (the hosted build) at the zip root, and an `mh_net.ini` that ALSO arms the harness: per-step hashes + the order record, clock not pinned | (2) brokered, instrumented |

No selftest executable, no standalone binary, no byte of the game.

**The ship `mh_net.ini` is [`src/mh_dll/mh_net.example.ini`](../src/mh_dll/mh_net.example.ini)
verbatim**, with a provenance header. That file already documents every key with its real default,
so a copy of it *is* the shipping configuration and cannot drift from it; a hand-authored minimal
ini would be a second statement of the same defaults with nothing comparing the two. The debug
variant is the same file with four observer keys flipped — `[net] sp_clock_log`,
`[trace] temporal_sp`, `[desync] verbose`, `[input] mouse_trace` — plus, in the zip that carries
`libmh.dll`, three `[harness]` keys: `enable=1` (every sim step hashed into `mh_harness.log`),
`fixed_step=0` (the game clock is NOT pinned — the harness's compiled default would make the sim run
one step per present, a different game; `0` is what `test_ui.py --determinism` runs) and
`order_mode=1` (every dispatched order recorded to `mh_orders.bin`). User ruling 2026-09-20: the
debug ini records orders and per-step hashes, because a bug report without them is one the desync
tooling cannot read. The harness keys follow `libmh.dll` because the instrument reads the sim
through the spine and refuses in configuration (1) (ruling Q4). `[debug] overlay` left the list the
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

**Same tag, two workflows, one release.** [`.github/workflows/launcher-release.yml`](../.github/workflows/launcher-release.yml)
builds and tests the Rust launcher (`src/launcher`) and attaches `mh_launcher-<version>.exe` to the
tag's GitHub Release; [`release.yml`](../.github/workflows/release.yml)'s `publish` job — the same
job that creates the release with the three game zips — also signs and attaches
[`manifest.json`](#the-manifest) and publishes it to GitHub Pages. The two workflows are not `needs:`
of each other (GitHub Actions cannot express that across files); each polls the GitHub Release for
the ONE thing it is missing from the other. Both files' headers carry the full rationale — read
`launcher-release.yml`'s "THE ORDERING PROBLEM" section before changing either.

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

`tools/gen_update_manifest.py` builds and minisign-signs `manifest.json` (dist LA2's schema) from
`dist/`'s `SHA256SUMS` (already there from `tools/release_package.py`) plus the launcher exe
`launcher-release.yml` attached to the same release. It is pure Python standard library — no
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

`manifest.json` + `manifest.json.minisig` are the *entire* Pages site — nothing else is hosted
there. `release.yml`'s `publish-pages` job deploys them with `actions/upload-pages-artifact` +
`actions/deploy-pages` (first-party GitHub actions, the same trust tier as `actions/checkout`)
rather than a `gh-pages` branch: a branch would need a third-party action or hand-rolled git-push
scripting, and accumulates every past deploy's binary content in its history forever unless
something force-pushes it, where the Pages deployment API keeps one current deployment with
GitHub's own deployment history and no git commit at all. **One-time repo setup this needs, that no
workflow run can do for itself:** Settings → Pages → Source → *GitHub Actions*, and the repository
variable `MH_PAGES_PUBLISH` = `true`. The job is skipped without that variable — GitHub Pages does
not exist on a private repository on the free plan, so the private rehearsal leaves it unset (the
release and its attached `manifest.json` + `.minisig` are unaffected) and only the public repo sets it.

### Verifying it worked

Same order as [section 2](#2-cutting-one)'s rehearsal, plus:

- `launcher-release.yml`'s `build-and-test` job is green (fmt, clippy, `cargo test`, and the
  static-CRT import-table check) and its `publish` job attached `mh_launcher-<version>.exe`.
- `release.yml`'s `publish` job attached `manifest.json` + `manifest.json.minisig` alongside the
  three zips and `SHA256SUMS`.
- The Pages URL (`https://<owner>.github.io/<repo>/manifest.json`) serves the same `manifest.json`
  that is on the release — `certutil -hashfile` or `Get-FileHash` should agree.
- A launcher built against the OLD `PUBLIC_KEY` refuses the new manifest (`refuse SIGNATURE` in its
  log); a launcher built against the current one applies it, per dist LA2's own acceptance clauses.
