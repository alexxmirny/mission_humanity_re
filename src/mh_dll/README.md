# src/mh_dll — the injected DLL for `mh.exe` (EN)

The Visual Studio solution behind `mh.dll`: the injected layer that restores multiplayer
(TCP transport + discovery/lobby/lockstep seams), hosts the enlarged game pools, and carries the
determinism/replay harness and launch verbs. Targets the **English build `/eng/mh.exe` only**
(EN-only since the 2026-07 refactor; RU support exists only in pre-refactor git history).
Plan and history for that refactor are in the git log.

## Targets (`mh.sln`, Release|Win32, v143, C++20)

| Project | Kind | Role |
| --- | --- | --- |
| `mh/` | DLL | **ALL binary-bound code** — every fixed-VA touch of mh.exe lives here and nowhere else. Output `Release\mh.dll` (+ a PostBuild copy to `$(MhDeployDir)` — set it via an environment variable or `/p:MhDeployDir=...`; the copy is guarded on the directory existing, so a checkout without one builds fine and deploys nothing). |
| `mh_common/` | static lib | Game-binary-independent utilities: `bnk`/`bmp_io`/`misc` (geometry), `run_context`, `net_transport` (Winsock TCP star — no game VAs), `net_key` (the `mh_key.txt` pre-shared key), and `lzw` — the game's LZW codec **both directions** plus its LZSS sibling (SV1 batch B; the encoder is a reimplementation, verified byte-identical against 501 real save blocks). Pure buffer logic, no game VA: the save-format framing that calls it lives in `libmh/save/save_block.*`. |
| `../mh_net_proto/` | static lib | The SESSION_INFO/JOIN control-frame protocol **and `net_crypto`** — SHA-256/HMAC/ChaCha20, the PSK handshake and the encrypted record layer (own CMake too, for the Linux relay). Hand-written crypto, so its unit tests pin the published RFC vectors: `net_proto_test` is not optional. |
| `mh_tools/` | console exe | Host-side asset/pool tooling. |
| `mh_nettest/` | console exe | `net_selftest.exe` — transport loopback tests (`selftest`, `selftest3`). Compiles the real transport+seam sources directly (no game, no DllMain). `build_selftest.bat` = thin msbuild wrapper (exe → `%TEMP%\mh_nettest`). |

## Layout of `mh/` and `libmh/` (relocated 2026-09-16, fork F5O)

**Read this before any path in the rest of this section.** Until F5O the 628-TU shared roster sat
under `mh/<domain>/` even though three projects compiled it. It now lives in the directory of the
library that owns it, domain subdirectories intact:

- **`libmh/{ai,lockstep,orders,save,sim,state,tact}/`** — the SHARED ROSTER, compiled by `libmh`
  (the archive), `libmh_dll` (the hosted arm) and `mh_nettest` (the offline oracle). This is
  **shared location 1 of 2**, and it is explicitly so: a library's second CRT arm and its tests
  compiling the library's own sources from the library's own directory is the intended reading of
  "one directory per target".
- **`mh/addr/mh_calls.gen.cpp`** — **shared location 2 of 2**: ruling Q5's PER-IMAGE shim, compiled
  by six projects (`libmh_dll`, `libmh_std`, `libref_host`, `mh`, `mh_harness`, `mh_nettest`),
  because whichever image compiles the roster must supply the `mh::call::detail::s_*` shapes.
- **`mh/`** — mh.dll's own code (`seams/`, `hook/`, `ui/`, `desync/`, `patch/`) plus the
  header-only trees `addr/`, `crt/`, `fp/`, `config/`, `fix/`, `include/`.
- `mh_harness/harness.cpp` and `mh_nettest/module_bind_compiled_in.cpp` each moved into the one
  project that owns them.

**The include rule (why no `#include` line moved).** Every project that compiles or includes the
roster carries **both** `..\libmh` and `..\mh` on `AdditionalIncludeDirectories` — `libmh`,
`libmh_dll`, `libmh_std`, `libref_host`, `mh`, `mh_harness`, `mh_nettest`. A roster TU's
`#include "sim/sim_state.h"` resolves under `libmh/` and its `#include "addr/mh_calls.gen.h"` under
`mh/`, so all 1260 files moved with zero source edits. Rationale and the tooling consequences:
[docs/dll-split.md](../../docs/dll-split.md) § "F5O".

Bullets below say `sim/`, `orders/` and so on for the module; the directory is `libmh/<that>`.

- `addr/mh_addrs.gen.h` — **GENERATED** EN VAs (`tools/gen_dll_addrs.py` from
  `tools/data/dll_addr_manifest.json`). Never hand-edit, never hand-type hex VAs in code; add the
  Ghidra symbol to the manifest and regenerate. `--check` is the drift gate (in `tools/lint_repo.py`).
- `addr/mh_structs.gen.h` — **GENERATED** C++ mirrors of game structs the DLL dereferences (so seam code
  uses `widget->x` / `ev->event_type`, not hand-coded offsets). Pipeline: name the structs in the manifest
  `structs` list → `mh_dump_struct_layouts.py` (Ghidra, EN) writes `tools/data/dll_struct_layouts.json` →
  `tools/gen_dll_structs.py` emits the header with a `static_assert` on every size/offset (a Ghidra retype
  that shifts a field breaks the BUILD). Never hand-edit; `--check` is the drift gate in `lint_repo.py`.
  Types live in `namespace mh::game` as `mh_<ghidra_name>`.
- `addr/mh_calls.gen.{h,cpp}` — **GENERATED** typed C++ callables for the game's own functions, so
  calling into original code is `mh::call::llm_strat_unit_order_move(side, unit, x, y, seq)` rather
  than hand-written register marshalling. Pipeline: `mh_dump_call_protos.py` (Ghidra, EN) writes
  `tools/data/dll_call_protos.json` → `tools/gen_dll_calls.py` emits an inline wrapper per function
  plus one naked marshalling thunk per distinct **storage shape** (82 of them for 1258 functions).
  `--check` is the drift gate in `lint_repo.py`. Three things to know:
  - **The predicate is committed STORAGE, not the convention name.** Slots come from Ghidra's real
    per-parameter storage, so the generator never assumes the `__watcall` register order — which is
    just as well, since a handful of functions genuinely pass EAX/EBX/EDX.
  - **A function that isn't callable is loud, not absent.** It's declared returning an incomplete
    type named for the reason, so calling it fails to compile with e.g. *"use of undefined type
    `MH_UNAVAILABLE__return_type_never_determined`"*. Fix by committing the prototype in Ghidra and
    re-dumping — never by hand-editing the header. Prototyping is demand-driven: let the call site
    you actually need decide what gets an RE pass.
  - **The thunks are verified, not assumed** — `net_selftest.exe callstest` (generated alongside
    them, `mh_nettest/mh_calls_selftest.gen.cpp`) calls every shape against a synthetic Watcom callee
    that records what it really received, under both cleanup disciplines. It is part of the gate.
    But note *what that proves*: the stubs come from the same slot table as the thunks, so it shows
    the code matches the MODEL, not that the model matches the binary. The independent check is the
    callee's own **`RET imm`**, which the dumper reads and cross-checks (`Σ round_up(slot,4)` for
    callee-cleanup, `0` for caller-cleanup); a disagreement refuses the function. The trap this
    exists for: 872 green checks coexisted with ten functions
    emitted as zero-arg callables that end `RET 8`.
- `addr/mh_export.gen.h` — **GENERATED** the inverse: install a plain C++ function AS a game
  function (`tools/gen_dll_exports.py`, sharing the shape table by importing `gen_dll_calls`).
  `MH_EXPORT_REPLACE(llm_some_fn, my_body)` defines a naked entry thunk that unmarshals the Watcom
  registers/stack into a cdecl call of `my_body`, plus `mh_export_install_llm_some_fn()`. The
  binding is type-checked against the generated signature, so a wrong parameter list is a compile
  error. Two things differ from the call direction:
  - **Cleanup cannot be finessed.** Here we ARE the callee and our callers are already-compiled
    game code, so `ret N` vs plain `ret` is baked in from the committed convention.
  - **The arm guard is per-function expected BYTES**, not `WATCOM_PROLOGUE` (`hook/export.h`).
    Installing rewrites 8 bytes, the generator knows which 8 it compiled against, and plenty of
    worthwhile leaf targets have no frame prologue at all (`w_strlen` opens `PUSH EDX; MOV EDX,EAX`).
    Functions shorter than 8 bytes are refused outright — the detour would run past their end.
    Verified by `net_selftest.exe exportstest`.
- `addr/mh_regions.gen.h` — **GENERATED** state region **registry** (`tools/gen_state_registry.py`
  from `tools/data/state_regions.json`; RI-STATE / ST1). One derivation of where each state region
  lives, merged from the five manifests that used to answer that independently — the module state
  views, the shadow region sets, the save block table, the patcher's relocation sites and
  `harness.cpp`'s determinism hash. `mh::state::ptr<T>(RID_…)` is how a module binds its state;
  `covering(addr, size)` is how the save driver checks a block.
  - **145 of 243 entries carry a `static_assert` pinning them to `mh_addrs.gen.h`**, so a VA that
    reaches one header and not the other is a compile error. That is the check whose absence let
    `SavePlanetToDisk`'s first block *be* the order queue with nothing connecting it to `mh::orders`.
  - Coverage is checked **twice, for different failures**: `save_driver.cpp` `static_assert`s that
    every block in the generated save table is covered (catches a hand-edit of either header), and
    `gen_state_registry.py --check` re-checks against the **committed** registry (the arm that can
    actually go red on a data change — a `--refresh` merges the save table in and could never
    disagree with itself). `--selftest` is the checked-in demonstration that the second one fires.
  - Bases are **`constexpr`**: ST1 changes where an address comes from, not what it is. Runtime-
    resolved bases so a relocated build reports its real one are ST2; per-owner serializers are ST4.
- **the differential shadow oracle -- REMOVED at F2D (2026-09-12).** `addr/mh_shadow.gen.h`,
  `shadow/shadow.{h,cpp}`, `tools/gen_dll_shadow.py`, 1480 `MH_SHADOW_REPLACE` sites and 475 <!-- CITATION-OK -->
  `shadow_arm` wrappers are gone. It installed a dispatcher that, on every call, ran the ORIGINAL
  through a trampoline, snapshotted, re-ran our body on the restored pre-state, compared the regions
  the function was *measured* to write, then handed the game back the original's effects -- so a
  wrong reimplementation was reported, never applied. It did its job: the translations it was built
  to prove are proven and promoted. The correctness instruments now are the config-level oracles
  (`test_ui.py --sp-determinism`, the recorded-session A/B/C arms, the determinism gate) plus
  the offline suites.
  Its two committed inputs went with it; `tools/data/state_regions_measured.json` freezes the region
  claims `gen_state_registry.py` still consumes (see that file's note). History: git.

- `orders/` — **the first reimplementation module** (RI-ORDERS / O2), and the shape every later one
  should copy. `mh::orders::enqueue(...)`, not `reimpl_order_enqueue` — the mapping back to the
  original lives in the seam binding, which is the one place it belongs. Three rules it exists to
  demonstrate:
  - **No hand-typed addresses.** `state()` resolves the container's globals once from
    `mh_addrs.gen.h`, so `gen_dll_addrs.py` verifies every one against `docs/symbols.md` and a Ghidra
    rename becomes a build failure rather than a pointer into the wrong array.
  - **No byte offsets.** The record is `mh::game::mh_llm_strat_order` from `mh_structs.gen.h`, so
    `rec.unit_index = …` compiles into the same store as `*(uint16_t*)(rec+0x08)` *and* carries a
    `static_assert` that the offset still matches Ghidra.
  - **The module owns its own verification wiring** — its seam bindings and the promotion installer
    live with the code they check, not in a seam. (Its differential-oracle bindings did too, until
    F2D retired that oracle.)
- `sim/` — **the strategic sim** (RI-SIM), 307 functions, and the one module whose state interface is
  NOT `ai/`'s with the names changed. `ai_state.h` gets its guarantee from `const`, which works
  because the AI writes almost nothing; the sim is the rosters' single legitimate writer (138 of the
  307 write a SHARED region, `units` alone has 47 writers), so const-ness there would reject the
  subsystem's purpose. `sim/sim_state.h` states three rules instead — **W1** the read view is const
  and its *membership* is the claim (a region reachable only through `sim_view` is one the sim was
  measured never to write: the cfg tables, the map extents); **W2** the write store's pointers are
  private and every write is a typed accessor returning ONE record, so no address escapes to go stale
  when ST2 moves a roster; **W3** the store's constructor is private with two friends, `state()` and
  the offline fixture.
  - Proven, not asserted: `mh_nettest/sim_write_negative.cpp` + `tools/check_const_view.py` (5 cases,
    **per-case** expected diagnostic — W1 → the const family, W2/W3 → C2248 — plus a positive arm),
    `net_selftest.exe statetest` consumer 5 (rebase `RID_UNITS`, the write half checked *through the
    accessor* because there is no pointer to compare), and `net_selftest.exe simtest`.
  - `tools/check_sim_addresses.py` (in `lint_repo.py`) enforces the half the compiler cannot: **only
    `sim/sim_state.cpp` may name `mh::state::ptr` or a game data address.** A TU that resolved a
    region itself would compile, run and pass everything else.
  - **Every `libmh/sim/` TU is `/arch:IA32 /fp:precise` in every image that compiles it.** The sim
    is x87; a TU built SSE2 in the offline oracle would verify a different function from the one the
    game runs. `libmh.vcxproj` and `libmh_dll.vcxproj` apply the two settings BLANKET (a project-wide
    setting cannot rot the way a hand-list does); `mh_nettest/mh_nettest.vcxproj` still flags them
    per file, so **add a new sim or AI TU to that list**. `mh.vcxproj` has carried no roster TU since
    F4D and none since F5O has one in its directory, so it has nothing left to flag.
    `tools/lint_fp_flags.py` gates this and now refuses a run that classifies implausibly few TUs —
    the move broke its include-path normaliser, and without the floor it would have reported
    "0 flagged, 0 exempt, 0 MISSING" and exited green over an empty scan.
- `seams/reimpl_probe.cpp` — the P0-EXPORT proof and the pattern to copy: `utils::w_strlen` is
  replaced by a C++ body with **no hand-written asm at the seam**. Before patching it calls the
  original through `mh_calls.gen.h` and compares against the replacement on boundary inputs, arming
  nothing on a mismatch (a miniature of the shadow-mode oracle the M6 plan defines), and it
  counts its own invocations so "the determinism run stayed identical" is backed by evidence the
  replacement actually ran.
- `en_guard.*` — the EN-only init gate: 3-probe Watcom-prologue check; on any other image the DLL
  logs loudly and **arms nothing**. Per-hook guards below are the second safety layer.
- `hook/` — the low-level toolkit, no feature knowledge: `detour` (`install_jmp` full-body replace +
  `install_trampoline` steal-prologue run-before + `WATCOM_PROLOGUE`), `patch`
  (`patch_bytes_guarded` expected-bytes swap), `watcall` (`call_watcall1/2/3` — the hand-written
  __watcall(EAX[,EDX[,EBX]]) bridges, kept for call sites that hold a *runtime* function pointer;
  when you know which game function you are calling, prefer the generated `mh::call::` wrapper above,
  which is typed and shape-verified). **All hook mechanics go through this** — no ad-hoc
  VirtualProtect/E9 in feature code.
- `seams/` — feature glue over a shared spine:
  - `net_internal.h` — the spine: `PROLOGUE`, `TEV_*` ids, init-written cross-TU globals
    (C++17 `inline` vars), `seam_log`, `ms_of`, and the cross-TU function declarations.
  - `net_seams.cpp` — transport + lobby core AND the central install orchestrator
    (`MH_Seam_Init`, `install_mp_bootstrap`, `lazy_start`).
  - `net_discovery.cpp` — session record synth/browser, S2 advertise, S3 store, S4 JOIN/START/LEAVE.
  - `net_lockstep.cpp` — mode-3 pacing/perf: time_tick/present hooks, overlay de-fang, hires/qpc
    clock, horizon heartbeat, game-over leave-lockstep.
  - `net_diag.cpp` — observe-only instrumentation: temporal trace, function tracer, the
    presence_lost/savegame/GAME_MODE loggers.
  - `launch.cpp` / `harness.cpp` / `mp_menu.cpp` — launch verbs + MP drivers, determinism harness,
    menu restore. (`pool.cpp` — the DLL-hosted projectile pool — was **retired 2026-07-30**, RI-STATE
    / ST0; the retired patch is archived with its restore procedure.)
  - `gfx_overlay.cpp` (+ generated `overlay_font.h`) — the in-game debug overlay
    — a DLL-native RGB565 text blitter that paints ini-declared pages of named
    value providers onto the composed frame. Piggybacks the same present hook, **before**
    `MH_Capture_OnPresent` so captures include it; gated behind `[debug]` (no section → nothing installs)
    and logs to its own `mh_overlay.log`, so the diffed arm-log sequence is unchanged.
  - `gfx_capture.cpp` / `ui_drive.cpp` — the UI testing harness: frame capture
    (dump the locked RGB565 back-buffer to BMP) + UI drive (inject synthetic mouse-ring events to click
    widgets, no OS input). Both piggyback the `net_lockstep` present hook; both gated behind their ini
    section (`[capture]` / `[uitest]`) so normal runs are untouched.
- `attic/` — dead reference code, excluded from the build.

## Rules that keep this maintainable

1. **Binary coupling only in `mh/`** — if it dereferences a game VA, it lives in the DLL project.
   `mh_common` must compile without `addr/` ("used by glue" does not pull a utility into the DLL).
2. **Addresses are generated** — the manifest + `gen_dll_addrs.py` are the only way a VA enters the
   code. Per-TU `constexpr uintptr_t ADDR_X = mh::addr::sym;` copies are fine (internal linkage).
3. **Ship-safe no-op semantics** — every install is PROLOGUE- or expected-bytes-guarded; a mismatch
   disarms that seam loudly (arm-log line) and never half-patches.
4. **Install order is part of the behavior** — installs run centrally from `MH_Seam_Init` (directly
   or via per-cluster install functions called in the exact documented order). Trace hooks go LAST
   (the never-double-hook guard depends on it). The arm-log line sequence is diffed against the
   baseline in the refactor gate — treat it as an interface.
5. **Spine pattern** — state crossing a TU boundary becomes an `inline` var in `net_internal.h`;
   cluster-private state stays in the TU's anon namespace. Same-TU naked detour + handler + tramp
   can all stay anon; a symbol referenced across TUs from `__asm` uses plain `extern` + one def.
6. **Known traps** — trampolining a function you also read through an expected-bytes guard breaks
   the guard (install order!); SEH `__try` must stay in leaf functions (no C++ unwind mixing);
   multi-line `__asm` continuation macros need `// clang-format off` guards (bare comment only —
   trailing text on the directive line disables it).

## Adding a new seam (recipe)

1. Name the target in Ghidra; add `{ghidra_symbol, kind}` to `tools/data/dll_addr_manifest.json`;
   `python tools/gen_dll_addrs.py` (regenerates the header; name-lookup failure = hard error).
2. Pick the TU by concern (or add a new `.cpp` — register it in `mh.vcxproj` AND
   `mh_nettest/mh_nettest.vcxproj`). Local `ADDR_*` constexprs; hook via `mh::hook`.
3. Detour/handler bodies live with their cluster; the install goes into `MH_Seam_Init` (or the
   cluster's install fn) at a deliberate point in the order, emitting one arm-log line.
4. Cross-TU state → spine `inline` var; document who writes/reads it.
5. Run the gate (below). If the arm-log gains a line, note it as the new expected baseline delta.

## The gate (ONCE per session — before session end or a merge to `master`)

> **ONE COMMAND since 2026-09-10:** `python tools/run_gate.py` runs every step below OVERLAPPED —
> build first, then determinism on the VMs concurrently with the local units (suite,
> both A/B/C recorded sessions, selftests, lint) under a core budget (default: logical CPUs − 1,
> the user's call). One rig lease for the whole run (children pass through), per-unit logs in
> `tmp/gate/`, a final verdict table. `--skip det,spdet` on a box without the rig; `--list` for the
> roster. The steps below remain the reference and the by-hand form. The recorded-session arms and
> the suite's tactical-journal tail are themselves parallel since the same date (arms A∥B, one lane
> each; journals pooled) — a serial re-run for debugging is `--jobs 1` / the single-arm
> `--ui-replay`.
>
> **Not after every change** (user, 2026-07-26). While iterating, run only steps **1, 2 and 5**
> (build + transport selftests + lint — seconds), plus the single UI scenario your change touches if
> there is one:
> `python tools/ui_test.py --host 192.168.0.37:<script>.txt`. Run the whole thing below before you
> commit to `master` or wrap up the session.
>
> **What the steps actually cost (re-measured 2026-08-25).** The full UI suite is **~2 min wall** —
> 11 registered scenarios run 2 at a time, longest single scenario 91 s. This paragraph has now been
> wrong twice: it said "~20-30 min each" before parallel lanes + headless runs, then "~5 min / 12
> scenarios" until the registry changed. Step 4, determinism, is the expensive one and needs both rig
> peers. **The scenario count lives in `TESTS` in `tools/test_ui.py` and the elapsed time is printed
> by the runner** — read both there. Any figure in prose is a snapshot of a lane count and a registry
> that both move, which is exactly how this line went stale the last two times.

> **THE BUILD PRODUCES FOUR SHIPPING DLLs SINCE FORK F4E** — `mh.dll` and its three satellites
> `mh_net.dll` (the MP transport, F4B), `libmh.dll` (**the spine**, F4D) and `mh_harness.dll` (the
> determinism/replay **instrument**, F4E), all in `Release\`. `mh.dll` LoadLibrary()s each from
> beside itself at `DLL_PROCESS_ATTACH`, in that order, and what absence MEANS is different for all
> three:
>
> * **no `mh_net.dll`** = a real DEGRADATION. Single-player works; the MP browser carries a standing
>   notice explaining why it lists nothing. Nothing warns except `mh_net.log`'s first line.
> * **no `libmh.dll`** = **CONFIGURATION (1)**, a shipped configuration rather than a loss (fork
>   ruling Q10, ratified). `mh.dll` genuinely does not contain the spine — 627 of the 666 TUs moved —
>   so the game runs the original binary's own bodies: nothing is promoted, nothing is instrumented,
>   and a player cannot tell. **`mh.dll` is BYTE-IDENTICAL either way** (same sha256 deployed to both
>   lanes, measured); the configuration is which files sit beside it.
> * **no `mh_harness.dll`** = **UNINSTRUMENTED, and nothing else**. The game is bit-for-bit the game
>   it always was, because the harness only ever observes and it ships disarmed — which is exactly
>   why a lane that quietly lost it keeps passing every pixel scenario in the suite while
>   `--determinism`, `--sp-determinism` and both `--ui-abc` arms have no `mh_harness.log` to read.
>   The ONE loud case is `[harness] enable=1` with the file missing, or with `libmh.dll` missing
>   under it (ruling Q4): both refuse on three channels, name what to fix, and let the boot continue.
>
> Every lane and rig peer deploys all three satellites — `make_lane.DEFAULT_SATELLITES` is the ONE
> place that set is written, and `ui_test.SATELLITES` / `mp_run.SATELLITES` derive from it;
> `make_lane --refresh-dll` refreshes the ones a lane already has (never creates one, or it would
> undo an `--omit-satellite`). The three lanes that deliberately do not are the `module_absent`,
> `libmh_absent` and `harness_absent` scenarios. `docs/dll-split.md` is the reference.
>
> **A FIFTH BINARY IS BUILT AND IT SHIPS NOWHERE NEAR THE OTHER FOUR** (fork F4G):
> `Release\standalone\libmh.dll` plus `libref_host.exe` beside it are **configuration (3)**, the
> standalone reference — libmh and a host, no game binary. It has the same file name as the spine in
> `Release\` and is a DIFFERENT BINARY: that one is the HOSTED arm (no `MH_LIBMH_BUILD`, so every
> `MH_CRT()` site reaches the original executable's own Watcom CRT at a fixed VA) and this one is the
> STANDALONE arm (the vendored CRT, no original VA on any live path). Neither can do the other's job
> and the folder is what keeps them apart; a swapped pair is refused by name at mh.dll's bind.
> The trap: "libmh.dll" names TWO different binaries, and the one that ships beside mh.dll cannot
> run standalone. Gate step 4c runs it.
>
> `run_gate.py` checks two things straight after the build, both of which need binaries and so cannot
> be lint rows:
> * the **subset rule** (`check_module_bind.py --subset`) — a satellite loaded from
>   `DLL_PROCESS_ATTACH` may not import anything its loader does not already import;
> * the **export contracts** (`gen_libmh_contract.py --rederive --check`,
>   `gen_harness_contract.py --rederive --check` and `gen_libmh_std_contract.py --rederive --check`)
>   — the committed row lists against what the images' objects actually say. The third is
>   configuration (3)'s, and its failure is the mild one of the three: the standalone host IMPORTS
>   its spine, so a missing row is a link error naming the symbol rather than a silent fallback.
> It also prints the **three configurations' artifacts by name** with their sizes — one msbuild
> produces all of them, and a single build line is exactly what would hide one that stopped being
> produced. A row missing from `libmh.def` is not a build error: the bind refuses the
>   whole module by name and the game silently runs configuration (1); a host row missing from
>   `mh.def` makes the INSTRUMENT refuse to arm, the boot continues, and the only sign is that
>   `mh_harness.log` was never written.
>
> **The spine crossing is a STANDING ARM, not a one-time proof** (`check_module_bind.py --libmh`,
> carried by the `menu_walk` and `libmh_absent` scenarios). A brokered lane that binds `libmh.dll`
> and never calls through it reads exactly like one that crossed 13,465 times in every other gate
> this project has; the thunks count, and the counters are reported at the end of the arm.

```powershell
# 1. build (produces mh.dll, mh_net.dll and net_selftest.exe in Release\)
#    `/nodeReuse:false` IS PART OF THE RECIPE. With node reuse on, MSBuild's `/m` workers outlive the
#    build by ~15 min holding the stdout handle they were started with, so any caller that captures
#    output (a script, or an agent running this in the background) waits forever for an EOF that
#    never arrives -- a 0%-CPU "hang" with an empty log, whose surviving process tree is not killed
#    when the caller is. This is what produced the stray build/selftest trees on 2026-08-02.
#    `/m` IS NOT ENOUGH ON ITS OWN and never was: it parallelizes across PROJECTS, and this solution
#    is two big ones (mh 453 TUs, mh_nettest 669). Within a project, source files compile one at a
#    time unless the project sets `<MultiProcessorCompilation>`, which none of the five did until
#    2026-08-23 -- so a full build used exactly one core for ~10 min while `/m` sat in the command
#    line looking like it had handled the matter. Full rebuild of the solution is now ~97 s.
#    DERIVE BOTH PATHS, DO NOT PASTE THE LITERALS. This recipe used to hardcode an MSBuild root
#    and a repo checkout path; neither existed on the box it was last run
#    from, and a wrong MSBuild path FAILS SILENTLY the moment the command is piped through a filter
#    (the pipeline reports the filter's status -- two builds "succeeded" having never run, and the
#    stale binary then passed a mutation check it should have failed).
#    machine_config.py + machine.local.json are the one place that knows these.
$repo = git rev-parse --show-toplevel
$vs   = (python "$repo/tools/machine_config.py" --json | ConvertFrom-Json).VS_INSTALL_ROOT
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\src\mh_dll\mh.sln" `
  /t:Build /p:Configuration=Release /p:Platform=x86 /m /nodeReuse:false /v:minimal /nologo
# 2. the selftests -- ONE COMMAND, and it runs the whole suite UNDER ASan first.
#    ~26 s when nothing changed since the last run, ~165 s when both modes build from scratch
#    (re-measured 2026-08-23; it was 627 s before the build fixes recorded below).
python $repo\tools\run_selftests.py    # expect: run_selftests: PASS
#    ASan IS THE DEFAULT HERE since 2026-08-06, because the alternative is measurably worse: the
#    plain build turns an out-of-bounds write into DELAYED, NON-LOCAL, INTERMITTENT heap damage, and
#    a crashed run prints NOTHING (stdout is lost on abnormal exit) so it does not even resemble a
#    failed check. Both times this bit -- `ring_counts{128}` (2026-08-03, ~60% of runs) and
#    `group_scratch{101}` (2026-08-06, 3/20 runs) -- the plain build needed many repeats to look
#    suspicious and ASan named the file and line on run #1. The driver also repeats the two
#    heap-graph suites (`aitest`, `statetest`) on the plain pass, checks EXIT CODES rather than
#    parsing output for a verdict, and refuses to report a clean ASan pass from a binary that is not
#    actually instrumented (both modes stage through the shared ..\Release\net_selftest.exe, so a
#    silently-no-op ASan build would otherwise look green). `--no-asan` for the fast iterate loop.
#
#    The hand-run form below is what the driver does; keep it for running ONE suite while debugging.
#    A MISTYPED MODE NAME NOW EXITS 2 with the mode list. Until 2026-08-01 it fell through to
#    `selftest` and printed its PASS banner, so a typo in this list produced a green result for a
#    test that does not exist -- which is how `lockstepstest` (for `lockstest`) "passed" a gate run.
cmd /c $repo\src\mh_dll\mh_nettest\build_selftest.bat
& $env:TEMP\mh_nettest\net_selftest.exe selftest
& $env:TEMP\mh_nettest\net_selftest.exe selftest3
& $env:TEMP\mh_nettest\net_selftest.exe authtest   # link security: wrong key + mute probe refused, right key plays
& $env:TEMP\mh_nettest\net_selftest.exe linktest   # R-live: a CONNECTED-but-silent peer is dropped (and not early)
& $env:TEMP\mh_nettest\net_selftest.exe callstest   # P0-CALLS: every generated marshalling thunk, both cleanup disciplines
& $env:TEMP\mh_nettest\net_selftest.exe exportstest # P0-EXPORT: every generated ENTRY thunk + its stack-cleanup contract
& $env:TEMP\mh_nettest\net_selftest.exe orderstest  # O2: the order container's logic, incl. the overflow branches no rig can reach
& $env:TEMP\mh_nettest\net_selftest.exe interlocktest # C1/C4: the patch/seam interlock decision + entry ownership
#    Both halves of the interlock are about things NOT happening -- a byte that was not written, a
#    second entry patch that was refused -- and a green rig run looks exactly like a run where the
#    check was never reached. Boundary + must-not-over-refuse cases are the point; mutation-checked.
#    (`seamtest` is a KNOWN pre-existing failure -- it crashes on the baseline commit too, verified
#     2026-07-25 by stashing. Not part of this gate.)
& $env:TEMP\mh_nettest\net_selftest.exe statetest   # ST2: a state region is REBASED, and the hash,
#    the save resolver and the shadow region sets all follow it -- mutation-checked in BOTH
#    directions (poking the relocated copy changes the hash; poking the abandoned location does
#    not). Same shape as interlocktest: the load-bearing half is a thing that does NOT happen, so
#    no rig run can show it -- a determinism gate over a moved region would go green having
#    hashed nothing at all.
#    RUN THIS ONE THREE TIMES and check every exit code, not just the output. It is the only
#    selftest that operates a big graph of heap buffers, so its failure mode includes DYING rather
#    than reporting a failed check -- and a crash looks nothing like a FAIL line. From some unknown
#    date until 2026-08-03 it exited 0xC0000374 (heap corruption) on roughly 60% of runs, from a
#    one-character fixture bug (`std::vector<uint32_t> ring_counts{128}` = one element, not 128),
#    and every gate run in the log still read green because the gate invoked it once and a single
#    run passed often enough. The repeat costs about a second.

#    `compare_exclude` takes a region out of the comparison while KEEPING its snapshot/restore.
#    Same shape as statetest and interlocktest: no rig run can distinguish the two configurations,
#    because both report `0 divergence(s)` and they differ only in state the site does not compare.
#    That is not hypothetical -- on 2026-08-05 a true-positive false-red at
#    llm_strat_ai_group_home_guard_replenish was "fixed" by DROPPING four order regions from the
#    declaration, which removed the restore too, so both arms' order writes landed and the real
#    unit order was issued TWICE per shadowed call. The soak went green either way. Mutation-checked
#    in both directions (excluded region diverges -> IDENTICAL; the same state with the flag off ->
#    DIVERGENT), plus an assertion that the manifest's exclusions actually reached the generated
#    header, so re-dropping the regions goes red here.
foreach ($i in 1..3) {
  & $env:TEMP\mh_nettest\net_selftest.exe aitest    # AI0: the strategic AI's decision logic over heap buffers.
  if ($LASTEXITCODE -ne 0) { Write-Error "aitest exited $LASTEXITCODE on run $i (a CRASH, not a failed check)" }
}
#    Most of the ~215-function AI cluster is PURE over its state, so this -- not the rig -- is the
#    oracle it gets verified with: it reaches an empty candidate list, a NEGATIVE damage tally, the
#    swap-remove's victim-is-last branch and the player-7 record overrun, none of which a scenario
#    reaches. Mutation-checked. Read what it does NOT prove in the file header: self-consistency
#    with the disassembly as read, not equivalence with the original.
foreach ($i in 1..3) {
  & $env:TEMP\mh_nettest\net_selftest.exe simtest   # SIM0: the strategic SIM's logic over heap buffers.
  if ($LASTEXITCODE -ne 0) { Write-Error "simtest exited $LASTEXITCODE on run $i (a CRASH, not a failed check)" }
}
#    The aitest of the 307-function sim migration, and worth more here than it was there because the
#    sim's rig runs are the expensive ones. Repeated like aitest/statetest: its fixture is ~4 MB of
#    real-extent rosters that the code under test WRITES, i.e. exactly the population that produced
#    both historical 0xC0000374s. Mutation-checked (8/8 caught), and the mutation campaign's own
#    lesson is in the file: a mutant only tests an assertion where the two behaviours differ AT THE
#    POINT THE ASSERTION RUNS -- one was missed because the fixture had already set the bit the
#    mutant would have set.
& $env:TEMP\mh_nettest\net_selftest.exe lockstest   # L1: the turn engine's horizon/barrier logic over heap buffers
& $env:TEMP\mh_nettest\net_selftest.exe resynctest  # C8-c: the six resync residue functions over plain locals
& $env:TEMP\mh_nettest\net_selftest.exe watchdogtest # D16: the link watchdog's TIMING decisions, pure
#    A watchdog may only charge a peer for time it was WATCHING. MH_Net_Send holds the conn lock
#    across a blocking send() (SO_SNDTIMEO 5 s), so a stalled link starves the watchdog: it emits no
#    keepalives, the peer reads that as OUR silence and drops us, and when we finally take the lock
#    every last_rx has aged past the timeout so we drop it too -- BOTH ends, on a link that was
#    carrying data moments earlier. No rig run can show it: it needs a link slow enough for send()
#    to block (a LAN never is; a relayed internet game did, 2026-08-02), and the load-bearing half
#    is a drop that must NOT happen. Mutation-checked THREE ways -- no crediting (the bug), credit
#    everything, never-drop -- the obvious wrong fixes fail silently in OPPOSITE directions.
& $env:TEMP\mh_nettest\net_selftest.exe savetest    # SV1: the save format -- version gate (A) + block layer & LZW codec (B)
#    savetest takes an OPTIONAL argv[2] = a real .sav, which pushes every block of that file
#    through read_block AND re-encodes it, expecting byte identity ('ALL BLOCKS IDENTICAL').
#    Not part of the gate -- the hermetic fixtures are -- but it is the broadest evidence
#    there is for the codec: 501 blocks across four saves, e.g.
#        net_selftest.exe savetest tools\uiscripts\saves\11.sav
# 3. UI regression suite: every committed menu/lobby/HUD walk, diffed vs baselines
#     `tutorial_enter` is the one scenario here whose failure is NOT a pixel diff. It walks the real
#     main menu into TUTORIAL, clicks Start, and gates its in-game capture on the SIM CLOCK -- so if
#     the tutorial stops advancing it TIMES OUT rather than capturing a different frame. That is the
#     shape the 2026-09-09 dropped-return bug had: the game stayed alive and kept presenting, and
#     only the sim stopped. Read a red here as "did it still run", not "did it still look right".
python $repo\tools\test_ui.py --jobs 4   # expect: N passed, 0 failed
#    Runs HEADLESS by default since 2026-07-28 (no window, no focus theft; captures byte-identical,
#    so the baselines are unaffected) and LOCALLY by default since 2026-07-29 -- one lane folder per
#    peer per test on this box, which isolates each test from the previous one's setup.dat/[video]
#    state AND leaves the peer VMs free, so step 4 below can run at the same time instead of queueing
#    behind this. `--jobs N` needs the local topology (each test owns its lane, port and mutex).
#    `--no-local` runs it across the real VMs instead -- for a machine-dependent question only; a pure
#    UI/render regression does not need it. `--visible` to watch it. The determinism run below keeps
#    the blit for itself -- no blit means no vsync wait, the wrong frame rate to measure pacing at.
#    a FAIL names the exact capture -> READ the actual vs baseline PNG to see what moved. See the
#    `ui-testing` skill (author/debug loop + masking + how to grow the suite). A green suite needs no action.
# 3b. REMOVED AT FORK F5M S4b (2026-09-16) -- the per-fixture PROMOTED vs ORIGINAL A/B.
#     It replayed each registered sim fixture four times (record a golden with promotion OFF, replay
#     it to prove the fixture reproducible, run the promoted arm against it, then POKE the module's
#     write region and require the golden to go red) and reported EQUIVALENT per fixture. It was the
#     one step that compared us with the thing we replaced at FUNCTION grain, and it is gone because
#     its driver is archive-class research tooling the fork does not carry and the set of promoted
#     bodies is now frozen.
#     WHAT STILL ASKS THE SAME QUESTION, at a coarser grain: step 4b below (`--sp-determinism`) runs
#     the single-player oracle UNPROMOTED then PROMOTED from a pinned seed and requires all 61 region
#     channels to agree over 800 steps -- original-vs-ours, every promoted body at once. Step 3c's
#     A-vs-B arm does the same over a recorded human campaign session.
#     WHAT IS NOT REPLACED, stated rather than glossed: (1) the per-fixture, per-function grain -- a
#     divergence in 4b names a channel and a step, not a function; (2) the GO-RED arm, which re-proved
#     on every run that a difference WOULD be seen. Nothing else re-proves that per module.
#     WHAT WAS ALREADY GIVEN UP when this arm replaced the coverage one in 2026-09-05, kept here
#     because it is still true: the per-ROW reach census. `coverage.py --baseline --domain sim|tact
#     --check` still answers "which of the 530 rows did the fixture enter" when you want it -- it is
#     not in the gate because it needed a Debug build plus rig minutes and then went red on a
#     near-threshold AI branch that flipped between recording sessions while the simulation itself
#     stayed bit-identical (7c6bf827). A gate that is both slow and intermittently wrong gets switched
#     off within a week, so it was taken out on purpose rather than left to rot. NOTE the committed
#     sim baseline was recorded at soak_saved=600 and the registry now says 1500 -- re-record before
#     trusting it again.
# 3c. THE SINGLE-PLAYER CAMPAIGN A/B/C -- one recorded human session, three arms (added 2026-09-07).
python $repo\tools\test_ui.py --ui-abc spcamp_solo   # expect: ui-abc: PASS
#     WHAT IT ADDS OVER A SCRIPTED FIXTURE, which is the whole reason it is a separate step. The
#     scripted sim fixtures (step 3b until it was removed, and the scenarios 4b still drives) start
#     from a lobby-created skirmish and produce both arms with the SAME replayer -- so they cover
#     neither campaign session INITIALISATION (mode 1: campaign definition, planet state, landing)
#     nor the planet TRANSITION, and a harness bug that moves both arms the same way is invisible to
#     them by construction. This fixture is a real human
#     campaign session, replayed end to end, against three references:
#        A = ship            every promotion on
#        B = all-original    every [promote] key and [rebind] row rolled back
#        C = the recording   that session's own committed per-step trajectory (.oracle.gz)
#     A-vs-B is the promotion oracle; B-vs-C is the HARNESS oracle and is the arm nothing else has.
#     IT ENDS WHEN THE JOURNAL ENDS (stop_step=0), so it costs what the session costs rather than a
#     wall-clock budget, and a run that had to be killed is reported as a failure rather than read as
#     a pass. It also prints a per-arm ORDER-CODE HISTOGRAM against the recording's own: a hash
#     comparison cannot tell agreement from absence, and this fixture passed 30,000 steps with ZERO
#     orders in either arm before the keystate input channel was replayed.
#     ITS TEETH, both directions: `--ui-abc spcamp_solo --ui-gored 5000` inverts the verdict and must
#     go RED at that step. It found a wrong `verified` promotion the first time it ran
#     (llm_strat_unit_state_squad_merge -- G148), which nothing offline could see.
#     `--ui-steps N` bounds it to a rung for diagnosis; that is NOT the gate -- a prefix cannot reach
#     the transition or any order after it.
#     THE ~1-IN-3 SELF-DIVERGENCE IS FIXED (2026-09-08, SPCAMP-FLAKE), and
#     the caveat that used to sit here -- "re-run every red before believing it" -- is retired with
#     it. The cause was not in the sim: a replay had TWO input producers. The game's own
#     `llm_input_wndproc_tap` runs on every window message and keeps writing REAL host mouse events
#     into the ring the journal injects into, so anything the machine's mouse did during a run landed
#     in the replayed input -- which is why it correlated with "machine load" (somebody was using the
#     box) and vanished whenever anyone sat down to reproduce it -- and the game window was on the
#     OPERATOR'S desktop, because ui_run_arm launched it with a plain subprocess.Popen while the
#     `[rig] isolated desktop` banner claimed otherwise (fixed 2026-09-08; the tactical path had
#     the same defect and fixed it first). `[harness] replay_isolate_input` (default ON) suppresses that producer for the length of a
#     journal replay. Proven both ways on one build with the host mouse being driven throughout:
#     suppressed = 0 foreign ring events and 20 identical runs; `--ui-no-isolate` = 144 foreign
#     events and divergence at step 17. **A red from this step is now a real red.**
#     AND THE SCENARIO PASSES END TO END (2026-09-08, first time): A-vs-B, B-vs-C and A-vs-C all
#     read 0 mismatches over 30,042 compared steps, the order histogram is exact in all three arms,
#     and the run ENDS ON `UI-REC EXIT: journal exhausted` in ~180 s with 0 FORCED barriers. Two
#     more scheduling defects had to go first, both of them a record being released by the wrong
#     clock: a `P` STEP barrier whose EVALUATION was gated on a present count (the 813-mismatch
#     B-vs-C residue at step 29,069), and the journal TAIL -- the records after the last `P`, which
#     this recording has because the session ends by opening the menu -- carrying an extrapolated
#     step target the stopped sim could never reach, which deadlocked the queue 243 records from
#     the end and burned the whole 900 s budget presenting menu frames. If this step ever runs long
#     again, read the `; TJ STUCK` line before touching --ui-timeout: 180 s is the real cost.
# 3d. THE TUTORIAL A/B/C -- the second recorded human session, three arms (added 2026-09-09).
python $repo\tools\test_ui.py --ui-abc tutorial_solo   # expect: ui-abc: PASS
#     WHAT IT ADDS OVER 3c, which is why it is a separate fixture and not a second rung of that one.
#     Every spcamp journal lands SESSION_MODE=1 (CAMPAIGN) at planet 0. This one lands
#     SESSION_MODE=2 (single-player skirmish) at planet 31 -- the injected slot llm_game_start_tutorial
#     stamps before calling session_begin_multi -- so it is the ONLY fixture that runs the tutorial's
#     own session boot, its hardcoded 1v1 player setup, llm_tutorial_step_driver's per-frame
#     objective/komenda/panel evaluation, and the menu ASYNC-CALLBACK PUMP that drives it. Its
#     order histogram is its own: codes 20/E6/EA that spcamp never issues, 49 squad merges against
#     spcamp's 11, and 7E at 48 against spcamp's 3.
#     COST: ~200 s for the pair of arms (115 s ship + 85 s all-original, 18,317 steps each), ending
#     on `UI-REC EXIT: journal exhausted` rather than a wall-clock budget.
#     ITS TEETH, PROVEN: `--ui-abc tutorial_solo --ui-gored 5000` -> A-vs-B and A-vs-C both go RED
#     AT the poke (first mismatch 5000) while the unpoked B-vs-C stays clean, so the poke is
#     localised and the comparison is live rather than blind.
#     WHY IT EXISTS: until it did, `llm_game_start_tutorial` and `llm_tutorial_step_driver` were
#     EXPORT_REPLACE-promoted -- live in every hosted run -- with nothing in the whole gate entering
#     the tutorial, and the offline suite that did cover the driver called it and DISCARDED its
#     return. It found a shipped, player-facing defect on its first run (G156, fixed at EN v397):
#     the driver's real `return 0` was typed `void`, so the generated thunk returned garbage EAX and
#     the async-callback pump -- which uninstalls any callback returning nonzero -- dropped the
#     tutorial after ONE tick. The process stayed alive and kept presenting throughout, which is why
#     nothing resembled a crash and no existing arm could see it.
# 4. the real behavioral gate: 2-machine lockstep determinism (both VMs up).
#    PREFERRED = the UI-PATH run: drive both peers into the game through the REAL menu->lobby->Start
#    (no force-entry), run N in-game steps, mp_analyze -> ALL PAIRS IDENTICAL:
python $repo\tools\test_ui.py --determinism --ship-pacing --steps 3000   # expect: ALL PAIRS IDENTICAL
#    --ship-pacing is LOAD-BEARING: without it the rig pins lockstep_step_ms=30/sim_step_ms=10 in its
#    own ini, so a green run says nothing about the pacing players actually get. That gap hid a
#    client freeze (adaptive floor vs sim_step, 2026-07-25) through a fully green 800-step gate.
#    Run 3000+ steps: the frozen-client case only appeared past ~1165.
#    `--det-standard` runs THREE shapes and reports them separately: ASYMMETRIC (ours vs the
#    original -- the only shape that catches a deterministic-but-WRONG engine), SYMMETRIC (ship
#    config), and U28 3-PEER START BARRIER (a third peer as a local lane; both clients disagree
#    with the host at Start and the barrier must override BOTH). Each fails if its run turns out
#    VACUOUS -- promotion that never went live, or a client whose injected disagreement never
#    survived to Start -- rather than passing on a green hash.
#
#    WHEN THE CHANGE TOUCHES A PROMOTED SEAM, run the standard shapes instead of the one above
#    (the C7 standard-shape rule):
python $repo\tools\test_ui.py --determinism --det-standard --ship-pacing --steps 3000
#      SYMMETRIC (ship config) promotion on both peers, fixes at shipping defaults -- what players run.
#      ...plus the U28 3-peer start barrier and U32's conquest run.
#
#    THE ASYMMETRIC SHAPE IS NO LONGER IN THAT SET (removed 2026-09-01, user's call), and the reason
#    is that the question it asks is not a question about shipped behaviour. It gates OURS against
#    the ORIGINAL netcode -- and the original netcode is DEAD. Retail mh.exe has no socket layer at
#    all: its lobby builds and CRC-checks packets that are never transmitted ("dead at the wire",
#    the lobby is dead at the wire), and MH's working multiplayer IS the restored one, with the injected DLL
#    supplying the transport. The original lockstep path only executes at all because the byte
#    patches animate it, so "do we match the original" is a COMPATIBILITY question, not the gate's.
#    The mechanism is kept for exactly that -- future compat testing -- and runs by hand:
#      python tools\ui_test.py --determinism --steps 3000 ^
#          --host <hostip>:mp_host_start.txt --client <clientip>:mp_client_start.txt ^
#          --connect-ip <hostip> --ship-pacing ^
#          --extra-ini-host   tools/uiscripts/ini/ship_config.ini ^
#          --extra-ini-client tools/uiscripts/ini/rollback_original.ini
#    BOTH FRAGMENTS ARE REQUIRED. Promotion is the shipping default (SHIP_PROMOTE_LOCKSTEP=1, C8-f),
#    so the host-only fragment sets a key that is already 1 and the run silently becomes symmetric --
#    which is exactly what the client fragment exists to prevent.
#    Each shape reports its OWN verdict, compared-step count and promotion liveness lines, and a shape
#    whose asymmetry EVAPORATED (promotion requested, nothing went live) FAILS rather than passing on a
#    green hash. Putting a migrated-fix knob in --extra-ini-host is REFUSED by name: a fix enabled on
#    one peer makes every difference expected, which is how a real divergence gets waved through.
#
#    WHEN THE CHANGE TOUCHES TACTICAL MODE, the two shapes above cannot see it at all: mode 6 never
#    calls llm_strat_sim_step, so the strategic cadence hook never fires and the run emits zero hash
#    lines for the whole excursion. Its own oracle is single-machine and takes ~2 min:
python $repo\tools\test_ui.py --tact-determinism --tact-selftest  # expect: tact-determinism: PASS
#    Two arms through the same `--tactical <save>` entry, compared on the per-frame T/TR lines the
#    llm_tact_frame cadence emits, plus a THIRD arm that pokes one tactical region mid-run.
#    --tact-selftest IS NOT OPTIONAL: a hash over bytes nothing writes is identical across two runs
#    too, so only the RED arm distinguishes "deterministic" from "measuring nothing". It requires the
#    compare to fail at EXACTLY the poked frame and to name EXACTLY the poked region (default
#    tact_doors, chosen because it does not otherwise change during a run).
#
#    WHEN THE CHANGE TOUCHES THE EFFECT SEAM -- tools/data/*_effect_classes.json, effects/*, or the
#    generated gates -- the determinism run above CANNOT see it. Both of its arms are the ORIGINAL
#    game, so no shadow window ever opens and every gate stays transparent; a seam that suppressed
#    everything, or nothing, would pass it unchanged. This is the check that opens a real window
#    over real gated entries at a real tactical frame, and it takes ~30 s:
python $repo\tools\test_ui.py --tact-effects-probe 120 --tact-frames 300  # expect: tact-effects-probe: PASS
#    Three subjects, one per disposition: an `effectful` gate must read performed=1 suppressed=1,
#    and BOTH a `state` and a `pure` one must read performed=2 suppressed=0. Both non-effectful
#    classes, because they are never-suppressed for different reasons and a suppression built only
#    from the effectful cases passes its own test vacuously. ABSENCE OF A PROBE LINE IS A FAIL, not
#    a pass -- a run that never reached the probe frame prints nothing, which is exactly what a
#    grep-for-failures check would call green.
# 4c. CONFIGURATION (3): the standalone fixtures, replayed against libmh.dll (added 2026-09-13, F4G).
#    Every committed LIB-REF fixture (DISCOVERED under the fixtures directory, not listed) unpacked from its
#    sha-checked form and replayed 5000 steps by libref_host against Release\standalone\libmh.dll --
#    a DIFFERENT binary from Release\libmh.dll and necessarily so: that one is the HOSTED arm, whose
#    MH_CRT() sites call the original binary's Watcom CRT at fixed VAs and therefore cannot run in a
#    process with no mh.exe mapped. Purely local: three console processes,
#    no lane, no VM, no game copy, ~70 s serial.
python $repo\tools\replay_libref.py   # expect: PASS (3 fixture(s))
#    IT ASSERTS ITS OWN SUBJECT. A build that quietly went back to linking the archive into the host
#    would replay exactly as green, so the unit reads libref_host.exe's IMPORT TABLE and fails if
#    libmh.dll is not in it. It also re-verifies the asset declaration
#    (tools/data/libref_asset_dispositions.json: the eight init\H_31xx.DMP the fixtures ask for and
#    the retail bank does not have) against a real mh.nam on any box with the game, and REFUSES if a
#    declared-absent name turns out to be present. Without the declaration every fixture replays
#    5000/5000 IDENTICAL and then fails on a fail-closed asset trap -- G183.
#    Fallback / force-entry-specific paths only:
# python $repo\tools\mp_run.py --steps 800 --lobby        # (force-entry via launch.cpp)
#    + diff the host mh_net.log ';'-lines vs tmp/refactor-baseline/host_logs/mh_net.log
#      (exclude the timing/heap-dependent GameRecv/label= DIAG lines)
# repo lint (fast, Ghidra-free): addr drift + clang-format + ruff, plus check_const_view -- which
# compiles src/mh_dll/mh_nettest/ai_const_negative.cpp once per case and requires each write through
# the AI const state view to FAIL WITH A CONST DIAGNOSTIC, and the positive arm to compile. It is in
# the fast gate rather than a script somebody remembers, because the failure mode is a `const`
# quietly disappearing from a header during an unrelated edit, which nothing else here can see.
python $repo\tools\lint_repo.py
```

### ASan on the selftests — IN the gate by default since 2026-08-06

`tools/run_selftests.py` (gate step 2) runs the whole suite under ASan before the plain pass, so this
is no longer something to remember. The manual form, for driving ONE suite while debugging:

```powershell
cmd /c $repo\src\mh_dll\mh_nettest\build_selftest.bat --asan
& $env:TEMP\mh_nettest_asan\net_selftest.exe aitest      # own outdir; runtime DLL copied beside it
```

**Why it stopped being opt-in.** It was "reach for it deliberately" until 2026-08-06, when the second
instance of the same one-character fixture bug (`std::vector<int32_t> group_scratch{101}`) cost
another session — discovered mid-gate as a 3/20 flaky crash, bisected against a stashed tree, filed,
and only then found. The 2026-08-03 fix had closed one instance without adding anything that would
catch a sibling, which is the failure this default removes. Cost measured (2026-08-23): ~13 s of
extra run, and ~75 s of extra build only on a run that has to build the ASan mode from scratch —
toggling `/fsanitize=address` used to force a full rebuild of BOTH modes because they shared an
`IntDir`, which is now split (`mh_nettest.vcxproj`), so an unchanged rebuild of either is ~1 s. Still
worth running by hand after adding or resizing a fixture buffer, rather than waiting for the gate.

**Why it earns the slot.** `aitest` is the primary oracle for ~215 reimplemented AI functions, and a
wrong extent in a translation produces exactly an out-of-bounds access. Untreated that is *delayed,
non-local and intermittent*: on 2026-08-03 a one-element `std::vector<uint32_t> ring_counts{128}`
(the `initializer_list` constructor, not a size) made `aitest` die `0xC0000374` on ~60% of runs, with
a stack pointing at `recorder::clear` freeing a `std::vector<double>` that appears nowhere in the
source. With `--asan` and the same bug present, the first run says:

```
ERROR: AddressSanitizer: heap-buffer-overflow ... WRITE of size 4
  #0 in `test_plan_turret_upgrade'::`2'::<lambda_1>::operator()  ai_selftest.cpp:5683
```

**Static analysis does not substitute for it, measured rather than assumed.** clang-tidy
(`bugprone-*`, `clang-analyzer-*`, `cppcoreguidelines-*`, `modernize-*`, `readability-*`) and MSVC
`/analyze` were both run against that exact line and both came back clean. They cannot see it: the
defect is legal, unambiguous overload resolution, and catching it needs the container's size modelled
through the constructor and range-checked against a subscript 5000 lines away. `_ITERATOR_DEBUG_LEVEL=1`
does catch it, but reports only by dying `0xC0000409` with nothing on stdio.

Baseline as of 2026-08-03: **all 13 modes clean under ASan** (`aitest callstest exportstest orderstest
selftest selftest3 interlocktest authtest linktest lockstest statetest savetest resynctest watchdogtest`). A new
report is therefore a real finding, not accumulated noise. (`seamtest` is a separate, documented
pre-existing failure and is not in that list.)


## Configuration reference

- **The ini is OPTIONAL (ship build, 2026-07-25).** `MH_Seam_Init` arms with no `mh_net.ini` present;
  every `[net]` key carries a shipping default and the file only overrides. `[net] enable=0` is the
  off switch. (Until 2026-07-30 a hosted-pool build detected itself from the exe's import of
  `MH_HostedPoolBase`; the pool is retired and that detector is gone — the import is now purely the
  DLL force-load anchor.)
- **Link security is a FILE, not an ini key** — `mh_key.txt` next to the exe (generated on first
  run, shared by the host with its players).
  Both rig runners deploy a pinned key to every peer; without that a multi-peer run is refused.
- **[`mh_net.example.ini`](mh_net.example.ini)** — **the whole configuration surface, one file**
  (`[config]` / `[net]` / `[capture]` / `[uitest]` / `[debug]` / `[trace]` / `[menu]` / `[video]` /
  `[ui]` / `[pause]` / `[desync]` / `[patch]` / `[input]` / `[compat]` / `[tombstone]` / `[save]` /
  `[bisect]` / `[harness]`), each key with its real default taken from the source. Copy it next to
  the exe and uncomment what you need; an all-default copy behaves like no file.
- **The harness is a SECTION of that file, and it arms on `[harness] enable=1`** (fork F2G / plan
  D12, ruling Q6). It used to be a separate `mh_harness.ini` that armed off its own mere existence,
  with no `enable=` key — two failure shapes in one: a `[harness]` block written into `mh_net.ini`
  was silently ignored (that is how an "800-step" gate once compared 200 steps), and a stale copy
  of the file on a test machine armed somebody else's run. A leftover
  `mh_harness.ini` beside the exe is now **REFUSED** by `mh/config/config.h` — the process terminates
  and names the file in `mh_config_refused.log` — rather than ignored, on the rule that a file which
  used to arm an instrument and now arms nothing is the quiet-wrong answer.
- **Retired sections are refused, not ignored**: `[promote]` / `[promote_skip]` /
  `[state_handler_skip]` / `[rebind]` (F2E) and `[pacing]` / `[test]` / `[probe]` (F2G — `fps_cap`
  moved into `[video]` beside the `no_present` it requires, `lane` into `[uitest]`, and the LT1C
  cell-grid probe is gone). Each refusal names the section and where its statement went.

- **`[config] mode` — WHICH IMPLEMENTATION RUNS, and the only configuration key most installs need**
  (fork plan D11, landed at tracker `fork` F2A). `brokered` (the default) is today's ship build: the
  reimplemented spine serves the domains it owns. `original` runs the game's own bodies — every
  promotion default becomes 0 and no rebind row arms — and it is the SUPPORTED, TESTED rollback, not
  merely an available one. `standalone` is not selectable from an ini: it is a property of the build
  (`MH_LIBMH_BUILD`), so the selector agrees with the standalone arm by construction rather than by
  convention. An unrecognised value falls back to `brokered` and names itself in the log.
  - **Everything below derives from it.** The `[promote]` keys and the `[rebind]` rows still work and
    still WIN where they are set explicitly, but they are now an unadvertised per-key override on top
    of the mode — the mode is what sets the defaults they fall back to. Do not build a new rollback
    out of them; `[config] mode=original` is the rollback, proved byte-identical to the derived
    47-key + 719-row fragment over 8000 steps across all 61 region channels.
  - **A non-default mode announces itself** (`; [config] mode=original -- the ORIGINAL engine runs …`)
    and `brokered` deliberately prints nothing: the arm-log line sequence is a standing refactor gate,
    so the ship log must not grow a line that the absence of one already said.
- **`[promote]` — running the reimplemented bodies for real.** `lockstep=1` installs the whole
  turn-engine closure (ten seams); `orders=1` does the same for the order container. Absent is the
  default and the rollback: nothing installed, the stock path byte for byte. `lockstep_seams=a,b`
  narrows the set, but that is a **diagnostic** shape — only a whole-closure run may be read as a ship
  verdict, and the DLL says which it is on a `RUN-CONFIG:` line so a runner can refuse.
  - **`sim_tick=1` is a separate opt-in and is NOT part of `lockstep=1`** (C6). It is the eleventh seam
    and it arrives by **rebinding the determinism harness's detour** rather than patching the entry —
    the instrument must keep that entry, because if the *harness* lost the race a run would record no
    hashes and look clean rather than fail. It is out of the default closure on purpose: every L1-P
    promotion result was measured with `sim_tick` original, and quietly changing what `lockstep=1`
    installs would make all of that evidence describe a configuration nobody ran.
  - **Read the liveness lines before believing a promoted run.** Each seam logs its first call and then
    widens: `; [promote] <name>: call #N (OURS is live)`. An asymmetric test whose asymmetry evaporated
    does not fail, it passes.

When you add an ini option, add it to the matching example file. The two stay honest via:

```bash
grep -rhoE 'GetPrivateProfile(Int|String)A\("[a-z]+", *"[a-zA-Z_0-9]+"' src/mh_dll/mh/
```

(plus `net_ini_int`/`net_ini_str` in `launch.cpp` and the `page.<name>.*` / `*_key` families in
`gfx_overlay.cpp`, which are built dynamically and so never appear in that scan).

## Pinned external contracts

- Output path `src\mh_dll\Release\mh.dll` — consumed by `tools/mp_run.py`.
- Export names (`MH_HostedPoolBase`, …) — referenced by the patcher import descriptors; DllMain
  arming semantics per the ini gates (`mh_net.ini` — ONE file since fork F2G). **`MH_HostedPoolBase` is the
  FORCE-LOAD ANCHOR** every injection patch imports (`net_load`, `net_load_EN`, `harness_load`,
  `launch_load`) and none of them calls it; it returns `NULL` since ST0. Renaming it would unload the
  DLL from every already-deployed patched exe.
- Formatting: `.clang-format` / `.editorconfig` at repo root; the tree is format-idempotent and
  `tools/lint_repo.py` enforces it.
