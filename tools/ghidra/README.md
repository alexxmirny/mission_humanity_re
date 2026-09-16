# Ghidra Watcom compiler-spec fixes

Two independent changes live in `x86watcom.cspec.fixed`, both against the same stock file. Install
and re-apply them together — they are one file.

1. **float/double return in ST0** (2026-07-07) — the original fix, documented below.
2. **`__mh_stkprobe`, the stack-probe register model** (2026-08-03) — §2.
3. **`ECX` moved to `<unaffected>` in `__watcall`, plus `__mh_watcall_ecx_volatile`** (2026-08-03) — §3.

**Validate before installing** with the cspec checker in the Ghidra-side tooling. Ghidra's schema
is stricter than XML's and a bad spec does not fail quietly — it refuses to open the program at
all, which you only discover after a restart. The two mistakes that have actually been made here,
and which that checker encodes, are worth knowing even without it: a `--` inside an XML comment
(illegal, and silently fatal), and a `<pentry>` set that no longer matches the model it belongs to.

---

# 1. Float/double return in ST0

`mh.exe` loads with language **`x86:LE:32:watcom`** / compiler spec **`watcomcpp`**, provided by
the third-party **`ghidrawatcall`** Ghidra extension (2019-era, not part of stock Ghidra). Its
`__watcall` (the default convention for ~all of this binary) had a **broken return model**:

- `<output>` listed **only `EAX` (≤4 bytes)** — no `ST0` float pentry, no `EDX:EAX` 8-byte pentry.
- Consequence: every `double`/`float` return resolved to a bogus **hidden struct-return pointer in
  EAX** (`__return_storage_ptr__`), and 8-byte int returns were unmodeled. Setting a function's
  return type to `double` produced garbage unless you hand-built **custom ST0 storage** per function
  (see the mine-extraction pair `0x49681e`/`0x496932`, and the analyst's `__mh_float` workaround
  convention stored in the program DB).

Validated 2026-07-07: pre-fix `getReturnLocation(double)` under `__watcall` = `EAX:4 (ptr)`;
post-fix = `ST0:10`. Regression scan: lint PROTO artifacts 268 -> 263 (slight improvement, no new
artifacts); int returns unchanged.

## The fix

Add the standard x86 float/8-byte return pentries to the `<output>` of `__watcall`, `__stdcall`,
and `__cdecl` (float returns go to ST0 regardless of how args are passed):

```xml
<pentry minsize="4" maxsize="10" metatype="float" extension="float"><register name="ST0"/></pentry>
<pentry minsize="1" maxsize="4"><register name="EAX"/></pentry>
<pentry minsize="5" maxsize="8"><addr space="join" piece1="EDX" piece2="EAX"/></pentry>
```

Input / `unaffected` / `killedbycall` are **unchanged**, so integer-return and argument handling are
unaffected. `x86watcom.cspec.stock` (md5 `3208001adb99eaab3e42b7fe2c1cf993`) and
`x86watcom.cspec.fixed` here are the exact before/after; `diff` them for the precise change.

## Applying it (must re-apply after any Ghidra reinstall/upgrade of the ghidrawatcall extension)

Target file:
`<GHIDRA>/Ghidra/Processors/ghidrawatcall/data/languages/x86watcom.cspec`

```sh
G=/f/apps/ghidra_12.0_PUBLIC_20251205/Ghidra/Processors/ghidrawatcall/data/languages
cp "$G/x86watcom.cspec" "$G/x86watcom.cspec.prefix-bak"     # backup stock
cp tools/ghidra/x86watcom.cspec.fixed "$G/x86watcom.cspec"  # install fix
```

Then **fully restart Ghidra** (the compiler spec is cached at the runtime level; a program
close/reopen may reuse the stale one) and reopen `mh.exe`.

**Rollback:** `cp "$G/x86watcom.cspec.prefix-bak" "$G/x86watcom.cspec"` and restart Ghidra.

## Notes

- This is a **decompiler-output** change only; it does **not** modify the program database
  (`idata/`), so it is not captured by Ghidra's own version store (`versioned/`) either — hence
  this re-appliable copy. (The DB is not in git at all since 2026-08-03; this file is.)
- For **maximum robustness**, functions with a confirmed float return are kept on **explicit custom
  ST0 storage** in the DB (self-contained, correct even if this cspec fix is ever lost), rather than
  relying on the fixed model. The fix's main value is prospective: new double-returning functions
  now just need their return type set to `double` (no custom-storage surgery), and the per-function
  `__mh_float` workaround is no longer needed going forward.

---

# 2. `__mh_stkprobe` — the stack-probe register model (2026-08-03)

## The defect

Watcom emits a call to its stack probe, `assert_stack_capacity` @`0x004cf46f`, into the prologue of
essentially every non-leaf function in this binary. It was typed `__stdcall`, whose model declares
`ECX` and `EDX` **`killedbycall`**. The probe is called *before* the prologue reads the caller-set
registers, so the enclosing function's incoming `EDX` (2nd `__watcall` param) and `ECX` (4th) were
killed at the top of every body — and every later read of them decompiled as `extraout_EDX` /
`extraout_ECX` instead of the declared parameter name.

Its **stack** contract really is `__stdcall` (size pushed by the caller, `RET 0x4` callee-cleans).
Only its **register** contract was misstated.

## Why the fix is safe

The preservation is proven by exhaustive inspection, not inferred from a pattern. The entire call
chain is 53 bytes and none of it writes `EDX`, `ECX` or `EBX`:

| function | size | what it touches |
| --- | --- | --- |
| `assert_stack_capacity` `0x004cf46f` | 16 B | `XCHG [ESP+4],EAX` / `CALL __STK` / `MOV EAX,[ESP+4]` / `RET 4` — an EAX-preserving trampoline whose only job is to hand the size to `__STK` (which takes it in EAX) and give the caller its EAX back |
| `__STK` `0x004cf47f` | 31 B | EAX and ESI only, both `PUSH`/`POP`'d |
| `llm_crt_get_thread_data` `0x004ded64` | 6 B | `MOV EAX,[0x01003024]` / `RET` |

`__mh_stkprobe` is a clone of `__stdcall` with `ECX`/`EDX` moved into `<unaffected>` and the
contradictory `likelytrash` `ECX` dropped. It is applied to **`assert_stack_capacity` only** —
deliberately not to `__STK`, whose parameter arrives in EAX and which returns with a plain `RET`, a
different input contract this model would misstate. The callers only ever see the trampoline, so
that is the whole of the benefit.

## Measured effect

Controlled A/B over the 218-function AI cluster, **identical DB markup, only the convention
differing** (assign, re-export, revert, re-export — an earlier uncontrolled run against stale
listings conflated this with recent renames and must not be repeated that way):

| token | `__stdcall` files | `__mh_stkprobe` files | occurrences before | after |
| --- | --- | --- | --- | --- |
| `extraout_EDX` | 117 | **27** | 670 | **85** |
| `extraout_ECX` | 80 | **67** | 429 | **362** |
| `extraout_EAX` | 4 | 4 | 16 | 16 |
| `extraout_EBX` | 0 | 0 | 0 | 0 |
| `unaff_` | 9 | 9 | 47 | 47 |

107 files improved, 1 net-improved file gained a single token
(`llm_strat_ai_group_scan_building_targets`, `EDX -8 / ECX +1`). The `ECX` drop is the *same*
mechanism, not a wider change: those functions take their 4th `__watcall` parameter in `ECX`
(verified — `calc_resource_sum_5x5(…, int *out_sums @ECX)`,
`group_compute_centroid(…, int *out_y @ECX)`), which the probe was orphaning exactly as it orphaned
`EDX`.

## Scope

This said nothing about whether `__watcall` itself should preserve `ECX`. **That question was
settled separately the same day — see §3 below**, on program-wide evidence rather than the
AI-cluster sample quoted in the first draft of this section.

## Applying

Same procedure as fix 1 — they are the same file. The convention **assignment** is a database
change (checked in), but the **model** lives only in the Ghidra install: after a reinstall the
program's stored `__mh_stkprobe` reference has no definition to resolve against, so re-apply the
cspec before trusting any decompile.


---

# 3. `ECX` is callee-saved in `__watcall` (2026-08-03)

## The change

`__watcall` moved `ECX` from `<killedbycall>` to `<unaffected>`, and a companion model
`__mh_watcall_ecx_volatile` (exactly the old `__watcall`) is pinned to the measured minority that
really do clobber it. Assignment is generated into `tools/data/ecx_clobberers.json` by
`ghidra_scripts/mh_classify_reg_preservation.py` and applied by `mh_set_calling_convention.py`.

## The evidence, program-wide

Measured over **2815 functions**, not the AI cluster — a cspec model is program-wide, and the
222-function AI sample the first draft of §2 quoted is not something to generalise from (it gave
9%/17% figures that were roughly right by luck, but for a curated high-level subset):

| register | never writes | saves+restores | **preserves** | clobbers | cspec status |
| --- | --- | --- | --- | --- | --- |
| **ECX** | 1655 | 880 | **90.1%** | 265 (9.4%) | was `killedbycall`, now `<unaffected>` |
| EBX | 1313 | 993 | 81.9% | 487 (17.3%) | `<unaffected>` since before this repo |
| EDX | 720 | 895 | 57.4% | 1169 (41.5%) | `killedbycall` — correctly |

The decisive comparison is EBX: it has been trusted as `<unaffected>` all along and is preserved
*less* reliably than ECX by every measure. EDX is the genuine volatile.

## Why the minority is pinned rather than accepted

For a function that really clobbers ECX, declaring it unaffected does **not** produce a visible
artifact — it produces a **silently stale value** that reads as correct code. An honest
`extraout_ECX` is the better failure, so the 272 measured clobberers keep it.

Eight clobberers on other models are excluded from the manifest. **The reason is that their INPUT
contract differs** — `__mhfastocall` passes only `EAX` in a register and the rest on the stack,
`__cdecl`/`__stdcall` pass everything on the stack — so a `__watcall`-derived model would change
their **parameter passing**, which is a corruption rather than an artifact.

> An earlier draft of this section also claimed those models "kill ECX already". **That is false for
> `__mhfastocall`**, which declares ECX (and EDX, and EBX) `<unaffected>` with an *empty*
> `killedbycall`. It is true only of `__stdcall` and `__cdecl`. The three `__mhfastocall` entries are
> therefore live silent-stale-value cases, pre-existing and not introduced here —
> preserve-everything models make false promises. The exclusion is right; the
> stated reason was not, and it was hiding a larger defect.

## Measured effect and verification

Controlled A/B over the 218-function AI cluster, DB markup held constant:

| token | before | after | occurrences |
| --- | --- | --- | --- |
| `extraout_ECX` | 67 files | **20** | 362 → **82** |
| `extraout_EDX` | 27 | 27 | 85 → 85 (untouched, as expected) |
| `extraout_EAX` / `EBX` / `unaff_` | 4 / 0 / 9 | unchanged | unchanged |

The token count is **not** the acceptance test. What matters is that no honest artifact was replaced
by a stale value, checked three ways:

* **Gate green** — `aitest` 712/0, `callstest` 966/0, `exportstest` 613/0, `orderstest` 146/0,
  `interlocktest` 44/0, transport `selftest` PASS. `callstest`/`exportstest` validate every generated
  marshalling and entry thunk against the DB, and they hold because both models have **identical
  `<input>` pentries** — the ABI is untouched, only decompilation changes.
* **`dll_call_protos.json`** — `ok` unchanged at 1415; no function lost a working prototype. 53 moved
  `no_prototype` → `unassigned_return`, being ex-`unknown` functions that now carry an explicit
  convention (which they needed: the new default would otherwise have applied the ECX rule to them).
* **Site-level read** of the largest mover, `llm_strat_ai_active_unit_tick`: 8 fabricated SSA values
  (`extraout_ECX` … `_06`) collapsed to **1**. Before, `ATTACK_CANDIDATES[extraout_ECX_02]` was a
  different variable from the loop's `extraout_ECX` and a fabricated value was passed into
  `llm_strat_unit_issue_default_order`. After, one consistent candidate index with
  `uVar9 = extraout_ECX + 1` — and one honest artifact remains, so ECX is still killed where it should be.

## Re-run the classifier after anything that changes function bodies

A function that becomes a clobberer and is not on `__mh_watcall_ecx_volatile` is exactly the silent
case above. `mh_classify_reg_preservation.py` is cheap (~23 s program-wide); re-run it and re-apply
after disassembly changes, new function boundaries, or a re-analysis.

**Known gap, carried deliberately:** 15 functions land in `unsure` (pushed ECX, no exit path proven
to restore it) and are pinned volatile with the clobberers. That is the conservative direction — the
cost is an honest artifact, not a wrong value.

---

# 4. `EBX` clobberers pinned, and the legacy models retired (2026-08-03)

Two follow-ons to §3, both using the models it introduced — **neither needed a cspec change**, so
neither needed a Ghidra restart.

## The legacy models are gone

`__mhfastocall` (66 functions), `__custumocall` (8) and `__mh_float` (2) were hand-made before the
Watcom cspec was imported and declared essentially every register `<unaffected>` with an empty
`killedbycall`. All 76 moved onto measured models: **68 → `__watcall`** (EDX is not in its
`<unaffected>` list, so an EDX-only clobberer is already correct there), 5 → `__mh_watcall_ebx_volatile`,
3 → `__mh_watcall_ecx_ebx_volatile`. Safe because parameter storage does not move: 47 carry
per-function custom storage which overrides the model, and the other 29 are `__mhfastocall` with ≤1
parameter, whose sole parameter is `EAX` under both. **No surviving convention declares `EAX`
`<unaffected>`**, which retires that question rather than answering it.

## The 493 EBX clobberers

`__watcall` declares EBX `<unaffected>` and 487 functions (17.3%) do not preserve it. Each is now on
a model whose `killedbycall` matches what it actually clobbers
(`tools/data/ebx_clobberers.json`): **241 → `__mh_watcall_ebx_volatile`**, **252 → 
`__mh_watcall_ecx_ebx_volatile`** (already ECX-pinned, and also clobbering EBX).

**~1200 `unknown`/`default` functions were deliberately NOT pinned.** They clobber neither register,
so their effective model is already `__watcall` — it is the `default_proto`. Pinning them would be
churn with no semantic change, and it is worth saying out loud because the raw classifier count
invites exactly that mistake.

## What it changed, and the prediction that was wrong

`extraout_EBX` stayed at **0**, as expected. Fixing a too-PERMISSIVE model can only alter a decompile
where the original code held a live value in a register its callee destroys, and correct compiler
output never does — the compiler knew the real convention. §3's `done_when` predicted
`extraout_EBX` would rise; that prediction was wrong and the legacy migration (zero movement across
137 callers) had already shown why.

The real benefit was elsewhere and unpredicted: **`unaff_EBX` fell 31 → 17**, because a register
declared `<unaffected>` prevented Ghidra binding the function's own **EBX parameter**. Two AI
functions had their third parameter rendered as a phantom inherited value:

```c
- (int param_1, uint param_2, uint unaff_EBX, uint param_4, uint param_5, byte param_6)
-   for (; local_10 = param_2, unaff_EBX != param_5; unaff_EBX = height_m & unaff_EBX + 1)
+ (int param_1, uint param_2, uint param_3,   uint param_4, uint param_5, byte param_6)
+   for (local_14 = param_3; local_10 = param_2, local_14 != param_5; local_14 = height_m & local_14 + 1)
```

Same root cause as §2's `extraout_EDX`: a preservation claim swallowing a parameter binding. So the
lesson to carry is that these models are worth correcting for **parameter recovery**, not for the
artifact counts that motivated §3.

Gate green throughout; `dll_call_protos` `ok` unchanged at 1415 (41 functions moved
`no_prototype` → `unassigned_return`, a sharper diagnostic for ex-`unknown` functions, not a
capability change).

---

# 5. Rebuilding the install after a Ghidra upgrade (2026-08-04, Ghidra 12.0 → 12.1.2)

The full checklist for standing up a new Ghidra install, learned by doing it. §1–4 above are the
`x86watcom.cspec` half; this is everything else.

| item | what to do | verify |
| --- | --- | --- |
| **`ghidrawatcall` module** | Copy `Ghidra/Processors/ghidrawatcall` across from the old install. It is **third-party** (not stock Ghidra) and **pure data** — empty `Module.manifest`, no compiled Java, just `.ldefs` + `.cspec` reusing stock `x86.sla`/`x86.pspec` — so its `version=9.1` never blocks anything | program opens as `x86:LE:32:watcom` / `watcomcpp` |
| **Fixed cspec** (§1–4) | Install `x86watcom.cspec.fixed` over the module's `x86watcom.cspec` | the cspec checker, run against the INSTALLED path |
| **REP MOVS→memcpy SLEIGH patch** | `patch` `ia.sinc`, then recompile **both** `x86.slaspec` and `x86-64.slaspec` (`support/sleigh.bat`). The patch survived 12.0→12.1.2 with only an +8 line offset even though `ia.sinc` grew 47 lines | `memcpy`/`memset` counts unchanged in a decompile export |
| **Watcom FID db** | **Nothing to copy.** The `.fidb` is never installed into the Ghidra tree — it lives in `tools/ghidra/watcom_fidb/` and is *attached by path*. An upgrade wipes the attachment record in user settings, not the file | re-attach via Function ID if you need auto-ID |
| **ReVa** | Build from source (below) | its plugins appear in the **tool plugin list**, not merely in the Extensions dialog |
| **Jython** | 12.1+ no longer bundles it. Install `Extensions/Ghidra/ghidra_<ver>_Jython.zip` if you want `run-script` on the 34 Jython `tools/*.py` to behave as before | a `run-script` call succeeds |

## Building ReVa from source

Preferred over downloading a release: ReVa's CI builds per Ghidra **patch** version and lags (v7.3.0
shipped 12.0–12.0.4 and 12.1, but **not** 12.1.1/12.1.2), so a release download can block a Ghidra
upgrade outright. Building also removes the "compiled against a different patch release" risk.

```sh
git clone --depth 1 --branch <tag> \
    https://github.com/cyberkaida/reverse-engineering-assistant.git \
    reverse-engineering-assistant          # NAME MATTERS -- see trap 2
cd reverse-engineering-assistant
GHIDRA_INSTALL_DIR=<ghidra> gradle --no-daemon \
    -Dorg.gradle.java.home=<jdk21> buildExtension
# -> dist/ghidra_<ver>_PUBLIC_<date>_reverse-engineering-assistant.zip
```

Verify the artifact is real: `extension.properties` must carry the target `version=` **natively**
(not hand-edited), and the jar must be **class-file major 65 = Java 21**.

**Trap 1 — Gradle 8.14 cannot run on JDK 25** (`Unsupported class file major version 69`). Ghidra
12.1.2 *runs* fine on JDK 25 but *building* its extensions needs **JDK 21**. Pin 21 for both in any
container image.

**Trap 2 — the build takes the module name from its containing directory.** A clone in `reva_repo/`
produces `reva_repo/lib/reva_repo.jar`, which then silently never loads (see below).

## Where extensions go — this one costs hours

Install to the **user** extensions dir under the **upstream directory name**:

```
%APPDATA%\ghidra\ghidra_<ver>_PUBLIC\Extensions\reverse-engineering-assistant\
```

**Not** `<install>\Ghidra\Extensions\<any-other-name>\`. An extension in the wrong place still shows
as *installed* in `File → Install Extensions`, logs **nothing**, errors **nothing**, and simply never
appears in the project/tool plugin lists. The Extensions dialog scans a wider set of paths than
plugin discovery uses, so "shows as installed" and "will load" are different questions.

**Do not verify an extension by loading its classes under PyGhidra** — PyGhidra puts every module jar
on the classpath, so a successful `jpype.JClass("reva.plugin.RevaPlugin")` proves only that the
bytecode is loadable, never that Ghidra's `ClassSearcher` will discover it as a `Plugin`. The only
real check is the tool plugin list.

## Also remember

- **Back up the `.rep` first.** A 12.1 upgrade is **one-way**: "programs and data type archives
  created or modified in 12.1 will not be usable by an earlier Ghidra version," and the DB is not in
  git.
- **Opening a project under a newer Ghidra does NOT upgrade it — MODIFYING it does.** Verified
  2026-08-04: a project opened and fully decompiled under 12.1.2 still opens cleanly under 12.0
  (3108 functions), and the live program reported `no unsaved changes, no unversioned changes` after
  the switch. Ghidra's release note is precise about this — programs *"created or modified"* in 12.1
  become unreadable by older versions. So the one-way door is the **first write**, not the first
  open, which leaves a free rollback window: until something is saved, downgrading is just pointing
  the launcher back at the old install.
- A new Ghidra version means a **new user-settings dir**, so the one-time ReVa plugin enablement
  (ReVa Application Plugin in the project window; ReVa Plugin in CodeBrowser → File → Save Tool)
  must be redone.

---

# 6. `ECX` and `EDX` are callee-saved in `__cdecl` (2026-08-06)

## The change

`__cdecl` moved **`ECX` and `EDX`** from `<killedbycall>` to `<unaffected>`, and a companion model
**`__mh_cdecl_volatile`** (the old `__cdecl`, plus `EBX`) is pinned to the one measured clobberer.
The contradictory `<likelytrash>` `ECX` is dropped, same as §2.

## The evidence

Only **18** functions carry `__cdecl` in this binary, so the population is measurable in full rather
than sampled. Measured by the per-function register-preservation classifier, 2026-08-06:

| register | preserved | clobbered |
| --- | --- | --- |
| **ECX** | **17 / 18** | 1 |
| **EDX** | **17 / 18** | 1 |

It is the same function both times — `llm_crt_thread_entry_trampoline` @`0x004f46a7`, which clobbers
ECX, EDX **and** EBX and has **zero in-binary callers** (the OS enters it through a thread-start
function pointer). Every other one either never writes the register or pushes and pops it.

This is textbook-cdecl versus *this* binary: standard x86 `cdecl` treats ECX/EDX as scratch, and
Watcom's codegen here saves them anyway (`push ebx / push ecx / push edx` prologues). The model
described the ABI Watcom *could* have used, not the one it emitted.

## Why it was worth doing

The cost landed in **callers**, not in the 18. `llm_strat_ai_recompute_map_influence` was unreadable
— 12 `extraout_` locals and invented loop bounds like `while (extraout_EDX + 1 < 2)` — while both of
its callees were correctly typed `__cdecl` the whole time. It holds `player` in ECX and its loop
counter in EDX across all six call sites, and the model said both were destroyed.

**That is the lesson to carry: a MODEL defect reads exactly like a per-function prototype defect.**
Everything checkable about that function was green.

## Measured effect

Controlled A/B over the **96 callers** of the 17 preserving functions, DB markup held constant
(baseline captured before the restart, `tmp/cdecl_ab_before.json`):

| token | occurrences before | after | functions before | after |
| --- | --- | --- | --- | --- |
| `extraout_ECX` | 89 | **1** | 24 | **1** |
| `extraout_EDX` | 74 | **6** | 25 | **3** |
| `extraout_EAX` | 10 | 10 | 2 | 2 |
| `extraout_EBX` | 10 | 10 | 1 | 1 |
| `unaff_ECX` | 5 | 6 | 1 | 2 |

**39 → 6** functions carry any token; **33 fully cleaned; 0 got worse.** Two readings that matter
more than the counts:

* The residual `extraout_ECX`/`extraout_EDX` in `llm_strat_ai_recompute_map_influence` is **its own
  plate text** — the ad-hoc A/B script did not strip comments the way `lint_annotations` does. Its
  body is clean. Do not chase it.
* `unaff_ECX` going **up** is the §4 pattern and is an improvement: in
  `llm_strat_bldg_completion_dispatch` and `llm_strat_ai_random_point_near` it is now a **recovered
  4th `__watcall` parameter** (`uint unaff_ECX`, `uint *unaff_ECX`) rather than an invisible one —
  the arg is bound, only the auto-name is ugly, which is `lint_annotations`' cosmetic bucket. A
  preservation claim was swallowing a parameter binding.

## Verification

* `dll_call_protos.json` — `ok` **unchanged at 1443**, every status count identical, and the 93
  marshalling shapes identical in distribution. Both models have the **same `<input>`/`<output>`
  pentries**, so this is a decompilation-only change and the ABI is untouched by construction.
* Run the cspec checker before installing. It caught a real error on the first attempt:
  `--` inside an XML comment is illegal, and a malformed spec does not fail loudly — Ghidra refuses
  to open the program.

## Applying

Same file and procedure as §1. Rollback point for *this* change specifically is
`x86watcom.cspec.pre-cdecl-bak` beside the installed spec (the older `.prefix-bak` is stock).
Re-run the register-preservation classifier after anything that changes function bodies: a `__cdecl`
function that becomes a clobberer and is not moved to `__mh_cdecl_volatile` is the silent-stale-value
case §3 describes.
