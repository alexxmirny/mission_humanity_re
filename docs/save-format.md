# The save/load subsystem — closure analysis (SV1 step 1)

The `RI-SAVE` counterpart of the order-container closure's O1 section: the exact seam set, the block
contract, the block table, and the versioning behaviour, established **2026-07-28** as preparation for
reimplementing the serializer.

**Status: SV1 and SV1-DRIVERS are both CLOSED (2026-07-30).** The version gate (batch A), the block
layer + both directions of the LZW codec (batch B), appended capacity at EOF (batch C) and the
**table-driven drivers** are reimplemented, adversarially reviewed and mutation-tested. The block table
is **extracted mechanically** (`tools/data/save_block_table.json`) with both directions pair-checked at
generation time, and five real save files — plus all nine of their embedded planet members — load into
heap state through the reimplemented drivers and re-save **byte-identically**.

**All FOUR roots are now PROMOTED and CLOSED (`SV1-P`, `SV1-P-LOAD`, `SV1-P-CONTAINER`, 2026-07-30):**
`SavePlanetToDisk` and `LoadPlanetFromDisk` behind `[promote] save` / `load`, `game_SaveGame` and
`llm_game_load` behind `[promote] container` / `container_load` — each with its own in-call oracle, its
own control arm and its own mutation arm, because the four have four different determinism properties.
Every `SHIP_PROMOTE_*` for this subsystem is still **0**: the subsystem should ship whole, and that
flip is `SV1-P-CAP`'s decision with its own evidence (`strict_reads` and the write-side cap are the
other two open pieces of it), not a tail on the promotions.

Confidence is marked per claim. Where this document says **verified**, the instructions were read out
of `/eng/mh.exe`'s disassembly directly by the main session, not taken from the decompiler or from a
delegated report.

## The two roots, and the cut

* **save** — `game_SaveGame` `0x0044716e`, one caller (`llm_menu_savegame_confirm`)
* **load** — `llm_game_load` `0x004475de`, one caller (`llm_menu_loadgame_confirm`)

Closure taken as the callee tree of each root, **cut at the CRT boundary** (`open_file`,
`write_to_file`, `sprintf`, …) — the same cut rule O1 used. Per-planet work is a second driver pair,
`SavePlanetToDisk` `0x00447bb3` / `LoadPlanetFromDisk` `0x00448107`, which are **also called directly**
from `llm_strat_try_enter_tactical_mission` and `SwitchToPlanet` (tactical handoff and quicksave).
Same function, same block table, different filename-kind selector — so the table below governs all
three entry paths, which is a scope fact worth knowing before touching any of it.

Helpers in the closure: `llm_map_save_regions` `0x00424773` / `llm_map_load_regions` `0x00424a68`,
`llm_game_save_player_data` `0x004dda66` / `FUN_004ddaa5`, `FUN_0041c1e6` /
`llm_ui_bldg_panel_load_state` `0x0041c245`, `FUN_0041c152` / `llm_game_load_available_projects`,
plus load-side fixups (`llm_strat_planet_map_session_init` `0x004dc65a`, `llm_planet_tlo_load`, and
six small state resets called **after** `close_file`).

Explicitly OUT: `llm_cfg_save_setup_dat` (a different file) and
`llm_cfg_save_final_snapshot`/`_load_final_snapshot` (writes nine baked `cfg::FINAL::struct::*` tables
to `init\<name>`). Those share the two block primitives but have no call edge to either root —
**corroborated once only** (absence of a reference-manager edge), and one corroboration is not enough for a negative claim, so treat
"independent" as MEDIUM until their callers are traced.

## The block contract, and a register mapping worth getting right

| | |
| --- | --- |
| write | `llm_lzw_compress_and_write_block` `0x00448661` |
| read | `LZW_ReadCompressedFromFile` `0x004486e6` |

**VERIFIED storage, because the declaration order misleads.** Ghidra prints the signature as
`int(void *src, undefined4 file_h, uint size)`, which reads as EAX/EDX/EBX in `__watcall` order and is
**not** what the parameters are bound to. The committed storage is:

```
src = EAX:4      file_h = EBX:4      size = EDX:4
```

and the call sites agree — e.g. at `0x00447eef` the three set-up instructions are
`MOV EBX,[EBP-0x18]` (the handle), `MOV EDX,0x2d820` (the size), `MOV EAX,0xdd8c48` (`units`).
The generated `mh_calls` layer keys off committed **storage**, not the declaration order, so it is
correct; a human reading the signature text is the one who gets this backwards. (A delegated analysis
of this subsystem did exactly that, reporting `EDX=file_h, EBX=size`. Caught by spot-checking the
call sites — which is why the closure's headline claims get re-read centrally.)

Both directions go through the real LZW codec (`Compress` `0x4f48e0` / `Decompress` `0x4f4b7e`) — even
a 4-byte scalar block pays an LZW header plus a compress/decompress round trip. Both primitives return
**1 = failure, 0 = success**. The read side carries a genuine per-block self-check: the 8-byte block
header stores the expected *uncompressed* size and the reader compares it against the caller's
`dst_size`, so a size mismatch is caught at the block, not absorbed silently.

**That risk was flagged here as MEDIUM and is now SETTLED — the premise was wrong.** The suspicion was
that `UnpackSaveData` returning `-1` was a decode error the reader swallows by forcing a 0 return. It is
not an error at all: `-1` means the payload lacks the `'LZW '` magic, `FUN_004ddc70` (now
`llm_lzss_decompress_block`) is a **second codec** rather than an error path, and the forced success is
correct. A corrupt block *can* still report as loaded, but for a different reason — the discarded read
count. Open question 1 below carries the full answer.

## The container: the outer save file EMBEDS the per-planet files

Established **2026-07-30** (batch C), instruction by instruction, and it answers what used to be open
question 5. `game_SaveGame` writes:

```
40 bytes            version string, PLAIN write_to_file -- NOT a block
18 blocks           Unit .. ADVISOR_NEXT_TIME (the table below)
1 helper            FUN_0041c152 -> AvailableProjects (1 block)
                    <- the accumulated failure count is checked HERE: nonzero => close_file, return 0
per included planet:
    u32             the planet file's LENGTH, PLAIN write_to_file
    <file verbatim>   streamed in <=0x927c0 chunks through G_LZW_TEMP_DATA
1 block             0x400 bytes of llm_build_media_diag_report's output
```

**Corrected 2026-07-30 (SV1-DRIVERS).** This block previously listed *three* helper pairs here. Only
`FUN_0041c152` is called from `game_SaveGame` (`CALL 0x0041c152` @ `0x004473de`); the other two --
`llm_game_save_player_data` (2 blocks) and `FUN_0041c1e6` (2) -- are called from **SavePlanetToDisk**,
at `0x004480cd` and `0x004480d5`, i.e. they are part of the per-planet file, not the container. The
20-block count a real container walks (18 + AvailableProjects + the media trailer) confirms it against
every one of the 41 saves on disk.

And the per-planet file, for the same reason -- `SavePlanetToDisk` is 38 blocks, then the region graph,
then three more, then the progress loop, then the two helper pairs:

```
38 blocks           order queue .. 0x10000 @ 0xb64bb0
region graph        CALL llm_map_save_regions @ 0x0044804a -- 4 + N + 2 blocks, see below
3 blocks            fog_of_war 0x90000, is_human, SAVE_MISC_DWORD
8 blocks            progress[i], stride 0x384, size 3 * (u16)[0x00e16305]
2 helper pairs      llm_game_save_player_data (2 blocks), FUN_0041c1e6 (2)
```

So the plain `u32` at file offset `0x7378` of `11.sav` (`0x5518d` = 348 557) is **the byte length of an
appended, verbatim-copied `save%02d.dat`**. `game_SaveGame` opens the planet file, `file_seek(END)` +
`file_tell` for its size, writes that size as a bare `u32`, then streams the bytes across;
`llm_game_load` does the exact mirror, reading the `u32` and streaming that many bytes back *out* to
`save%02d.dat` before calling `LoadPlanetFromDisk` on it. The earlier note that `0x5518d` "is not the
number of bytes that follow" was comparing against the wrong end: it is the length of one embedded
member, not of the remainder. It also explains the 102-block walk — 18 game-level, then the embedded
planet file's own chain, then the trailing media block.

**The embedded-member count is IMPLICIT — nothing in the file records it.** Both sides recompute the
same predicate over `i = 1..0x1f`:

```
Planets[i].system_index == CurrentSystem && (G_PLANET_STATUS[i] != 0 || G_PLANET_INDEX == i)
```

byte-for-byte the same test at `0x0044741a` (save) and `0x004479c2` (load). It is reproducible on load
because `Planets`, `CurrentSystem`, `G_PLANET_STATUS` and `G_PLANET_INDEX` all arrive in blocks read
*earlier in the same file*. Worth stating plainly: the member count is a **derived** quantity, so
anything that changes that predicate changes the file layout.

**The trailing media block is WRITE-ONLY.** `llm_build_media_diag_report` `0x004ce728` issues a real
`mciSendCommandA` CD/TOC query and its 0x400 bytes are the last block written — and **no reader ever
reads it**; `llm_game_load` stops after the planet loop. Every save file therefore carries a CD
fingerprint nothing consumes, and EOF is one compressed block past the last thing that matters. Its
call idiom looks like a bug and is not:

```
MOV EBX,[EBP-0x34]                     ; file handle
MOV EDX,0x400                          ; size
CALL llm_build_media_diag_report        ; returns the buffer in EAX, preserves EBX and EDX
CALL llm_lzw_compress_and_write_block   ; src=EAX, size=EDX, file_h=EBX
```

### The three compat gates, exactly

The detected version slot gates three read-side branches, and one of them is not the shape the
versioning section originally implied:

* `ver < 2` — one **additional** `0x78b4` (30 900) read into a stack buffer and discarded. Purely
  additive; every following block is unchanged.
* `ver < 4` — **either/or**, not additive. Read `0xe10` (3 600) into a stack buffer, loop 30 records
  converting `0x78`-byte ANSI to `0xf0`-byte wide through `llm_str_ansi_to_wide` into `MESSAGE_QUEUE`,
  then **jump past** the `0x1c20` (7 200) read the modern path uses.
* `ver < 5` — skip the last two blocks (`INVASION_ALERT_TIME` `0x100`, `ADVISOR_NEXT_TIME` `8`) **and
  call `llm_strat_invasion_alert_reset_all`** `0x0049b447`. A skip *and* a reset, not just a skip.

### The table is EXTRACTED, not transcribed — and the extraction found the gates

The extractor is a Ghidra-side generator: it parses the exported `.asm` listings (the decompile
export run with `"disasm": true`), tracks EAX/EDX/EBX across each driver, and emits one record
per block call into `tools/data/save_block_table.json`, which is the committed answer this
document describes. The format IS an ordered list of (address, size) pairs, so
transcribing it by hand into C++ twice would put the one thing that must not drift — the order, and
the pairing between the two directions — into forty places where a typo is invisible.

It also **checks each writer's sequence against its reader's** rather than assuming they agree:

| pair | verdict |
| --- | --- |
| `SavePlanetToDisk` → `LoadPlanetFromDisk` | **OK** — 42 blocks, identical addresses, sizes and order |
| `llm_map_save_regions` → `llm_map_load_regions` | OK |
| `llm_game_save_player_data` → `FUN_004ddaa5` | OK |
| `FUN_0041c1e6` → `llm_ui_bldg_panel_load_state` | OK |
| `FUN_0041c152` → `llm_game_load_available_projects` | OK |
| `game_SaveGame` → `llm_game_load` | 19 written vs **20** read, misaligned from slot 3 |

The last row is not a defect: it is the three gates above, and the mismatch is how the tool *found*
them without being told they existed. That is the argument for the pairing check being part of the
generator rather than a one-off sanity look.

## The block table — and the answer to the cap-raise question

`SavePlanetToDisk` writes ~40 blocks; `LoadPlanetFromDisk` reads the identical set back in the same
order. The question that matters for M5/Tier-C cap raises is whether each block's SIZE is a hardcoded
immediate or derived from a live count.

**Every cap-relevant array is a HARDCODED IMMEDIATE.** Spot-verified in the disassembly:

| region | source | size | at |
| --- | --- | ---: | --- |
| `units` | `0x00dd8c48` | `0x2d820` (186 400) | `MOV EDX,0x2d820` @ `0x00447ee5` |
| `buildings` | `0x00c3d2a0` | `0x35520` (218 400) | `MOV EDX,0x35520` @ `0x00447efa` |
| projectile pool | | `0x2c4fc` (181 500) | (matches the hardcoded-limits catalog) |
| order queue | `_G_LLM_STRAT_ORDER_QUEUE` | `0x4fb0` (20 400) | `0x00447d36` |
| FX anims | | 210 000 | `0x00447db4` |
| the five sub-pools | productions / mines / unit_storage / turrets / labs | `0x6540` / `0x3800` / `0xbea0` / `0x3700` / `0x640` | `0x00447f43`…`0x00447f97` |
| soldiers | | `0x5aa0` | `0x00447f19` |

So the finding recorded for the projectile pool **generalises to the whole
per-planet table, `units` and `buildings` included**: a cap raise does not change the save format, and
equally does not extend it — the immediate must be bumped by hand, and a roster raised past what the
immediate covers is silently truncated at the file boundary.

**Two exceptions, both interesting:**

* `progress[i]` (8 blocks) takes its size from data: `MOVZX EDX,word ptr [0x00e16305]` then
  `LEA EDX,[EDX+EDX*2]`, i.e. *3 × a word field* (**verified** at `0x004480af`). Whether that word can
  actually vary is open question 2.
* the **region graph** is genuinely self-describing — a fixed header, then N × `0x40c` records where N
  comes from the header and drives the load loop. It is the existence proof that this codebase can do
  variable-length serialization when it chooses to, and the model to copy if the reimplementation wants
  appended capacity at EOF.

**The region graph, exactly** (read out of `llm_map_save_regions` `0x00424773` /
`llm_map_load_regions` `0x00424a68`, 2026-07-30):

```
4 blocks x 4 bytes  counter, MERGE_THRESHOLD, count, G_LAST_MAP_INDEX
N blocks x 0x40c    one per node of the linked list at 0x0051de7c;  N = G_LAST_MAP_INDEX
1 block  0x40000    a malloc'd u32-per-cell grid of region ids
1 block  0x10000    a malloc'd byte-per-cell terrain_flags grid
```

**The last two are UNCONDITIONAL**, not per-node and not conditional — an earlier note in the ledger
called them "two conditional per-node buffers", which is wrong; they are written once, after the node
loop, and read once, after the record loop. Measured N across real saves: **12 to 72**.

## Versioning: a real check, and it REFUSES

The load-bearing clause in SV1/SV1-P's acceptance is that a mismatched save must be refused rather
than misparsed. It is — **verified**.

The file's first block is a 40-byte plain-text version string taken from a six-entry table at
`0x005d08f4`, indexed by `_G_LLM_GAME_MISSION_VERSION_INDEX` `0x005d09e4`, which is a **baked constant
never written anywhere in the image**. Read out of `/eng/mh.exe`, that constant is **5**, and the table
is:

```
0  " Extermination, demo version"      3  " Extermination, ver 1.02"
1  " Extermination, ver 1.0"           4  " Extermination, ver 1.03"
2  " Extermination, ver 1.01"          5  " 'Mission: Humanity' ver 1.00"
```

On load the header is matched against slots 1..5; **no match → `close_file` and return 0, i.e. the
load is refused.** A match records the detected slot, which then gates three backward-compatibility
branches (`< 2` reads an extra legacy `0x78b4` block; `< 4` reads the old narrow-string message queue
and converts it; `< 5` skips the invasion-alert / advisor-next-time blocks). Since this exe stamps 5,
all three gates are false for its own saves — they exist to accept an older *Extermination* save in the
*Mission: Humanity* build.

There is a latent uninitialised-local bug in the `index == 0` branch (the detected-version local is
never assigned there, so the compat gates would read uninitialised stack), but that branch is
**statically dead in this binary** because the index is a baked 5. A reimplementation should not port
it; if it does, treat that path as "latest".

## Shadowability

Poor, and structurally so — which SV1's own `done_when` already anticipates.

* **File-handle touchers — not shadowable.** Both drivers, both per-planet drivers, both block
  primitives, and all four helper pairs. A file write cannot be rolled back between two arms, so a
  shadowed call writes the file twice or desyncs the read cursor. Buffer-level unit tests are the
  evidence, exactly as SV1 states.
* **Hardware toucher — flag it.** `llm_build_media_diag_report` `0x004ce728` runs at the tail of every
  save and issues a real `mciSendCommandA` CD/TOC query. That is a hardware escape, not just file I/O,
  and its result varies with whether a disc is present.
* **Network reach — the non-obvious one.** `llm_game_load` calls `llm_strat_time_resync_and_tick`
  `0x00449e21` *after* `close_file`, and that reaches `llm_strat_time_tick`, whose MP branches can send
  lockstep packets. Presumably inert outside a live session (MEDIUM — the guards were not traced), but
  a load performed during an MP session could transmit as a side effect. Open question 3.
* **Shadowable in principle:** the post-`close_file` state resets, notably
  `llm_strat_planet_map_session_init`.

### Per function: why no shadow site, and what stands in for it

SV1's `done_when` asks that every function whose effect escapes the declared regions be recorded
individually with its compensating buffer test named. For the batch-B closure:

| function | why there is no shadow site | what stands in for it |
| --- | --- | --- |
| `llm_lzw_compress_and_write_block` `0x448661` | Writes to a live file handle. The rollback between two arms cannot un-write a file, so a shadowed call appends the block **twice**. | `savetest`: `write_block` byte-identity against the original's own bytes, 6 fixtures + 501 real blocks, plus the failed-write return and the high-water statistic. |
| `llm_lzw_compress_block_with_header` `0x4cecd6` | Reached only from the above, and fills the shared 600 KB staging buffer. | Same tests — it is inside `write_block`. Its odd storage (EAX = *size*, EBX = *dst*) is asserted by the byte-identity of the header fields. |
| `ReadCompressedFromFile` `0x4486e6` | Reads a live file handle. A second arm would consume the payload again and **desync the read cursor** for every following block. | `savetest`: `read_block` over 6 fixtures + 501 real blocks; both refusals; the inclusive-cap boundary pair; both truncation mechanisms. |
| `UnpackSaveData` `0x4ced2b` | Reached only from the above. | The tri-state classification tests, including the `-1` → LZSS dispatch and the `0` → failure mapping. |
| `Compress` `0x4f48e0` + `llm_lzw_emit_code` / `llm_lzw_find_match` / `llm_lzw_encode_core` | **Shadowable in principle** — pure over buffers. But its state is ~96 KB of scratch that the matrix does not carry, and its only caller is the un-shadowable writer, so a site here would buy nothing the buffer test does not already give. | 501 real blocks **re-encoded byte-identically**, which is a stronger statement than per-call agreement: it pins the exact bit stream, resets and all. |
| `Decompress` `0x4f4b7e` + `decompressImpl` / `handleCode` / `updateDict` | Same. | 501 real blocks decoded and checked against `src/formats/decompress.py`, an independent implementation. |
| `llm_lzss_decompress_block` `0x4ddc70` + `llm_lzss_next_flag_bit` | Reached only when a payload is not `'LZW '`. | **The weakest evidence in the batch**, and tagged `conf:med` for it: a hand-assembled 8-byte stream (four literals plus one back-reference) built from the disassembly. **No block in any save on hand is in this format**, so nothing real exercises it. If a shipped `init\*` file turns out to use it, that is the fixture to add. |
| `game_SaveGame` `0x44716e`, `llm_game_load` `0x4475de`, `SavePlanetToDisk` `0x447bb3`, `LoadPlanetFromDisk` `0x448107` (SV1-DRIVERS) | Each one **opens the file itself** and drives every block through it, so a shadow arm would write a second file or consume the reader's cursor twice — the same reason as the block layer one level down, compounded by `game_SaveGame` also invoking `SavePlanetToDisk` for real. | `savetest`'s `SVD:` checks over an injected `block_io` **and** an injected `state_io`, plus the byte-identity round trip of five real saves and all nine of their embedded planet members. 16/16 mutations caught. |
| `llm_map_save_regions` `0x424773` / `llm_map_load_regions` `0x424a68` | Same file handle, plus a `malloc`/`free` pair per direction and a mutation of the live region grid (the mark pass sets `+0x41c` on every gridded node). | The framing rides the driver round trip above (N from 12 to 72 across real saves); the node↔record marshalling has its own buffer-level tests, including the list REVERSAL and the stale-slot carry-over. |
| the version gate, `0x004476b9`-`0x00447778` (batch A) | **Not a function at all** — an inline code region inside `llm_game_load`, so there is no entry to hook even in principle. Its host touches a live file handle regardless. | `savetest`'s 20 version checks over a 40-byte header buffer and a synthetic six-entry table, plus 8 mutations. Being pure is why it was chosen as batch A: it sidesteps the whole shadowability problem. |
| `mh::save::write_extension` / `read_extension` (batch C) | **There is no original to shadow.** This is new code — an appended, length-prefixed trailer the vanilla format does not have. A shadow site compares our body against the game's; there is no game body here. | `savetest`'s 26 `SVX:` checks, all 11 mutations caught. The claim that *matters* is not equivalence but the pairing rule (`extension_requires_version_refusal`), because appending is only safe if a vanilla build refuses the file rather than ignoring the trailer. |

## RNG: not restored, and actively perturbed

**Verified by positive evidence, not by an empty xref.** `_G_LLM_STRAT_RNG_STATE` `0x00603ecc` is not
one of the ~90 blocks, and every one of its references lives inside the four RNG helpers themselves.
More than that, the load path *changes* it: `llm_strat_planet_map_session_init` unconditionally calls
`llm_strat_rng_seed_channel(2, 0)` — a hardcoded reseed of the AI channel to zero — and
`llm_snd_ambient_reseed_planet_event_times` draws fresh numbers through `llm_rand_below_fx` while
loading.

So a loaded game's RNG stream is deliberately *not* a continuation of the saved one. SV1's note that
"RNG state is NOT restored by vanilla load — that matters only for MP save/load and must be decided
explicitly" now has its evidence: the reimplementation must reproduce the reset-and-redraw, not invent
persistence, unless MP save/load is explicitly given different semantics.

## The codec, both directions (batch B)

Reimplemented 2026-07-30 as `mh::lzw` / `mh::lzss` (`src/mh_dll/mh_common/lzw.cpp`) plus the framing
in `mh::save::write_block` / `read_block` (`src/mh_dll/libmh/save/save_block.{h,cpp}`). The whole closure
is small — ~1.4 KB of code — and is now read instruction by instruction. Ghidra names for the pieces:
`llm_lzw_emit_code` `0x4f49cc`, `llm_lzw_find_match` `0x4f4b31`, `llm_lzw_encode_core` `0x4f49fd`,
`llm_lzss_decompress_block` `0x4ddc70`, `llm_lzss_next_flag_bit` `0x4ddcd7`.

**The file layout is confirmed end to end, not inferred.** A 40-byte version string, then a flat chain
of blocks. `11.sav`'s 378 352 bytes resolve into **exactly 102 blocks landing precisely on EOF**, every
block's inner LZW header agreeing with its outer one, with **one plain `u32`** (`0x5518d`, at file
offset `0x7378`) written between the game-level section and the per-planet section — the only thing in
the file that is not a block. What that scalar is remains a batch-C question.

**LZW specifics that a plausible reimplementation gets wrong.** Codes are 9..12 bits, LSB-first within
each byte; `0x100` resets the dictionary, `0x101` ends the stream, entry *i* is code *i*+`0x102`.

* The **encoder widens one insertion later than the decoder** (`0xff`/`0x2ff`/`0x6ff` vs
  `0xfe`/`0x2fe`/`0x6fe`). That is the standard LZW off-by-one — the decoder learns entry *N* only
  after receiving code *N+1* — and both numbers have to be reproduced or the two desync.
* On a full dictionary (`0xeff` entries) the encoder emits the pending character, then `0x100`, **then
  jumps back into its own preamble and emits `0x100` a second time** at the new width. Both markers are
  on the wire; a reimplementation that emits one diverges on any stream long enough to fill up.
* The dictionary array `G_LZW_DICT` `0x66f6b7` is a **union of two different node layouts** — the
  encoder's `{u16 prefix, u16 ch, u32 next}` and the decoder's `{u32 prefix, u32 ch}`. They are never
  live at once. Unifying them is the obvious refactor and it is wrong.
* The chain tables (`0x6776b7` heads, `_G_LLM_LZW_ENC_CHAIN_TAILS` `0x67b6b7` tails) hold **pointers**
  with 0 meaning empty. Storing bare indices instead makes node 0 — a real, reachable node — read as
  empty; the port stores index+1.
* `0x100` restarts the decoder's **outer** loop, so the code after a reset is a fresh prefix and is
  *not* paired with the pre-reset one in `updateDict`.

**The codec's *state* globals are internal, but the STAGING BUFFER IS NOT — a correction made
2026-07-30.** `G_LZW_DICT`, the two chain tables, `LZW_END_PTR`, `LZW_UNCOMPRESSED_SIZE` and
`G_LZW_MAX_COMPRESSED_SIZE` are each referenced *only* from inside the codec — `LZW_UNCOMPRESSED_SIZE`
from four sites in `Compress`/`Decompress`, `G_LZW_MAX_COMPRESSED_SIZE` from exactly the one read and
one write in the writer. Checked twice, because one source is not enough for a
negative claim: the reference manager *and* a byte-granular whole-image scan for raw pointers,
which found **zero** untracked immediates and **zero** unformatted data pointers to any of them. So the
high-water mark really is a statistic with no consumer, and promotion inherits no obligation to
maintain any of them beyond the codec itself (`write_block` maintains the high-water mark anyway).

**`G_LZW_TEMP_DATA` was in that list and should not have been.** It has **21 references from five
functions**: the two block primitives, plus `game_SaveGame`, `llm_game_load`,
`llm_game_check_setup_dat_exists` and `llm_log_dump_all_files`. The 600 KB buffer is a **shared
scratch pad**, not codec-private — the two drivers use it as the chunk buffer for the verbatim
planet-file copy (see "The container" below) and `llm_game_load` reads the 40-byte version header into
it. Every use fills it before reading, so the sharing is safe and strictly sequenced; the point is
that a reimplementation which gave the block layer a *private* buffer on the strength of the original
claim would have been reasoning from a false premise. Same failure shape as every other one of these: a
negative claim true of the functions we were looking at, written down as true of the image.

**Evidence.** `libmh_selftest.exe savetest` = **101 checks, 0 failures**, over six real blocks chosen to
cover 9/10/11/12-bit codes, a mid-stream dictionary reset and the expansion case; each is decoded
against a plaintext hash produced by `src/formats/decompress.py` (an *independent* decoder, written
2026-07-05 from the resource format) and then **re-encoded byte-identically to the original's own
bytes**. Opt-in, `savetest <path.sav>` runs every block of a real file: **501 blocks across four saves,
all decoded, all re-encoded byte-identically, each file consumed exactly to EOF.**

**Twenty-nine mutations**, and the shape of the run is part of the
evidence. The first pass caught all 26 it tried but **eight of them by HANGING** rather than by an
assertion — a corrupt code stream drives the prefix walk into a cycle, which is non-terminating in the
original too. Hangs are a poor verdict (no name, no bound), so the decoder gained the output bound and
the cycle cap described above; on re-run **all seven of those became named failures**, and the three
mutations that delete the new bounds are caught in turn. The one that still reports a hang is the cycle
cap's own — removing the guard against a hang can only be demonstrated by a hang, and the positive
direction (a planted cyclic dictionary is refused cleanly) is a named check. Fixtures are generated by
`tools/gen_lzw_fixtures.py`.

## Appended capacity at EOF (batch C)

SV1's `scope` asks to "keep vanilla blocks byte-compatible and **append enlarged capacity at EOF**".
Batch C settled how, and the container discovery changed the design: the model to copy is not the
self-describing region graph, it is **the outer format's own idiom** — a plain `u32` length followed by
an opaque member, which is exactly how the per-planet files are already embedded.

`mh::save::write_extension` / `read_extension_header` / `read_extension_payload`
(`src/mh_dll/libmh/save/save_ext.{h,cpp}`) append

```
u32 magic ('MHX1')   u32 tag   u32 length   length bytes
```

after the last vanilla byte. One tag per enlarged array (`'UNIT'`, `'BLDG'`), so a reader takes the
ones it understands and skips the rest — the property the vanilla format lacks, and the reason a cap
raise currently has to bump every consumer at once.

**Appending is safe, and that is a property of the reader rather than a hope.** `llm_game_load` never
consults EOF: it reads the header, a fixed block list, and then loops `i = 1..0x1f` extracting one
embedded member per planet satisfying its predicate, and stops. It terminates on a *derived*
condition, so bytes past the last member it wanted are never read. The vanilla writer's own trailing
0x400 media-diag block is already such an unread trailer — the existence proof.

**Which is exactly why an extension must not be silent.** "A vanilla build ignores it" is the same
sentence as "a vanilla build loads a 500-unit save as if it had 400 units", i.e. the silent misparse
SV1 and SV1-P both call the load-bearing failure. So appending capacity is only half a feature: a file
carrying an extension **must stamp a version string the vanilla table does not contain**, and batch A's
gate then refuses it at the header. `extension_requires_version_refusal()` states that as a predicate a
test drives in both directions rather than a paragraph nobody re-reads. It is also, retrospectively,
the best argument for having done the version gate first.

Two deliberate departures from the vanilla framing, both stated at the call site: the payload is **not
compressed** (routing it back through the 600 000-byte staging buffer would reimpose the very ceiling
an extension exists to escape), and a **short read is refused** (the vanilla block reader cannot detect
truncation because it discards its read count; this framing is ours, so it checks).

Evidence: 26 `SVX:` checks in `libmh_selftest.exe savetest`, and **11/11 mutations caught**
driven from a dated one-off. Three of those eleven only became catchable
after the *harness* was fixed: a mutation the driver cannot OBSERVE is not a mutation the check caught, which is worth remembering before
writing the next mutation driver.

## Blocks do not align to symbols — the shifted-block idiom

Found by the ST1 region registry (RI-STATE), which flagged two save blocks reaching past the array
they name, and settled 2026-07-30 against the disassembly.

**The writer's idiom is `write(&array[0].some_field, sizeof(array))`** — the address of a *field*
with the size of the *whole array*. The transfer is therefore displaced right by the field's offset:
it skips that many bytes at the head and runs the same number past the tail.

| symbol | symbol extent | block | head skipped | tail overrun |
| --- | --- | --- | ---: | ---: |
| `player_data` `game_player_data[8]` | `[0xe6dec0, 0xfb26a0)` | `&player_data[0].ai_established`, `sizeof` = `0x1447e0` | **+16** | **+16** |
| `_G_LLM_PROD_SHUTTLE_SLOTS` `[80]` | `[0xbd215c, 0xbe1a1c)` | `&slots[0].src_building_index`, `sizeof` = 63680 | **+20** | **+20** |
| `Upgrades` `Upgrade[99]` | `[0xbcf8d0, 0xbd2108)` | base, but **100** × 104 = 10400 | 0 | **+104** |

**Both region bases are CORRECT** — confirmed from the indexing disp32s, not inferred:

- `0x004dd95c MOV dword ptr [EDX + EAX + 0xe6ded8],0x0` writes `player_data[i].ai_enabled`, which the
  struct puts at `+0x18` → base `0xe6dec0`.
- `0x0048dfc6 MOV word ptr [EDX + 0xbd2170],AX` writes `slots[i].src_building_index` at `+0x14` →
  base `0xbd215c`.

That matters because both structs open with a run of *undefined* padding exactly as long as their
block's head skip (`reserved_0x0[16]`, `reserved_0x0[20]`), which reads like a symbol anchored too
early. It is not — the two write sites above fix each base independently, so the leading padding is real layout rather than an early anchor.

**Two of the three are harmless, and the third is not.**

- `_G_LLM_PROD_SHUTTLE_SLOTS` loses nothing. The `Upgrades` block's own +104 overrun ends at exactly
  `0xbd2170` — *precisely* the 20 bytes of `slots[0]` that the prod block skips. Between the two
  blocks the array is fully persisted, split at a boundary that belongs to neither symbol.
- `Upgrades`' overrun runs into the 84-byte inter-symbol gap plus those 20 bytes. Nothing is lost.
- **`player_data[0]`'s first 16 bytes are covered by NO block and are never saved.** Nothing else in
  the format touches `[0xe6dec0, 0xe6ded0)`. Those bytes are `reserved_0x0[16]`, still unidentified,
  so the practical cost is unknown rather than zero. The matching 16 bytes past the array end *are*
  saved and restored.

Every case round-trips consistently, because the read side uses the same displaced address and
length as the write side — so this is sloppiness with one real hole, not corruption.

### What a reimplemented serializer must do — and the head/tail asymmetry

A block's window is a property of the *block*, not of the region it names, which is why a registry
entry carries both `size` (what the symbol measures) and `reach` (how far a consumer actually goes).
An earlier version of this section said the shifted window "has to move with" a relocated region.
**That is wrong for the tail, and dangerously so.** The two halves are not alike:

| | on save | on load | after the region moves into DLL-owned memory |
| --- | --- | --- | --- |
| **head skip** (`+16`/`+20`) | reads from `base+N` | writes to `base+N` | **safe** — both inside the region |
| **tail overrun** (`+16`/`+20`/`+104`) | reads `[base+size, base+size+N)` | **writes** there | **out-of-bounds** — in the exe those bytes are adjacent `.bss`; in our allocation they are past its end, and the load direction is an OOB *write* |

So the rule is three parts, not one:

1. **The block's LENGTH is format and must be preserved** — but *not* for the reason a first reading
   suggests, and the real mechanism is worth having exactly. The size is **in the file**: every block
   opens with the 8-byte `{compressed_len, uncompressed_size}` header, so framing is self-describing
   and a correctly-framed shorter block would *not* shift the blocks after it. What the reader does
   with the size is the point — it does not adopt it, it **asserts** it:

   ```
   0x0044872f   if (h.uncompressed_size != dst_size) return 1;   // dst_size is the CALL SITE's constant
   ```

   `dst_size` is the immediate baked into each block call (`MOV EDX, <size>`), i.e. our generated
   table. So the file says how big the block is, the code says how big it must be, and a disagreement
   is **refused**. That refusal happens *before* the payload read at `0x0044875f`, so the cursor is
   left mid-block — and since the original does not branch on a per-block failure (it accumulates and
   keeps going, see "Two narrowings"), every following block then reads a garbage header. Ours stops
   at the first refusal instead.

   So "keep the head skip, drop the tail" does not round-trip, and it fails *loudly and early* rather
   than by silent misalignment. This is also the exact instruction that makes a cap-raised save
   unreadable by a vanilla build and vice versa — a bigger array's block does not fit the other
   build's constant — which is why the version-stamp idea (`SV1-P-CAP`) was redundant and got dropped.
2. **The head skip is faithful and safe** — keep it exactly.
3. **The tail must never be sourced from, or written to, past the region.** Those N bytes become an
   explicitly owned filler: on save, emit filler after `region[N..size)`; on load, apply the first
   `size-N` bytes to `region[N..size)` and route the last N to the filler.

**Zero is the right filler, measured rather than assumed.** In a loaded game (`--load 11`, all three
arrays confirmed non-zero first, so the reading is not vacuous) every span a shifted block reaches
outside its array reads all zero:

| span | bytes | live content |
| --- | ---: | --- |
| `player_data` tail `[0xfb26a0, 0xfb26b0)` | 16 | all zero |
| `_G_LLM_PROD_SHUTTLE_SLOTS` tail `[0xbe1a1c, 0xbe1a30)` | 20 | all zero |
| `Upgrades` overrun gap `[0xbd2108, 0xbd215c)` | 84 | all zero |

with no defined symbol, no measured state region and no xref to any of them. The two *skipped heads*
(`player_data[0][0..16)`, `slots[0][0..20)`) are zero as well — which is why the one real hole, the
16 bytes of player 0 that no block covers, has no observed cost. One session is not "always", so this
is evidence for the design, not an invariant.

**And it is provable before it is needed.** The zero-filler choice can be verified *today*, while the
regions are still at their `.bss` addresses, by running the existing `save_verify` A/B with a
synthesized tail: if the file stays byte-identical to the original's, the substitution is proven
before any region moves.

**One block can span two regions.** `Upgrades`' +104 overrun covers the 84-byte gap *and* the 20-byte
head of `_G_LLM_PROD_SHUTTLE_SLOTS` — so if either region moves independently, that single block has
two live sources. A block therefore decomposes into a list of `(region, offset, length)` slices plus
gap slices; the registry has the extents to compute that, and ST3 is where it becomes a lint.

**DONE, 2026-09-06 (`SB-HOSTFREE`; docs/state-boundary.md D7.8).** `gen_save_table_header.py` walks
every block against the **measured** region sizes -- never `reach`, which is what made the failure
silent -- and emits `BLOCK_SLICES` / `SLICED_BLOCKS` for the four blocks that span more than one
region (`Upgrades`, `_G_LLM_GAME_SESSION_MODE` at **50** region runs, `MESSAGE_TIME`,
`_G_LLM_CLICK_SELECT_TARGET_ID`). `save_driver` gathers and scatters those run by run on the same
tri-state seam the ST4 owned blocks use, resolving each run by **region id** through
`state_io::resolve_region` -- address resolution cannot do it, because `covering()` would match the
overrunning window again. A gap run stays at its stock address; nothing relocates memory no region
claims. `tools/check_save_block_slices.py` is the lint (0 unhandled of 68 blocks, in `lint_repo`).

**The zero-filler is not needed and was not used.** Its premise -- "the overrun reads all-zero" -- is
true of the six gap-only blocks and false of these four, which is what made it the wrong remedy; and
the six need nothing anyway, because their gap lies inside the host's `reach` and the host bind
copies `reach` bytes, so the gap travels with the region.

## Latent defects in the original (found while porting)

The first five are unreachable from a file the game itself wrote and reachable from a corrupt or
crafted one, which makes them SV1-P's business rather than curiosities. **The last two are different:
the stale region-record slots happen on EVERY save**, and the unchecked region count is an invariant
the game maintains rather than verifies.

| where | defect |
| --- | --- |
| `ReadCompressedFromFile` `0x448744` | The cap test is `JBE`, i.e. **inclusive**, so `0x927c0` passes — but the payload is read to `G_LZW_TEMP_DATA+8` and the buffer is *exactly* `0x927c0` bytes including the header (`0xa4f128`…`0xae18e8`, where `_G_LLM_STRAT_GROUP_STEP_HEADING_REMAP` starts). A block at the cap **overruns the staging buffer by 8 bytes** into that table. The reimplementation keeps the same accept/reject boundary and gives its own buffer the slack. |
| `ReadCompressedFromFile` `0x448764` | `read_from_file`'s count is stored to a stack slot nothing reads — a **dead store**, so a short read is undetectable. |
| `llm_lzw_encode_core` `0x4f4ad4` | After a dictionary reset it re-reads an input byte with **no end-of-input test**, and the loop's test is an equality (`CMP ESI,end / JZ`), so if the dictionary fills on the token whose next character is the **last input byte**, the cursor steps over the end and the encoder never terminates. |
| `Compress` `0x4f4940` | The same shape at the other end: the first input byte is read **before** the first end test, so a **zero-length** block runs away. Whether any driver can request one is a batch-C question — every per-planet size is a hardcoded immediate, but `llm_map_save_regions` derives some from counts. |
| `llm_lzss_decompress_block` | Its end test is only between tokens, so a final back-reference can write **up to 8 bytes past** the declared size. |
| `llm_map_save_regions` `0x0042490c` | The 0x40c record is built in **one reused stack buffer** and only `count` neighbour slots are refilled per node, so a node with fewer neighbours than its predecessor writes the **predecessor's values** into the file for the slots it does not cover — and **uninitialised stack** for the first node. Every save therefore carries a few hundred bytes of stale/garbage neighbour data. Harmless (the loader reads only `count` slots) but it means two saves of identical state are not necessarily identical files. Reproduced deliberately in `marshal_region_records`, and asserted, so it cannot be "tidied up" into a divergence. **MEASURED 2026-07-30 (SV1-P), and it is not a "not necessarily" — it happens every time:** the original written twice from the same state disagrees with itself over **180 of 239 blocks**, indices 42..221, zero uncompressed-size mismatches, files ~1 KB apart. See "The promoted save direction" below. |
| `llm_map_save_regions` / `llm_map_load_regions` | The writer emits **one record per linked-list node** while the header block carries **G_LAST_MAP_INDEX**, and nothing checks that the two agree. A list longer than N desynchronises the file from that block onward; shorter, and the loader reads into the next block. Ours refuses. |

A fifth, shared with any LZW implementation: a **desynced or corrupt code stream can drive the prefix
walk into a cycle**, which is a non-terminating loop in the original as much as in the port. It showed
up for real during mutation testing, where a deliberately broken encoder hung `savetest`.

### The cap raise ceiling this implies — the one that matters for M5

**The cap is checked on READ ONLY.** `0x927c0` appears **seven** times in the image, and exactly *one*
of them bounds a block: `0x0044873d` in `ReadCompressedFromFile`. The other six —
`game_SaveGame` ×2, `llm_game_load` ×2, `llm_log_dump_all_files` ×2 — are chunked-copy clamps that
correctly bound their own use of the same staging buffer, and none of them looks at a block.
`llm_lzw_compress_and_write_block` has no bound of any kind. (This paragraph previously said "exactly
once in the whole program", which was false; `find-constant-uses` on 2026-07-30 returned seven. The
conclusion is unchanged, but anyone auditing the cap by grepping for the constant would have been
misled about which sites matter.)
The obvious consequence is that the writer emits a block the reader then **refuses**, so the failure
lands on the *load* of a save that appeared to write fine.

**The non-obvious consequence is worse, and it is the sharpest thing batch B turned up.** `Compress`
writes into `G_LZW_TEMP_DATA+8` with no destination bound at all, and that buffer is only 600 000
bytes — so an array whose *compressed* form approaches the cap does not merely produce an unloadable
block, it **overflows the staging buffer during the save** and corrupts whatever `.bss` follows it.
The reader's cap is the only bound in the binary, and it is on the wrong side of the operation to
prevent this. `mh::save::write_block` passes the encoder an explicit limit and returns failure
instead; that is not faithful, and overflowing is not a behaviour worth reproducing.

That gives a Tier-C cap raise a hard, previously unstated ceiling: **no single array may exceed
600 000 bytes once compressed.** How much raw data that is depends entirely on the array's entropy,
and the real blocks span the whole range —

| block | raw | compressed | ratio |
| --- | ---: | ---: | ---: |
| 98 (per-planet, sparse) | 1 329 120 | 3 726 | 0.003 |
| 1 (buildings) | 211 400 | 13 218 | 0.063 |
| 0 (units) | 57 500 | 6 671 | 0.116 |
| 84 | 262 144 | **263 574** | **1.005** |

— so a mostly-empty roster compresses 300:1 and an incompressible one *expands*. The roster arrays are
the sparse kind, which is why the 500-unit/500-building grand build has never hit this. The number to
watch is not the array size but its **compressed** size, and nothing in the game warns you: the first
symptom is a save that will not load. A cap raise that approaches the ceiling wants the writer taught
to check, which is SV1-P's call (the same place `strict_reads` is decided).

## The drivers, reimplemented (SV1-DRIVERS)

`game_SaveGame`, `llm_game_load`, `SavePlanetToDisk`, `LoadPlanetFromDisk` and the four helper pairs
are `mh::save::{load,save}_{container,planet}` — **walks over the table**, not transcriptions of it.
`tools/gen_save_table_header.py` turns `save_block_table.json` into `save_table.gen.h`, and the
generation IS the pair check: it zips the writer's program against the reader's and fails if any step
disagrees on address or size. Applying the three version gates makes the JSON's own "19 written vs 20
read" diagnostic line up exactly. `lint_repo`'s **save-table-drift** step re-runs that, and it has been
watched to go red (a read-side size bumped by 16 → *"CONTAINER: step 5 size 34016 written vs 34032
read"*, naming both call addresses).

**Nothing is bound to the game.** Alongside the block layer's injected `block_io` the driver takes a
`state_io` — a resolver from a game address to a pointer, which is the identity function in the live
build and a **sparse address space over heap slabs** in `savetest`. That is what lets a real save file
load and re-save with no game running and no file open.

**The oracle is byte identity of a re-save**, which proves the table, the order, the version gates, the
framing and all three variable-length parts at once:

| file | members | container blocks | planet members re-saved |
| --- | ---: | ---: | --- |
| `11.sav` | 1 | 19 | planet 1: 82 blocks, N=23 |
| `2-almost.sav` | 2 | 19 | planets 1, 2: 81 + 71 blocks, N=22, 12 |
| `4-saibel-1.sav` | 3 | 19 | planets 1, 2, 3: 81 + 72 + 111 blocks, N=22, 13, 52 |
| `pre_base.sav` | 2 | 19 | planets 1, 8: 81 + 131 blocks, N=22, 72 |
| `insanity8.sav` | 1 | 19 | planet 1: 83 blocks, N=24 |

Every one identical, consumed to exact EOF, at both levels. `savetest` = **179 checks**, and
**16/16 mutations caught by a named assertion**
driven from a dated one-off.

**Three things the round trip has to handle that a flat table cannot express:**

1. **The region graph** — N from the file's own fourth header block. Its node↔record marshalling is
   reimplemented separately (`marshal_region_records` / `unmarshal_region_records`) because it carries
   a finding: **the original's load REVERSES the list.** `llm_map_load_regions` pushes each node onto
   the head at `next_ptr` (`0x0051de7c`) and `llm_map_save_regions` walks from that head, so a save/load/save cycle
   emits the region records in the opposite order — the original is *not* byte-stable across a cycle
   here. The round-trip oracle therefore keeps the records in FILE order and tests the marshalling on
   its own, asserting the reversal in both directions ("is the exact reverse" and "is NOT the original
   order") so neither can pass vacuously.
2. **The progress slice** — `3 × (u16)[0x00e16305]`, and that word **is in no block**: it comes from
   config. So a save is readable only by a build whose config agrees, and what stands between a config
   change and a corrupted load is the block layer **refusing** a header whose uncompressed size
   disagrees. Asserted in that direction: seeding 70 instead of 69 makes the load fail, not misparse.
3. **The embedded-member count** — implicit, recomputed by the predicate. Note the hole this opens and
   how it is closed: because the media trailer is captured opaquely, a predicate that selected too FEW
   members would leave the surplus bytes in the trailer and a verbatim re-emit would still be
   byte-identical. So the trailer is asserted to be **exactly one well-formed 0x400 block**.

**What the drivers do NOT reproduce**, stated because a reader will look for it: the media trailer's
*contents*. `llm_build_media_diag_report` issues a live `mciSendCommandA` CD/TOC query, so a re-save
carries the trailer across verbatim rather than regenerating it. Nothing reads it, and batch C's
appended extension rides in the same captured tail (tested together: a ~20 KB extension after the media
block survives a load/save cycle byte for byte).

## The promoted save direction (SV1-P)

`SavePlanetToDisk` `0x00447bb3` runs our C++ under `[promote] save`. `libmh/save/save_live.{h,cpp}`
resolves the three injections SV1-DRIVERS left open — `block_io` over `write_to_file`/`close_file`,
`state_io` as the **identity function**, the workspace over the original's own `G_LZW_TEMP_DATA` —
and the body is the preamble plus `mh::save::save_planet` walking `table::PLANET`.

**Two of the three outward calls stay calls, and the reason differs for each.** `llm_map_save_regions`
serialises a linked list of malloc'd nodes, not an array at a fixed address, so a driver walking it
would make the output depend on the heap. `llm_game_save_player_data` was recorded in the ledger as
"prologue + its two block writes + epilogue" and **that is wrong** — `0x004dda97` also calls
`FUN_004ee8df` with `s_Saving_file_00506db0`, a status side effect inlining would have dropped
silently. `FUN_0041c1e6` really is just two block writes and its own accumulator, so its steps are
walked. The distinction is carried by `driver_env::delegates`.

### The oracle: an in-call A/B, because a file write cannot be rolled back

The "Shadowability" section above concluded this subsystem is un-shadowable, and it is right — but a
promotion does not need a rollback, it needs the two writers to see the **same state**, which is
obtained by *redirecting the second write* rather than undoing the first. Under
`[promote] save_verify=1` the promoted body writes our file, renames it aside, calls the original
through a **trampoline** (`install_export` would leave the original unreachable), and compares.

**A verify-mode install must NOT claim its body dead** (fixed 2026-09-10). All four seams install
through `save_live.cpp`'s `install_with_trampoline` in verify mode, and that function used to end with
`mh::hook::note_promoted(target)` on the argument that "the body is dead code now, exactly as after
install_jmp". It is the opposite: the trampoline exists *so that* `call_original_*` can jump back to
`target+8`, which makes `[target+8, end)` the most-executed code in the run. `note_promoted` is the
tree's single way of saying a body is dead, and X-TOMB (2026-09-01) trap-fills the dead remainder of
every noted body with `INT3` — so from that commit the first A/B round of a `save_verify=1` run walked
into its own tombstone and `TerminateProcess`'d:

```
; [save] SavePlanetToDisk replacement served call #1 (planet=3 mode=3, verify=1)
; [tombstone] HIT -- map_SavePlanetToDisk @00447BBB ENTERED (armed as promoted)
```

`00447BBB` is `00447BB3 + 8`, the trampoline's own jump-back address; the load direction hit `0044810F`
the same way. The call was removed rather than exempted — the claim was false, not merely inconvenient
(the C1 interlock would also have refused a registered byte fix inside a body that still executes in
arm B). Nothing replaces it: no other detour targets these four entries, and `install_export`'s
per-function byte guard still refuses a second install on them. The verify install now says so in the
log, once per seam, so a future run cannot be left guessing which instrument is armed over what:

```
; [promote] save: verify mode keeps the ORIGINAL body REACHABLE through the trampoline, so it is
  NOT noted as promoted and NOT tombstoned -- entry 00447BB3
```

**The four seams DO compose.** `promote_container_load_verify.ini` warned that `load` "MUST stay unset"
because a nested oracle would run inside each arm of the container one. Measured 2026-09-10 with
`save`+`container`+`load`+`container_load`+`save_verify=1` all on in a single run: the nesting is
bounded (each arm's inner round completes before the outer one continues) and all four verdicts came
back green together — per-planet save EQUIVALENT/0 REAL, per-planet load IDENTICAL over 47 steps,
container EQUIVALENT over 19/19 blocks, container-load IDENTICAL with 3 of 3 members byte-identical.
The one-variable-per-run rule still applies to *attributing* a red verdict; it is not a hard limit.

### THE ORIGINAL IS NOT BYTE-REPRODUCIBLE — measured, not inferred

`[promote] save_verify=2` runs the original **twice** from the same state:

```
CONTROL orig-vs-orig: 180 of 239 blocks DIFFER (indices 42..221, 0 with a different
                      UNCOMPRESSED size); 291165 vs 290296 bytes          [x3 saves]
```

Indices 42..221 are exactly the 180 `0x40c` region records, and the cause is the reused stack buffer
in the defect table above. So **"byte-identical to what the original writes" is unsatisfiable by any
implementation, the original included** — an acceptance criterion, not just a curiosity, and SV1-P's
`done_when` was re-scoped against this measurement.

The comparator was therefore made **complete rather than lenient**: excusing the span would leave 75%
of the file unverified, so a differing record is *decoded* (through `mh::lzw`) and compared on the
header plus the neighbour slots the node's own count says are **in use**. Everything past the count is
undefined by the original's own construction.

| run | verdict |
| --- | --- |
| CONTROL orig-vs-orig | EQUIVALENT — 180 differ byte-wise, 180 excused, **0 REAL** |
| **A/B ours-vs-orig** | EQUIVALENT — 180 differ byte-wise, 180 excused, **0 REAL** |
| MUTATION (`save_verify_poke=100`) | DIVERGENT — 181 differ, 180 excused, **1 REAL** |

i.e. **every block that is not a region record — all 42 our walk emits included — is byte-identical
to the original's**, and the comparator has been watched refusing.

### Triggering a save, and why not through the UI

There is **no UI route**: every registered harness scenario enters via NETWORK GAME, and the MP
ESC-menu widget arrays (`_MP_LOCKSTEP` `0x65345f`, `_MP_OTHER` `0x65347b`) carry DIPLOMACY in the slot
where the SP array (`0x653493`) carries the save/load widgets `0x6513a3`/`0x6513e7`. `[harness] save_at
/ save_every / save_count` calls the root directly instead — the same call `game_SaveGame` makes,
from real mid-match state. `[harness] load_at` reads it back through the **original**
`LoadPlanetFromDisk` (promoted save → `rc=1`, vanilla save → `rc=1`, no file at all → `rc=0`).

## The promoted load direction (SV1-P-LOAD)

`LoadPlanetFromDisk` `0x00448107` runs our C++ behind its own key `[promote] load`.

**The delegate set is MIRRORED, not shared** — each helper pair splits the *opposite* way from its
save-side twin. `FUN_004ddaa5` is **pure** (prologue + two reads) and is walked; its save-side twin
`llm_game_save_player_data` is the impure one. `llm_ui_bldg_panel_load_state` is **impure** (it calls
`llm_ui_bldg_panel_open` at `0x0041c267` before its two reads) and is delegated; its save-side twin
`FUN_0041c1e6` is the pure one. Two more have no save-side twin: `STEP_REGIONS` is **two** calls here
(`llm_map_region_pool_reset` `0x004234b8` before `llm_map_load_regions` `0x00424a68` — without the
reset the fresh node list prepends to the previous planet's graph), and
`llm_strat_planet_map_session_init` `0x004dc65a` emits **no blocks at all**.

The **post-close apply phase** is what makes this root bigger than its twin: eight outward calls
(`llm_planet_tlo_load` guarded by `MODE != 2` and early-returning 0 on failure, two globals zeroed,
`llm_map_setup_dimensions`, `llm_map_fog_of_war_recompute`, the two camera setters,
`llm_gfx_load_planet_extra_sprite_banks`), none of them file I/O.

### The oracle: load the same file twice

A load emits STATE, so there is nothing to byte-compare — but the same redirect trick works one level
up. Every block reads to a FIXED address, so a second load overwrites the first with identical data:
run ours, hash every region the block table names, run the original on the same file, hash again.
`STEP_REGIONS` is excluded (a malloc'd pointer graph, emitted by the original in both arms).

| run | verdict |
| --- | --- |
| CONTROL orig-vs-orig | IDENTICAL over 47 hashed steps |
| **A/B ours-vs-orig** | IDENTICAL over 47 hashed steps, ×3 vanilla saves |
| MUTATION (`load_verify_poke=6`) | DIVERGENT — 1 of 47, naming `expr@0x00447db4` |

**THE ORIGINAL LOAD IS SELF-CONSISTENT** — 47 of 47 steps agree with themselves — where the original
SAVE disagrees with itself over 180 of 239 blocks. Two roots in one subsystem with two different
determinism properties, which is exactly why the two oracles are shaped differently and why the load
side needs no excusal machinery.

### The period-2 invariant, measured

Every load pushes each region node onto the HEAD of the list while the saver walks from that head, so
a save/load/save cycle reverses the record order. From an interleaved run (`[harness] save_keep`
archives each save, since the per-planet path is fixed):

```
save1 == save3 (same order)  True     save2 == reversed(save1)  True
save2 != save1               True     same multiset             True
```

Asserted on the record ID **sequence**, not on bytes — a byte form is unsatisfiable by any
implementation for the reason above.

## The promoted container writer (SV1-P-CONTAINER)

`game_SaveGame` `0x0044716e` runs `mh::save::save_container` behind `[promote] container`. The `.sav`
path is `G_SAVE_DIR` + `save_name` + `".sav"` — **no `temp\` component**; that belongs to the
per-planet files only.

**The members are STREAMED, not held.** `driver_env::member_io` reproduces the game's model — open
`save%02d.dat`, seek END, tell for the length, seek 0, write the `u32` prefix, then the chunk loop
(`0x004474ed`…`0x004475a6`) — instead of the arena the round-trip driver uses. Injected rather than
branched on, so `savetest`'s 179 checks and 11.sav's round-trip are unchanged, which is what proves
the null default inert. `media_source` came with it: `STEP_MEDIA` is a live CD/TOC query in the game
(`llm_build_media_diag_report` → an ordinary block) and a captured image everywhere else.

### The oracle compares the BLOCK SECTION, and the count is derived

A `.sav` is a 40-byte version string, then blocks, then **length-prefixed members** (raw bytes, not
blocks), then the media block — so the chain walk starts past the header and must stop at the member
boundary. Sniffing that boundary was tried and is wrong: a member's `u32 length` plus its own first
block header reads as a plausible block whose uncompressed size matches in both arms, so the walk went
one step too far and reported it as a divergence. `table::CONTAINER` says exactly how many blocks the
writer emits (every `STEP_BLOCK` that is not `discard`), and `blocks_compared == blocks_expected` is
part of the pass condition so a short walk fails rather than passing vacuously.

| run | verdict |
| --- | --- |
| CONTROL orig-vs-orig | EQUIVALENT — 19 of 19 container blocks, 0 differ |
| **A/B ours-vs-orig** | EQUIVALENT — 19 of 19 container blocks, 0 differ |
| MUTATION (`container_verify_poke=100`) | DIVERGENT — 1 differ, first block 0 |

The members' *contents* are out of scope and stated to be: they are per-planet files carrying the
original's region-record stack noise, and SV1-P settled their equivalence.

## The promoted container reader (SV1-P-CONTAINER, second half)

`llm_game_load` `0x004475de` runs `mh::save::load_container` behind `[promote] container_load` — its
own key, because the two directions of this root carry separate evidence.

**It was recorded as unpromotable and the reason was wrong.** The tail reads as though it passes an
argument **in the flags** (`0x00447b4e` `FLD` / `FCOMP` / `FNSTSW` / `SAHF` / `CALL`), which a
generated marshalling thunk could not carry. It does not: `llm_strat_time_resync_and_tick`
`0x00449e21` is 55 bytes of straight line with no conditional, no `SETcc`, no `CMOVcc` and no
`ADC`/`SBB`, and its own prologue would destroy EFLAGS anyway. The sequence is dead. The blocker had
been recorded from the **call site alone**; reading the callee took one query.

What actually remained was duller, and smaller than the ledger's estimate of ~7 prototypes: **three**
(`llm_strat_register_bldg_type_callbacks`, `llm_map_cam_mark_viewport_dirty`, and `FUN_004cad05` →
`llm_menu_build_placement_pending_clear`). Six of the nine tail callees were already callable —
including `llm_snd_ambient_reseed_planet_event_times`, whose "EAX plus two PUSHed halves of a double"
was recorded as needing a storage decision and already had one committed (`EAX:4` + `Stack[0x4]:8`,
`RET 0x8`). One of the three is worth keeping: Ghidra infers **two** register parameters for
`llm_map_cam_mark_viewport_dirty` that do not exist — the body is `PUSHAD` … `POPAD` / `RET`, and
`PUSHAD`'s own read of EAX and EDX is what got counted as two live-in arguments.

### The two behaviours the block table cannot encode

The table is extracted as an ordered list of block CALLS, so it captures the three version gates
exactly. What it cannot capture is the work `llm_game_load` does *around* them that emits no block —
and both instances are read-side only:

* **`ver < 4`** — the `0xe10` block the table marks `discard` is **not discarded on that path**. It is
  30 ANSI message records, and the original converts each `0x78`-byte record into a `0xf0`-byte wide
  one in `MESSAGE_QUEUE` through `llm_str_ansi_to_wide` (`0x004478b6`…`0x004478f4`). A reader that
  only dropped it would leave the *previous session's* message queue standing.
* **`ver < 5`** — skipping the two `ver >= 5` blocks **also calls**
  `llm_strat_invasion_alert_reset_all` (`0x0044797e`). A skip *and* a reset.

Neither is reachable from a save this build wrote (it stamps version 5); they exist to read an older
*Extermination* file. They are injected hooks on `driver_env` rather than inline code, so the driver
stays bindable to `libmh_selftest.exe savetest`, which has no game to call into — and **which step fires
which hook is found by constexpr predicates over the generated table, with `static_assert`s on the
match count**. A table reshape therefore breaks the build instead of silently disarming a compat path,
which is the failure mode that matters when nothing in the test corpus exercises it.

The live container env also gained an **arena**: `env.mem` was null, so the first version-gated
discard block would have dereferenced it. Its size is derived from the table's largest discard.

### The oracle has TWO halves, because a container load produces two things

| | |
| --- | --- |
| **state** | every block reads to a FIXED address, so loading the same file twice overwrites the first result with the second — SV1-P-LOAD's trick one level up. Ours, hash; the original through a trampoline, hash; compare per step. |
| **members** | the per-planet files the load EXTRACTS, compared **byte for byte**. |

The second is the half the container **writer's** evidence does not cover, and it does not contradict
that side's "members are out of scope": nothing is *marshalled* on this path — it copies bytes already
in the `.sav` — so byte identity is satisfiable here, where region-record stack noise made it
unsatisfiable there. Ours are archived between the arms, since the second arm rewrites the same paths.

**One span is excused, derived from the disassembly rather than from a failing run.** The apply tail
FSTPs a fresh `GetCurrentTime()` into `LAST_GAME_TIME`, and the six contiguous clock doubles at
`0x00e587b1`…`0x00e587e1` lie **inside** the `_G_LLM_GAME_SESSION_MODE` block (`0x00e58344 + 0x657`).
Two arms read the wall clock at two instants, so no arm can agree there — the CONTROL included. The
excusal is a **hole in that one step's hash**, not a step waved through.

| run | verdict |
| --- | --- |
| CONTROL orig-vs-orig | **IDENTICAL** — 0 of 19 hashed steps, all members byte-identical (harness save, and `4-saibel-1`) |
| **A/B ours-vs-orig** | **IDENTICAL** ×4 saves — `uitest` (1 member), `2-almost` (2), `4-saibel-1` (3), `pre_base` (2) |
| MUTATION state (`container_load_verify_poke=5`) | **DIVERGENT** — 1 of 19, naming `'Planets'`; members clean |
| MUTATION member (`container_load_member_poke=101`) | **DIVERGENT** — 0 of 19 steps, 2 of 3 members identical |
| ROLLBACK (`container_load=0`) | no LIVE line, no first-call line, trigger reports `promoted=0` |
| PLAIN-JMP (`save_verify=0`) | ours extracted `save01/02/03.dat` **byte-identical** to the original's, across two runs |

**The CONTROL is what earns the A/B its meaning, and it is why it ran first**: 0 of 19 is the
measurement that the excusal hole is the right *size*. Compare the save direction, where the same
control found the original disagreeing with **itself** over 180 of 239 blocks — three roots in one
subsystem, three different determinism properties.

Two mutation arms rather than one, because the halves fail independently, and each stayed in its own
lane. `[harness] loadgame_name` is what made the multi-member evidence reachable at all: the harness's
own save embeds exactly **one** member, so a run that only loads it never executes the extraction
loop's second iteration — and *"1 of 1 members byte-identical"* reads exactly as green as *"3 of 3"*.
The three multi-member files are RU-polygon saves, which load in the EN build — further confirmation
that the data layout and the version-string table are common.

### Two narrowings, both stated, both in the safe direction

* **The failure accumulator.** The original accumulates block failures and tests the total **once**,
  at `0x0044798e` — so a corrupt block does not stop it, and it keeps reading the remaining blocks
  into **live state** before returning 0. Ours stops at the first refusal. Both end in `return 0`
  having already overwritten some state; ours overwrites strictly less of it.
* **A handle the original leaks.** On a member file that fails to open (`0x00447aa3`) the original
  jumps to the exit **without closing the `.sav`**. Ours closes it.

`SHIP_PROMOTE_SAVE`, `_LOAD`, `_CONTAINER` and `_CONTAINER_LOAD` are all **0**: the subsystem should
ship whole, and the flip is its own decision with its own evidence (`SV1-P-CAP`), not a tail on this.

## Open questions

1. ~~**Does a corrupt block report success?**~~ **ANSWERED 2026-07-30, and the premise was wrong.**
   `UnpackSaveData` returning `-1` is **not** an error report: `Decompress` returns `-1` when the
   payload's first dword is not `'LZW '`, so `-1` means *"this block is in the other format"* and
   `FUN_004ddc70` — now `llm_lzss_decompress_block` — is that format's decoder, not a fallback error
   path. Forcing the result to 0 afterwards is **correct**. The real answer to the question behind it
   is worse than the guess, though: **a truncated or corrupt payload does report success.** The read
   count is discarded (see the defect table), the header check has already passed, and the decoder
   finishes off whatever the previous block left in the staging buffer. So *"a mismatched save is
   refused"* holds at the **header** level — version string, and the per-block uncompressed-size
   self-check — and not at the payload level. `mh::save::block_workspace::strict_reads` is the opt-in
   fix, off by default because changing when a load fails is a behaviour change; **SV1-P has to decide
   it explicitly.**
2. ~~**Is `progress[i]`'s count-derived size actually variable?**~~ **ANSWERED 2026-07-30.** It is a
   live runtime value, not a build-time constant — the word at `0x00e16305` has 35 referrers and is
   written by `Construct` `0x0045f004` — but it is **69 in all 41 real saves on disk**, so the block is
   207 bytes everywhere and no save has ever exercised another value. The sharper finding is what it
   implies: **the word is in no block**, so the size is not recoverable from the file. It comes from
   the loaded config, which makes a save readable only by a build whose config agrees, and the only
   thing between a config change and a corrupted load is the block layer refusing a header whose
   uncompressed size disagrees. Asserted in that direction in `savetest`.
3. **Can a load transmit?** Trace the SESSION_MODE / lockstep-status guards on `llm_strat_time_tick`'s
   send branches against the state `llm_game_load` leaves before calling it.
4. **Is the cfg-snapshot pair really independent?** Currently inferred from an absent reference-manager
   edge — one corroboration, which is not enough for a negative claim.
5. ~~**What is the plain `u32` at file offset `0x7378`?**~~ **ANSWERED 2026-07-30.** It is the **byte
   length of an appended, verbatim-copied `save%02d.dat`** — the outer save file is a container that
   embeds the per-planet files, each length-prefixed. See "The container" above. The reason `0x5518d`
   did not match "the number of bytes that follow" is that what follows is *one embedded member plus a
   trailing media block*, not a single run.
