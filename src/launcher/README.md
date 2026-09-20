# mh_launcher

The per-user launcher for Mission Humanity: it **keeps a release up to date**, puts it next to your
`mh.exe`, **sets the relay up** in the game's configuration, starts the game, tells you whether the
game stopped or was stopped, and — when it was stopped — **writes a minidump of it from outside the
crashed process**, packs a report, shows you every file in it, and sends it only once you say so.
**dist LA1** (the skeleton, the launch, the exit classification), **dist LA2** (the signed-manifest
update path), **dist LA4** (the report zip and the crash capture), **dist RP1** (the upload: a
signed multipart POST over a pinned TLS connection, one retry, and an outbox for what still did not
go), **dist LA6** (the relay from the signed manifest, and the Host / Join front page), **dist LA7**
(it finds `mh.exe` by itself) and **dist LA8** (the configuration picker, and a Host / Join that
installs what is missing before it starts the game).

How the crash capture works, what goes into a report and what is redacted out of it (and why the
multiplayer key gets into a report by three routes rather than the two the tracker names) are in
the launcher page of the maintainer docs; in short: mh.dll's vectored handler writes a small
marker and signals the launcher, the launcher writes the minidump from outside the process, and
every text file in the zip is scrubbed of any 64-hex-digit run before it is packed.

```powershell
cargo build -p mh_launcher --release      # target\release\mh_launcher.exe
```

64-bit (`x86_64-pc-windows-msvc`) launching a 32-bit game, which is fine and deliberate — nothing
here loads into the game's process, so there is no reason to build an i686 binary (plan decision
D10).

## What it does

| view | |
| --- | --- |
| **Play** (the front page) | the **configuration** (`net` / `net-debug` / `brokered-debug`, one line each on what they are for; a *Change...* button, or the open picker when nothing is installed yet), then **Host** and **Join** — both make sure the picked configuration is installed (downloading, verifying and installing it first if it is not, with the progress shown in place), write the relay from the accepted manifest into the game's configuration, and start the game; the line under them says which relay, or that there is none. Then the launcher waits: `running — pid N` while the game runs, and one line when it ends. When the launcher could not find `mh.exe` by itself (below), the page opens with a *Where is mh.exe?* field instead |
| **Status** | the game directory (found, typed or picked), whether an `mh.exe` is there, what is installed in it, *Install* / *Uninstall* for a release zip you already have, and the **Updates** block: where the manifest comes from, *Check for updates*, *Update the game*, *Update the launcher* |
| **Report** | a required description, the session log directory it will zip, the crash (if there was one), a checkbox for the memory snapshot, and *Build the report* — which writes `%LOCALAPPDATA%\MissionHumanity\reports\mh_report_<UTC>.zip` and lists what went into it; then *Send this report…*, which opens the **consent screen** and, if you agree there, uploads it. Reports an earlier session could not send are listed at the top of this view on every launch until they go |

## Where it keeps things

```
%LOCALAPPDATA%\MissionHumanity\
    launcher.toml            the game directory, what is installed there, the update URL,
                             the configuration picked on the Play page
    versions\<ver>\          one unzipped release set per version; the newest two are kept
    versions\<ver>.staging\  a download in flight — becomes versions\<ver>\ by a rename
    accepted\               the last manifest.json that passed every gate, with its .minisig —
                             re-verified before its relay is written (LA6)
    download\                the zip being verified, deleted once it has been unpacked
    update\                  a candidate launcher, deleted once its health gate has answered
    logs\launcher.log        this program's own log, one line per action, flushed per line
    reports\                 the report zips this launcher built, and the dumps beside them
    reports\outbox\          reports the collector has not taken yet — re-offered every launch
```

Per-user, so **nothing here ever needs elevation** — no UAC prompt on install, launch or (from LA2)
update. The binary embeds no application manifest, so Windows runs it `asInvoker`, and it is not
named `setup`/`install`/`update`, which is what would otherwise trip UAC's installer-detection
heuristic on a name alone.

## Finding the game = the launcher's own folder, then the current one, then the saved one

The intended way to use this program is to **download it, drop it into the game folder, and run
it** (dist LA7). So at start the launcher looks for `mh.exe` in this order and takes the **first**
folder that has it, without asking:

1. the folder the launcher executable itself is in;
2. the process's current directory (a shortcut's *Start in*, or a console opened in the game folder);
3. the game directory saved in `launcher.toml` from an earlier run.

What it finds is written back to `launcher.toml`, so the Status field is pre-filled and the Play
page opens ready with Host / Join enabled. The *Where is mh.exe?* prompt appears only when none of
the three holds the game. A saved directory whose `mh.exe` has gone is **named** in the status line
(`the saved game directory … no longer holds mh.exe`) rather than silently kept — with the folder
that was used instead when one was found, or with the prompt when none was. `--game-dir` on the
command line skips the search altogether: that is the test harness's path, and it still overrides
everything.

## Installing = unzipping, plus a receipt

The copy is the paragraph from [INSTALL.md](../../INSTALL.md): every file in the zip goes **next to
the game executable**, flat, no subfolder. What the launcher adds is `mh_launcher_installed.txt` in
the game directory — one row per file with its SHA-256 and whether it displaced something.

**The row that makes the receipt necessary is `mh.dll`.** The retail game ships its own `mh.dll`
(exporting `DecompressLZWData`, `GetSightAreaFromRadius`, `MH_HostedPoolBase`) and this project's
`mh.dll` is a drop-in that re-exports those and adds its own — so an install **overwrites a game
file**, the only one it does. That file is parked as `mh.dll.mhbak` first and put back on uninstall;
an uninstall that merely deleted what it copied would leave the game unable to start. The digests
are the guard in the other direction: a file whose content no longer matches its row was changed by
somebody else since, so it is left alone and named in the result instead of being deleted.

## Updating = a signed manifest, then a rename

The launcher fetches one small file, `manifest.json`, from a base URL (`launcher.toml`'s
`update_base_url`, by default this project's GitHub Pages site) together with its
`manifest.json.minisig`, and **verifies the signature against a single public key compiled into the
binary before it parses a byte of it**. A manifest that does not verify is not read.

**Why a signed file and not the GitHub API** (plan decision D11): the unauthenticated REST limit is
60 requests an hour *per IP* and a `304 Not Modified` still spends one, so a polling launcher
rate-limits a whole household together; and `releases/latest` sorts by the tagged commit's date
rather than by publication date, so it can name a release that is not the newest. **The launcher
never contacts `api.github.com`** — `update::check_url` refuses that host by name, a unit test
asserts it, and LA2's acceptance run measured it with the Windows Filtering Platform connection
audit (the only outbound connection during a full update was the stand-in server).

Four things are refused, each with a named reason that goes in the log:

| refusal | what it is |
| --- | --- |
| `refuse SIGNATURE` | the file was not signed by the key this build carries |
| `refuse NOT NEWER` | the offered version is not newer than the installed one — a **rollback**, whoever signed it (semver, so `0.2.0-rc1 < 0.2.0`) |
| `refuse STALE` / `refuse CLOCK` | the manifest is more than 30 days old (a **freeze** — an old genuine file pinned so the launcher never learns of a newer one) or more than a day in the future |
| `refuse URL` | not `https`, not on the configured origin, or `api.github.com` |

Then the apply, in this order and no other: download → **check the SHA-256 the signed manifest
carries** → unpack into `versions\<ver>.staging\` → **rename** it to `versions\<ver>\` → copy beside
`mh.exe`. The rename is what makes it atomic; unpacking in place leaves a window in which the version
directory holds half of two releases.

**The previous version stays.** Nothing is pruned until the new one has actually run: a successful
launch writes `first_run_ok` inside `versions\<ver>\`, and only then is the list trimmed to the
newest two. A game that crashes in the first seconds does *not* count as having started — which is
the whole point, since that is what a broken install looks like.

**The launcher's own update is gated by running the candidate.** It is downloaded, digest-checked,
and then executed with `--verify-binary`: it must exit 0 and print the version the manifest promised.
Only then does `self-replace` put it in place of the running executable and start it. A digest proves
the bytes are the bytes that were signed; only executing them proves they are a program that works on
*this* machine — the missing VC++ runtime, the wrong architecture, the antivirus quarantine. If the
gate fails, the candidate is deleted and the old launcher carries on.

## Play = pick a configuration, press Host or Join, and the rest is done for you

The front page carries a picker with the three configurations the signed manifest offers — the
same three zips `tools/release_package.py` builds, in the words of the release page's table:

| pick | what it is |
| --- | --- |
| `net` | the game as shipped, plus the restored multiplayer — what most players want. **Preselected** |
| `net-debug` | the same, with `mh_harness.dll` and the diagnostic logging keys switched on in `mh_net.ini` |
| `brokered-debug` | the hosted / brokered build (`libmh.dll`) — for testing the reimplementation, not for play |

The picker is open when nothing is installed in the game folder yet, and behind a *Change...*
button once something is. A pick is written to `launcher.toml` (`chosen_tag`) and installs
nothing by itself: **Host and Join are what install** (dist LA8). Pressed on a folder that does
not hold the picked configuration, they run the update path first — fetch the signed manifest,
download the picked zip, check its digest, unpack and swap it in, copy it beside `mh.exe`,
provision the relay — with each step shown on the page, and start the game when it is in. A
folder that already holds the picked configuration just starts. `update::readiness` decides which,
from the receipt (`mh_launcher_installed.txt`), not from `launcher.toml`.

**Switching is an uninstall followed by an install**, never a copy over the top: the old set is
removed by its receipt (so `mh.dll.mhbak` — the game's own dll — is put back and then parked
again by the new install, and stays the *game's* file rather than becoming our previous one), and
the new zip is copied in. The order is download → verify → stage → uninstall old → install new, so
a download that fails leaves the old configuration in place, and a manifest that does not carry
the picked tag is `refuse MALFORMED` before anything is removed. The "is this newer" gate is
asked against the receipt's configuration: a different configuration at the same version is a
switch, not a rollback, and is allowed.

`--launch` follows the same path, so a fresh folder can be brought to a running game from a
command line: `mh_launcher --app-dir <state> --update-url <base> --launch --exit-after-launch`
with `chosen_tag` set in `launcher.toml` (what clicking the picker writes). A refused install
closes the launcher with exit code 1 when `--exit-after-launch` was given.

## Host / Join = the relay from the signed manifest, then the game

The front page is two buttons, and they do the same thing: **put the relay in place and start the
game**. Hosting or joining is decided in the game's own menu afterwards (NETWORK GAME → create a
game, or → *Refresh list* to browse the relay's directory — mp:R2/R7); the two buttons exist so the
front page says that in two words.

**Where the relay comes from.** The signed `manifest.json` may carry
`"relay": {"addr": "host:port", "key": "<64 hex>"}` beside the asset table (dist LA6;
`tools/gen_update_manifest.py --relay-addr/--relay-key`, which the release workflow feeds from the
`MH_RELAY_ADDR` repository variable and the `MH_RELAY_KEY` secret). The field is inside the signed
body, so the only thing that can point every launcher at a relay is the release key. Every accepted
manifest is kept under `accepted\` **with its signature**, and the launch path re-verifies that copy
before reading its relay — an edited copy verifies as nothing and provisions nothing.

**What is written, and how.** On install, on update and before every launch:

| file | what | how |
| --- | --- | --- |
| `mh_net.ini` | `[net] transport=udp` and `[net] relay=<addr>` | **edited in place, line by line, over the raw bytes**: the first `[net]` section is found, the first `transport=` and `relay=` lines in it are replaced (a later duplicate is dead text to the game and stays), a missing one is inserted right after the `[net]` header, a missing section is appended, a missing file is created with just that block. Every other byte — comments, ordering, blank lines, CRLF/LF, a Latin-1 comment — is preserved, and a file that already says the right thing is not rewritten at all |
| `mh_key.txt` | the key, on line 1 | written only when the file's first non-comment line (BOM ignored, case ignored — the game's own reader) differs. A key file the game wrote with the same key is left exactly as it is |

A manifest **without** the field touches **neither** file — an existing ini stays byte-identical —
and the front page says "No relay". A relay the signer would not have produced (a `;` in the
address, a bad port, a key that is not 64 hex digits or `open`) makes the whole manifest
`refuse MALFORMED` at parse time rather than a half-provisioned game directory. On the install
path the ini is provisioned **before** its SHA-256 goes into `mh_launcher_installed.txt`, so the
receipt describes the file as the launcher left it and *Uninstall* still recognises it; `mh_key.txt`
is not in the receipt (the game mints one itself when there is none, and an uninstall that deleted
the key would cut the player off from every relay game they had been playing). A launch whose
provisioning fails (a read-only ini, say) is **refused**, not started — a player who pressed *Host*
and got a game quietly playing direct would blame the relay.

**What a report says about it.** The relay's host must not land in a report, so `report.rs` blanks
the ini's `relay=` line by name and scrubs the address it held (and its bare host, when dotted) out
of every text file in the zip — which covers `mh_net.log`'s `udp relay leg UP -- host:port` line
without a list of log formats. The launcher itself never logs the address; the front page shows
it.

`--launch` is exactly what the buttons do, so the whole path is scriptable:
`mh_launcher --app-dir <state> --game-dir <game> --update --exit-after-update`, then
`--launch --exit-after-launch`, and `mh_net.log` in the new session directory carries the
`udp relay` lines.

## Telling a crash from a quit

The launcher **stays resident and waits on the child**, because that is the only place the exit code
exists — a launcher that spawns and quits has thrown away the one fact LA4's crash report is built
on. The code is read as a `u32` and classified by the NTSTATUS severity field (top two bits set):

| exit code | shown as |
| --- | --- |
| `0` | *the game exited normally (code 0)* |
| `0xC0000005` (Rust hands it over as `-1073741819`) | *the game CRASHED: 0xC0000005 (STATUS_ACCESS_VIOLATION)* |
| any other `0xCxxxxxxx` | *the game CRASHED: 0x…* |
| `0x80000003`, `1`, … | *…not a crash, but not a clean exit* |

Testing the shape rather than a list of constants is what makes an unfamiliar fault still read as a
fault. Two boundaries it does not cross, both on purpose: severity `WARNING` (a debugger's
`STATUS_BREAKPOINT`) is not a crash, and a small non-zero code is the program's own refusal —
flattening those two would make "it wouldn't start" and "it faulted" the same line in every report.

`src/launcher/src/bin/mh_exit_probe.rs` is the fixture that proves the round trip on a real `wait()`: copy it into
an empty folder **as `mh.exe`**, put a code in `mh_exit_probe_code.txt` beside it (the launcher runs
the game with no arguments, so the file is how the fixture is told), point `--game-dir` at the folder
and press Play.

## Sending a report = a consent screen, then one signed POST

`Send this report…` does not send anything. It opens a screen that lists **every file in the zip
with its size**, the zip's SHA-256, the address it would go to, your description as written, and
the whole of `report.json` — and only the button under that list starts a connection. The command
line behaves identically: `--send <zip>` prints exactly the same text and exits **2** having opened
no socket unless `--consent` is also given.

The upload is `POST /v1/reports`, multipart (`description`, `meta`, `report`), authenticated by a
write-only token compiled into this build plus an HMAC-SHA256 over the timestamp and the body — the
scheme `src/collector` defines, with a unit test carrying a vector generated by the server's own
module so the two halves cannot drift.

**TLS trust is pinned, and it is pinned to the ROOT.** The collector has no domain name yet, so its
edge serves a certificate from its own local CA and no public authority vouches for it. This
launcher therefore trusts exactly one root — the certificate compiled into it — and neither the
Windows store nor webpki's roots, so an enterprise MITM root or a hand-installed CA cannot re-point
it. It has to be the root rather than the leaf: that edge re-issues the leaf about every twelve
hours and the intermediate every seven days, each with a fresh key (measured on the live edge on
2026-09-18; the served chain is leaf + intermediate, and the root is never sent). Alongside the
certificate the build carries the **SHA-256 of that root's SubjectPublicKeyInfo**, and the uploader
refuses to open a socket unless the certificate it was given hashes to it — so a mis-fetched or
silently re-keyed CA fails closed instead of being trusted. Plain `http://` is refused outright,
with no local exception: a local collector can serve TLS too.

**One retry, then kept.** A socket error, a TLS refusal, a 5xx, a 429 or a 408 is tried once more
two seconds later; if that fails the zip is moved to `reports\outbox\` and the Report view offers
it again on every launch until it goes (`--outbox` lists it, `--send-outbox` sends it). A 4xx that
is about the request rather than the moment — 413, 422, 401 — is **not** kept: the identical bytes
would meet the identical refusal.

The outbox holds zips and nothing else: no index, no sidecar. A resend reads the description and
`report.json` back **out of the zip**, which is why a report sent days later in a different process
produces exactly the POST an immediate send would have.

## Two renderers

eframe defaults to `glow`, which needs OpenGL 2.0+ from the display driver. A Windows guest with a
synthetic display adapter (Hyper-V, VirtualBox), a plain RDP session, or a fresh install that has not
met its GPU driver has Microsoft's software OpenGL 1.1 and no ICD, and `run_native` then fails
outright with `egui_glow requires opengl 2.0+` — measured on this project's own test VM, which is
exactly the kind of machine a twenty-year-old game gets played on. So the launcher tries **wgpu**
(D3D12/D3D11, and WARP in software) **first and glow second**, logging which one it got.

## Command line

A GUI program with a command line, because every clause LA1 is accepted on is about what the program
*does*, and a verification that can only be performed by a person clicking is a verification that
runs once.

```
--game-dir <path>     use this game directory and remember it (skips the search above)
--install <zip>       install this release zip at startup
--uninstall           undo the launcher's install at startup
--launch              start the game at startup -- exactly what Host and Join do (the picked
                      configuration is installed first if it is missing, then the relay from
                      the accepted manifest is written into mh_net.ini + mh_key.txt)
--exit-after-launch   close the launcher once the game it started has exited
--view <name>         open on play | status | report (default: play, the Host / Join page)
--size <W>x<H>        initial window size in points (default 1280x720, minimum 800x600)
--app-dir <path>      keep launcher state here instead of %LOCALAPPDATA%\MissionHumanity

--send <zip>          describe this report, and send it if --consent is also given
--send-outbox         the same, for every report waiting in reports\outbox
--consent             yes, send what was just described. Without it, nothing is sent (exit 2)
--outbox              list the reports waiting to be sent, and exit
--report-url <url>    send to this collector instead of the one compiled in (https only)
--report-token-file <path>  read the report token from here
--report-ca <path>    trust this root certificate (PEM) instead of the one compiled in
--report-pin <hex>    SHA-256 of that root's SubjectPublicKeyInfo. Required with --report-ca

--check-update        fetch the signed manifest, verify it, and say what it offers
--update              the above, then download and install that version
--self-update         the above, but for the launcher executable itself
--exit-after-update   close the launcher once the startup update work has finished
--update-url <url>    fetch the manifest from here instead, and remember it
--verify-binary       print this build's version and exit 0 — the health gate
```

The exit code is 0 when the requested startup work succeeded and 1 when an update was refused or
failed, so the whole of LA2's acceptance can be scripted. `--exit-after-update` is also what a
replacement launcher inherits after a self-update: its arguments are the old process's minus the flag
that asked for the update, so in the new process there is no update work left and it means "exit at
once".

## Publishing a manifest

[`tools/gen_update_manifest.py`](../../tools/gen_update_manifest.py) builds and signs the file this
launcher reads, from a `dist/` directory `tools/release_package.py` has already filled. It carries
its own key generation, and its `--selftest` is a `lint_repo.py` row.

```powershell
python tools\gen_update_manifest.py --genkey --secret-key mh.key --public-key mh.pub
python tools\gen_update_manifest.py --version 0.1.0 --dist dist --secret-key mh.key `
    --asset-base-url https://github.com/<owner>/<repo>/releases/download/v0.1.0 `
    --launcher-exe target\release\mh_launcher.exe `
    --relay-addr <host>:7100 --relay-key <64 hex>      # LA6; or MH_RELAY_ADDR / MH_RELAY_KEY
```

`--genkey` prints the line to paste into `update::PUBLIC_KEY`. **The secret half never enters this
repository** — it is a repository secret in CI and a password-manager entry otherwise. Wiring that
into the release workflow is dist LA3.

## Not here yet

- **A collector in a release build.** RP1's uploader is here and proven, but a launcher only knows
  where to send if `MH_REPORT_URL` / `MH_REPORT_TOKEN` / `MH_REPORT_CA_PEM` / `MH_REPORT_SPKI_PIN`
  were set when it was compiled (see `.github/workflows/launcher-release.yml`). A build without
  them says so in the Report view instead of offering a button.
- **The real signing key.** `update::PUBLIC_KEY` is the DEVELOPMENT key LA2's acceptance run was
  performed with. dist **LA3** replaces it with the release key in the commit that teaches the
  release workflow to sign, and publishes `manifest.json` to GitHub Pages.
- **A static CRT.** The release binary imports `VCRUNTIME140.dll`, so it needs the VC++
  redistributable a player may not have. Fixing that means `-C target-feature=+crt-static`, which
  lives in a workspace-level `.cargo/config.toml` rather than in this crate — a packaging item.
