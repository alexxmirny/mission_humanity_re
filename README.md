# mh — a re-implemented engine core for *Exterminacja* / *Mission Humanity*

A reverse-engineering and re-implementation project for the 2001 Techland real-time strategy game
released as **Exterminacja** in Poland and **Mission Humanity** in English. It does two things the
retail game cannot do for itself:

- **It restores multiplayer.** The shipped executable has a full lobby that builds, checksums and
  then never transmits a single packet — it has no socket layer at all. This project supplies one:
  a real TCP transport, a session browser, an authenticated link and a lockstep turn engine, so two
  or more machines can actually play a game together.
- **It re-implements the game's deterministic spine in C++** — the strategic simulation step, the
  order pipeline, the AI, the save format, the lockstep engine — and runs that code *inside the
  retail process*, in place of the original bodies, one domain at a time. Rendering, UI and
  platform glue stay the original binary's.

Nothing here is a rewrite of the whole game and nothing here is a stand-alone game. The code ships
as a set of DLLs that a legally-owned installation loads at startup. **Read
[INSTALL.md](INSTALL.md) to build it and run it.**

## What this repository does not contain

The game is **Techland's** and stays Techland's. This repository ships **no game assets and no
retail binaries** — no executable, no resource archives, no sprites, audio, text, maps or missions.
You bring your own copy of the game; everything here is written from scratch or derived from
analysis of it. The patch tooling refuses a binary it does not recognise: every patch site carries
the original bytes it expects, and a mismatch aborts the whole apply rather than corrupting an
executable. Full statement and the third-party components used here:
[THIRD_PARTY.md](THIRD_PARTY.md).

## How it gets into the game

No byte of the game executable is modified on disk. A stand-in `msvfw32.dll` next to the game
exploits the fact that the game statically imports the system video-for-Windows DLL and that the
application directory wins the module search, so the loader chain becomes

```
mh.exe  ->  msvfw32.dll (ours, forwards every real export)  ->  mh.dll  ->  its satellites
```

and `mh.dll` is initialised before the game's own entry point runs. Static patches that used to be
baked into a patched executable — raised array caps, relocated pools, the CD check — are compiled
into `mh.dll` and applied to the *loaded image* instead, guarded site by site
([docs/inmem-patching.md](docs/inmem-patching.md)).

`mh.dll` then loads its satellites from beside itself. **Which files are present is the
configuration** — there is no configuration key that selects a DLL, and `mh.dll` is byte-identical
in every configuration. The mechanism, including what each absence means and how it is reported:
[docs/dll-split.md](docs/dll-split.md).

## The three supported configurations

| | `mh.dll` | `mh_net.dll` | `mh_harness.dll` | `libmh.dll` | game binary |
| --- | --- | --- | --- | --- | --- |
| **(1) all-original + net restoration** | yes | yes | optional | **absent** | yes |
| **(2) brokered** | yes | yes | optional | `Release\libmh.dll` | yes |
| **(3) standalone reference** | — | — | — | `Release\standalone\libmh.dll` | **none** |

**(1) All-original + net restoration.** The game runs its own simulation, exactly as it shipped;
this project adds only the parts it never had. `mh.dll` is the router and patch host, `mh_net.dll`
is the transport. Nothing is promoted and nothing is instrumented, and a player cannot tell the
difference from retail except that multiplayer works. It is selected by **not** having `libmh.dll`
in the folder, and the boot log says so by name.

**(2) Brokered.** The same files, plus `libmh.dll` — the re-implemented spine, built to run
*hosted*: it reaches back into the original executable for the C runtime at fixed addresses. When it
is present, `mh.dll` routes whole domains to it rather than to the original bodies, at domain
granularity. This is what ships by default. The `[config] mode` key in `mh_net.ini` chooses between
`brokered` (the default) and `original` — the latter runs the game's own bodies even with
`libmh.dll` present, and is the supported, tested rollback.

**(3) Standalone reference.** `Release\standalone\` holds a **different** `libmh.dll` — the same
source compiled with a vendored C runtime and no address into any game binary — plus
`libref_host.exe`, which drives it. There is no `mh.exe` in the process at all: the host loads a
committed fixture of simulation state and replays it for a fixed number of steps, comparing every
step's hash. It is how the spine is verified on a machine with no game copy, and it is the offline
half of continuous integration. The two `libmh.dll` files share a name and cannot do each other's
job; keep them in their own folders, and note that `mh.dll` refuses the wrong one by name rather
than running it.

## Repository layout

| | |
| --- | --- |
| `src/mh_dll/` | the MSBuild solution: the four shipping DLLs, the standalone host, the offline test binary, the asset tools ([src/README.md](src/README.md)) |
| `src/mh_net_proto/` | the wire protocol and its cryptography, as a static library with its own tests |
| `src/mh_shim/` | the `msvfw32` proxy that force-loads `mh.dll` |
| `src/patcher/` | the declarative binary-patch pipeline and its manifests |
| `src/formats/` | resource pack/unpack/convert tooling for the game's data files |
| `tools/` | the Python automation: build gates, lints, the replay and test harnesses |
| `docs/` | the design and reference documents listed below |
| `.github/workflows/ci.yml` | the offline gate — build, selftests, standalone replay, lint |

## Documentation

| | |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | **start here** — how the game itself runs: subsystems, the two game modes, the simulation loop, the order pipeline, state |
| [docs/dll-split.md](docs/dll-split.md) | how `mh.dll` loads a satellite, what each absence means, and the measured boot shapes for all three configurations |
| [docs/inmem-patching.md](docs/inmem-patching.md) | applying a static patch manifest to the loaded image instead of to a file |
| [docs/libmh-abi.md](docs/libmh-abi.md) | the boundary between the re-implemented spine and its host |
| [docs/libmh-sim-abi.md](docs/libmh-sim-abi.md) | the simulation half of that boundary |
| [docs/state-boundary.md](docs/state-boundary.md) | who owns which piece of game state, and how a re-implemented module reaches it |
| [docs/save-format.md](docs/save-format.md) | the save-game format and the block contract |
| [BNK_FORMAT.md](BNK_FORMAT.md) | the sprite-bank format (contributed — see [THIRD_PARTY.md](THIRD_PARTY.md)) |
| [docs/release.md](docs/release.md) | how a release is cut: the tag convention, the three drop-in zips, and how the version stamp reaches the binaries |

## License

MIT, on this project's own code and documentation — [LICENSE](LICENSE). Third-party components and
the retail-game boundary: [THIRD_PARTY.md](THIRD_PARTY.md).
