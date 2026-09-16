# Project conventions

The rules the code itself cites: how symbols are named, what the CRT classification means, the two
game concepts that are routinely conflated, and the commit/doc discipline. Source files across
`src/` and `tools/` point here rather than restating any of it, so **this is the public mirror of
the project's working conventions** — the text is carried verbatim where it can be, and the private
research layers it used to cross-reference are described rather than linked.

Sections here are stable anchor targets. If you change a rule, change it here.

---

## Symbol naming

To keep AI-authored names distinguishable from ones a human renamed by hand, **any symbol renamed by
the LLM (Claude/ReVA) carries an `llm` marker prefix:**

- **Functions** → `llm_` prefix, `snake_case`, e.g. `llm_gfx_blit_sprite_rle`,
  `llm_gfx_skip_sprite_row`.
- **Global variables / data labels** → `_G_LLM_` prefix, `UPPER_SNAKE_CASE`, e.g.
  `_G_LLM_FRAMEBUFFER`, `_G_LLM_BLIT_CUR_Y`, `_G_LLM_RGB565_HALVE_MASK`.
- **Data types (structs)** → `llm_` prefix, `snake_case` (e.g. `llm_strat_player_profile`,
  `llm_map_region`); new types go in the type-manager category **`/llm`** (hand-made ones live under
  `/Manual`). When retyping a hand struct in place, keep its correct hand-named fields;
  LLM-added/renamed fields need no extra prefix — the type's `llm_` name is the marker. Field
  comments ARE the documentation (they surface in `docs/structs.md` and in every decompile) — write
  the semantics there, not only in prose docs.

The prefix is a **provenance marker, not part of the symbol's identity**: a symbol *without* one was
named (or verified) by a human, so don't assume it is AI-generated. When renaming, replace any
earlier LLM name in place — don't leave the old label behind as a secondary symbol.

Prefixes are markers, not namespaces — keep the descriptive part meaningful. The domain hints in use
are `map_` (planet-map geometry/regions), `gfx_`, `fx_`, `snd_`, `net_`, `ui_`/`lobby_`,
`unit_`/`bldg_`, `game_`, `rand_`/utility — reuse these rather than inventing near-synonyms.
**Deleting a data type is destructive and needs the user's explicit approval** (present the
usage-report evidence first); renames and retypes do not.

### No `::` in symbols or data types

Hand-named symbols and data types used to carry C++-style namespaces (`cfg::ReadLine`,
`map::object::unit`, `cfg::t::tile_coord`). Since 2026-08-05 they are **flat, with the chain folded
into the name using `_`** — `cfg_ReadLine`, `map_object_unit`, `cfg_t_tile_coord`. 134 types and 141
functions were migrated; do not reintroduce the form.

**Three things the rule does NOT cover, so don't "fix" them:** (a) **the C++ source under
`src/mh_dll`**, where `mh::call`, `mh::sim::state`, `mh::lockstep`, `std::` and friends are ordinary,
correct C++ namespaces — the rule is about the *disassembler's* symbol/type namespace, not about code
we write; (b) the disassembler's **own generated namespaces** — DLL imports (`KERNEL32.DLL`, …),
jump-table case namespaces (`switchD_004…::caseD_…`), and the recovered
`llm_strat_order_queue_dispatch::override::jmp_…` entries; (c) **13 hand-namespaced data labels the
2026-08-05 pass missed** — it migrated types and functions, not labels. Flatten one opportunistically
if a session touches it.

Why the form went: (1) a namespace never survives into anything machine-readable — `getName()`
returns the SHORT name, so every generated index held `ReadLine` while 34 doc files wrote
`cfg_ReadLine`, matching nothing; (2) `::` is not a C identifier, so the struct-header generator was
already flattening it to `_` to keep the emitted header compilable — the convention had broken the
build once. Keep the chain when naming something new in a family (`cfg_final_unit_Construct`, not
`Construct`): `Construct` alone existed in **9** different namespaces, so the prefix is real
disambiguation, not decoration.

### Semantic typedefs and enums

The `cfg_t_*`, `map_t_*`, `cfg_enum_*`/`E_*` families exist to distinguish same-width quantities.
Worth extending, but with realistic expectations proven by the 2026-07-04 pilot: **type propagation
is shallow** — a typedef on a struct field barely surfaces (inline field-folding plus `>>`/`&`/`+`
arithmetic strips it), and it does not cross into untyped callee parameters. So don't run a broad
typedef campaign expecting free highlighting; a good field or parameter *name* conveys as much and
survives arithmetic.

**Two things that DO pay off:** (1) typing **output-pointer helper signatures** —
`f(x, y, dir, tile_coord *out_col, tile_coord *out_row)` stamps the caller's anonymous stack locals
for free and propagates multi-site, and this binary is full of such coordinate helpers; (2) **enums**
for discrete domains (heading/dir8/mode/kind), since member names render through comparisons and
arithmetic.

---

## Coordinates

The scalar coordinate typedefs are **`tile_coord` and `fine_coord`** (category `/map/t`, both
`typedef int`). Tile coordinates are **stored as `byte`** in `map_object_unit` but **computed and
passed as `int`** — a reimplementation that narrows an `int32_t` storage field to a `uint8_t` unit
field is reproducing the original's own intentional narrowing, not introducing a bug.

**Do not write those two as `map_t_tile_coord` / `map_t_fine_coord`.** The flattening convention
above would predict those names, but they are not what the type manager holds: `map_t_fine_coord`
does not exist, and **`map_t_tile_coord` is a DIFFERENT, live type** — a 3-byte struct
`{byte x; byte y; byte len;}` under `/Manual/map`, for a genuine packed triple. Applying it to a
register-width scalar is a real bug, and this trap has bitten twice (parameters 2026-07-18, then
globals 2026-08-06 — the latter typed three 4-byte AI move-order globals as the 3-byte struct until
2026-08-07). For a single scalar coordinate use plain `int`.

---

## Strategic vs tactical

Strategic mode is ~95% of gameplay and the overwhelming majority of named code — **1014
`llm_strat_*` functions / 337 `_G_LLM_STRAT_*` globals against 143 `llm_tact_*`** (re-counted
2026-08-21; treat any quoted figure as a floor and re-count before budgeting). It is the *default*
mode, so **new strategic-mode symbols don't need a `strat_` prefix**: name them by sub-domain
directly (`llm_bldg_construct_finalize`, `llm_unit_bldg_apply_lethal_damage`) unless a bare name
would collide with, or be mistaken for, a tactical-mode counterpart — then add `strat_` for disambiguation only.

**Tactical-mode symbols always carry `tact_`** (`llm_tact_render_view`, `_G_LLM_TACT_UNITS`),
without exception — it is the rarer, one-time-per-session mode and needs the flag to stand out.

This is a *prospective* rule: the existing `strat_`-prefixed symbols are legacy and are **not** being
bulk-renamed; rename an old one opportunistically only if a session touches it anyway.

**Exception — the strategic-AI cluster keeps `strat_`.** The "no `strat_`" default is **overridden
for the AI sub-domain** by the established pattern: **219 `llm_strat_ai_*` functions against 0
`llm_ai_*`** (re-counted 2026-08-21 from `docs/symbols.md`). A new AI function whose siblings and
callers are all `llm_strat_ai_*` **keeps the `strat_` prefix**; do not introduce bare `llm_ai_` names.

---

## CRT and statically linked library functions

This is a **32-bit Watcom build with the C runtime statically linked** — confirmed: no
`MSVCRT`/`CRTDLL`/math DLL in the import table, so the whole CRT *and* any FP math are baked in. The
Watcom graphics library is **not** linked; rendering is GDI32 plus dynamically-loaded DirectDraw plus
the game's own `llm_gfx_*` renderer.

Library functions are **not** `llm_` material — they get real library names, never AI names:

1. **Name CRT functions that are called from game code** — with the real library name if you have it
   on hand (`qsort`, `memcpy`, `sprintf`, …). If you don't have the real name it still gets renamed
   under rule 5 (→ `CRT_<addr>`), not left as `FUN_`.
2. **Propagate the `sub:crt` tag to a CRT function's callees — but only along DIRECT static call
   edges.** Two hard stops: (a) **never follow an indirect/`code*` call as a CRT edge** — `qsort`'s
   real "callee" is the *comparator*, which is game code passed as a pointer, and callback targets are
   never CRT; (b) if a static callee already carries an `llm_`/hand name, or is an import thunk,
   **STOP and surface it** rather than overwriting — either the parent was misclassified or it is a
   genuinely shared helper.
3. **Apply a prototype, or tag `todo:proto`** if the calling convention needs research (a
   register-contract leaf helper that is not a clean `__watcall` signature).
4. **Don't spend effort *naming* CRT callees** — apply a real name only if you already have it; deep
   internal helpers stay `CRT_<addr>` plus `sub:crt`. Rule 1 is the boundary functions; this is the guts.
5. **Rename any unnamed `sub:crt` `FUN_*` → `CRT_<address>`** (e.g. `CRT_004de84e`) — unique, stable,
   greppable, and it signals "classified, real name unknown", distinct from a resolved real name.

Provenance and scope: carry **`fid:watcom106` only when the name actually came from a function-ID
hit**; hand-classified ones get `sub:crt` alone. Keep `sub:crt` meaning *C runtime specifically* — if
a different statically linked Watcom library turns up, use a distinct `sub:` prefix, and only if the
function-ID pass actually proves it linked.

---

## ENERGY is not POWER

**`ENERGY` and `POWER` are two different game concepts — do not conflate them.** Per the user
(2026-07-03): the config/code keyword `ENERGY` is an **HP-like stat** — units' hit points, and
buildings' construction-progress/"charge" value climbing toward a maximum during construction or
repair. **There is no in-game resource called "energy."** `POWER` is the actual generated resource:
power plants produce it, buildings consume it.

Existing hand- and AI-named symbols use `energy` for the HP-like stat, for consistency with the
config keyword (`llm_unit_bldg_energy_refill_full`, the `energy` field on the building record) —
that is expected and correct, not a bug to fix.

The trap is *inferring* meaning: do not assume that a field, order or comment saying "energy" has
anything to do with the power-generation economy, or vice versa. Verify against the actual config
keyword or struct field before writing a comment that uses either word for the other's concept.
This distinction is not derivable from the code alone, so brief it explicitly into any analysis that
touches building power / charge / HP fields.

---

## Subagent model selection

Run **read-only analysis and exploration subagents on the smaller model** by default —
decompile-reading, xref-tracing, and structured "propose a name plus evidence" fan-outs do not need a
heavier one. If a subagent's task genuinely needs a stronger model (subtle multi-function dataflow
reasoning, ambiguous calling-convention recovery, anything where a wrong confident answer would be
applied as a rename), **tell the user before spawning it on a larger model** rather than silently
upgrading.

The tier buys the DECISIONS, not the reading.

---

## Script policy

Persistent engine vs inline script vs one-off. Decide by (a) will the *shape* of the operation
recur, and (b) does it mutate a database:

- **Extend or use a persistent engine** (a `tools/` script plus a request JSON) for any recurring
  shape, even if this particular instance feels unique. If you catch yourself writing a mutation
  script whose skeleton you have written before, stop and extend the engine instead — the varying
  part belongs in a data file, not in code.
- **Inline script code** for read-only exploration: under ~20 lines, output consumed immediately. No
  file, no cleanup, and the code stays visible in the session transcript.
- **One-off script files are a last resort** — a mutation genuinely not expressible in an engine.
  They live in a dated, committed one-off directory and are **never deleted**: a deleted one-timer
  erases the only readable record of what it changed. A second similar one-off is the signal to fold
  the shape into an engine.

---

## Documentation and generated docs

- `docs/symbols.md` and `docs/structs.md` are **fully generated** symbol and struct tables. Never
  hand-edit them; regenerate them in the same session as any rename or retype, or the next reader
  trusts a stale table.
- The per-subsystem `docs/*.md` files carry **resolved findings only** — settled mechanics, structs
  and data maps. They deliberately have no "Open"/"TODO" sections: open items live in the project's
  open-problem ledger, so a reader of `docs/` only ever sees settled ground.
- **Stale structured data reads as authoritative.** A generated index, tracker or manifest that was
  not refreshed in the same session as the work it describes is worse than an honest gap — nobody
  re-measures a number that is already written down.

---

## Commit conventions

- **Commit as you go.** When a mechanism, an item, or a coherent slice of work is done and its gate
  is green, commit it right then. Do not park finished work in the working tree, and do not end a
  session with the tree dirty. This does **not** extend to pushing, force-pushing, opening PRs, or
  any history rewrite. If you inherit uncommitted work, commit it **separately first** so your own
  change is not buried inside it.
- **One commit per resolved mechanism, family or function** — not one per session. Describe what
  changed and why the conclusion holds, since for analysis work the tracked diff shows *what* was
  renamed but not the mechanism you concluded.
