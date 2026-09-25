# The DLL split — how mh.dll loads a sibling module

Fork F4 turns one injected `mh.dll` into four binaries — `mh.dll`, `mh_net.dll`, `libmh.dll`,
`mh_harness.dll` (fork stage F4). This document owns the part every one of
those splits depends on and none of them can decide for itself: **how a module gets loaded and
bound, and what happens when it is not there.**

Everything below was measured on 2026-09-13 (fork F4A) on the EN build, on real boots of the game.
Nothing here is inferred from documentation about how Windows is supposed to behave.

---

## The problem

`mh.dll`'s entire arm runs in `DllMain` (`src/mh_dll/mh/mh.c`) — four calls whose order is "a
contract, not a convenience". After the split, three of those four either live in a satellite or
call into one, so a satellite must be bound **before** the arm, i.e. from inside `DLL_PROCESS_ATTACH`,
under the loader lock. Three standing source rules say that is exactly where you may not load a DLL,
and the obvious alternative — a static import — destroys the property the whole split is for.

Three candidate mechanisms, all measured rather than argued (F4 ruling Q2):

| | mechanism | verdict |
| --- | --- | --- |
| **A** | `LoadLibrary` + `GetProcAddress` from `mh.dll`'s `DllMain` | **RULED. Works.** |
| **B** | two-phase: bind from `on_present`, off the loader lock | works; kept as the documented fallback and as the arm point for anything that does not need to exist before the arm |
| **C** | static import | **REJECTED — measured fatal.** A missing satellite kills the process at `0xC0000135` before one instruction of ours runs |

---

## The ruling

> **mh.dll binds a sibling DLL with `LoadLibrary` on an absolute path next to itself, plus
> `GetProcAddress` per symbol, as the FIRST statement of `DLL_PROCESS_ATTACH`. Every satellite's own
> `DllMain` touches nothing outside its own module. `mh.dll`'s `DllMain` stays the sole
> orchestrator, calling each satellite's exported init explicitly, in an order it chooses.**

Mechanism: `src/mh_dll/mh/include/mh_module_bind.h` (the argument) and
`src/mh_dll/mh/seams/module_bind.cpp` (the code).

> **The instrument that proved it was the F4A SPIKE satellite** (`src/mh_dll/mh_spike/`, armed only
> by `[modules] spike`, default `off`). **It is DELETED, and so is the knob** — F4B shipped the first
> real satellite and F4A ruling (a) says a real satellite is unconditional. Every `mh_spike` line
> below is therefore a RECORD of a measurement, not a recipe you can run; the F4B section further
> down carries the same measurements on `mh_net.dll`. The spike's `/p:MhSpikeStatic=1` build (the R2
> static-import arm) went with it: its numbers are here, its subject is not.

### Why the loader-lock ban does not bite this shape

The ban is real and all three sites are right about their own cases. What makes them right is not
"`LoadLibrary` under the lock" as a blanket fact; it is **what the load drags in behind it**. Taking
the three sites one at a time, which is what makes this ruling durable rather than a local escape:

**1. `seams/net_lockstep.cpp:335-337` (the comment block above `ensure_key_once`) — the key mint.**

> *"Why not in `MH_Seam_Init`: that runs in DllMain, and minting a key needs the system CSPRNG,
> which we reach via `LoadLibrary("advapi32")` — a LoadLibrary under the loader lock is a documented
> deadlock. `on_present` is the first place that is both off the loader lock and guaranteed to
> run."*

**Applies to its own case; does not transfer.** `advapi32` is a *system* DLL that this process does
not already depend on, and the CSPRNG path behind it loads further providers — an open-ended
dependency graph whose `DllMain`s would run under a lock we hold, any of which may wait on another
thread. The satellite case has none of that: see the subset rule below. The site's other half —
"`on_present` is the first place that is both off the loader lock and guaranteed to run" — is what
mechanism **B** is built on, and it is still true.

**2. `seams/video.cpp:969-972` — DPI awareness.**

> *"Resolved dynamically from the ALREADY-LOADED user32 (`GetModuleHandle`, never `LoadLibrary`):
> calling LoadLibrary under the loader lock is a documented deadlock…"*

**Not a load at all, and the rule it states is one we obey.** The module is already in the process;
the correct primitive is a lookup, not a load. The spike's *reverse* edge — the satellite calling
back into `mh.dll` — uses exactly this: `GetModuleHandleA("mh.dll")` + `GetProcAddress`, never a
load. F4D's `libmh.dll → mh.dll` edge and F4E's `mh_harness.dll → libmh.dll` edge inherit that
verbatim.

**3. `src/mh_shim/msvfw32/proxy_core.cpp:14-16` — the shim's Rule 1.**

> *"NOTHING happens in DllMain. mh.dll is a static import, so the loader has already initialised it
> by the time we get here; we have nothing to do and must not do it. In particular no LoadLibrary
> — calling it under the loader lock is a documented deadlock)."*

**This is the rule for a SATELLITE's DllMain, and F4 adopts it wholesale — it is not a rule about
`mh.dll`'s.** Read in context, the shim's statement is "*I* have nothing to do here": its own work
(loading the real `msvfw32`) is deferred to the first forwarded call, and it defers it because that
load is a system DLL with its own dependency graph — the same reason as site 1. Its Rule 2 (load by
**absolute path**, never a bare name, because a bare name found the shim *itself*) is also adopted
here, and it buys a second thing F4 needs: with exactly one path tried, "the module is not here" is
a fact rather than a search that quietly succeeded somewhere else.

### The subset rule (the fact that makes A safe, stated so a future satellite can check it)

When `mh.dll`'s `DllMain` runs, every DLL `mh.dll` statically imports has already been **fully
initialised** — that is the loader's dependency order, and the static-import arm below
measures it directly (the satellite's `DllMain` runs 181 us BEFORE ours).
Today that set is:

    KERNEL32.dll   USER32.dll   WINMM.dll   WS2_32.dll        (dumpbin /imports mh.dll, 2026-09-13)

So:

> **A satellite is safe to load from `mh.dll`'s `DllMain` when (a) its own `DllMain` is inert, and
> (b) its static imports are a SUBSET of `mh.dll`'s own.** Then the load maps a file, snaps an
> import table against modules that are already initialised, and runs one inert `DllMain` — there is
> no second `DllMain` to run and nothing to wait for. At `DLL_PROCESS_ATTACH` during process
> initialisation there is also only one thread, so there is no other lock holder to deadlock against.

`mh_net.dll` is covered by this without doing anything: `WS2_32` is already in the list, because the
transport is compiled into `mh.dll` today. A satellite that wants something *outside* the set (a new
system DLL, COM, a thread) must either add the import to `mh.dll` too — so the loader initialises it
before us — or move to mechanism **B**.

---

## The measurements

Lane: `workdir/mh_lanes/f4a`, a stock-exe lane (retail `mh.exe` + the `msvfw32` proxy shim, so the
chain under test is the shipping one: `mh.exe → msvfw32.dll → mh.dll → mh_spike.dll`).

### A — bind from `DllMain`, satellite present

```
; [modules] mh_spike: loader order -- satellite DLL_PROCESS_ATTACH is AFTER mh.dll's by 1015 us
  (attach_calls=1 attach_tid=5028, mh.dll tid=5028); bound at DllMain (under the loader lock)
; [modules] mh_spike: init (abi=F4A00001 attach_calls=1 attach_tid=5028)
; [modules] mh_spike: reverse edge ok (called mh.dll from the satellite)
; [modules] mh_spike: BOUND at DllMain (under the loader lock) -- 3 exports resolved,
  init returned F4A00001, call-through 2+3=5
```

Six boots (one through `ui_test.py esc_menu.txt`, five manual console launches with stderr
redirected): **6/6 alive, stderr empty every time, 150k–190k presented frames each, the satellite's
`DllMain` ran exactly once on `mh.dll`'s own thread**, 1015–6834 µs after `mh.dll`'s. The `esc_menu`
capture read 0.666 % — byte-identical to the knob-off control run of the same scenario.

Three things this proves separately, and the third is the one a `GetProcAddress` success does not
give you: the module loaded; its exports resolved; **a call across the boundary returns the right
answer** (`2+3=5`), and so does one back the other way (the satellite calling an `mh.dll` export it
found with `GetModuleHandleA`).

### A — satellite is a *different* real DLL, which itself statically imports `mh.dll`

Planting the `msvfw32` shim under the satellite's name (it statically imports `mh.dll`) loaded
cleanly from inside `mh.dll`'s own `DLL_PROCESS_ATTACH`: the loader snapped its import table against
an `mh.dll` that was **mid-initialisation**, its inert `DllMain` ran, and the contract check then
refused it by name and the boot continued:

```
; [modules] mh_spike: LOADED BUT REFUSED -- …\mh_spike.dll is missing an export this build requires
  (init=0 add=0 probe=0). Continuing without it.
```

That is the shape F4D and F4E need — a satellite that calls back into its loader — and it is the
one thing about mechanism A that looked genuinely uncertain beforehand. A module being *mid-*
`DllMain` does not stop its exports being bindable: its image is mapped and its C runtime has run
long before `DllMain` is called.

### A — satellite absent (the degradation)

```
; [modules] mh_spike: NOT BOUND -- LoadLibrary(…\mh_spike.dll) failed, Win32 error 126
  (the file is not there). Continuing WITHOUT the module: this is the absent-tolerant path,
  not a failure of the boot.
```

The boot continues through the whole arm to its end marker (`; [interlock] … detour install(s)`,
log line 1153 of 1187) and the main menu renders. This is the registered suite scenario
`module_absent`.

### B — bind from `on_present`

Binds 525 ms after `DllMain`, on the first presented frame, with the same four lines. **`check_arm_order`
PASSES against the committed `brokered` baseline unchanged**, because those lines land at log lines
1161-1164 — *after* the arm window, which ends at 1152. Recorded here because it is an F4F input:

> The **DllMain** arm's lines land at the very head of the arm window and therefore DO cost a ruled
> baseline edit (measured: `INSERTED` at template step #2, log line #1). The **on_present** arm's
> cost nothing. Neither is a reason to choose one over the other — the reason is whether the thing
> being bound has to exist before `MH_Core_Arm_Early` — but F4F should not be surprised by either.

### C — static import (rejected)

Built with `/p:MhSpikeStatic=1` (`src/mh_dll/mh/mh.vcxproj`; the artefact is never deployed):

* **satellite present:** `satellite DLL_PROCESS_ATTACH is BEFORE mh.dll's by 181 us`, same thread.
  **This is R2, measured.** A statically-imported satellite is initialised *before* its importer, so
  its `DllMain` runs at a point `mh.dll` does not choose and cannot orchestrate. That is G104 at
  module scope — the failure class where every gate stayed green and every boot died at
  `0xC0000409` (`mh_core_arm_export.h:21-28`) — and it is why **every satellite's `DllMain` must be
  provably inert**: the rule has to hold whether or not the final build imports statically.
* **satellite absent:** process exit **`0xC0000135`** (`STATUS_DLL_NOT_FOUND`), **zero log
  directories created**, nothing on stderr. Not one instruction of ours ran. Every "degrade, don't
  fail" ruling in F3/F4 — Q2's no-module browser notice, Q4's uninstrumented config (1) — is
  unimplementable on top of a static import, and this is the measurement that says so.

### What "inert `DllMain`" means, precisely

A satellite's `DLL_PROCESS_ATTACH` may only touch memory **inside its own module**. No arming, no
hook, no file, no `LoadLibrary`, no thread, no call into another module. `DisableThreadLibraryCalls`
is permitted and wanted — it is loader bookkeeping, and the shim does it for the same reason.
Recording a counter for a later readout is *self-observation*, not work: nothing outside the module
can observe it until `mh.dll` asks. (`mh_spike.cpp`'s `DllMain` is three kernel32 calls and a
return, and that is the template.)

The trap worth naming: **"my `DllMain` is empty" is not the same statement as "my module does
nothing at attach".** `_DllMainCRTStartup` runs the module's C++ static constructors *before* the
body sees the notification, so a satellite with dynamic initialisers has already run arbitrary code
under the loader lock. Keep satellite statics to zero-initialised PODs, or the rule is decorative.

**This rule is a gate, not a paragraph** — `check_module_bind.py --dllmain-inert`, a `lint_repo` row.
It finds every `DllMain` in `src/`, exempts exactly one **by name** (`src/mh_dll/mh/mh.c`, the sole
orchestrator — and a rename refuses rather than silently having nothing to exempt), and requires
every other body to call nothing but the three permitted primitives. A scan that finds **no**
satellite refuses too. It covers the body only; the static-constructor half above is still a rule a
human keeps.

### One measurement trap, recorded because it cost a wrong reading here

The static-import arm was first anchored the way `msvfw32/proxy_core.cpp:28-33` anchors `mh.dll` —
`&Export` stored in a non-const global, chosen there precisely because a *call* could be optimised
away. That project builds with `WholeProgramOptimization` **off**; `mh.dll`'s Release build has it
**on**, and under `/GL`+`/LTCG` the unreferenced global was eliminated, the import descriptor was
never emitted, and `dumpbin /imports` showed the ordinary four system DLLs. The measurement build
was silently measuring the dynamic case. The anchor is a real call now.

---

## The bind-failure path

Four outcomes, one shape: **name it loudly in `mh_net.log` and carry on.**

| outcome | when | line |
| --- | --- | --- |
| `BOUND` | loaded, all exports resolved, ABI matched | reports the arm point, the ABI and the call-through result |
| `NOT BOUND` | `LoadLibrary` failed | names the **full path** and the **Win32 error** |
| `LOADED BUT REFUSED` | loaded, an export missing | names which of the contract's symbols were absent |
| `ABI MISMATCH` | loaded, struct size or version disagrees | prints both sides' numbers |

Why the log is `mh_net.log` and not a new file: F4 ruling Q9 — it keeps its name and is documented
as the **core-arm log** (49 of the 69 structural lines in a brokered boot are `mh.dll`'s). The path
is composed locally from `MH_RunDir()` rather than taken from `net_internal.h`'s `g_log`, because
`g_log` is filled by `build_paths()`, which runs *after* the bind; a bind that logged through it
would write to an empty path and the one line that reports whether the module loaded would say
nothing.

**The asymmetry with a mis-spelled key is deliberate.** `[net] module=non` is **refused**
(`mh::config::detail::refuse` — the F2E rule), because that is an author believing something false
about what will execute. A **missing module** is the shipped degradation this whole item exists to
build, and it continues. Same file, opposite handling, and the difference is who was wrong.

Since F4B there is a fifth row, and it exists to keep those two apart:

| outcome | when | line |
| --- | --- | --- |
| `NOT ATTEMPTED` | `[net] module=none` — the run declined the load | says which key, and that no file was looked for |

`NOT BOUND` and `NOT ATTEMPTED` arm identically and the game behaves identically; they are separate
outcomes because "the module is not there" and "this run was told not to look" are different facts
about a boot, and `check_module_bind --expect absent` refuses a `declined` log rather than accepting
it as a weaker pass. Collapsing them is the `[net] enable` conflation F3B spent an item undoing one
level up.

---

## F4B — `mh_net.dll`, the first real satellite

Everything above was measured on a SPIKE (`src/mh_dll/mh_spike/`, armed by `[modules] spike`).
**That spike is gone, and so is its knob** — F4A ruling (a): a real satellite is unconditional and
**absence is the configuration**. A per-module enable key would rebuild the "off or absent?"
ambiguity F3B spent an item removing from `[net] enable`. What replaced it is `mh_net.dll`.

### What moved, and what deliberately did not

| | |
| --- | --- |
| into `mh_net.dll` | `net_transport.cpp` (1429) + `net_key.cpp` (150), moved out of `mh_common/` into their own project `src/mh_dll/mh_net/`, plus the objects they use out of `mh_net_proto.lib` (`net_crypto`, `net_wire`) |
| stayed in `mh.dll` | `run_context.cpp` (ruling Q1), `mh_common`'s `bmp_io`/`bnk`/`lzw`/`misc`, and `mh_net_proto`'s `session_info.cpp` — `net_discovery.cpp` serialises browser descriptors with it, and it is a pure static-lib object, so both images link the copy they use |
| deleted | `MH_Net_Init` and `MH_Net_Shutdown` — **zero call sites anywhere in `src/` or `tools/`**, re-measured at F4B's own HEAD. `MH_Net_Init` was a second, drifting reader of the `[net]` block that `lazy_start` already reads; `MH_Net_Shutdown` was a teardown path nothing has ever called |

**The contract is `mh_common/include/mh_net_module.h`**: 23 bound symbols (22 `MH_Net_*` + `MH_Key_Load`)
in one X-macro list, expanded four times — the function-pointer table, the `GetProcAddress` loop, the
forwarding shims, and the offline gate's parse. Plus two module-level entries the bind calls
directly (`MH_NetModule_Init`, `MH_NetModule_Probe`) and `mh_net.def`'s 25-name export list.

### The 18 ungated call sites, and why they needed no guards

The F4 surface pass measured 80 `MH_Net_*` call sites in `mh.dll` across 11 files, **18 of them with
no transport-present test in front** (`launch.cpp` 12, `ui_drive.cpp` 1, `gfx_overlay.cpp` 1,
`harness.cpp` 1; `desync_watch`'s 3 were gated). While the transport was linked in, an ungated call
reached a real function that answered "not started". The moment it is a file that can be missing, an
ungated call is a call through a null pointer.

**The fix is not 18 guards.** `mh.dll` still DEFINES all 23 symbols — as forwarding shims over the
bound table (`mh/seams/module_bind.cpp`) — and each answers, when unbound, the value the real body
answers with `g_started == 0`. So an absent module and an un-started transport are indistinguishable
to every caller, **all 80 call sites are unchanged byte for byte**, and the ungated count is zero *by
construction* rather than by inspection. Gating call sites would have made absence a property every
caller has to remember, unverifiable by anything but reading, and growing with every new call site.

One row is not zero-shaped and the reason is a hang: **`MH_Net_IdAssigned` answers 1**.
`launch.cpp:907` WAITS on it before entering a joined game; with no module nothing can ever assign an
id, so 0 would turn the degradation into a freeze. 1 means "there is nothing left to settle", which
is true. Every absent value and its justification is in `mh_net_module.h`.

The gate is **`check_module_bind.py --net-surface`** (a `lint_repo` row, source-only): the table and
`mh_net.def` agree in both directions, every declared `MH_Net_*` has a row, every `MH_Net_*` call in
`mh/` resolves to one, and the two `module_bind` TUs stay one-per-project so the shims and the real
bodies can never land in one image. Nine planted negatives, and a 20-row floor so a broken parse
cannot pass vacuously.

### The WS2_32 anchor — the subset rule staying true after the transport left

`mh.dll` imported `WS2_32` for exactly one reason: the transport was compiled into it. Moving the
transport out takes the import with it, and the subset rule then **breaks on the very first satellite
it was written for** — loading `mh_net.dll` from `DLL_PROCESS_ATTACH` would drag a fresh `ws2_32.dll`
in behind it and run its `DllMain` under our loader lock.

So `module_bind.cpp` keeps a deliberate one-instruction `htons()` call whose result is stored in a
`volatile`. It is a real call, not an address in a global, for the reason the measurement trap above
records. Measured: `mh.dll`'s `WS2_32` import is now a **single ordinal, and it is the anchor's**.

```
mh.dll       KERNEL32.DLL USER32.DLL WINMM.DLL WS2_32.DLL     (dumpbin /imports, 2026-09-13)
mh_net.dll   KERNEL32.DLL USER32.DLL WS2_32.DLL
```

`check_module_bind.py --subset` is what stops that anchor being deleted as dead code. It needs the
built PEs, so it is **not** a lint row (lint does not build) — `run_gate.py` runs it immediately
after the Release build, and aborts the gate on a break.

### The three boot shapes, measured

Manual console launches, stderr redirected, ~20 s each, on stock-exe lanes
(`workdir/mh_lanes/f4b_{bound,absent,none,orig}`):

| lane | first line of `mh_net.log` | alive | stderr | frames |
| --- | --- | --- | --- | --- |
| `f4b_bound` (plain lane) | `BOUND at DllMain … 23 exports resolved, init returned F4B00001, call-through ok; module DLL_PROCESS_ATTACH was AFTER mh.dll's by 7324 us` | yes | empty | 185,061 |
| `f4b_absent` (`--omit-satellite mh_net.dll`) | `NOT BOUND -- LoadLibrary(…\mh_net.dll) failed, Win32 error 126 (the file is not there)` | yes | empty | 185,076 |
| `f4b_none` (`[net] module=none`) | `NOT ATTEMPTED -- \`[net] module=none\` … mh.dll does not look for the module at all` | yes | empty | 185,004 |

All three reach the arm's end marker (`; [interlock] … detour install(s)`), and both no-module lanes
print the same `; ==== NO NETWORK MODULE ====` banner and the same seven-stub and browser-notice
lines. **`NOT ATTEMPTED` is a fifth bind outcome**, distinct from `NOT BOUND` on purpose: "the module
is not there" and "this run was told not to look" are different facts, and `--expect absent` refuses
a `declined` log (and vice versa) rather than treating one as a weaker pass for the other.

### What it cost the arm-order baselines

The pre-ruled edit, and exactly it: **one inserted structural step at template step #2** in all three
committed baselines, plus one ruled TEXT change in `brokered_nomodule`.

| baseline | `--compare` verdict (a pre-F4B run of the same mode → the F4B run) |
| --- | --- |
| `brokered` | 1 structural edit, 0 text changes, 67 → 68 lines |
| `original` | 1 structural edit, 0 text changes, 41 → 42 lines |
| `brokered_nomodule` | 1 structural edit, **1 text change**, 69 → 70 lines — in BOTH no-module arms |

The bind's outcome is **one** log line rather than two: the loader-order fact (the R2 measurement) is
folded into the `BOUND` line instead of printed separately, so the three arms are structurally
identical and cost one step each.

The text change is the banner losing its `([net] module=none)` parenthetical. From F4B two causes
reach that line and naming one of them in the other's run would be a log that lies; the cause is
stated by the `[modules]` line at the head of the same file. The baseline's step #1 carries an `alt`
so **both** no-module causes select `brokered_nomodule` — they arm identically, so they must.
`check_arm_order`'s `NOMODULE_RE` moved onto the `==== NO NETWORK MODULE` banner itself, which makes
the key *more* derived than before: a statement about what the arm did, not about a key somebody set.

Two tool changes rode with it, both general:
* the normalizer masks **Windows absolute paths** to `<PATH>` — the refusal line names the full path
  it tried (deliberately: msvfw32's Rule 2), and a lane's path contains the lane's name, which would
  have pinned the baseline to the folder it was recorded in.
* `--update` now **REFUSES** to re-record a baseline whose `optional` steps the recording run did not
  arm. This was live: F4B's own first re-record silently deleted F3G's two `[inmem]` entries, which
  makes the baseline *stricter* than the mechanism — the exact hazard `merge_annotations` was written
  to prevent, from the other side.

### Lanes, scenarios and the selftests

* `make_lane.py` now deploys `mh_net.dll` into **every** lane (`DEFAULT_SATELLITES`); the absent arm
  is an explicit `--omit-satellite mh_net.dll`, which refuses a name the lane would not have deployed
  anyway. `ui_test.py` REFRESHES a satellite a lane already has (never creates one, or it would
  re-deploy the file the absent lane was asked to omit) and DEPLOYS to VM peers fatally, like
  `mh.dll`; `mp_run.py` does the same for force-entry runs.
* **`module_absent` is retargeted, not folded into `no_net_boot`.** They reach the same degraded UI
  by different routes — `no_net_boot` DECLINES the load and gates its capture on the browser notice;
  `module_absent` has the file genuinely missing and gates the LOADER outcome. Folding them would
  delete the only test of the path a player can actually create.
* **The selftests keep compiling the transport TUs directly** (`net_selftest.exe` builds
  `mh_net/net_transport.cpp` and `net_key.cpp`). The suites test the transport, not the loader:
  `transporttest`, `netsessiontest`, `watchdogtest` and `netqueuetest` exercise sockets, framing,
  handshakes and queue policy, none of which is a statement about `GetProcAddress`, and routing them
  through a DLL boundary would make every one of them depend on a deployment step. The test binary
  gets `mh_nettest/module_bind_compiled_in.cpp` (one symbol, `MH_NetModule_IsBound() == 1`) instead of
  the shims — two TUs, each in exactly one project, so a build that took both fails to LINK.

---

## F4D-PRE — the edge in the OTHER direction

Everything above is about `mh.dll` reaching a satellite. F4D also needs the reverse to be finite:
`libmh.dll` is 627 TUs and 94.6 % of today's `mh.dll`, and a spine that calls back into its host is
a spine that cannot ship without it. F4D-PRE measured that edge and closed it.

### What it actually was

Measured with `dumpbin -symbols` over the **hosted** build of the 627-TU roster
(`src/mh_dll/mh/Debug` — `mh.vcxproj`'s compilation of the same files *without* `MH_LIBMH_BUILD`),
unresolved externals minus everything the roster defines itself. Non-toolchain result, at the item's
own HEAD:

| symbol | roster TUs that want it |
| --- | --- |
| `mh::hook::install_export_ok` | **22** |
| `mh::hook::entry_owner_of` | 6 |
| `mh::hook::install_trampoline` | 1 |
| `MH_Harness_RebindLandPlayers` | 1 |
| `MH_Harness_WantsWallclockPin` | 1 |
| `mh::lzw::compress` / `mh::lzw::decompress` / `mh::lzss::decompress_block` | 1–2 (`mh_common`, not `mh.dll`) |

**Read the archive instead and you measure zero.** `libmh.lib` is built WITH `MH_LIBMH_BUILD` — the
LIB-REF standalone arm — in which all five rows are compiled out by construction. The archive is the
artifact for a host with no game image; F4D's `libmh.dll` is a HOSTED build of the same roster, and
that is the build whose edge decides whether the split is possible. Measuring the wrong one says
"already closed" about an edge that is open.

**And the biggest row is invisible to any source scan.** Not one of the 22 TUs *names*
`install_export_ok`: it arrives through `MH_EXPORT_REPLACE` (`addr/mh_export.gen.h`). A
`grep`-shaped gate would have called the edge closed while it was open, which is why the gate below
reads objects and only *also* reads source.

### The shape it became — F4B's table, arrow reversed

`libmh/include/libmh_hook.h`: five rows, one X-macro, expanded three ways (the struct, the
unbound-walk's name table, the offline gate's parse); `libmh_set_hook_api(&table, VERSION)` with the
same handshake and the same walk as `libmh_host_api`. `libmh/state/hook_api.{h,cpp}` is libmh's side,
`mh/seams/libmh_hook_host.cpp` is `mh.dll`'s five forwarders, and `MH_Core_Arm_Early` binds it
beside the two host-api binds it already did — ahead of `MH_Harness_Init` and therefore ahead of
every `[promote]` installer (the same G104 earliest-common-point argument, one level over).

Two things differ from every other table in the tree, both deliberate:

* **Absence is a CONFIGURATION, not a boot-order bug.** `mh::host()` aborts unbound because a module
  that cannot reach its host callbacks cannot run; a module that cannot reach the *injection*
  harness simply is not injected, which is what a standalone libmh always is. So every row documents
  the answer it gives unbound — and every one of those is the value the eight per-TU
  `#ifdef MH_LIBMH_BUILD` stubs it replaced already gave, which is what makes the change
  behaviour-neutral by construction rather than by re-measurement. `MH_Net_IdAssigned` has the same
  shape one table over: an absent answer chosen so the degradation is not a freeze.
* **The per-TU guard is now a rule violation, not an accepted arrangement.** Until this item,
  `lint_libmh_layering` said "a harness include must be GUARDED"; eight TUs complied and each wrote
  its own `#else` stub. A guard hides the edge from a link-level reader while leaving it in the
  hosted build. `check_libmh_outbound.py` therefore fails a guarded harness include too, and the two
  lints are siblings rather than duplicates.

**What F4D flips, and what it does not.** Today libmh is compiled into `mh.dll`, so the bind is an
ordinary same-image call. At F4D `libmh_set_hook_api` becomes one more row of the export contract:
`GetProcAddress` it and call it with the same struct. **Not one call site in the 627-TU roster
changes**, because no call site names the host — that property is the whole reason this landed
before F4D rather than inside it.

### Q5 — the marshalling thunks are per-image now

`addr/mh_calls.gen.cpp` (2625 generated naked thunks that call INTO the original at fixed VAs) left
`libmh.vcxproj`'s roster: it is host machinery, not spine, and carrying it in the ARCHIVE made its
`mh::call::detail::s_*` shapes read as part of libmh's export contract. **Measured: the
`mh.dll`→libmh contract drops 121 → 99, i.e. 22 rows, not the 19 the F4 surface pass predicted.**
The thunks are not gone — whichever image compiles the roster compiles the shim too
(`mh.vcxproj`, `mh_nettest.vcxproj`, and now `libref_host.vcxproj`, whose `/WHOLEARCHIVE` makes a
forgotten row a link error rather than a silent gap). Same "both images link the copy they use"
arrangement F4B set for `mh_net_proto`'s `session_info.cpp`. **F4D's own `libmh.dll` project will
need that row too** — its bodies still reach the original through those shapes.

### The gate

`tools/check_libmh_outbound.py`, two mechanisms, neither sufficient alone:

| | reads | sees | blind to | where it runs |
| --- | --- | --- | --- | --- |
| SRC | the include CLOSURE from the 627 TUs (1283 files), comment-stripped | any reach into `hook/`, `seams/`, `effects/`, `shadow/` or `include/mh_*.h`, **guarded or not**, plus `mh::hook::` / `MH_Harness_` tokens | anything a MACRO introduces | `lint_repo` (`--src-only`; lint does not build) |
| OBJ | `dumpbin -symbols` over the hosted roster objects | every unresolved external, macro-introduced ones included; prints THE NUMBER | header-inlines, folded code, a stale object (mtimes are compared and reported) | the item's proof, and any session touching the roster |

Unresolved externals are classified, and an unclassified *project* symbol fails: `toolchain`
(derived from the decorated name, never a list), `shim` (Q5's per-image thunks), `common`
(`mh_common`'s codecs), `host` — the residue, which must be empty. Three positive controls must be
seen or the run exits 2 rather than reporting a silent zero; the re-pick rule for them is in the
tool, and it is the F3C/F3D lesson (an anchor that dies with the change it watches turns a correct
result into a misleading red).

One trap it caught in its own item, worth carrying: routing `entry_owner_of` through an
**out-of-line** accessor made `sim_order_dispatch.cpp` materialise `0x00466892` as an immediate to
pass it, and LIB-VA0's byte ratchet (`scan_libmh_vas.py`) reported a new original VA in the archive.
The standalone arm of `state/hook_api.h` is inline constants for exactly that reason — the eight
stubs it replaced were each inline without saying so.

---

## F4D — `libmh.dll`, the spine

The 627-TU roster ships as its own DLL and **mh.dll stops compiling it**. That is the whole of ruling
Q10, ratified: there is ONE spine-less `mh.dll`, byte-identical whichever configuration it runs in,
and the configuration is expressed by which files sit beside it.

Measured: the same `sha256` deployed to both lanes, `libmh.dll` present in one and not the other.

| | Release size |
| --- | --- |
| `mh.dll` (spine-less) | 545 KB |
| `libmh.dll` | 1.50 MB |
| `mh_net.dll` | 402 KB |

### The contract is DERIVED, not written

`mh.vcxproj` is **666** TUs, of which **627** are the roster and **39** are mh.dll's own (R9: the
F4-pass figure was 663, and mh/Debug additionally held 8 F2-era orphans that put two phantom rows in
F4D-PRE's own count — both cleared here). Those 39 name **101** of the spine's symbols, and every one
has to resolve at runtime.

`tools/gen_libmh_contract.py` derives the list as an intersection of two object-level facts:

    { external symbols UNDEFINED in mh.dll's objects } & { DEFINED in libmh.dll's objects }

which is what an export contract *is*. It cannot drift from either side, and it sees what no source
scan can — the same blindness F4D-PRE measured in the other direction. **97 rows the day it was
first derived, 100 after the process-wide singletons joined, 101 after the `[hookapi]` report line
added a caller.** That last one is the mechanism working: adding a call to a spine symbol adds a row,
and the build fails until the generator is re-run.

It emits four things: the committed row list (`tools/data/libmh_contract.json`), `libmh.def`, and
mh.dll's `seams/libmh_contract.gen.{h,cpp}`. `--check` re-renders the three from the committed list
(a lint row, no build); `--rederive --check` re-measures from the objects (a `run_gate` row, straight
after the build, beside `--subset`).

### A symbol with no signature still gets a definition

The obvious shape — 101 hand-written C++ forwarders — needs 101 exact signatures, i.e. 101 chances
to transcribe one subtly wrong. So the shims carry no types at all:

```cpp
extern "C" __declspec(naked) void mh_libmh_thunk_N(void) {
    __asm { mov eax, [g_libmh_fn + N*4] ; test eax,eax ; jz absent
            inc [g_libmh_crossings]     ; jmp eax
      absent: inc [g_libmh_absent_calls] ; xor eax,eax ; ret }
}
#pragma comment(linker, "/alternatename:?mangled@...=_mh_libmh_thunk_N")
```

A `jmp` through the slot is a perfect `__cdecl` tail call — the arguments are already on the caller's
stack, the caller cleans them, the return value comes back in EAX/EDX/ST0 untouched — so the thunk
never needs the arity, the types or the return class. `/alternatename` attaches it to the mangled
symbol at LINK time, which is also why the mangled name stays UNDEFINED in the object and the
derivation above keeps measuring the same set after the split. Proven under mh.dll's own Release
flags (`/O2 /GL` + `/LTCG`) before the generator was written.

**`xor eax,eax` is not a convenience here, it is the definition of configuration (1).** Read down the
contract and every row is an installer (`install_promotion*` → 0, "not installed", so the original
entry stays), a predicate about an install, an observer registration, an entry thunk (→ nullptr, so
the installer refuses), or harness instrumentation. The three rows that reach live gameplay
(`mh::tact::mission_start`, `group_issue_order`, `unit_enqueue_command`) are reached only from the
harness's own force-entry verbs; the shipping route into tactical mode is an original body in
configuration (1) because nothing promoted it.

**Eight rows cannot be a naked thunk and the generator REFUSES rather than guesses.** Four are
MECHANICAL, derived from the demangled return type — a reference return would hand the caller a null
to dereference, a by-value struct returns through a hidden pointer the stub leaves unfilled. Four are
SEMANTIC: zero is a legal value of the right type and the wrong answer. All eight are hand-written in
`mh/seams/libmh_bind.cpp`, which includes the real headers, so the compiler checks each signature.

### The finding this item did not go looking for

**Three header-only mutable singletons were about to be silently duplicated.** `mh::state::live()`
(the region registry), `owner_table()` and `owner_count()` were unconditional magic statics — exactly
right while the spine and the injection layer are ONE image, because the linker folds the COMDAT and
every reader shares the object. Two images make that false:

* `mh::ai::island_move()` rebases 48 regions from inside libmh;
* mh.dll's seams read `live_base()` for dozens (`harness`, `desync_watch`, `net_lockstep`, `launch`,
  `ui_drive`, `gfx_overlay`, `net_diag`, `net_seams` — 9 TUs define the static today);
* `claim()` is called from roster TUs (48 + 2 regions) and `owner_of()` / `owner_serves()` are read
  from `desync_watch.cpp` and `harness.cpp`.

With two tables the rebases land in one and the reads come from the other, the determinism hash walks
the abandoned addresses, and **nothing goes red** — not the arm log, not the UI suite, not even a
two-peer determinism run, because both peers would be wrong identically.

The fix: the image that CONTAINS the spine owns the storage (`MH_SPINE_IN_IMAGE`, set by every
project that compiles the roster and by none that does not), and any other image declares the
accessor and reaches it across the boundary like every other row. It needs no gate of its own — a
roster-compiling project missing the define fails to LINK, and `mh.vcxproj` acquiring it fails to
link too, because its forwarding definition would be a duplicate. `libmh/state/spine_exports.cpp` takes
the three addresses so the linker is obliged to emit an out-of-line body the `.def` can export;
without it the Release build inlined `owner_table`/`owner_count` at every call site and the link
failed with `LNK2001` on exactly those two (Debug, at `/Od`, had emitted and exported them fine).

### THE STANDING ARM — the crossing witness

The recorded miss F4D exists to close is not the boundary. **LIB-SPINE-API proved the spine crossing
live once** (13e7e4ec, 86 routed rows, the full battery through the brokered entry) and then nothing
ever asserted it again. After the split, a brokered lane that binds `libmh.dll` and never calls
through it is indistinguishable from one that crossed four hundred times: the bind line is identical,
the boot completes, the menu renders, the capture matches.

So the thunks count. `crossings` on the bound path, `absent_calls` on the other, reported once from
`on_present` — off the loader lock, after the whole arm has run, and AFTER the window
`check_arm_order` gates, so the report costs no baseline edit in any configuration:

```
; [libmh] crossings=13465 absent-calls=0   -- the spine boundary was ENTERED (configuration (2))
; [libmh] crossings=0 absent-calls=796     -- NOT ENTERED (configuration (1))
```

`check_module_bind.py --libmh` requires the bind verdict AND the counters to agree with it, in both
directions. The absent arm is not decoration: the same numbers in both configurations would mean the
instrument is not reading anything. It is carried by two registered scenarios — `menu_walk`
(`--expect bound`) and the new `libmh_absent` (`--expect absent`, plus `--expect bound` for mh_net so
it cannot pass on a lane that lost the transport too).

**Proven able to red, live.** A lane with `mh_net.dll` planted under the name `libmh.dll`
(`workdir/mh_lanes/f4d_wrong`) loads it, refuses it by name (`0 of 101 contract symbols`), and runs
240k frames in configuration (1) with every other gate green — and the arm reds, with or without
`--expect`, because `LOADED BUT REFUSED` is a broken deployment rather than a configuration.

### The four boot shapes, measured

Manual console launches, stderr redirected, ~22 s each, on stock-exe lanes (`workdir/mh_lanes/f4d_*`):

| lane | libmh outcome | promotions live | crossings | alive | stderr | frames |
| --- | --- | --- | --- | --- | --- | --- |
| `f4d_bound` | `BOUND … 101 exports resolved, init returned F4D00001, call-through ok; attach AFTER mh.dll's by 12783 us` | 396 | 13465 / 0 | yes | empty | 272,448 |
| `f4d_absent` (`--omit-satellite libmh.dll`) | `NOT BOUND … error 126 … This run is CONFIGURATION (1)` | 1 | 0 / 796 | yes | empty | 273,018 |
| `f4d_orig` (`[config] mode=original`) | `BOUND` | — | — | yes | empty | — |
| `f4d_wrong` (mh_net.dll planted as libmh.dll) | `LOADED BUT REFUSED — 0 of 101` | 1 | 0 / 796 | yes | empty | 240,603 |

The one promotion live in configuration (1) is mh.dll's OWN (`reimpl_probe.cpp`'s `utils_w_strlen`),
which is why `mh::hosthook::install_export_ok` is a semantic hand row: every `MH_EXPORT_REPLACE` site
routes through that accessor, including mh.dll's two, and a generated zero would have refused installs
that have nothing to do with the spine.

### The subset rule, and two static CRTs

`libmh.dll` imports **KERNEL32 and USER32 and nothing else** against mh.dll's four, so the rule holds
with room to spare — but only because the Release build pins the **static CRT**. A dynamic one would
put `VCRUNTIME140` and `MSVCP140` in that table and the loader would initialise two fresh DLLs under
a lock we hold. `SUBSET_PAIRS` is one line per satellite; the row is in.

Two static CRTs means **two heaps**, and the contract was reviewed for it: nothing crosses owning
memory. The rows pass PODs and function pointers, the two `const char *` returns
(`mh::save::last_save_path`, `mh::libmh_in::last_trap`) point into static storage, and the spine's
file I/O is already the host's — libmh asks mh.dll to open through the host-api `vfs_open` row and
hands the handle back to the side that made it (SIMABI-VFS). That property was designed in long
before this item; it is recorded here so a future row that breaks it is recognised as breaking
something.

### What it cost the arm-order baselines, and the fifth baseline

Two INSERTED structural steps in every committed template, **0 text changes**, both ruled:

| baseline | `--compare` verdict (the pre-F4D run of the same mode → the F4D run) |
| --- | --- |
| `brokered` | 2 structural edits, 0 text changes, 68 → 70 lines |
| `original` | 2 structural edits, 0 text changes, 42 → 44 lines |
| `brokered_nomodule` | 2 structural edits, 0 text changes, 70 → 72 lines |

The first is the libmh bind line at the head of the window — the second member of the class F4A
pre-ruled at ruling (b). The second is the `[hookapi]` report, which F4D-PRE built the table for and
deliberately did not report because its own `done_when` said the baselines were untouchable. It
carries `spine=bound` / `spine=absent (configuration 1)` for a reason worth stating: with no
libmh.dll the generated thunks answer zero, so `rc=0 unbound=0` would read as a healthy bind of a
table that does not exist.

**`config1.json` is the FIFTH baseline**, recorded from the first configuration-(1) boot that has ever
happened. It is NOT a variant of `original` — that mode HAS the spine and declines to promote from
it, so its arm still emits the `[rebind]` block and the `[promote]` domain reports (44 steps against
configuration (1)'s 40). And it could not be keyed the usual way: `[config mode=…]` is printed by
`mh::rebind::report_arming`, a **libmh row**, so the line that names the mode is one of the lines the
missing module takes with it. `CONFIG1_RE` anchors on the bind's own outcome line instead — the same
move F4B made when it re-pointed `NOMODULE_RE` onto the banner.

### Tools that had to follow the objects

Three read the roster's objects, and all three were pointed at `mh/Debug`, which now holds none:

* `check_libmh_outbound.py`'s OBJ arm → `libmh_dll/Debug`. The **configuration** is what its argument
  is about (hosted, no `MH_LIBMH_BUILD`), not the project; this is now the artifact that ships.
* `gen_libmh_contract.py` reads both images'.
* `check_net_lockstep_refs.py` looks for a closure object in the chosen objdir and then in the spine's
  trees. Its missing-object path only appends a NOTE, so left alone the split would have turned a
  load-bearing arm into a run that reported "closure object missing" for every file and still printed
  OK. It also needed a closure-OWNERSHIP filter: `mh::state::live()` is now undefined in the seam
  objects and its COMDAT is emitted by two closure objects among a dozen others, which the tool read
  as a reach INTO the closure and exited 2 over.

`make_lane.DEFAULT_SATELLITES` is now the one place the satellite set is written — `ui_test` and
`mp_run` derive from it. F4B shipped three hand-kept copies and said to unify them "if cheap"; a
second satellite is where three copies stop being a duplicate and start being a drift, and the drift
would not announce itself (a peer without `libmh.dll` boots, plays, and passes most things).

`lint_libmh_layering.py` was **narrowed, not retired** (F4D-PRE deferred the ruling here). Its
harness-include half is strictly subsumed by `check_libmh_outbound`'s SRC arm, which reds a harness
include *guarded or not* — so keeping it would have shipped a gate whose pass message states a rule
that is no longer true. Its second claim is not about layering at all and nothing else makes it: no
libmh module may name `libref_host/host_stock_bases.gen.h`, the HOST-ONLY original-address table.

---

## F4E — `mh_harness.dll`, the instrument

`seams/harness.cpp` (8,313 lines, mh.dll's single largest file) ships as its own DLL, and **mh.dll
stops compiling it**. What is left behind is the ~185 lines F3B already named as core and could not
move: `MH_Core_Arm_Early`, `build_paths`, `harness_enabled` and the relocating state bind, now
`mh/seams/core_arm.cpp`. F3B drew the line; this item made it a file boundary.

| | Release size | exports |
| --- | --- | --- |
| `mh.dll` (spine-less, harness-less) | 458 KB | 30 (27 host rows + the 3 original `__declspec` exports) |
| `libmh.dll` | 1.43 MB | 103 (81 mh.dll contract rows + 20 instrument-only + 2 module entries) |
| `mh_net.dll` | 392 KB | 25 |
| `mh_harness.dll` | 254 KB | 14 (12 contract rows + 2 module entries) |

`mh.dll` was 545 KB at F4D and is 458 KB now: the instrument was ~16 % of what was left of it. `mh.vcxproj` is 42
TUs (41 minus `harness.cpp`, plus `core_arm.cpp` and `harness_bind.cpp`); `mh_harness.vcxproj` is 4
(the instrument, the per-image `mh_calls.gen.cpp` shim, its DllMain and its generated contract).

### The three edges, all derived

`harness.cpp` reaches in three directions and the split turns every one into a runtime name.
`tools/gen_harness_contract.py` derives all three from objects, exactly as `gen_libmh_contract.py`
does and for the same reason — a source scan cannot see a symbol a macro introduced (F4D-PRE
measured a 22-TU edge no TU named):

| edge | rows | how |
| --- | --- | --- |
| mh.dll → harness | **12** | hand-listed WITH their absent values in `mh/include/mh_harness_module.h` (F4B's X-macro shape), cross-checked against the derivation |
| harness → mh.dll | **27** | `GetModuleHandleA("mh.dll")` + `GetProcAddress`; mh.dll exports them through a generated `mh.def` |
| harness → libmh.dll | **30** | `GetModuleHandleA("libmh.dll")` + `GetProcAddress`; `libmh.def` gained the 20 that are not already mh.dll's |

**Twelve, not thirteen, and the difference is the file cut.** `MH_Harness_ReportRelocation` reports
the relocating state bind, whose arena and report live in `MH_Core_Arm_Early`; it moved to
`core_arm.cpp` with them and stopped crossing the boundary. Two more tactical rebinds were not rows
either — F2E deleted the tactical promotions and with them every caller, leaving two dead functions
in a live file; **fork F5H deleted them on 2026-09-14** (R9: the surface pass predicted 12 and named
11 of them; the derivation found 12, a different 12).

**Thirty, not the ~41 Q4 predicted**, and again the cut is why: nine of the spine symbols the
measurement counted (`libmh_set_host_api`, `libmh_set_tact_host_api`, `libmh_in_open`,
`mh::state::bind_relocated` / `bind_stock`, `mh::rebind::arm_from_config`, the two `hostapi` tables,
`MH_LibMH_BindHookApi`) were `MH_Core_Arm_Early`'s, not the instrument's.

**Both harness tables use signature-free naked thunks** attached by `/alternatename`, so not one of
harness.cpp's several hundred call sites changed. The one thing they do differently from
`gen_libmh_contract`'s is the absent path: those answer `xor eax,eax`, because for mh.dll that IS
configuration (1). Here it would be a lie — the harness refuses to arm unless both tables resolved
whole, so a null slot means an ARMED instrument has found its own table incomplete, and an
instrument that invents numbers is the one failure a determinism harness may not have. The absent
path records the row index and jumps to a noreturn trap that names the row on three channels and
kills the process. Unreachable by construction; loud if construction was wrong.

### Ruling Q4 — the loud refusal, and the second key that decides how loud

Q4 asks the harness to late-bind its libmh rows with **a hard loud refusal when libmh.dll is
absent**. It does, on `mh::config::detail::refuse`'s three channels (`mh_harness_refused.log` beside
the exe, `OutputDebugString`, stderr) — and **without** its `TerminateProcess`. The F2E scoping rule
is what decides that: a misspelled `[config] mode` terminates because it is an author believing
something false about *which bodies run*; this is not that. The instrument installs nothing, so its
absence cannot change which bodies run — it changes whether the run is MEASURED, and that absence is
not quiet either way, because `mh_harness.log` is never written and every consumer of it
(`mp_analyze`, `--ui-abc`, `--sp-determinism`, `check_arm_order`'s harness channel) reds on a file
that is not there. Killing the process would turn a stray ini key into a dead game for no evidence
gained.

**It is shouted only when somebody asked, and that took a measurement to get right.** The first
armed-lane boot after the split printed 819 bytes of refusal on a lane whose harness was DISARMED:
the module was refusing a dependency nobody wanted. Configuration (1) is a *shipped* configuration —
a player with no `libmh.dll` and no `[harness] enable=1` is having an ordinary evening, and writing a
refusal log into their game folder would be this project shouting at somebody who asked for nothing.
So `MH_HarnessModuleHost` carries `configured` (`[harness] enable=1`, read by mh.dll) beside
`spine_bound`, and the refusal is RECORDED always, SHOUTED only when the two disagree with what the
operator asked for.

**AMENDED BY mp:D29 (2026-09-24): the refusal now applies only when the configuration (1) FALLBACK
fails.** Q4 assumed every spine row the harness reads is one with no honest zero-spine answer. D29
measured that the per-step region hash reaches exactly **three** of the 33 — `mh::state::live()`,
`owner_table()`, `owner_count()` (`hash_slice` → `emit_slice` → `owner_serves`; `hash_base` →
`live_base`) — and those three DO have one, which mh.dll already computes: its own region table
seeded from `REGIONS[]` and an empty owner table (`seams/libmh_bind.cpp`, "not a fallback but the
correct answer"). So:

* **the fallback rows** are a third row class in `tools/gen_harness_contract.py` (`CONFIG1_FALLBACK`).
  With the spine present they stay ordinary SPINE rows — the libmh build is unchanged and G179's
  one-registry rule holds. With **no libmh.dll**, the generated `mh_harness_bind_config1` binds them
  **out of mh.dll** (which now exports them through the generated `mh.def`) into their spine slots,
  all or nothing, and the module arms the instrument **spine-free**. It binds mh.dll's REAL table —
  the export IS the function mh.dll's own readers call — never a harness-private copy, which would
  stay green the day mh.dll moved a region (net_selftest `bindtest`'s planted-copy arm).
* **every other spine slot stays null**, and every harness path that could reach one is either
  **guarded** (it has an honest spine-free answer: `rng_trace_set_step`, the order-count ledger note,
  the step-1 inbound census — which keeps check_arm_order's end-marker prefix and says `n/a, no
  libmh` —, the clause-6 rebind yield — `set_armed` would trap, and there are no rebind rows to
  yield —, `pin_strat_seed`'s libmh seed push, `replay_suppress_enqueue`'s libmh sink gate, rdump's
  RNGD/NOTE flush, a UI journal's order records) or its `[harness]` key is **refused by name**
  (`mh_harness/config1.h` `SPINE_ONLY_KEYS`: the boot/world snapshot, save/load, tactical
  synth/journal, `skip_pace_hook`, `rng_trace`, `skip_input_update` and `pin_menu_clock` keys) — in
  mh_harness.log, OutputDebugString and stderr, and the key is switched off while the hash keeps
  running. Not `mh_harness_refused.log`: that file's existence means "not instrumented".
* **the refusal above survives, narrowed**: with no libmh.dll AND an mh.dll that does not export the
  three rows (an older mh.dll beside a newer mh_harness.dll), the module refuses exactly as before.
* **regions the spine-free hash cannot cover: none.** In configuration (1) nothing is relocated and
  nothing is owned, so all 62 manifest slices sit at their fixed mh.exe VAs. The header states it
  per run: `; [harness] configuration (1): spine ABSENT, registry=mh.dll, rebased=0 owned=0,
  uncovered=none, manifest fp=XXXXXXXX` (written after the step-1 census, outside the arm window;
  `uncovered` counts slices whose region has no address). The hash DEFINITION is the same inline
  code as the spine path (`state/region_view.h`), so a MIXED pair — configuration (1) vs
  `mode=original` — is comparable. The manifest fingerprint is now a compile-time constant
  (`HASH_MANIFEST_FP`, region_view.h; libmh's `world::hash_manifest_fingerprint()` returns it), so
  both configurations print it, and `tools/mp_analyze.py` REFUSES a pair whose fingerprints differ.

The mirror case is mh.dll's: `[harness] enable=1` with **no mh_harness.dll at all**. That one is
`seams/harness_bind.cpp`'s, same three channels, same non-fatal continue, and it exists because G178
is the recorded cost of the opposite — a determinism run whose instrument silently never installed
produced a log nobody could tell from a healthy one.

### What the instrument's absence means, and why the twelve shims are silent

Three satellites, three meanings of "not there":

* **no `mh_net.dll`** — a DEGRADATION the player can see.
* **no `libmh.dll`** — CONFIGURATION (1): the game runs the original binary's own bodies.
* **no `mh_harness.dll`** — UNINSTRUMENTED, and *nothing else*. The game is bit-for-bit the game it
  always was, because the harness only ever observes and it ships disarmed.

So mh.dll defines all twelve `MH_Harness_*` symbols as forwarding shims over the bound table
(`seams/harness_bind.cpp`), each answering exactly what the un-armed body already answered — `0` for
the four `Rebind*` ("nobody owns the entry, an ordinary entry install is the correct route"), `0` for
the three `Wants*`, a no-op for the four tick/arm entries. An absent instrument and a disarmed one
are indistinguishable to every caller, which is the property all of those callers were already
written against. Every call site is unchanged, byte for byte.

### The `[harness]` key that was not the harness's

`seams/reimpl_probe.cpp` read `[harness] st0_goldens` — the ONE `[harness]` read outside the
instrument, measured tree-wide. After the split that is mh.dll reading another module's
configuration section: a key whose name says which DLL owns it, in the file two DLLs read, meaning
nothing to the one it names. It is `[st0] goldens` now, the section this probe already writes its
output under (ruling Q3: the thirteen oracle seams stay in mh.dll, each self-gated on its own ini
section), and a surviving `[harness] st0_goldens=1` is **refused by name**, F2E's rule applied at key
scope — that fragment is an author who believes a measurement will run, and the only other symptom
would be `gen_st0_goldens.py` finding no sweep in a log nobody knew was wrong.

### The two things that had to move because two images now compile the same header

* **The run paths.** `build_paths()` composes one ini path and eight per-run output paths; harness.cpp
  uses those nine names ~350 times. mh.dll composes them (ruling Q1: exactly one place in the process
  knows where this run's logs live) and hands them over as `MH_Core_ArmPaths()`, a borrowed const
  struct the harness COPIES into its own arrays at `MH_Harness_Init` — one composition, zero call-site
  churn, and nothing crossing owning memory between two static CRTs.
* **`MH_SPINE_IN_IMAGE` is NOT defined for `mh_harness.vcxproj`**, and its absence is load-bearing
  (G179). The project compiles no roster code, so `mh::state::live()`, `owner_table()` and
  `owner_count()` must not be magic statics here: it declares them and reaches libmh's across the
  boundary, like mh.vcxproj does. Defining it would give the instrument its own region registry — the
  rebases landing in libmh's copy and the harness's hash walking its own — and NOTHING would go red.

`harness.cpp` also keeps its own `harness_enabled()`, asking the same ini the same way mh.dll does.
That duplication is deliberate: mh.dll's core arm has to know whether an instrument will run (the
relocating bind and the rebind yield both turn on it) and the instrument has to know whether to arm.
The alternative — a host row carrying a bool — would make the harness's own arm gate depend on a bind
having succeeded.

### Where the file lives, and where the gate looks

`harness.cpp` lives at **`mh_harness/harness.cpp`** since fork **F5O** (2026-09-16), which reversed
F4E's ruling that it stay in mh.dll's `seams/` directory and be compiled from there. F5O's rule is
one directory per owning target, and `mh_harness.vcxproj` is the only project that OWNS this file
(`mh_nettest` links it into the offline image as a second consumer, exactly as it consumes the
roster). F4E's two objections were paid off rather than argued away: the forty-odd include lines
still resolve untouched, because the project's include path gained `$(SolutionDir)libmh` beside the
`$(SolutionDir)mh` it already had; and `check_fork_d5_hooks.py`, which names harness-owned files BY
PATH precisely so its scope cannot silently empty, was repointed in the same commit and still
refuses to run when the named file is absent.

That gate's set follows the MODULE rather than the directory, and after F5O the two agree, so ONE
glob covers it: everything in `mh_harness/`. What it must still not reach is
`mh/seams/harness_bind.cpp` — mh.dll's side of the boundary, no more harness-owned than
`module_bind.cpp` is mh_net-owned. It reports 3 files, 0 raw-primitive callers, 23 arms through the
named hook points — unchanged across both the split and the move, which is what D5 was built for.

`net_selftest.exe` keeps compiling BOTH halves directly (`harness.cpp` + `core_arm.cpp`) and **not**
`harness_bind.cpp`: taking the shims as well would be a duplicate definition of every row, i.e. a
LINK error rather than a silent second answer. Same arrangement as `module_bind_compiled_in.cpp` one
satellite over. The one thing the module tells the instrument — "your tables did not resolve" —
crosses as a setter (`MH_Harness_SetModuleRefused`), so harness.cpp includes none of the module's
plumbing and the offline image behaves exactly as it always did.

### The export list that was derived from the wrong question

`libmh.def` was the intersection `{mh.dll needs} ∩ {libmh defines}`, which was the whole story while
mh.dll was the only image reaching the spine. The first armed boot after the split printed

```
mh_harness.dll REFUSES TO ARM: libmh.dll is bound but does not export every spine symbol the
harness reads.  Resolved 10 of 30 libmh.dll spine symbols.
```

— the refusal working, and the export list being wrong: only 10 of the instrument's 30 rows overlap
mh.dll's. `gen_libmh_contract.py` now emits the UNION (103 names) while the SLOT TABLE stays
mh.dll's 81, because mh.dll has neither a caller nor a thunk for the twenty that are only the
instrument's. **Worth carrying: the mh.dll contract SHRANK 101 → 81 across this split**, because 20
of its rows existed only for harness.cpp; two of the eight hand rows (`mh::sim::rng_trace_at`,
`rng_trace_note_at`) stopped being rows with them and were deleted rather than kept "in case".

### The four boot shapes, measured (plus the one that is not a shape)

Manual console launches, stderr redirected, 22 s each, on stock-exe lanes (`workdir/mh_lanes/f4e_*`):

| lane | `[harness] enable` | harness outcome | stderr | alive | frames |
| --- | --- | --- | --- | --- | --- |
| `f4e_bound` | absent | `BOUND … 12 exports resolved, init returned F4E00001, host=27/27 spine=30/30` | **empty** | yes | 202,430 |
| `f4e_absent` (`--omit-satellite mh_harness.dll`) | absent | `NOT BOUND … error 126 … This run is UNINSTRUMENTED` | **empty** | yes | 197,139 |
| `f4e_armed` | `=1` | `BOUND … host=27/27 spine=30/30` | **empty** | yes | 194,548 |
| `f4e_absent_armed` (`--omit-satellite mh_harness.dll`) | `=1` | `NOT BOUND … \`[harness] enable=1\` IS SET, so this run asked for an instrument and will not get one` | 701 B, named | yes | 197,614 |
| `f4e_nospine_armed` (`--omit-satellite libmh.dll`) | `=1` | `BOUND … host=27/27 **spine=0/30**` | 754 B, named | yes | 201,549 |

All five reach the arm's end marker. The last two are the two halves of Q4's refusal — an instrument
that is not there, and an instrument whose dependency is not there — and they are the reason this is
a CONSOLE-LAUNCH table and not a scenario: a green scenario proves the process survived and says
nothing about what it wrote (R2).

`spine=0/30` in the last row is the ruling stated in numbers: configuration (1) with the instrument
DECLINING to arm, rather than an instrument reading zeroes and reporting them as state.

**The configured-but-absent shape costs the arm log one WORD, not one line.** The first draft wrote a
second `; [modules] mh_harness: CONFIGURED BUT ABSENT` line into `mh_net.log`; measured, that is a
structural step present in one boot shape and absent from the others, i.e. an unbaselined arm log for
a configuration that is an operator's mistake — the worst thing to make `check_arm_order` argue
about. The sentence is folded into the one line the bind already writes, so the difference is TEXT
and it is carried as a second `alt`. The shouting is unaffected: three channels, unchanged.

### What it cost the arm-order baselines

One INSERTED structural step in every committed template, **0 text changes**, the third member of the
class F4A pre-ruled at ruling (b):

| baseline | `--compare` verdict (the pre-F4E run of the same mode → the F4E run) |
| --- | --- |
| `brokered` | 1 structural edit, 0 text changes, 70 → 71 lines |
| `original` | 1 structural edit, 0 text changes, 44 → 45 lines |
| `brokered_nomodule` | 1 structural edit, 0 text changes, 72 → 73 lines |
| `config1` | 1 structural edit, 0 text changes, 40 → 41 lines |

`brokered` was **hand-merged rather than re-recorded**, because `--update` correctly REFUSED: this
menu-only recording does not arm F3G's two optional `[inmem]` steps, and re-recording would have made
the baseline stricter than the mechanism. That refusal is F4B's own tool fix doing its job.

Two **`alt`** texts on that same step, and neither is a fifth template — absence of the
instrument is not a different ARM, it is the same twelve shims answering what the un-armed bodies
already answered, so the step is the same step read three ways: BOUND, NOT BOUND, and NOT BOUND with
the configured-but-absent sentence. The registered `harness_absent` scenario reads the second;
`workdir/mh_lanes/f4e_absent_armed` is where the third was measured.

**The bind goes LAST of the three, and that ordering IS a contract** (the only one among them that
is): the instrument is handed libmh's bind verdict as an argument to its init, so it cannot be bound
before libmh has been. `mh.c` says so in the comment rather than leaving it to be tidied.

---

## F4G — configuration (3), and the second libmh.dll

Everything above is about `mh.exe`'s process. This section is about the one that has no `mh.exe` in
it at all: **configuration (3), the standalone reference** — "libmh + a host, no game binary"
(recorded in the fork plan). It had no gate. `tools/fixture_replay.py` was invoked by no tool and
no unit (its `replay` verb drives the GAME), `run_gate`'s units neither built nor ran `libref_host`,
and F2's "the standalone replay fixtures still verify 5000/5000" was a hand check.

### The finding: the libmh.dll that ships cannot replay standalone

The tracker asked for `libref_host` to be "re-pointed from libmh.lib" to `libmh.dll`. Measured, that
is not implementable against the file F4D shipped, and the reason is one macro:

```cpp
// crt/crt_select.h
#ifdef MH_LIBMH_BUILD
#define MH_CRT(fn) ::mh::crt::fn     // the VENDORED CRT (crt_heap.h, crt_sprintf.h, …)
#else
#define MH_CRT(fn) ::mh::call::fn    // the ORIGINAL BINARY's Watcom CRT, at a fixed VA
#endif
```

`libmh_dll.vcxproj` deliberately does **not** define it — that is what makes it the HOSTED arm, the
build whose edges F4D-PRE measured and the one configuration (2) needs. So `Release\libmh.dll`
reaches into `mh.exe` for every allocation; a standalone host faults on the first one. The plan's
"a real dynamic library, standalone-capable" describes a build that did not exist yet.

**The size of the difference was already committed, in the outward-call census** — this item only had
to read it (`tools/data/libmh_call_census.json`, `summary.config_measures`):

| the roster compiled as | `mh::call::` VA call sites | distinct targets |
| --- | --- | --- |
| **hosted** (`Release\libmh.dll`) | **1563** | 746 |
| **standalone** (`libmh.lib`, `standalone\libmh.dll`) | **3** | 2 |

1440 of those sites are `config_dependent` and 93 are the `vendored_crt` rows — the allocator, the
format family, the string ops, the x87 leaves and the sort. Not a corner case: it is the CRT.

**So configuration (3) gets its own DLL, and it is genuinely a second binary with the same name.**

| | built from | `MH_LIBMH_BUILD` | CRT | deployed to |
| --- | --- | --- | --- | --- |
| `Release\libmh.dll` | the 627 TUs, recompiled | no (HOSTED) | the original's, at fixed VAs | beside `mh.dll` — config (2) |
| `Release\standalone\libmh.dll` | `libmh.lib`, linked whole | yes (STANDALONE) | vendored | beside `libref_host.exe` — config (3) |

`libmh_std.vcxproj` (generated by `gen_libmh_vcxproj.py`, now writing three projects) **compiles no
roster TU**: it links the archive with `/WHOLEARCHIVE`, so the DLL and the archive are the same
object code by construction and the artifact costs one link rather than a third 627-TU compile. The
"every object in the artifact must resolve" property `libref_host` was built around did not weaken —
it MOVED to that link. Two files with one name is a real hazard and it is caught loudly: mh.dll
resolves every contract row before adopting any, so the standalone module planted in `Release\` is
`LOADED BUT REFUSED — 0 of 81`, the `f4d_wrong` shape. The trap: **"libmh.dll" names TWO different binaries**, and the one that ships beside `mh.dll` cannot run standalone.

### The host, and the singleton that proved the boundary is real

`libref_host.vcxproj` now links `standalone\libmh.lib` (the IMPORT library) instead of the archive,
and **drops `MH_SPINE_IN_IMAGE`** — the deliberate inverse of the rule at F4D, for the same reason
`mh_harness.vcxproj` never had it: the host no longer contains the spine, so `mh::state::live()`,
`owner_table()` and `owner_count()` must not be magic statics there (G179). That reversal is not
argued, it is measured: dropping the define turned exactly those three into undefined externals, and
they appear in the derived export list as rows 8–10. The host is 487 KB now, down from 1.26 MB.

**A static import is CORRECT here, and it is the deliberate inverse of `libmh.def`'s rule.** There,
an import library on a link line would put `libmh.dll` in `mh.dll`'s import table and a missing file
would kill `mh.exe` at load — and absence is a shipped configuration. A standalone host has no
configuration to degrade into: libmh IS the program, and failing process load is the loudest correct
answer.

The export list is DERIVED, `gen_libmh_contract`'s shape one deployment over
(`tools/gen_libmh_std_contract.py`): `{UNDEF in libref_host's objects} ∩ {DEFINED in libmh.lib's
objects}` → **41 rows** (26 C++ + 12 `libmh_*` C entries + the three singletons), committed as
`tools/data/libmh_std_contract.json`, rendered into `libmh_std.def`. `--check` is a lint row;
`--rederive --check` runs in `run_gate` beside the other two. No thunks, no slot table and no absent
values, because there is no absence to answer — a missing row here is a LINK ERROR NAMING THE SYMBOL.

```
libref_host.exe   libmh.dll  KERNEL32.dll          (dumpbin /imports, 2026-09-13)
libmh.dll         KERNEL32.dll
```

### The asset question, which is why this did not simply work

All three committed fixtures replay **5000/5000 ALL STEPS IDENTICAL** and then exit 1:

```
  [asset] asset_read("init\H_3100.DMP", cap=0) -- REFUSED (no --assets dir)
  sim traps: 8 fire(s) across 1 entr(ies) -- THIS IS A RED
```

The obvious reading — the one the F4D hand-off recorded — is that the replay needs an extracted
`--assets` tree this box lacks. **Measured, it needs no assets at all.** The requests are the eight
AI players' scripted base layouts (`sim/sim_load_base_layout_dmp.cpp`), and the retail bank does not
contain them: `mh.nam` carries 112 `init\H_*.DMP` entries and the highest is **H_2507**. The hosted
arm misses them too and the loader takes its `len <= 0` early return on both sides, which is exactly
why the hashes were identical. The trap was firing on a question the host cannot answer from
nothing — "is this miss mine or the bank's?" — not on a gap.

The answer is DECLARED and FALSIFIABLE rather than trapped or invented:

* `tools/data/libref_asset_dispositions.json` names the eight paths, with the measurement behind
  them. **Names only; no game data is committed** (the `.nam` is an index of 64-byte records, and
  reading it extracts nothing).
* `libref_host --assets-absent <file>` answers a MISS for exactly those and still TRAPS for anything
  else, so the fail-closed property is untouched and a NEW asset request is still a finding. With
  `--assets` also given, a declared-absent name that turns out to EXIST refuses the run.
* the gate runner re-verifies every row against a real `mh.nam` on any box that has the game, and
  REFUSES if one is present. Note which direction is checkable: nobody can prove a file is absent
  from a bank they do not have, but anyone with the bank can prove the declaration lied. On a box
  with no game copy the declaration is CARRIED, and the run says so rather than passing silently.

**No `bootstrap.py` row was needed** — that was the contingency if the fixtures had turned out to
need a machine-local asset tree, and they do not.

### The gate unit

`tools/replay_libref.py`, `run_gate`'s **`libref`** unit (the roster is 9 units now, up from 8).
Purely local: three console processes, no lane, no VM, no rig lease, no game copy. Measured 70 s and
62 s on two consecutive end-to-end runs (per fixture 19.5–23.8 s).

It asserts its own SUBJECT, which is the whole point of the unit: a build that quietly went back to
linking the archive would replay exactly as green, so it reads `libref_host.exe`'s import table and
fails if `libmh.dll` is not in it. It also refuses a short run — the fixture declares its step count,
and a replay that stopped early would report ALL STEPS IDENTICAL about however few steps it managed.
Fixtures are DISCOVERED from `tools/data/fixtures/`, never listed, so a new one cannot be added and
silently not gated.

**It reads the committed fixtures through `fixture_replay.py`'s own readers** (`load_manifest`,
`_read_z`, `sha256`) rather than re-implementing them. That is the other half of R4: the complaint
was not only that the fixtures were never replayed, it was that the tool which PACKED them is
reached by nothing — so a change to the committed format would have broken a gate nobody ran. A
second reader of a committed format is the same drift one level down.

### "All three configs have a build in the gate", concretely

One msbuild of `mh.sln` produces every artifact — there is no per-configuration build and there must
not be, because (1) and (2) differ only in which files a deployment has beside `mh.dll` (ruling Q10)
and (3) is its own folder. What a single build line DOES hide is an artifact that stopped being
produced, so `run_gate`'s build step now names the set:

```
[gate]   mh.dll                         471552 B   configs (1),(2): the router and patch host
[gate]   mh_net.dll                     401920 B   configs (1),(2): the transport
[gate]   mh_harness.dll                 260608 B   configs (1),(2): the instrument
[gate]   libmh.dll                     1499136 B   config (2): the HOSTED spine
[gate]   standalone/libmh.dll          1211392 B   config (3): the STANDALONE spine
[gate]   standalone/libref_host.exe     487936 B   config (3): the host
[gate]   net_selftest.exe              5065216 B   the offline suites
```

### The arm-log flush gap (F4F's hand-off)

F4F found that an ARMED run killed at the MENU leaves `mh_harness.log` **EMPTY**: the log writer's
1-second flush timer is consulted only by the NEXT `append_line`, so a run that writes its arm report
and then never steps holds the whole report in a 64 KB buffer until `TerminateProcess` discards it.
`harness.cpp`'s constraint 1 claimed "at most FLUSH_MS of output can be lost, never the run"; for
that shape it was false, and the comment says so now.

Two halves, because they cover different runs: the arm reports flush EXPLICITLY at their end
(`MH_Harness_Init`, `MH_Harness_LateArm` — one `WriteFile` per armed boot), and `log_flush_due()`
runs off the present hook, which makes the timer a real bound for the runs that install one
(`MH_Harness_WantsPresentTick` does not always).

Proven both ways on one lane (`workdir/mh_lanes/f4g_armed`, armed, killed at the menu at t+22 s):
**1340 bytes with the fix, 0 bytes with a control build whose three flush calls are removed.** And
proven neutral on a PLAYING armed lane (`sp_det.txt --harness --steps 300`, synth seed pinned):
the two builds' `mh_harness.log` are **BYTE-IDENTICAL, 922 lines**. The fix changes when bytes reach
disk, not which bytes.

One thing it does NOT make true, measured: `check_arm_order`'s harness channel still cannot be gated
from a menu-armed lane, because that channel's end marker is the inbound-refusal census the REPLAY
writes. A menu run now leaves a real arm report and the gate says `NO END MARKER` instead of
`is EMPTY` — a better refusal of the same shape, not a pass.

---

## F5O — one directory per owning target, and the two places that are deliberately shared

The split above moved OBJECTS between images while leaving every source file where it had always
been: 628 of mh.dll's TUs were compiling into `libmh.dll` out of `src/mh_dll/mh/`. F5O made the tree
say what the build does.

**What moved (1262 tracked files, a pure `git mv`; not one `#include` line changed):**

| from | to | why |
| --- | --- | --- |
| `src/mh_dll/mh/{ai,lockstep,orders,save,sim,state,tact}/` | `src/mh_dll/libmh/<same>/` | the shared roster, into the directory of the library that owns it — domain subdirectories kept |
| `src/mh_dll/mh/seams/harness.cpp` | `src/mh_dll/mh_harness/harness.cpp` | one owning project (see "Where the file lives", above) <!-- CITATION-OK --> |
| `src/mh_dll/mh/seams/module_bind_compiled_in.cpp` | `src/mh_dll/mh_nettest/module_bind_compiled_in.cpp` | compiled by exactly one project, the test binary <!-- CITATION-OK --> |

**EXACTLY TWO LOCATIONS ARE SHARED BETWEEN BUILD TARGETS, and both are stated rather than
incidental** — a reader should never have to infer that a directory is multi-target:

1. **`src/mh_dll/libmh/<domain>/`** — the roster. `libmh` (the archive), `libmh_dll` (the hosted arm),
   `mh_nettest` (the hosted offline oracle) and, since fork F5I, `libmh_test` (the standalone one)
   all compile these same TUs. A library's second CRT arm and its tests compiling the library's own
   sources out of the library's own directory is the natural reading of "one directory per target",
   not an exception to it.
2. **`src/mh_dll/mh/addr/mh_calls.gen.cpp`** — ruling Q5's PER-IMAGE shim. It is not in the archive
   and it is not any one project's file: **seven** projects compile their own copy (`libmh_dll`,
   `libmh_std`, `libmh_test`, `libref_host`, `mh`, `mh_harness`, `mh_nettest`), because whichever
   image compiles the roster must supply the `mh::call::detail::s_*` shapes the roster's bodies
   reference. It stays under `mh/addr/` with the rest of the generated VA layer, and
   `check_libmh_outbound.py` rules the resulting unresolved externals as the `shim` bucket — so the
   arrangement is gated, not asserted.

**The include rule.** Every project that compiles or includes the roster carries **both** `..\libmh`
and `..\mh` on `AdditionalIncludeDirectories` (spelled `$(SolutionDir)…` or `$(ProjectDir)..\…` to
match each project's own style): `libmh`, `libmh_dll`, `libmh_std`, `libmh_test`, `libref_host`,
`mh`, `mh_harness`, `mh_nettest`. That is what let 1260 files move with **zero** edits to their `#include`
lines — a roster TU's `#include "sim/sim_state.h"` resolves under `libmh/`, its
`#include "addr/mh_calls.gen.h"` under `mh/`, and neither spelling had to learn where it now lives.
`mh/` is still on the path because it still holds mh.dll's own code plus the header-only trees
`addr/`, `crt/`, `fp/`, `config/`, `fix/`, `patch/`, `include/` and the `seams/` remainder.

**The tooling followed by KEY SPACE, not by path.** Every scanner that walked `src/mh_dll/mh` now
walks all three source trees and keys against whichever one holds the file
(`tools/_dllsrc.py`), so `libmh/sim/sim_state.cpp` still keys as `sim/sim_state.cpp` and
`mh_harness/harness.cpp` still keys as `seams/harness.cpp`. That is what made the move cheap:
~1500 lines of committed census data (`libmh_rebind.json`, `sim_liveness.json`,
`hostapi_callers.json`, the object-path adjudication) needed no re-keying at all, and the data that
DID move is exactly the data keyed by a repo-relative path. Two gates had to be widened by hand or
they would have emptied silently rather than failed — `lint_fp_flags.py` (its include normaliser
stripped only `../mh/`; it now also carries a non-vacuity floor) and
`check_instrument_wiring.py` (its module scan tested `startswith("mh/")` and read the move as
3 module sinks instead of 7, with zero crossings into `libmh.dll`).

**The Solution Explorer tree says the same thing (F5P).** Visual Studio shows a flat file list unless
a sibling `<project>.vcxproj.filters` groups it, and after the move a flat list is actively
misleading — `sim\sim_state.cpp` under `libmh` and `..\libmh\sim\sim_state.cpp` under `libmh_dll` are
one file. All **12** projects now carry a `.filters` (2396 entries, 71 folders) whose folder tree
**mirrors the on-disk layout** rather than inventing a taxonomy: an item's folder is the directory
part of its `Include`, with leading `..\` folded away, so a project's own files sit unprefixed and its
borrowed ones appear under `libmh\sim`, `mh\addr`, `mh_net_proto\src` — the borrow is visible at a
glance. One rule, one implementation: `tools/gen_vcxproj_filters.py` renders it and owns the nine
hand projects; `tools/gen_libmh_vcxproj.py` imports that same renderer for the generated three, in
the same pass as their `.vcxproj`. Both `--check` arms are `lint_repo` rows, with planted-staleness
`--selftest`s beside them, because a `.filters` is a **second copy of a project's item list** — the
copy VS silently rewrites when someone drags a file in Solution Explorer, and the only copy nothing
else in this repo reads. Folder GUIDs are `uuid5(project, folder)`, not fresh `uuid4`s, or
regeneration would be a diff and the drift gate could not tell stale from re-rolled.

---

## F5I — two offline oracles: the hosted exe and the standalone exe

Before F5I one test binary, `net_selftest.exe`, compiled all 340 selftest TUs and ran all 32 gate
suites in the HOSTED arm (no `MH_LIBMH_BUILD`). That meant the standalone arm — the archive
`libref_host.exe` replays, the configuration-(3) `libmh.dll` — was proven only end to end, never by
the per-function suites. F5I split the binary by ARM, and the split is by SUBJECT, not by file:

| | `net_selftest.exe` (`mh_nettest/`, hand-listed) | `libmh_selftest.exe` (`libmh_test/`, generated) |
| --- | --- | --- |
| arm | hosted: mh.dll's own machinery + the transport | standalone: `MH_LIBMH_BUILD` + `MH_SPINE_IN_IMAGE`, the pair `libmh.vcxproj` uses |
| gate suites | 17 | 15 |
| roster | compiles the 628 roster TUs hosted | compiles the same 628 standalone, INTO the exe (no `libmh.lib`: an archive built without `/fsanitize=address` would hand the ASan pass a green report over uninstrumented spine objects) |
| own tests | 13 TUs | 338 TUs (the three `*_negative.cpp` compile-refusal TUs are driven by `check_const_view.py`, never in a project) |
| flags | per-TU `/arch:IA32 /fp:precise` from `lint_fp_flags.py`'s classification | the archive's blanket `/arch:IA32 /fp:precise`, no `/GL` |

**Which suite runs where is data**: `tools/data/selftest_roster.json` (suite → exe, check count,
the `why` each suite is in the gate), asserted three ways — `check_selftest_roster.py` parses both
`SUITE_TABLE`s out of source (lint, no build), `run_selftests.py` compares each exe's live
`--list-suites` before running anything, and the two gate sets must be disjoint with their union the
roster. A suite that silently stopped running would have read as a pass; now it reads as a roster
mismatch.

**The move was proven behaviour-neutral in three isolated steps** (`tools/prove_suite_identity.py`
records every suite's normalised transcript + exit code and diffs a later exe against it): the
dispatch refactor (32/32 identical), the file move alone (32/32), then the second exe running the
moved suites in the standalone arm against the HOSTED baseline. That last comparison is the
oracle-subject risk the tracker named, and it was real — it read 28/32 the first time:

- **Three suites stayed hosted because their precondition is the STOCK bind.** `statetest`'s subject
  is a region moving off its stock VA; `bindtest` asserts the hosted answer to "where does each
  region live"; `hostintest`'s first arm requires the stock bind to answer for every region. Under
  `MH_LIBMH_BUILD` every one of the 845 regions' stock base is literally 0 (`MH_STOCK_BASE`, by
  design: the largest source of original VAs in the archive), so standalone all three would assert
  over a table of zeros and print green. The standalone binding proof is `run_gate`'s libref unit.
- **`simtest`'s registrar oracles needed an arm-specific form.** Hosted, every original has its own
  naked thunk, so body ↔ name is 1:1. Standalone, `ours` is the C++ function itself and MSVC's
  identical-COMDAT folding aliases identical bodies (three no-op state handlers; the empty, the
  `power_consume()`-only and the `add_storage_capacity(); power_consume();` building callbacks). The
  injectivity checks became CONTAINMENT in an enumerated alias-group list — containment, not
  equality, because the plain link folds and the `/fsanitize=address` link does not, so an equality
  would be red in exactly one of the two builds the gate runs. A copy-paste binding a second original
  to an existing wrapper still creates an unlisted alias in every configuration.
- **`fptest`'s negative arm is flag-dependent.** Its four `PC=53-DEPENDENT` rows expect the C++ body
  to diverge from the transcribed x87 at PC=64. Hosted that holds because the test TU compiles at the
  x86 default `/arch:SSE2`; under the archive's blanket `/arch:IA32` the C++ body is x87 too and the
  control word reaches both arms, so the standalone form asserts the two columns AGREE. The `/GL`
  hypothesis was tried first and refuted by experiment. An arm-neutral blindness guard
  (`trunc_scaled_int` must separate further at PC=64 than at PC=53) replaces the per-case zero and
  was proved to red with the control word never set.
- **`savetest` crashed standalone, and the fault was the FIXTURE.** Its region resolver read
  `REGIONS[rid].base + off`, which is 0 standalone, so every region aliased onto one slab and a
  container load scattered state that surfaced three checks later as a null member buffer. It now
  resolves through `BLOCK_SLICES[].stock`, proved arm-neutral over all 56 region runs. The
  `save_driver` itself never describes a region by stock base.
- **One genuine spine defect.** `fill_one_sa` counted `table[id] != nullptr` as "ours" where its
  hosted twin asks `is_our_bldg_type_callback`; that over-reports by id 0 and the four out-of-range
  ids — 100 against the 95 the same cfg gives hosted — and it is the number the registrar LOGS as its
  non-vacuity evidence. It now asks the name-keyed binding set.

After the cut `mh_nettest.vcxproj` is 689 rows. Every consumer that names the exe routes by the
roster's `exe` column (`coverage.py`, `mutate.py`, the migration loop's brief, the loop prompts);
the sweep's non-vacuity floors caught `check_net_lockstep_refs`' OBJ arm dead since F4D (its
lockstep directory had moved with F4D; closure symbols 0 → 263). `build_selftest.bat` builds both
projects and stages both exes; `run_selftests.py` is unchanged for the operator.

## What F4B / F4D / F4E inherit

All three take the mechanism above unchanged: absolute path next to `mh.dll`, `LoadLibrary` +
`GetProcAddress` from `DllMain`, inert satellite `DllMain`, `mh.dll` calling the exported init. What
differs is only **what "absent" means**, and each has to say so in its own refusal line:

| satellite | absent means | the refusal has to |
| --- | --- | --- |
| `mh_net.dll` (F4B) — **DONE**, see the F4B section above | **degrade**: no transport. `transport_present()` answers false and the MP browser carries the standing no-module notice (F3F's `brokered_nomodule` shape, now for real) | LANDED: `transport_present()` is now `MH_NetModule_IsBound()` — the load result — and the `[net] module` read moved into the bind, where `none` means "do not even attempt it" |
| `libmh.dll` (F4D) — **DONE**, see the F4D section above | **config (1)**: the game runs its own bodies. Not an error at all — it is a shipped configuration | LANDED: the line names the configuration (`This run is CONFIGURATION (1): mh.dll does not contain the spine`), and the crossing counters say the same thing in numbers at the end of the arm |
| `mh_harness.dll` (F4E) — **DONE**, see the F4E section above | **uninstrumented**: no determinism harness, and *nothing else* -- the game is bit-for-bit the game it always was. Q4 additionally wants the *harness*'s own late-bind of its `libmh` symbols to be a **hard loud refusal** when `libmh.dll` is absent | LANDED, and the two cases are distinguished by two different files: mh.dll's `harness_bind.cpp` shouts `CONFIGURED BUT ABSENT` when `[harness] enable=1` finds no module, and the module's own init shouts `REFUSES TO ARM … Resolved 0 of 30 libmh.dll spine symbols` when its dependency is gone. R9: **30 spine rows, not 41** -- nine of the predicted ones were `MH_Core_Arm_Early`'s and stayed in mh.dll with it |

Three notes the three items will each want:

* **The 114-symbol contract (F4D) should be BOUND BY A GENERATOR, not by hand.** The repo already
  generates 2625 lines of thunks from a table (`addr/mh_calls.gen.cpp`); 114 `GetProcAddress` lines
  are the same shape, and a hand-maintained binder table is the G106 hand-list failure waiting to
  happen. **Delay-load (`/DELAYLOAD`) was considered and is NOT recommended** — it was not measured,
  and it is listed here so nobody re-derives it as an obvious win: it still calls `LoadLibrary` at
  the first call site (so it renames the loader-lock question rather than answering it), its failure
  path is an SEH exception per call site rather than one named refusal, and it cannot carry data
  exports.
  **LANDED at F4D and the count was wrong**: the generator is `tools/gen_libmh_contract.py` and the
  contract is **101** rows, not 114 (R9, again). Delay-load stayed rejected and gained a third
  reason: the naked-thunk shape needs no signatures either, so the only thing delay-load would have
  bought is the thing it is worst at.
* **`mh_net.dll` needs nothing extra from the subset rule** (`WS2_32` is already one of `mh.dll`'s
  four imports). Check the new satellite's `dumpbin /imports` against `mh.dll`'s before assuming the
  same for `libmh.dll` — that one links a C++ runtime.
  **MEASURED at F4D: KERNEL32 + USER32 and nothing else**, because the Release build pins the static
  CRT. The warning was the right one to carry — a dynamic CRT is exactly what would have broken it.
* **The arm-log position is a choice, and F4F should be told which one each item made** — head of
  the arm window (a ruled baseline edit) for anything that must precede `MH_Core_Arm_Early`,
  post-window (free) for anything that does not.
  **F4D made BOTH**: the bind line at the head (one ruled inserted step, the pre-ruled class) and the
  `[libmh] crossings=` report post-window (free, from `on_present`). The `[hookapi]` line is a third,
  inside the window, and it is the edit F4D-PRE deferred here.
  **F4E made the first only**: one inserted step at the head of the window in all four templates, 0
  text changes, plus an `alt` on that same step for the harness-absent spelling. It adds NO
  post-window line -- the instrument's own row counts are folded into the bind line, so there is
  nothing for F4F to place.

---

## Running it

```
# the gates
python tools/test_ui.py module_absent                  # the live ABSENT arm (a registered scenario,
                                                       #   so it rides run_gate's suite unit)
python tools/check_module_bind.py --selftest           # lint_repo row (the log gate's negatives)
python tools/check_module_bind.py --dllmain-inert      # lint_repo row (the satellite rule)
python tools/check_module_bind.py --dllmain-inert --selftest
python tools/check_module_bind.py --net-surface        # lint_repo row (the bound surface)
python tools/check_module_bind.py --net-surface --selftest
python tools/check_module_bind.py --subset             # run_gate, straight after the Release build
                                                       #   (two pairs since F4D: mh_net AND libmh)

# F4D: the SPINE. The export contract is DERIVED from the two images' objects, so its two gates
# split the same way check_libmh_outbound's do -- one needs no build, one is the load-bearing one.
python tools/gen_libmh_contract.py --check             # lint_repo row (the three emitted files
                                                       #   re-render from the committed list)
python tools/gen_libmh_contract.py --selftest          # lint_repo row (the planted negatives)
python tools/gen_libmh_contract.py --rederive --check  # run_gate, after the build: the committed
                                                       #   list vs what the OBJECTS say
python tools/test_ui.py libmh_absent                   # the live CONFIGURATION (1) arm (registered)
python tools/check_module_bind.py <lane> --libmh --expect bound   # THE STANDING ARM: bound AND
python tools/check_module_bind.py <lane> --libmh --expect absent  #   crossings > 0 (or 0, absent)

# F4D-PRE: libmh's OUTBOUND edge (the reverse direction)
python tools/check_libmh_outbound.py --src-only        # lint_repo row (the include closure)
python tools/check_libmh_outbound.py --selftest        # lint_repo row (the planted negatives)
python tools/check_libmh_outbound.py                   # BOTH arms -- needs a Debug build; prints
                                                       #   the number. This is the load-bearing one.

# THE BOOT SHAPES, BY HAND -- R2 says a green gate is not evidence for loader work, so these
# are MANUAL CONSOLE LAUNCHES with stderr redirected, not scenarios.
#
# F4D's four (the spine):
python tools/make_lane.py --name f4d_bound  --lane 92 --port 6592 --headless --stock-exe
python tools/make_lane.py --name f4d_absent --lane 93 --port 6593 --headless --stock-exe \
    --omit-satellite libmh.dll
python tools/make_lane.py --name f4d_wrong  --lane 96 --port 6596 --headless --stock-exe
#   ... then copy mh_net.dll over f4d_wrong/libmh.dll: a module that LOADS and carries none of the
#   contract, which is the live red proof of the crossing arm.
python tools/check_module_bind.py workdir/mh_lanes/f4d_bound  --libmh --expect bound
python tools/check_module_bind.py workdir/mh_lanes/f4d_absent --libmh --expect absent

# F4B's three (the transport):
python tools/make_lane.py --name f4b_bound  --lane 92 --port 6592 --headless
python tools/make_lane.py --name f4b_absent --lane 93 --port 6593 --headless \
    --omit-satellite mh_net.dll
python tools/make_lane.py --name f4b_none   --lane 94 --port 6594 --headless   # then put
                                                       #   `module=none` under [net] in its ini
# for each: (cd workdir/mh_lanes/<lane> && mh.focus.exe --skip-intro)  with stderr to a file, then
python tools/check_module_bind.py workdir/mh_lanes/f4b_bound  --expect bound
python tools/check_module_bind.py workdir/mh_lanes/f4b_absent --expect absent
python tools/check_module_bind.py workdir/mh_lanes/f4b_none   --expect declined
python tools/check_arm_order.py workdir/mh_lanes/<lane>/logs/<stamp>_solo

# F4E: the INSTRUMENT. Its two import contracts + its export list are DERIVED the same way libmh's
# are, so its two gates split the same way -- one needs no build, one is the load-bearing one.
python tools/gen_harness_contract.py --check            # lint_repo row (the four emitted files
                                                        #   re-render from the committed lists)
python tools/gen_harness_contract.py --selftest         # lint_repo row (the planted negatives)
python tools/gen_harness_contract.py --rederive --check # run_gate, after the build: the committed
                                                        #   lists vs what the OBJECTS say
python tools/test_ui.py harness_absent                  # the live UNINSTRUMENTED arm (registered)
python tools/check_module_bind.py <lane> --module mh_harness --expect bound
python tools/check_module_bind.py <lane> --module mh_harness --expect absent

# F4G: CONFIGURATION (3). Local, rig-free, no game copy needed to RUN (only to re-verify the asset
# declaration, which is skipped with a printed verdict on a box without one).
python tools/replay_libref.py                           # run_gate's `libref` unit
python tools/replay_libref.py --fixture libref-replay-v1
python tools/replay_libref.py --selftest                # lint_repo row
python tools/gen_libmh_std_contract.py --check          # lint_repo row (libmh_std.def re-renders)
python tools/gen_libmh_std_contract.py --selftest       # lint_repo row (the planted negatives)
python tools/gen_libmh_std_contract.py --rederive --check  # run_gate, after the build
#   ... and by hand, which is what the unit runs:
#   src\mh_dll\Release\standalone\libref_host.exe --fixture <unpacked dir> --assets-absent <list>

# F4E's five console shapes (the instrument). The last two are Q4's two halves and are the reason
# this is a console table rather than a scenario: a green scenario proves the process survived and
# says nothing about what it wrote (R2).
python tools/make_lane.py --name f4e_bound  --lane 80 --port 6580 --headless --stock-exe
python tools/make_lane.py --name f4e_absent --lane 81 --port 6581 --headless --stock-exe     --omit-satellite mh_harness.dll
python tools/make_lane.py --name f4e_armed  --lane 82 --port 6582 --headless --stock-exe
#   ... then `[harness] enable=1` in f4e_armed's ini; repeat the two absent arms with it set.
#   REFRESHING A LANE AFTER A REBUILD NOW REFRESHES ITS SATELLITES TOO (`make_lane --refresh-dll`):
#   until F4E it copied mh.dll alone, and a lane left holding yesterday's libmh.dll beside today's
#   mh.dll is exactly the quiet pair a green gate cannot see -- it took a live boot printing
#   `Resolved 10 of 30 libmh.dll spine symbols` to notice one.
```

**A plain lane is the BOUND arm** — `make_lane` deploys `mh_net.dll` into every lane by default, so
absence is what has to be asked for. That default inverted at F4B: while the only satellite was the
knob-gated spike, a lane carried one only on `--satellite`.
