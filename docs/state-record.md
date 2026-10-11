# State recording file format (`mh_match_state.bin[.gz]`, `mh_desync_state.bin`)

Spec for mp:D40 (whole-match recording, `net-debug`), mp:D41 (the ship `net` ring, flushed on a
detected desync), mp:D42 (`tools/state_record.py`) and mp:D43 (launcher report carriage). Written
before any of them, so the writer, the reader and the launcher's tail cut are built against one
definition. **Version 1.** Anything that changes a layout below bumps `version`, and every reader
refuses a version it does not know, by name.

Why it exists and what it costs: measured 2026-09-27 with the desync watch's dirty-block probe
(`[desync] dirty_probe=1`) on a replayed 66,586-step field match. The match changes 432 bytes of the
2.8 MB hash manifest per sim step on average, so recording raw byte runs for every step of a whole
match is about 29 MB before compression.

## What is recorded

- The **raw bytes of every hash-manifest slice** (`mh::state::HASH_REGIONS`, read at
  `hash_base(i)`), in manifest order. Raw means no VERDICT masking: the local-only fields
  (`ctrl_group_id`, the fog byte, rng slot 1, soldier animation) are stored as they are in memory.
  A reader that compares two peers applies the VERDICT masks itself (the mp:D39 mask table).
- One state per **sim step**, taken at the step's PRE-BODY boundary: the same point the desync watch
  and the determinism harness sample at. `step` is the desync watch's step counter (`g_step`, reset
  to 0 at `session_begin_multi`; the first recorded step is 1). The writer logs the offset between
  this axis and the harness's match step axis (`mh_match_harness.log`, `step_base`) once, so the two
  can be joined.

## Layout

All integers little-endian. No padding anywhere; every structure is byte-packed.

### Header (once, at offset 0)

| Field | Type | Meaning |
|---|---|---|
| `magic` | u32 | `0x5253484D` (`"MHSR"` as bytes) |
| `version` | u16 | `1` |
| `flags` | u16 | bit 0: `RING` (a D41 desync ring dump, not a whole match). Others 0. |
| `header_len` | u32 | total header bytes including the region table; chunks start here |
| `manifest_fp` | u64 | the desync watch's manifest fingerprint (names + lens + excluded) |
| `keyframe_every` | u32 | steps between keyframes (the writer's setting; informational) |
| `region_count` | u32 | number of region-table entries |
| region table | | `region_count` entries of `{u32 len; u8 name_len; char name[name_len]}` |

The region table is what makes a file decodable **without a matching build**: a reader takes the
names and lengths from the file, never from its own manifest.

### Chunks (from `header_len` to end of file)

Every chunk is `{u32 tag; u32 payload_len; u32 crc32; u8 payload[payload_len]}`. `crc32` is the
standard zlib CRC-32 (polynomial 0xEDB88320, init/final xor 0xFFFFFFFF) of the payload only.

A reader walks chunks in order and **stops cleanly at the first incomplete chunk** (the file was cut
by a crash, a kill, or a tail cut) or at a CRC mismatch, reporting the last complete step. Neither is
an error unless nothing decodable remains.

| Tag | Value (`"...."` as bytes) | Payload |
|---|---|---|
| `KEYF` | `0x4659454B` | `u32 step`, then every region's raw bytes, concatenated in table order (sum of `len`) |
| `STEP` | `0x50455453` | `u32 step`, `u32 run_count`, then `run_count` runs of `{u16 region; u32 offset; u16 len; u8 bytes[len]}` |
| `END ` | `0x20444E45` | `u32 last_step`, `u32 steps_recorded`, `u64 raw_bytes_written` (the file's size before this chunk) |

Semantics:

- `KEYF` at step *s* = the full state at step *s*. The first chunk after the header is always a
  `KEYF`. Further keyframes come every `keyframe_every` steps (default 3000, one minute at 50 steps/s),
  counted from the first recorded step: steps *f*, *f*+K, *f*+2K, ....
- `STEP` at step *s* = the runs that changed between step *s−1* and step *s*. Applying them to the
  state at *s−1* gives the state at *s*. A step with no change still writes a `STEP` with
  `run_count = 0`, so every step is explicit and a gap means lost data, not "unchanged".
- A keyframe step also gets its `STEP` chunk (written first), so a reader can replay deltas across a
  keyframe without special-casing it; the `KEYF` then re-bases the reader's state (and a reader MAY
  check the rebuilt state equals it).
- Runs never cross a region; `offset + len <= region len`. Nearby changed bytes MAY be coalesced into
  one run (the writer merges gaps of up to 8 unchanged bytes); a reader must not assume runs are
  minimal.
- `END ` is written when recording stops normally. A file without it was cut short; that is
  reported, not refused. The mh.dll writer also leaves it out when it stops recording early (its
  writer fell behind, a write failed, or an allocation failed). It logs the step, and the file
  stays valid through its last complete chunk.

## The mh.dll writer (mp:D40)

`[desync] state_record=1` in `mh_net.ini` (keyframes every 3000 steps, fixed): the launcher's
**Record game state** setting, default off (mp:Q4; `[log] level=debug` does not turn it on). One file per
match, opened at the match's first step in the match folder: `mh_match_state.bin`, or
`mh_match_state_<n>.bin` when that folder already holds one. It is closed at the session's end,
and steps after that ("Continue game") are not recorded. `mh_net.log` carries:

- `; [desync] STATE RECORD -> <path>: ...` when the file opens. The writer thread logs it: it
  creates the file (mp:Q4), so the line can follow the step-axis line.
- `; [desync] STATE RECORD step axis: ...`. This line gives the offset between `step` and the
  harness's match step axis in `mh_match_harness.log`. The harness's match step 1 is the first sim
  step that sees the session folder. The line counts the sim steps that ran in that folder before
  `session_begin_multi` (0 = the same axis). With no session folder open there is no harness
  segment, and the line says so.
- `; [desync] STATE RECORD match end: ...` gives steps, keyframes, file bytes and the sim-thread
  cost per step. The writer then logs `; [desync] STATE RECORD writer: closed <path> -- ...` from its
  own thread once the file is closed: bytes, writes, max write, the keyframes it wrote from its image,
  the peak queue and how long the create took.

**Keyframes after the first (mp:Q4).** The writer keeps the first keyframe as an image and applies every
STEP chunk it writes to it, so a later keyframe at step K is that image (the state at K) re-stamped and
re-sealed on the writer thread. The bytes on disk are what a copy of the shadow at K would give; the sim
thread no longer copies 2.8 MB per keyframe. `net_selftest.exe staterectest` decodes every step of
recordings with keyframes every 25 steps and a mid-match rebase against the live memory; skipping the
image update fails 5 of its checks.

**Single-player sessions (mp:Q2).** A campaign, skirmish, tutorial or loaded save never passes
`session_begin_multi`, so its file is armed at the solo session folder's open
(`mh::desync::solo_session_begin`) instead, on a step counter of its own, and closed at that
session's close like any other. Same format, same name, same folder; the axis line reads
`; [desync] STATE RECORD step axis (SOLO session, mp:Q2): file step k = the k-th sim step after the
solo session folder opened ...` (0 pre-steps = the harness segment's axis).

### The compressed file (mp:D46)

The v1 stream is not changed by compression: **the file mh.dll leaves after a normal match end is the
same bytes inside a gzip member**, `mh_match_state.bin.gz` (`mh_match_state_<n>.bin.gz`). A whole
36-minute match was 242 MB raw and 38.7 MB at deflate level 6 (measured on field match 638fc214).

- **When.** After the writer thread has closed the raw file (END written), a separate
  below-normal-priority thread streams it through deflate level 6 (RFC 1951 inside an RFC 1952 gzip
  header and trailer; no optional header fields, mtime 0). The sim and render threads never wait for
  it (nor, since mp:Q4, for the raw writer). Library: miniz
  3.0.2 (MIT, `src/mh_dll/include/miniz/`, compiled by `desync/state_miniz.c`; see `THIRD_PARTY.md`).
  It is always on (the `state_compress` key was retired 2026-10-06). A recording that STOPPED early (writer
  fell behind, allocation failed) is a valid prefix and is compressed the same way; one whose write
  failed is left raw.
- **The order of events is the safety rule.** raw -> `<raw>.gz.tmp` -> **verify** (re-inflate the temp
  file; CRC-32 and length must equal what was read from the raw file) -> flush -> rename to
  `<raw>.gz` -> only then delete the raw file. At every instant at least one complete file exists:
  a process that exits during the compress leaves the raw file whole and a `.gz.tmp` that no reader or
  glob looks at; a process that dies between the rename and the delete leaves two complete copies. The
  game's own exit (the WM_DESTROY witness) waits at most 3 s for the raw writers and the compress threads
  (mp:Q4, `desync::drain_writers`, one `; [desync] exit drain:` line), so only a longer compress, a
  crash or a killed process leaves the recording **raw** -- the accepted cost, by user ruling. A failed compress logs why and keeps the
  raw file.
- **Naming.** A recording is `<stem>.bin` or `<stem>.bin.gz`; a name whose `.gz` exists counts as
  taken when mh.dll picks the next `mh_match_state_<n>.bin`.
- **Readers.** `tools/state_record.py` (`load`, so `info`/`at`/`diff`) reads either form, by the gzip
  magic, and naming the raw path reads the `.gz` sibling when only that exists (the raw file wins when
  both do); a gzip stream cut short reads as `truncated` at the cut. `tools/replay_match_segment.py`
  finds either. The launcher report carries the `.gz` whole and stored, or unpacks it to cut at a keyframe
  when over budget (`src/launcher/src/report.rs`, `Kind::State`).
- **Log.** One line in `mh_net.log` at completion: `; [desync] STATE RECORD compressed <path>: <raw>
  raw bytes -> <gzip> gzip bytes (<pct>%), <ms> ms (deflate level 6, verified by re-inflating; the raw
  file is removed)`, or `compress FAILED ... the raw file is kept as it is`.
- **Out of scope.** The D41 ring dump `mh_desync_state.bin` (~4 MB) is not compressed.

### The order recording (mp:Q3)

Not this format, and not a state record: the companion files mh.dll writes beside it at `[desync]
order_record=1` (on at `[log] level=debug`), one pair per session folder, gzipped at the session close
through the same compressor (`mh_rec_orders.bin.gz`, `mh_rec_clock.bin.gz`).

- `mh_rec_orders.bin` -- **MHOR v1**, the determinism harness's order format: `u32 magic 'MHOR'`
  (`0x524F484D`), `u32 version 1`, then flat `{i32 step; u8 order[0x44]}` records (0x48 bytes). For
  every step, the due-now queue (`llm_strat_order[0..count)`) as it stands at
  `llm_strat_order_queue_dispatch`'s entry -- the orders that step runs. A step whose queue is empty
  has no records. An order that waits N steps appears N times: the format is a per-step **snapshot**
  (the replay replaces the queue with each step's records), so it cannot carry a delta.
- `mh_rec_clock.bin` -- raw little-endian doubles, the game clock at each step's pre-body boundary;
  entry 0 = step 1.
- **Axis.** Step 1 = the first sim step with the session folder open -- the harness's per-match
  segment axis, so `mh_rec_orders.bin` compares record for record with `mh_match_orders.bin` and
  `mh_rec_clock.bin` with `mh_match_clock.bin`. Copied next to mh.exe as `mh_orders.bin` /
  `mh_clock.bin` they are the harness's `order_mode=2` replay inputs (a replay also needs the match's
  seed blob, which this recorder does not write).
- **Batching.** Records and clocks are staged on the sim thread and handed to the async log sink every
  50 steps (or 256 KB) as reliable binary records, at the start of a step, so a batch always holds whole
  steps; a process killed between hand-overs leaves two files that end on the same step. A session the
  harness stop closes in place (pre-body of its stop step) ends with that step's clock and none of its
  orders.
- **Readers.** `tools/mp_order_diff.py` (`load`, so the CLI and `check_orders_agree.py`) reads `.bin` and
  `.bin.gz`.
- **Log.** `; [desync] ORDER RECORD -> <path> ...` at open; `; [desync] ORDER RECORD closed (<why>): <path>
  -- N step(s), N dispatch call(s), N order record(s) (P% repeat the previous step's queue ...), bytes,
  sim thread mean us/step`; one `ORDER RECORD compressed` / `ORDER RECORD clock compressed` line per file.

### Cutting a tail (mp:D43)

A file may be shortened for upload by keeping the **header plus every chunk from some `KEYF` chunk
onward**. The result is a valid file: its first chunk is a keyframe. The launcher finds `KEYF`
boundaries by walking the chunk headers (tag + length); it never needs to decode a payload.

### Ring dumps (mp:D41)

Same format, with `flags` bit 0 set and `keyframe_every` 0. The only `KEYF` is the first chunk, at the
ring's oldest step. `STEP` chunks follow, one per step, up to the recording's end: the desync plus
the configured tail after it, or less if the byte cap or the match end came first. `END` closes it,
with `steps_recorded` = last - first + 1. The mh.dll writer is `src/mh_dll/mh/desync/state_ring.cpp`
(`[desync] state_ring`, on in ship `net`), and it writes `mh_desync_state.bin` (`_<n>` if taken)
into the match folder once per match. Its `mh_net.log` lines name the path, the step range, how many
steps precede the mismatching step, and the bytes.
