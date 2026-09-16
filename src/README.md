# src/ — the software

Everything that gets built or shipped. Five trees: the DLL solution, the wire protocol, the loader
shim, the binary patcher, and the data-format pipeline.

Start at [/README.md](../README.md) for what the project is and at [/INSTALL.md](../INSTALL.md) for
how to build and run it. This file is the map of the source.

## `mh_dll/` — the MSBuild solution (`mh.sln`, `Release|Win32`, v143, C++20)

One build produces every artifact of all three configurations; the configuration is which files a
deployment has beside `mh.dll`, never a build switch. Twelve projects:

| Project | Output | Role |
| --- | --- | --- |
| `mh/` | `Release\mh.dll` | **the router and patch host** — every fixed-address touch of the game binary lives here and nowhere else. Detours, byte patches, the in-memory static-patch applier, the UI/lobby fixups, the hook registry, the satellite binds |
| `mh_net/` | `Release\mh_net.dll` | the multiplayer transport: sockets, framing, the pre-shared key, the link watchdog |
| `libmh_dll/` | `Release\libmh.dll` | **the re-implemented spine**, HOSTED build — it reaches the game executable's own C runtime at fixed addresses, so it only runs inside that process |
| `libmh/` | `libmh.lib` | the same sources as a static archive, built STANDALONE (vendored C runtime, no game address on any live path) |
| `libmh_std/` | `Release\standalone\libmh.dll` | that archive linked whole into a DLL — configuration (3)'s spine. It compiles no source of its own, so the DLL and the archive are the same object code by construction |
| `libref_host/` | `Release\standalone\libref_host.exe` | the host that drives the standalone spine over a committed fixture. No game binary in the process |
| `mh_harness/` | `Release\mh_harness.dll` | the determinism and replay instrument. It only ever observes, and it ships disarmed |
| `mh_common/` | static lib | game-independent utilities: sprite-bank and BMP I/O, geometry, the run-context paths, and the game's LZW codec in both directions plus its LZSS sibling |
| `../mh_net_proto/` | static lib | the control-frame protocol and its cryptography (see below) |
| `mh_nettest/` | `Release\net_selftest.exe` | the offline suites — the oracle for everything that can be tested without the game |
| `mh_tools/` | `Release\mh_tools.exe` | host-side sprite-bank tooling: `unpack` every sprite to editable bitmaps, `pack` them back, `verify` the round trip |
| `../mh_shim/msvfw32/` | `Release\msvfw32.dll` | the loader shim (see below) |

The DLL's own reference — the layout of `mh/`, the generated address/struct/call headers and their
drift gates, the rules that keep it maintainable, and **the gate** (the full build-and-test
procedure) — is [mh_dll/README.md](mh_dll/README.md). The whole configuration surface, one key at a
time with its real default, is [mh_dll/mh_net.example.ini](mh_dll/mh_net.example.ini).

Three properties of the split are worth knowing before reading any of it, and all three are
documented in [/docs/dll-split.md](../docs/dll-split.md):

- **`mh.dll` loads its satellites by absolute path from beside itself**, at
  `DLL_PROCESS_ATTACH`, and continues when one is missing. A satellite's own `DllMain` must touch
  nothing outside its own module, and its static imports must be a subset of `mh.dll`'s — both are
  enforced by a gate, not by a paragraph.
- **The export contracts are DERIVED from the built objects**, not hand-written: the row lists are
  an intersection of what one image leaves undefined and what the other defines, so they cannot
  drift from either side.
- **Absence means something different for each satellite** — a degradation for the transport, a
  supported configuration for the spine, an uninstrumented run for the harness — and each says which
  in the boot log.

## `mh_net_proto/` — the wire protocol

The session/join control-frame formats, the routing frame header, and the cryptography: SHA-256,
HMAC, ChaCha20, the pre-shared-key handshake and the encrypted record layer. **Pure logic, no
platform**: no sockets and no Windows headers, so the same sources build for a headless relay as
for the injected DLL. It carries its own `CMakeLists.txt` for that reason. The cryptography is
hand-written, so its unit tests pin the published RFC vectors and are not optional.
[mh_net_proto/README.md](mh_net_proto/README.md).

## `mh_shim/msvfw32/` — how the DLL gets loaded

A stand-in `msvfw32.dll` that sits next to the game executable. The game statically imports the
system video-for-Windows DLL, `msvfw32` is not a KnownDLL, and the application directory wins the
module search — so the loader maps this one, which statically imports `mh.dll` and thereby runs
`mh.dll`'s `DllMain` before the game's entry point. Every real export is forwarded to the genuine
system DLL, loaded by absolute path on the first forwarded call. Nothing happens in this module's
`DllMain`. Deleting the file restores the stock game.

## `patcher/` — the declarative binary patcher

`mhpatch.py`: every patch is described as address + assembly-or-bytes + the **expected original
bytes**, so it is transparent, guarded and reversible. A site whose expected bytes do not match is
rejected. Built on Keystone (x86-32 assembly, fed the site address so branches resolve) and LIEF
(PE parsing, section-to-offset mapping, import resolution). The `*.mh.patch.json` manifests beside
it are the patch set: raised array caps, relocated pools, and the small behavioural fixes.

These manifests are also compiled into `mh.dll` and applied to the **loaded image** instead of to a
file, which is how the project stopped distributing a patched executable —
[/docs/inmem-patching.md](../docs/inmem-patching.md). `mhpatch` remains the reference
implementation and the parity oracle the in-memory applier is checked against.
[patcher/README.md](patcher/README.md).

## `formats/` — the data-format pipeline

Python tooling for the game's own data files, run entirely outside the game: `unpack.py` / `pack.py`
for the resource archive, `decompress.py` (and a C++ twin) for its compression, `convert_gfx.py`,
`bnk_names.py` (annotates an unpacked sprite tree with the names the game's config assigns, plus
per-bank contact sheets), `build_mod.py` (the one-click rebuild: edited sprites to banks, staged
over a pristine base, archive rebuilt), and the `cfgkit` / `mapkit` packages for the config grammar
and the map format.

The sprite-bank format itself is documented in [/BNK_FORMAT.md](../BNK_FORMAT.md), which was
contributed rather than written here — see [/THIRD_PARTY.md](../THIRD_PARTY.md).

## Third-party code in this tree

`mh_dll/include/` holds three vendored single-header libraries — `CLI11.hpp`, `stb_image.h` and
`BitmapPlusPlus.hpp`. They are not our work and keep their own licenses; notices are in
[/THIRD_PARTY.md](../THIRD_PARTY.md).
