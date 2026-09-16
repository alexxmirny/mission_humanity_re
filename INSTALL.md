# INSTALL and RUN

How to build this project from a clean clone and run each of its three configurations. Read
[README.md](README.md) first for what the configurations *are*; this file is the mechanics.

Everything here is Windows. The game is a 32-bit x86 executable and so is everything that loads
into its process.

- [1. Prerequisites](#1-prerequisites)
- [2. Build](#2-build)
- [3. Verify offline — no game copy needed](#3-verify-offline--no-game-copy-needed)
- [4. Run configuration (1) — all-original + net restoration](#4-run-configuration-1--all-original--net-restoration)
- [5. Run configuration (2) — brokered](#5-run-configuration-2--brokered)
- [6. Run configuration (3) — standalone reference](#6-run-configuration-3--standalone-reference)
- [7. Multiplayer](#7-multiplayer)
- [8. What is bring-your-own-game](#8-what-is-bring-your-own-game)

## 1. Prerequisites

| | what | why |
| --- | --- | --- |
| **Visual Studio 2022** Build Tools or higher | the **Desktop development with C++** workload, including the **x86** toolset (v143) and the **C++ Clang tools for Windows** component | `mh.sln` builds `Release` for `x86`; the lint checks formatting with that exact `clang-format` |
| **Python 3.11** | plus `python -m pip install -r tools/requirements.txt` (12 pinned distributions) | every gate, lint and harness in `tools/` |
| **a legally obtained copy of the game** | *Exterminacja* / *Mission Humanity*, installed normally | only for §4-§7. §2 and §3 do not need it |

Python 3.11 is what the pins in `tools/requirements.txt` were taken from and what CI uses; the
environment check accepts 3.9 or newer. Newer minors generally work, but an unpinned `ruff` will
disagree with the lint about formatting, so install from the requirements file rather than by name.

Check the machine before building:

```powershell
python tools\bootstrap.py --check
```

It prints one line per requirement with a **remedy** for each failure, grouped by tier. On a machine
that will only build and test — no reverse-engineering, no game — use `--ci` instead: the analysis,
game and rig groups then report `ABSENT` by name rather than failing, and only the tiers the build
and the tests actually need stay as gates.

**Per-machine paths.** `tools/machine_config.py` is the single place that knows machine-specific
locations (the Visual Studio root, the game install, rig addresses). Do **not** edit it — override
it either with a gitignored `tools/machine.local.json` holding `{"NAME": "value"}`, or with
`MH_<NAME>` environment variables, which win. `python tools\machine_config.py` prints every resolved
value and where it came from. In practice a fresh clone needs at most one override,
`MH_VS_INSTALL_ROOT`, and §2 derives even that.

## 2. Build

One MSBuild invocation produces every artifact of all three configurations. There is no
per-configuration build.

```powershell
$repo = git rev-parse --show-toplevel
# DERIVE the Visual Studio root, never paste it: a wrong MSBuild path fails silently the moment the
# command is piped, and you get a "successful" build that never ran.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
# CARRY the derived root forward: the Python tooling (lint_repo's clang-format row, in §3) resolves
# the VS root independently, and its committed default is one specific edition's path. Without this
# line, §3 fails with [WinError 2] on any other edition/location (Build Tools included).
$env:MH_VS_INSTALL_ROOT = $vs
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\src\mh_dll\mh.sln" `
  /t:Build /p:Configuration=Release /p:Platform=x86 /m /nodeReuse:false /v:minimal /nologo
```

For a permanent setting (surviving the shell), put it in the gitignored `tools\machine.local.json`
instead: `{"VS_INSTALL_ROOT": "C:\\...\\your VS or BuildTools root"}`.

`/nodeReuse:false` is part of the recipe, not a nicety: with node reuse on, MSBuild's worker
processes outlive the build holding the stdout handle they were started with, so any caller that
captures output waits forever for an EOF that never arrives.

A full build is a few minutes cold. It must produce **seven** artifacts under
`src\mh_dll\Release\`:

| artifact | configuration | role |
| --- | --- | --- |
| `mh.dll` | (1), (2) | the router and patch host — the only module that touches the game's addresses |
| `mh_net.dll` | (1), (2) | the multiplayer transport |
| `mh_harness.dll` | (1), (2) | the determinism/replay instrument (optional; ships disarmed) |
| `libmh.dll` | (2) | the re-implemented spine, **hosted** build |
| `standalone\libmh.dll` | (3) | the re-implemented spine, **standalone** build (vendored C runtime) |
| `standalone\libref_host.exe` | (3) | the host that drives the standalone spine |
| `net_selftest.exe` | — | the offline test suites |

Two more are built beside them and are used by §4-§7 rather than being a configuration of their
own: `msvfw32.dll` (the loader shim) and `mh_tools.exe` (asset tooling).

A green build line is not proof the set is complete — check the seven by name. The same list, with
sizes, is printed by `python tools\run_gate.py` and asserted by
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

**The two `libmh.dll` files have the same name and are different binaries.** `Release\libmh.dll`
calls the game executable's own C runtime at fixed addresses and cannot run in a process without
it; `Release\standalone\libmh.dll` carries a vendored runtime and reaches no game address at all.
Keep each in its own directory. If the standalone one is deployed as the hosted one, `mh.dll`
refuses it by name and the game quietly runs configuration (1) instead.

## 3. Verify offline — no game copy needed

These four steps are the whole offline gate, and they are exactly what CI runs
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)). None of them touches a game install, a
virtual machine or a network.

```powershell
python tools\bootstrap.py --ci        # environment, with the game/analysis tiers expected-absent
python tools\run_selftests.py         # the offline suites, under AddressSanitizer and then plain
python tools\replay_libref.py         # configuration (3): the committed fixtures, replayed
python tools\lint_repo.py --ci        # formatting, generated-file drift, and the contract checks
```

**`run_selftests.py` is the gate, not a bare `net_selftest.exe <suite>` run.** A single
un-instrumented pass reporting "0 failures" is not evidence that the run did not silently corrupt
heap memory; the ASan pass turns that into a deterministic stack trace naming the file and line on
the first run. `--no-asan` is for the fast iterate loop only.

**`replay_libref.py`** unpacks each committed fixture in `tools/data/fixtures/` and replays it
through `standalone\libref_host.exe`, comparing a hash per simulated step. Fixtures are discovered,
never listed, so a new one cannot be added and silently not gated. It also reads
`libref_host.exe`'s import table and fails if `libmh.dll` is not in it — a build that quietly went
back to static linking would otherwise replay exactly as green. If the fixture directory is absent
it **refuses with exit 2 and a named reason** rather than a traceback.

**`lint_repo.py --ci`** runs the same rows as `lint_repo.py` minus a declared table of skips, each
printed by name with its reason, and a total/run/skipped line at the end. The skips exist because
some rows' subject is a directory that a public clone does not have, or a retail game copy. A row
in the skip table that no longer matches a real row is itself a failure, so the table cannot rot
into a silent exclusion.

What a green offline run does **not** prove: anything that needs the game to boot. That is §8.

## 4. Run configuration (1) — all-original + net restoration

Install the game normally, then copy these files from `src\mh_dll\Release\` into the game's
installation folder, **next to the game executable**:

| file | |
| --- | --- |
| `msvfw32.dll` | the loader shim — this is what makes the game load `mh.dll` at all |
| `mh.dll` | the router and patch host |
| `mh_net.dll` | the multiplayer transport |
| `mh_harness.dll` | *optional*; without it the run is simply uninstrumented |

**Do not copy `libmh.dll`.** Its absence *is* configuration (1).

Then launch the game the way you normally would. Nothing else is required: the DLL arms with no
configuration file present and every key carries a shipping default.

**Confirm which configuration you got.** Each run creates `logs\<runid>_<role>\` inside the game
folder. Open `mh_net.log` there; the first lines report each satellite's bind outcome by name:

```
; [modules] mh_net: BOUND at DllMain (under the loader lock) -- 23 exports resolved, ... call-through ok
; [modules] libmh: NOT BOUND -- LoadLibrary(...\libmh.dll) failed, Win32 error 126 (the file is not
    there). This run is CONFIGURATION (1): mh.dll does not contain the spine, so the game runs the
    original binary's own bodies. Nothing is promoted and nothing is instrumented; that is the
    configuration, not a failure of the boot.
; [modules] mh_harness: BOUND at DllMain ... host=27/27 spine=0/30
```

Five outcomes are distinguished on purpose — `BOUND`, `NOT BOUND` (the file is missing),
`NOT ATTEMPTED` (the run was told not to look), `LOADED BUT REFUSED` (present but not the right
module) and `ABI MISMATCH` — so "the module is not here" and "this run declined it" never read as
the same fact. Details: [docs/dll-split.md](docs/dll-split.md).

**To uninstall**, delete `msvfw32.dll` from the game folder. The game then loads the system DLL
again and runs unmodified, whatever else is left lying beside it.

**Optional: the configuration file.** Copy `src\mh_dll\mh_net.example.ini` next to the executable as
**`mh_net.ini`** and uncomment what you need. It documents every key the DLL reads, each with its
real default, so an all-commented copy behaves exactly like no file. Two rules that bite: a section
you omit entirely disables that feature, and a second block with the same section name is dead text
(only the first is read). Note the file is named `mh_net.ini` even though it configures all of it —
a leftover `mh_harness.ini` from an older layout is **refused**, naming itself in
`mh_config_refused.log`, rather than ignored.

**Optional: the static patches.** The manifests that raise the game's hardcoded array caps are
compiled into `mh.dll` and applied to the loaded image when asked. In `mh_net.ini`:

```ini
[patch]
inmem=1
manifest=grand_all_caphike_storagecap_EN
```

`manifest` has no default on purpose — the compiled-in manifests are alternatives rather than a
batch, so with more than one available and the key empty the applier refuses and lists them in
`mh_net.log`. The apply is all-or-nothing: a site whose expected original bytes have moved leaves
the image untouched and says which. See [docs/inmem-patching.md](docs/inmem-patching.md).

## 5. Run configuration (2) — brokered

Everything from §4, **plus** `src\mh_dll\Release\libmh.dll` — the hosted build, from `Release\`
itself and not from `Release\standalone\`.

That is the entire difference. `mh.dll` is byte-for-byte the same file in both configurations; the
configuration is which files sit beside it.

In `mh_net.ini`, `[config] mode` decides what to do with the spine that is now present:

```ini
[config]
mode=brokered     ; brokered (default) | original
```

- `brokered` — the re-implemented spine serves the domains it owns. This is the default and what
  ships.
- `original` — the game's own bodies run even though `libmh.dll` is loaded. Every promotion default
  becomes 0. This is the supported and tested rollback, and it announces itself in the log; the
  brokered default deliberately prints nothing.
- `standalone` is **not** selectable here. It is a property of the build, not of a run, so a hosted
  process cannot claim it. An unrecognised value falls back to `brokered` and says so by name.

**Confirm the spine is actually being used**, which a successful bind does not tell you: at the end
of the arm, `mh_net.log` reports how many times control crossed into it.

```
; [libmh] crossings=13465 absent-calls=0 -- the spine boundary was ENTERED in this run (configuration (2))
; [libmh] crossings=0 absent-calls=796 -- the spine boundary was NOT ENTERED in this run (configuration (1))
```

A brokered run that binds `libmh.dll` and never calls through it would otherwise be
indistinguishable from one that crossed thousands of times.

## 6. Run configuration (3) — standalone reference

No game, no game folder, no installation step. The artifacts are already in place after §2:

```
src\mh_dll\Release\standalone\
    libmh.dll          the spine, standalone build
    libref_host.exe    the host
```

Run every committed fixture:

```powershell
python tools\replay_libref.py
```

Expect `PASS (3 fixture(s))`, each having run its declared 5000 steps; the per-fixture log under
`tmp\libref\` includes `state-hash mismatches: 0 (ALL STEPS IDENTICAL)` near the end. One fixture
at a time:

```powershell
python tools\replay_libref.py --fixture libref-replay-v1
```

A fixture is a packed, hash-checked snapshot of simulation state plus the per-step hashes the
original produced. The host loads it, runs the standalone spine for the fixture's declared number of
steps, and compares. A run that stopped early is a failure rather than a pass over however many
steps it managed.

The fixtures request eight scripted base-layout assets that the retail resource bank does not
actually contain. That is declared in `tools/data/libref_asset_dispositions.json` — names only, no
game data — and the host is told about it with `--assets-absent`, so a miss for one of those eight
is a documented answer while any *other* asset request still trips the fail-closed trap. On a
machine that does have the game, the gate re-verifies the declaration against the real bank and
refuses if a declared-absent name turns out to be present. Note which direction is checkable: one
cannot prove a file is absent from a bank one does not have, but anyone with the bank can prove the
declaration lied.

## 7. Multiplayer

Configurations (1) and (2) both restore multiplayer; `mh_net.dll` must be present. There is no
role configuration — the host clicks Create, a joiner clicks Join and types the host's address.

**The link is authenticated by a file, not by a setting.** `mh_key.txt` appears next to the game
executable on first run. The host shares that file with its players; every peer in a game must hold
the same key, and a peer with the wrong one is refused rather than desynchronised.

## 8. What is bring-your-own-game

The full correctness gate for this project is larger than §3 and its remainder cannot run without a
retail copy. The parts that are **not** offline:

- the **UI regression suite** — registered menu, lobby and HUD walks, compared against recorded
  frames. The recorded baseline frames are rendered from retail assets and are therefore not
  published; the suite regenerates its own baselines on a machine that has the game.
- the **two-peer determinism run** — both peers driven through the real menu into a lockstep game,
  then every step's hash compared. It needs two machines and the game.
- the **in-memory patch parity oracle** — it compares what the in-DLL applier wrote into a live
  image against what the file-based patcher produces from the same manifest, so it needs a clean
  game copy to patch.
- anything else that boots the executable.

Those rows are named individually in `lint_repo.py --ci`'s skip table and in `bootstrap.py --ci`'s
expected-absent tiers, so what a green offline run does not cover is enumerable rather than
implied. On a machine that does have the game, point `MH_POLYGON` (or a
`tools/machine.local.json` entry) at the installation and run `python tools\run_gate.py` for the
whole thing.
